// test_ts_write_chunking.cpp — a write bigger than one block must still reach the ECU.
//
// TunerStudio's blockingFactor is the most payload an ECU will take in a single command. Writes used to
// go out whole: fine for years, because the only writes were single edited fields of a few bytes. The
// first time a WHOLE image went out — pushing a local tune to an out-of-sync ECU — the ECU rejected the
// 64 KB packet and the push silently did nothing.
//
// So: writeFlat must split by BOTH the page geometry and the block size, and every byte must arrive
// exactly once, in order, at the right page and offset. A fake ECU on a pty answers the commands.
//
//   cmake --build build --target ts_write_chunk_test && ./build/ts_write_chunk_test

#include "comms/TsLink.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

// What the fake ECU saw: one record per 'C' command it accepted.
struct Write { int page, offset, len; };

// A minimal TS slave. Reads 'C' commands, refuses any payload over `blockLimit` exactly as a real ECU
// does, and reconstructs the image so we can prove the bytes landed where they were meant to.
struct FakeEcu {
    int fd{-1};
    int blockLimit{256};
    std::vector<Write> writes;
    std::vector<uint8_t> image;          // flat, page 0 based at 0 / page 1 after it
    std::atomic<bool> stop{false};
    std::atomic<bool> oversized{false};

    void run() {
        std::vector<uint8_t> buf;
        while (!stop.load()) {
            uint8_t b[4096];
            const ssize_t n = ::read(fd, b, sizeof b);
            if (n > 0) buf.insert(buf.end(), b, b + n);
            else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }

            // The studio frames each command; strip to the payload and act on the opcode.
            while (buf.size() >= 2) {
                const size_t frameLen = (static_cast<size_t>(buf[0]) << 8) | buf[1];   // big-endian length
                if (buf.size() < 2 + frameLen + 4) break;                              // + CRC32
                const uint8_t* p = buf.data() + 2;
                if (*p == 'C' && frameLen >= 7) {
                    const int page   = p[1] | (p[2] << 8);
                    const int offset = p[3] | (p[4] << 8);
                    const int len    = p[5] | (p[6] << 8);
                    writes.push_back({ page, offset, len });
                    if (len > blockLimit) { oversized = true; _reply(0x83, {}); }      // as an ECU refuses it
                    else {
                        const size_t at = (page == 0 ? 0u : 4096u) + static_cast<size_t>(offset);
                        if (image.size() < at + len) image.resize(at + len, 0);
                        std::memcpy(image.data() + at, p + 7, static_cast<size_t>(len));
                        _reply(0x00, {});
                    }
                } else if (*p == 'B') {
                    _reply(0x00, {});
                } else {
                    _reply(0x00, {});
                }
                buf.erase(buf.begin(), buf.begin() + 2 + frameLen + 4);
            }
        }
    }

    void _reply(uint8_t code, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> pkt;
        const size_t len = 1 + body.size();
        pkt.push_back(static_cast<uint8_t>(len >> 8)); pkt.push_back(static_cast<uint8_t>(len & 0xFF));
        pkt.push_back(code);
        pkt.insert(pkt.end(), body.begin(), body.end());
        const uint32_t crc = crc32_ieee::compute(reinterpret_cast<const char*>(pkt.data() + 2), int(len));
        for (int i = 3; i >= 0; --i) pkt.push_back(static_cast<uint8_t>((crc >> (i * 8)) & 0xFF));
        (void)::write(fd, pkt.data(), pkt.size());
    }
};

int main() {
    std::printf("TsLink write chunking tests\n");

    int master = -1, slave = -1;
    char name[128] = {0};
    if (::openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::printf("  SKIP: no pty available\n");
        return 0;
    }
    ::fcntl(master, F_SETFL, O_NONBLOCK);

    FakeEcu ecu;
    ecu.fd = master;
    ecu.blockLimit = 256;
    std::thread th([&] { ecu.run(); });

    TsLink link;
    if (!link.open(name)) {
        std::printf("  SKIP: cannot open %s\n", name);
        ecu.stop = true; th.join(); return 0;
    }
    link.setPages({ { 0x0000, 0, 4096, true }, { 0x0001, 4096, 1024, true } });
    link.setBlockSize(ecu.blockLimit);

    // A payload that crosses the page boundary and is many blocks long either side of it.
    std::vector<uint8_t> data(5120);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + (i >> 8));

    const bool ok = link.writeFlat(0, data);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("  writeFlat(5120 bytes, 4096+1024 pages, block %d) -> %s in %zu command(s)\n",
                ecu.blockLimit, ok ? "ok" : "FAILED", ecu.writes.size());

    CHECK(ok);
    CHECK(!ecu.oversized.load());                       // the bug: one 4096-byte packet, refused

    // Every command within the block limit, and inside exactly one page.
    bool bounded = true, inPage = true;
    for (const Write& w : ecu.writes) {
        if (w.len > ecu.blockLimit || w.len <= 0) bounded = false;
        const int pageSize = w.page == 0 ? 4096 : 1024;
        if (w.offset < 0 || w.offset + w.len > pageSize) inPage = false;
    }
    CHECK(bounded);
    CHECK(inPage);
    CHECK(ecu.writes.size() == 16 + 4);                 // 4096/256 + 1024/256

    // …and the bytes themselves, reassembled, are what went in — no gap, overlap or reordering.
    bool same = ecu.image.size() >= 4096 + 1024;
    for (size_t i = 0; same && i < 4096; ++i)          same = ecu.image[i] == data[i];
    for (size_t i = 0; same && i < 1024; ++i)          same = ecu.image[4096 + i] == data[4096 + i];
    CHECK(same);

    // A write that would run off the end of every page is refused rather than half-applied.
    ecu.writes.clear();
    const bool past = link.writeFlat(5000, std::vector<uint8_t>(512, 0xAB));
    CHECK(!past);
    std::printf("  a write past the last page is refused (%zu command(s) sent)\n", ecu.writes.size());

    // Teeth: the fake ECU really does refuse an over-limit packet, so the pass above is chunking doing
    // its job and not a slave that accepts anything. This is the old behaviour, reproduced deliberately.
    ecu.writes.clear();
    link.setBlockSize(64 * 1024);                       // "one packet, whole page" — what it used to do
    const bool unchunked = link.writeFlat(0, data);
    CHECK(!unchunked);
    CHECK(ecu.oversized.load());
    std::printf("  unchunked, the same write is refused (%zu command(s), oversized=%d)\n",
                ecu.writes.size(), int(ecu.oversized.load()));

    link.close();
    ecu.stop = true;
    th.join();
    ::close(master);
    std::printf(g_fails ? "ts write chunking: FAILED (%d)\n" : "ts write chunking: OK\n", g_fails);
    return g_fails ? 1 : 0;
}

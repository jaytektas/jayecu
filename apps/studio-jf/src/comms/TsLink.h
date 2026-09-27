#pragma once

// TsLink — TunerStudio binary-protocol serial link (interop, not DNA: lives at arm's length from the
// native EcuLink, which is untouched). Speaks the msEnvelope CRC framing a TS-compatible ECU (rusEFI,
// legacy MS) uses: each request is <size:2 BE><payload><crc32:4 BE>; each response is
// <size:2 BE><code:1><data><crc32:4 BE> with code 0 = OK. Synchronous, blocking reads with a timeout —
// raw POSIX/Win32 like the DeviceScanner (the framework's JSerialPort is async, which doesn't suit a
// transact-style protocol).

#include "Crc32.h"

#include <chrono>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

class TsLink {
public:
    ~TsLink() { close(); }

    bool open(const std::string& portName, int baud = 115200) {
        close();
#if defined(_WIN32)
        (void)baud;
        const std::string dev = "\\\\.\\" + portName;
        h_ = ::CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { lastError_ = "cannot open " + portName; return false; }
        DCB dcb{}; dcb.DCBlength = sizeof(dcb);
        if (::GetCommState(h_, &dcb)) {
            dcb.BaudRate = CBR_115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
            dcb.fBinary = TRUE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
            ::SetCommState(h_, &dcb);
        }
        COMMTIMEOUTS to{}; to.ReadIntervalTimeout = 20; to.ReadTotalTimeoutConstant = 100;
        ::SetCommTimeouts(h_, &to);
#else
        fd_ = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) { lastError_ = "cannot open " + portName; return false; }
        termios tio{};
        if (::tcgetattr(fd_, &tio) == 0) {
            ::cfmakeraw(&tio);
            const speed_t sp = baud == 115200 ? B115200 : B115200;   // TS links run 115200 in practice
            ::cfsetispeed(&tio, sp); ::cfsetospeed(&tio, sp);
            tio.c_cflag |= (CLOCAL | CREAD);
            tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
            ::tcsetattr(fd_, TCSANOW, &tio);
        }
#endif
        port_ = portName;
        return true;
    }

    void close() {
        if (isOpen()) awaitBurnSettled();      // never drop the link on top of an unfinished burn
#if defined(_WIN32)
        if (h_ != INVALID_HANDLE_VALUE) { ::CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }
#if defined(_WIN32)
    bool isOpen() const { return h_ != INVALID_HANDLE_VALUE; }
#else
    bool isOpen() const { return fd_ >= 0; }
#endif
    const std::string& portName()  const { return port_; }
    const std::string& lastError() const { return lastError_; }

    // The ini's blockingFactor: the most payload the ECU accepts in one read or write.
    void setBlockSize(int n) { block_ = std::max(16, n); }
    int  blockSize() const   { return block_; }

    std::string querySignature() {                                    // 'S' → signature ("" on failure)
        const std::vector<uint8_t> d = transact({ 'S' });
        std::string s(d.begin(), d.end());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    // The ECU's own account of what is wrong with its configuration ([TunerStudio] retrieveConfigError,
    // 'e' on rusEFI). Text, and the point of the exercise: "Config Error" as a lamp says something is
    // wrong; this says WHICH thing, which is the difference between a fix and a guess.
    std::string configErrorText(const std::string& command) {
        if (command.empty()) return {};
        const std::vector<uint8_t> d = transact({ command.begin(), command.end() });
        std::string s(d.begin(), d.end());
        while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }

    std::vector<uint8_t> readPage(int page, int offset, int count) {  // 'R' → raw config bytes
        std::vector<uint8_t> p = { 'R' };
        le16(p, page); le16(p, offset); le16(p, count);
        return transact(p);
    }

    bool writePage(int page, int offset, const std::vector<uint8_t>& data) {   // 'C' write (ack = code 0)
        std::vector<uint8_t> p = { 'C' };
        le16(p, page); le16(p, offset); le16(p, static_cast<int>(data.size()));
        p.insert(p.end(), data.begin(), data.end());
        transact(p);
        return lastError_.empty();
    }

    bool burn(int page) {                                             // 'B' commit to flash (slow; code ≠ 0 ok)
        std::vector<uint8_t> p = { 'B' };
        le16(p, page);
        transact(p, 2000, /*requireOk=*/false);
        // The ack does NOT mean the flash is written. rusEFI's burn only raises a flag; a background task
        // does the write. Measured on the bench: burn, reboot immediately, and the value is back to what
        // flash held — give it the definition's pageActivationDelay and it survives the power cycle.
        //
        // Reads, writes and telemetry all work fine during that window, so this does NOT block them (a
        // sleep here would stall the UI thread the burn timer runs on). Only the two things that can
        // ABANDON the write wait it out: a controller command (the imported dialogs carry "Reboot ECU" and
        // "Reset to DFU" buttons) and closing the link.
        burnUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(burnSettleMs_);
        return lastError_.empty();
    }

    void setBurnSettleMs(int ms) { burnSettleMs_ = std::max(0, ms); }

    // Wait out whatever remains of the post-burn window (no-op when none is pending).
    void awaitBurnSettled() {
        const auto now = std::chrono::steady_clock::now();
        if (burnUntil_ > now) std::this_thread::sleep_for(burnUntil_ - now);
        burnUntil_ = now;
    }

    // PAGE MAP — the ini's geometry, which is what makes the flat image the widgets read a fiction the
    // link has to undo. A TS config is several pages, each with its own offset space and its own wire
    // identifier (rusEFI's second page is the bytes 00 01 = 0x0100, not 1), and a read that runs past a
    // page's end is answered with OUT_OF_RANGE rather than the next page's bytes. Given the map, the flat
    // offsets the Cache hands out are split back into (page, offset) here; given no map, everything
    // behaves as it did before — one page 0 covering the whole image.
    struct Page { int id = 0, base = 0, size = 0; bool burn = true; };
    void setPages(std::vector<Page> pages) { pages_ = std::move(pages); }
    const std::vector<Page>& pages() const { return pages_; }

    // The whole configuration, page by page, as one flat image ({} if any page fails).
    std::vector<uint8_t> readImage(int chunk, int flatSize) {
        if (pages_.empty()) return readRegion(0, 0, flatSize, chunk);
        std::vector<uint8_t> img;
        img.reserve(flatSize);
        for (const auto& pg : pages_) {
            const std::vector<uint8_t> part = readRegion(pg.id, 0, pg.size, chunk);
            if (static_cast<int>(part.size()) != pg.size) return {};
            img.insert(img.end(), part.begin(), part.end());
        }
        return img;
    }

    // A write addressed in FLAT space, split at page boundaries.
    bool writeFlat(int flatOffset, const std::vector<uint8_t>& bytes) {
        if (pages_.empty()) return writePage(0, flatOffset, bytes);
        bool ok = true;
        int done = 0;
        while (done < static_cast<int>(bytes.size())) {
            const int at = flatOffset + done;
            const Page* pg = pageAt(at);
            if (!pg) { lastError_ = "write outside every page (offset " + std::to_string(at) + ")"; return false; }
            const int room = pg->base + pg->size - at;
            // …and never more than one block per packet. blockingFactor is the ECU's receive limit, and
            // exceeding it fails the whole write — invisible while the only writes were single edited
            // fields, fatal the first time a whole image or table goes out.
            const int n = std::min({ room, static_cast<int>(bytes.size()) - done, block_ });
            ok = writePage(pg->id, at - pg->base, { bytes.begin() + done, bytes.begin() + done + n }) && ok;
            if (!ok) return false;
            done += n;
        }
        return ok;
    }

    // Commit every page that HAS a burn command — pages the definition gives an empty one (rusEFI's
    // scatter-offset and trim pages) are RAM-only and answer a burn with an error.
    bool burnAll() {
        if (pages_.empty()) return burn(0);
        bool ok = true;
        for (const auto& pg : pages_) if (pg.burn) ok = burn(pg.id) && ok;
        return ok;
    }

    const Page* pageAt(int flatOffset) const {
        for (const auto& pg : pages_)
            if (flatOffset >= pg.base && flatOffset < pg.base + pg.size) return &pg;
        return nullptr;
    }

    // Read an arbitrary-length region, chunked to `chunk` (the ini's blockingFactor). {} on any miss.
    std::vector<uint8_t> readRegion(int page, int offset, int length, int chunk = 1024) {
        std::vector<uint8_t> out;
        for (int got = 0; got < length; ) {
            const int n = std::min(chunk, length - got);
            const std::vector<uint8_t> part = readPage(page, offset + got, n);
            if (static_cast<int>(part.size()) != n) return {};
            out.insert(out.end(), part.begin(), part.end());
            got += n;
        }
        return out;
    }

    std::vector<uint8_t> readOutputChannels(int offset, int count) {  // 'O' → live telemetry block
        std::vector<uint8_t> p = { 'O' };
        le16(p, offset); le16(p, count);
        return transact(p);
    }

    // A raw CONTROLLER COMMAND — an [ControllerCommands] payload, sent exactly as the definition gives it.
    // No reply is required: several of these (reboot, DFU) deliberately never answer. Check lastError() for
    // whether the bytes actually went out.
    std::vector<uint8_t> sendCommand(const std::vector<uint8_t>& payload, int timeoutMs = 1000) {
        awaitBurnSettled();                    // a command may be "Reboot ECU" — let the burn land first
        return transact(payload, timeoutMs, /*requireOk=*/false);
    }

    // ---- Composite tooth logger ('l' + sub-command) -------------------------
    // rusEFI's engine sniffer feed: 8-byte records of every trigger/coil/injector state change.
    // Sub-commands from its generated headers (TS_COMPOSITE_*): 1 enable, 2 disable, 3 read.
    // We drive it explicitly rather than leaning on the "enable if not enabled" read, so the logger
    // is off again when we are done — it is a per-edge cost on the ECU we asked for and should hand
    // back. Decoding belongs to model/RusefiCycle, not here: this is transport.
    bool compositeEnable()  { sendCommand({ 'l', 0x01 }); return lastError_.empty(); }
    bool compositeDisable() { sendCommand({ 'l', 0x02 }); return lastError_.empty(); }

    // One buffer of composite records, or empty. Empty is NORMAL and not an error: the ECU answers
    // "out of range" whenever no buffer has filled yet, and it only fills on trigger activity or a
    // 5 s timeout — so a caller polls this until it gets something.
    std::vector<uint8_t> compositeRead(int timeoutMs = 1500) {
        return transact({ 'l', 0x03 }, timeoutMs, /*requireOk=*/true);
    }

private:
    static void le16(std::vector<uint8_t>& p, int v) { p.push_back(uint8_t(v & 0xff)); p.push_back(uint8_t((v >> 8) & 0xff)); }

    bool readExactly(std::vector<uint8_t>& out, size_t n, int timeoutMs) {
        const auto start = std::chrono::steady_clock::now();
        while (out.size() < n) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) {
                lastError_ = "read timeout (" + std::to_string(out.size()) + "/" + std::to_string(n) + " bytes)";
                return false;
            }
            uint8_t tmp[512];
#if defined(_WIN32)
            DWORD got = 0;
            if (!::ReadFile(h_, tmp, DWORD(std::min(sizeof(tmp), n - out.size())), &got, nullptr)) { lastError_ = "read error"; return false; }
            if (got > 0) out.insert(out.end(), tmp, tmp + got);
#else
            fd_set rf; FD_ZERO(&rf); FD_SET(fd_, &rf);
            timeval tv{ 0, 50 * 1000 };
            if (::select(fd_ + 1, &rf, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd_, &rf)) {
                const ssize_t got = ::read(fd_, tmp, std::min(sizeof(tmp), n - out.size()));
                if (got > 0) out.insert(out.end(), tmp, tmp + got);
            }
#endif
        }
        return true;
    }

    // msEnvelope round-trip: <size:2 BE><payload><crc32:4 BE> out, <size:2 BE><code:1><data><crc32:4 BE> in.
    // On success lastError_ is cleared, so an empty return (a write ack) is distinguishable from failure.
    std::vector<uint8_t> transact(const std::vector<uint8_t>& payload, int timeoutMs = 1000, bool requireOk = true) {
        lastError_.clear();
        if (!isOpen()) { lastError_ = "port not open"; return {}; }
        const uint16_t sz = uint16_t(payload.size());
        const uint32_t reqCrc = crc32_ieee::compute(reinterpret_cast<const char*>(payload.data()), int(payload.size()));
        std::vector<uint8_t> req = { uint8_t(sz >> 8), uint8_t(sz & 0xff) };
        req.insert(req.end(), payload.begin(), payload.end());
        for (int s = 24; s >= 0; s -= 8) req.push_back(uint8_t((reqCrc >> s) & 0xff));
#if defined(_WIN32)
        ::PurgeComm(h_, PURGE_RXCLEAR);
        DWORD wn = 0; ::WriteFile(h_, req.data(), DWORD(req.size()), &wn, nullptr);
#else
        ::tcflush(fd_, TCIFLUSH);
        if (::write(fd_, req.data(), req.size()) != ssize_t(req.size())) { lastError_ = "write failed"; return {}; }
#endif
        std::vector<uint8_t> hdr;
        if (!readExactly(hdr, 2, timeoutMs)) return {};
        const int n = (hdr[0] << 8) | hdr[1];
        std::vector<uint8_t> body, crcb;
        if (!readExactly(body, size_t(n), timeoutMs) || !readExactly(crcb, 4, timeoutMs)) return {};
        const uint32_t got = (uint32_t(crcb[0]) << 24) | (uint32_t(crcb[1]) << 16) | (uint32_t(crcb[2]) << 8) | crcb[3];
        if (got != crc32_ieee::compute(reinterpret_cast<const char*>(body.data()), int(body.size()))) {
            lastError_ = "response CRC mismatch";
            return {};
        }
        if (requireOk && (body.empty() || body[0] != 0x00)) {          // code 0 = OK
            lastError_ = "ECU error code " + std::to_string(body.empty() ? 0 : body[0]);
            return {};
        }
        return { body.begin() + (body.empty() ? 0 : 1), body.end() }; // strip the code → data
    }

#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    std::string port_, lastError_;
    std::vector<Page> pages_;
    int block_{256};
    int burnSettleMs_ = 0;
    std::chrono::steady_clock::time_point burnUntil_{};
};

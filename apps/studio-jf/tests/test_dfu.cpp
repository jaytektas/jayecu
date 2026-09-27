// The DFU write, against a simulated STM32 ROM bootloader — so everything that can go wrong in the
// protocol is found here and not on an ECU. The simulator keeps a real 2 MB flash with the F767's
// sector map, refuses to program a byte that has not been erased (as flash does), places each block by
// the DfuSe rule  address = pointer + (block - 2) * ITS transfer size,  and resets on leave.
//
// What is pinned: the image lands byte-for-byte at 0x08000000; only the sectors it covers are erased,
// so the tune banks at the top survive; the block size is the DEVICE's; a device error and a bad
// write are both reported rather than passed; and the device is told to leave only after verifying.
//
//   ./build/dfu_test
//
#include "../src/comms/Dfu.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[dfu] %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

struct SimDevice : dfu::Transport {
    static constexpr uint32_t kBase = 0x08000000, kSize = 2 * 1024 * 1024;
    std::vector<uint8_t> flash = std::vector<uint8_t>(kSize, 0x5A);   // "the old firmware and tunes"
    uint16_t xfer = 2048;
    std::string layout = "@Internal Flash  /0x08000000/04*032Kg,01*128Kg,07*256Kg";
    uint8_t state = 2;               // dfuIDLE
    uint8_t status = 0;
    uint32_t pointer = kBase;
    std::vector<uint8_t> pending;    // the DNLOAD waiting for its GETSTATUS
    uint16_t pendingBlock = 0;
    bool pendingOp = false, busyOnce = false;
    std::vector<uint32_t> erased;
    bool left = false;
    bool failErase = false, corruptWrite = false;

    std::vector<std::pair<uint32_t, uint32_t>> sectors() const {
        std::vector<std::pair<uint32_t, uint32_t>> s;
        uint32_t a = kBase;
        for (int i = 0; i < 4; ++i, a += 32768)  s.push_back({ a, 32768 });
        s.push_back({ a, 131072 }); a += 131072;
        for (int i = 0; i < 7; ++i, a += 262144) s.push_back({ a, 262144 });
        return s;
    }
    void error(uint8_t st) { state = 10; status = st; }

    void execute() {
        if (pendingBlock == 0 && pending.empty()) { left = true; state = 7; return; }   // leave
        if (pendingBlock == 0) {
            const uint32_t addr = pending.size() >= 5 ? (pending[1] | pending[2] << 8 | pending[3] << 16 | uint32_t(pending[4]) << 24) : 0;
            if (pending[0] == 0x21) { pointer = addr; state = 5; return; }
            if (pending[0] == 0x41) {
                if (failErase) { error(0x0A); return; }
                for (auto [a, sz] : sectors()) if (a == addr) {
                    std::memset(flash.data() + (a - kBase), 0xFF, sz);
                    erased.push_back(a); state = 5; return;
                }
                error(0x08); return;                                     // not a sector start
            }
            error(0x0F); return;
        }
        const uint32_t addr = pointer + uint32_t(pendingBlock - 2) * xfer;
        if (addr < kBase || addr + pending.size() > kBase + kSize) { error(0x08); return; }
        for (size_t i = 0; i < pending.size(); ++i) {
            uint8_t& cell = flash[addr - kBase + i];
            if (cell != 0xFF) { error(0x06); return; }                  // programming un-erased flash
            cell = pending[i];
        }
        if (corruptWrite && pendingBlock == 7) flash[addr - kBase + 3] ^= 0x01;
        state = 5;
    }

    bool controlOut(uint8_t request, uint16_t value, const uint8_t* data, uint16_t len) override {
        if (left) return false;
        switch (request) {
        case 1:                                                          // DNLOAD
            if (state != 2 && state != 5) { error(0x0F); return true; }
            if (len > xfer) { error(0x0F); return true; }
            pending.assign(data, data + len); pendingBlock = value; pendingOp = true; busyOnce = true;
            state = 3;                                                   // dfuDNLOAD_SYNC
            return true;
        case 4: if (state == 10) { state = 2; status = 0; } return true;   // CLRSTATUS
        case 6: if (state == 5 || state == 9) state = 2; return true;      // ABORT
        }
        return false;
    }
    int controlIn(uint8_t request, uint16_t value, uint8_t* data, uint16_t len) override {
        if (left) return -1;
        if (request == 3) {                                              // GETSTATUS
            if (pendingOp) {
                if (busyOnce) { busyOnce = false; state = 4; }           // dfuDNBUSY, once
                else { pendingOp = false; execute(); }
            }
            data[0] = status; data[1] = 1; data[2] = 0; data[3] = 0; data[4] = state; data[5] = 0;
            return 6;
        }
        if (request == 2) {                                              // UPLOAD
            if (state != 2 && state != 9) { error(0x0F); return -1; }
            const uint32_t addr = pointer + uint32_t(value - 2) * xfer;
            const uint16_t n = std::min<uint16_t>(len, xfer);
            std::memcpy(data, flash.data() + (addr - kBase), n);
            state = 9;
            return n;
        }
        return -1;
    }
    std::string layoutString() const override { return layout; }
    uint16_t transferSize() const override { return xfer; }
};

static std::vector<uint8_t> image(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = 12345;
    for (auto& b : v) { x = x * 1103515245 + 12345; b = uint8_t(x >> 16); }
    return v;
}

int main() {
    const auto sectors = dfu::parseLayout("@Internal Flash  /0x08000000/04*032Kg,01*128Kg,07*256Kg");
    check(sectors.size() == 12 && sectors[4].address == 0x08020000 && sectors[4].size == 131072 &&
          sectors[11].address == 0x081C0000, "the F767 layout string parses to its 12 sectors");
    check(dfu::parseLayout("rubbish").empty(), "a string that is not a layout parses to nothing");

    std::atomic<bool> noCancel{ false };
    const std::vector<uint8_t> fw = image(571180);                       // the size of today's code.bin

    {
        SimDevice d;
        std::string err;
        int last = -1; bool monotonic = true;
        const bool ok = dfu::flash(d, fw, dfu::kFlashBase,
            [&](const dfu::Progress& p) { if (p.percent < last) monotonic = false; last = p.percent; }, noCancel, err);
        check(ok, ("a 571 KB image flashes (" + err + ")").c_str());
        check(std::memcmp(d.flash.data(), fw.data(), fw.size()) == 0, "the image is in flash byte for byte at 0x08000000");
        check(d.erased.size() == 7, "exactly the 7 sectors the image covers are erased");
        check(d.flash[0x180000] == 0x5A && d.flash[0x1C0000] == 0x5A, "the tune banks at 0x08180000 / 0x081C0000 are untouched");
        check(d.left, "the device is told to leave DFU");
        check(monotonic && last == 100, "progress only goes forward, and ends at 100");
    }
    {
        SimDevice d; d.xfer = 1024;                                      // a device with a different block size
        std::string err;
        check(dfu::flash(d, fw, dfu::kFlashBase, nullptr, noCancel, err) &&
              std::memcmp(d.flash.data(), fw.data(), fw.size()) == 0, "the block size is the device's, whatever it is");
    }
    {
        SimDevice d; d.failErase = true;
        std::string err;
        check(!dfu::flash(d, fw, dfu::kFlashBase, nullptr, noCancel, err) && !d.left && !err.empty(),
              "a failed erase is reported and the device stays in DFU");
    }
    {
        SimDevice d; d.corruptWrite = true;
        std::string err;
        const bool ok = dfu::flash(d, fw, dfu::kFlashBase, nullptr, noCancel, err);
        check(!ok && !d.left && err.find("does not hold") != std::string::npos,
              "a bad write is caught by the read-back, before leaving");
    }
    {
        // THE REAL CHIP, as probed: STM32F767 ROM bootloader, 2048-byte blocks, and it comes up in
        // dfuERROR (status 10) after the application jumps to it — which must be cleared first.
        SimDevice d; d.state = 10; d.status = 10;
        std::string err;
        check(dfu::flash(d, fw, dfu::kFlashBase, nullptr, noCancel, err) && d.left,
              "a device that starts in dfuERROR (as the real F767 does) is cleared and flashed");
    }
    {
        SimDevice d; d.state = 5;                                        // left mid-download by an earlier try
        std::string err;
        check(dfu::flash(d, fw, dfu::kFlashBase, nullptr, noCancel, err), "a device left mid-job by an earlier attempt is recovered");
    }
    {
        SimDevice d;
        std::atomic<bool> cancel{ true };
        std::string err;
        check(!dfu::flash(d, fw, dfu::kFlashBase, nullptr, cancel, err) && d.flash[0] == 0x5A,
              "cancel before the erase leaves the old firmware in place");
    }
    {
        SimDevice d;
        std::string err;
        check(!dfu::flash(d, image(3 * 1024 * 1024), dfu::kFlashBase, nullptr, noCancel, err) && d.erased.empty(),
              "an image bigger than the flash is refused before anything is erased");
    }

    std::printf("\n[dfu] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}

// READ-ONLY probe of the ECU's DFU bootloader through the studio's own USB layer: open it, print the
// flash layout and block size it declares, and tell it to leave (start the firmware already there).
// Nothing is erased or written. Put the ECU in DFU first (the `dfu` CLI command).
//
//   cmake --build build --target dfu_probe && ./build/dfu_probe
//
#include "../src/comms/Dfu.h"

#include <cstdio>

int main() {
    std::string err;
    auto dev = dfu::openDevice(err);
    if (!dev) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    std::printf("layout:        %s\n", dev->layoutString().c_str());
    std::printf("transfer size: %u\n", dev->transferSize());
    const auto sectors = dfu::parseLayout(dev->layoutString());
    std::printf("sectors:       %zu", sectors.size());
    if (!sectors.empty()) std::printf("  (0x%08X .. 0x%08X)", sectors.front().address,
                                      sectors.back().address + sectors.back().size);
    std::printf("\n");

    uint8_t st[6] = {};
    const int n = dev->controlIn(3, 0, st, 6);                 // GETSTATUS
    std::printf("status:        %s state=%u status=%u\n", n == 6 ? "ok" : "FAILED", st[4], st[0]);
    if (st[4] == 10) {                                          // dfuERROR: clear it, or nothing else is accepted
        dev->controlOut(4, 0, nullptr, 0);                      // CLRSTATUS
        dev->controlIn(3, 0, st, 6);
        std::printf("cleared:       state=%u status=%u\n", st[4], st[0]);
    }

    // Leave: address pointer to the start of flash, then an empty download; the device resets into
    // the firmware that is already there.
    const uint8_t setAddr[5] = { 0x21, 0x00, 0x00, 0x00, 0x08 };
    dev->controlOut(1, 0, setAddr, 5);
    for (int i = 0; i < 20; ++i) { if (dev->controlIn(3, 0, st, 6) != 6 || st[4] == 5) break; }
    dev->controlOut(1, 0, nullptr, 0);
    for (int i = 0; i < 20; ++i) { if (dev->controlIn(3, 0, st, 6) != 6 || st[4] == 7 || st[4] == 8) break; }
    std::printf("left DFU\n");
    return 0;
}

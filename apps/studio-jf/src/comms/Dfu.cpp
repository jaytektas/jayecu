#include "Dfu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace dfu {

namespace {

// DFU 1.1 class requests, and the DfuSe commands ST carries inside DNLOAD block 0.
enum : uint8_t { DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6 };
enum : uint8_t { CMD_SET_ADDRESS = 0x21, CMD_ERASE = 0x41 };
enum : uint8_t { dfuIDLE = 2, dfuDNBUSY = 4, dfuDNLOAD_IDLE = 5, dfuMANIFEST = 7, dfuUPLOAD_IDLE = 9, dfuERROR = 10 };

struct Status { uint8_t status = 0, state = 0; uint32_t pollMs = 0; };

bool getStatus(Transport& d, Status& s) {
    uint8_t b[6] = {};
    if (d.controlIn(GETSTATUS, 0, b, 6) < 6) return false;
    s.status = b[0];
    s.pollMs = uint32_t(b[1]) | uint32_t(b[2]) << 8 | uint32_t(b[3]) << 16;
    s.state  = b[4];
    return true;
}

std::string hex32(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b; }

// Poll GETSTATUS until the device reaches `want`. In DfuSe the GETSTATUS after a DNLOAD is what sets
// the operation going, so this is not only waiting — it is the second half of every command.
bool waitFor(Transport& d, uint8_t want, int timeoutMs, std::string& error, const std::string& what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    Status s;
    while (true) {
        if (!getStatus(d, s)) { error = what + ": the device stopped answering"; return false; }
        if (s.state == want) return true;
        if (s.state == dfuERROR) {
            error = what + " failed (DFU status " + std::to_string(s.status) + ")";
            return false;
        }
        if (std::chrono::steady_clock::now() > until) { error = what + " timed out"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(s.pollMs, 5)));
    }
}

bool command(Transport& d, uint8_t cmd, uint32_t address, int timeoutMs, std::string& error, const std::string& what) {
    const uint8_t b[5] = { cmd, uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16), uint8_t(address >> 24) };
    if (!d.controlOut(DNLOAD, 0, b, 5)) { error = what + ": USB transfer failed"; return false; }
    return waitFor(d, dfuDNLOAD_IDLE, timeoutMs, error, what);
}

// Back to dfuIDLE from wherever an earlier attempt left it.
bool toIdle(Transport& d, std::string& error) {
    Status s;
    if (!getStatus(d, s)) { error = "the device did not answer"; return false; }
    if (s.state == dfuERROR) { d.controlOut(CLRSTATUS, 0, nullptr, 0); if (!getStatus(d, s)) { error = "the device did not answer"; return false; } }
    if (s.state == dfuDNLOAD_IDLE || s.state == dfuUPLOAD_IDLE) { d.controlOut(ABORT, 0, nullptr, 0); if (!getStatus(d, s)) { error = "the device did not answer"; return false; } }
    if (s.state != dfuIDLE) { error = "the device is in an unexpected state (" + std::to_string(s.state) + ")"; return false; }
    return true;
}

}  // namespace

std::vector<Sector> parseLayout(const std::string& layout) {
    // "@Internal Flash  /0x08000000/04*032Kg,01*128Kg,07*256Kg"
    std::vector<Sector> out;
    const size_t a = layout.find('/');
    if (a == std::string::npos) return out;
    const size_t b = layout.find('/', a + 1);
    if (b == std::string::npos) return out;
    char* end = nullptr;
    uint32_t addr = uint32_t(std::strtoul(layout.substr(a + 1, b - a - 1).c_str(), &end, 16));
    if (addr == 0) return out;
    std::string groups = layout.substr(b + 1);
    const size_t more = groups.find('/');          // a second region would follow another '/'
    if (more != std::string::npos) groups.resize(more);
    size_t pos = 0;
    while (pos < groups.size()) {
        size_t comma = groups.find(',', pos);
        if (comma == std::string::npos) comma = groups.size();
        const std::string g = groups.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t star = g.find('*');
        if (star == std::string::npos) return {};
        const long count = std::strtol(g.substr(0, star).c_str(), nullptr, 10);
        size_t i = star + 1;
        uint32_t size = 0;
        while (i < g.size() && g[i] >= '0' && g[i] <= '9') size = size * 10 + uint32_t(g[i++] - '0');
        if (i < g.size() && (g[i] == 'K' || g[i] == 'k')) size *= 1024;
        else if (i < g.size() && (g[i] == 'M' || g[i] == 'm')) size *= 1024 * 1024;
        if (count <= 0 || size == 0) return {};
        for (long n = 0; n < count; ++n, addr += size) out.push_back({ addr, size });
    }
    return out;
}

bool flash(Transport& d, const std::vector<uint8_t>& image, uint32_t address,
           const std::function<void(const Progress&)>& progress, const std::atomic<bool>& cancel,
           std::string& error) {
    auto say = [&](const char* stage, int pct) { if (progress) progress({ stage, pct }); };
    if (image.empty()) { error = "the firmware image is empty"; return false; }
    const uint16_t xfer = d.transferSize();
    if (xfer == 0) { error = "the device did not say what block size it takes"; return false; }
    const uint32_t end = address + uint32_t(image.size());

    // WHICH SECTORS. Every one the image touches, and none it does not. A sector that starts below the
    // image would erase whatever sits in front of it, so that is refused rather than done.
    std::vector<Sector> erase;
    for (const Sector& s : parseLayout(d.layoutString()))
        if (s.address < end && s.address + s.size > address) {
            if (s.address < address) { error = "the image does not start on a sector boundary"; return false; }
            erase.push_back(s);
        }
    if (erase.empty()) { error = "could not read the flash layout from the device"; return false; }
    if (erase.back().address + erase.back().size < end) { error = "the image is larger than the flash"; return false; }

    if (!toIdle(d, error)) return false;

    say("Erasing", 0);
    for (size_t i = 0; i < erase.size(); ++i) {
        if (cancel) { error = "cancelled"; return false; }
        // A 256 KB sector takes seconds to erase; allow plenty.
        if (!command(d, CMD_ERASE, erase[i].address, 20000, error, "Erasing " + hex32(erase[i].address))) return false;
        say("Erasing", int(25 * (i + 1) / erase.size()));
    }
    if (cancel) { error = "cancelled"; return false; }

    // From here the old firmware is gone, so the job finishes whatever is asked of it.
    if (!command(d, CMD_SET_ADDRESS, address, 5000, error, "Setting the address")) return false;
    const size_t blocks = (image.size() + xfer - 1) / xfer;
    say("Writing", 25);
    for (size_t n = 0; n < blocks; ++n) {
        const size_t off = n * xfer, len = std::min<size_t>(xfer, image.size() - off);
        if (!d.controlOut(DNLOAD, uint16_t(n + 2), image.data() + off, uint16_t(len))) {
            error = "Writing " + hex32(address + uint32_t(off)) + ": USB transfer failed"; return false;
        }
        if (!waitFor(d, dfuDNLOAD_IDLE, 5000, error, "Writing " + hex32(address + uint32_t(off)))) return false;
        say("Writing", 25 + int(55 * (n + 1) / blocks));
    }

    // READ IT ALL BACK. Upload uses the same block arithmetic, from an address pointer set in dfuIDLE.
    say("Verifying", 80);
    d.controlOut(ABORT, 0, nullptr, 0);
    if (!command(d, CMD_SET_ADDRESS, address, 5000, error, "Setting the address to verify")) return false;
    d.controlOut(ABORT, 0, nullptr, 0);
    std::vector<uint8_t> back(xfer);
    for (size_t n = 0; n < blocks; ++n) {
        const size_t off = n * xfer, len = std::min<size_t>(xfer, image.size() - off);
        const int got = d.controlIn(UPLOAD, uint16_t(n + 2), back.data(), xfer);
        if (got < int(len)) { error = "Verifying " + hex32(address + uint32_t(off)) + ": read failed"; return false; }
        if (std::memcmp(back.data(), image.data() + off, len) != 0) {
            error = "Verifying: the flash at " + hex32(address + uint32_t(off)) + " does not hold what was written";
            return false;
        }
        say("Verifying", 80 + int(19 * (n + 1) / blocks));
    }
    d.controlOut(ABORT, 0, nullptr, 0);

    // LEAVE: point at the image and send an empty download; the GETSTATUS after it starts the firmware.
    // The device resets under us, so a failed or missing answer here is the expected outcome.
    if (!command(d, CMD_SET_ADDRESS, address, 5000, error, "Setting the start address")) return false;
    d.controlOut(DNLOAD, 0, nullptr, 0);
    // Keep asking until the device either stops answering (it has reset into the new firmware) or says
    // it is manifesting — one GETSTATUS is not enough for a device that reports busy first.
    for (int i = 0; i < 50; ++i) {
        Status s;
        if (!getStatus(d, s) || s.state == dfuMANIFEST || s.state == dfuMANIFEST + 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(s.pollMs, 5)));
    }
    say("Done", 100);
    return true;
}

}  // namespace dfu

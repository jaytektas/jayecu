#pragma once

// DeviceScanner — probe the serial bus for jayecu ECUs by POLLING each port for its identity. The ECU
// never broadcasts unsolicited (identity is polled: host sends OMNI_CMD_IDENTITY 'Q', ECU replies with a
// PACKET_IDENTITY frame — see firmware/Comms/OmniProtocol.h). So per port we send a 'Q' request and read
// the reply, decoding "jayecu <board> <ver> <build> <hash> <uid>". Raw platform serial calls rather than
// the framework's JSerialPort so the probe can BLOCK with a timeout — JSerialPort is async (a signal per
// chunk on the main thread), which doesn't suit a synchronous per-port probe.
// Frame: [AA 55][type][rsv][len:u16 LE][ts:u64][seq:u16][payload][crc16:2 LE]; identity = type 0x07.
// Caller passes only jayecu-VID/PID ports, so 'Q' is never written to a foreign serial device.

#include "Products.h"
#include <j/io/SerialPort.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <atomic>
#include <thread>
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

class DeviceScanner {
public:
    struct Device {
        std::string port, signature;                                  // /dev/ttyACMx + full identity text
        std::string product, board, fwVersion, fwHash, layoutHash, uid;   // parsed from the signature
        // Canonical descriptor filename: board + layoutHash is the only identity that matters.
        std::string label() const {
            std::string s = product + " " + board + " " + fwVersion;
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            return s + "   (" + port + ")";
        }
    };

    // WHICH ENUMERATED PORTS ARE WORTH PROBING. Not a detail of the caller: we WRITE a 'Q' to every
    // candidate, and the answer is a platform question rather than a preference.
    //
    // On POSIX a USB serial device is named for its driver — ttyACM (CDC) or ttyUSB (a converter) —
    // and everything else in /sys/class/tty is a virtual console or an on-board UART that must not be
    // written to. Windows has no such convention: a USB CDC device and a motherboard header are both
    // "COMn", so the name says nothing and every enumerated port is a candidate. Asking for ttyACM
    // there matched nothing at all, which is what left the Windows build unable to see any ECU.
    static bool isUsbSerial(const jf::JSerialPortInfo& info) {
#if defined(_WIN32)
        return info.port.rfind("COM", 0) == 0;
#else
        const std::string nm = info.port.substr(info.port.rfind('/') + 1);
        return nm.rfind("ttyACM", 0) == 0 || nm.rfind("ttyUSB", 0) == 0;
#endif
    }

    // Every candidate the system is offering, in enumeration order.
    //
    // NARROWED TO USB SERIAL DEVICES, which this file has said twice that the caller does and, on
    // Windows, did not: isUsbSerial() there is "the name begins with COM", and every built-in and
    // virtual port in the machine begins with COM. The scan then OPENS each one and waits perPortMs
    // for an identity reply — so a PC with a long COM list spends ports x 1.5s before it reaches the
    // ECU, on the thread that draws the window. Under wine that is 35 ports (COM1-32 are /dev/ttyS*,
    // which all exist and all open in a fifth of a millisecond, and none of which ever answers): about
    // 52 seconds of frozen UI and then no ECU found.
    //
    // A USB serial device is one the system can name a vendor and product for — SetupAPI carries
    // VID/PID, a motherboard COM port does not — which is the same question the Linux branch asks by
    // looking for ttyACM/ttyUSB. NOT narrowed to jayecu's own 0483:5740: a rusEFI board over the
    // TunerStudio protocol is a device this studio connects to, and it has its own ids.
    //
    // …unless NOTHING reports ids, which is wine: its setupapi has no DEVCLASS_PORTS, so every port
    // arrives through the HARDWARE\DEVICEMAP\SERIALCOMM fallback with no ids on it. Filtering on
    // absent information would find nothing at all, so there the name test still decides.
    static std::vector<std::string> usbSerialPorts() {
        const std::vector<jf::JSerialPortInfo> all = jf::JSerialPort::availablePorts();
        std::vector<std::string> out;
#if defined(_WIN32)
        // WINDOWS ONLY. The Linux test is already a USB one — ttyACM/ttyUSB are USB device nodes and
        // nothing else is offered — so it keeps deciding there, unchanged: an adapter whose ids the
        // system cannot read is still a port a person plugged in, and dropping it would be this same
        // bug pointing the other way.
        const bool anyIds = std::any_of(all.begin(), all.end(),
                                        [](const jf::JSerialPortInfo& i) { return i.hasVidPid; });
        for (const auto& info : all)
            if (anyIds ? info.hasVidPid : isUsbSerial(info)) out.push_back(info.port);
#else
        for (const auto& info : all)
            if (isUsbSerial(info)) out.push_back(info.port);
#endif
        return out;
    }

    // The one port to try when enumeration came up empty — a last guess at the bench ECU, not a
    // discovery. There is no equivalent guess to make on Windows, where COM numbers are assigned.
    static const char* fallbackPort() {
#if defined(_WIN32)
        return "";
#else
        return "/dev/ttyACM0";   // bench ECU (USB-CDC)
#endif
    }

    // Poll each given port for its identity; return the ECUs that answered. Ports should already be
    // narrowed to jayecu-VID/PID candidates by the caller (we WRITE a 'Q' request to each). legacyProbe
    // additionally sends one bare unframed 'Q' byte at open — harmless to a jayecu, but a rusEFI/legacy
    // unit responds to it immediately (original DeviceScanner parity).
    static std::vector<Device> scan(const std::vector<std::string>& ports, int perPortMs = 1500,
                                    bool legacyProbe = false) {
        // IN PARALLEL, because the ports are independent devices and this waits on all of them.
        //
        // One at a time made the cost of a scan (number of ports) x (per-port budget), paid on the
        // thread that draws the window — so connecting on a machine with a long COM list froze the UI
        // long enough for the window manager to offer to kill the app, which is what it looked like
        // from the outside: press Connect, nothing happens, "not responding". Thirty-five ports at
        // 1200 ms is 42 seconds, and no amount of shortening the per-port budget fixes an O(ports)
        // wait — it only makes each dead port cheaper.
        //
        // Nothing is shared: scanPort opens its own handle, writes, reads and closes. Bounded lanes
        // rather than a thread per port, because "open every serial device on the machine at once" is
        // a different kind of rude, and eight lanes already turn the worst case into about a second.
        // Results are written to a slot each, so the order is the enumeration order however the lanes
        // happen to finish — a scan that lists ECUs in a different order each run is its own bug.
        std::vector<Device> found;
        if (ports.empty()) return found;
        std::vector<std::optional<Device>> slot(ports.size());
        const unsigned lanes = std::min<size_t>(8, ports.size());
        std::atomic<size_t> next{0};
        std::vector<std::thread> crew;
        crew.reserve(lanes);
        for (unsigned i = 0; i < lanes; ++i)
            crew.emplace_back([&] {
                for (size_t k = next++; k < ports.size(); k = next++)
                    slot[k] = scanPort(ports[k], perPortMs, legacyProbe);
            });
        for (auto& t : crew) t.join();
        for (auto& d : slot)
            if (d) found.push_back(*d);
        return found;
    }

#if defined(_WIN32)
    // Poll one COM port for up to timeoutMs (Win32): open \\.\COMx, send 'Q', read the reply frame.
    static std::optional<Device> scanPort(const std::string& port, int timeoutMs = 1500, bool legacyProbe = false) {
        const std::string dev_ = "\\\\.\\" + port;                         // \\.\COMx opens any COM index
        HANDLE h = ::CreateFileA(dev_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::nullopt;                // busy (live session) / no such port
        DCB dcb{}; dcb.DCBlength = sizeof(dcb);
        if (::GetCommState(h, &dcb)) {
            dcb.BaudRate = CBR_115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
            dcb.fBinary = TRUE; dcb.fParity = FALSE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
            ::SetCommState(h, &dcb);
        }
        COMMTIMEOUTS to{}; to.ReadIntervalTimeout = 50; to.ReadTotalTimeoutConstant = 200;   // ~200 ms read slices
        ::SetCommTimeouts(h, &to);
        if (legacyProbe) { DWORD qn = 0; ::WriteFile(h, "Q", 1, &qn, nullptr); }   // bare byte for legacy units
        const std::vector<uint8_t> req = identityRequest();
        std::vector<uint8_t> buf; Device dev; bool got = false;
        const auto start = std::chrono::steady_clock::now();
        // A PORT THAT HAS SAID NOTHING AT ALL IS NOT AN ECU. The full budget is for a device that is
        // answering and simply has not finished — it is not for a dead port to burn on our behalf.
        // Every motherboard COM port opens instantly and then says nothing for the whole timeout, so
        // the cost of a machine with a long COM list is ports x timeoutMs of frozen UI: under wine,
        // where COM1-32 are /dev/ttyS* that all open and none reply, that is 35 x 1.5s.
        //
        // An ECU answers its identity immediately — it is a framed reply to one byte, written the
        // moment the request lands — so "nothing whatsoever after kSilentMs" is a sound verdict, while
        // a device mid-reply has bytes in `buf` and keeps the whole budget.
        constexpr int kSilentMs = 250;
        while (true) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) break;
            if (buf.empty() && elapsed >= kSilentMs) break;
            DWORD wn = 0; ::WriteFile(h, req.data(), static_cast<DWORD>(req.size()), &wn, nullptr);
            uint8_t tmp[512]; DWORD n = 0;
            if (::ReadFile(h, tmp, sizeof(tmp), &n, nullptr) && n > 0) {
                buf.insert(buf.end(), tmp, tmp + n);
                if (parseIdentity(buf, dev)) { got = true; break; }
                if (buf.size() > 16384) buf.erase(buf.begin(), buf.end() - 8192);
            }
        }
        ::CloseHandle(h);
        if (!got) return std::nullopt;
        dev.port = port;
        return dev;
    }
#else
    // Poll one port for up to timeoutMs: send an identity request ('Q') and read the reply frame.
    static std::optional<Device> scanPort(const std::string& port, int timeoutMs = 1500, bool legacyProbe = false) {
        const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) return std::nullopt;                                   // busy (live session) / no perms
        termios tio{};
        if (::tcgetattr(fd, &tio) == 0) {
            ::cfmakeraw(&tio);
            ::cfsetispeed(&tio, B115200); ::cfsetospeed(&tio, B115200);
            tio.c_cflag |= (CLOCAL | CREAD);
            tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
            ::tcsetattr(fd, TCSANOW, &tio);
        }
        // A PROBE THAT COULD NOT BE SENT IS NOT A SILENT PORT. write() is warn_unused_result and the (void)
        // cast never silenced it — worse, it threw away the one fact that distinguishes "this device said
        // nothing" from "we never managed to ask it".
        if (legacyProbe && ::write(fd, "Q", 1) < 0) { ::close(fd); return std::nullopt; }   // legacy/rusEFI
        const std::vector<uint8_t> req = identityRequest();                // 'Q' — polled identity
        std::vector<uint8_t> buf;
        Device dev; bool got = false;
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < timeoutMs) {
            if (::write(fd, req.data(), req.size()) < 0) break;            // (re)poll each slice; a port that
                                                                           // cannot be written to is done
            fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
            timeval tv{ 0, 200 * 1000 };                                   // 200 ms slices
            if (::select(fd + 1, &rf, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &rf)) {
                uint8_t tmp[512];
                const ssize_t n = ::read(fd, tmp, sizeof(tmp));
                if (n > 0) {
                    buf.insert(buf.end(), tmp, tmp + n);
                    if (parseIdentity(buf, dev)) { got = true; break; }
                    if (buf.size() > 16384) buf.erase(buf.begin(), buf.end() - 8192);   // cap the hunt window
                }
            }
        }
        ::close(fd);
        if (!got) return std::nullopt;
        dev.port = port;
        return dev;
    }
#endif

private:
    // CRC16-CCITT (poly 0x1021, init 0xFFFF) — the firmware's omni_crc16, same as EcuLink's frame check.
    static uint16_t crc16ccitt(const uint8_t* data, int len) {
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                                             : static_cast<uint16_t>(crc << 1);
        }
        return crc;
    }

    // A bare identity-request frame: [AA 55]['Q'][rsv][len:u16 LE][ts:u64][seq:u16][crc16:2 LE], no payload.
    // len = total (18). typeId 'Q' = OMNI_CMD_IDENTITY (firmware/Comms/OmniProtocol.h).
    static std::vector<uint8_t> identityRequest() {
        std::vector<uint8_t> f = { 0xAA, 0x55, 'Q', 0x00,
                                   18, 0,                                  // len u16 LE = 18
                                   0, 0, 0, 0, 0, 0, 0, 0,                 // timestamp u64
                                   1, 0 };                                 // sequence u16 LE
        const uint16_t crc = crc16ccitt(f.data(), static_cast<int>(f.size()));
        f.push_back(static_cast<uint8_t>(crc & 0xFF));
        f.push_back(static_cast<uint8_t>(crc >> 8));
        return f;
    }
    static bool parseIdentity(const std::vector<uint8_t>& buf, Device& dev) {
        const int sz = static_cast<int>(buf.size());
        for (int i = 0; i + 18 <= sz;) {
            if (buf[i] != 0xAA || buf[i + 1] != 0x55) { ++i; continue; }
            const uint16_t total = static_cast<uint16_t>(buf[i + 4] | (buf[i + 5] << 8));
            if (total < 18 || total > 8192) { ++i; continue; }
            if (i + total > sz) break;                                     // wait for the rest of the frame
            const uint8_t* fr = buf.data() + i;
            const uint16_t want = static_cast<uint16_t>(fr[total - 2] | (fr[total - 1] << 8));
            if (crc16ccitt(fr, total - 2) == want && fr[2] == 0x07) {      // valid IDENTITY frame
                const int paylen = total - 18;
                std::string sig(reinterpret_cast<const char*>(fr + 16), reinterpret_cast<const char*>(fr + 16) + (paylen > 0 ? paylen : 0));
                if (isKnownProduct(sig)) {
                    dev.signature = sig;
                    std::vector<std::string> parts; size_t p = 0;
                    while (p <= sig.size()) {
                        const size_t sp = sig.find(' ', p);
                        parts.push_back(sig.substr(p, sp == std::string::npos ? std::string::npos : sp - p));
                        if (sp == std::string::npos) break;
                        p = sp + 1;
                    }
                    if (parts.size() > 0) dev.product    = parts[0];
                    if (parts.size() > 1) dev.board      = parts[1];
                    if (parts.size() > 2) dev.fwVersion  = parts[2];
                    if (parts.size() > 3) dev.fwHash     = parts[3];
                    if (parts.size() > 4) dev.layoutHash = parts[4];
                    if (parts.size() > 5) dev.uid        = parts[5];
                    return true;
                }
            }
            i += total;
        }
        return false;
    }
};

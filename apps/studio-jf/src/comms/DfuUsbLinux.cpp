// The DFU transport on Linux: the kernel's usbfs, directly — no libusb. DFU needs only control
// transfers and two facts from the descriptors, and usbfs gives both through one device file.
//
// PERMISSION: /dev/bus/usb/BBB/DDD for 0483:df11 must be writable by the person running the studio.
// tools/udev/70-jayecu.rules grants it (uaccess for the desktop session).

#if defined(__linux__)

#include "Dfu.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dfu {

namespace {

// The sysfs folder of the first device with this VID:PID, or "".
std::string findSysfs() {
    DIR* d = opendir("/sys/bus/usb/devices");
    if (!d) return {};
    std::string found;
    while (dirent* e = readdir(d)) {
        const std::string dir = std::string("/sys/bus/usb/devices/") + e->d_name;
        unsigned vid = 0, pid = 0;
        std::ifstream fv(dir + "/idVendor"), fp(dir + "/idProduct");
        if (!(fv >> std::hex >> vid) || !(fp >> std::hex >> pid)) continue;
        if (vid == kStmVid && pid == kStmDfuPid) { found = dir; break; }
    }
    closedir(d);
    return found;
}

class LinuxTransport : public Transport {
public:
    LinuxTransport(int fd, int iface, uint16_t xfer, std::string layout)
        : fd_(fd), iface_(iface), xfer_(xfer), layout_(std::move(layout)) {}
    ~LinuxTransport() override {
        unsigned int i = unsigned(iface_);
        ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &i);
        close(fd_);
    }
    bool controlOut(uint8_t request, uint16_t value, const uint8_t* data, uint16_t len) override {
        return control(0x21, request, value, uint16_t(iface_), const_cast<uint8_t*>(data), len) >= 0;
    }
    int controlIn(uint8_t request, uint16_t value, uint8_t* data, uint16_t len) override {
        return control(0xA1, request, value, uint16_t(iface_), data, len);
    }
    std::string layoutString() const override { return layout_; }
    uint16_t transferSize() const override { return xfer_; }

    static int control(int fd, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t len) {
        usbdevfs_ctrltransfer c{};
        c.bRequestType = type; c.bRequest = request; c.wValue = value; c.wIndex = index;
        c.wLength = len; c.timeout = 5000; c.data = data;
        return ioctl(fd, USBDEVFS_CONTROL, &c);
    }

private:
    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t len) {
        return control(fd_, type, request, value, index, data, len);
    }
    int fd_, iface_;
    uint16_t xfer_;
    std::string layout_;
};

}  // namespace

bool devicePresent() { return !findSysfs().empty(); }

static std::unique_ptr<Transport> openOnce(std::string& error, bool& hung);

// A bootloader whose job was cut off mid-transfer (the studio closed during a flash) can stop answering
// on USB altogether — every request fails, even the device descriptor. A USB reset of the port brings
// it back WITHOUT powering it down, so it is still in DFU afterwards. Observed on the bench, and exactly
// what this does: one reset, one more try.
std::unique_ptr<Transport> openDevice(std::string& error) {
    bool hung = false;
    auto dev = openOnce(error, hung);
    if (dev || !hung) return dev;
    const std::string sys = findSysfs();
    int bus = 0, num = 0;
    { std::ifstream b(sys + "/busnum"), d(sys + "/devnum"); b >> bus; d >> num; }
    char node[64];
    std::snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, num);
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return nullptr;
    ioctl(fd, USBDEVFS_RESET, 0);
    close(fd);
    for (int i = 0; i < 30 && !devicePresent(); ++i) usleep(100 * 1000);
    usleep(500 * 1000);
    error.clear();
    return openOnce(error, hung);
}

static std::unique_ptr<Transport> openOnce(std::string& error, bool& hung) {
    hung = false;
    const std::string sys = findSysfs();
    if (sys.empty()) { error = "the ECU's bootloader is not on the USB bus"; return nullptr; }
    int bus = 0, dev = 0;
    { std::ifstream b(sys + "/busnum"), d(sys + "/devnum"); b >> bus; d >> dev; }
    char node[64];
    std::snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, dev);
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("cannot open ") + node + " (" + std::strerror(errno) + ")" +
                (errno == EACCES ? " \xE2\x80\x94 install tools/udev/70-jayecu.rules" : "");
        return nullptr;
    }

    // Reading the device file returns the device descriptor followed by the active configuration's.
    uint8_t desc[4096];
    const ssize_t n = read(fd, desc, sizeof desc);
    int iface = -1, iString = 0;
    uint16_t xfer = 0;
    for (ssize_t i = 18; n > 18 && i + 2 <= n && desc[i] >= 2; i += desc[i]) {
        const uint8_t len = desc[i], type = desc[i + 1];
        if (i + len > n) break;
        if (type == 4 && len >= 9 && iface < 0 && desc[i + 3] == 0 &&   // interface, alternate setting 0
            desc[i + 5] == 0xFE && desc[i + 6] == 0x01) {                  // application-specific, DFU
            iface = desc[i + 2];
            iString = desc[i + 8];
        } else if (type == 0x21 && len >= 7 && xfer == 0) {             // DFU functional descriptor
            xfer = uint16_t(desc[i + 5] | desc[i + 6] << 8);
        }
    }
    if (iface < 0 || xfer == 0) { close(fd); error = "the device is not a DFU bootloader"; return nullptr; }

    unsigned int claim = unsigned(iface);
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &claim) < 0) {
        error = std::string("cannot claim the DFU interface (") + std::strerror(errno) + ")";
        close(fd);
        return nullptr;
    }

    // The interface string names the flash layout. String descriptors are UTF-16LE; the layout is ASCII.
    // A device just out of a reset, or one an interrupted job left busy, can fail the first request —
    // so a few tries, and the reason kept for the error if none works.
    std::string layout, why;
    for (int attempt = 0; iString && layout.empty() && attempt < 5; ++attempt) {
        if (attempt) usleep(200 * 1000);
        uint8_t s[255] = {};
        const int got = LinuxTransport::control(fd, 0x80, 6, uint16_t(0x0300 | iString), 0x0409, s, sizeof s);
        if (got < 0) { why = std::strerror(errno); continue; }
        for (int i = 2; i + 1 < got && i < s[0]; i += 2) layout += char(s[i]);
    }
    if (layout.empty()) {
        hung = true;
        unsigned int rel = unsigned(iface);
        ioctl(fd, USBDEVFS_RELEASEINTERFACE, &rel);
        close(fd);
        error = "the bootloader did not give its flash layout" + (why.empty() ? std::string() : " (" + why + ")");
        return nullptr;
    }
    return std::make_unique<LinuxTransport>(fd, iface, xfer, layout);
}

}  // namespace dfu

#endif

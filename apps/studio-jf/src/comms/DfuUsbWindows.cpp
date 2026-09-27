// The DFU transport on Windows: WinUSB, through SetupAPI — no libusb, the same as the Linux side's
// usbfs. DFU needs only control transfers and two facts from the descriptors, and winusb.dll gives both.
//
// THE DRIVER. Windows has no in-box driver for the STM32 bootloader (0483:df11), so WinUSB has to be
// bound to it once, which needs an administrator. The studio ships libwdi's wdi-simple beside studio.exe
// (third_party/libwdi-win) and installDriver() runs it elevated — the Windows counterpart of the Linux
// udev rule, asked for at the same point in an update and for the same reason: it must be in place
// before the ECU is put into its bootloader. wdi-simple pre-installs into the driver store when no
// device is plugged in, so Windows binds WinUSB the moment the bootloader appears.
//
// FINDING THE DEVICE. A WinUSB device is opened through its device-interface path, and the interface
// GUID is whatever the driver's .inf declared — libwdi generates one. So the GUID is read from the
// device's own registry key (DeviceInterfaceGUIDs) rather than assumed, which is what libusb does too.

#if defined(_WIN32)

#include "Dfu.h"

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace dfu {

namespace {

std::wstring upper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towupper(c)); });
    return s;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string lastError() {
    const DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring w = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L'.')) w.pop_back();
    return w.empty() ? "error " + std::to_string(e) : narrow(w);
}

// What the system knows about the bootloader right now.
struct Found {
    bool present = false;          // on the bus, whatever driver it has (or none)
    std::wstring service;          // its driver, e.g. "WinUSB"; empty when none is bound
    std::wstring instanceId;       // USB\VID_0483&PID_DF11\<serial>
    std::wstring interfacePath;    // what CreateFile opens; empty unless a driver exposes an interface
};

std::wstring interfacePathFor(const GUID& guid, const std::wstring& instanceId) {
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; path.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, &dev)) continue;
        wchar_t id[512] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &dev, id, 511, nullptr) && upper(id) == upper(instanceId))
            path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

Found find() {
    Found f;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return f;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t hw[2048] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<PBYTE>(hw), sizeof hw - 4, nullptr)) continue;
        // A REG_MULTI_SZ; the first entry is "USB\VID_0483&PID_DF11&REV_2200".
        if (upper(hw).find(L"VID_0483&PID_DF11") == std::wstring::npos) continue;
        f.present = true;
        wchar_t id[512] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &dev, id, 511, nullptr)) f.instanceId = id;
        wchar_t svc[256] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, nullptr,
                                              reinterpret_cast<PBYTE>(svc), sizeof svc - 2, nullptr))
            f.service = svc;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            wchar_t guids[1024] = {};
            DWORD size = sizeof guids - 4, type = 0;
            LSTATUS r = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(guids), &size);
            if (r != ERROR_SUCCESS) {
                size = sizeof guids - 4;
                r = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(guids), &size);
            }
            RegCloseKey(key);
            GUID g{};
            if (r == ERROR_SUCCESS && guids[0] && CLSIDFromString(guids, &g) == NOERROR)
                f.interfacePath = interfacePathFor(g, f.instanceId);
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return f;
}

class WindowsTransport : public Transport {
public:
    WindowsTransport(HANDLE file, WINUSB_INTERFACE_HANDLE usb, uint8_t iface, uint16_t xfer, std::string layout)
        : file_(file), usb_(usb), iface_(iface), xfer_(xfer), layout_(std::move(layout)) {}
    ~WindowsTransport() override {
        WinUsb_Free(usb_);
        CloseHandle(file_);
    }
    bool controlOut(uint8_t request, uint16_t value, const uint8_t* data, uint16_t len) override {
        return control(usb_, 0x21, request, value, iface_, const_cast<uint8_t*>(data), len) >= 0;
    }
    int controlIn(uint8_t request, uint16_t value, uint8_t* data, uint16_t len) override {
        return control(usb_, 0xA1, request, value, iface_, data, len);
    }
    std::string layoutString() const override { return layout_; }
    uint16_t transferSize() const override { return xfer_; }

    static int control(WINUSB_INTERFACE_HANDLE usb, uint8_t type, uint8_t request, uint16_t value,
                       uint16_t index, uint8_t* data, uint16_t len) {
        WINUSB_SETUP_PACKET p{};
        p.RequestType = type; p.Request = request; p.Value = value; p.Index = index; p.Length = len;
        ULONG done = 0;
        if (!WinUsb_ControlTransfer(usb, p, data, len, &done, nullptr)) return -1;
        return int(done);
    }

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    uint8_t iface_;
    uint16_t xfer_;
    std::string layout_;
};

// Is WinUSB already in the driver store for the bootloader, with no bootloader plugged in to ask?
// The store keeps each installed .inf as %WINDIR%\INF\oemNN.inf, readable by anyone; ours names the
// device by its hardware id and the WinUSB service.
bool driverInStore() {
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return false;
    const std::wstring dir = std::wstring(win) + L"\\INF\\";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"oem*.inf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        HANDLE f = CreateFileW((dir + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::string text(size_t(std::min<DWORD>(fd.nFileSizeLow, 256 * 1024)), '\0');
        DWORD got = 0;
        ReadFile(f, text.data(), DWORD(text.size()), &got, nullptr);
        CloseHandle(f);
        text.resize(got);
        // .inf files are UTF-16 or ANSI; drop the zero bytes so both read as ASCII.
        text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
        std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::toupper((unsigned char)c)); });
        found = text.find("VID_0483&PID_DF11") != std::string::npos && text.find("WINUSB") != std::string::npos;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

}  // namespace

bool devicePresent() { return find().present; }

bool driverInstalled() {
    const Found f = find();
    if (f.present) return upper(f.service) == L"WINUSB";
    return driverInStore();
}

bool installDriver(std::string& error) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring tool = dir + L"driver\\wdi-simple.exe";
    if (GetFileAttributesW(tool.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "the driver installer is missing (" + narrow(tool) + ") — reinstall the studio";
        return false;
    }
    // Where wdi-simple extracts the .inf and co-installers before installing them. A per-user folder,
    // so the elevated process and this one agree on it and nothing is left in the program folder.
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring dest = std::wstring(local[0] ? local : L".") + L"\\jayecu\\usb-driver";
    const std::wstring args =
        L"--vid 0x0483 --pid 0xDF11 --type 0 --name \"jayecu ECU bootloader\" --inf jayecu_dfu.inf "
        L"--manufacturer \"jayecu\" --dest \"" + dest + L"\" --progressbar --timeout 60000";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";                       // the administrator prompt: a driver needs one
    sei.lpFile = tool.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        error = GetLastError() == ERROR_CANCELLED ? "the administrator prompt was declined" : lastError();
        return false;
    }
    WaitForSingleObject(sei.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {                             // libwdi's error codes are negative
        error = "the driver installer failed (code " + std::to_string(int(code)) + ")";
        return false;
    }
    return true;
}

std::unique_ptr<Transport> openDevice(std::string& error) {
    Found f = find();
    if (!f.present) { error = "the ECU's bootloader is not on the USB bus"; return nullptr; }
    // Windows can take a moment after the device appears to bind its driver and publish the interface.
    for (int i = 0; i < 30 && f.interfacePath.empty(); ++i) { Sleep(100); f = find(); }
    if (f.interfacePath.empty()) {
        error = upper(f.service) == L"WINUSB" || f.service.empty()
                  ? "the USB driver for the ECU's bootloader is not installed"
                  : "the ECU's bootloader has another driver (" + narrow(f.service) +
                    "), not WinUSB — run Install the jayecu USB driver from the Start menu";
        return nullptr;
    }
    HANDLE file = CreateFileW(f.interfacePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "cannot open the ECU's bootloader (" + lastError() + ")"; return nullptr; }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        error = "cannot start WinUSB on the ECU's bootloader (" + lastError() + ")";
        CloseHandle(file);
        return nullptr;
    }
    ULONG timeout = 5000;
    WinUsb_SetPipePolicy(usb, 0, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);

    // The configuration descriptor, walked the same way as on Linux: the DFU interface (class 0xFE,
    // subclass 1, alternate setting 0) and its functional descriptor's wTransferSize.
    uint8_t desc[4096] = {};
    ULONG n = 0;
    WinUsb_GetDescriptor(usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, desc, sizeof desc, &n);
    int iface = -1, iString = 0;
    uint16_t xfer = 0;
    for (ULONG i = 0; n > 9 && i + 2 <= n && desc[i] >= 2; i += desc[i]) {
        const uint8_t len = desc[i], type = desc[i + 1];
        if (i + len > n) break;
        if (type == 4 && len >= 9 && iface < 0 && desc[i + 3] == 0 && desc[i + 5] == 0xFE && desc[i + 6] == 0x01) {
            iface = desc[i + 2];
            iString = desc[i + 8];
        } else if (type == 0x21 && len >= 7 && xfer == 0) {
            xfer = uint16_t(desc[i + 5] | desc[i + 6] << 8);
        }
    }
    if (iface < 0 || xfer == 0) {
        WinUsb_Free(usb); CloseHandle(file);
        error = "the device is not a DFU bootloader";
        return nullptr;
    }

    std::string layout;
    for (int attempt = 0; iString && layout.empty() && attempt < 5; ++attempt) {
        if (attempt) Sleep(200);
        uint8_t s[255] = {};
        ULONG got = 0;
        if (!WinUsb_GetDescriptor(usb, USB_STRING_DESCRIPTOR_TYPE, uint8_t(iString), 0x0409, s, sizeof s, &got)) continue;
        for (ULONG i = 2; i + 1 < got && i < s[0]; i += 2) layout += char(s[i]);
    }
    if (layout.empty()) {
        WinUsb_Free(usb); CloseHandle(file);
        error = "the bootloader did not give its flash layout — unplug the ECU's USB, plug it back in, "
                "and connect again";
        return nullptr;
    }
    return std::make_unique<WindowsTransport>(file, usb, uint8_t(iface), xfer, layout);
}

}  // namespace dfu

#endif

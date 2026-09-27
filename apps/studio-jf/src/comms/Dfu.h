#pragma once

// Dfu — write an ECU firmware image over USB DFU, into the STM32's own (ROM) bootloader.
//
// Ported from omnidyno's DfuManager, which does the same for that project's own bootloader. What
// changed for jayecu, and why:
//   * The image goes to 0x08000000, where the chip starts — jayecu has no bootloader of its own.
//   * The BLOCK SIZE is read from the device, never assumed. In ST's DfuSe protocol the chip places
//     block n at  address_pointer + (n - 2) * wTransferSize  — ITS transfer size, not the length of
//     what was sent. Send 1024-byte blocks to a device that declared 2048 and every other kilobyte
//     lands in the wrong place.
//   * Only the sectors the image covers are erased (read from the device's own layout string), so the
//     tune banks near the top of flash survive — and the tune is pulled into the studio beforehand
//     regardless.
//   * Every block is read back and compared before the device is told to leave DFU. A write the chip
//     acknowledged is not proof the flash holds it.
//   * No Qt: the protocol sits on a four-call USB transport (DfuTransport), so it runs the same on
//     Linux and Windows and is tested here against a simulated device.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dfu {

inline constexpr uint16_t kStmVid = 0x0483, kStmDfuPid = 0xDF11;
inline constexpr uint32_t kFlashBase = 0x08000000;

// The USB side, per platform. Control transfers go to the DFU interface; `interfaceNumber` and the
// two descriptor facts are read when the device is opened.
struct Transport {
    virtual ~Transport() = default;
    // Class request to the interface, host -> device. False on a USB error.
    virtual bool controlOut(uint8_t request, uint16_t value, const uint8_t* data, uint16_t len) = 0;
    // Class request to the interface, device -> host. Bytes received, or -1 on a USB error.
    virtual int controlIn(uint8_t request, uint16_t value, uint8_t* data, uint16_t len) = 0;
    // The DFU interface's string ("@Internal Flash  /0x08000000/04*032Kg,01*128Kg,07*256Kg").
    virtual std::string layoutString() const = 0;
    // wTransferSize from the DFU functional descriptor.
    virtual uint16_t transferSize() const = 0;
};

// Open the STM32 bootloader, if it is on the bus. nullptr + `error` when it is not, or cannot be opened.
std::unique_ptr<Transport> openDevice(std::string& error);
// Is it on the bus at all? (Cheap; does not open it.) On Windows: whatever driver it has, or none.
bool devicePresent();

#if defined(_WIN32)
// WINDOWS NEEDS A DRIVER BOUND TO THE BOOTLOADER, once — the counterpart of the Linux udev rule.
// Is WinUSB bound to it (or waiting in the driver store for it, when it is not plugged in)?
bool driverInstalled();
// Bind it: runs the wdi-simple beside studio.exe through the administrator prompt. Blocking — run it
// off the main thread. False + `error` when it was declined or failed.
bool installDriver(std::string& error);
#endif

// One erasable flash sector.
struct Sector { uint32_t address, size; };
// Parse the layout string into sectors. Empty when it cannot be read.
std::vector<Sector> parseLayout(const std::string& layout);

struct Progress {
    std::string stage;    // "Erasing", "Writing", "Verifying"
    int percent = 0;      // of the whole job
};

// Erase, write, verify and leave. Blocking; run it off the main thread. `cancel` is honoured between
// blocks until the write starts; after that an interrupted write would leave no firmware at all, so the
// job runs to the end. False + `error` on any failure — the device is then still in DFU and a retry
// can start again from scratch.
bool flash(Transport& dev, const std::vector<uint8_t>& image, uint32_t address,
           const std::function<void(const Progress&)>& progress, const std::atomic<bool>& cancel,
           std::string& error);

}  // namespace dfu

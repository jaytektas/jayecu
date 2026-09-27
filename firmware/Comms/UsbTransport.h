#pragma once

#include "ITransport.h"

namespace Comms {

// ---------------------------------------------------------------------------
// UsbTransport — thin wrapper over the STM32 USB CDC driver.
//
// On a real STM32 target, include usbd_cdc_if.h and replace the stub bodies.
// The extern "C" functions are defined by the CubeMX-generated USB middleware.
// ---------------------------------------------------------------------------

// Provided by usbd_cdc_if.c (or a stub for non-USB targets).
extern "C" size_t   UsbCdc_Read(uint8_t* buffer, size_t max_length);
extern "C" size_t   UsbCdc_ReadBlocking(uint8_t* buffer, size_t max_length, uint32_t timeout_ms);
extern "C" bool     UsbCdc_Send(const uint8_t* data, size_t length);
extern "C" bool     UsbCdc_IsConnected();

class UsbTransport : public ITransport {
public:
    bool send(const uint8_t* data, size_t length) override {
        return UsbCdc_Send(data, length);
    }

    size_t receive(uint8_t* buffer, size_t max_length) override {
        return UsbCdc_Read(buffer, max_length);
    }

    size_t receive_blocking(uint8_t* buffer, size_t max_length, uint32_t timeout_ms) override {
        return UsbCdc_ReadBlocking(buffer, max_length, timeout_ms);
    }

    bool is_available() const override {
        return UsbCdc_IsConnected();
    }
};

} // namespace Comms

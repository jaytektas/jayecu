#pragma once

#include <cstdint>
#include <cstddef>

namespace Comms {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool   send(const uint8_t* data, size_t length) = 0;
    virtual size_t receive(uint8_t* buffer, size_t max_length) = 0;
    // Block up to timeout_ms for the first bytes (event-driven wake), then return them. Default:
    // fall back to the non-blocking receive so transports that can't block still work.
    virtual size_t receive_blocking(uint8_t* buffer, size_t max_length, uint32_t /*timeout_ms*/) {
        return receive(buffer, max_length);
    }
    virtual bool   is_available() const = 0;
};

} // namespace Comms

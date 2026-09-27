#pragma once
#include "../../Storage/IConfigStore.h"
#include <cstring>

// In-memory IConfigStore stub for native unit tests.
template<uint32_t CAPACITY = 4096>
class RamConfigStore : public IConfigStore {
public:
    bool available() const override { return available_; }
    void set_available(bool v) { available_ = v; }

    bool read(uint8_t* out, uint32_t max_len, uint32_t* len_out) override {
        if (!available_ || len_ == 0) return false;
        const uint32_t n = len_ < max_len ? len_ : max_len;
        memcpy(out, buf_, n);
        if (len_out) *len_out = n;
        return true;
    }

    bool read_at(uint32_t offset, uint8_t* out, uint32_t max_len, uint32_t* len_out) override {
        if (!available_ || len_ == 0 || offset >= len_) return false;
        const uint32_t avail = len_ - offset;
        const uint32_t n = avail < max_len ? avail : max_len;
        memcpy(out, buf_ + offset, n);
        if (len_out) *len_out = n;
        return true;
    }

    bool write(const uint8_t* data, uint32_t len) override {
        if (!available_ || len > CAPACITY) return false;
        memcpy(buf_, data, len);
        len_ = len;
        return true;
    }

    bool write_record(const uint8_t* header, uint32_t header_len,
                      const uint8_t* payload, uint32_t payload_len) override {
        if (!available_ || header_len + payload_len > CAPACITY) return false;
        memcpy(buf_, header, header_len);
        memcpy(buf_ + header_len, payload, payload_len);
        len_ = header_len + payload_len;
        return true;
    }

    uint32_t stored_len() const { return len_; }

    void clear() { len_ = 0; }

private:
    uint8_t  buf_[CAPACITY] = {};
    uint32_t len_       = 0;
    bool     available_ = true;
};

#pragma once

// OmniFrame — the wire format itself, and nothing else.
//
// [AA 55][typeId][rsv][len:u16 LE][ts:u64][seq:u16][payload][crc16:2 LE], len = the TOTAL frame.
// See firmware/Comms/OmniProtocol.h, which is the other end of this.
//
// Split out because there are now two things that speak it: EcuLink, on the main thread, one
// request at a time between repaints; and ChunkReader, on a worker, driving a whole transfer at
// the link's pace. Two copies of a frame parser is two things to get subtly different — a CRC
// seeded differently, a resync that drops a byte more or fewer — and the failure would be a
// corrupt tune rather than a compile error. So there is one, and it is pure: no port, no state,
// no signals, which is also what makes it testable without an ECU.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace omni {

// Frame typeIds. See firmware/Comms/OmniProtocol.h.
enum : uint8_t {
    RSP_ACK = 0x00, RSP_TELEMETRY = 0x01, RSP_CONFIG = 0x02,
    RSP_BURN_ACK = 0x04, PACKET_IDENTITY = 0x07, RSP_FILE_DATA = 0x08,
};

inline uint16_t leU16(const unsigned char* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
inline void appendLE16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
inline void appendLE64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// CRC16-CCITT (poly 0x1021, init 0xFFFF) — matches the firmware's omni_crc16.
inline uint16_t crc16ccitt(const uint8_t* data, int length) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// A complete request frame for `cmd`, correlated by `seq` (the reply echoes it back).
inline std::vector<uint8_t> buildFrame(char cmd, const std::vector<uint8_t>& body, uint16_t seq) {
    const uint16_t total = static_cast<uint16_t>(16 + body.size() + 2);
    std::vector<uint8_t> frame;
    frame.reserve(total);
    frame.push_back(0xAA);
    frame.push_back(0x55);
    frame.push_back(static_cast<uint8_t>(cmd));   // typeId = command
    frame.push_back(0);                           // reserved
    appendLE16(frame, total);
    appendLE64(frame, 0);                         // timestamp (informational)
    appendLE16(frame, seq);                       // correlation id
    frame.insert(frame.end(), body.begin(), body.end());
    appendLE16(frame, crc16ccitt(frame.data(), static_cast<int>(frame.size())));
    return frame;
}

struct Frame {
    uint16_t             seq    = 0;   // echoed correlation id
    uint8_t              typeId = 0;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> raw;          // the complete frame, for the per-packet trace dump
};

// Consume ONE frame from the front of `rx`, resyncing past any garbage ahead of it.
//
// Returns nothing when there is not yet a whole frame — `rx` then keeps what it has and the caller
// reads more. Anything consumed is removed from `rx`, so the caller never has to track an offset.
//
// The CRC is checked BEFORE the frame is consumed, in place, so a corrupt length field cannot make
// us skip the wrong number of bytes. On a bad CRC only the two preamble bytes are dropped and the
// hunt resumes from the next byte, rather than trusting a length we have just proved unreliable.
inline std::optional<Frame> takeFrame(std::vector<uint8_t>& rx) {
    while (rx.size() >= 2) {
        const unsigned char* u = rx.data();
        if (u[0] != 0xAA || u[1] != 0x55) {                  // hunt for sync
            int next = -1;
            for (size_t i = 1; i < rx.size(); ++i)
                if (rx[i] == 0xAA) { next = static_cast<int>(i); break; }
            if (next < 0) {                                  // keep a trailing AA candidate
                if (rx.size() > 1) rx.erase(rx.begin(), rx.end() - 1);
                return std::nullopt;
            }
            rx.erase(rx.begin(), rx.begin() + next);
            continue;
        }
        if (rx.size() < 16) return std::nullopt;             // need the full header
        const uint16_t total = leU16(u + 4);
        // Upper bound = a full block frame + headroom (block 4096 + 16 header + 6 sub-hdr + 2 CRC).
        if (total < 18 || total > 8192) { rx.erase(rx.begin(), rx.begin() + 2); continue; }
        if (rx.size() < total) return std::nullopt;          // wait for the rest

        const uint16_t want = leU16(rx.data() + total - 2);
        if (crc16ccitt(rx.data(), total - 2) != want) {
            rx.erase(rx.begin(), rx.begin() + 2);            // corrupt → resync from the next byte
            continue;
        }
        Frame f;
        f.typeId  = rx[2];
        f.seq     = leU16(rx.data() + 14);
        f.payload.assign(rx.begin() + 16, rx.begin() + (total - 2));
        f.raw.assign(rx.begin(), rx.begin() + total);
        rx.erase(rx.begin(), rx.begin() + total);
        return f;
    }
    return std::nullopt;
}

}  // namespace omni

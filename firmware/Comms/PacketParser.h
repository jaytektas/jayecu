#pragma once

#include "OmniProtocol.h"
#include <cstdint>
#include <cstddef>

namespace Comms {

// Fired when a complete, CRC-valid omnidyno frame arrives.
//   stream_idx — which transport slot
//   type_id    — the frame's typeId (OMNI_CMD_* request / OMNI_RSP_* response)
//   sequence   — the frame's correlation id; a reply MUST echo the request's sequence so the host
//                can match the response to its pending request (no host-side flag guessing).
//   payload    — payload bytes (after the 16-byte header, before the CRC)
//   length     — payload byte count
typedef void (*PacketCallback)(void* ctx, size_t stream_idx, uint8_t type_id, uint16_t sequence,
                               const uint8_t* payload, uint16_t length);

// Sync-framed parser: hunts for AA 55, reads the 16-byte header (learns total length), accumulates the
// frame, validates the CRC16-CCITT trailer, then dispatches. Resyncs on any bad sync/length/CRC.
class PacketParser {
public:
    PacketParser() = default;

    void init(size_t stream_idx, PacketCallback callback, void* ctx);
    void reset();
    void update(uint32_t delta_ms);   // no timeout state needed; kept for caller API compatibility
    void parse_byte(uint8_t byte);

    // Silently-dropped-frame tally. parse_byte() may run inline-in-ISR, where the logging facility
    // is best called from task context, so the drop paths only COUNT here; a task-context consumer
    // (CommsManager::update) edge-logs new drops to the console. last_len = the most recent bad
    // frame_len_, which makes an oversized-frame drop (host failed to chunk a write) diagnosable.
    struct RejectStats { uint16_t oversize = 0; uint16_t crc = 0; uint16_t last_len = 0; };
    RejectStats reject_stats() const { return rej_; }

private:
    size_t         stream_idx_ = 0;
    PacketCallback callback_    = nullptr;
    void*          ctx_         = nullptr;
    uint16_t       idx_         = 0;   // bytes accumulated into buf_
    uint16_t       frame_len_   = 0;   // total frame length, known once the header is in (0 = unknown)
    RejectStats    rej_{};             // dropped-frame counters, surfaced via reject_stats()
    uint8_t        buf_[OMNI_MAX_FRAME];
};

} // namespace Comms

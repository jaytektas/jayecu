#include "PacketParser.h"

namespace Comms {

void PacketParser::init(size_t stream_idx, PacketCallback callback, void* ctx) {
    stream_idx_ = stream_idx;
    callback_   = callback;
    ctx_        = ctx;
    reset();
}

void PacketParser::reset() {
    idx_       = 0;
    frame_len_ = 0;
}

void PacketParser::update(uint32_t /*delta_ms*/) {
    // Sync-framed: a stalled partial frame is harmless — the next AA 55 resyncs. No timeout needed.
}

void PacketParser::parse_byte(uint8_t byte) {
    // Sync lock: byte 0 = AA, byte 1 = 55; anything else hunts for a fresh sync.
    if (idx_ == 0) {
        if (byte == OMNI_SYNC1) buf_[idx_++] = byte;
        return;
    }
    if (idx_ == 1) {
        if (byte == OMNI_SYNC2) {
            buf_[idx_++] = byte;
        } else {
            idx_ = 0;
            if (byte == OMNI_SYNC1) buf_[idx_++] = byte;   // this byte may itself start a frame
        }
        return;
    }

    buf_[idx_++] = byte;

    // Header complete → learn the total frame length.
    if (idx_ == OMNI_HEADER_SIZE) {
        const OmniPacketHeader* h = reinterpret_cast<const OmniPacketHeader*>(buf_);
        frame_len_ = h->length;
        if (frame_len_ < OMNI_MIN_FRAME || frame_len_ > OMNI_MAX_FRAME) {
            rej_.oversize++; rej_.last_len = frame_len_;   // host sent a frame the buffer can't hold (e.g. an unchunked config write)
            reset();
            return;
        }
    }

    // Full frame in → validate CRC16 over [0 .. frame_len_-2] and dispatch.
    if (frame_len_ != 0 && idx_ >= frame_len_) {
        const uint16_t rx_crc = static_cast<uint16_t>(buf_[frame_len_ - 2] |
                                                      (buf_[frame_len_ - 1] << 8));   // LE trailer
        const bool crc_ok = (rx_crc == omni_crc16(buf_, static_cast<uint16_t>(frame_len_ - OMNI_CRC_SIZE)));
        if (crc_ok && callback_) {
            const OmniPacketHeader* h = reinterpret_cast<const OmniPacketHeader*>(buf_);
            const uint16_t plen = static_cast<uint16_t>(frame_len_ - OMNI_HEADER_SIZE - OMNI_CRC_SIZE);
            callback_(ctx_, stream_idx_, h->type_id, h->sequence, buf_ + OMNI_HEADER_SIZE, plen);
        } else if (!crc_ok) {
            rej_.crc++; rej_.last_len = frame_len_;        // corruption, or a truncated u16 length on an over-large host frame
        }
        reset();
        return;
    }

    if (idx_ >= OMNI_MAX_FRAME) reset();   // overflow guard
}

} // namespace Comms

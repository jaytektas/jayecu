#pragma once
#include "CanFrame.h"
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// IsoTpTx — minimal ISO 15765-2 sender for OBD-II multi-frame responses.
//
// Handles a single active transfer (OBD Mode 09 is functionally addressed — the one tester):
// emit a First Frame, wait for the tester's Flow Control, then pace Consecutive Frames by the
// negotiated block size + separation time (STmin). Pure and allocation-free; poll() emits one
// frame per due tick so it composes with the CanBroker's ~1 ms update loop.
//
// Framing (per ISO 15765-2):
//   First Frame (FF):       [0x10 | (len>>8)] [len & 0xFF] + 6 data bytes
//   Flow Control (FC, RX):  [0x30|FS] [BlockSize] [STmin]      (FS: 0=CTS, 1=WAIT, 2=OVFL)
//   Consecutive Frame (CF): [0x20 | seq(1..15 wrap 0)] + up to 7 data bytes
// ---------------------------------------------------------------------------
class IsoTpTx {
public:
    static constexpr uint8_t  CF_MAX = 7;    // data bytes per consecutive frame
    static constexpr uint16_t CAP    = 64;   // max multi-frame payload (Mode 09 needs <=23)

    [[nodiscard]] bool busy() const { return state_ != IDLE; }

    // Begin a multi-frame transfer (assumes len > 7 — the caller sends short payloads single-frame).
    void start(uint32_t resp_id, const uint8_t* data, uint16_t len) {
        resp_id_  = resp_id;
        len_      = (len > CAP) ? CAP : len;
        memcpy(buf_, data, len_);
        sent_ = 0; seq_ = 1; bs_ = 0; bs_left_ = 0; stmin_ms_ = 0; next_ms_ = 0; first_ = false;
        state_    = SEND_FF;
    }

    // Feed a received Flow Control frame (tester -> ECU physical request id).
    void on_flow_control(const CanFrame& f) {
        if (state_ != WAIT_FC) return;              // no transfer awaiting FC -> ignore stray FC
        if ((f.data[0] & 0xF0) != 0x30) return;     // not an FC PCI
        const uint8_t fs = f.data[0] & 0x0F;
        if (fs == 0x02) { state_ = IDLE;    return; }   // overflow/abort -> give up
        if (fs == 0x01) { wait_since_ms_ = 0; return; }   // WAIT: stay, and restart the N_Bs timer
        // fs == 0x00 (Clear To Send)
        bs_       = f.data[1];
        bs_left_  = bs_;                              // 0 = send the whole remaining message
        const uint8_t st = f.data[2];
        stmin_ms_ = (st <= 0x7F) ? st : 1;           // ms; 0xF1-0xF9 = 100-900 us -> 1 ms
        state_    = SENDING;
        first_    = true;                            // first CF of this block is due immediately
    }

    // Drive the transfer. If a frame is due at now_ms, fill `out` and return true.
    bool poll(uint32_t now_ms, CanFrame& out) {
        // N_Bs (ISO 15765-2): a sender waits at most 1 s for flow control, then gives the transfer up.
        // Without it, one lost FC frame left busy() true until reboot — and every multi-frame reply
        // after it (all of Mode 09) was refused for the rest of the power cycle.
        if (state_ == WAIT_FC) {
            if (wait_since_ms_ == 0) wait_since_ms_ = now_ms ? now_ms : 1u;
            else if (static_cast<uint32_t>(now_ms - wait_since_ms_) > N_BS_MS) { state_ = IDLE; return false; }
            return false;
        }
        if (state_ == SEND_FF) {
            out = mk();
            out.data[0] = 0x10 | ((len_ >> 8) & 0x0F);
            out.data[1] = static_cast<uint8_t>(len_ & 0xFF);
            for (uint8_t i = 0; i < 6; i++) out.data[2 + i] = (i < len_) ? buf_[i] : 0x00;
            sent_  = 6;
            seq_   = 1;
            state_ = WAIT_FC;
            wait_since_ms_ = 0;                      // the N_Bs clock starts at the next poll
            return true;
        }
        if (state_ == SENDING) {
            if (!first_ && static_cast<int32_t>(now_ms - next_ms_) < 0)
                return false;                        // STmin not yet elapsed
            first_ = false;
            out = mk();
            out.data[0] = 0x20 | (seq_ & 0x0F);
            uint8_t n = 0;
            for (; n < CF_MAX && sent_ < len_; n++) out.data[1 + n] = buf_[sent_++];
            for (uint8_t i = 1 + n; i < 8; i++)     out.data[i] = 0x00;   // pad tail
            seq_ = (seq_ == 0x0F) ? 0 : static_cast<uint8_t>(seq_ + 1);
            if (sent_ >= len_)               { state_ = IDLE;    return true; }  // done
            if (bs_ != 0 && --bs_left_ == 0) { state_ = WAIT_FC; wait_since_ms_ = 0; return true; }  // block done -> await FC
            next_ms_ = now_ms + stmin_ms_;           // STmin 0 -> next CF due at this same tick
            return true;
        }
        return false;
    }

private:
    enum State : uint8_t { IDLE, SEND_FF, WAIT_FC, SENDING };
    CanFrame mk() const { CanFrame r{}; r.id = resp_id_; r.ext = false; r.dlc = 8; return r; }

    uint8_t  buf_[CAP] = {};
    uint16_t len_ = 0, sent_ = 0;
    uint8_t  seq_ = 1, bs_ = 0, bs_left_ = 0, stmin_ms_ = 0;
    uint32_t resp_id_ = 0x7E8, next_ms_ = 0;
    uint32_t wait_since_ms_ = 0;           // when WAIT_FC began (0 = starting now)
    static constexpr uint32_t N_BS_MS = 1000;
    bool     first_ = false;               // next CF is the first of a (re)authorised block
    State    state_ = IDLE;
};

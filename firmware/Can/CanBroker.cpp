#include "CanBroker.h"
#include "ObdResponder.h"
#include "../Engine/TelemScale.h"   // telem_round<T>, used by the pack fragment
#include "../Diagnostics/DtcManager.h"   // P1655 — the broadcast bit-rate check says so
#include "signal_ids.h"

// A bus that is transmitting and getting no acknowledgement. One code per bus, so the fault names
// the wire rather than leaving you to work out which of the two it is.
static constexpr uint16_t P_CAN_NO_ACK = 0x1657;   // …and 0x1658 for the second bus
#include <cstring>

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool CanBroker::add_bus(uint8_t idx, ICanChannel* ch) {
    if (idx >= MAX_BUSES) return false;
    buses_[idx] = ch;
    return true;
}

bool CanBroker::add_rx(uint8_t bus, uint32_t id, bool ext, RxCallback cb, void* ctx, uint32_t mask) {
    if (rx_count_ >= MAX_RX_SUBS) return false;
    rx_subs_[rx_count_++] = { bus, id, ext, cb, ctx, mask };
    return true;
}

void CanBroker::clear_rx(void* ctx) {
    uint8_t w = 0;
    for (uint8_t r = 0; r < rx_count_; r++) {
        if (rx_subs_[r].ctx != ctx) rx_subs_[w++] = rx_subs_[r];
    }
    rx_count_ = w;
}

void CanBroker::enable_obd(uint8_t bus) {
    obd_enabled_ = true;
    obd_bus_     = bus;
    // Registers RX for 0x7DF (functional) + 0x7E0 (physical / multi-frame Flow Control).
    obd_responder_.begin(this);
}

// ---------------------------------------------------------------------------
// Telemetry — packed from the SignalBus on demand
//
// The cache used to be pushed in by a post_telemetry() that nothing ever called, so every OBD Mode 01
// PID answered out of a zeroed struct: 0 rpm, -40 C coolant, 0 kPa. Same pack fragment the serial
// telemetry frame uses, so the two can never disagree about a channel's scale.
// ---------------------------------------------------------------------------

void CanBroker::refresh_telemetry() {
    if (!signal_bus_) return;
    SignalBus& bus  = *signal_bus_;
    EcuTelemetry& t = telem_cache_;
    #include "../../generated/sensor_telem_pack.inc"   // one read per channel, scaled per type
}

bool CanBroker::send_frame(uint8_t bus, const CanFrame& frame) {
    if (bus >= MAX_BUSES || !buses_[bus]) return false;
    const bool ok = buses_[bus]->send(frame);
    // Count what actually went to the driver, and separately what it refused. A bus whose load reads
    // low while refusals climb is a jammed mailbox, not a quiet wire — and those look identical if you
    // only count successes.
    if (ok) { stats_[bus].tx_frames++; stats_[bus].tx_bits += frame_bits(frame); }
    else    { stats_[bus].tx_fail++; }
    return ok;
}

// ---------------------------------------------------------------------------
// Per-bus health — load, throughput, and what the controller says about the wire.
// ---------------------------------------------------------------------------

void CanBroker::publish_stats(SignalBus& bus, uint32_t now_ms) {
    if (stats_window_ms_ == 0) { stats_window_ms_ = now_ms; return; }   // first call: start the window
    const uint32_t elapsed = now_ms - stats_window_ms_;
    if (elapsed < 1000u) return;
    stats_window_ms_ = now_ms;

    static constexpr SignalId LOAD[MAX_BUSES] = { SIG_CAN1_LOAD_PCT, SIG_CAN2_LOAD_PCT };
    static constexpr SignalId TXF [MAX_BUSES] = { SIG_CAN1_TX_FPS,   SIG_CAN2_TX_FPS   };
    static constexpr SignalId RXF [MAX_BUSES] = { SIG_CAN1_RX_FPS,   SIG_CAN2_RX_FPS   };
    static constexpr SignalId TEC [MAX_BUSES] = { SIG_CAN1_TEC,      SIG_CAN2_TEC      };
    static constexpr SignalId REC [MAX_BUSES] = { SIG_CAN1_REC,      SIG_CAN2_REC      };
    static constexpr SignalId STAT[MAX_BUSES] = { SIG_CAN1_STATE,    SIG_CAN2_STATE    };
    static constexpr SignalId LERR[MAX_BUSES] = { SIG_CAN1_LAST_ERR, SIG_CAN2_LAST_ERR };
    static constexpr SignalId BOFF[MAX_BUSES] = { SIG_CAN1_BUS_OFF,  SIG_CAN2_BUS_OFF  };
    static constexpr SignalId FAIL[MAX_BUSES] = { SIG_CAN1_TX_FAIL,  SIG_CAN2_TX_FAIL  };

    for (uint8_t b = 0; b < MAX_BUSES; b++) {
        BusStats& st = stats_[b];
        // A bus that is DOWN publishes nothing at all rather than a confident zero: "switched off" and
        // "up and silent" are different answers, and the channel expiring says the first one.
        if (!buses_[b] || !buses_[b]->is_up()) {
            st = BusStats{ 0, 0, 0, 0, st.tx_fail };      // keep the cumulative refusals
            continue;
        }
        const uint32_t bits = st.tx_bits + st.rx_bits;
        const uint32_t rate = bitrate_[b] ? bitrate_[b] : 500000u;
        const float    load = (float)bits * 1000.0f / (float)elapsed / (float)rate * 100.0f;
        const float    secs = (float)elapsed / 1000.0f;

        bus.set(LOAD[b], load,                              true, now_ms, 3000u);
        bus.set(TXF[b],  (float)st.tx_frames / secs,        true, now_ms, 3000u);
        bus.set(RXF[b],  (float)st.rx_frames / secs,        true, now_ms, 3000u);
        bus.set(FAIL[b], (float)st.tx_fail,                 true, now_ms, 3000u);

        const CanErrorStatus e = buses_[b]->error_status();
        bus.set(TEC[b],  (float)e.tec,       true, now_ms, 3000u);
        bus.set(REC[b],  (float)e.rec,       true, now_ms, 3000u);
        bus.set(STAT[b], (float)e.state,     true, now_ms, 3000u);
        bus.set(LERR[b], (float)e.last_err,  true, now_ms, 3000u);
        bus.set(BOFF[b], (float)e.bus_off_n, true, now_ms, 3000u);

        // A BUS THAT CANNOT COMPLETE A FRAME. AutoRetransmission holds an unfinished frame in its
        // mailbox and retries for ever, so all three fill, send_frame starts refusing, and the LOAD
        // reads ZERO because nothing ever completes to be counted. Every number needed to see that
        // was already published and none of them said it.
        //
        // WHICH ERROR IT IS matters and is the half worth reading: a STUFF or FORM error means the
        // bit rate disagrees (the other end is sampling a different wire than the one being driven),
        // while an ACK error means there is nobody else out there at all. Measured on the bench, a
        // 500k peer against a 1 Mbit ECU gives last_err 1 — stuff — and state 2, not the ACK error
        // the first version of this looked for. The channel names both now; this raises on the
        // outcome they share.
        //
        // Listen-only is exempt: it never transmits, so it can neither fail to complete nor be refused.
        const bool offering = st.tx_fail > tx_fail_seen_[b] || st.tx_frames > 0;
        const bool mute     = (e.state >= 2) || (e.last_err == 1 || e.last_err == 2 || e.last_err == 3);
        if (dtc_ && !listen_only_[b]) {
            if (offering && mute && st.tx_frames == 0)
                dtc_->raise(P_CAN_NO_ACK + b, DtcSource::CONFIG, DTC_SEV_LEVEL1, now_ms, DTC_TTL_DEFAULT);
            else if (st.tx_frames > 0)
                dtc_->heal(P_CAN_NO_ACK + b);          // something completed: somebody is there
        }
        tx_fail_seen_[b] = st.tx_fail;

        st.tx_frames = st.rx_frames = st.tx_bits = st.rx_bits = 0;   // tx_fail is cumulative
    }
}

// ---------------------------------------------------------------------------
// Main update loop — call from CanTask at ~1ms cadence
// ---------------------------------------------------------------------------

void CanBroker::reconfigure_obd(bool enabled, uint8_t bus) {
    if (enabled == obd_enabled_ && (!enabled || bus == obd_bus_)) return;   // nothing moved
    clear_rx(&obd_responder_);          // drop the subscriptions held for the OLD bus
    obd_enabled_ = false;
    if (enabled) enable_obd(bus);       // re-registers on the new one
}

void CanBroker::reconfigure_generic(const CanConfig& cfg, uint32_t now_ms) {
    generic_.set_broker(this);
    generic_.set_dtc(dtc_);
    generic_.set_sensors(sensors_);
    generic_.configure(cfg, now_ms);
}

void CanBroker::update(uint32_t tick_ms) {
    process_rx(tick_ms);
    if (obd_enabled_) obd_responder_.tick(tick_ms);   // pump any in-flight multi-frame OBD response
    // Broadcast AFTER the responder: a scan tool waiting on a Flow Control is answering a question
    // somebody asked, and should not queue behind forty frames nobody is waiting for.
    generic_.tick(tick_ms, signal_bus_);
    if (signal_bus_) publish_stats(*signal_bus_, tick_ms);
}

void CanBroker::process_rx(uint32_t tick_ms) {
    for (uint8_t b = 0; b < MAX_BUSES; b++) {
        if (!buses_[b]) continue;
        CanFrame frame;
        while (buses_[b]->receive(frame)) {
            stats_[b].rx_frames++;
            stats_[b].rx_bits += frame_bits(frame);
            // ONE decode. It writes the signal bus for fields that name a channel and leaves the
            // value for a CAN-interface sensor to acquire for those that do not.
            generic_.on_rx(b, frame, tick_ms, signal_bus_);
            for (uint8_t s = 0; s < rx_count_; s++) {
                auto& sub = rx_subs_[s];
                if ((sub.bus == ANY_BUS || sub.bus == b)
                    && ((frame.id & sub.mask) == (sub.id & sub.mask))
                    && sub.ext == frame.ext) {
                    sub.cb(sub.ctx, b, frame, tick_ms);
                }
            }
        }
    }
}


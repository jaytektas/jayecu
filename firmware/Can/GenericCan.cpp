#include "GenericCan.h"
#include "CanBroker.h"
#include "../Signal/SignalBus.h"
#include "../Diagnostics/DtcManager.h"
#include "../Sensors/Sensors.h"
#include "signal_ids.h"

using namespace canmsg;

// A frame the tune describes but the ECU cannot run: a transmit frame on a listen-only bus, or a
// pool index that points outside the pool.
static constexpr uint16_t P_CAN_GENERIC_CONFIG = 0x1655;
// Two producers claim one channel. A write is refused only by a strictly HIGHER priority, so a
// receive field and a sensor at the same number simply alternate — the value on the bus is then
// whichever wrote last, which is a scheduling artefact rather than a measurement.
static constexpr uint16_t P_CAN_SIGNAL_CONFLICT = 0x1656;

// ---------------------------------------------------------------------------
// Configure — one pass over the pool, building a schedule and a sorted receive index.
// ---------------------------------------------------------------------------

void GenericCan::configure(const CanConfig& cfg, uint32_t now_ms) {
    cfg_  = &cfg;
    tx_n_ = 0;
    rx_n_ = 0;
    // The received values are indexed by FIELD SLOT, and a repack moves fields between slots — so a value
    // left from before could be read, fresh-stamped, as a different field's. A changed CAN configuration
    // starts every field with nothing received (the task only calls this when the CAN section changed).
    for (FieldValue& v : fv_) v = FieldValue{};

    bool bad_cfg = false;

    for (uint16_t i = 0; i < CAN_GC_FRAME_COUNT; i++) {
        const GcFrameConfig& f = cfg.gc_frame[i];
        if (!(f.flags & FRAME_USED)) continue;                   // free slot
        const uint8_t bus = static_cast<uint8_t>(f.bus & 1u);
        // TWO GATES, NOT THREE. The bus has an enable and the FRAME has an enable; a third
        // "generic CAN on this bus" in between said nothing the frame flags did not already say,
        // and every extra switch is another state the pages have to gate on and get wrong.
        if (!cfg.bus[bus].enabled) continue;
        if (f.flags & FRAME_OFF) continue;              // parked: kept in the tune, not run

        uint16_t first = 0, count = 0;
        if (f.first_field >= CAN_GC_FIELD_COUNT ||
            static_cast<uint32_t>(f.first_field) + f.field_count > CAN_GC_FIELD_COUNT) {
            bad_cfg = true;                                      // says so below, then runs what it can
        }
        if (!field_run(f, first, count)) continue;          // a frame with no fields sends nothing

        if (f.flags & FRAME_TX) {
            // A transmit frame on a listen-only bus can never go out. That is a wiring/tune mismatch
            // worth a code rather than a silent nothing: the frames are configured, the bus is up, and
            // the wire stays empty.
            if (cfg.bus[bus].listen_only) { bad_cfg = true; continue; }
            if (f.period_ms == 0) continue;                      // 0 = not scheduled
            if (tx_n_ >= MAX_FRAMES) { bad_cfg = true; break; }
            TxEntry& e = tx_[tx_n_];
            e.frame = i;
            // Spread within this frame's own period — see the note in the header.
            e.next_due_ms = now_ms + (tx_n_ % f.period_ms);
            for (uint8_t k = 0; k < 8; k++) e.data[k] = 0;
            tx_n_++;
        } else {
            if (rx_n_ >= MAX_FRAMES) { bad_cfg = true; break; }
            rx_[rx_n_].key   = frame_key(bus, (f.flags & FRAME_EXT) != 0, f.id);
            rx_[rx_n_].frame = i;
            rx_n_++;
        }
    }

    // Sort the receive index by key. Insertion sort: it runs once per tune edit over at most a few
    // dozen entries, and the alternative is a heap or a recursion for no measurable gain.
    for (uint16_t i = 1; i < rx_n_; i++) {
        const RxEntry k = rx_[i];
        int32_t j = static_cast<int32_t>(i) - 1;
        while (j >= 0 && rx_[j].key > k.key) { rx_[j + 1] = rx_[j]; j--; }
        rx_[j + 1] = k;
    }

    // WHO ELSE WRITES THESE CHANNELS. Checked once per tune edit, over the receive fields only —
    // a transmit field reads the bus and cannot conflict with anything.
    bool clash = false;
    for (uint16_t a = 0; a < rx_n_ && !clash; a++) {
        const GcFrameConfig& fa = cfg.gc_frame[rx_[a].frame];
        uint16_t first = 0, count = 0;
        if (!field_run(fa, first, count)) continue;
        for (uint16_t n = 0; n < count && !clash; n++) {
            const uint16_t sig = cfg.gc_field[first + n].sig;
            if (sig == SIG_NONE || sig >= SIG_COUNT) continue;
            // A sensor already publishing it — the case that matters, because a sensor publishes even
            // when INVALID and would mark a good CAN value invalid at equal priority.
            if (sensors_ && sensors_->produces(static_cast<SignalId>(sig))) { clash = true; break; }
            // …and two receive fields claiming it, which is the same fault inside one subsystem.
            for (uint16_t b = 0; b < rx_n_ && !clash; b++) {
                if (b == a) continue;
                const GcFrameConfig& fb = cfg.gc_frame[rx_[b].frame];
                uint16_t bf = 0, bc = 0;
                if (!field_run(fb, bf, bc)) continue;
                for (uint16_t m = 0; m < bc; m++)
                    if (cfg.gc_field[bf + m].sig == sig) { clash = true; break; }
            }
        }
    }

    // Remembered, and ASSERTED FROM tick(). Raised here once with now = 0 and a lifetime, these were aged
    // out of ACTIVE on the next table sweep after the first few seconds of uptime — a configuration fault
    // that only ever showed as stored. A judge that stops asserting cannot leave a fault standing, so the
    // judge keeps asserting, with the real time, for as long as the configuration is wrong.
    bad_cfg_ = bad_cfg;
    clash_   = clash;
    assert_config_dtcs(now_ms);
}

void GenericCan::assert_config_dtcs(uint32_t now_ms) {
    if (!dtc_) return;
    if (bad_cfg_) dtc_->raise(P_CAN_GENERIC_CONFIG, DtcSource::CONFIG, DTC_SEV_LEVEL1, now_ms, DTC_TTL_DEFAULT);
    else          dtc_->heal(P_CAN_GENERIC_CONFIG);
    if (clash_)   dtc_->raise(P_CAN_SIGNAL_CONFLICT, DtcSource::CONFIG, DTC_SEV_LEVEL1, now_ms, DTC_TTL_DEFAULT);
    else          dtc_->heal(P_CAN_SIGNAL_CONFLICT);
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

bool GenericCan::build(const GcFrameConfig& f, SignalBus& signals, uint8_t* out) const {
    uint16_t first = 0, count = 0;
    if (!field_run(f, first, count)) return false;
    const uint8_t dlc = (f.dlc > 8) ? 8 : f.dlc;

    for (uint16_t n = 0; n < count; n++) {
        const GcFieldConfig& fd = cfg_->gc_field[first + n];
        if (fd.width == 0 || fd.width > 32) continue;            // not a field
        const bool little = (fd.flags & FIELD_LITTLE) != 0;

        const bool has_value = fd.sig != SIG_NONE && fd.sig < SIG_COUNT
                            && signals.valid(static_cast<SignalId>(fd.sig));
        if (has_value) {
            const float v = signals.get(static_cast<SignalId>(fd.sig));
            pack_bits(out, dlc, fd.bit_off, fd.width, little, encode_value(fd, v));
            continue;
        }
        switch (fd.policy) {
            case ABSENT_HOLD: break;                             // leave the previous bits
            case ABSENT_SKIP: return false;                      // suppress the whole frame
            case ABSENT_ZERO:
            default:
                // RAW zero, not the encoded zero. Through a transform like raw = value*10 + 1013 the
                // encoded zero is 1013, which reads back as a plausible 0 kPa; raw zero reads as
                // -101.3 kPa, which is visibly not a measurement. When a field cannot be filled, the
                // reading that is obviously broken is the more useful of the two.
                pack_bits(out, dlc, fd.bit_off, fd.width, little, 0u);
                break;
        }
    }
    return true;
}

void GenericCan::tick(uint32_t now_ms, SignalBus* signals) {
    assert_config_dtcs(now_ms);
    if (!cfg_ || !broker_ || !signals || tx_n_ == 0) return;

    uint8_t fired = 0;
    for (uint16_t i = 0; i < tx_n_ && fired < MAX_PER_TICK; i++) {
        TxEntry& e = tx_[i];
        if (static_cast<int32_t>(now_ms - e.next_due_ms) < 0) continue;

        const GcFrameConfig& f = cfg_->gc_frame[e.frame];
        const uint16_t period = f.period_ms ? f.period_ms : 1000u;

        // A frame that has fallen more than one period behind resyncs to now rather than trying to
        // catch up: a burst of stale frames is worse than a gap, because every one of them is a
        // measurement that has already been superseded.
        e.next_due_ms = (static_cast<uint32_t>(now_ms - e.next_due_ms) > period)
                      ? now_ms + period
                      : e.next_due_ms + period;

        if (!build(f, *signals, e.data)) continue;               // ABSENT_SKIP: nothing this cycle

        CanFrame fr{};
        fr.id  = f.id;
        fr.ext = (f.flags & FRAME_EXT) != 0;
        fr.rtr = false;
        fr.dlc = (f.dlc > 8) ? 8 : f.dlc;
        for (uint8_t k = 0; k < fr.dlc; k++) fr.data[k] = e.data[k];

        // A frame the driver refuses is NOT dropped silently and NOT retried forever: its deadline has
        // already moved, so the next tick carries the next measurement rather than this stale one.
        if (broker_->send_frame(f.bus & 1u, fr)) { sent_++; fired++; }
        else                                      { refused_++; }
    }
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void GenericCan::on_rx(uint8_t bus, const CanFrame& fr, uint32_t now_ms, SignalBus* signals) {
    if (!cfg_ || !signals || rx_n_ == 0) return;

    // Most frames on a shared bus are not ours, so the cost of NOT being interested has to be small.
    const uint32_t key = frame_key(bus, fr.ext, fr.id);
    uint16_t lo = 0, hi = rx_n_;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>((lo + hi) >> 1);
        if (rx_[mid].key < key) lo = static_cast<uint16_t>(mid + 1);
        else                    hi = mid;
    }
    if (lo >= rx_n_ || rx_[lo].key != key) return;

    // Two frames may share an id — the same bytes decoded into different signals is a legitimate way
    // to split a crowded frame across tunable groups — so walk the whole run of equal keys.
    for (uint16_t i = lo; i < rx_n_ && rx_[i].key == key; i++) {
        const GcFrameConfig& f = cfg_->gc_frame[rx_[i].frame];
        uint16_t first = 0, count = 0;
        if (!field_run(f, first, count)) continue;

        for (uint16_t n = 0; n < count; n++) {
            const uint16_t fi = static_cast<uint16_t>(first + n);
            const GcFieldConfig& fd = cfg_->gc_field[fi];
            if (fd.width == 0 || fd.width > 32) continue;

            const uint8_t dlc = fr.dlc > 8u ? 8u : fr.dlc;
            const bool little = (fd.flags & FIELD_LITTLE) != 0;
            // A field the frame did not carry is not decoded — not stamped, so it goes stale on its
            // Valid For time exactly like an absent sensor, instead of reading its missing bits as 0.
            if (!field_fits(dlc, fd.bit_off, fd.width, little)) continue;
            const uint32_t raw = unpack_bits(fr.data, dlc, fd.bit_off, fd.width, little,
                                             (fd.flags & FIELD_SIGNED) != 0);
            // The sender saying it has nothing: skip the field entirely rather than decode the code.
            // Not stamping it is what makes this "no reading" — the value stops being refreshed and
            // expires on its Valid For time, which is what an absent sensor should look like.
            // Compared on the FIELD'S OWN BITS: a signed field is sign-extended above, so 0x8000 came
            // back as 0xFFFF8000 and never matched — and -32768 x scale went out as a real reading.
            const uint32_t fmask = (fd.width >= 32) ? 0xFFFFFFFFu : ((1u << fd.width) - 1u);
            if ((fd.flags & FIELD_SENTINEL) && (raw & fmask) == (fd.sentinel & fmask)) continue;

            const float v = decode_value(fd, raw);

            // Decoded once, offered twice. The value is kept for a sensor to acquire whether or not
            // it also goes on the bus — a sensor that names this field wants the reading, not the
            // channel. now_ms is stamped last so a reader that sees a fresh timestamp sees the value
            // that goes with it.
            fv_[fi].v     = v;
            fv_[fi].at_ms = now_ms ? now_ms : 1u;      // 0 is reserved for "nothing has arrived"

            // A field with no channel is not a dead field: it is one a SENSOR consumes, and the
            // sensor publishes it — with calibration and diagnostics — rather than this writing the
            // same channel underneath it.
            if (fd.sig == SIG_NONE || fd.sig >= SIG_COUNT) continue;

            // The TTL is what makes a dead sender visible. Without it the last value sits on the bus
            // for ever and a dash shows a number that stopped being true minutes ago; with it the
            // channel expires and everything reading it sees an absent channel instead.
            signals->set_typed(static_cast<SignalId>(fd.sig), v, now_ms, fd.ttl_ms, fd.priority);
        }
        decoded_++;
    }
}

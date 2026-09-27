#pragma once
//
// GenericCan — the CAN bus as user-defined signal I/O.
//
// The frames are not a protocol compiled into this firmware. They live in the tune
// (`CanConfig::gc_frame` / `gc_field`) and the user defines them for their car: an id, a direction,
// a period, and a list of fields, each field a slice of bits bound to a signal. Published protocols
// (a dash's broadcast set, a vehicle interface) reach the ECU as studio TEMPLATES loaded into that
// same pool, so a frame that turns out to be wrong is a tune edit rather than a firmware release.
//
// TRANSMIT reads the signal bus and sends on a period. RECEIVE decodes an arriving frame and writes
// the signal bus with a TTL. Nothing downstream learns about CAN: a received value becomes a signal
// and is then indistinguishable from a sensor, and a transmitted field reads whatever any other
// module already publishes.
//
// ONE DECODE, TWO SINKS. A received field is decoded exactly once and the result is offered two
// ways: written straight to the signal bus when the field names a channel, and left in `value(i)`
// for a SENSOR to acquire when it does not. That is what keeps this from being a second CAN input
// path beside the sensor one — a CAN-interface sensor reads a field from here and then gets the
// calibration, the operating-window diagnostics and the per-sensor DTCs that every other sensor has,
// rather than a parallel decode with none of them.
//
// A field whose `sig` is unset therefore is not a dead field: it is a field a sensor consumes. That
// also settles who writes the channel — the sensor does, once — instead of the two of them
// alternating on the bus.
//
// NOTHING IS COPIED OUT OF THE CONFIG. The tune is already in RAM, so the schedule and the receive
// index hold pool INDICES and the field maths reads `g_config` in place. A hundred frames therefore
// cost a few hundred bytes of index rather than a second copy of the pool.
//
#include <cstdint>
#include "CanFrame.h"
#include "CanMessageTypes.h"
#include "CanFieldValue.h"
#include "../../generated/modules/can_config.h"

class CanBroker;
class SignalBus;
class DtcManager;
class Sensors;

class GenericCan {
public:
    // The schedule and the index are sized by the pool: every frame in the tune can be live at once,
    // because refusing to run the 97th frame of a 96-frame pool is a limit with no reason behind it.
    static constexpr uint16_t MAX_FRAMES = CAN_GC_FRAME_COUNT;

    // Emitted per tick. The broker ticks at 1 kHz; a cap keeps one tick from monopolising the TX
    // mailboxes and starving the OBD responder that shares them.
    static constexpr uint8_t MAX_PER_TICK = 8;

    using FieldValue = GenericCanFieldValue;

    // ONE FRAME'S IDENTITY AS ONE NUMBER: the bus, the extended flag and the id. It is what an
    // arriving frame is matched on, and it is also what a CAN SENSOR stores to name the frame it
    // reads — the same key on both sides, so a sensor and the receive index can never disagree
    // about which frame 0x360 means.
    static constexpr uint32_t frame_key(uint8_t bus, bool ext, uint32_t id) {
        return (static_cast<uint32_t>(bus & 1u) << 30) | (ext ? (1u << 29) : 0u) | (id & 0x1FFFFFFFu);
    }

    // The pool index of the receive field named by (frame key, start bit), or -1 for no such field.
    //
    // A SENSOR NAMES ITS FIELD STRUCTURALLY, not positionally. The studio repacks the whole field pool
    // on any structural edit — frames index a contiguous run, so inserting one renumbers every later
    // field — and a stored pool index would have followed that silently onto somebody else's bits.
    // A frame id and a start bit survive it: neither moves when a frame is added above, and both are
    // what the Receive page already shows, so the reference reads the same way the document does.
    //
    // Static and taking the config, because this resolves the TUNE and not this object's state: the
    // sensor rebuild and GenericCan::configure() run on different tasks, and a resolver that read a
    // built index would answer against whichever of the two had run last.
    //
    // Defined here rather than in the .cpp because it is a pure function of the TUNE: the sensor
    // pipeline resolves through it, and making that an out-of-line call would drag CanBroker and a
    // transceiver into every translation unit that only wanted to ask which field a sensor reads.
    [[nodiscard]] static int32_t find_field(const CanConfig& cfg, uint32_t key, int16_t start_bit) {
        if (start_bit < 0 || start_bit > 63) return -1;      // -1 = no field named
        for (uint16_t i = 0; i < CAN_GC_FRAME_COUNT; i++) {
            const GcFrameConfig& f = cfg.gc_frame[i];
            if (!(f.flags & canmsg::FRAME_USED)) continue;
            if (f.flags & canmsg::FRAME_TX)      continue;
            if (frame_key(static_cast<uint8_t>(f.bus & 1u),
                          (f.flags & canmsg::FRAME_EXT) != 0, f.id) != key) continue;
            uint16_t first = 0, count = 0;
            if (!field_run(f, first, count)) continue;
            for (uint16_t n = 0; n < count; n++)
                if (cfg.gc_field[first + n].bit_off == static_cast<uint16_t>(start_bit))
                    return static_cast<int32_t>(first + n);
        }
        return -1;
    }

    // …the same question against the config this instance was configured with. -1 before the first
    // configure(), which is a sensor that publishes nothing until the frames are built.
    [[nodiscard]] int32_t find_field(uint32_t key, int16_t start_bit) const {
        return cfg_ ? find_field(*cfg_, key, start_bit) : -1;
    }

    // The tune this was configured with — the LIVE config, not a copy (see the note above). A CAN
    // sensor fingerprints the frame pools through this to know when a frame edit has moved the field
    // it reads, which its own bytes cannot tell it.
    [[nodiscard]] const CanConfig* config() const { return cfg_; }

    // The decoded value of pool field `i`, for a sensor to acquire. Null if the index is not a field.
    [[nodiscard]] const FieldValue* value(uint16_t i) const {
        return (i < CAN_GC_FIELD_COUNT) ? &fv_[i] : nullptr;
    }

    // …and the staleness the tune gave that field, so a sensor's freshness rule and the bus channel's
    // are the same number rather than two that can disagree.
    [[nodiscard]] uint16_t field_ttl_ms(uint16_t i) const {
        return (cfg_ && i < CAN_GC_FIELD_COUNT) ? cfg_->gc_field[i].ttl_ms : 0u;
    }

    // (Re)build the schedule and the receive index from the tune. Idempotent and cheap enough to call
    // on every config-generation move — it is a pass over the pool, not an allocation.
    void configure(const CanConfig& cfg, uint32_t now_ms);

    void set_broker(CanBroker* b)  { broker_ = b; }
    void set_dtc(DtcManager* d)    { dtc_ = d; }
    // Optional. Without it a receive field claiming a channel a sensor also publishes is still built —
    // it just cannot be REPORTED, and an unreported conflict is the failure this exists to prevent.
    void set_sensors(const Sensors* s) { sensors_ = s; }

    // Fire whatever transmit frames are due. A null signal bus sends nothing: every field would be
    // absent, which is not the same as a bus that is switched off.
    void tick(uint32_t now_ms, SignalBus* signals);

    // Decode one received frame. Called for EVERY frame the controller hands up, so the first thing
    // it does is a binary search that rejects an uninteresting id in a few comparisons.
    void on_rx(uint8_t bus, const CanFrame& fr, uint32_t now_ms, SignalBus* signals);

    // Diagnostics. Counters rather than flags: a bus that refuses 1% of frames and one that refuses
    // all of them are different faults, and they look identical if you only count successes.
    [[nodiscard]] uint32_t sent()     const { return sent_; }
    [[nodiscard]] uint32_t refused()  const { return refused_; }
    [[nodiscard]] uint32_t decoded()  const { return decoded_; }
    [[nodiscard]] uint16_t tx_count() const { return tx_n_; }
    [[nodiscard]] uint16_t rx_count() const { return rx_n_; }

private:
    bool bad_cfg_ = false, clash_ = false;       // the configuration's faults, asserted every tick
    void assert_config_dtcs(uint32_t now_ms);
    // A frame's fields, as a (first, count) run clamped to the pool. A tune whose indices are out of
    // range is a tune the studio should never have written, but it is also the one thing that would let
    // a bad edit read past the end of the array — so it is checked here rather than trusted.
    static bool field_run(const GcFrameConfig& f, uint16_t& first, uint16_t& count) {
        first = f.first_field;
        count = f.field_count;
        if (first >= CAN_GC_FIELD_COUNT) return false;
        if (static_cast<uint32_t>(first) + count > CAN_GC_FIELD_COUNT)
            count = static_cast<uint16_t>(CAN_GC_FIELD_COUNT - first);
        return count > 0;
    }

    // Build one transmit frame's payload. Returns false when a field's absent policy suppresses the
    // whole frame this cycle. `out` carries the PREVIOUS contents on entry, which is what makes
    // ABSENT_HOLD free: a field with nothing to say simply does not overwrite its own bits.
    bool build(const GcFrameConfig& f, SignalBus& signals, uint8_t* out) const;

    const CanConfig* cfg_     = nullptr;
    CanBroker*       broker_  = nullptr;
    DtcManager*      dtc_     = nullptr;
    const Sensors*   sensors_ = nullptr;

    // --- transmit: a deadline per frame ---------------------------------------------------------
    // Deadlines are SPREAD across each frame's own period at configure(). A protocol's rate tiers are
    // not coprime — six frames at 50 Hz started together fall due on the same millisecond for ever, so
    // every 20 ms the bus sees a six-frame burst and then silence. Spreading costs one modulo and
    // turns that into an even trickle, which matters because a shared bus is a queue, not a wire.
    struct TxEntry {
        uint16_t frame;        // index into cfg_->gc_frame
        uint32_t next_due_ms;
        uint8_t  data[8];      // last bytes built — doubles as the working buffer, so ABSENT_HOLD
    };                         // needs no separate storage and no special case in build()
    TxEntry  tx_[MAX_FRAMES] = {};
    uint16_t tx_n_ = 0;

    // --- receive: a sorted index, searched per arriving frame ------------------------------------
    // The set only changes when the tune does, so it is sorted once at configure() and binary
    // searched thereafter. A linear scan here would be paid on every frame on the wire, most of
    // which are not ours.
    struct RxEntry {
        uint32_t key;          // frame_key(bus, ext, id) — one comparable number
        uint16_t frame;        // index into cfg_->gc_frame
    };
    RxEntry  rx_[MAX_FRAMES] = {};
    uint16_t rx_n_ = 0;

    FieldValue fv_[CAN_GC_FIELD_COUNT] = {};

    uint32_t sent_    = 0;
    uint32_t refused_ = 0;
    uint32_t decoded_ = 0;
};

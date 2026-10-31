#pragma once
#include "Dtc.h"

// ---------------------------------------------------------------------------
// DtcManager — the "error desk". A passive 64-slot table of active/stored DTCs,
// the single source of truth. Error sources call raise()/heal() at their own
// state-machine edges; the manager dedupes by code, counts activations, tracks
// active/stored status, and serialises to/from an SD image. Indicators, OBD
// Mode 03/04, the LED and persistence all read from here.
//
// Lifecycle of a code:
//   raise()  inactive→active : count++, ACTIVE|STORED set, first/last stamped
//   raise()  already active   : just refresh last-seen (no double count)
//   heal()                    : clear ACTIVE, keep STORED (history, like a MIL)
//   clear()/clear_all()       : remove from the table (OBD Mode 04 / TS command)
// ---------------------------------------------------------------------------

class DtcManager {
public:
    static constexpr uint8_t SLOTS = 64;

    // boot_id is the persisted power-cycle counter (universal timestamp half).
    void init(uint16_t boot_id);

    // --- producers (called by any error source) ---
    // Condition is true: ensure the code is active. Safe to call every frame —
    // count only bumps on the inactive→active edge. severity is the code's level.
    // Returns TRUE on that edge (a new activation) so the owner can log a freeze-frame.
    // ttl_ms: HOW LONG THIS CODE STAYS CURRENT WITHOUT BEING RE-RAISED — the signal bus's rule, applied
    // to faults. A judge that evaluates every pass declares a few of its periods and simply keeps
    // asserting while the fault is true; stop asserting — because the check was unticked, the module was
    // switched off, or the condition is no longer evaluated at all — and the code ages out of ACTIVE on
    // its own (age(), below). The history is untouched: it drops to STORED exactly as heal() leaves it.
    //
    // That is what makes this a property of the TABLE rather than of every raiser. The alternative was
    // for each of nineteen raise sites to notice it had stopped judging and heal itself, which is a rule
    // that has to be remembered once per module for ever — and was not: see the note on heal() below,
    // written after the knock report, and the sensor operating-window codes that outlived their own
    // tick box until 2026-09-16.
    //
    // DTC_TTL_LATCH (0) opts out, and means "this is an EVENT, not a condition": a pre-ignition strike,
    // a misfire tally, a DTC a Lua script raised. Nothing is going to re-assert those, and they are
    // meant to stand until healed or cleared.
    bool raise(uint16_t code, uint8_t source, uint8_t severity, uint32_t now_ms,
               uint32_t ttl_ms = DTC_TTL_DEFAULT);

    // Age out ACTIVE codes nobody has re-raised within their declared ttl — SignalBus::expire for the
    // error desk. Call once per frame. Drops ACTIVE, keeps STORED; ttl 0 never expires here.
    void age(uint32_t now_ms);
    // HOW OFTEN THE BACKSTOP HAS ACTUALLY CAUGHT SOMETHING. A producer that knows its condition has
    // cleared heals immediately; ageing is for the ones that stop saying anything at all. If this stays
    // zero in normal running then the ttl is costing nothing and insuring everything, and if it climbs
    // it is naming a producer that goes quiet while its fault may still be real — which is worth
    // knowing either way, and is not otherwise visible.
    uint16_t aged_count() const noexcept { return aged_; }
    // Condition has cleared: drop ACTIVE, keep the stored history.
    //
    // HEALING IS THE RAISER'S JOB, AND A DISABLED MODULE IS NOT THERE TO DO IT. Every module that
    // raises a code returns early from update() while its enable flag is off, and that return skips
    // the heal() in its enabled path — so a code raised while the module was running stays ACTIVE for
    // ever, because the only thing that would retire it is the thing no longer running. Reported
    // against knock: enable it, let it raise, disable it, and the code still reads as current.
    //
    // So a module with an enable gate heals its own codes in the DISABLED branch, on the
    // enabled->disabled EDGE. The edge is sufficient — restore() brings codes back STORED-but-not-
    // active, so a code of yours can only be ACTIVE if THIS run raised it, which means you were
    // enabled at the time. It is also all that is affordable: heal() is a linear scan of the table,
    // and Sensors measured the heal-every-frame version at milliseconds inside a 1 kHz frame.
    //
    // Boost/Knock/Launch/Stepper/TransientThrottle keep a was_enabled_ bool for that edge; App and
    // EgtProtect already know from active_dtc_/cut_code_, which are non-zero only while raised.
    void heal(uint16_t code);

    // System-active (key-on) gate. While inactive (USB/bench), raise() accepts ONLY config/validity
    // codes (PIN_ARBITER pin conflicts, CONFIG tune validity) — runtime faults (sensors, trigger,
    // protection, CAN) are suppressed. The inactive edge also heals any already-active runtime codes
    // (drops ACTIVE, keeps STORED history), so the bench shows a clean active list. Set every frame.
    void set_active(bool a) noexcept;

    // A REQUIRED SIGNAL IS MISSING — the module "signal missing" codes (P17xx). Two rules every such
    // check needs, so they live here rather than in each module:
    //   * A 5 V SENSOR REFERENCE IS DOWN: every pin-read sensor publishes invalid on purpose, and the
    //     supply raises its own code (P0641 / P0651). A module reporting its input "missing" then names
    //     a symptom, not the fault — and at key-off, when the references go a few ms before the battery
    //     reading falls below the key threshold, it named one that is not a fault at all: P1720 with the
    //     key going off, battery 12.46 V, CEL flash, stored code (Kelly's Hilux, three times).
    //   * PERSISTENCE: missing for MISSING_SIGNAL_DELAY_MS before it counts — the same 100 ms the supply
    //     codes wait. A signal really gone still reports within a tenth of a second; one late frame does
    //     not light the CEL for a second and leave a stored code.
    // Healing stays immediate. `since` is the caller's own timer, one per code (0 = not missing).
    static constexpr uint32_t MISSING_SIGNAL_DELAY_MS = 100;
    void report_missing(uint32_t& since, uint16_t code, uint8_t source, uint8_t severity,
                        bool missing, uint32_t now_ms, uint32_t ttl_ms);
    // Set by Sensors every pass (false with the key off). See report_missing().
    void set_supply_lost(bool lost) noexcept { supply_lost_ = lost; }
    bool supply_lost() const noexcept { return supply_lost_; }

    // CROSS-TASK GUARD. The table is raised into by the engine task, the CAN task and Lua, read and
    // cleared by comms/CLI/OBD, and serialised by the housekeeping save — all different tasks, and a
    // record copied half-way through an update is a torn record on the card. The platform supplies a
    // lock (FreeRTOS scheduler suspend: short, no interrupt latency, nests); host tests leave it unset.
    using LockFn = void (*)();
    static void set_lock(LockFn lock, LockFn unlock) { s_lock_ = lock; s_unlock_ = unlock; }

    // --- consumer / housekeeping ---
    bool clear(uint16_t code);   // remove one stored code; false if not present
    void clear_all();            // wipe the table (Mode 04)
    // Bumped on every clear_all(). Edge-triggered producers watch this to know the table was
    // wiped under them and re-assert their still-active faults (order-independent — no strobe race).
    uint32_t clear_generation() const { return clear_gen_; }

    // Codes that crossed inactive→active since the last drain — drained once per
    // frame by the owner (EngineTask), which captures a freeze-frame for each via
    // set_freeze_frame(). Bounded; overflow drops the surplus edges (the table
    // still records them, only the freeze-frame snapshot is missed).
    static constexpr uint8_t PENDING_MAX = 16;
    uint8_t drain_new_activations(uint16_t* out, uint8_t max);

    // Store the freeze-frame (n values, clamped to DTC_FF_CHANNELS) on a code's
    // record. No-op if the code isn't present. Called on the activation edge.
    void set_freeze_frame(uint16_t code, const float* ff, uint8_t n);

    // --- queries (LED / telemetry / OBD) ---
    uint8_t active_count() const;
    uint8_t stored_count() const;
    uint8_t worst_severity() const;                  // worst severity among ACTIVE codes (0 = none)
    // …and among STORED ones, for a protection level configured to watch those instead: a fault whose
    // damage healing does not undo should keep the engine in limp until somebody clears the codes.
    uint8_t worst_stored_severity() const;
    uint8_t source_severity(uint8_t source) const;   // worst ACTIVE severity for one source (0 = none)
    uint8_t code_severity(uint16_t code) const;      // severity if THIS code is ACTIVE, else 0
    uint16_t worst_code(uint8_t lo_sev, uint8_t hi_sev) const;  // code of the worst ACTIVE DTC with severity in [lo,hi] (0=none)
    uint8_t  list_active_band(uint8_t lo_sev, uint8_t hi_sev, uint16_t* out, uint8_t max) const;  // ACTIVE codes with severity in [lo,hi] → out[] (contiguous), returns count
    uint8_t list_active(uint16_t* out, uint8_t max) const;  // active codes → out[], returns n
    const DtcRecord& slot(uint8_t i) const { return table_[i]; }

    // --- the table image: the 'G' reply, and the body of the SD store ---
    // Layout: [magic u32][version u16][boot_id u16][n u16][DtcRecord x n]. Returns bytes written,
    // or 0 if buf can't even hold the 10-byte header.
    //
    // PAGED: writes stored records starting at `first` (index into the STORED set, not the slot
    // array) and stops when buf_len runs out — `n` in the header is the count IN THIS IMAGE, not the
    // table total. The comms path caps buf_len at the wire's max payload and pages, because a full
    // table serialises to 10 + 36*64 = 2314 bytes -- more than twice what one frame can carry
    // (OMNI_MAX_PAYLOAD 1030). Emitting that produced a header announcing a length the framing could
    // not deliver, and every client waiting on it hung.
    uint32_t serialize(uint8_t* buf, uint32_t buf_len, uint16_t first = 0) const;
    // Records that fit in a buffer of `buf_len` bytes — lets a caller page without guessing.
    static constexpr uint32_t records_that_fit(uint32_t buf_len) {
        return buf_len < 10u ? 0u : (buf_len - 10u) / sizeof(DtcRecord);
    }
    static constexpr uint32_t image_max_bytes() { return 10 + sizeof(DtcRecord) * SLOTS; }

    // --- persistence (the SD store; the caller does the actual SD read/write) ---
    // The store is the whole-table image with its own version (3) and a CRC32 over every byte before
    // it: [image, version 3][crc32 u32]. The 'G' image keeps version 2 and has no CRC — the studio and
    // the dump tool read it as before.
    //
    // A STORE IS LOADED ONLY IF THIS FIRMWARE WROTE IT. The card outlives the firmware: a board that ran
    // something else first can hold a dtc.bin whose bytes happen to start "DTC1", and the magic and the
    // version were all that used to be checked — so whatever followed came back as stored codes, with
    // severities that drove the protection levels watching STORED faults. Now the length must be exact,
    // the CRC must match, and every record must be one the table could have held. See restore().
    enum class StoreLoad : uint8_t {
        Loaded,     // a version-3 store, CRC good
        Upgraded,   // a version-2 store an older build wrote: loaded, rewritten as version 3 at the next save
        Rejected,   // not a store this firmware wrote: nothing loaded, overwritten at the next save
    };
    uint32_t  store_image(uint8_t* buf, uint32_t buf_len) const;   // whole table; 0 if it does not fit
    StoreLoad restore(const uint8_t* buf, uint32_t len);
    static constexpr uint32_t store_max_bytes() { return image_max_bytes() + 4u; }

    // Persisted power-cycle counter; the boot loader peeks the last value out of
    // the SD store, increments it, and feeds it back via init() for this session.
    uint16_t boot_id() const { return boot_id_; }
    // Read just the stored boot_id out of a store (0 unless restore() would accept it).
    static uint16_t image_boot_id(const uint8_t* buf, uint32_t len);

    // Dirty flag for the SD writer: any table mutation sets it; the save task
    // consumes it and writes the image only when something actually changed
    // (no per-frame SD churn). clear-on-read.
    bool consume_dirty();
    void mark_dirty() { dirty_ = true; }   // a save that failed: the change is still owed

    // The store image of the whole table (store_image) into `buf`, only if it changed since the last call —
    // the dirty check and the copy under ONE lock, so a change landing between them is never lost. Returns
    // bytes, 0 if clean.
    uint32_t snapshot_if_dirty(uint8_t* buf, uint32_t buf_len);

private:
    int  find(uint16_t code) const;   // slot index of a stored code, or -1
    // Cheap "is it worth looking" — see the .cpp. False means definitely absent.
    bool maybe_present(uint16_t code) const;
    // A slot for a new code, or -1 when every slot holds an ACTIVE code of equal-or-higher severity.
    // Preference: empty > stored-but-healed (oldest) > lowest-severity ACTIVE code this one outranks.
    int  alloc(uint16_t code, uint8_t severity);
    // The store's version if restore() would accept `buf` (DTC_STORE_VERSION, or the older one it still
    // reads), 0 if not. The one check, shared by restore() and image_boot_id().
    static uint16_t store_version(const uint8_t* buf, uint32_t len);
    static uint32_t seal_store(uint8_t* buf, uint32_t n);

    DtcRecord table_[SLOTS] = {};
    // NOT PERSISTED, and it must not be: a ttl is a statement about who is judging in THIS run, and a
    // code restored from the card comes back STORED-but-not-active, with nobody asserting it.
    uint32_t  ttl_[SLOTS] = {};
    // 256-bit presence filter over the code's low bits, so heal()/find() answer "not here" without
    // walking 64 records. Conservative: a bit may be set for a code that has since been cleared, which
    // costs one walk — it must never be CLEAR for a code that is present.
    uint32_t  present_[8] = {};
    uint16_t  aged_    = 0;     // codes retired by age() rather than by their producer
    uint16_t  boot_id_ = 0;
    uint32_t  clear_gen_ = 0;   // bumped by clear_all(); read via clear_generation()
    bool      dirty_   = false;
    bool      active_  = true;  // system-active gate: false on USB/bench (see set_active)
    bool      supply_lost_ = false;   // a 5 V sensor reference is down (see report_missing)
    // When each slot's code last went active (this run). Not persisted: CONFIRMED is what persists.
    uint32_t  active_since_[SLOTS] = {};
    bool      cycle_open_ = false;          // a key cycle is in progress (see set_active)
    void      close_cycle();                // key-on edge: age the codes the last cycle left clean
    void      erase_slot(uint8_t i);

    friend struct DtcGuard;
    static LockFn s_lock_;
    static LockFn s_unlock_;
    uint16_t  pending_[PENDING_MAX] = {};   // codes that just went active (drained per frame)
    uint8_t   pending_n_ = 0;
};

#pragma once

#include "../../generated/signal_ids.h"
#include "SignalLock.h"
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// SignalBus — the shared sensor data layer between producers and consumers.
//
// Producers (analog HAL, CAN parsers, Lua scripts) call set().
// Consumers (engine modules) call get() / valid() / age_ms().
//
// Thread safety: get()/valid() read one aligned word (value cell or the valid
// byte), which is atomic on Cortex-M7 — readers never lock and never tear on a
// value. Writes go through a priority gate (accept_) that reads then stores
// several fields — a read-modify-write. The engine task is the highest-priority
// writer, so on a single core nothing can preempt its RMW: it writes lock-free.
// A lower-priority writer (the Lua thread, or any override at prio > PRIO_BASE)
// CAN be preempted by the engine task mid-RMW, so it wraps the gate+store in a
// short critical section (SigLockGuard) that blocks task switching. See
// SignalLock.h. All bus writers run in task context (no ISR writes the bus).
//
// By-name lookup (get_by_name / set_by_name) uses SIGNAL_NAMES[] from
// signal_ids.h — O(N) scan over SIG_COUNT entries, negligible cost.
// ---------------------------------------------------------------------------

// A slot's value is a 4-byte cell reinterpreted per the channel's declared type:
// `f` for analog / engineering-unit signals (the default), `u` / `i` for counters,
// bitfields and scaled integers that must not be round-tripped through float. Same
// width as the old plain float, so the slot — and the whole bus array — do not grow.
// The channel's type is the catalog's authority (a formal declaration, not stored
// here); producers and consumers use the matching accessor — the bus never converts
// between f / u / i. Type-erased readers (Lua, by-name) consult the generated type
// table, not the slot.
union SignalCell {
    float    f = 0.0f;
    uint32_t u;
    int32_t  i;
};

// Write priority for the override bus. A higher-priority write out-votes a lower one for as long as
// it stays fresh (its ttl); an equal-or-higher write always lands; ANY write reclaims a stale slot
// — so a lower-priority base producer takes the slot back the instant a higher-priority override
// stops refreshing. This replaces temporal "last word" (Lua running last in the engine task) with
// precedence, which is what lets Lua move off the engine task onto its own thread. Base producers
// (sensors, modules) write at PRIO_BASE; Lua overrides at PRIO_LUA. 8-bit, so intermediate authority
// levels (e.g. a protection layer between base and Lua) can slot in later.
// A WRITE IS REFUSED ONLY BY A STRICTLY HIGHER PRIORITY (see accept_), so two producers at the SAME
// priority alternate, last one wins, and nothing says so. That is why an inbound CAN value does not
// sit at PRIO_BASE: a sensor publishes even when it is INVALID — the publish-invalid rule, Stages.h —
// so a configured-but-failing sensor would mark a perfectly good CAN channel invalid, which is the
// worse of the two failures. A frame the tuner configured to carry `clt` means it; a sensor also
// claiming that channel is the misconfiguration, and GenericCan raises a code naming it.
enum SignalPriority : uint8_t { PRIO_BASE = 0, PRIO_CAN = 10, PRIO_LUA = 200 };

struct SignalValue {
    SignalCell value;           // reinterpreted per channel type — see SignalCell
    bool     valid      = false;
    uint8_t  prio       = PRIO_BASE;  // authority of the current value (see SignalPriority)
    uint32_t set_at_ms  = 0;    // platform_get_tick_ms() when last written
    uint32_t ttl_ms     = 0;    // freshness lifetime declared by the last writer (0 = never expire)
};

class SignalBus {
public:
    // --- Producer API ---

    // ttl_ms: how long this value stays fresh. 0 = never expires (legacy). A writer
    // updating at rate R passes ttl ~= a few periods, so the slot decays if it stops
    // refreshing — see expire_stale(). Whoever writes LAST sets the freshness, so a
    // slow producer turning off can't blank a fast co-producer's value (the co-writer
    // keeps set_at_ms fresh). Expiry is age-based, never producer-based.
    // A producer using the WRONG accessor is the writer-side twin of the reader bug: set() on an integer
    // channel stores float bits where every consumer reads .u. The catalog already knows which member is
    // live, so the setter routes to it rather than corrupting the cell. Free — SIGNAL_TYPES is a const
    // table in flash, and storing the type in the slot would only add a second source of truth to keep in
    // sync with it.
    void set(SignalId s, float value, bool valid = true, uint32_t now_ms = 0,
             uint32_t ttl_ms = 0, uint8_t prio = PRIO_BASE) {
        if (s < SIG_COUNT && s != SIG_NONE && SIGNAL_TYPES[s] != SIG_T_FLOAT) {
            // Declared integer: convert, never reinterpret — and keep the caller's VALIDITY. This went
            // through set_typed(), which has no validity argument and always publishes valid, so
            // set(ch, x, false) on an integer channel announced a reading instead of withdrawing one.
            if (SIGNAL_TYPES[s] == SIG_T_U32)
                set_u32(s, static_cast<uint32_t>(value < 0.0f ? 0.0f : value + 0.5f), valid, now_ms, ttl_ms, prio);
            else
                set_i32(s, static_cast<int32_t>(value < 0.0f ? value - 0.5f : value + 0.5f), valid, now_ms, ttl_ms, prio);
            return;
        }
        if (s >= SIG_COUNT || s == SIG_NONE) return;
        // A CUT IS ONLY EVER ASKED FOR, NEVER REFUSED. fuel_cut / ign_cut hold a cut while some requester
        // keeps writing `true`, and end when it expires. Anything else written there is dropped: a Lua
        // signalWrite("fuel_cut", 0) or a CAN field mapped to the channel would otherwise take the slot
        // at its priority and out-vote the rev limiter and engine protection for as long as it lasted —
        // for ever, with no ttl. A script may add a cut; it cannot take one away.
        if ((s == SIG_FUEL_CUT || s == SIG_IGN_CUT) && !(valid && value >= 0.5f)) return;
        SigLockGuard g(prio > PRIO_BASE);   // low-priority override: atomic RMW vs the engine task
        if (accept_(slots_[s], valid, now_ms, ttl_ms, prio))
            slots_[s].value.f = value;
    }

    // Typed producer API — for channels whose declared type is integer (counters,
    // bitfields, scaled ints): write the raw cell so no precision or bit pattern is
    // lost through float. Use the accessor matching the channel's catalog type.
    void set_u32(SignalId s, uint32_t value, bool valid = true, uint32_t now_ms = 0,
                 uint32_t ttl_ms = 0, uint8_t prio = PRIO_BASE) {
        if (s >= SIG_COUNT || s == SIG_NONE) return;
        SigLockGuard g(prio > PRIO_BASE);
        if (accept_(slots_[s], valid, now_ms, ttl_ms, prio))
            slots_[s].value.u = value;
    }

    void set_i32(SignalId s, int32_t value, bool valid = true, uint32_t now_ms = 0,
                 uint32_t ttl_ms = 0, uint8_t prio = PRIO_BASE) {
        if (s >= SIG_COUNT || s == SIG_NONE) return;
        SigLockGuard g(prio > PRIO_BASE);
        if (accept_(slots_[s], valid, now_ms, ttl_ms, prio))
            slots_[s].value.i = value;
    }

    // Convenience: set a boolean signal (stores 0.0 / 1.0)
    void set_bool(SignalId s, bool value, uint32_t now_ms = 0, uint32_t ttl_ms = 0,
                  uint8_t prio = PRIO_BASE) {
        set(s, value ? 1.0f : 0.0f, true, now_ms, ttl_ms, prio);
    }

    // Invalidate a slot (value becomes stale / unknown)
    void invalidate(SignalId s) {
        if (s >= SIG_COUNT || s == SIG_NONE) return;
        slots_[s].valid = false;
    }

    // Age out slots nobody has refreshed within their declared ttl. Call once per
    // frame with the current tick. Age-based, so a co-producer that keeps writing a
    // shared channel keeps it fresh; only a channel no producer is refreshing decays.
    // ttl_ms == 0 slots never expire here.
    //
    // A TTL OF N IS N MILLISECONDS, not N+1: a value set at t is gone once t+N is reached. This was
    // `> ttl`, and the frame ages the bus after its reads, so a value set with ttl N was still read in
    // the frame at t+N — one frame too many. Nothing noticed except a cut that publishes for exactly one
    // decision (a soft cut, ttl = frame_ms()): each one held a frame longer, so a 50 % cut cut ~60 %.
    void expire_stale(uint32_t now_ms) {
        for (uint16_t s = 0; s < SIG_COUNT; s++) {
            SignalValue& v = slots_[s];
            if (v.valid && v.ttl_ms != 0 && (now_ms - v.set_at_ms) >= v.ttl_ms) {
                v.valid = false;
            }
        }
    }

    // Withdraw every value held at `prio` or above: it goes invalid, so the next base write takes the
    // slot back. For an override whose OWNER has gone — a Lua script switched off or replaced. A script
    // write with no ttl latches (by design: a safety the base cannot release), and with nothing to
    // release it, a cut the old script wrote stayed in force after the script itself was gone.
    void release_prio(uint8_t prio) {
        SigLockGuard g(true);
        for (uint16_t s = 0; s < SIG_COUNT; s++) {
            SignalValue& v = slots_[s];
            if (v.prio >= prio) { v.valid = false; v.prio = PRIO_BASE; v.ttl_ms = 0; }
        }
    }

    // --- Consumer API ---

    float get(SignalId s, float fallback = 0.0f) const {
        if (s >= SIG_COUNT || s == SIG_NONE || !slots_[s].valid) return fallback;
        return slots_[s].value.f;
    }

    // Typed consumer API — read the raw cell for integer-typed channels (see set_u32 /
    // set_i32). The bus does not convert: use the accessor matching the channel's type.
    uint32_t get_u32(SignalId s, uint32_t fallback = 0) const {
        if (s >= SIG_COUNT || s == SIG_NONE || !slots_[s].valid) return fallback;
        return slots_[s].value.u;
    }

    int32_t get_i32(SignalId s, int32_t fallback = 0) const {
        if (s >= SIG_COUNT || s == SIG_NONE || !slots_[s].valid) return fallback;
        return slots_[s].value.i;
    }

    bool get_bool(SignalId s) const {
        return get_typed(s) >= 0.5f;   // a bool channel may be declared u32; get() would reinterpret it
    }

    bool valid(SignalId s) const {
        return s < SIG_COUNT && s != SIG_NONE && slots_[s].valid;
    }

    uint32_t age_ms(SignalId s, uint32_t now_ms) const {
        if (s >= SIG_COUNT || s == SIG_NONE) return 0xFFFFFFFFu;
        return now_ms - slots_[s].set_at_ms;
    }

    // --- By-name API (used by Lua signalRead/signalWrite) ---

    // Returns SIG_NONE if name not found.
    static SignalId id_by_name(const char* name) {
        for (uint16_t i = 0; i < SIG_COUNT; i++) {
            if (strcmp(SIGNAL_NAMES[i], name) == 0) {
                return static_cast<SignalId>(i);
            }
        }
        return SIG_NONE;
    }

    // TYPE-ERASED read: the caller does not know the channel's cell type, so consult the catalog and use
    // the matching accessor. Reading .f from an integer cell REINTERPRETS its bits — frame_count = 1000
    // comes back as ~1.4e-42, not 1000 — so this is not a precision question but a correctness one.
    // Everything that reads by name (Lua's signalRead, the CAN/by-name plumbing) goes through here.
    float get_typed(SignalId s, float fallback = 0.0f) const {
        if (s >= SIG_COUNT || s == SIG_NONE || !slots_[s].valid) return fallback;
        switch (SIGNAL_TYPES[s]) {
            case SIG_T_U32: return static_cast<float>(slots_[s].value.u);
            case SIG_T_I32: return static_cast<float>(slots_[s].value.i);
            default:        return slots_[s].value.f;
        }
    }

    float get_by_name(const char* name, float fallback = 0.0f) const {
        const SignalId s = id_by_name(name);
        return (s != SIG_NONE) ? get_typed(s, fallback) : fallback;
    }

    // Returns false if name not found. ttl_ms: freshness lifetime (0 = never expire); a producer
    // refreshing at rate R passes a few periods so the slot decays via expire_stale() if it stops.
    // TYPE-ERASED write, the mirror of get_typed: storing a float into an integer cell leaves consumers
    // reading .u on the float's bit pattern.
    void set_typed(SignalId s, float value, uint32_t now_ms = 0, uint32_t ttl_ms = 0,
                   uint8_t prio = PRIO_BASE) {
        if (s >= SIG_COUNT || s == SIG_NONE) return;
        switch (SIGNAL_TYPES[s]) {
            case SIG_T_U32: set_u32(s, static_cast<uint32_t>(value < 0.0f ? 0.0f : value + 0.5f),
                                    true, now_ms, ttl_ms, prio); break;
            case SIG_T_I32: set_i32(s, static_cast<int32_t>(value < 0.0f ? value - 0.5f : value + 0.5f),
                                    true, now_ms, ttl_ms, prio); break;
            default:        set(s, value, true, now_ms, ttl_ms, prio); break;
        }
    }

    bool set_by_name(const char* name, float value, uint32_t now_ms = 0, uint32_t ttl_ms = 0,
                     uint8_t prio = PRIO_BASE) {
        const SignalId s = id_by_name(name);
        if (s == SIG_NONE) return false;
        set_typed(s, value, now_ms, ttl_ms, prio);
        return true;
    }

    // Reset all slots to invalid (call at startup)
    void clear() {
        for (auto& slot : slots_) {
            slot = SignalValue{};
        }
    }

private:
    // The priority gate. Returns true (and stamps validity/freshness/priority) when a write at
    // `prio` should land; the caller then writes the value cell. A write is rejected ONLY when a
    // strictly-higher-priority value is still fresh — equal-or-higher always lands, and any writer
    // reclaims a stale slot (invalid, or aged past its ttl). `ttl_ms == 0` never goes stale on age,
    // so it latches until an equal-or-higher writer (or an explicit short-ttl write) replaces it.
    // Reads are unaffected: get() never consults prio/ttl, so a consumer still reads one word.
    bool accept_(SignalValue& v, bool valid, uint32_t now_ms, uint32_t ttl_ms, uint8_t prio) {
        // A MISSING TIMESTAMP IS NOT A TIME. now_ms defaults to 0 for every caller that does not pass
        // one, and that 0 was being subtracted from a real tick to age the value ALREADY in the slot.
        // The subtraction is unsigned, so 0 - 100000 is not "before" — it is 4 294 867 296. Every live
        // override therefore looked ~50 days old, was judged stale, and a base-priority write walked
        // straight over it. Those channels had quietly reverted to "whoever wrote last wins", which is
        // the temporal rule this gate exists to replace.
        //
        // Ignoring it costs nothing. "Never published" is carried by v.valid, not by the timestamp —
        // and !v.valid is tested first and short-circuits — so an untimed write still lands on a slot
        // nobody has written yet. The timestamp only ever mattered for judging a LIVE value's age, and
        // a caller that did not supply one has said nothing about what time it is.
        //
        // An expired override is still reclaimed: expire_stale() runs on the engine frame and marks it
        // invalid, after which !v.valid lets an untimed writer take the slot back.
        const bool timed = (now_ms != 0);
        const bool stale = !v.valid || (timed && v.ttl_ms != 0 && (now_ms - v.set_at_ms) >= v.ttl_ms);
        if (prio < v.prio && !stale)
            return false;                     // out-voted by a live higher-priority override
        v.valid     = valid;
        v.prio      = prio;
        v.set_at_ms = now_ms;
        v.ttl_ms    = ttl_ms;
        return true;
    }

    SignalValue slots_[SIG_COUNT];
};

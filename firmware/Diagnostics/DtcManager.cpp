#include "DtcManager.h"
#include <cstring>

DtcManager::LockFn DtcManager::s_lock_   = nullptr;
DtcManager::LockFn DtcManager::s_unlock_ = nullptr;

struct DtcGuard {
    DtcGuard()  { if (DtcManager::s_lock_)   DtcManager::s_lock_(); }
    ~DtcGuard() { if (DtcManager::s_unlock_) DtcManager::s_unlock_(); }
    DtcGuard(const DtcGuard&) = delete;
    DtcGuard& operator=(const DtcGuard&) = delete;
};

namespace {
constexpr uint32_t DTC_MAGIC   = 0x44544331u;  // "DTC1"
constexpr uint16_t DTC_VERSION = 2;            // 2: DtcRecord gained the freeze-frame (36B)
}

void DtcManager::init(uint16_t boot_id) {
    boot_id_ = boot_id;
    // NOTE: table is left as-is — restore() repopulates it from SD; a cold start
    // (no SD image) leaves it zeroed by the in-class initialiser.
}

// IS THIS CODE IN THE TABLE AT ALL? A 256-bit filter over the code's low byte, so the common question —
// "heal something that was never raised" — is answered without touching the table.
//
// It is the common question by a long way: a module that raise-or-heals every pass heals on every
// healthy pass, and healing meant a linear walk of 64 records of 36 bytes each. Measured at ~600 cycles
// a call; the h-bridge alone was spending 11 us of every 1 kHz frame on four of them, retiring codes
// that had never existed. A false positive just costs the walk that used to happen anyway, so the
// filter never has to be cleared precisely — only never to MISS a code that is present.
bool DtcManager::maybe_present(uint16_t code) const {
    return (present_[(code >> 5) & 7u] & (1u << (code & 31u))) != 0;
}

int DtcManager::find(uint16_t code) const {
    if (code == 0 || !maybe_present(code)) return -1;
    for (uint8_t i = 0; i < SLOTS; i++)
        if (table_[i].code == code) return i;
    return -1;
}

int DtcManager::alloc(uint16_t code, uint8_t severity) {
    int empty = -1, evict = -1;
    uint32_t evict_ms = 0xFFFFFFFFu;
    // Lowest-severity ACTIVE code that the incoming one outranks, oldest first — the last-resort victim.
    int  weak = -1;
    uint8_t  weak_sev = severity;          // only a STRICTLY lower severity may be displaced
    uint32_t weak_ms  = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (table_[i].code == 0) { empty = i; break; }
        // Eviction candidate: a stored-but-not-active code, oldest last-seen first.
        if (!(table_[i].status & DTC_ACTIVE) && table_[i].last_ms <= evict_ms) {
            evict_ms = table_[i].last_ms; evict = i;
        }
        if ((table_[i].status & DTC_ACTIVE) &&
            (table_[i].severity < weak_sev ||
             (table_[i].severity == weak_sev && weak >= 0 && table_[i].last_ms < weak_ms))) {
            if (table_[i].severity < severity) { weak = i; weak_sev = table_[i].severity; weak_ms = table_[i].last_ms; }
        }
    }
    // Empty > healed > a strictly-less-severe ACTIVE code. That last step matters during bring-up: a rig
    // full of legitimate "not wired yet" config faults (level 1/2) would otherwise saturate the table
    // and make a later level 3 — knock, overboost, throttle correlation — vanish with no record, exactly
    // when someone is first turning the key. A level 1 losing its slot to a level 3 is the right trade; the
    // important fault is the one that must survive. Equal or higher severity is never displaced.
    const int slot = (empty >= 0) ? empty : (evict >= 0 ? evict : weak);
    if (slot < 0) return -1;                          // full of codes at or above this severity → drop
    table_[slot] = {};
    table_[slot].code = code;
    return slot;
}

// Sources accepted REGARDLESS of the key-on gate (and not auto-healed on the inactive edge): config /
// validity, plus THROTTLE — an ETB is driven engine-stopped (autocal, key-on sweep) and only faults on a
// real drive fault (never spuriously), so its safety-critical codes must record even on USB/bench power.
static inline bool dtc_is_config_source(uint8_t source) {
    return source == DtcSource::PIN_ARBITER || source == DtcSource::CONFIG
        || source == DtcSource::THROTTLE;
}

bool DtcManager::raise(uint16_t code, uint8_t source, uint8_t severity, uint32_t now_ms,
                       uint32_t ttl_ms) {
    if (code == 0) return false;
    // System-active (key-on) gate: on USB/bench only config/validity codes may raise; runtime faults
    // (sensors / trigger / protection / CAN) are suppressed so the bench shows no phantom faults.
    if (!active_ && !dtc_is_config_source(source)) return false;
    DtcGuard g;
    int i = find(code);
    if (i < 0) {
        i = alloc(code, severity);
        if (i < 0) return false;          // table saturated with active codes — dropped
        present_[(code >> 5) & 7u] |= (1u << (code & 31u));
        table_[i].source     = source;
        table_[i].severity   = severity;
        table_[i].first_boot = boot_id_;
        table_[i].first_ms   = now_ms;
    }
    DtcRecord& r = table_[i];
    ttl_[i]     = ttl_ms;                 // the freshness the CURRENT raiser declares (see age())
    r.severity  = severity;               // a code's level is fixed by its source
    // …and so is its source: the raiser is the authority, not whatever the record was saved with. A code
    // restored from the card kept the id it was STORED under, so when the subsystem ids moved (Dtc.h) a
    // restored config code re-raised as "sensor 94" and was healed at key-off like a runtime fault.
    r.source    = source;
    r.last_boot = boot_id_;
    r.last_ms   = now_ms;
    bool edge = false;
    if (!(r.status & DTC_ACTIVE)) {       // inactive → active edge
        if (r.count < 0xFFFFu) r.count++;
        r.heal_cycles = 0;
        // A code that already failed in an EARLIER key cycle and fails again is confirmed (OBD's two-trip
        // rule); so is an event — nothing will hold it active for DTC_CONFIRM_MS.
        const bool earlier_cycle = (r.status & DTC_STORED) && !(r.status & DTC_CYCLE_ACT) && r.count > 1;
        if (earlier_cycle || ttl_ms == DTC_TTL_LATCH) r.status |= DTC_CONFIRMED;
        active_since_[i] = now_ms;
        dirty_ = true;                    // edge changes the persisted set
        edge   = true;
        if (pending_n_ < PENDING_MAX) pending_[pending_n_++] = code;   // queue for the freeze-frame log
    } else if (!(r.status & DTC_CONFIRMED) && (now_ms - active_since_[i]) >= DTC_CONFIRM_MS) {
        r.status |= DTC_CONFIRMED;        // held active long enough to be real
        dirty_ = true;
    }
    if (!(r.status & DTC_CYCLE_ACT)) dirty_ = true;
    r.status |= (DTC_ACTIVE | DTC_STORED | DTC_CYCLE_ACT);
    return edge;
}

uint8_t DtcManager::drain_new_activations(uint16_t* out, uint8_t max) {
    DtcGuard g;
    uint8_t n = 0;
    for (uint8_t i = 0; i < pending_n_ && n < max; i++) out[n++] = pending_[i];
    pending_n_ = 0;   // drained (caller sizes out[] to PENDING_MAX; surplus is dropped)
    return n;
}

void DtcManager::set_freeze_frame(uint16_t code, const float* ff, uint8_t n) {
    if (!ff || !maybe_present(code)) return;
    DtcGuard g;
    const int i = find(code);
    if (i < 0) return;
    if (n > DTC_FF_CHANNELS) n = DTC_FF_CHANNELS;
    for (uint8_t k = 0; k < n; k++) table_[i].ff[k] = ff[k];
    dirty_ = true;   // the persisted image changed
}

void DtcManager::age(uint32_t now_ms) {
    // SignalBus::expire, for faults. A judge that has stopped asserting cannot leave one standing:
    // whatever the reason it stopped — its check unticked, its module switched off, its condition no
    // longer evaluated — the code drops out of ACTIVE and the history keeps it.
    DtcGuard g;
    for (uint8_t i = 0; i < SLOTS; i++) {
        DtcRecord& r = table_[i];
        if (!r.code || !(r.status & DTC_ACTIVE) || ttl_[i] == 0) continue;
        if ((now_ms - r.last_ms) <= ttl_[i]) continue;
        r.status &= static_cast<uint8_t>(~DTC_ACTIVE);   // keep STORED history, exactly as heal() does
        if (aged_ < 0xFFFFu) ++aged_;
        dirty_ = true;
    }
}

void DtcManager::heal(uint16_t code) {
    if (!maybe_present(code)) return;    // the common case, and no lock for it
    DtcGuard g;
    const int i = find(code);
    if (i >= 0 && (table_[i].status & DTC_ACTIVE)) {
        table_[i].status &= static_cast<uint8_t>(~DTC_ACTIVE);  // keep STORED history
        dirty_ = true;
    }
}

void DtcManager::set_active(bool a) noexcept {
    // A KEY CYCLE opens on the first key-on frame — including the first frame after boot, which is why
    // this is not keyed off active_ (that starts true so boot-time raises are not suppressed).
    if (a && !cycle_open_) { cycle_open_ = true; close_cycle(); }
    if (!a) cycle_open_ = false;
    if (a == active_) return;
    active_ = a;
    if (a) return;
    DtcGuard g;
    // Inactive edge (key-off / USB): heal every ACTIVE runtime fault — drop ACTIVE, keep STORED
    // history — so the bench active list is clean. Config/validity codes (pin conflict, tune
    // validity) stay live: those are exactly what you want flagged while tuning.
    for (uint8_t i = 0; i < SLOTS; i++) {
        DtcRecord& r = table_[i];
        if ((r.status & DTC_ACTIVE) && !dtc_is_config_source(r.source)) {
            r.status &= static_cast<uint8_t>(~DTC_ACTIVE);
            dirty_ = true;
        }
    }
}

// The previous key cycle is over. Every stored code that did NOT go active in it had a clean cycle:
// count it, drop CONFIRMED after DTC_UNCONFIRM_CYCLES, erase the record after DTC_ERASE_CYCLES. A code
// that did go active starts again from zero (raise() already zeroed it). Then open the new cycle.
//
// This was half built: heal_cycles and DTC_CONFIRMED were in the record and on the card, and nothing
// ever set either, so a code fixed a year ago sat in the table for ever and "confirmed" meant nothing.
void DtcManager::close_cycle() {
    DtcGuard g;
    for (uint8_t i = 0; i < SLOTS; i++) {
        DtcRecord& r = table_[i];
        if (!r.code) continue;
        if (r.status & DTC_CYCLE_ACT) {
            r.status &= static_cast<uint8_t>(~DTC_CYCLE_ACT);
            dirty_ = true;
            continue;
        }
        if (r.status & DTC_ACTIVE) continue;           // standing right now: not clean
        if (r.heal_cycles < 0xFFu) r.heal_cycles++;
        if (r.heal_cycles >= DTC_UNCONFIRM_CYCLES) r.status &= static_cast<uint8_t>(~DTC_CONFIRMED);
        dirty_ = true;
        if (r.heal_cycles >= DTC_ERASE_CYCLES) erase_slot(i);
    }
}

void DtcManager::erase_slot(uint8_t i) {
    table_[i] = {};
    ttl_[i] = 0;
    active_since_[i] = 0;
    dirty_ = true;
}

bool DtcManager::consume_dirty() {
    DtcGuard g;
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

uint32_t DtcManager::snapshot_if_dirty(uint8_t* buf, uint32_t buf_len) {
    DtcGuard g;
    if (!dirty_) return 0;
    const uint32_t n = serialize(buf, buf_len);
    if (n) dirty_ = false;
    return n;
}

bool DtcManager::clear(uint16_t code) {
    DtcGuard g;
    const int i = find(code);
    if (i < 0) return false;
    erase_slot(static_cast<uint8_t>(i));
    return true;
}

void DtcManager::clear_all() {
    DtcGuard g;
    for (uint8_t i = 0; i < SLOTS; i++) { table_[i] = {}; ttl_[i] = 0; active_since_[i] = 0; }
    for (uint8_t i = 0; i < 8; i++) present_[i] = 0;   // the table is empty: so is the filter
    dirty_ = true;
    clear_gen_++;   // signal edge-triggered producers to re-assert still-active faults
}

uint8_t DtcManager::active_count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < SLOTS; i++)
        if (table_[i].code && (table_[i].status & DTC_ACTIVE)) n++;
    return n;
}

uint8_t DtcManager::stored_count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < SLOTS; i++) if (table_[i].code) n++;
    return n;
}

uint8_t DtcManager::worst_severity() const {
    uint8_t w = 0;
    for (uint8_t i = 0; i < SLOTS; i++)
        if (table_[i].code && (table_[i].status & DTC_ACTIVE) && table_[i].severity > w)
            w = table_[i].severity;
    return w;
}

uint8_t DtcManager::worst_stored_severity() const {
    uint8_t w = 0;
    for (uint8_t i = 0; i < SLOTS; i++)
        if (table_[i].code && (table_[i].status & DTC_STORED) && table_[i].severity > w)
            w = table_[i].severity;
    return w;
}

uint8_t DtcManager::source_severity(uint8_t source) const {
    uint8_t w = 0;
    for (uint8_t i = 0; i < SLOTS; i++)
        if (table_[i].code && table_[i].source == source &&
            (table_[i].status & DTC_ACTIVE) && table_[i].severity > w)
            w = table_[i].severity;
    return w;
}

uint8_t DtcManager::code_severity(uint16_t code) const {
    const int i = find(code);
    return (i >= 0 && (table_[i].status & DTC_ACTIVE)) ? table_[i].severity : 0;
}


uint16_t DtcManager::worst_code(uint8_t lo_sev, uint8_t hi_sev) const {
    DtcGuard g;
    uint16_t code = 0;
    uint8_t  best = 0;
    for (uint8_t i = 0; i < SLOTS; i++) {
        const DtcRecord& r = table_[i];
        if (r.code && (r.status & DTC_ACTIVE) &&
            r.severity >= lo_sev && r.severity <= hi_sev && r.severity >= best) {
            best = r.severity; code = r.code;
        }
    }
    return code;
}

uint8_t DtcManager::list_active_band(uint8_t lo_sev, uint8_t hi_sev,
                                     uint16_t* out, uint8_t max) const {
    DtcGuard g;
    uint8_t n = 0;
    for (uint8_t i = 0; i < SLOTS && n < max; i++) {
        const DtcRecord& r = table_[i];
        if (r.code && (r.status & DTC_ACTIVE) && r.severity >= lo_sev && r.severity <= hi_sev)
            out[n++] = r.code;
    }
    return n;
}

uint8_t DtcManager::list_active(uint16_t* out, uint8_t max) const {
    DtcGuard g;
    uint8_t n = 0;
    for (uint8_t i = 0; i < SLOTS && n < max; i++)
        if (table_[i].code && (table_[i].status & DTC_ACTIVE)) out[n++] = table_[i].code;
    return n;
}

uint32_t DtcManager::serialize(uint8_t* buf, uint32_t buf_len, uint16_t first) const {
    DtcGuard g;
    if (buf_len < 10u) return 0;                       // not even the header fits
    const uint32_t cap = records_that_fit(buf_len);    // how many records this buffer can hold
    uint32_t o = 10;                                   // header written last (n isn't known until then)
    uint16_t n = 0, seen = 0;
    for (uint8_t i = 0; i < SLOTS && n < cap; i++) {
        if (!table_[i].code) continue;                 // empty slot
        if (seen++ < first) continue;                  // page offset, counted over STORED records
        std::memcpy(buf + o, &table_[i], sizeof(DtcRecord));
        o += sizeof(DtcRecord);
        n++;
    }
    uint32_t h = 0;
    std::memcpy(buf + h, &DTC_MAGIC, 4);   h += 4;
    std::memcpy(buf + h, &DTC_VERSION, 2); h += 2;
    std::memcpy(buf + h, &boot_id_, 2);    h += 2;
    std::memcpy(buf + h, &n, 2);                       // records IN THIS IMAGE (see the header comment)
    return o;
}

bool DtcManager::restore(const uint8_t* buf, uint32_t len) {
    if (len < 10) return false;
    uint32_t magic; uint16_t ver, boot, n;
    std::memcpy(&magic, buf + 0, 4);
    std::memcpy(&ver,   buf + 4, 2);
    std::memcpy(&boot,  buf + 6, 2);
    std::memcpy(&n,     buf + 8, 2);
    if (magic != DTC_MAGIC || ver != DTC_VERSION || n > SLOTS) return false;
    if (len < 10u + static_cast<uint32_t>(sizeof(DtcRecord)) * n) return false;
    DtcGuard g;
    clear_all();
    uint32_t o = 10;
    for (uint16_t k = 0; k < n; k++) {
        DtcRecord r{};
        std::memcpy(&r, buf + o, sizeof(DtcRecord));
        o += sizeof(DtcRecord);
        // Restored codes come back STORED but NOT active — they re-arm to active
        // only if their condition is still true after boot (the source raises them).
        r.status = static_cast<uint8_t>(r.status & ~DTC_ACTIVE);
        if (r.code && k < SLOTS) {
            table_[k] = r;
            present_[(r.code >> 5) & 7u] |= (1u << (r.code & 31u));   // restored codes are present too
        }
    }
    dirty_ = false;   // just loaded from SD — already in sync, don't rewrite it
    return true;
}

uint16_t DtcManager::image_boot_id(const uint8_t* buf, uint32_t len) {
    if (len < 10) return 0;
    uint32_t magic; uint16_t ver, boot;
    std::memcpy(&magic, buf + 0, 4);
    std::memcpy(&ver,   buf + 4, 2);
    std::memcpy(&boot,  buf + 6, 2);
    if (magic != DTC_MAGIC || ver != DTC_VERSION) return 0;
    return boot;
}

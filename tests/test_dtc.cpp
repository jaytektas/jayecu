// Host test for the unified DTC table (DtcManager) — raise/dedupe/count, heal,
// query, SD save/restore round-trip, clear, and full-table eviction.
#include "Diagnostics/DtcManager.h"
#include "dtc_categories.h"   // dtc_indicator_bit(code) — P-code -> category bit
#include <cassert>
#include <cstdio>
#include <cstdint>

static int _p = 0, _f = 0;
static void check(bool c, const char* m) {
    if (c) { _p++; printf("ok  %s\n", m); }
    else   { _f++; printf("FAIL %s\n", m); }
}

int main() {
    constexpr uint16_t P0117 = 0x0117;   // coolant circuit low
    constexpr uint16_t P0118 = 0x0118;   // coolant circuit high
    constexpr uint8_t  CLT_SRC = 3;      // a sensor source id

    DtcManager d;
    d.init(/*boot_id*/ 7);

    // ---- raise: appears, counts the activation edge ----
    d.raise(P0117, CLT_SRC, DTC_SEV_LEVEL2, 1000);
    check(d.active_count() == 1 && d.stored_count() == 1, "raise: one active, one stored");
    check(d.slot(0).count == 1, "raise: count = 1 on first activation");
    check(d.worst_severity() == DTC_SEV_LEVEL2, "raise: worst severity reported");
    check(d.source_severity(CLT_SRC) == DTC_SEV_LEVEL2, "raise: per-source severity");

    // ---- age: a code its judge stopped asserting leaves CURRENT, and keeps its history ----
    // THE POINT OF THE WHOLE MECHANISM. A judge can stop for reasons it never gets to report: its check
    // is unticked, its module is switched off, the condition is no longer evaluated at all. The table is
    // the only place that can notice, which is why this is a ttl and not nineteen heal-on-disable blocks.
    {
        DtcManager t; t.init(3);
        t.raise(P0118, CLT_SRC, DTC_SEV_LEVEL1, 1000, /*ttl*/ 500);
        check(t.active_count() == 1, "age: raised with a ttl is current");
        t.age(1400);
        check(t.active_count() == 1, "age: still current inside its ttl");
        t.raise(P0118, CLT_SRC, DTC_SEV_LEVEL1, 1400, 500);   // the judge says so again
        t.age(1800);
        check(t.active_count() == 1, "age: re-asserting refreshes it");
        t.age(2100);                                        // 700 ms since the last assert
        check(t.active_count() == 0, "age: nobody asserting -> no longer current");
        check(t.stored_count() == 1, "age: …and the history is kept, exactly as heal() leaves it");
        check(t.slot(0).count == 1, "age: ageing out is not a new activation");
    }

    // ---- latching: an EVENT nobody re-asserts must NOT age out ----
    {
        DtcManager t; t.init(3);
        t.raise(P0117, CLT_SRC, DTC_SEV_LEVEL3, 1000, DTC_TTL_LATCH);
        t.age(1000 + DTC_TTL_DEFAULT * 10);
        check(t.active_count() == 1, "latch: a pre-ignition/misfire/Lua code stands until healed");
        t.heal(P0117);
        check(t.active_count() == 0, "latch: …and heal still retires it");
    }

    // ---- raise again while still active: refresh, no double count ----
    d.raise(P0117, CLT_SRC, DTC_SEV_LEVEL2, 1100);
    check(d.slot(0).count == 1 && d.active_count() == 1, "re-raise while active: count stays 1");

    // ---- heal: drops active, keeps stored history (like a MIL) ----
    d.heal(P0117);
    check(d.active_count() == 0 && d.stored_count() == 1, "heal: not active, still stored");
    check(d.worst_severity() == 0, "heal: nothing active → worst severity 0");

    // ---- re-raise after heal: new activation edge → count 2 ----
    d.raise(P0117, CLT_SRC, DTC_SEV_LEVEL2, 2000);
    check(d.slot(0).count == 2, "re-raise after heal: count = 2");

    // ---- a second, distinct code takes a second slot ----
    d.raise(P0118, CLT_SRC, DTC_SEV_LEVEL3, 2100);
    check(d.stored_count() == 2 && d.active_count() == 2, "second distinct code → second slot");
    check(d.worst_severity() == DTC_SEV_LEVEL3, "worst severity tracks the most severe active");
    uint16_t act[8]; const uint8_t n = d.list_active(act, 8);
    check(n == 2, "list_active returns both active codes");

    // severity-band lists — what each LED cycles (orange = 1-2, red = 3)
    uint16_t band[8];
    check(d.list_active_band(1, 2, band, 8) == 1 && band[0] == P0117, "band 1-2 lists the level 2 code (orange cycle)");
    check(d.list_active_band(3, 3, band, 8) == 1 && band[0] == P0118, "band 3 lists the level 3 code (red cycle)");
    check(d.worst_code(3, 3) == P0118, "worst_code(3) still picks a level 3 code");

    // ---- save → restore round-trip (the SD image) ----
    uint8_t img[DtcManager::image_max_bytes()];
    const uint32_t wrote = d.serialize(img, sizeof(img));
    check(wrote > 0, "serialize produced an SD image");
    DtcManager d2; d2.init(8);
    check(d2.restore(img, wrote), "restore accepts a valid image");
    check(d2.stored_count() == 2, "restore: both stored codes came back");
    check(d2.active_count() == 0, "restore: codes return STORED but NOT active (re-arm on condition)");
    check(d2.slot(0).count == 2, "restore: per-code count preserved");

    // A restored code re-raised by a DIFFERENT source takes the raiser's source, not the one it was saved
    // under. Source ids are not frozen (the subsystem ids moved from 91.. to 200..), and a restored config
    // code that kept its stale id was judged by it — healed at key-off like a runtime fault.
    d2.raise(P0117, DtcSource::CONFIG, DTC_SEV_LEVEL2, 3000);
    check(d2.source_severity(DtcSource::CONFIG) == DTC_SEV_LEVEL2, "re-raise adopts the raiser's source");
    check(d2.source_severity(CLT_SRC) == 0, "…and drops the source it was stored under");
    d2.heal(P0117);

    // restore rejects garbage
    uint8_t bad[16] = { 0xDE, 0xAD };
    check(!d2.restore(bad, sizeof(bad)), "restore rejects a bad/short image");

    // ---- boot_id persistence: peek the stored id out of the image, bump it ----
    check(DtcManager::image_boot_id(img, wrote) == 7, "image_boot_id reads the stored boot_id (7)");
    check(DtcManager::image_boot_id(bad, sizeof(bad)) == 0, "image_boot_id returns 0 on a bad header");
    // The store's boot path: next session id = stored + 1.
    DtcManager d3; d3.init(static_cast<uint16_t>(DtcManager::image_boot_id(img, wrote) + 1));
    check(d3.boot_id() == 8, "boot path: init with stored boot_id + 1");

    // ---- dirty flag: drives the SD writer (only persist on change) ----
    DtcManager f; f.init(1);
    check(!f.consume_dirty(), "fresh table is not dirty");
    f.raise(P0117, CLT_SRC, DTC_SEV_LEVEL1, 1);
    check(f.consume_dirty(), "raise (inactive->active edge) sets dirty");
    check(!f.consume_dirty(), "consume_dirty is clear-on-read");
    f.raise(P0117, CLT_SRC, DTC_SEV_LEVEL1, 2);
    check(!f.consume_dirty(), "re-raise while active does NOT dirty (no per-frame churn)");
    f.heal(P0117);
    check(f.consume_dirty(), "heal sets dirty");
    f.clear_all();
    check(f.consume_dirty(), "clear_all sets dirty (Mode 04 persists the empty table)");
    // restore leaves the table in sync with SD → not dirty (no immediate rewrite).
    f.restore(img, wrote);
    check(!f.consume_dirty(), "restore leaves the table not-dirty (already in sync with SD)");

    // ---- clear one, clear all (OBD Mode 04) ----
    check(d.clear(P0117) && d.stored_count() == 1, "clear(code) removes one stored code");
    check(!d.clear(0x9999), "clear of an absent code returns false");
    d.clear_all();
    check(d.stored_count() == 0, "clear_all wipes the table");

    // ---- eviction: fill past 64 with healed codes; a new one reuses a healed slot ----
    DtcManager e; e.init(1);
    for (uint16_t k = 0; k < DtcManager::SLOTS; k++) { e.raise(0x1000 + k, CLT_SRC, DTC_SEV_LEVEL1, k); e.heal(0x1000 + k); }
    check(e.stored_count() == DtcManager::SLOTS, "table filled to capacity (all healed)");
    e.raise(0x2000, CLT_SRC, DTC_SEV_LEVEL3, 9999);   // one more → evicts an oldest healed slot
    check(e.stored_count() == DtcManager::SLOTS, "over-capacity stays at SLOTS (evicted a healed code)");
    check(e.source_severity(CLT_SRC) == DTC_SEV_LEVEL3, "the new active code is present after eviction");

    // ---- DTC category indicator mapping (schema dtc_indicators) ----
    // Real sensor + protection codes must bucket into the right category bit.
    check(dtc_indicator_bit(0x0117) == 0,  "CLT circuit P0117 -> coolant (bit 0)");
    check(dtc_indicator_bit(0x0217) == 0,  "overtemp P0217 -> coolant (bit 0)");
    check(dtc_indicator_bit(0x1217) == 0,  "mfr coolant warning P1217 -> coolant (bit 0)");
    check(dtc_indicator_bit(0x0112) == 1,  "IAT circuit P0112 -> iat (bit 1)");
    check(dtc_indicator_bit(0x1098) == 1,  "mfr intake over-temp P1098 -> iat (bit 1)");
    check(dtc_indicator_bit(0x0107) == 2,  "MAP P0107 -> map_boost (bit 2)");
    check(dtc_indicator_bit(0x0234) == 2,  "overboost P0234 -> map_boost (bit 2)");
    check(dtc_indicator_bit(0x0122) == 4,  "TPS P0122 -> throttle (bit 4)");
    check(dtc_indicator_bit(0x2237) == 5,  "lambda P2237 -> o2_mixture (bit 5)");
    check(dtc_indicator_bit(0x0192) == 6,  "fuel pressure P0192 -> fuel_press (bit 6)");
    check(dtc_indicator_bit(0x0335) == 10, "sync loss P0335 -> trigger (bit 10)");
    check(dtc_indicator_bit(0x0562) == 14, "battery low P0562 -> battery (bit 14)");
    check(dtc_indicator_bit(0x0452) == 16, "fuel tank pressure P0452 -> emissions (bit 16)");
    check(dtc_indicator_bit(0x1650) == 19, "pin conflict P1650 -> pin_conflict (bit 19)");
    check(dtc_indicator_bit(0x0700) <  0,  "transmission P0700 -> uncategorised (-1)");

    // The Diagnostics OR-loop: a coolant + a trigger code => bits 0 and 10 set, others clear.
    {
        DtcManager m; m.init(1);
        m.raise(0x0217, CLT_SRC, DTC_SEV_LEVEL3, 1);   // coolant
        m.raise(0x0335, 92, DTC_SEV_LEVEL3, 1);        // trigger (source PROTECTION)
        uint32_t mask = 0;
        for (uint8_t i = 0; i < DtcManager::SLOTS; i++) {
            const DtcRecord& r = m.slot(i);
            if (r.code && (r.status & DTC_ACTIVE)) {
                const int b = dtc_indicator_bit(r.code);
                if (b >= 0) mask |= (1u << b);
            }
        }
        check(mask == ((1u << 0) | (1u << 10)), "category mask = coolant|trigger bits only");
        m.heal(0x0335);                              // trigger clears -> only coolant remains
        mask = 0;
        for (uint8_t i = 0; i < DtcManager::SLOTS; i++) {
            const DtcRecord& r = m.slot(i);
            if (r.code && (r.status & DTC_ACTIVE)) {
                const int b = dtc_indicator_bit(r.code);
                if (b >= 0) mask |= (1u << b);
            }
        }
        check(mask == (1u << 0), "healing the trigger code drops its category bit");
    }

    // ---- raise() edge return + new-activation drain (freeze-frame log feed) ----
    {
        DtcManager q; q.init(1);
        check(q.raise(0x0117, CLT_SRC, DTC_SEV_LEVEL2, 1) == true,  "raise returns true on the new-activation edge");
        check(q.raise(0x0117, CLT_SRC, DTC_SEV_LEVEL2, 2) == false, "re-raise while active returns false (no edge)");
        check(q.raise(0x0107, CLT_SRC, DTC_SEV_LEVEL3, 3) == true,   "a second new code is another edge");

        uint16_t nc[DtcManager::PENDING_MAX];
        uint8_t k = q.drain_new_activations(nc, DtcManager::PENDING_MAX);
        check(k == 2, "drain returns both new activations");
        check(nc[0] == 0x0117 && nc[1] == 0x0107, "drain preserves activation order");
        check(q.drain_new_activations(nc, DtcManager::PENDING_MAX) == 0, "drain is clear-on-read");

        q.heal(0x0117);
        check(q.raise(0x0117, CLT_SRC, DTC_SEV_LEVEL2, 5) == true, "re-raise after heal is a fresh edge");
        check(q.drain_new_activations(nc, DtcManager::PENDING_MAX) == 1, "the re-armed code queues again");
    }

    // ---- per-code freeze-frame: stored on the record, survives serialize/restore ----
    {
        DtcManager z; z.init(1);
        z.raise(0x0217, 92, DTC_SEV_LEVEL3, 100);          // overtemp -> slot 0
        const float ff[DTC_FF_CHANNELS] = { 3500.0f, 240.5f, 105.0f, 13.8f };
        z.set_freeze_frame(0x0217, ff, DTC_FF_CHANNELS);
        check(z.slot(0).code == 0x0217, "ff: code landed in slot 0");
        check(z.slot(0).ff[0] == 3500.0f && z.slot(0).ff[3] == 13.8f,
              "set_freeze_frame stores the snapshot on the record");

        uint8_t fimg[DtcManager::image_max_bytes()];
        const uint32_t fw = z.serialize(fimg, sizeof(fimg));
        DtcManager z2; z2.init(1);
        check(z2.restore(fimg, fw), "restore image carrying freeze-frames");
        check(z2.slot(0).ff[0] == 3500.0f && z2.slot(0).ff[2] == 105.0f,
              "freeze-frame survives serialize/restore (persisted in dtc.bin)");

        z.set_freeze_frame(0x9999, ff, DTC_FF_CHANNELS); // absent code -> no-op, no crash
        check(z.stored_count() == 1, "set_freeze_frame on an absent code is a no-op");
    }

    // ---- system-active (key-on) gate: runtime DTCs off on USB, config/validity still raise ----
    {
        constexpr uint16_t P1650 = 0x1650;   // pin conflict (config/validity)
        DtcManager g; g.init(1);
        g.raise(P0117, CLT_SRC, DTC_SEV_LEVEL2, 100);                 // sensor fault while active
        g.raise(P1650, DtcSource::PIN_ARBITER, DTC_SEV_LEVEL1, 100);  // config fault while active
        check(g.active_count() == 2, "gate: both active while system-active");

        g.set_active(false);                                        // USB/bench
        check(g.active_count() == 1, "gate: inactive edge heals the runtime fault, keeps config");
        g.raise(P0118, CLT_SRC, DTC_SEV_LEVEL3, 200);                  // runtime raise on USB -> ignored
        check(g.active_count() == 1, "gate: runtime raise suppressed on USB");
        g.raise(P1650, DtcSource::PIN_ARBITER, DTC_SEV_LEVEL1, 200);  // config still flags a bad tune
        check(g.active_count() == 1, "gate: config/validity still raises on USB");

        g.set_active(true);                                         // key-on: runtime returns
        g.raise(P0118, CLT_SRC, DTC_SEV_LEVEL3, 300);
        check(g.active_count() == 2, "gate: runtime raises again once key-on");
    }

    // ---- severity-aware eviction when the table is full of ACTIVE codes ----
    // Config faults ("enabled but not wired") stay ACTIVE for as long as the misconfiguration does, and
    // that is deliberate — the user must see what isn't working. But a rig mid-bring-up can hold dozens
    // of them, and before this a saturated table dropped EVERY later code, so a level 3 arriving after the
    // flood vanished with no record. A level 3 must be able to displace a level 1; never the reverse.
    {
        DtcManager s1; s1.init(1); s1.set_active(true);
        for (uint16_t k = 0; k < DtcManager::SLOTS; k++) s1.raise(0x1000 + k, 98, DTC_SEV_LEVEL1, 100 + k);
        check(s1.active_count() == DtcManager::SLOTS, "evict: saturated with active level 1 codes");
        s1.raise(0x2135, 91, DTC_SEV_LEVEL3, 9999);
        check(s1.worst_code(DTC_SEV_LEVEL1, DTC_SEV_LEVEL3) == 0x2135, "evict: a level 3 displaces a level 1 when full");

        DtcManager s2; s2.init(1); s2.set_active(true);
        for (uint16_t k = 0; k < DtcManager::SLOTS; k++) s2.raise(0x3000 + k, 91, DTC_SEV_LEVEL3, 100 + k);
        s2.raise(0x0117, 3, DTC_SEV_LEVEL1, 9999);
        uint16_t out2[DtcManager::SLOTS]; const uint8_t n2 = s2.list_active(out2, DtcManager::SLOTS);
        bool level1_in = false; for (uint8_t i = 0; i < n2; i++) if (out2[i] == 0x0117) level1_in = true;
        check(!level1_in, "evict: a level 1 NEVER displaces a level 3");

        DtcManager s3; s3.init(1); s3.set_active(true);
        for (uint16_t k = 0; k < DtcManager::SLOTS; k++) s3.raise(0x4000 + k, 91, DTC_SEV_LEVEL2, 100 + k);
        s3.raise(0x5555, 91, DTC_SEV_LEVEL2, 9999);
        uint16_t out3[DtcManager::SLOTS]; const uint8_t n3 = s3.list_active(out3, DtcManager::SLOTS);
        bool peer_in = false; for (uint8_t i = 0; i < n3; i++) if (out3[i] == 0x5555) peer_in = true;
        check(!peer_in, "evict: equal severity does not displace a peer (no churn)");
    }

    // ---- S17: confirm + clean-cycle aging (key cycles, not boots) ----
    {
        constexpr uint16_t C = 0x0300;
        auto status = [](DtcManager& m, uint16_t code) -> uint8_t {
            for (uint8_t i = 0; i < DtcManager::SLOTS; i++) if (m.slot(i).code == code) return m.slot(i).status;
            return 0;
        };
        auto heal_n = [](DtcManager& m, uint16_t code) -> int {
            for (uint8_t i = 0; i < DtcManager::SLOTS; i++) if (m.slot(i).code == code) return m.slot(i).heal_cycles;
            return -1;
        };
        auto key_cycle = [](DtcManager& m) { m.set_active(false); m.set_active(true); };

        DtcManager m; m.init(1); m.set_active(true);            // key cycle 1 opens
        m.raise(C, 5, DTC_SEV_LEVEL1, 1000, 500);
        check(!(status(m, C) & DTC_CONFIRMED), "confirm: a first brief fault is only pending");
        m.heal(C);
        key_cycle(m);                                            // cycle 2: cycle 1 was not clean
        check(heal_n(m, C) == 0, "age: the cycle it failed in is not a clean one");
        m.raise(C, 5, DTC_SEV_LEVEL1, 5000, 500);
        check(status(m, C) & DTC_CONFIRMED, "confirm: failing again in a later key cycle confirms");
        m.heal(C);

        DtcManager h; h.init(1); h.set_active(true);
        h.raise(C, 5, DTC_SEV_LEVEL1, 1000, 500);
        h.raise(C, 5, DTC_SEV_LEVEL1, 1000 + DTC_CONFIRM_MS, 500);
        check(status(h, C) & DTC_CONFIRMED, "confirm: held active DTC_CONFIRM_MS confirms");

        DtcManager e; e.init(1); e.set_active(true);
        e.raise(C, 5, DTC_SEV_LEVEL1, 1000, DTC_TTL_LATCH);
        check(status(e, C) & DTC_CONFIRMED, "confirm: an event (latched) is confirmed at once");

        key_cycle(m);                                            // cycle 3: cycle 2 failed
        for (int k = 0; k < DTC_UNCONFIRM_CYCLES; k++) key_cycle(m);
        check(heal_n(m, C) == DTC_UNCONFIRM_CYCLES, "age: clean key cycles are counted");
        check(!(status(m, C) & DTC_CONFIRMED) && (status(m, C) & DTC_STORED),
              "age: confirmed drops after the unconfirm cycles, history stays");

        uint8_t img[DtcManager::image_max_bytes()];
        const uint32_t n = m.serialize(img, sizeof(img));
        DtcManager r; check(r.restore(img, n), "age: heal_cycles survive a save");
        check(heal_n(r, C) == DTC_UNCONFIRM_CYCLES, "age: heal_cycles restored");
        r.set_active(false); r.set_active(false);                // USB/bench power-ups are not cycles
        check(heal_n(r, C) == DTC_UNCONFIRM_CYCLES, "age: bench (inactive) sessions do not count");
        for (int k = DTC_UNCONFIRM_CYCLES; k < DTC_ERASE_CYCLES; k++) key_cycle(r);
        check(status(r, C) == 0, "age: erased after DTC_ERASE_CYCLES clean cycles");

        DtcManager s; s.init(1); s.set_active(true);
        s.raise(C, 5, DTC_SEV_LEVEL1, 1000, 500);
        uint8_t im2[DtcManager::image_max_bytes()];
        check(s.snapshot_if_dirty(im2, sizeof(im2)) > 0, "save: a change is snapshotted");
        check(s.snapshot_if_dirty(im2, sizeof(im2)) == 0, "save: and only once");
        s.mark_dirty();
        check(s.snapshot_if_dirty(im2, sizeof(im2)) > 0, "save: a failed save stays owed");
    }

    printf("\n%d passed, %d failed\n", _p, _f);
    return _f ? 1 : 0;
}

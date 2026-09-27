#include "test_helpers.h"
#include "TriggerConfigCheck.h"
#include <cstring>
static StreamsConfig mk(uint8_t en, uint8_t prim, uint8_t cell_len, uint8_t phased,
                        int16_t c0 = 0, int16_t c1 = 0, uint8_t repeats = 0) {
    StreamsConfig s{}; s.enabled = en; s.primitive = prim; s.cell_len = cell_len;
    s.phased = phased; s.repeats = repeats;
    // Fill EVERY live cell: leaving the tail at zero makes a "uniform" pattern asymmetric by
    // accident, which is a way to write a test that passes for the wrong reason.
    for (uint8_t k = 0; k < cell_len && k < MAX_PATTERN_TEETH; ++k) s.cell[k].v = (k == 0) ? c0 : c1;
    return s;
}
int main() {
    fprintf(stdout, "=== trigger config: what can this ever know? ===\n");
    StreamsConfig st[MAX_STREAMS];

    SECTION("a 60-2 alone reaches CRANK — it locates itself, nothing tells it the cycle");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,1,0,0);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::CRANK); CHECK(r.can_fire()); }

    SECTION("60-2 plus a fixed cam reaches PHASE");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,1,0,0); st[2] = mk(1,2,0,0);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::PHASE); }

    SECTION("60-2 plus a PHASED cam still reaches PHASE — a phaser can say which revolution");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,1,0,0); st[2] = mk(1,2,0,1);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::PHASE);   // it cannot ANCHOR, but the crank already does that
      CHECK(r.fault == TriggerConfigFault::NONE); }

    SECTION("an even crank ALONE can never know where it is — refused");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::NONE); CHECK(!r.can_fire());
      CHECK(r.fault == TriggerConfigFault::NO_ANCHOR); }

    SECTION("an even crank plus a FIXED cam reaches PHASE — the cam is its only reference");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[2] = mk(1,2,0,0);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::PHASE); }

    SECTION("an even crank whose only reference is a PHASER is refused");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[2] = mk(1,2,0,1);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::NONE);
      CHECK(r.fault == TriggerConfigFault::ONLY_PHASED_ANCHOR); }

    SECTION("a Motronic flywheel — relative ring gear plus a TDC mark on the SECOND crank slot");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0);      // ring gear: even, relative
      st[1] = mk(1,2,0,0);                                    // single reference pulse, crank rate
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      // The anchor is the OTHER CRANK STREAM, not a cam. An earlier draft of the rule refused this
      // outright, which would have turned away a Porsche 944.
      CHECK(r.ceiling == SyncLvl::CRANK); CHECK(r.can_fire()); }

    SECTION("GM 3800 — high-res even track plus a low-res asymmetric one");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0);            // 18X/24X: even, fine, relative
      st[1] = mk(1,1,3,0, 500,600);                                 // 3X: asymmetric sequence
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::CRANK); }

    SECTION("a UNIFORM sequence is an even wheel written the long way round");
    { std::memset(st,0,sizeof st); st[0] = mk(1,1,3,0, 1200,1200);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::NONE); }

    SECTION("cam-only (a CAS) reaches PHASE on its own");
    { std::memset(st,0,sizeof st); st[2] = mk(1,1,3,0, 2400,2700);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.ceiling == SyncLvl::PHASE); }

    SECTION("nothing enabled is its own fault, not a silent NONE");
    { std::memset(st,0,sizeof st);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720);
      CHECK(r.fault == TriggerConfigFault::NO_STREAMS); }

    // ---- SYNC ALWAYS: one even wheel in distributor mode --------------------------------------
    auto shape = [](uint8_t ign, uint8_t ncyl, bool even = true, bool rotary = false) {
        TriggerEngineShape e; e.known = true; e.ign_mode = ign; e.ncyl = ncyl;
        e.even_fire = even; e.rotary = rotary; return e;
    };
    const uint8_t DIST = static_cast<uint8_t>(IgnitionCoilMode::SINGLE_COIL_DISTRIBUTOR);
    const uint8_t COP  = static_cast<uint8_t>(IgnitionCoilMode::COIL_ON_PLUG);

    SECTION("4cyl dizzy (2 teeth per rev), distributor mode: sync always, full sync");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 2;
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 4));
      CHECK(r.sync_always); CHECK(r.ceiling == SyncLvl::PHASE); CHECK(r.can_fire()); }

    SECTION("8cyl dizzy on the cam (8 per cycle), distributor mode: sync always");
    { std::memset(st,0,sizeof st); st[2] = mk(1,0,0,0); st[2].slots = 8;
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 8));
      CHECK(r.sync_always); CHECK(r.ceiling == SyncLvl::PHASE); }

    SECTION("the same dizzy with coil-on-plug is refused, and says why");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 2;
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(COP, 4));
      CHECK(!r.sync_always); CHECK(r.ceiling == SyncLvl::NONE);
      CHECK(r.fault == TriggerConfigFault::EVEN_WHEEL_COILS); }

    SECTION("a 36-tooth even wheel in distributor mode on a 4-cyl: teeth are not all alike -> refused");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 36;
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 4));
      CHECK(!r.sync_always); CHECK(r.fault == TriggerConfigFault::EVEN_WHEEL_COILS); }

    SECTION("a dizzy with a SEQUENTIAL (or semi-sequential, or bank) stage is refused");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 2;
      auto e = shape(DIST, 4); e.cyl_timed_injection = true;
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, e);
      CHECK(!r.sync_always); CHECK(r.ceiling == SyncLvl::NONE);
      CHECK(r.fault == TriggerConfigFault::EVEN_WHEEL_INJECTION); }

    SECTION("odd-fire or rotary never syncs always");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 2;
      CHECK(!check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 4, false)).sync_always);
      CHECK(!check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 4, true, true)).sync_always); }

    SECTION("an even wheel WITH a fixed cam is not sync-always — the cam anchors it (full sync)");
    { std::memset(st,0,sizeof st); st[0] = mk(1,0,0,0); st[0].slots = 36; st[2] = mk(1,2,0,0);
      const auto r = check_trigger_config(st, MAX_STREAMS, ANGLE_720, shape(DIST, 4));
      CHECK(!r.sync_always); CHECK(r.ceiling == SyncLvl::PHASE); }

    return test_summary();
}

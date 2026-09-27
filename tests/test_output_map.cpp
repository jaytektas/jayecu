// THE OUTPUT ROWS AS THE SCHEDULER SEES THEM.
//
// outputs.output[i] IS pin i. A coil or injector row names ONE cylinder value (a cylinder, All, a bank),
// written by the studio; the firmware allocates nothing. Two things are pinned here:
//
//   resolution       — what each value names: a cylinder, a rotor's faces, every cylinder, a bank; and the
//                      wasted-spark companion, found from the firing order so a changed order moves no row.
//   binding          — the scheduler claims and drives exactly the pins the rows name, at each row's own
//                      polarity; a row in a stage that is switched off claims nothing; a per-cylinder cut
//                      reaches every row that names the cylinder.
//
//   build: tests/CMakeLists.txt -> ctest -R output_map
#include "test_helpers.h"
#include "Scheduler/EventScheduler.h"
#include "Scheduler/OutputMap.h"
#include "output_rows_helper.h"

#include <cstring>
#include <vector>

namespace {

uint32_t g_now = 0;

// Four-stroke, cylinder order = firing order (TDCs 0/180/360/540), one sequential stage.
EngineConfig engine4(uint8_t cycle = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE)) {
    EngineConfig e{};
    e.cylinder_count = 4;
    e.cycle_type     = cycle;
    e.num_inj_stages = 1;
    e.inj_stage[0].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
    e.inj_stage[0].injections_per_cycle = 1;
    const uint8_t order[4] = {1, 3, 4, 2};
    for (uint8_t i = 0; i < 4; ++i) { e.firing_order[i].cyl = order[i]; e.cyl[i].bank = 1; }
    update_cylinder_angles(e);
    return e;
}

struct Pin final : ITimerChannel {
    int  enables = 0;
    OutputAction enabled_at = OutputAction::DRIVE_LOW;
    std::vector<OutputAction> drives;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return 1000000; }
    void enable_output(OutputAction a) noexcept override { ++enables; enabled_at = a; }
    void force_output_now(OutputAction a) noexcept override { drives.push_back(a); }
};

struct Alarm final : IAlarmTimer {
    bool armed = false;
    void arm(uint32_t) noexcept override { armed = true; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};

struct Rig {
    Pin            pins[OUTPUTS_OUTPUT_COUNT];
    ITimerChannel* pool[OUTPUTS_OUTPUT_COUNT] = {};
    PinArbiter     arb;
    EventScheduler sched;
    Alarm          angle, time;

    Rig(const EngineConfig& e, const OutputMap& m) {
        for (unsigned i = 0; i < OUTPUTS_OUTPUT_COUNT; ++i) pool[i] = &pins[i];
        arb.bind(pool, OUTPUTS_OUTPUT_COUNT);
        sched.assign_pin_arbiter(arb);
        sched.set_config(e, m, true);
        sched.claim_outputs();
    }
    // Run one engine cycle arming every injection event at its base angle; returns the rows that were
    // driven, in order, with the level they were driven to.
    void run_injection() {
        sched.assign_angle_alarm(angle);
        sched.assign_time_alarm(time);
        sched.register_compute_hook([](void* ud, uint8_t idx, EventAction k) noexcept {
            auto* s = static_cast<EventScheduler*>(ud);
            if (k == EventAction::INJ_SCHEDULE) s->arm_injection(idx, s->inj_event_base_angle(idx), 2000);
        }, &sched);
        sched.build_skeleton(100);
        sched.set_firing_enabled(true);
        sched.set_phase_known(true);
        for (int pass = 0; pass < 2; ++pass)
            for (int t = 0; t < 72; ++t) {
                g_now += 1000;
                sched.on_virtual_tooth(static_cast<uint16_t>(t), g_now, 1000);
                while (angle.armed) { angle.armed = false; sched.on_angle_alarm(); }
            }
    }
};

} // namespace

int main() {
    fprintf(stdout, "=== Output map ===\n");
    EngineConfig e = engine4();

    // ---- what a row's cylinder value names --------------------------------------------------------
    SECTION("coil-on-plug: each coil fires only its own cylinder");
    {
        EngineConfig c = engine4();
        Rig r(c, layout_output_map(c, TestCoils::COP));
        for (uint8_t cyl = 0; cyl < 4; ++cyl) CHECK(r.sched.coil_mask(cyl) == (OutputMask(1) << cyl));
    }

    SECTION("wasted spark: a row names ONE cylinder and its coil fires the companion too");
    {
        // Firing 1-3-4-2: pairs 1+4 and 3+2. The layout names cylinders 1 and 2 on IGN1 and IGN2.
        EngineConfig w = engine4();
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(w, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);
        CHECK(rows[0].cylinder == 1);
        CHECK(rows[1].cylinder == 2);
        CHECK(rows[2].function == 0);
        Rig r(w, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.sched.coil_mask(0) == 0b01);   // cylinder 1: IGN1
        CHECK(r.sched.coil_mask(3) == 0b01);   // cylinder 4: its companion, also IGN1
        CHECK(r.sched.coil_mask(1) == 0b10);   // cylinder 2: IGN2
        CHECK(r.sched.coil_mask(2) == 0b10);   // cylinder 3: its companion, also IGN2
    }

    SECTION("wasted spark: a changed firing order re-pairs the coils without moving a row");
    {
        EngineConfig w = engine4();
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(w, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);   // IGN1=cyl1, IGN2=cyl2
        // Now fire 1-2-4-3: pairs become 1+4 and 2+3 — the same pairs by chance on a four, so use a
        // six instead, where the pairs genuinely change: 1-5-3-6-2-4 pairs 1+6, 5+2, 3+4.
        EngineConfig s6{};
        s6.cylinder_count = 6; s6.cycle_type = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE);
        s6.num_inj_stages = 1; s6.ign_mode = static_cast<uint8_t>(IgnitionCoilMode::WASTED_SPARK);
        const uint8_t o1[6] = {1, 5, 3, 6, 2, 4};
        for (uint8_t i = 0; i < 6; ++i) { s6.firing_order[i].cyl = o1[i]; s6.cyl[i].bank = 1; }
        update_cylinder_angles(s6);
        OutputConfig r6[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(s6, TestCoils::WASTED, r6, OUTPUTS_OUTPUT_COUNT);
        CHECK(r6[0].cylinder == 1 && r6[1].cylinder == 2 && r6[2].cylinder == 3);
        {
            Rig r(s6, output_map_from(r6, OUTPUTS_OUTPUT_COUNT));
            CHECK(r.sched.coil_mask(5) == 0b001);   // cylinder 6 rides IGN1 with cylinder 1
            CHECK(r.sched.coil_mask(4) == 0b010);   // cylinder 5 rides IGN2 with cylinder 2
            CHECK(r.sched.coil_mask(3) == 0b100);   // cylinder 4 rides IGN3 with cylinder 3
        }
        // Same rows, firing order 1-4-2-5-3-6 (pairs 1+5, 4+3, 2+6): no row changes, the pairs follow.
        const uint8_t o2[6] = {1, 4, 2, 5, 3, 6};
        for (uint8_t i = 0; i < 6; ++i) s6.firing_order[i].cyl = o2[i];
        update_cylinder_angles(s6);
        Rig r(s6, output_map_from(r6, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.sched.coil_mask(0) == 0b001);
        CHECK(r.sched.coil_mask(4) == 0b001);   // cylinder 5 now rides IGN1
        CHECK(r.sched.coil_mask(5) == 0b010);   // cylinder 6 now rides IGN2
        CHECK(r.sched.coil_mask(3) == 0b100);   // cylinder 4 still rides IGN3 with cylinder 3
    }

    SECTION("the companion is joined only under wasted spark");
    {
        EngineConfig w = engine4();
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(w, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);
        w.ign_mode = static_cast<uint8_t>(IgnitionCoilMode::COIL_ON_PLUG);   // the same rows, read as COP
        Rig r(w, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.sched.coil_mask(0) == 0b01);
        CHECK(r.sched.coil_mask(3) == 0);        // no row names cylinder 4, and nothing invents one
    }

    SECTION("on a two-stroke the wasted-spark companion is 180 degrees away");
    {
        EngineConfig t = engine4(static_cast<uint8_t>(EngineCycleType::TWO_STROKE));
        // TDCs 0/90/180/270 by firing position 1-3-4-2: cylinders 1 (0) and 4 (180) pair.
        Rig r(t, layout_output_map(t, TestCoils::WASTED));
        CHECK(r.sched.coil_mask(0) == r.sched.coil_mask(3));
        CHECK(r.sched.coil_mask(1) == r.sched.coil_mask(2));
        CHECK(r.sched.coil_mask(0) != r.sched.coil_mask(1));
    }

    SECTION("distributor: one All row fires every cylinder");
    {
        EngineConfig d = engine4();
        Rig r(d, layout_output_map(d, TestCoils::DISTRIBUTOR));
        for (uint8_t cyl = 0; cyl < 4; ++cyl) CHECK(r.sched.coil_mask(cyl) == 0b1);
    }

    SECTION("rotary: the number is the rotor, and a rotor's value names all three faces");
    {
        EngineConfig rt{};
        rt.cylinder_count = 6; rt.cycle_type = static_cast<uint8_t>(EngineCycleType::ROTARY);
        rt.num_inj_stages = 1; rt.inj_stage[0].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
        for (uint8_t i = 0; i < 6; ++i) { rt.firing_order[i].cyl = i + 1; rt.cyl[i].bank = 1; }
        update_cylinder_angles(rt);
        Rig r(rt, layout_output_map(rt, TestCoils::COP));
        for (uint8_t f = 0; f < 3; ++f) {
            CHECK(r.sched.coil_mask(f) == (OutputMask(1) << 0));
            CHECK(r.sched.trailing_coil_mask(f) == (OutputMask(1) << MAX_ROTORS));
            CHECK(r.sched.injector_mask(0, f) == 0b01);
        }
        for (uint8_t f = 3; f < 6; ++f) {
            CHECK(r.sched.coil_mask(f) == (OutputMask(1) << 1));
            CHECK(r.sched.trailing_coil_mask(f) == (OutputMask(1) << (MAX_ROTORS + 1)));
            CHECK(r.sched.injector_mask(0, f) == 0b10);
        }
    }

    SECTION("injectors: Bank n names that bank's cylinders, All names every cylinder");
    {
        EngineConfig b = engine4();
        b.inj_stage[0].mode = static_cast<uint8_t>(InjectionMode::BANK);
        b.cyl[2].bank = 2; b.cyl[3].bank = 2;
        const uint8_t two[MAX_INJ_STAGES] = {2, 0, 0, 0};
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(b, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT, two);
        CHECK(rows[OUT_ROW_LS_BASE].cylinder == OUT_CYL_BANK1);
        CHECK(rows[OUT_ROW_LS_BASE + 1].cylinder == OUT_CYL_BANK2);
        {
            Rig r(b, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
            CHECK(r.sched.injector_mask(0, 0) == 0b01);
            CHECK(r.sched.injector_mask(0, 1) == 0b01);
            CHECK(r.sched.injector_mask(0, 2) == 0b10);
            CHECK(r.sched.injector_mask(0, 3) == 0b10);
        }
        rows[OUT_ROW_LS_BASE].cylinder = OUT_CYL_ALL;
        rows[OUT_ROW_LS_BASE + 1].function = 0;
        Rig r(b, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        for (uint8_t cyl = 0; cyl < 4; ++cyl) CHECK(r.sched.injector_mask(0, cyl) == 0b01);
    }

    SECTION("the cylinders the rows leave without a coil or a stage-1 injector are reported (P1653 / P1654)");
    {
        EngineConfig w = engine4();
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(w, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);
        {
            Rig r(w, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
            CHECK(r.sched.cylinders_without_coil() == 0);       // cylinders 3 and 4 ride their pair's coil
            CHECK(r.sched.cylinders_without_injector() == 0);
        }
        rows[1].function = 0;                                   // IGN2 (cylinders 2 + 3) removed
        rows[OUT_ROW_LS_BASE + 3].function = 0;                 // LS4 (cylinder 4) removed
        {
            Rig r(w, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
            CHECK(r.sched.cylinders_without_coil() == 0b0110);
            CHECK(r.sched.cylinders_without_injector() == 0b1000);
        }
        // A stage-2 injector does not stand in for a missing stage-1 one.
        w.num_inj_stages = 2;
        w.inj_stage[1].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
        rows[OUT_ROW_LS_BASE + 9].function = 2; rows[OUT_ROW_LS_BASE + 9].cylinder = 4; rows[OUT_ROW_LS_BASE + 9].inj_stage = 1;
        {
            Rig r(w, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
            CHECK(r.sched.cylinders_without_injector() == 0b1000);
        }
        // A cylinder outside the firing order is not reported: it fires nothing by design.
        EngineConfig e3 = engine4();
        e3.firing_order[3].cyl = 0;
        OutputConfig r3[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e3, TestCoils::COP, r3, OUTPUTS_OUTPUT_COUNT);
        r3[1].function = 0;                                     // cylinder 2 (not in the order) has no coil
        Rig r(e3, output_map_from(r3, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.sched.cylinders_without_coil() == 0);
    }

    SECTION("a coil off an IGN pin or an injector off an LS pin is not bound");
    {
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT);
        rows[OUT_ROW_HS_BASE].function = 1; rows[OUT_ROW_HS_BASE].cylinder = 1;   // a coil on HS1
        rows[6].function = 2; rows[6].cylinder = 1;                                // an injector on IGN7
        const OutputMap m = output_map_from(rows, OUTPUTS_OUTPUT_COUNT);
        CHECK(!m.ign[6].used);
        Rig r(e, m);
        CHECK(r.sched.coil_mask(0) == 0b1);
        CHECK(r.arb.owner_of(OUT_ROW_HS_BASE) == PinOwner::FREE);
        CHECK(r.arb.owner_of(6) == PinOwner::FREE);
    }

    // ---- binding ----------------------------------------------------------------------------------
    SECTION("the scheduler claims exactly the pins the rows name — not a positional default");
    {
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT);
        // Cylinder 1's coil moves to IGN6, its injector to LS10.
        rows[5] = rows[0];               rows[0].function = 0;
        rows[OUT_ROW_LS_BASE + 9] = rows[OUT_ROW_LS_BASE]; rows[OUT_ROW_LS_BASE].function = 0;
        Rig r(e, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.arb.owner_of(0) == PinOwner::FREE);
        CHECK(r.arb.owner_of(5) == PinOwner::IGNITION);
        CHECK(r.arb.owner_of(OUT_ROW_LS_BASE) == PinOwner::FREE);
        CHECK(r.arb.owner_of(OUT_ROW_LS_BASE + 9) == PinOwner::INJECTION);

        r.run_injection();
        CHECK(r.pins[OUT_ROW_LS_BASE].drives.empty());          // the old default pin never moves
        CHECK(!r.pins[OUT_ROW_LS_BASE + 9].drives.empty());     // cylinder 1 fires on LS10
    }

    SECTION("each injector is driven at its OWN row's polarity");
    {
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT);
        rows[OUT_ROW_LS_BASE + 1].active_high = 0;              // cylinder 2's injector is active-low
        Rig r(e, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        // Brought out of Hi-Z at the CLOSED level: low for active-high, high for active-low.
        CHECK(r.pins[OUT_ROW_LS_BASE].enabled_at     == OutputAction::DRIVE_LOW);
        CHECK(r.pins[OUT_ROW_LS_BASE + 1].enabled_at == OutputAction::DRIVE_HIGH);
        r.run_injection();
        CHECK(!r.pins[OUT_ROW_LS_BASE].drives.empty() && r.pins[OUT_ROW_LS_BASE].drives.front() == OutputAction::DRIVE_HIGH);
        CHECK(!r.pins[OUT_ROW_LS_BASE + 1].drives.empty() && r.pins[OUT_ROW_LS_BASE + 1].drives.front() == OutputAction::DRIVE_LOW);
        // And the safe stop drives each to ITS closed level.
        r.sched.emergency_off();
        CHECK(r.pins[OUT_ROW_LS_BASE].drives.back()     == OutputAction::DRIVE_LOW);
        CHECK(r.pins[OUT_ROW_LS_BASE + 1].drives.back() == OutputAction::DRIVE_HIGH);
    }

    SECTION("a coil is brought up at its own row's idle level");
    {
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT);
        rows[2].active_high = 0;
        Rig r(e, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.pins[0].enabled_at == OutputAction::DRIVE_LOW);
        CHECK(r.pins[2].enabled_at == OutputAction::DRIVE_HIGH);
    }

    SECTION("an injector in a switched-off stage claims nothing");
    {
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e, TestCoils::COP, rows, OUTPUTS_OUTPUT_COUNT);
        rows[OUT_ROW_LS_BASE + 9].function = 2; rows[OUT_ROW_LS_BASE + 9].inj_stage = 1; rows[OUT_ROW_LS_BASE + 9].cylinder = 1;
        Rig r(e, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        CHECK(r.arb.owner_of(OUT_ROW_LS_BASE + 9) == PinOwner::FREE);
    }

    SECTION("a per-cylinder cut reaches every row that names the cylinder");
    {
        // Wasted spark: cylinder 1 shares IGN1 with cylinder 4. A second-stage injector serves cylinder 1.
        EngineConfig e2 = engine4(); e2.num_inj_stages = 2;
        e2.inj_stage[1].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e2, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);
        Rig r(e2, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        r.sched.set_cylinder_cuts(1u << 0);
        const uint64_t m = r.sched.execution_mask();
        CHECK(!(m & (1ULL << 0)));                                   // the shared coil
        CHECK(m & (1ULL << 1));                                      // the other coil
        CHECK(!(m & (1ULL << (MAX_IGN_CHANNELS + 0))));              // stage-1 injector, LS1
        CHECK(!(m & (1ULL << (MAX_IGN_CHANNELS + 4))));              // stage-2 injector, LS5
        CHECK(m & (1ULL << (MAX_IGN_CHANNELS + 1)));                 // cylinder 2's injector untouched
    }

    SECTION("a FUEL-only cylinder cut leaves the shared coil firing; a SPARK-only cut leaves the injectors");
    {
        // Pre-ignition defaults to fuel only precisely because, on wasted spark, a cylinder's coil also
        // fires its partner. One mask for both used to cut the coil anyway.
        EngineConfig e2 = engine4();
        OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
        layout_output_rows(e2, TestCoils::WASTED, rows, OUTPUTS_OUTPUT_COUNT);
        Rig r(e2, output_map_from(rows, OUTPUTS_OUTPUT_COUNT));
        r.sched.set_cylinder_cuts(1u << 0, 0u);                       // fuel only, cylinder 1
        uint64_t m = r.sched.execution_mask();
        CHECK(m & (1ULL << 0));                                      // the shared coil still fires
        CHECK(!(m & (1ULL << (MAX_IGN_CHANNELS + 0))));              // cylinder 1's injector cut
        r.sched.set_cylinder_cuts(0u, 1u << 0);                       // spark only
        m = r.sched.execution_mask();
        CHECK(!(m & (1ULL << 0)));                                   // the coil cut
        CHECK(m & (1ULL << (MAX_IGN_CHANNELS + 0)));                 // the injector fires
        r.sched.set_cylinder_cuts(0u, 0u);
        CHECK(r.sched.cylinder_cuts() == 0);
    }

    return test_summary();
}

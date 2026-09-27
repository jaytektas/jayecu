#include "test_helpers.h"
#include "../firmware/Platform/platform_hal.h"      // platform_learned_block
#include "../generated/learned_layout.h"            // LEARNED_BOOST_LTT_*
#include <cmath>
#include <cstring>
#include "../firmware/Engine/Modules/Boost.h"
#include "../firmware/Engine/EngineStateMachine.h"   // EngineRunState
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

// Build a config with both tables 1-D on their own primary axis, second axis reserved + off.
//   boost_target_table    : flat ~150 kPa target across rpm
//   base_boost_duty_table : flat 40% feed-forward across TARGET PRESSURE (its axis is the target the
//                           module published this frame, not rpm — see the schema note on the table)
// A FRESH CONFIG IS A FRESH BOARD. The learned region is one buffer for the whole process — that is
// what makes it battery-backed state rather than a member — so a trim learned in one section is still
// there in the next, and several sections below were quietly measuring the previous one's homework.
// Clearing it here rather than per-section means the trap cannot be re-sprung by a section added later.
static void clear_learned() {
    if (auto* p = platform_learned_block(LEARNED_BOOST_LTT_OFFSET, LEARNED_BOOST_LTT_BYTES))
        std::memset(p, 0, LEARNED_BOOST_LTT_BYTES);
}

static BoostConfig make_cfg() {
    clear_learned();
    BoostConfig c{};
    c.enabled              = 1;
    c.mode                 = 1;        // closed loop unless a test flips it
    c.activation_rpm       = 2000;
    c.activation_kpa       = 1100;     // 110.0 kPa (scale 0.1) — above atmospheric
    c.kp                   = 500;      // 0.5 %/kPa
    c.ki                   = 2000;     // 2.0 %/kPa/s
    c.kd                   = 0;
    c.max_duty_pct = 900;
    c.overboost_limit_kpa  = 2500;     // 250.0 kPa
    c.overboost_cut_method = 0;        // fuel
    // UNASSIGNED IS -1. A zero-initialised selector names signal id 0, which is a real channel
    // (SIG_ABS_MODE) — so a config that had not thought about the arm switch was permanently DISARMED
    // and every section below it measured a module that never ran.
    c.arm_sig              = -1;       // always armed
    c.trim_sig             = -1;       // no trim knob
    c.trim_max_kpa         = -1000;    // -100.0 kPa at full travel, when one is wired
    c.scramble_sig         = -1;       // no scramble input by default
    c.scramble_kpa         = 300;      // 30.0 kPa bump when wired
    c.scramble_base_pct    = 0;
    c.scramble_hold_s      = 0;        // no minimum-on unless a section asks for one
    c.scramble_max_s       = 0;        // no maximum
    c.scramble_rest_s      = 0;
    // THE HANDOVER, wide open by default so the sections that predate it still exercise the loop.
    // 50 kPa under a 150 kPa target puts the control point at 100 kPa, below every map these tests
    // use — a zero here would mean the loop only ever engaged at or above target, which is a real
    // setting but not the one most of this file is about.
    c.control_point_kpa    = 500;      // 50.0 kPa (scale 0.1)
    c.spool_assist         = 0;
    c.start_delay_en       = 0;
    // Clamps wide open unless a section narrows them: a floor of 0, an integrator allowed its full
    // authority, no throttle gate, and only the absolute overboost limit armed.
    c.min_duty_pct         = 0;
    c.iterm_max_pct        = 1000;     // 100.0 %
    c.max_deriv_kpa_s      = 10000;    // 1000 kPa/s — effectively no clamp at these test rates
    c.min_tps_pct          = 0;
    c.overboost_offset_kpa = 0;

    // axis channels: the target table on rpm (gear reserved, off), the base duty on the TARGET the
    // module publishes (rpm reserved, off). Flat cells either way, so these tests read one number from
    // each table and the axes only have to be coherent, not interesting.
    c.boost_target_table_x_src    = SIG_RPM;          c.boost_target_table_y_src    = SIG_GEAR; c.boost_target_table_y_en    = 0;
    c.base_boost_duty_table_x_src = SIG_BOOST_TARGET; c.base_boost_duty_table_y_src = SIG_RPM;  c.base_boost_duty_table_y_en = 0;

    // breakpoints + row-0 cells
    const float rpm[8] = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
    const uint16_t tgt[8] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};  // 150.0 kPa (scale 0.1)
    for (int i = 0; i < 8; i++) {
        c.boost_rpm_axis[i] = rpm[i];
        c.boost_target_table[i] = tgt[i];
    }
    c.boost_rpm_axis_n = 8;   // 8 populated rpm bins (flat here, but declare the live count)

    // FLAT 40 % ACROSS THE WHOLE TARGET AXIS. Flat on purpose: a shaped feed-forward would mean every
    // section that moves the map also moves the base duty under it, and then nothing a test measured
    // would be attributable to the loop.
    for (int i = 0; i < 16; i++) {
        c.base_targ_axis[i] = 100.0f + 10.0f * i;   // 100..250 kPa absolute
        c.base_boost_duty_table[i] = 400;           // 40% (scale 0.1)
    }
    c.base_targ_axis_n = 16;
    // The correction slots: all four wired to a channel with an axis, all four OFF. A populated axis
    // with a zero live count is what TableEval's debug guard exists to catch, so they are declared
    // whether or not a section switches one on.
    c.corr1_table_x_src = SIG_IAT;  c.corr2_table_x_src = SIG_CLT;
    c.corr3_table_x_src = SIG_GEAR; c.corr4_table_x_src = SIG_TPS;
    const float caxis[8] = {0, 15, 30, 45, 60, 75, 90, 100};
    for (int i = 0; i < 8; i++) {
        c.corr1_axis[i] = caxis[i]; c.corr2_axis[i] = caxis[i];
        c.corr3_axis[i] = caxis[i]; c.corr4_axis[i] = caxis[i];
    }
    c.corr1_axis_n = c.corr2_axis_n = c.corr3_axis_n = c.corr4_axis_n = 8;

    // The Ki curve: declared and populated, scheduling OFF. Flat at the scalar's own value so a
    // section that switches scheduling on without reshaping it measures no change — which is what
    // makes the shaped case below attributable to the shape.
    c.ki_sched_en = 0;
    c.ki_table_x_src = SIG_BOOST_ERROR;
    const float kax[8] = {-100, -50, -20, -5, 5, 20, 50, 100};
    for (int i = 0; i < 8; i++) { c.ki_err_axis[i] = kax[i]; c.ki_table[i] = 2000; }   // 2.0 %/kPa/s
    c.ki_err_axis_n = 8;

    // Long-term trim: gates wide open, learning OFF. A section that wants it turns ltt_en on; the
    // gates are deliberately permissive here so a failure points at the learn logic and not at one of
    // four conditions a test forgot to satisfy.
    c.ltt_en            = 0;
    c.ltt_min_tps_pct   = 0;
    c.ltt_min_rpm       = 0;
    c.ltt_max_rpm       = 12000;
    c.ltt_min_gear      = 0;
    c.ltt_dwell_ms      = 100;
    c.ltt_learn_pct     = 1000;    // 100 % of the integrator per dwell, so one dwell is measurable
    c.ltt_authority_pct = 150;     // 15.0 points of duty

    // The actuator: a solenoid unless a section says otherwise. The gate travel limits matter even
    // then — a zero-initialised BoostConfig has min AND max at 0, so the clamp range is [0,0] and a
    // motorised gate is commanded permanently shut whatever the loop asked for.
    c.output_mode        = 0;
    c.gate_pos_sig       = -1;
    c.gate_min_pos_pct   = 0;
    c.gate_max_pos_pct   = 1000;   // 100.0 %
    c.gate_rate_pct_s    = 0;      // no slew limit unless a section asks
    c.gate_kp            = 2000;   // 2.0 %/%
    c.gate_ki            = 4000;   // 4.0 %/%/s
    c.gate_kd            = 0;
    c.gate_iterm_max_pct = 500;    // 50.0 %
    c.base_rpm_axis_n  = 8;
    for (int i = 0; i < 8; i++) c.base_rpm_axis[i] = rpm[i];

    // The start-delay curve: flat 3.00 s, populated whether or not a section enables it, so the axis
    // never sits with real breakpoints and a zero live count (TableEval's debug guard fires on that).
    c.start_delay_table_x_src = SIG_RPM;
    for (int i = 0; i < 8; i++) { c.delay_rpm_axis[i] = rpm[i]; c.start_delay_table[i] = 300; }
    c.delay_rpm_axis_n = 8;
    return c;
}

static SignalBus make_bus(float rpm, float map_kpa) {
    SignalBus b{};
    b.set(SIG_RPM, rpm);
    b.set(SIG_MAP, map_kpa);
    return b;
}

static EnginePosition make_pos(float rpm) { EnginePosition p{}; p.rpm = rpm; return p; }

static bool  g_last_fuel_cut=false, g_last_ign_cut=false;
static float g_last_ltt = 0.0f;     // the learned trim the module published on the last step
// Step the loop n times at 10 ms cadence so the PI integrates; return the final published duty.
// prot = prot_boost_corr_pct injected into the frame each step.
static float run(Boost& bc, float rpm, float map_kpa, int n,
                 int8_t prot = 0, EngineFrame* out = nullptr) {
    EngineFrame f{};
    float duty = -1.0f;
    for (int i = 0; i < n; ++i) {
        g_ms += 10;
        SignalBus b = make_bus(rpm, map_kpa);
        if (prot) b.set(wk::prot_boost_corr, static_cast<float>(prot));
        bc.update(make_pos(rpm), b, f);
        g_last_fuel_cut = b.valid(wk::fuel_cut); g_last_ign_cut = b.valid(wk::ign_cut);
        g_last_ltt = b.get(SIG_BOOST_LTT_PCT, 0.0f);
        duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
    }
    if (out) *out = f;
    return duty;
}

int main() {
    fprintf(stdout, "=== Boost ===\n");

    SECTION("disabled — publishes nothing");
    {
        auto cfg = make_cfg(); cfg.enabled = 0;
        Boost bc; bc.init(cfg);
        CHECK(run(bc, 4000.0f, 200.0f, 5) < 0.0f);   // wastegate_duty default -> never published
    }

    SECTION("below activation RPM — no wastegate output");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        CHECK(run(bc, 1000.0f, 200.0f, 5) < 0.0f);   // 1000 < activation_rpm 2000
    }

    SECTION("below activation MAP — no wastegate output (don't fight the spring)");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        CHECK(run(bc, 4000.0f, 90.0f, 5) < 0.0f);    // 90 kPa < activation 110 kPa
    }

    SECTION("boost target published as telemetry");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        EngineFrame f;
        run(bc, 4000.0f, 150.0f, 1, 0, &f);
        CHECK(f.boost_target_kpa > 140.0f && f.boost_target_kpa < 160.0f);   // ~150 kPa
    }

    SECTION("closed loop — under target drives duty up toward max");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        // map (130) well under target (150): error positive -> PI adds on top of the 40% base.
        const float duty = run(bc, 4000.0f, 130.0f, 50);
        CHECK(duty > 40.0f);    // above the feed-forward base
    }

    SECTION("closed loop — over target backs duty off below base");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        // map (180) over target (150): error negative -> PI pulls below the 40% base.
        const float duty = run(bc, 4000.0f, 180.0f, 50);
        CHECK(duty < 40.0f);
    }

    SECTION("duty clamps to max_duty_pct");
    {
        auto cfg = make_cfg(); cfg.max_duty_pct = 600;
        Boost bc; bc.init(cfg);
        const float duty = run(bc, 4000.0f, 120.0f, 200);   // far under target, wind up hard
        CHECK(duty <= 60.0f + 1e-3f);
    }

    SECTION("prot_boost_corr_pct pulls the target/duty back");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        const float full = run(bc, 4000.0f, 130.0f, 50, /*prot=*/0);
        Boost bc2; bc2.init(cfg);
        const float pulled = run(bc2, 4000.0f, 130.0f, 50, /*prot=*/-50);   // halve the target
        CHECK(pulled < full);
    }

    SECTION("prot -100 kills boost — target at atmosphere (no boost), feed-forward zeroed");
    {
        auto cfg = make_cfg(); cfg.mode = 0;   // open loop: duty is pure feed-forward * corr
        Boost bc; bc.init(cfg);
        EngineFrame f;
        const float duty = run(bc, 4000.0f, 130.0f, 5, /*prot=*/-100, &f);
        CHECK(std::fabs(f.boost_target_kpa - 101.3f) < 0.5f);   // no boost: atmosphere, not a vacuum
        CHECK(duty == 0.0f);   // 40% base * 0 corr
    }

    // --- the feed-forward is indexed by the TARGET ----------------------------------------------

    SECTION("base duty follows the target, not the rpm it happens to be at");
    {
        auto cfg = make_cfg();
        cfg.mode = 0;                          // open loop: duty is the feed-forward and nothing else
        cfg.boost_target_table[1] = 1200;      // 2000 rpm asks for 120.0 kPa
        cfg.boost_target_table[5] = 2000;      // 6000 rpm asks for 200.0 kPa
        cfg.base_boost_duty_table[2]  = 200;   // 120 kPa of target costs 20 %
        cfg.base_boost_duty_table[10] = 700;   // 200 kPa of target costs 70 %
        Boost bc; bc.init(cfg);
        // Were this still indexed by rpm both would read the flat 40 % the rest of the row holds; the
        // whole point of the re-axis is that one rpm asking for two boost levels wants two duties.
        CHECK(std::fabs(run(bc, 2000.0f, 130.0f, 3) - 20.0f) < 1e-3f);
        Boost bc2; bc2.init(cfg);
        CHECK(std::fabs(run(bc2, 6000.0f, 130.0f, 3) - 70.0f) < 1e-3f);
    }

    SECTION("protection walks the feed-forward DOWN the table, not just scales it");
    {
        auto cfg = make_cfg();
        cfg.mode = 0;
        cfg.boost_target_table[5] = 2000;      // 6000 rpm asks for 200.0 kPa
        cfg.base_boost_duty_table[10] = 700;   // 200 kPa -> 70 %
        cfg.base_boost_duty_table[5]  = 300;   // 150 kPa -> 30 %
        Boost bc; bc.init(cfg);
        EngineFrame f;
        // -51 % protection takes the BOOST (98.7 kPa above atmosphere) to 48.4 — a 149.7 kPa target, a
        // different CELL — so the base duty asked for is the one that belongs to the boost permitted.
        const float duty = run(bc, 6000.0f, 130.0f, 3, /*prot=*/-51, &f);
        CHECK(std::fabs(f.boost_target_kpa - 150.0f) < 0.5f);
        CHECK(duty > 13.0f && duty < 16.5f);   // ~30 % at the 150 kPa cell, times the 0.49 correction
    }

    // --- the handover: where the closed loop takes over from the feed-forward -------------------
    //
    // Every one of these puts the map ABOVE activation (110 kPa) and BELOW the control point, which
    // needs the offset opened out to 20 kPa: at the shipped 50 the control point lands at 100 kPa,
    // under activation, and the two gates can never be told apart.

    SECTION("below the control point the loop stays out and rests on the base duty");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200;   // control point = 150 - 20 = 130 kPa
        Boost bc; bc.init(cfg);
        // 120 kPa: past activation, 10 kPa short of the control point. Were the loop running it would
        // see +30 kPa of error and wind up; resting, it publishes the feed-forward and nothing else.
        const float duty = run(bc, 4000.0f, 120.0f, 200);
        CHECK(std::fabs(duty - 40.0f) < 1e-3f);
    }

    SECTION("spool assist rests on full duty instead, clamped to the ceiling");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.spool_assist = 1;
        Boost bc; bc.init(cfg);
        const float duty = run(bc, 4000.0f, 120.0f, 200);
        CHECK(std::fabs(duty - 90.0f) < 1e-3f);   // 100% asked, max_duty_pct 90 delivered
    }

    SECTION("above the control point it takes over and the trim appears");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200;
        Boost bc; bc.init(cfg);
        const float duty = run(bc, 4000.0f, 140.0f, 50);   // 140 >= 130, and 10 kPa under target
        CHECK(duty > 40.0f);
    }

    SECTION("dropping back below the control point FREEZES the integrator, it does not reset it");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200;
        Boost bc; bc.init(cfg);
        const float wound = run(bc, 4000.0f, 140.0f, 50);        // take over and wind up
        CHECK(wound > 45.0f);                                     // something to lose
        const float resting = run(bc, 4000.0f, 120.0f, 20);       // back under the control point
        CHECK(std::fabs(resting - 40.0f) < 1e-3f);                // resting on the feed-forward
        const float resumed = run(bc, 4000.0f, 140.0f, 1);        // one frame back above it
        // A reset would put this at the base duty. The trim survived the excursion, which is what
        // stops a lift or a gearchange throwing away everything the loop had learned.
        CHECK(std::fabs(resumed - wound) < 1.0f);
    }

    SECTION("the start delay holds the loop off even when the control point is satisfied");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.start_delay_en = 1;   // 3.00 s
        Boost bc; bc.init(cfg);
        const float early = run(bc, 4000.0f, 140.0f, 100);   // 1.0 s in: past the control point, still waiting
        CHECK(std::fabs(early - 40.0f) < 1e-3f);
        const float late = run(bc, 4000.0f, 140.0f, 250);    // 3.5 s in: handed over
        CHECK(late > 40.0f);
    }

    SECTION("the delay is measured per spool — dropping out of activation re-arms it");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.start_delay_en = 1;
        Boost bc; bc.init(cfg);
        run(bc, 4000.0f, 140.0f, 400);                       // 4 s: well handed over
        CHECK(run(bc, 4000.0f, 140.0f, 1) > 40.0f);
        run(bc, 1000.0f, 140.0f, 5);                         // off boost: activation drops
        const float again = run(bc, 4000.0f, 140.0f, 100);   // 1 s into the NEXT spool
        CHECK(std::fabs(again - 40.0f) < 1e-3f);             // waiting again, not carrying the old clock
    }

    // --- the motorised gate -------------------------------------------------------------------------

    SECTION("a motorised gate is given a POSITION and no solenoid duty at all");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.output_mode = 1;
        cfg.gate_pos_sig = SIG_WASTEGATE_VALVE_POS_1;
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float pos_target = -1.0f, wg_duty = -1.0f;
        for (int i = 0; i < 3; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_WASTEGATE_VALVE_POS_1, 60.0f);
            bc.update(make_pos(4000.0f), b, f);
            pos_target = b.get(SIG_WASTEGATE_POS_TARGET, -1.0f);
            wg_duty    = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        // The 40 % base duty means "hold it 40 % shut", which is a valve 60 % OPEN.
        CHECK(std::fabs(pos_target - 60.0f) < 1e-3f);
        CHECK(wg_duty < 0.0f);   // nothing published for a solenoid that is not there
    }

    SECTION("the inner loop drives the motor toward the position it was asked for");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.output_mode = 1;
        cfg.gate_pos_sig = SIG_WASTEGATE_VALVE_POS_1;
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        auto at = [&](float actual) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_WASTEGATE_VALVE_POS_1, actual);
            bc.update(make_pos(4000.0f), b, f);
            return b.get(SIG_WASTEGATE_POS_DUTY, 0.0f);
        };
        // The first frame has no dt yet, so the servo cannot have integrated anything — step twice.
        at(20.0f);
        CHECK(at(20.0f) > 0.0f);    // valve too shut (target 60): drive it open
        Boost bc2; bc2.init(cfg);
        at(90.0f);
        CHECK(at(90.0f) < 0.0f);    // too open: drive it back
    }

    SECTION("...in open loop too, where the outer loop's clock never ticks");
    {
        // The servo borrowed last_ms_ once, which only advances while the pressure loop is running —
        // so an open-loop tune on a motorised gate had dt == 0 for ever and a valve that never moved.
        auto cfg = make_cfg(); cfg.mode = 0; cfg.output_mode = 1;
        cfg.gate_pos_sig = SIG_WASTEGATE_VALVE_POS_1;
        cfg.gate_kp = 0;                 // integral only: it cannot accumulate at all without a dt
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float d = 0.0f;
        for (int i = 0; i < 20; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_WASTEGATE_VALVE_POS_1, 20.0f);
            bc.update(make_pos(4000.0f), b, f);
            d = b.get(SIG_WASTEGATE_POS_DUTY, 0.0f);
        }
        CHECK(d > 1.0f);
    }

    SECTION("the slew limit turns a step demand into a ramp");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.output_mode = 1;
        cfg.gate_pos_sig = SIG_WASTEGATE_VALVE_POS_1;
        cfg.gate_rate_pct_s = 100;                     // 100 %/s == 1 % per 10 ms frame
        for (int i = 0; i < 16; i++) cfg.base_boost_duty_table[i] = 0;      // ask for 0 % shut
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        auto step = [&]() {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_WASTEGATE_VALVE_POS_1, 50.0f);
            bc.update(make_pos(4000.0f), b, f);
            return b.get(SIG_WASTEGATE_POS_TARGET, -1.0f);
        };
        CHECK(std::fabs(step() - 100.0f) < 1e-3f);     // the FIRST command lands where asked: 0 shut
        step(); step();                                //  == 100 % open. No ramp from nowhere.
        // The module reads the config through a pointer, so moving the table under it is a retune
        // without a reset — which is the case the slew limit exists for.
        for (int i = 0; i < 16; i++) cfg.base_boost_duty_table[i] = 1000;   // now 100 % shut...
        float last = -1.0f;                            //  ...clamped to max_duty 90 -> 10 % open
        for (int i = 0; i < 5; ++i) last = step();
        // Five frames of 1 % is a valve that has moved 5 %, not one that has jumped 90.
        CHECK(last > 94.0f && last < 96.0f);
    }

    SECTION("a motorised gate with no position sensor falls back rather than servoing blind");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.output_mode = 1; cfg.gate_pos_sig = -1;
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        g_ms += 10;
        SignalBus b = make_bus(4000.0f, 130.0f);
        bc.update(make_pos(4000.0f), b, f);
        CHECK(b.get(SIG_WASTEGATE_POS_DUTY, -999.0f) == -999.0f);   // nothing driven
        CHECK(b.get(SIG_WASTEGATE_DUTY, -1.0f) >= 0.0f);            // the duty is published instead
    }

    // --- the long-term trim -----------------------------------------------------------------------

    SECTION("learning migrates the integrator into a cell and bleeds it out of the loop");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.ltt_en = 1;
        Boost bc; bc.init(cfg);
        // 8 frames is 70 ms of dwell, inside the 100 ms this config asks for: the loop is correcting but
        // has not yet earned the right to write anything down.
        const float before = run(bc, 4000.0f, 140.0f, 8);
        CHECK(before > 40.0f);
        const float trim_before = g_last_ltt;
        CHECK(std::fabs(trim_before) < 1e-3f);                 // nothing learned yet
        run(bc, 4000.0f, 140.0f, 20);                          // past the 100 ms dwell
        CHECK(g_last_ltt > 0.5f);                              // the cell took the correction
    }

    SECTION("...and the total correction is preserved across the migration");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.ltt_en = 1;
        Boost bc; bc.init(cfg);
        const float settled = run(bc, 4000.0f, 140.0f, 40);
        const float after   = run(bc, 4000.0f, 140.0f, 1);
        // What moved is WHERE the correction lives, not how much of it there is. A bleed that did not
        // match what the cell took would show up here as a step in the delivered duty.
        CHECK(std::fabs(after - settled) < 1.0f);
        CHECK(g_last_ltt > 0.5f);                              // and it really did move
    }

    SECTION("the authority caps a cell, and nothing is bled that the cell refused");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.ltt_en = 1;
        cfg.ltt_authority_pct = 20;                            // 2.0 points, reached almost at once
        Boost bc; bc.init(cfg);
        const float duty = run(bc, 4000.0f, 140.0f, 400);
        CHECK(g_last_ltt <= 2.0f + 1e-3f);
        // Bleeding the intended move rather than what was taken would hand the loop's correction to a
        // table that refused it, and the duty would sag by exactly the amount that went nowhere.
        CHECK(duty > 45.0f);
    }

    SECTION("the gates keep a reading that belongs to the conditions out of the table");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.ltt_en = 1;
        cfg.ltt_min_rpm = 6000;                                // this pull is at 4000
        Boost bc; bc.init(cfg);
        run(bc, 4000.0f, 140.0f, 100);
        CHECK(std::fabs(g_last_ltt) < 1e-3f);
    }

    SECTION("a trim that has been learned is applied even with learning switched off");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.ltt_en = 1;
        // BOTH configs built before anything is learned: make_cfg() clears the learned region, so
        // building the second one afterwards would wipe exactly what this section is about to check.
        auto frozen = make_cfg(); frozen.mode = 0; frozen.ltt_en = 0;   // open loop, learning off
        Boost bc; bc.init(cfg);
        run(bc, 4000.0f, 140.0f, 60);                          // learn something
        const float learned = g_last_ltt;
        CHECK(learned > 0.5f);
        Boost bc2; bc2.init(frozen);
        const float duty = run(bc2, 4000.0f, 140.0f, 5);
        // Switching learning off freezes what it knows rather than discarding it — the open-loop duty
        // is the 40 % base plus whatever the cell holds.
        CHECK(duty > 40.5f);
        CHECK(std::fabs(duty - (40.0f + learned)) < 0.2f);
    }

    // --- the integral gain, scheduled against error -----------------------------------------------

    SECTION("boost error is published whatever the mode");
    {
        auto cfg = make_cfg(); cfg.mode = 0;   // open loop has no PID, but the error is still the truth
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        g_ms += 10;
        SignalBus b = make_bus(4000.0f, 130.0f);
        bc.update(make_pos(4000.0f), b, f);
        CHECK(std::fabs(b.get(SIG_BOOST_ERROR, -999.0f) - 20.0f) < 0.5f);   // 150 target - 130 map
    }

    SECTION("a flat Ki curve is the Ki scalar");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200;
        Boost bc; bc.init(cfg);
        const float scalar = run(bc, 4000.0f, 140.0f, 50);
        auto cfg2 = make_cfg(); cfg2.control_point_kpa = 200; cfg2.ki_sched_en = 1;
        Boost bc2; bc2.init(cfg2);
        const float curved = run(bc2, 4000.0f, 140.0f, 50);
        CHECK(std::fabs(scalar - curved) < 0.2f);
    }

    SECTION("...and a shaped one is gentle near target and firm away from it");
    {
        // 0.2 %/kPa/s in the middle bins, 8.0 at the ends. Kp is zeroed so the whole trim is the
        // integrator's, and the control point is opened right out so the handover never confounds it.
        auto shaped = [](uint16_t target_x10) {
            auto c = make_cfg();
            c.control_point_kpa = 4000;
            c.kp = 0;
            c.ki_sched_en = 1;
            for (int i = 0; i < 8; i++) { c.ki_table[i] = 200; c.boost_target_table[i] = target_x10; }
            c.ki_table[0] = c.ki_table[7] = 8000;
            return c;
        };
        // Both run at 150 kPa of map. A 160 kPa target is 10 kPa of error, which lands in the gentle
        // middle of the curve; a 400 kPa target is 250 kPa of error, past the top breakpoint and so
        // holding the firm end bin. A loop that ignored the schedule would wind at one rate for both.
        auto near_cfg = shaped(1600);
        Boost bc; bc.init(near_cfg);
        const float gentle = run(bc, 4000.0f, 150.0f, 30);
        auto far_cfg = shaped(4000);
        Boost bc2; bc2.init(far_cfg);
        const float firm = run(bc2, 4000.0f, 150.0f, 30);
        CHECK(firm > gentle + 5.0f);
    }

    // --- the driver's own controls ----------------------------------------------------------------

    SECTION("an unasserted arm switch is the same answer as switched off");
    {
        auto cfg = make_cfg(); cfg.arm_sig = SIG_GEAR;   // a settable channel standing in for a switch
        Boost bc; bc.init(cfg);
        CHECK(run(bc, 4000.0f, 150.0f, 5) < 0.0f);       // nothing published: the gate runs on its spring
    }

    SECTION("...and asserting it resumes the tune unchanged");
    {
        auto cfg = make_cfg(); cfg.arm_sig = SIG_GEAR;
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float duty = -1.0f;
        for (int i = 0; i < 50; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_GEAR, 1.0f);                       // armed
            bc.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        CHECK(duty > 40.0f);
    }

    SECTION("the trim knob moves the target by its authority, and only by that");
    {
        auto cfg = make_cfg(); cfg.trim_sig = SIG_GEAR; cfg.trim_max_kpa = -400;   // -40 kPa at 100 %
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        auto at = [&](float knob) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 150.0f);
            b.set(SIG_GEAR, knob);
            bc.update(make_pos(4000.0f), b, f);
            return f.boost_target_kpa;
        };
        CHECK(std::fabs(at(0.0f)   - 150.0f) < 0.5f);    // off position does nothing
        CHECK(std::fabs(at(50.0f)  - 130.0f) < 0.5f);    // half travel, half the authority
        CHECK(std::fabs(at(100.0f) - 110.0f) < 0.5f);
        CHECK(std::fabs(at(400.0f) - 110.0f) < 0.5f);    // clamped: an input failed high asks no more
    }

    SECTION("scramble holds for its minimum even when the button is tapped");
    {
        auto cfg = make_cfg(); cfg.scramble_sig = SIG_GEAR; cfg.scramble_hold_s = 10;   // 1.0 s
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        auto step = [&](float btn) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 150.0f);
            b.set(SIG_GEAR, btn);
            bc.update(make_pos(4000.0f), b, f);
            return f.boost_target_kpa;
        };
        step(1.0f);                                      // one frame of button — a tap
        CHECK(step(0.0f) > 175.0f);                      // still bumped 10 ms later
        for (int i = 0; i < 50; ++i) step(0.0f);         // 0.5 s in
        CHECK(f.boost_target_kpa > 175.0f);
        for (int i = 0; i < 80; ++i) step(0.0f);         // past 1.0 s
        CHECK(f.boost_target_kpa < 160.0f);              // the hold ran out and it let go
    }

    SECTION("scramble cannot be leant on: the maximum ends it and the rest locks it out");
    {
        auto cfg = make_cfg();
        cfg.scramble_sig = SIG_GEAR; cfg.scramble_max_s = 20; cfg.scramble_rest_s = 30;  // 2 s / 3 s
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        auto held = [&]() {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 150.0f);
            b.set(SIG_GEAR, 1.0f);                       // button held down throughout
            bc.update(make_pos(4000.0f), b, f);
            return f.boost_target_kpa;
        };
        for (int i = 0; i < 100; ++i) held();            // 1 s of holding
        CHECK(f.boost_target_kpa > 175.0f);
        for (int i = 0; i < 150; ++i) held();            // past 2 s: the maximum trips
        CHECK(f.boost_target_kpa < 160.0f);
        for (int i = 0; i < 200; ++i) held();            // still holding, 2 s into the 3 s rest
        CHECK(f.boost_target_kpa < 160.0f);              // locked out, not re-triggering
        for (int i = 0; i < 200; ++i) held();            // past the rest
        CHECK(f.boost_target_kpa > 175.0f);              // and it may run again
    }

    SECTION("the scramble duty bump rides inside the protection scaling");
    {
        auto cfg = make_cfg(); cfg.mode = 0;
        cfg.scramble_sig = SIG_GEAR; cfg.scramble_base_pct = 200;   // +20 points
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float duty = -1.0f;
        for (int i = 0; i < 3; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_GEAR, 1.0f);
            bc.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        CHECK(std::fabs(duty - 60.0f) < 1e-3f);          // 40 base + 20 bump
        Boost bc2; bc2.init(cfg);
        for (int i = 0; i < 3; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_GEAR, 1.0f);
            b.set(wk::prot_boost_corr, -100.0f);
            bc2.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        CHECK(std::fabs(duty) < 1e-3f);                  // kill-boost takes the bump with it
    }

    // --- the correction slots ----------------------------------------------------------------------

    SECTION("a target slot multiplies the target");
    {
        auto cfg = make_cfg();
        cfg.corr1_en = 1; cfg.corr1_applies = 0;               // target
        cfg.corr1_table_x_src = SIG_TPS;
        for (int i = 0; i < 8; i++) cfg.corr1_table[i] = -100;  // -10.0 % everywhere
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        g_ms += 10;
        SignalBus b = make_bus(4000.0f, 150.0f);
        b.set(SIG_TPS, 50.0f);
        bc.update(make_pos(4000.0f), b, f);
        CHECK(std::fabs(f.boost_target_kpa - 145.1f) < 0.5f);   // a tenth less BOOST: 101.3 + 48.7 * 0.90
    }

    SECTION("a duty slot adds points of duty, and leaves the target alone");
    {
        auto cfg = make_cfg(); cfg.mode = 0;                    // open loop: duty is the feed-forward
        cfg.corr2_en = 1; cfg.corr2_applies = 1;                // duty
        cfg.corr2_table_x_src = SIG_TPS;
        for (int i = 0; i < 8; i++) cfg.corr2_table[i] = 150;    // +15.0 points
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float duty = -1.0f;
        for (int i = 0; i < 3; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_TPS, 50.0f);
            bc.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        CHECK(std::fabs(duty - 55.0f) < 1e-3f);                 // 40 base + 15
        CHECK(std::fabs(f.boost_target_kpa - 150.0f) < 0.5f);   // untouched
    }

    SECTION("slots sum, and a disabled slot is not evaluated at all");
    {
        auto cfg = make_cfg();
        cfg.corr1_en = 1; cfg.corr1_applies = 0; cfg.corr1_table_x_src = SIG_TPS;
        cfg.corr3_en = 0; cfg.corr3_applies = 0; cfg.corr3_table_x_src = SIG_TPS;   // OFF
        for (int i = 0; i < 8; i++) { cfg.corr1_table[i] = -100; cfg.corr3_table[i] = -500; }
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        g_ms += 10;
        SignalBus b = make_bus(4000.0f, 150.0f);
        b.set(SIG_TPS, 50.0f);
        bc.update(make_pos(4000.0f), b, f);
        // Only slot 1 counts. Were the disabled slot read, its -50 % would drag this to 120.8 kPa.
        CHECK(std::fabs(f.boost_target_kpa - 145.1f) < 0.5f);
        cfg.corr3_en = 1;
        Boost bc2; bc2.init(cfg);
        g_ms += 10;
        SignalBus b2 = make_bus(4000.0f, 150.0f);
        b2.set(SIG_TPS, 50.0f);
        bc2.update(make_pos(4000.0f), b2, f);
        CHECK(std::fabs(f.boost_target_kpa - 120.8f) < 0.5f);   // 101.3 + 48.7 * (1 - 0.60)
    }

    SECTION("a duty slot cannot survive a kill-boost correction");
    {
        auto cfg = make_cfg(); cfg.mode = 0;
        cfg.corr2_en = 1; cfg.corr2_applies = 1; cfg.corr2_table_x_src = SIG_TPS;
        for (int i = 0; i < 8; i++) cfg.corr2_table[i] = 200;    // +20 points
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float duty = -1.0f;
        for (int i = 0; i < 3; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_TPS, 50.0f);
            b.set(wk::prot_boost_corr, -100.0f);
            bc.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        // Added AFTER the protection scaling this would still be driving the solenoid at 20 %.
        CHECK(std::fabs(duty) < 1e-3f);
    }

    // --- the clamps ------------------------------------------------------------------------------

    SECTION("min duty is a floor while the module is active");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.min_duty_pct = 250;   // 25 %
        for (int i = 0; i < 16; i++) cfg.base_boost_duty_table[i] = 100;   // 10 % everywhere
        Boost bc; bc.init(cfg);
        CHECK(std::fabs(run(bc, 4000.0f, 130.0f, 5) - 25.0f) < 1e-3f);
    }

    SECTION("...and protection scales the floor away rather than being held up by it");
    {
        auto cfg = make_cfg(); cfg.mode = 0; cfg.min_duty_pct = 250;
        for (int i = 0; i < 16; i++) cfg.base_boost_duty_table[i] = 100;
        Boost bc; bc.init(cfg);
        // Kill boost. A fixed floor would still be driving the solenoid at 25 % here, which is the
        // gate held partly SHUT — more boost, in answer to a correction demanding none.
        CHECK(std::fabs(run(bc, 4000.0f, 130.0f, 5, /*prot=*/-100) - 0.0f) < 1e-3f);
        Boost bc2; bc2.init(cfg);
        const float half = run(bc2, 4000.0f, 130.0f, 5, /*prot=*/-50);
        CHECK(std::fabs(half - 12.5f) < 1e-3f);   // the floor falls with the correction
    }

    SECTION("the integrator ceiling caps the trim, and caps what is delivered with it");
    {
        auto cfg = make_cfg(); cfg.control_point_kpa = 200; cfg.iterm_max_pct = 50;   // 5 %
        cfg.kp = 0;                          // isolate the integral term
        Boost bc; bc.init(cfg);
        const float duty = run(bc, 4000.0f, 140.0f, 500);   // 10 kPa under target for 5 s
        // Without the ceiling the anti-windup would let this reach max_duty (90); with it the trim is
        // 5 % on top of the 40 % base and the output has to reflect the clip, not just the accumulator.
        CHECK(std::fabs(duty - 45.0f) < 0.5f);
    }

    SECTION("the closed loop min TPS keeps the loop resting on the base duty");
    {
        auto cfg = make_cfg(); cfg.min_tps_pct = 750;   // 75 %
        Boost bc; bc.init(cfg);
        // No tps on the bus at all: a gate that is set but cannot be read keeps the loop out.
        CHECK(std::fabs(run(bc, 4000.0f, 130.0f, 50) - 40.0f) < 1e-3f);
    }

    SECTION("...and lets it close once the throttle is open");
    {
        auto cfg = make_cfg(); cfg.min_tps_pct = 750;
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        float duty = -1.0f;
        for (int i = 0; i < 50; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(4000.0f, 130.0f);
            b.set(SIG_TPS, 90.0f);
            bc.update(make_pos(4000.0f), b, f);
            duty = b.get(SIG_WASTEGATE_DUTY, -1.0f);
        }
        CHECK(duty > 40.0f);
    }

    SECTION("the overboost OFFSET trips on a target the absolute limit is miles above");
    {
        auto cfg = make_cfg(); cfg.overboost_offset_kpa = 200;   // 20 kPa over target
        Boost bc; bc.init(cfg);
        EngineFrame f;
        // 180 kPa against a 150 kPa target: 30 over, and nowhere near the 250 kPa absolute ceiling.
        run(bc, 4000.0f, 180.0f, 3, 0, &f);
        CHECK(g_last_fuel_cut);
    }

    SECTION("...and does not trip on a target protection has zeroed");
    {
        auto cfg = make_cfg(); cfg.overboost_offset_kpa = 200;
        Boost bc; bc.init(cfg);
        EngineFrame f;
        // Target 0, map 130. Relative to nothing this is an infinite overshoot; it must not be read
        // that way, or every kill-boost event would ask for a fuel cut the moment it acted.
        run(bc, 4000.0f, 130.0f, 3, /*prot=*/-100, &f);
        CHECK(!g_last_fuel_cut);
    }

    SECTION("overboost backstop requests a fuel cut");
    {
        auto cfg = make_cfg(); Boost bc; bc.init(cfg);
        EngineFrame f;
        // map 260 kPa > overboost_limit 250 kPa -> fuel cut requested (cut_method 0 = fuel).
        run(bc, 4000.0f, 260.0f, 3, 0, &f);
        CHECK(g_last_fuel_cut);
        CHECK(!g_last_ign_cut);
    }

    SECTION("overboost cut stands with boost control OFF, and releases with hysteresis");
    {
        // "Spring only" — control off, unarmed, below activation — is exactly when a stuck gate creeps.
        // The backstop used to live on the active path only.
        auto cfg = make_cfg(); cfg.enabled = 0; cfg.overboost_hyst_kpa = 50;   // 5 kPa margin
        Boost bc; bc.init(cfg);
        EngineFrame f;
        run(bc, 4000.0f, 260.0f, 3, 0, &f);
        CHECK(g_last_fuel_cut);                       // off, and still cutting over 250 kPa
        run(bc, 4000.0f, 248.0f, 3, 0, &f);
        CHECK(g_last_fuel_cut);                       // 2 kPa under: still inside the release margin
        run(bc, 4000.0f, 244.0f, 3, 0, &f);
        CHECK(!g_last_fuel_cut);                      // 6 kPa under: released
        auto low = make_cfg(); low.activation_rpm = 5000;   // enabled, but below activation
        Boost b2; b2.init(low);
        run(b2, 3000.0f, 260.0f, 3, 0, &f);
        CHECK(g_last_fuel_cut);
    }

    SECTION("scramble input bumps the target");
    {
        auto cfg = make_cfg(); cfg.scramble_sig = SIG_GEAR;   // reuse a settable signal as the momentary input
        Boost bc; bc.init(cfg);
        EngineFrame f{};
        g_ms += 10;
        SignalBus b = make_bus(4000.0f, 150.0f);
        b.set(SIG_GEAR, 1.0f);   // scramble asserted
        bc.update(make_pos(4000.0f), b, f);
        CHECK(f.boost_target_kpa > 175.0f);   // 150 + 30 scramble = ~180 kPa
    }

    return test_summary();
}

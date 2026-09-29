#include "test_helpers.h"
#include "../firmware/Engine/Modules/Lambda.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "well_known_signals.h"
#include "../generated/learned_layout.h"
#include "../generated/ecu_config.h"
#include "../generated/sensors_catalog.h"
#include <initializer_list>
#include <cmath>
#include <algorithm>

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none — and Sensors then falls back to the full sweep, which
// is exactly the behaviour an unknown range is supposed to get.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;
void platform_learned_reset();
static void reset_region() { platform_learned_reset(); }

static int catalog_idx_for(SignalId sig) {
    for (uint16_t i = 0; i < SENSOR_COUNT; i++)
        if (SENSOR_CATALOG[i].primary_channel == static_cast<uint16_t>(sig)) return static_cast<int>(i);
    return -1;
}
static void clear_widebands() {
    for (uint16_t i = 0; i < SENSOR_COUNT; i++)
        if (SENSOR_CATALOG[i].type == SENSOR_TYPE_LAMBDA && SENSOR_CATALOG[i].group == SENSOR_GROUP_O2)
            g_config.sensors.sensor[i].enabled = 0;
}
static void enable_wb(SignalId sig) {
    const int i = catalog_idx_for(sig);
    if (i >= 0) g_config.sensors.sensor[i].enabled = 1;
}
// ROLES, NOT WIRES. lambda.wb[] is parallel to the wideband pool in catalogue order, so wb[k] belongs
// to the k'th lambda sensor. The loop asks "who is Overall?" — so a test that wants a sensor listened
// to has to give it the job, exactly as the wideband list page does.
// One value, and it IS the job: 0 Unassigned, 1 Overall, 2..3 Bank 1..2, 4.. Cylinder 1..12.
enum : uint8_t { WB_UNASSIGNED = 0, WB_OVERALL = 1, WB_BANK1 = 2, WB_CYL1 = 4 };
// The control's option order: 3 is THIS selector's own bank (1 for the first, 2 for the second), 4 is
// overall, and 5.. names a wideband outright without involving a role at all.
// Aligned across both selectors: 3 is THIS one's own bank, 4.. names a wideband outright, and OVERALL
// is last and exists only on Bank 1 — a sensor that sees the whole engine cannot be one bank's source.
enum : uint8_t { SRC_DISABLED = 0, SRC_NB1 = 1, SRC_NB2 = 2, SRC_OWN_BANK = 3,
                 SRC_WB1 = 4, SRC_WB2 = 5, SRC_OVERALL = 19 };
static void assign_wb(SignalId sig, uint8_t job) {
    uint8_t k = 0;
    for (uint16_t i = 0; i < SENSOR_COUNT; i++) {
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        if (d.type != SENSOR_TYPE_LAMBDA || d.group != SENSOR_GROUP_O2) continue;
        if (static_cast<SignalId>(d.primary_channel) == sig) {
            g_config.lambda.wb[k].assign = job;
            return;
        }
        ++k;
    }
}
static void clear_assignments() {
    for (unsigned k = 0; k < 15; ++k) g_config.lambda.wb[k].assign = WB_UNASSIGNED;
}
static void set_engine(uint8_t ncyl, std::initializer_list<uint8_t> banks) {
    g_config.engine.cylinder_count = ncyl;
    uint8_t c = 0;
    for (uint8_t b : banks) { if (c < ENGINE_CYL_COUNT) g_config.engine.cyl[c].bank = b; c++; }
}

// The trim borrows ve_table's axes, so the harness must give ve_table a usable grid: a zeroed axis
// would collapse every operating point into one cell.
static void set_ve_grid() {
    auto& f = g_config.fuel_calculator;
    const float rpm[8]  = {500, 1500, 2500, 3500, 4500, 5500, 6500, 8000};
    const float load[8] = {20, 50, 80, 110, 140, 180, 220, 250};
    for (int i = 0; i < 8; i++) { f.ve_table_x_axis[i] = rpm[i]; f.ve_table_y_axis[i] = load[i]; }
    f.ve_table_x_axis_n = 8; f.ve_table_y_axis_n = 8;
    f.ve_table_z_en = 0;
    f.lambda_ltft_x_src = SIG_RPM; f.lambda_ltft_y_src = SIG_MAP;
}

static LambdaConfig make_cfg() {
    LambdaConfig c = g_config.lambda;   // SHIPPED defaults, not zero: a zeroed axis has no live bin count
    c.enabled = 1; c.ltft_enabled = 1;
    // GAINS ARE A SURFACE NOW, indexed on rpm x MAP — the plant is a different beast at idle and at
    // load. Flat here so these tests stay about the controller rather than about interpolation, and
    // the axes are set because a zeroed axis puts every operating point in one cell.
    for (unsigned i = 0; i < LAMBDA_STFT_KP_TABLE_ALLOC; i++) { c.stft_kp_table[i] = 500; c.stft_ki_table[i] = 800; }
    {
        const float rpm[8] = {750, 1000, 2000, 3000, 4000, 5000, 6000, 8000};
        const float map[8] = {-100, -75, -50, -25, 0, 50, 100, 200};
        for (int i = 0; i < 8; i++) { c.lambda_gain_rpm_axis[i] = rpm[i]; c.lambda_gain_map_axis[i] = map[i]; }
    }
    c.stft_kp_table_x_src = SIG_RPM; c.stft_kp_table_y_src = SIG_MAP;
    c.stft_ki_table_x_src = SIG_RPM; c.stft_ki_table_y_src = SIG_MAP;
    c.stft_max_enrich = 200; c.stft_max_disenrich = 200;
    c.ltft_max_enrich = 250; c.ltft_max_disenrich = 250;
    c.ltft_gain = 50;
    // THE LEARN GATE, set OPEN. A zeroed LambdaConfig means learn_max_rpm = 0, which blocks
    // everything — the real defaults come from the schema, so a unit test has to state them. Each is
    // at its neutral value except the ones a test is actually about; the gate itself is covered by
    // its own section rather than by accident here.
    c.learn_min_clt = -40; c.learn_min_rpm = 0; c.learn_max_rpm = 12000;
    c.learn_run_time_s = 0; c.learn_tps_rate_limit = 0;
    c.learn_max_tps_en = 0; c.learn_max_map_en = 0;
    // WHICH SENSOR DRIVES IT is a setting now, not an inference — so a test has to say. Left at the
    // default (-1, Disabled) the loop has nothing to hear, which is the whole point of the change.
    c.o2_src_1 = SRC_OVERALL;   // and the harness gives lambda_1 that job (assign_wb)
    // Delay table: flat zero unless a test sets it, so learning lands in the CURRENT cell.
    for (unsigned i = 0; i < LAMBDA_LTFT_DELAY_TABLE_ALLOC; i++) c.ltft_delay_table[i] = 0;
    const float rpm[8]  = {500, 1500, 2500, 3500, 4500, 5500, 6500, 8000};
    const float load[8] = {20, 50, 80, 110, 140, 180, 220, 250};
    for (int i = 0; i < 8; i++) { c.ltft_delay_table_x_axis[i] = rpm[i]; c.ltft_delay_table_y_axis[i] = load[i]; }
    // the delay axes are FIXED at 8 now — no live bin count to set
    c.ltft_delay_table_x_src = SIG_RPM; c.ltft_delay_table_y_src = SIG_MAP;
    return c;                                     // ltft_learn_when left empty -> built-in safe rule
}

static EnginePosition pos(float rpm = 3000.0f) { EnginePosition p{}; p.rpm = rpm; return p; }
struct Wb { SignalId sig; float lambda; };
static SignalBus make_bus(std::initializer_list<Wb> wbs, float target = 1.0f,
                          float rpm = 3000.0f, float map = 110.0f) {
    SignalBus b{};
    b.set(SIG_LAMBDA_TARGET, target); b.set(SIG_CLT, 80.0f);
    b.set(SIG_MAP, map); b.set(SIG_RPM, rpm);
    for (const Wb& w : wbs) b.set(w.sig, w.lambda);
    return b;
}

int main() {
    fprintf(stdout, "=== Lambda (single-loop closed-loop fuel trim) ===\n");
    set_ve_grid();

    SECTION("a clashing assignment is known at BOOT, not only after a config write");
    {
        // The DTC the studio paints red is the one you see while editing. This is the one you drive
        // away with — and it was never raised, because the check ran only when g_config_generation
        // changed and nothing bumps that except a live config write from the studio. An ECU booting on
        // a stored tune with two widebands holding one job reported a clean bill of health.
        reset_region(); clear_widebands(); clear_assignments();
        enable_wb(SIG_LAMBDA_1); enable_wb(SIG_LAMBDA_2);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        assign_wb(SIG_LAMBDA_2, WB_OVERALL);          // …and so does the other one
        LambdaConfig cfg = make_cfg();
        cfg.o2_src_1 = SRC_WB1;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        g_ms = 500000;
        SignalBus b = make_bus({{SIG_LAMBDA_1, 1.00f}, {SIG_LAMBDA_2, 1.00f}});
        lc.update(pos(), b, f);                        // ONE frame, no config write anywhere
        CHECK(b.get(SIG_O2_ASSIGN_FAULT, 0.0f) == 1.0f);

        // …and one claimant is not a clash, on the same first frame.
        clear_assignments(); assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        Lambda ok; ok.init(cfg); ok.on_engine_start(); EngineFrame f2{};
        SignalBus b2 = make_bus({{SIG_LAMBDA_1, 1.00f}, {SIG_LAMBDA_2, 1.00f}});
        ok.update(pos(), b2, f2);
        CHECK(b2.get(SIG_O2_ASSIGN_FAULT, 0.0f) == 0.0f);
    }

    SECTION("the loop listens to the CHOSEN sensor, and to no other");
    {
        // This used to average every enabled wideband, which made "what is driving my mixture" a thing
        // you deduced from which sensors happened to be alive. It is a selection now. Two sensors are
        // enabled and disagree wildly; only the selected one may move the trim.
        reset_region(); clear_widebands(); clear_assignments(); set_engine(4, {1, 1, 2, 2});
        enable_wb(SIG_LAMBDA_1); enable_wb(SIG_LAMBDA_2);
        assign_wb(SIG_LAMBDA_1, WB_CYL1);                // sensor 1 watches cylinder 1 (a reference)
        assign_wb(SIG_LAMBDA_2, WB_OVERALL);                // sensor 2 is the overall
        LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0;   // isolate the fast loop
        cfg.o2_src_1 = SRC_OVERALL;                            // …so this resolves to sensor 2
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{}; float stft = 0.0f;
        // Sensor 1 says 10 % RICH, sensor 2 says 6 % LEAN. An average would pull fuel; the choice adds.
        for (int i = 0; i <= 40; i++) { g_ms = 10000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 0.90f}, {SIG_LAMBDA_2, 1.06f}});
            lc.update(pos(), b, f); stft = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f; }
        fprintf(stdout, "    stft following the chosen sensor = %.3f%%\n", (double)stft);
        CHECK(stft > 0.0f);            // the CHOSEN sensor is lean -> add fuel, whatever the other says

        // …and selecting the other one reverses it, which no averaging scheme could do.
        // …and naming sensor 1 OUTRIGHT reverses it, which no averaging scheme could do.
        LambdaConfig c2 = make_cfg(); c2.ltft_enabled = 0; c2.o2_src_1 = SRC_WB1;   // -> sensor 1
        Lambda l2; l2.init(c2); l2.on_engine_start(); EngineFrame f2{}; float s2 = 0.0f;
        for (int i = 0; i <= 40; i++) { g_ms = 20000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 0.90f}, {SIG_LAMBDA_2, 1.06f}});
            l2.update(pos(), b, f2); s2 = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f; }
        CHECK(s2 < 0.0f);              // that one is rich -> take fuel away
    }

    SECTION("the learned surface shares ve_table's grid: two operating points, two cells");
    {
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg();
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        // Learn lean at one point...
        for (int i = 0; i <= 200; i++) { g_ms = 20000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.05f}}, 1.0f, 1500.0f, 50.0f);
            lc.update(pos(1500.0f), b, f); }
        SignalBus a = make_bus({{SIG_LAMBDA_1, 1.0f}}, 1.0f, 1500.0f, 50.0f);
        g_ms = 30000; lc.update(pos(1500.0f), a, f);
        const float learned_here = a.get(SIG_LTFT_PCT, 0.0f);
        // ...and a DIFFERENT point must be untouched, which is only true if the grid is real.
        SignalBus e = make_bus({{SIG_LAMBDA_1, 1.0f}}, 1.0f, 6500.0f, 220.0f);
        g_ms = 30100; lc.update(pos(6500.0f), e, f);
        const float elsewhere = e.get(SIG_LTFT_PCT, 0.0f);
        fprintf(stdout, "    learned at 1500/50 = %.2f%%, at 6500/220 = %.2f%%\n",
                (double)learned_here, (double)elsewhere);
        CHECK(learned_here > 0.5f);
        CHECK_NEAR(elsewhere, 0.0, 0.05);
    }

    SECTION("transport delay credits a reading to the cell that MADE it");
    {
        // Sit at one point long enough to fill the history, then jump. With a 300 ms delay the
        // correction must land where the engine WAS, not where it now is -- which is the whole reason
        // a hard pull can be tuned at all.
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg();
        for (unsigned i = 0; i < LAMBDA_LTFT_DELAY_TABLE_ALLOC; i++) cfg.ltft_delay_table[i] = 300;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        for (int i = 0; i <= 30; i++) { g_ms = 40000 + i * 30;      // ~900 ms at 1500/50
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.0f}}, 1.0f, 1500.0f, 50.0f);
            lc.update(pos(1500.0f), b, f); }
        for (int i = 0; i <= 6; i++) { g_ms = 41000 + i * 30;        // jump to 6500/220, lean
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.08f}}, 1.0f, 6500.0f, 220.0f);
            lc.update(pos(6500.0f), b, f); }
        SignalBus back = make_bus({{SIG_LAMBDA_1, 1.0f}}, 1.0f, 1500.0f, 50.0f);
        g_ms = 42000; lc.update(pos(1500.0f), back, f);
        SignalBus at_new = make_bus({{SIG_LAMBDA_1, 1.0f}}, 1.0f, 6500.0f, 220.0f);
        g_ms = 42100; lc.update(pos(6500.0f), at_new, f);
        fprintf(stdout, "    after a jump: old cell = %.3f%%, new cell = %.3f%%\n",
                (double)back.get(SIG_LTFT_PCT, 0.0f), (double)at_new.get(SIG_LTFT_PCT, 0.0f));
        // The lean reading arrived within 300 ms of the jump, so it belongs to the OLD cell.
        CHECK(back.get(SIG_LTFT_PCT, 0.0f) > 0.0f);
        CHECK_NEAR(at_new.get(SIG_LTFT_PCT, 0.0f), 0.0, 0.05);
    }

    SECTION("the surface learns the correction it measured, and nothing added to it");
    {
        // There was a RICH BIAS here: the store deliberately held the true correction plus a few
        // percent while the fast loop held minus the same, the two cancelling. It was sold as an
        // open-loop margin and it was a mistake — richness belongs in the target, and a constant
        // inside a learned store is a fuel adder no map shows, which Apply to Base Table then folded
        // into the fuel map on every fold.
        //
        // A PLANT THAT ANSWERS BACK, because the claim is about where the loop SETTLES: the mixture
        // reads target x (1 + error - what it is being given), so the two corrections have to add up
        // to the error or the lambda never reaches 1.
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        LambdaConfig cfg = make_cfg();
        cfg.o2_src_1 = SRC_WB1;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        const float ERR = 0.06f;                      // the table is 6 % lean here
        float lam = 1.0f + ERR, stored = 0.0f, fast = 0.0f;
        for (int i = 0; i <= 4000; i++) { g_ms = 600000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, lam}});
            lc.update(pos(), b, f);
            stored = b.get(SIG_LTFT_PCT, 0.0f) / 100.0f;
            fast   = b.get(SIG_STFT_PCT, 0.0f) / 100.0f;
            lam    = 1.0f + ERR - (stored + fast);
        }
        fprintf(stdout, "    settled: lambda %.4f   stored %+.2f%%   fast %+.2f%%\n",
                (double)lam, (double)(stored * 100), (double)(fast * 100));
        CHECK(std::fabs(lam - 1.0f) < 0.005f);        // the mixture reaches target
        // …and the STORE holds the whole of it, with the fast loop left at nothing. That is what makes
        // Apply to Base Table exact: the cell IS what the map was missing.
        CHECK(std::fabs(stored - ERR) < 0.005f);
        CHECK(std::fabs(fast) < 0.005f);
    }

    SECTION("a throttle transient (MAP prediction active) HOLDS the trim, and says why");
    {
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{}; float wound = 1.0f;
        for (int i = 0; i <= 30; i++) { g_ms = 70000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.05f}});
            lc.update(pos(), b, f); wound = b.get(SIG_FUEL_CORR_STFT, 1.0f); }
        CHECK(wound > 1.0f);
        // Predicted MAP standing in (Map Source 1): the wideband reads the transient's lean spike. The trim
        // must not chase it — frozen, not reset, with the reason published.
        // Held means the INTEGRAL is kept and nothing is added to it; the proportional part (which reacts
        // to the reading) drops out, as it does for every hold. So: constant through the transient.
        float held = 0.0f, first_held = -1.0f, reason = -1.0f;
        for (int i = 1; i <= 10; i++) { g_ms = 71000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.20f}, {SIG_MAP_SOURCE, 1.0f}});
            lc.update(pos(), b, f); held = b.get(SIG_FUEL_CORR_STFT, 1.0f); reason = b.get(SIG_LAMBDA_CL_HOLD, -1.0f);
            if (first_held < 0.0f) first_held = held; }
        fprintf(stdout, "    trim running %.4f; held through the transient %.4f -> %.4f; hold reason %.0f\n",
                (double)wound, (double)first_held, (double)held, (double)reason);
        CHECK_NEAR(held, first_held, 0.0001);          // frozen: the lean spike added nothing
        CHECK(held > 1.0f);                            // and kept, not reset
        CHECK_NEAR(reason, 6.0, 0.01);                 // "Transient"
        // …and once the transient is over it corrects again.
        float after = 0.0f;
        for (int i = 1; i <= 10; i++) { g_ms = 72000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.20f}, {SIG_MAP_SOURCE, 0.0f}});
            lc.update(pos(), b, f); after = b.get(SIG_FUEL_CORR_STFT, 1.0f); reason = b.get(SIG_LAMBDA_CL_HOLD, -1.0f); }
        CHECK(after > held + 0.001f);
        CHECK_NEAR(reason, 0.0, 0.01);
    }

    SECTION("disabling a trim zeroes it rather than freezing it");
    {
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{}; float wound = 1.0f;
        for (int i = 0; i <= 30; i++) { g_ms = 60000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.05f}});
            lc.update(pos(), b, f); wound = b.get(SIG_FUEL_CORR_STFT, 1.0f); }
        CHECK(wound > 1.0f);
        // Switch it off on the SAME instance -- init() holds a pointer, so the module sees it live.
        // A fresh Lambda would prove nothing: its integrator starts at 0 either way.
        cfg.enabled = 0; g_ms = 61000;
        SignalBus b = make_bus({{SIG_LAMBDA_1, 1.05f}});
        lc.update(pos(), b, f);
        CHECK_NEAR(b.get(SIG_FUEL_CORR_STFT, 9.0f), 1.0, 0.001);
    }

    SECTION("target oscillation cycles a WIDEBAND about its target; zero amplitude settles on it");
    {
        // The engine as the loop sees it: 5 % lean base fuelling, corrected by the fast trim.
        auto run = [&](uint16_t amp, float& lo, float& hi) {
            reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
            assign_wb(SIG_LAMBDA_1, WB_OVERALL);
            LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0; cfg.osc_amplitude = amp;
            Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
            float corr = 1.0f; lo = 9.0f; hi = 0.0f;
            for (int i = 0; i < 3000; i++) {
                g_ms = 200000 + i * 30;
                const float lam = 1.05f / corr;
                SignalBus b = make_bus({{SIG_LAMBDA_1, lam}});
                lc.update(pos(), b, f);
                corr = b.get(SIG_FUEL_CORR_STFT, 1.0f);
                if (i >= 1500) { lo = std::min(lo, lam); hi = std::max(hi, lam); }
            }
        };
        float lo0, hi0, lo2, hi2;
        run(0, lo0, hi0);
        CHECK(hi0 - lo0 < 0.005f);                 // no swing asked for: it settles on the target
        CHECK(std::fabs(0.5f * (lo0 + hi0) - 1.0f) < 0.005f);
        run(20, lo2, hi2);                         // +/-0.020 lambda
        CHECK(hi2 > 1.015f);                       // it swings out past both sides...
        CHECK(lo2 < 0.985f);
        CHECK(std::fabs(0.5f * (lo2 + hi2) - 1.0f) < 0.01f);   // ...about the target
    }

    SECTION("the fast trim is HELD through a fuel cut and an off-range reading, not reset or driven");
    {
        // It used to run straight through a cut: the wideband sweeps lean on overrun so it added fuel,
        // and past the sensor's range it zeroed the trim. Now: frozen during the cut, frozen for the
        // re-entry delay after it, frozen (not zeroed) on a readable-but-off-range reading.
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0; cfg.cl_after_cut_ms = 1000;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        float trim = 1.0f;
        for (int i = 0; i <= 30; i++) { g_ms = 100000 + i * 30;           // lean: wind some trim in
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.05f}});
            lc.update(pos(), b, f); trim = b.get(SIG_FUEL_CORR_STFT, 1.0f); }
        CHECK(trim > 1.0f);
        float before = -1.0f;                                             // the held value (integrator only)
        for (int i = 1; i <= 20; i++) { g_ms = 101000 + i * 30;           // overrun: cut, sensor reads air
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.45f}});
            b.set_bool(wk::fuel_cut, true, g_ms, 100);
            lc.update(pos(), b, f); trim = b.get(SIG_FUEL_CORR_STFT, 1.0f);
            if (before < 0.0f) before = trim; }
        CHECK(before > 1.0f);                                             // kept, not reset to 1.000
        CHECK_NEAR(trim, before, 0.0005);                                 // held: not driven richer
        for (int i = 1; i <= 10; i++) { g_ms = 102000 + i * 30;           // just after the cut: still held
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.30f}});
            lc.update(pos(), b, f); trim = b.get(SIG_FUEL_CORR_STFT, 1.0f); }
        CHECK_NEAR(trim, before, 0.0005);
        for (int i = 1; i <= 5; i++) { g_ms = 104000 + i * 30;            // off the sensor's range: held
            SignalBus b = make_bus({{SIG_LAMBDA_1, 3.0f}});
            lc.update(pos(), b, f); trim = b.get(SIG_FUEL_CORR_STFT, 1.0f); }
        CHECK_NEAR(trim, before, 0.0005);                                 // NOT zeroed
    }

    SECTION("asymmetric authority: enrich and disenrich clamp independently");
    {
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL);
        LambdaConfig cfg = make_cfg();
        cfg.ltft_enabled = 0; cfg.stft_max_enrich = 100; cfg.stft_max_disenrich = 30;   // 10 % / 3 %
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        float lean = 0.0f;
        for (int i = 0; i <= 400; i++) { g_ms = 70000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.2f}});
            lc.update(pos(), b, f); lean = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f; }
        Lambda rc; rc.init(cfg); rc.on_engine_start(); EngineFrame rf{}; float rich = 0.0f;
        for (int i = 0; i <= 400; i++) { g_ms = 80000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 0.8f}});
            rc.update(pos(), b, rf); rich = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f; }
        fprintf(stdout, "    clamped: adding %.2f%% (limit 10), removing %.2f%% (limit 3)\n",
                (double)lean, (double)rich);
        CHECK_NEAR(lean,  10.0, 0.3);
        CHECK_NEAR(rich,  -3.0, 0.3);
    }

    SECTION("bank offsets are learned scalars, published per bank");
    {
        reset_region(); clear_widebands(); clear_assignments(); enable_wb(SIG_LAMBDA_1);
        assign_wb(SIG_LAMBDA_1, WB_OVERALL); set_engine(6, {1,1,1,2,2,2});
        LambdaConfig cfg = make_cfg();
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        g_ms = 90000; SignalBus b = make_bus({{SIG_LAMBDA_1, 1.0f}});
        lc.update(pos(), b, f);
        // Nothing learned yet -> both banks neutral, and both signals exist (FuelCalculator reads them).
        CHECK_NEAR(b.get(SIG_LTFT_BANK_1_PCT, 9.0f), 0.0, 0.001);
        CHECK_NEAR(b.get(SIG_LTFT_BANK_2_PCT, 9.0f), 0.0, 0.001);
        CHECK_NEAR(b.get(SIG_STFT_BANK_1_PCT, 9.0f), 0.0, 0.001);
    }

    // ------------------------------------------------------------------------------------------------
    SECTION("the learn gate blocks what does not belong in an rpm x load cell");
    {
        // WHAT THIS IS PROTECTING. LTFT stores against rpm x load and only learns when there IS an
        // error, so an error caused by something else gets filed under the wrong axes and stays there.
        // The rule used to be "not cutting fuel, past post-start" while its own comment and the Learn
        // While help both claimed "warm, running, past post-start" — two comments asserting a guard
        // that was not implemented.
        auto learns = [](LambdaConfig cfg, float clt, float rpm, uint32_t ms_after_start,
                         float tps_a, float tps_b) {
            reset_region(); clear_widebands(); set_engine(4, {1, 1, 2, 2});
            enable_wb(SIG_LAMBDA_1);
            Lambda lc; lc.init(cfg); g_ms = 10000; lc.on_engine_start();
            EngineFrame f{}; float ltft = 0.0f;
            for (int i = 0; i <= 60; i++) {
                g_ms = 10000 + ms_after_start + i * 30;
                SignalBus b = make_bus({{SIG_LAMBDA_1, 1.10f}}, 1.0f, rpm);
                b.set(SIG_CLT, clt);
                b.set(SIG_TPS, (i % 2) ? tps_b : tps_a);       // a throttle that may be moving
                lc.update(pos(rpm), b, f);
                ltft = b.get(SIG_LTFT_PCT, 0.0f);              // read the frame that was GATED
            }
            // Deliberately no extra settled frame: one still update after a burst of movement passes
            // the rate test on its own merits and learns a step, which would say nothing about
            // whether the moving frames were blocked.
            return ltft;
        };
        LambdaConfig open_gate = make_cfg();          // every threshold neutral (see make_cfg)
        CHECK(learns(open_gate, 80.0f, 3000.0f, 5000, 10.0f, 10.0f) > 0.0f);   // the control case

        LambdaConfig warm = make_cfg(); warm.learn_min_clt = 60;
        CHECK(learns(warm, 20.0f, 3000.0f, 5000, 10.0f, 10.0f) == 0.0f);       // cold: warm-up error
        CHECK(learns(warm, 80.0f, 3000.0f, 5000, 10.0f, 10.0f) > 0.0f);        // …warm again

        LambdaConfig band = make_cfg(); band.learn_min_rpm = 450; band.learn_max_rpm = 6000;
        CHECK(learns(band, 80.0f, 300.0f,  5000, 10.0f, 10.0f) == 0.0f);       // cranking, not running
        CHECK(learns(band, 80.0f, 7000.0f, 5000, 10.0f, 10.0f) == 0.0f);       // past the band

        LambdaConfig settle = make_cfg(); settle.learn_run_time_s = 30;
        CHECK(learns(settle, 80.0f, 3000.0f, 0, 10.0f, 10.0f) == 0.0f);        // too soon after catching

        // A THROTTLE THAT IS MOVING. 10 % -> 40 % every 30 ms frame is ~1000 %/s; the enrichment
        // correcting that belongs to dTPS, and by the time the exhaust arrives the engine has moved
        // cells, so what would be stored is an error attributed somewhere it never happened.
        LambdaConfig tr = make_cfg(); tr.learn_tps_rate_limit = 2000;          // 200.0 %/s
        CHECK(learns(tr, 80.0f, 3000.0f, 5000, 10.0f, 40.0f) == 0.0f);
        CHECK(learns(tr, 80.0f, 3000.0f, 5000, 10.0f, 10.0f) > 0.0f);          // …a steady one is fine

        // AND THE EXPRESSION IS AN EXTRA CONDITION, NOT A REPLACEMENT FOR THESE. It used to replace
        // them wholesale, so a Learn While that forgot one silently dropped a safety gate.
        LambdaConfig expr = make_cfg(); expr.learn_min_clt = 60;
        CHECK(learns(expr, 20.0f, 3000.0f, 5000, 10.0f, 10.0f) == 0.0f);
    }

    // ------------------------------------------------------------------------------------------------
    SECTION("banked: two loops, and the scopes telescope so nothing is counted twice");
    {
        // Two banks are two measurements, so they are two controllers. What each cylinder receives is
        // its bank's whole answer, split across a GLOBAL term (what both banks agree on) and a per-bank
        // DEVIATION — because the global term rides with every other correction in the fuel path, and
        // adding the bank's full answer on top of it would apply the shared part twice.
        reset_region(); clear_widebands(); clear_assignments(); set_engine(4, {1, 1, 2, 2});
        enable_wb(SIG_LAMBDA_1); enable_wb(SIG_LAMBDA_2);
        LambdaConfig cfg = make_cfg(); cfg.ltft_enabled = 0;
        cfg.o2_src_1 = SRC_WB1;                       // bank 1 listens to sensor 1
        cfg.o2_src_2 = SRC_WB2;                       // bank 2 to sensor 2  -> BANKED
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        float common = 0.0f, b1 = 0.0f, b2 = 0.0f;
        // Bank 1 lean by 8 %, bank 2 rich by 4 %: they disagree, and each must get its own answer.
        for (int i = 0; i <= 60; i++) { g_ms = 10000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.08f}, {SIG_LAMBDA_2, 0.96f}});
            lc.update(pos(), b, f);
            common = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f;
            b1 = b.get(SIG_STFT_BANK_1_PCT, 0.0f);
            b2 = b.get(SIG_STFT_BANK_2_PCT, 0.0f);
        }
        fprintf(stdout, "    common %+.2f%%   bank1 %+.2f%%   bank2 %+.2f%%\n",
                (double)common, (double)b1, (double)b2);
        CHECK(b1 > 0.0f);                       // the lean bank asks for fuel
        CHECK(b2 < 0.0f);                       // the rich bank gives it back
        // …and the deviations are equal and opposite about the common part, which is what makes
        // (common + deviation) each bank's own answer rather than an approximation of it.
        CHECK(std::fabs(b1 + b2) < 0.01f);

        // UNBANKED COLLAPSES TO ONE LOOP. Bank 2 Disabled is not "a second loop switched off", it is
        // the whole engine on one source — so the bank terms must be exactly zero, or a cylinder would
        // receive a deviation nobody computed.
        LambdaConfig un = make_cfg(); un.ltft_enabled = 0;
        un.o2_src_1 = SRC_WB1; un.o2_src_2 = SRC_DISABLED;
        Lambda lu; lu.init(un); lu.on_engine_start(); EngineFrame fu{};
        float ub1 = 1.0f, ub2 = 1.0f, uc = 0.0f;
        for (int i = 0; i <= 60; i++) { g_ms = 30000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.08f}, {SIG_LAMBDA_2, 0.96f}});
            lu.update(pos(), b, fu);
            uc  = (b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f) * 100.0f;
            ub1 = b.get(SIG_STFT_BANK_1_PCT, 0.0f);
            ub2 = b.get(SIG_STFT_BANK_2_PCT, 0.0f);
        }
        CHECK(ub1 == 0.0f);
        CHECK(ub2 == 0.0f);
        CHECK(uc > 0.0f);                       // and sensor 2 is ignored entirely: bank 1 is lean
    }

    // ------------------------------------------------------------------------------------------------
    SECTION("banked: the DIFFERENCE is learned, and the fast loop hands it over");
    {
        // A PLANT THAT RESPONDS, because the claim is about a handover and a fixed reading cannot show
        // one: with the error never going away, the store and the loop both simply wind to their
        // clamps and "the loop holds less than the store" is true for the wrong reason. So the mixture
        // here answers back — each bank reads target x (1 + its own error - what it is being given) —
        // and the assertions are about where that settles.
        reset_region(); clear_widebands(); clear_assignments(); set_engine(4, {1, 1, 2, 2});
        enable_wb(SIG_LAMBDA_1); enable_wb(SIG_LAMBDA_2);
        LambdaConfig cfg = make_cfg();
        cfg.o2_src_1 = SRC_WB1; cfg.o2_src_2 = SRC_WB2;
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};

        const float ERR1 = 0.08f, ERR2 = -0.04f;      // bank 1 genuinely lean, bank 2 genuinely rich
        float common = 0.0f, d1 = 0.0f, d2 = 0.0f, s1 = 0.0f, s2 = 0.0f;
        float lam1 = 1.0f + ERR1, lam2 = 1.0f + ERR2;
        for (int i = 0; i <= 3000; i++) { g_ms = 10000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, lam1}, {SIG_LAMBDA_2, lam2}});
            lc.update(pos(), b, f);
            common = b.get(SIG_FUEL_CORR_STFT, 1.0f) - 1.0f;
            d1 = b.get(SIG_STFT_BANK_1_PCT, 0.0f) / 100.0f;
            d2 = b.get(SIG_STFT_BANK_2_PCT, 0.0f) / 100.0f;
            s1 = b.get(SIG_LTFT_BANK_1_PCT, 0.0f) / 100.0f;
            s2 = b.get(SIG_LTFT_BANK_2_PCT, 0.0f) / 100.0f;
            // what each bank is actually being given, and therefore what its sensor now sees
            lam1 = 1.0f + ERR1 - (common + d1 + s1);
            lam2 = 1.0f + ERR2 - (common + d2 + s2);
        }
        fprintf(stdout, "    settled: lambda %.4f / %.4f   stored %+.2f%% / %+.2f%%   fast %+.2f%% / %+.2f%%\n",
                (double)lam1, (double)lam2, (double)(s1 * 100), (double)(s2 * 100),
                (double)(d1 * 100), (double)(d2 * 100));
        // THE MIXTURE REACHES TARGET on both banks — which is the only thing a tuner cares about.
        CHECK(std::fabs(lam1 - 1.0f) < 0.005f);
        CHECK(std::fabs(lam2 - 1.0f) < 0.005f);
        // …and the STORE ended up holding the difference, not the fast loop: that is the handover, and
        // it is what makes the correction survive a restart instead of being re-earned every time.
        CHECK(s1 > 0.0f && s2 < 0.0f);
        CHECK(std::fabs(d1) < std::fabs(s1));
        CHECK(std::fabs(d2) < std::fabs(s2));

        // …AND UNBANKED APPLIES NONE OF IT. Take the second sensor away and there is no measurement of
        // a difference between banks any more — but the scalars just learned are still in the store.
        // Applying them would go on skewing one bank's fuelling under a configuration that cannot
        // correct them, which is the "frozen, not zeroed" failure this module refuses everywhere else.
        {
            // NO reset_region() here on purpose: the learned scalars the run above just produced are
            // still in the region, which is the whole point of the check.
            LambdaConfig ub = make_cfg();
            ub.o2_src_1 = SRC_WB1; ub.o2_src_2 = SRC_DISABLED;
            Lambda lb; lb.init(ub); lb.on_engine_start(); EngineFrame fb{};
            float a1 = 9.0f, a2 = 9.0f;
            for (int i = 0; i <= 20; i++) { g_ms = 300000 + i * 30;
                SignalBus b = make_bus({{SIG_LAMBDA_1, 1.00f}, {SIG_LAMBDA_2, 1.00f}});
                lb.update(pos(), b, fb);
                a1 = b.get(SIG_LTFT_BANK_1_PCT, 0.0f); a2 = b.get(SIG_LTFT_BANK_2_PCT, 0.0f);
            }
            CHECK(a1 == 0.0f);
            CHECK(a2 == 0.0f);

            // …and it is RETAINED, not wiped: put the second sensor back and the difference returns.
            LambdaConfig re = make_cfg();
            re.o2_src_1 = SRC_WB1; re.o2_src_2 = SRC_WB2;
            Lambda lr; lr.init(re); lr.on_engine_start(); EngineFrame fr{};
            float r1 = 0.0f;
            for (int i = 0; i <= 5; i++) { g_ms = 400000 + i * 30;
                SignalBus b = make_bus({{SIG_LAMBDA_1, 1.00f}, {SIG_LAMBDA_2, 1.00f}});
                lr.update(pos(), b, fr);
                r1 = b.get(SIG_LTFT_BANK_1_PCT, 0.0f);
            }
            fprintf(stdout, "    unbanked applies %+.2f%% / %+.2f%%, banked again %+.2f%%\n",
                    (double)a1, (double)a2, (double)r1);
            CHECK(r1 > 0.0f);
        }

        // UNBANKED LEARNS NO BANK OFFSET. One loop, one answer — a per-bank store would be recording a
        // difference nobody measured.
        reset_region(); clear_assignments();
        LambdaConfig un = make_cfg(); un.o2_src_1 = SRC_WB1; un.o2_src_2 = SRC_DISABLED;
        Lambda lu; lu.init(un); lu.on_engine_start(); EngineFrame fu{};
        float ub1 = 9.0f, ub2 = 9.0f;
        for (int i = 0; i <= 400; i++) { g_ms = 200000 + i * 30;
            SignalBus b = make_bus({{SIG_LAMBDA_1, 1.08f}, {SIG_LAMBDA_2, 0.96f}});
            lu.update(pos(), b, fu);
            ub1 = b.get(SIG_LTFT_BANK_1_PCT, 0.0f); ub2 = b.get(SIG_LTFT_BANK_2_PCT, 0.0f);
        }
        CHECK(ub1 == 0.0f);
        CHECK(ub2 == 0.0f);
    }

    SECTION("lambda by ROLE is published, and does not depend on what the loop listens to");
    {
        // Overall / Bank 1 / Bank 2 are jobs, and until now the job existed only inside the loop's
        // source resolution — so nothing outside could ask what bank 2 was reading. These forward
        // whichever ENABLED wideband holds each job.
        reset_region(); clear_widebands(); clear_assignments();
        enable_wb(SIG_LAMBDA_1); enable_wb(SIG_LAMBDA_2); enable_wb(SIG_LAMBDA_3);
        assign_wb(SIG_LAMBDA_1, WB_BANK1);
        assign_wb(SIG_LAMBDA_2, WB_BANK1 + 1);          // Bank 2
        assign_wb(SIG_LAMBDA_3, WB_OVERALL);
        LambdaConfig cfg = make_cfg();
        Lambda lc; lc.init(cfg); lc.on_engine_start(); EngineFrame f{};
        g_ms = 500000;
        SignalBus b = make_bus({{SIG_LAMBDA_1, 0.880f}, {SIG_LAMBDA_2, 0.940f}, {SIG_LAMBDA_3, 0.910f}});
        lc.update(pos(), b, f);
        // Forwarded, not averaged: a role names ONE sensor, so the value must be that sensor's own
        // reading. An average of the three would be 0.910 — which is also Overall's real value, so
        // Bank 1 and Bank 2 are what actually distinguish the two behaviours.
        CHECK_NEAR(b.get(SIG_LAMBDA_BANK_1,  0.0f), 0.880f, 0.0005f);
        CHECK_NEAR(b.get(SIG_LAMBDA_BANK_2,  0.0f), 0.940f, 0.0005f);
        CHECK_NEAR(b.get(SIG_LAMBDA_OVERALL, 0.0f), 0.910f, 0.0005f);

        // THE INDEPENDENCE THAT MATTERS. Point the loop at nothing at all: the roles still answer,
        // because a dash asking "what is bank 2 reading" is not asking what the controller is using.
        // Reusing the loop's resolved source here would publish nothing and pass every check above.
        clear_assignments();
        assign_wb(SIG_LAMBDA_1, WB_BANK1);
        assign_wb(SIG_LAMBDA_2, WB_BANK1 + 1);
        LambdaConfig off = make_cfg();
        off.enabled = 0; off.o2_src_1 = SRC_DISABLED; off.o2_src_2 = SRC_DISABLED;
        Lambda lo; lo.init(off); lo.on_engine_start(); EngineFrame f2{};
        SignalBus b2 = make_bus({{SIG_LAMBDA_1, 0.880f}, {SIG_LAMBDA_2, 0.940f}});
        lo.update(pos(), b2, f2);
        CHECK_NEAR(b2.get(SIG_LAMBDA_BANK_1, 0.0f), 0.880f, 0.0005f);
        CHECK_NEAR(b2.get(SIG_LAMBDA_BANK_2, 0.0f), 0.940f, 0.0005f);
        // …and a job nobody holds stays UNPUBLISHED rather than reading a confident 1.00.
        CHECK(!b2.valid(SIG_LAMBDA_OVERALL));
    }

    return test_summary();
}

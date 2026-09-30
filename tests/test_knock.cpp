#include "test_helpers.h"
#include "../firmware/Engine/Modules/Knock.h"
#include "../firmware/Engine/Modules/Ignition.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/learned_layout.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "../generated/ecu_config.h"
#include "../generated/module_dtc.h"

extern "C" uint32_t g_stub_tick_ms;   // host-controllable ms clock (platform_hal_stub)

// The learned region the noise-floor map lives in. Sized to the whole layout so every per-cylinder
// slice maps; zeroed between sections so each starts from "never learned".
void platform_learned_reset();   // the one host learned region (platform_hal_stub)
static void reset_region() { platform_learned_reset(); }

// Threshold is now a RATIO over the learned floor, so the config carries tables rather than a scalar.
// Flat surfaces keep these tests about the controller rather than about interpolation.
static KnockConfig make_cfg(bool enabled) {
    KnockConfig c{};
    // The learned noise map is a table, so its grid comes from its own axis arrays (shared by every
    // per-cylinder map). Set them as the schema's defaults do — a zeroed axis is a misconfiguration,
    // not a default, and would put every load in one cell.
    {
        const float rpm[]  = { 0.0f, 1000.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f };
        const float load[] = { 0.0f, 31.25f, 62.5f, 93.75f, 125.0f, 156.25f, 187.5f, 218.75f };
        for (unsigned i = 0; i < sizeof(rpm)/sizeof(*rpm); ++i)   c.knock_noise_1_x_axis[i] = rpm[i];
        for (unsigned i = 0; i < sizeof(load)/sizeof(*load); ++i) c.knock_noise_1_y_axis[i] = load[i];
    }
    c.enabled             = enabled ? 1 : 0;
    c.source              = 0;     // onboard (measurement driven)
    c.retard_step_deg     = 50;    // 0.5 deg/event (scale 0.01)
    c.retard_reapply_rate = 100;   // 1.0 deg/s (scale 0.01)
    c.suppress_min_tps    = 50;    // 5.0 %
    c.noise_seed_db       = -350;  // -35.0 dB assumed floor until a cell is learned (scale 0.1)
    c.learn_rate          = 50;    // 0.05 EMA once established (scale 0.001)
    c.learn_fast_n        = 20;    // 1/n averaging below this many samples
    c.learn_dwell_ms      = 0;     // no dwell requirement in these tests unless a section sets it
    c.learn_min_rpm       = 500;
    // Pre-ignition defaults, mirroring the schema. Zero-init would make preign_extreme_db 0 — i.e.
    // EVERY event extreme — and preign_cut_fuel 0, i.e. a confirmed event that never actually cuts.
    c.preign_enabled      = 0;     // sections that test it turn it on
    c.preign_pre_frac     = 35;    // % of window energy before the spark
    c.preign_margin_db    = 60;    // 6.0 dB over the knock threshold
    c.preign_extreme_db   = 300;   // 30.0 dB over the floor -> pre-ignition regardless of phase
    c.preign_events_to_act = 2;
    c.preign_cut_fuel     = 1;
    c.preign_cut_spark    = 0;
    c.preign_clear_s      = 0;     // latch until engine stop

    c.knock_noise_1_x_axis_n = 8;   // the learned noise map's own axes are resizable too
    c.knock_noise_1_y_axis_n = 8;
    c.knock_rpm_axis_n  = 8;
    c.knock_load_axis_n = 8;
    for (int i = 0; i < 8; ++i) {
        c.knock_rpm_axis[i]  = 800.0f + 1000.0f * static_cast<float>(i);
        c.knock_load_axis[i] = 20.0f  + 30.0f   * static_cast<float>(i);
    }
    c.knock_threshold_table_x_src = static_cast<int16_t>(wk::rpm);
    c.knock_threshold_table_y_src = static_cast<int16_t>(wk::fuel_load);
    c.knock_max_retard_table_x_src = static_cast<int16_t>(wk::rpm);
    c.knock_max_retard_table_y_src = static_cast<int16_t>(wk::fuel_load);
    for (int i = 0; i < 64; ++i) {
        c.knock_threshold_table[i]  = 60;   // 6.0 dB over the floor (scale 0.1)
        c.knock_max_retard_table[i] = 80;   // 8.0 deg (scale 0.1)
    }
    return c;
}

// One update() at adequate TPS lifts suppression and resolves the operating point. dt is 0 while the
// stub clock is stationary, so it does not decay accumulated retard.
static void arm(Knock& k, SignalBus& bus, EnginePosition& pos, EngineFrame& fr) {
    bus.set(wk::tps, 50.0f);
    bus.set(wk::fuel_load, 100.0f);
    bus.set(wk::rpm, 4000.0f);
    k.update(pos, bus, fr);
}

// Establish a floor for `cyl` in the live cell before asking for a verdict. Necessary, not
// ceremonial: the first cycle in any cell teaches instead of being judged (an unlearned cell has no
// basis for a verdict), so a test that wants detection has to supply evidence first — exactly what a
// real engine does on its first visit to an operating point.
static void teach(Knock& k, uint8_t cyl, float db, int n = 25) {
    for (int i = 0; i < n; ++i) k.on_knock_sense(cyl, db);
}

int main() {
    fprintf(stdout, "=== Knock ===\n");
    EngineFrame frame{}; EnginePosition pos{}; pos.rpm = 4000;

    SECTION("disabled -- loud knock produces no retard");
    {
        reset_region();
        auto cfg = make_cfg(false);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::tps, 50.0f);
        k.update(pos, bus, frame);
        k.on_knock_sense(0, 20.0f);   // loud, but the controller is off
        CHECK(k.current_retard() == 0.0f);
    }

    // ---- The ratio: measured against the cylinder's own floor, never an absolute -----------------

    SECTION("an unlearned cell LISTENS rather than judging -- and that is what breaks the trap");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        CHECK(k.noise_samples(0) == 0.0f);            // nothing has taught this cell
        CHECK_NEAR(k.noise_floor(0), -35.0f, 0.01f);  // so it reads the seed

        // -10 dB is 25 dB over the seed and would trip any threshold — but with no evidence the
        // verdict is unfounded, so the cycle teaches instead. THE TRAP THIS AVOIDS: judge against the
        // seed and a cylinder whose real floor is above it knocks on every cycle forever, while a
        // knocking cycle is never allowed to teach — permanently deaf, permanently retarding, on an
        // engine doing nothing wrong.
        k.on_knock_sense(0, -10.0f);
        CHECK(k.knock_count() == 0);                  // listened, did not judge
        CHECK_NEAR(k.noise_floor(0), -10.0f, 0.01f);  // and the floor is now the truth
        CHECK(k.noise_samples(0) == 1.0f);

        // With evidence in hand the same level is correctly silent, and a real excursion is caught.
        k.on_knock_sense(0, -10.0f);
        CHECK(k.knock_count() == 0);
        k.on_knock_sense(0, -2.0f);                   // 8 dB over its own floor
        CHECK(k.knock_count() == 1);
    }

    SECTION("a LOUD absolute level is NOT knock once the floor has learned to be that loud");
    {
        // The heart of the redesign. -10 dB would trip any absolute threshold set for a quiet engine,
        // but if this cylinder always sounds like -10 dB here then it is not knocking, it is idling.
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        for (int i = 0; i < 30; ++i) k.on_knock_sense(0, -10.0f);   // teach the floor
        CHECK_NEAR(k.noise_floor(0), -10.0f, 0.5f);
        const uint16_t before = k.knock_count();
        k.on_knock_sense(0, -10.0f);
        CHECK(k.knock_count() == before);          // same loud level, now correctly ignored
        k.on_knock_sense(0, -2.0f);                // 8 dB over ITS OWN floor -> knock
        CHECK(k.knock_count() == before + 1);
    }

    SECTION("a knocking cycle never teaches the floor");
    {
        // A reference that learns from knock chases it upward and goes deaf. Assert it explicitly.
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        for (int i = 0; i < 30; ++i) k.on_knock_sense(0, -30.0f);   // establish a floor at -30
        const float floor_before = k.noise_floor(0);
        const float n_before     = k.noise_samples(0);
        for (int i = 0; i < 20; ++i) k.on_knock_sense(0, 10.0f);    // sustained heavy knock
        CHECK(k.noise_floor(0) == floor_before);    // floor untouched
        CHECK(k.noise_samples(0) == n_before);      // and it did not count as evidence
    }

    SECTION("each cylinder learns its own floor");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        for (int i = 0; i < 30; ++i) { k.on_knock_sense(0, -30.0f); k.on_knock_sense(4, -12.0f); }
        CHECK_NEAR(k.noise_floor(0), -30.0f, 0.5f);
        CHECK_NEAR(k.noise_floor(4), -12.0f, 0.5f);
        // -20 dB is knock on the quiet cylinder and silence on the loud one — the same absolute level.
        const uint16_t before = k.knock_count();
        k.on_knock_sense(0, -20.0f);   // 10 dB over its floor
        CHECK(k.knock_count() == before + 1);
        k.on_knock_sense(4, -20.0f);   // BELOW its floor
        CHECK(k.knock_count() == before + 1);
    }

    SECTION("fast-learn converges a fresh cell in a few cycles, not at the EMA crawl");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        k.on_knock_sense(0, -20.0f);                 // first evidence is taken whole
        CHECK_NEAR(k.noise_floor(0), -20.0f, 0.01f);
        CHECK(k.noise_samples(0) == 1.0f);
        for (int i = 0; i < 4; ++i) k.on_knock_sense(0, -20.0f);
        CHECK_NEAR(k.noise_floor(0), -20.0f, 0.01f); // stays put on consistent evidence
    }

    SECTION("different cells learn independently -- a transient does not drag one floor everywhere");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        for (int i = 0; i < 30; ++i) k.on_knock_sense(0, -30.0f);   // low-load cell
        CHECK_NEAR(k.noise_floor(0), -30.0f, 0.5f);

        bus.set(wk::fuel_load, 220.0f);              // move to a high-load cell
        k.update(pos, bus, frame);
        CHECK(k.noise_samples(0) == 0.0f);           // the new cell knows nothing yet
        CHECK_NEAR(k.noise_floor(0), -35.0f, 0.01f); // so it reads the seed, not the other cell

        bus.set(wk::fuel_load, 100.0f);              // and back
        k.update(pos, bus, frame);
        CHECK_NEAR(k.noise_floor(0), -30.0f, 0.5f);  // the original cell kept what it learned
    }

    // ---- Retard behaviour (unchanged model, now driven by tables) --------------------------------

    SECTION("knock above threshold -- retard accumulates by step, then clamps to max");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);                                      // floor established at -30 dB
        for (int i = 0; i < 5; ++i) k.on_knock_sense(0, 20.0f);   // 50 dB over that floor
        CHECK_NEAR(k.current_retard(), 2.5f, 0.01f);              // 5 events * 0.5 deg
        CHECK(k.knock_count() == 5);
        for (int i = 0; i < 100; ++i) k.on_knock_sense(0, 20.0f);
        CHECK_NEAR(k.current_retard(), 8.0f, 0.01f);              // clamped to the max-retard table
    }

    SECTION("quiet -- retard decays at the recovery rate");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        for (int i = 0; i < 40; ++i) k.on_knock_sense(0, 20.0f);
        CHECK_NEAR(k.current_retard(), 8.0f, 0.01f);
        g_stub_tick_ms += 2000;                       // 1.0 deg/s * 2 s = 2.0 deg -> 6.0
        k.update(pos, bus, frame);
        CHECK_NEAR(k.current_retard(), 6.0f, 0.05f);
    }

    SECTION("suppressed (TPS gate or ANY fuel cut) -- retard recovers at the rate, never snaps to zero");
    {
        // A fuel cut is not only overrun — the rev limiter, launch, protections all publish wk::fuel_cut.
        // Zeroing the retard there handed a knocking engine its timing back on every limiter touch.
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        for (int i = 0; i < 40; ++i) k.on_knock_sense(0, 20.0f);
        CHECK_NEAR(k.current_retard(), 8.0f, 0.01f);
        bus.set_bool(wk::fuel_cut, true, g_stub_tick_ms, 5000);   // e.g. the rev limiter cutting fuel
        g_stub_tick_ms += 2000;                                   // 1.0 deg/s * 2 s
        k.update(pos, bus, frame);
        CHECK_NEAR(k.current_retard(), 6.0f, 0.05f);              // recovering, NOT zero
        const float held = k.current_retard();
        k.on_knock_sense(0, 20.0f);                               // suppressed -> ignored
        CHECK_NEAR(k.current_retard(), held, 0.001f);
    }

    SECTION("a CONSTANT per-cylinder gain now cancels out -- the learned floor absorbs it");
    {
        reset_region();
        auto cfg = make_cfg(true);
        // Worth pinning down, because it changes what that field is FOR. The gain is applied to the
        // measurement, and the floor is learned FROM trimmed measurements, so a constant trim shifts
        // both by the same amount and the difference — the thing actually thresholded — is unchanged.
        // Compensating sensor distance is exactly what the per-cylinder floor already does; the trim
        // survives as a manual override, not as something detection depends on.
        cfg.cyl_sensor[3].gain = 100;    // +10.0 dB trim on cyl 3 (scale 0.1)
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 5, -35.0f); teach(k, 3, -35.0f);
        CHECK_NEAR(k.noise_floor(5), -35.0f, 0.5f);   // untrimmed cylinder learns what it heard
        CHECK_NEAR(k.noise_floor(3), -25.0f, 0.5f);   // trimmed one learns the TRIMMED level

        // Same physical excursion on both -> same verdict on both, the gain notwithstanding.
        k.on_knock_sense(5, -32.0f);   // 3 dB over its floor -> under the 6 dB threshold
        CHECK(k.knock_count() == 0);
        k.on_knock_sense(3, -32.0f);   // trimmed to -22 against a -25 floor -> also 3 dB -> also under
        CHECK(k.knock_count() == 0);
        k.on_knock_sense(3, -25.0f);   // trimmed to -15 against a -25 floor -> 10 dB -> knock
        CHECK(k.knock_count() == 1);
        CHECK_NEAR(k.current_retard(), 0.5f, 0.01f);
    }

    // ---- The worker mailbox: measurements cross a task boundary, verdicts do not ------------------

    SECTION("posted measurements are classified when update() drains them, not before");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        CHECK(k.post_measurement(0, 20.0f, KnockProfile{}));
        CHECK(k.post_measurement(0, 20.0f, KnockProfile{}));
        CHECK(k.knock_count() == 0);              // posting alone decides nothing
        k.update(pos, bus, frame);
        CHECK(k.knock_count() == 2);              // the drain is where the verdict happens
        CHECK_NEAR(k.current_retard(), 1.0f, 0.01f);
    }

    SECTION("a full ring drops measurements rather than blocking the burst worker");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        int posted = 0;
        for (int i = 0; i < 64; ++i) if (k.post_measurement(0, 20.0f, KnockProfile{})) ++posted;
        CHECK(posted < 64);            // the ring is bounded
        CHECK(k.dropped() > 0);        // and says so rather than silently overwriting
        k.update(pos, bus, frame);
        CHECK(k.knock_count() == static_cast<uint16_t>(posted));
    }

    SECTION("external source -- update() reads the configured intensity signal");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.source = 1;                            // external
        cfg.external_intensity_sig = SIG_KNOCK_1;  // reuse as the external dB input for the test
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{};
        bus.set(wk::tps, 50.0f); bus.set(wk::fuel_load, 100.0f); bus.set(wk::rpm, 4000.0f);
        bus.set(SIG_KNOCK_1, -30.0f, true);        // quiet: establishes the floor for this cell
        for (int i = 0; i < 25; ++i) k.update(pos, bus, frame);
        bus.set(SIG_KNOCK_1, 20.0f, true);         // now 50 dB over that floor
        for (int i = 0; i < 4; ++i) k.update(pos, bus, frame);   // each tick reads + senses (dt=0, no decay)
        CHECK_NEAR(k.current_retard(), 2.0f, 0.01f);             // 4 * 0.5 deg
        CHECK(k.knock_count() == 4);
    }

    // ---- Pre-ignition: a DIFFERENT fault, with the opposite response ----------------------------

    // A profile whose energy sits entirely before `split_deg`, or entirely after it. Angles are
    // ATDC-positive, so a spark at 20 BTDC is -20.
    static auto profileAt = [](float from_deg, float to_deg, float loud_from, float loud_to) {
        KnockProfile p{};
        p.count = 16;
        p.start_deg = -40.0f;
        p.step_deg  = 5.0f;          // 16 buckets spanning -40 .. +40 deg
        for (unsigned i = 0; i < p.count; ++i) {
            const float a = p.angleOf(i);
            const bool loud = (a >= from_deg && a < to_deg);
            p.db[i] = loud ? loud_from : loud_to;
        }
        p.overall_db = loud_from;
        return p;
    };

    SECTION("energy AFTER the spark is knock -- retard, no cut, no DTC");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        // Spark at 20 BTDC = -20 deg. All the energy sits after it.
        auto p = profileAt(-10.0f, 40.0f, 0.0f, -60.0f);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.knock_count() == 1);              // counted as knock
        CHECK(k.current_retard() > 0.0f);         // and answered with retard
        CHECK(k.preign_cuts() == 0);              // nothing cut
    }

    SECTION("the SAME magnitude BEFORE the spark is pre-ignition -- cut, and never retard");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        // Same level, energy now entirely before the -20 deg spark. Phase is the ONLY difference.
        auto p = profileAt(-40.0f, -20.0f, 0.0f, -60.0f);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.preign_cuts() == 0x1);            // cylinder 0 cut
        CHECK(k.current_retard() == 0.0f);        // retard is the WRONG answer and was not applied
        CHECK(k.knock_count() == 0);              // and it is not counted as knock either
        CHECK(k.preign_events(0) == 1);
        CHECK(k.last_pre_frac() > 0.9f);
    }

    SECTION("pre-spark energy alone is not enough -- it must also be loud");
    {
        // The conjunction. A noisy sensor leaks energy into the pre-spark buckets; without the
        // magnitude test that alone would report pre-ignition on an engine that is merely knocking.
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        cfg.preign_margin_db = 200;               // 20 dB over the knock threshold
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        auto p = profileAt(-40.0f, -20.0f, -20.0f, -60.0f);
        k.on_knock_sense(0, -20.0f, &p);          // 10 dB over floor: knock, but not 26 dB over
        CHECK(k.preign_cuts() == 0);              // not called pre-ignition
        CHECK(k.knock_count() == 1);              // treated as knock instead
    }

    SECTION("an EXTREME event is pre-ignition whatever the phase says -- fail toward protection");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        cfg.preign_extreme_db = 300;              // 30 dB over the floor
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        // Energy squarely AFTER the spark — phase says knock — but violent enough that guessing wrong
        // costs a piston. The safe error is the cut.
        auto p = profileAt(0.0f, 40.0f, 10.0f, -60.0f);
        k.on_knock_sense(0, 10.0f, &p);           // 40 dB over the floor
        CHECK(k.preign_cuts() == 0x1);
        CHECK(k.current_retard() == 0.0f);
    }

    SECTION("a lone event does not cut when confirmation is required");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 3;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        auto p = profileAt(-40.0f, -20.0f, 0.0f, -60.0f);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.preign_cuts() == 0);
        CHECK(k.preign_events(0) == 1);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.preign_cuts() == 0);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.preign_cuts() == 0x1);            // third confirms
    }

    SECTION("the cut names the RIGHT cylinder");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 5, -30.0f);
        auto p = profileAt(-40.0f, -20.0f, 0.0f, -60.0f);
        k.on_knock_sense(5, 0.0f, &p);
        CHECK(k.preign_cuts() == (1u << 5));
        CHECK(k.preign_events(5) == 1);
        CHECK(k.preign_events(0) == 0);
    }

    SECTION("no phase information -> no pre-ignition verdict, it stays knock");
    {
        // The external-intensity source has no profile by construction. Guessing at phase there would
        // be inventing evidence, so the conjunction simply cannot fire.
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 1;
        cfg.preign_events_to_act = 1;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        k.on_knock_sense(0, 0.0f, nullptr);
        CHECK(k.preign_cuts() == 0);
        CHECK(k.knock_count() == 1);
        CHECK(k.last_pre_frac() < 0.0f);          // says "unknown" rather than 0
    }

    SECTION("disabled pre-ignition detection leaves everything as knock");
    {
        reset_region();
        auto cfg = make_cfg(true);
        cfg.preign_enabled = 0;
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; bus.set(wk::advance, 20.0f); arm(k, bus, pos, frame);
        teach(k, 0, -30.0f);
        auto p = profileAt(-40.0f, -20.0f, 0.0f, -60.0f);
        k.on_knock_sense(0, 0.0f, &p);
        CHECK(k.preign_cuts() == 0);
        CHECK(k.knock_count() == 1);
    }

    SECTION("switching the module off retires the DTCs it raised");
    {
        // The bug this covers: update() returns early when the enable flag is off, and every heal()
        // lives BELOW that return. So a code raised while knock was enabled stayed ACTIVE for ever —
        // the only thing that could retire it was the module that no longer runs. Reported as
        // "enable knock, then disable, and the DTC still shows current".
        reset_region();
        auto cfg = make_cfg(true);
        cfg.suppress_min_tps = 50;              // opt in to the throttle gate so it has a fault to find
        Ignition ign; Knock k; k.init(cfg, &ign);
        DtcManager dtc; dtc.init(1); k.set_dtc(&dtc);

        SignalBus bus{};
        bus.set(wk::fuel_load, 100.0f);
        bus.set(wk::rpm, 4000.0f);              // deliberately no wk::tps — the gate cannot be answered
        k.update(pos, bus, frame);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_TPS) != 0);   // raised while it was running

        // The switch the user actually flicks: the module holds a POINTER into the config, so writing
        // the flag is what a burn does. No re-init, no reconstruction.
        cfg.enabled = 0;
        k.update(pos, bus, frame);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_TPS) == 0);   // and retired on the way out

        // STORED, not erased: turning the detector off is not evidence the fault never happened.
        CHECK(dtc.stored_count() == 1);
    }

    SECTION("sensor health: a mapped sensor that stops reporting is raised; an unused one never is");
    {
        reset_region();
        auto cfg = make_cfg(true);
        g_config.engine.cylinder_count = 4;
        for (auto& c : cfg.cyl_sensor) c.input = 0;              // every cylinder on Knock 1
        Ignition ign; Knock k; k.init(cfg, &ign);
        DtcManager dtc; dtc.init(1); dtc.set_active(true); k.set_dtc(&dtc);
        SignalBus bus{};
        bus.set(wk::fuel_load, 100.0f); bus.set(wk::rpm, 4000.0f); bus.set(wk::tps, 50.0f);
        bus.set(SIG_KNOCK_1, -20.0f, true, g_stub_tick_ms, 1000u);   // sensor 1 reporting
        k.update(pos, bus, frame);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_1) == 0);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_2) == 0);       // no cylinder uses Knock 2: not a fault
        bus.invalidate(SIG_KNOCK_1);                              // the bursts stop (it expired)
        // …BUT NOT WHILE SPARK IS CUT: no combustion, no windows armed, nothing for any sensor to hear. A
        // protection cut after a trigger fault reported P1750 as well, a consequence posing as a fault.
        bus.set(wk::ign_cut, 1.0f, true, g_stub_tick_ms, 1000u);
        g_stub_tick_ms += 100; k.update(pos, bus, frame);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_1) == 0);
        bus.invalidate(wk::ign_cut);                              // combustion again, still no bursts
        g_stub_tick_ms += 100; k.update(pos, bus, frame);
        CHECK(dtc.code_severity(ModuleDtc::KNOCK_1) != 0);

        // Opt-in QUIET check: present, but reading like an unplugged input.
        auto q = make_cfg(true); q.quiet_check_en = 1; q.quiet_db = -600;   // -60 dB
        for (auto& c : q.cyl_sensor) c.input = 0;
        Knock k2; k2.init(q, &ign);
        DtcManager d2; d2.init(1); d2.set_active(true); k2.set_dtc(&d2);
        for (int t = 0; t < 15; ++t) {
            g_stub_tick_ms += 100;
            bus.set(SIG_KNOCK_1, -80.0f, true, g_stub_tick_ms, 1000u);   // far too quiet
            k2.update(pos, bus, frame);
        }
        CHECK(d2.code_severity(ModuleDtc::KNOCK_1) != 0);
    }

    SECTION("knock retard is PER CYLINDER: only the knocking one loses timing");
    {
        // One global retard meant a single noisy cylinder retarded all twelve.
        reset_region();
        auto cfg = make_cfg(true);
        Ignition ign; Knock k; k.init(cfg, &ign);
        SignalBus bus{}; arm(k, bus, pos, frame);
        teach(k, 2, -30.0f);
        for (int i = 0; i < 10; ++i) k.on_knock_sense(2, 20.0f);      // cylinder 3 (index 2) knocks
        CHECK(k.cylinder_retard(2) > 0.0f);
        CHECK_NEAR(k.cylinder_retard(0), 0.0f, 0.001f);               // the others have not
        CHECK_NEAR(k.current_retard(), k.cylinder_retard(2), 0.001f); // published = the worst
        k.update(pos, bus, frame);                                    // hands the per-cylinder set to Ignition
        CHECK_NEAR(k.cylinder_retard(0), 0.0f, 0.001f);
    }

    return test_summary();
}

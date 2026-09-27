#pragma once

#include "../EngineModule.h"
#include "../PiController.h"
#include "../../../generated/signal_ids.h"          // SignalId
#include "../../../generated/modules/lambda_config.h"

// ---------------------------------------------------------------------------
// Lambda — closed-loop fuel trim. ONE loop, not fifteen.
//
// Every enabled wideband is a reading of the same quantity: how far the mixture is from target. They
// average into one error, one PI corrects it (STFT), and one learned surface remembers what that
// correction keeps having to be (LTFT). That is the whole module.
//
//   STFT   the fast authority. NEVER STORED — it is runtime state and dies with the key.
//   LTFT   a learned surface that mirrors ve_table exactly: same axis arrays, same signal ids, same
//          allocation. So LTFT cell (i,j,k) IS VE cell (i,j,k), and the studio's Apply to Base Table
//          is a copy rather than a resample.
//
// EACH HAS ITS OWN SWITCH, because a tuner wants them independently: the learned surface applied with
// the fast loop stopped (measuring how wrong the tables are), or the fast loop chasing a change with
// nothing baked in behind it. Both off is open loop. OFF ZEROES the correction rather than freezing
// it — a trim still applying its last value after being switched off corrects the engine invisibly
// and hides the very error an autotune is trying to measure.
//
// LTFT applies whenever it is enabled, conditions or not: it helps before the O2 has lit, which is
// most of its value. What the conditions gate is LEARNING, and that is one expression the tuner
// writes (ltft_learn_when) rather than a list of scalars someone guessed at in advance.
//
// THE SURFACE HOLDS THE CORRECTION IT MEASURED, exactly. There was a rich bias here once — the store
// deliberately a few percent high, the fast loop the same amount low, the two cancelling — sold as an
// open-loop safety margin. Richness belongs in the TARGET, which is indexed and visible; a constant
// inside a learned store is a fuel adder no map shows, it costs the fast loop authority to hold, and
// Apply to Base Table folded it into the map on every fold.
//
// BANK DIFFERENCE IS TWO SCALARS, not a second surface. It is dominated by injector flow variance and
// manifold bias, which is close to a constant offset. A single-bank engine uses index 0 and that is
// the whole trim. Delivery rides the injector bank grouping, which cannot address banks separately in
// MULTI_POINT injection — there the two collapse to their mean.
//
// There is NO per-cylinder learned trim. Per-cylinder widebands are reference sensors: they publish
// telemetry so a tuner can set the manual cylN_fuel_corr_tables from real per-cylinder evidence.
//
// Runs BEFORE FuelCalculator so this frame's trims are fresh; reads lambda_target one frame stale
// (it changes slowly).
// ---------------------------------------------------------------------------

class DtcManager;

class Lambda : public EngineModule {
public:
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }
    // The running clock the learn gate's "Initial Engine Running Time" measures from. EngineTask
    // dispatches this on the transition INTO running, which is the only place that edge is known.
    void on_engine_start() override;
    void on_engine_stop()  override;

    void init(const LambdaConfig& cfg);
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    static constexpr uint8_t MAX_BANKS = 2;
    // Recent operating points, so a reading can be credited to the cell that MADE it. Sized to cover
    // the delay table's ceiling (1000 ms) at this module's cadence (1 kHz / 30 ~= 33 Hz) with margin.
    static constexpr uint8_t HIST = 40;
    static constexpr uint8_t MAX_WB    = 15;   // == LAMBDA_SIGNAL_COUNT (the wideband input pool)
    // Percent per stored count, for both the surface and the bank scalars. int16 rather than f32
    // because a trim shaped like the table it corrects does not fit in RAM as floats; 0.01 % spans
    // ±327 %, six times the widest authority the config allows.
    static constexpr float   LTFT_SCALE = 0.01f;

private:
    const LambdaConfig* cfg_ = nullptr;

    PiController stft_;                  // bank 1's fast loop (the only one when unbanked)
    // BANK 2'S OWN LOOP. Two banks are two measurements, so they are two controllers with two
    // integrators — sharing one would mean each bank's wind-up was the other's starting point.
    PiController stft2_;
    // …and its own narrowband state, for the same reason: the switching loop's side, its commanded
    // swing and its readiness are facts about ONE sensor.
    bool     nb2_rich_ = false, nb2_seen_ = false, osc2_hi_ = false, nb2_warm_ = false;
    int16_t*     ltft_ = nullptr;        // the learned surface, in the RAM learned region (SD-totem backed)
    int16_t*     bank_ = nullptr;        // [MAX_BANKS] learned bank offsets, same region, same scale

    // WHERE THE ENGINE HAS BEEN. A wideband reads gas that left the cylinder some time ago, so the
    // sample arriving now belongs to an earlier cell. Refusing to learn until the point settles would
    // make the whole boost region unlearnable on a fast engine — the cells that matter most — so the
    // recent positions are kept instead and the reading is credited backwards through them.
    struct Step { uint32_t ms; uint16_t cell; };
    Step    hist_[HIST]{};
    uint8_t hist_n_ = 0;                 // ring write cursor
    bool    hist_full_ = false;

    // Sub-count remainder carried to the next learn step. Without it a step smaller than half a count
    // rounds to nothing and learning STALLS — and the smaller the error, the likelier that is, so it
    // would fail exactly where a settled trim spends its life. Belongs to the cell it accumulated in;
    // cleared when the operating point moves to another.
    float    pend_ = 0.0f;

    // THE CHOSEN SOURCE, resolved from o2_src_1 on the config edge. One sensor, and its TYPE decides
    // the error domain — there is no list to average and no priority to infer.
    SignalId src_sig_  = SIG_NONE;
    bool     src_wide_ = false;          // a lambda-type sensor: error in lambda, target from the table
    bool     src_narrow_ = false;        // a narrowband: error in volts, target from nb_target_mv
    bool     assign_clash_ = false;      // two enabled widebands claim one role — published, not hidden
    // The sensor holding each of Overall / Bank 1 / Bank 2, resolved on the config edge. Separate from
    // src_sig_: that is what the LOOP listens to, this is what the role IS, and a dash asking for bank 2
    // must get an answer even when the loop is open or pointed elsewhere.
    SignalId role_sig_[3] = { SIG_NONE, SIG_NONE, SIG_NONE };
    // BANK 2 DISABLED = UNBANKED: one loop, one source, the whole engine. Set = a loop per bank.
    bool     banked_    = false;
    bool     src_fault_ = false;   // a source that resolves to nothing, or a banked pair of two kinds
    SignalId src2_sig_  = SIG_NONE;
    bool     src2_wide_ = false, src2_narrow_ = false;
    DtcManager* dtc_ = nullptr;

    // --- NARROWBAND SWITCHING LOOP ------------------------------------------------------------------
    // A narrowband cannot be a second kind of wideband, so it is not kept in wb_[]: it measures which
    // SIDE of stoichiometry the mixture is on, and the loop that consumes it ramps and jumps rather
    // than stepping on an error. Its own list, its own state, its own arming rule.
    bool     nb_warm_   = false;         // has this sensor EVER reached the warm voltage? A cold cell
                                         // cannot source EMF, and a warming one sits near the switch
                                         // point looking like a live reading of stoichiometry
    bool     osc_hi_    = false;         // which way the commanded oscillation is currently swinging
    bool     nb_rich_   = false;         // which side the sensor was on last frame (for the jump)
    bool     nb_seen_   = false;         // …and whether we have a previous side at all yet
    uint8_t  nb_state_  = 0;

    // --- LEARN GATE state ---------------------------------------------------------------------------
    uint32_t running_since_ms_ = 0;      // 0 = not running; else the tick the engine caught at
    uint32_t last_cut_ms_ = 0;          // last frame a fuel cut was active (closed-loop re-entry delay)
    float    tps_last_         = 0.0f;   // for the throttle RATE the transient limit tests
    bool     tps_seen_         = false;             // published: 0 off, 1 wideband, 2 narrowband closed, 3 open

    void resolve_source();
    void resolve_roles();
    void resolve_one(uint8_t role, uint8_t own_bank, SignalId& sig, bool& wide, bool& narrow) const;
    void check_assignments();

    uint32_t last_ms_      = 0;
    uint32_t cfg_gen_seen_ = 0;          // last g_config_generation resolved — re-resolve LIVE on a config write
    uint32_t cell_seen_    = 0xFFFFFFFF; // which learned cell the remainder belongs to
    uint16_t cell_at(uint32_t when_ms) const;   // which cell the engine was in `when_ms` ago

};

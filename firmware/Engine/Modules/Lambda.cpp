#include "Lambda.h"
#include <cstring>
#include <cmath>
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../../generated/table_descs.h"   // lambda_ltft_desc / lambda_bank_trim_desc
#include "well_known_signals.h"               // wk:: roles
#include "../../Platform/platform_hal.h"      // platform_learned_block + tick
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../../Signal/Expr.h"                // expr::exec — the learn gate is an expression
#include "../EngineFrame.h"
#include "../TableEval.h"                     // tbl::locate_site / interp_site / nearest_of
#include "../../../generated/signal_ids.h"
#include "../../../generated/learned_layout.h"
#include "../../../generated/sensors_catalog.h"
#include "../../../generated/ecu_config.h"
#include <algorithm>

// Bumped on every live config 'w' write (CommsManager). Modules self-watch it and re-derive on change
// — there is no on_config_change() wiring in this firmware (same idiom as Sensors / ScriptEngine).
extern volatile uint32_t g_config_generation;

static_assert(LEARNED_LAMBDA_BANK_TRIM_CELLS == Lambda::MAX_BANKS, "bank trim block != MAX_BANKS");

namespace {

// The lambda type's value domain, from the generated catalog rather than restated here.
const float LAMBDA_MIN = SENSOR_TYPE_CATALOG[SENSOR_TYPE_LAMBDA].min;
const float LAMBDA_MAX = SENSOR_TYPE_CATALOG[SENSOR_TYPE_LAMBDA].max;

constexpr SignalId STFT_BANK[Lambda::MAX_BANKS] = { SIG_STFT_BANK_1_PCT, SIG_STFT_BANK_2_PCT };
constexpr SignalId LTFT_BANK[Lambda::MAX_BANKS] = { SIG_LTFT_BANK_1_PCT, SIG_LTFT_BANK_2_PCT };

int16_t* map_block(uint32_t offset, uint32_t bytes) {
    auto* base = platform_learned_block(offset, bytes);
    return base ? reinterpret_cast<int16_t*>(base) : nullptr;   // null -> neutral, re-learn each boot
}

// Percent <-> stored count. Round half away from zero, so a step over half a count lands rather than
// truncating toward nothing; the caller reads the cell BACK to see what it actually took.
inline float   pct_of(int16_t raw) { return static_cast<float>(raw) * Lambda::LTFT_SCALE; }
inline int16_t raw_of(float pct) {
    const float c = pct / Lambda::LTFT_SCALE;
    return static_cast<int16_t>(std::clamp((c >= 0.0f) ? c + 0.5f : c - 0.5f, -32768.0f, 32767.0f));
}

// Clamp a correction between its asymmetric bounds. Adding fuel and removing it are not equally
// dangerous, so they are two numbers: a sensor reading falsely rich leans the engine out, and that is
// the direction that destroys pistons.
inline float clamp_trim(float pct, uint16_t max_enrich, uint16_t max_disenrich) {
    return std::clamp(pct, -static_cast<float>(max_disenrich) * 0.1f,
                            static_cast<float>(max_enrich)    * 0.1f);
}

}  // namespace

void Lambda::init(const LambdaConfig& cfg) {
    cfg_  = &cfg;
    ltft_ = map_block(LEARNED_LAMBDA_LTFT_OFFSET,      LEARNED_LAMBDA_LTFT_BYTES);
    bank_ = map_block(LEARNED_LAMBDA_BANK_TRIM_OFFSET, LEARNED_LAMBDA_BANK_TRIM_BYTES);
    stft_.reset();
    pend_ = 0.0f; cell_seen_ = 0xFFFFFFFFu; last_ms_ = 0;
    resolve_source();
    // …AND CHECK THE ASSIGNMENTS NOW. This ran only on a config write, and g_config_generation is
    // bumped by nothing else — so an ECU booting on a stored tune with two widebands holding one job
    // never raised P1754 at all. The clash the studio paints red is the one you see while editing; the
    // DTC is the one you drive away with, and it was the one that was missing.
    check_assignments();
    resolve_roles();
    cfg_gen_seen_ = g_config_generation;   // sync so update() only re-resolves on SUBSEQUENT writes
}



// THE SENSOR THE TUNER CHOSE, and what kind of thing it is. Resolved on the config edge rather than
// per frame: it is a catalogue lookup, and the answer only changes when the selection does.
//
// The TYPE is the important half. A wideband and a narrowband are not two grades of the same
// instrument — one measures lambda, the other reports which side of stoichiometry the mixture is on —
// so the sensor decides the error domain, and reading it from the catalogue means the domain and the
// sensor cannot disagree. A selection naming something that is not an O2 sensor at all resolves to
// neither, which leaves the loop with nothing to hear and raises P1753.
// TWO SENSORS CANNOT HOLD ONE JOB. A role names one sensor, so a second claimant is a configuration
// error rather than a preference — and the ECU marks its own homework, the way it does for the firing
// order, instead of leaving the studio to re-derive a rule the firmware has to enforce anyway. Swept
// on the config edge over ENABLED widebands only: an assignment left on a sensor that is switched off
// is not a clash, it is a leftover.
void Lambda::check_assignments() {
    assign_clash_ = false;
    uint32_t seen = 0;                      // one bit per job: overall, 2 banks, 12 cylinders
    uint8_t  wbi  = 0;
    for (uint16_t i = 0; i < SENSOR_COUNT && wbi < MAX_WB; i++) {
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        if (d.type != SENSOR_TYPE_LAMBDA || d.group != SENSOR_GROUP_O2) continue;
        const uint8_t idx = wbi++;
        if (!g_config.sensors.sensor[i].enabled) continue;
        const uint8_t a = g_config.lambda.wb[idx].assign;
        if (a == 0) continue;               // Unassigned: watching nothing, clashing with nobody
        // The assignment IS the job id now, so it is its own bit — no mode/number pair to combine,
        // and no way for the two halves to disagree about which job was meant.
        if (a >= 32) continue;
        const uint32_t m = 1u << a;
        if (seen & m) { assign_clash_ = true; return; }
        seen |= m;
    }
}

// WHICH SENSOR HOLDS EACH JOB — answered for its own sake, not as a side effect of the loop's source
// selection. resolve_source() below asks a narrower question ("what should the loop listen to"), and it
// resolves to SIG_NONE whenever the tuner has pointed the loop somewhere else or switched it off. The
// role is a property of the ASSIGNMENT, so a bank being read out on a dash must not depend on whether
// the closed loop happens to be using it. Enabled widebands only — an assignment left on a sensor that
// is switched off is a leftover, exactly as check_assignments() treats it.
void Lambda::resolve_roles() {
    for (auto& s : role_sig_) s = SIG_NONE;
    const uint8_t A_OVERALL = 1;                 // 0 Unassigned, 1 Overall, 2..3 Bank 1..2, 4.. Cylinder N
    uint8_t wbi = 0;
    for (uint16_t i = 0; i < SENSOR_COUNT && wbi < MAX_WB; i++) {
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        if (d.type != SENSOR_TYPE_LAMBDA || d.group != SENSOR_GROUP_O2) continue;
        const uint8_t idx = wbi++;
        if (!g_config.sensors.sensor[i].enabled) continue;
        const uint8_t a = g_config.lambda.wb[idx].assign;
        if (a < A_OVERALL || a > A_OVERALL + 2) continue;          // Overall, Bank 1, Bank 2 only
        role_sig_[a - A_OVERALL] = static_cast<SignalId>(d.primary_channel);
    }
}

void Lambda::resolve_source() {
    src_sig_ = SIG_NONE; src_wide_ = src_narrow_ = false;
    if (!cfg_) return;

    // BANK 2 IS THE BANKED/UNBANKED SWITCH, not merely a second setting. Disabled means ONE loop for
    // the whole engine on the Unbanked/Bank 1 source — which is why that field carries both names.
    banked_ = (cfg_->o2_src_1 != 0) && (cfg_->o2_src_2 != 0);
    resolve_one(cfg_->o2_src_1, 1, src_sig_, src_wide_, src_narrow_);
    resolve_one(cfg_->o2_src_2, 2, src2_sig_, src2_wide_, src2_narrow_);
    if (!banked_) { src2_sig_ = SIG_NONE; src2_wide_ = src2_narrow_ = false; }

    // A CHOSEN SOURCE THAT DOES NOT EXIST IS A FAULT, not silence. Selecting "Wideband Bank 2" when
    // nothing holds that job, or naming a sensor that is switched off, leaves the loop with nothing to
    // hear — and the difference between "correcting nothing because it is happy" and "correcting
    // nothing because it cannot hear" is the whole reason this is published.
    src_fault_ = (cfg_->o2_src_1 != 0 && src_sig_ == SIG_NONE);
    if (banked_ && src2_sig_ == SIG_NONE) src_fault_ = true;

    // AND THE TWO BANKS MUST MEASURE THE SAME KIND OF THING. A narrowband reports which side of
    // stoichiometry the mixture is on; a wideband reports lambda. Split a banked pair across the two
    // and the loops are stepping on errors in different units, with different meanings, feeding one
    // trim surface — so the pair is refused rather than half-run.
    if (banked_ && (src_wide_ != src2_wide_ || src_narrow_ != src2_narrow_)) src_fault_ = true;

    // …AND THEY MUST BE TWO SENSORS. One sensor cannot serve two independent loops: both banks would
    // correct from one reading, and each would be responding to the other's fuelling as much as its
    // own. Compared on the RESOLVED SIGNAL rather than on the two selections, because the same sensor
    // can be named two different ways — "Wideband Bank 1" and "Wideband 3" are the same wire when
    // sensor 3 holds that job, and comparing the dropdown values would call that pair fine.
    if (banked_ && src_sig_ != SIG_NONE && src_sig_ == src2_sig_) src_fault_ = true;
}

// One selector -> one sensor. `own_bank` is which bank's role "Wideband bank N" means for this
// selector: the first names bank 1, the second bank 2, which is the only difference between the two
// option lists.
void Lambda::resolve_one(uint8_t role, uint8_t own_bank,
                         SignalId& sig, bool& wide, bool& narrow) const {
    sig = SIG_NONE; wide = narrow = false;
    if (role == 0) return;                                     // Disabled

    // A NARROWBAND IS NAMED OUTRIGHT. There are two, and they have no scope to resolve: a switching
    // sensor reports the whole engine's relationship to stoichiometry or nothing at all.
    if (role == 1 || role == 2) {
        const char* want = (role == 1) ? "narrowband_1" : "narrowband_2";
        for (uint16_t i = 0; i < SENSOR_COUNT; i++) {
            const SensorDescriptor& d = SENSOR_CATALOG[i];
            if (d.type != SENSOR_TYPE_NARROWBAND || std::strcmp(d.id, want) != 0) continue;
            if (!g_config.sensors.sensor[i].enabled) return;   // named, but switched off
            sig = static_cast<SignalId>(d.primary_channel); narrow = true;
            return;
        }
        return;
    }

    // 3 = this selector's own bank, 4..18 = wideband 1..15 outright, 19 = overall. The roles resolve
    // through the assignment list — whichever ENABLED sensor holds that job — so re-plumbing a sensor
    // is one edit there and the control page never names a wire. ONE sensor may hold a role; the first
    // enabled claimant wins, which is the safe way round: a second cannot quietly take over.
    //
    // OVERALL IS LAST BECAUSE BANK 2 HAS NO OVERALL: a sensor that sees the whole engine cannot be one
    // BANK's source — two loops must measure two different things, or they correct the same gas from
    // the same reading and chase each other. Putting it at the end makes bank 2's list this one minus
    // its final entry, so every other option means the same number in both fields.
    // The assignment is ONE value now: 0 Unassigned, 1 Overall, 2..3 Bank 1..2, 4.. Cylinder 1..12.
    const uint8_t A_OVERALL = 1, A_BANK1 = 2;
    const uint8_t ROLE_OVERALL = 19;
    const bool by_index = (role >= 4 && role < ROLE_OVERALL);
    const uint8_t want_index = by_index ? static_cast<uint8_t>(role - 4) : 0;   // 0-based wideband N

    uint8_t wbi = 0;
    for (uint16_t i = 0; i < SENSOR_COUNT && wbi < MAX_WB; i++) {
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        if (d.type != SENSOR_TYPE_LAMBDA || d.group != SENSOR_GROUP_O2) continue;
        const uint8_t idx = wbi++;                             // this sensor's slot in lambda.wb[]
        if (!g_config.sensors.sensor[i].enabled) continue;      // fitted list only
        bool hit;
        if (by_index) {
            hit = (idx == want_index);                          // named outright: no role involved
        } else {
            const uint8_t a = g_config.lambda.wb[idx].assign;
            hit = (role == ROLE_OVERALL) ? (a == A_OVERALL)
                                         : (a == static_cast<uint8_t>(A_BANK1 + own_bank - 1));
        }
        if (!hit) continue;
        sig = static_cast<SignalId>(d.primary_channel); wide = true;
        return;
    }
}

// Which cell the engine was in at `when_ms`. Nearest recorded step rather than an interpolation: a
// cell is a discrete place and a learned correction has to land in exactly one. Falls back to the most
// recent step when the ring does not reach that far back (early after a reset).
uint16_t Lambda::cell_at(uint32_t when_ms) const {
    const uint8_t n = hist_full_ ? HIST : hist_n_;
    if (n == 0) return 0;
    uint16_t best = hist_[(hist_n_ + HIST - 1) % HIST].cell;
    uint32_t best_d = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < n; i++) {
        const Step& s = hist_[i];
        const uint32_t d = (s.ms > when_ms) ? (s.ms - when_ms) : (when_ms - s.ms);
        if (d < best_d) { best_d = d; best = s.cell; }
    }
    return best;
}

// The engine caught. Stamp it: "Initial Engine Running Time" is measured from THIS edge, not from
// key-on and not from the first frame — it is the settling a freshly running engine needs before what
// it is doing means anything (a sensor coming to temperature, a fuel system finding its pressure).
void Lambda::on_engine_start() { running_since_ms_ = platform_get_tick_ms(); if (!running_since_ms_) running_since_ms_ = 1; }
void Lambda::on_engine_stop()  { running_since_ms_ = 0; tps_seen_ = false; }

void Lambda::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;
    if (g_config_generation != cfg_gen_seen_) {
        cfg_gen_seen_ = g_config_generation;
        resolve_source(); check_assignments(); resolve_roles();
    }

    const uint32_t now  = platform_get_tick_ms();
    const float    dt_s = (last_ms_ != 0) ? (now - last_ms_) / 1000.0f : 0.0f;
    last_ms_ = now;

    const float target = bus.get(wk::lambda_target, 1.0f);   // 1 frame stale (FuelCalc, slow)

    // --- WHERE. The trim borrows ve_table's axes, so this is literally the VE table's own site: the
    //     same bins, the same fractions. Read the surface interpolated (a correction should be as
    //     smooth as the map it corrects) and LEARN into the nearest cell (a learned cell is written,
    //     so it has to land in exactly one).
    const tbl::TableDesc d    = lambda_ltft_desc(&g_config.fuel_calculator);
    const tbl::Site      site = tbl::locate_site(d, bus,
                                    static_cast<SignalId>(d.x.src ? *d.x.src : 0),
                                    static_cast<SignalId>(d.y.src ? *d.y.src : 0),
                                    static_cast<SignalId>(d.z.src ? *d.z.src : 0));
    const int xs = (d.x.alloc >= 1) ? d.x.alloc : 1;
    const int ys = (d.y.alloc >= 1) ? d.y.alloc : 1;
    const uint32_t cell = static_cast<uint32_t>(tbl::nearest_of(site.z)) * xs * ys
                        + static_cast<uint32_t>(tbl::nearest_of(site.y)) * xs
                        + static_cast<uint32_t>(tbl::nearest_of(site.x));

    // Record where the engine is NOW, then ask where it was when the gas being smelled was made.
    hist_[hist_n_] = { now, static_cast<uint16_t>(cell) };
    hist_n_ = static_cast<uint8_t>((hist_n_ + 1) % HIST);
    if (hist_n_ == 0) hist_full_ = true;

    const float    delay_ms  = tbl::table_eval(ltft_delay_table_desc(cfg_), bus);
    const uint32_t learn_cell = cell_at(now - static_cast<uint32_t>(std::max(0.0f, delay_ms)));

    // The carried remainder belongs to the cell it accumulated in.
    if (learn_cell != cell_seen_) { cell_seen_ = learn_cell; pend_ = 0.0f; }

    // --- MEASURE. The chosen sensor, if it is a wideband and it is readable. No averaging: the loop
    //     listens to ONE sensor, named on the page, so what is driving the mixture is a setting rather
    //     than something to deduce from which sensors happen to be alive.
    float  err = 0.0f;
    int    n   = 0;
    if (src_wide_ && bus.valid(src_sig_)) {
        const float lam = bus.get(src_sig_, 0.0f);
        // PLAUSIBLE means what the TYPE says it means. These were 0.5/1.6 hardcoded here, which is the
        // lambda domain restated in a second place — and it had already drifted from the definition's
        // own numbers. SENSOR_TYPE_CATALOG carries the domain; read it.
        // PLAUSIBLE means what the TYPE says it means — the lambda domain lives in the catalogue.
        if (lam >= LAMBDA_MIN && lam <= LAMBDA_MAX) {
            // TARGET OSCILLATION ON A WIDEBAND, in lambda. The setting has always said it works here
            // and only the narrowband path read it: a wideband settles on its target, so a catalyst
            // that wants the mixture cycling got a flat line. The swing flips when the reading REACHES
            // the displaced target (within a tenth of the swing) — not when it crosses it, as on a
            // narrowband: a well-damped loop approaches its target without ever crossing, and a
            // crossing rule would park it on one side for good. Zero amplitude is the old behaviour.
            const float amp = static_cast<float>(cfg_->osc_amplitude) * 0.001f;
            float tgt = target;
            if (amp > 0.0f) {
                tgt = target + (osc_hi_ ? amp : -amp);
                const bool reached = osc_hi_ ? (lam >= tgt - 0.1f * amp) : (lam <= tgt + 0.1f * amp);
                if (reached) { osc_hi_ = !osc_hi_; tgt = target + (osc_hi_ ? amp : -amp); }
            }
            err = (lam - tgt) / tgt;                           // lean -> positive -> add fuel
            n   = 1;
        }
    }
    const bool measured = n > 0 && target > 0.1f;
    if (measured) err /= static_cast<float>(n);

    // --- GATE. One expression, evaluated against the bus and the live config. Empty means the safe
    //     built-in rule, so a tune that never sets it still behaves.
    const bool have_expr = !expr::is_empty(cfg_->ltft_learn_when, sizeof(cfg_->ltft_learn_when));
    bool learn_permit = true;                 // no expression = the built-in conditions alone decide
    if (have_expr) {
        expr::Ctx ex;
        ex.bus = &bus; ex.cfg = reinterpret_cast<const uint8_t*>(&g_config);
        ex.cfg_size = sizeof(g_config); ex.now_ms = now;
        const expr::Result r = expr::exec(cfg_->ltft_learn_when, sizeof(cfg_->ltft_learn_when), ex);
        learn_permit = r.ran && r.v.ok && r.v.value != 0.0f;   // unanswerable -> do not bake anything in
    }

    // THE BUILT-IN CONDITIONS, and they are not replaced by the expression — they are ANDed with it.
    // An expression that forgot one of these would otherwise silently drop a safety gate, and "Learn
    // While" is meant to ADD a condition of your own, not to take responsibility for all of them.
    //
    // Each threshold gates only when it is set to something meaningful, so the neutral value is the
    // off switch: Min Coolant Temp at -40 never gates, which is also how an engine with no coolant
    // sensor learns without any special case here. An UNREADABLE channel never blocks learning on its
    // own account — it cannot answer, and refusing on that basis would disable the feature on a rig
    // that simply has not wired that sensor.
    const float rpm_now = bus.get(wk::rpm, 0.0f);
    bool built_in = !bus.valid(wk::fuel_cut)                                  // not cutting fuel
                 && bus.get(wk::fuel_corr_poststart, 1.0f) < 1.01f;           // past post-start

    if (bus.valid(wk::clt))                                                   // warm
        built_in = built_in && bus.get(wk::clt, 0.0f) >= static_cast<float>(cfg_->learn_min_clt);
    built_in = built_in && rpm_now >= static_cast<float>(cfg_->learn_min_rpm)   // running, in band
                        && rpm_now <= static_cast<float>(cfg_->learn_max_rpm);

    // …and running for long enough. running_since_ms_ is stamped on the edge INTO running, so a zero
    // here means the engine is not running at all — which is itself a reason not to learn.
    const uint32_t need_ms = static_cast<uint32_t>(cfg_->learn_run_time_s) * 1000u;
    built_in = built_in && running_since_ms_ != 0 && (now - running_since_ms_) >= need_ms;

    // THE THROTTLE'S RATE, not its position. What disqualifies a tip-in is that the mixture is being
    // corrected by accel enrichment, whose error belongs to dTPS rather than to the operating point.
    if (bus.valid(wk::tps) && cfg_->learn_tps_rate_limit > 0 && dt_s > 0.0f) {
        const float tps = bus.get(wk::tps, 0.0f);
        // AN UNKNOWN RATE IS NOT A SLOW ONE. Without a previous sample there is nothing to difference,
        // and permitting on that basis let the first frame after the throttle became readable learn —
        // which is exactly the frame a tip-in is most likely to be under way. Unanswerable means do
        // not bake anything in, the same rule the expression path already follows.
        const float rate = tps_seen_ ? std::fabs(tps - tps_last_) / dt_s : 1e9f;   // %/s
        built_in = built_in && rate <= static_cast<float>(cfg_->learn_tps_rate_limit) * 0.1f;
        tps_last_ = tps; tps_seen_ = true;
    }

    if (cfg_->learn_max_tps_en && bus.valid(wk::tps))
        built_in = built_in && bus.get(wk::tps, 0.0f) <= static_cast<float>(cfg_->learn_max_tps) * 0.1f;
    if (cfg_->learn_max_map_en && bus.valid(wk::map))
        built_in = built_in && bus.get(wk::map, 0.0f) <= static_cast<float>(cfg_->learn_max_map) * 0.1f;

    learn_permit = learn_permit && built_in;

    // --- NARROWBAND. THE FIRST READABLE ONE, not an average of them: two switching signals averaged
    //     produce a mid-scale number that means neither thing, and sits on the switch point chattering.
    //     (Per-bank narrowband control would be two loops; this is one, and says so.)
    float nb_v = 0.0f;
    bool  nb_valid = src_narrow_ && bus.valid(src_sig_);
    if (nb_valid) nb_v = bus.get(src_sig_, 0.0f);
    // WARMTH BELONGS TO THE SENSOR, NOT TO THE LOOP. Only losing the reading altogether — unplugged,
    // timed out, never assigned — makes us stop knowing it is hot. This used to be cleared whenever
    // closed loop was switched off, which meant toggling the loop demanded the sensor prove itself
    // again by swinging past the warm voltage: a car cruising at stoichiometry sits at ~0.45 V and
    // might never get there, so the loop would sit refusing to run with a perfectly hot sensor.
    if (!nb_valid) nb_warm_ = false;

    // HAS IT EVER BEEN HOT? A cold Nernst cell cannot source much EMF, and a WARMING one sits near the
    // switch point looking exactly like a live sensor reporting stoichiometry — which is the reading
    // most likely to be believed and least likely to be true. "Reached the warm voltage at least once"
    // is a direct readiness test that needs no timer and no heater feedback.
    if (nb_valid && nb_v * 1000.0f >= static_cast<float>(cfg_->nb_warm_mv)) nb_warm_ = true;
    nb_valid = nb_valid && nb_warm_;

    // ARMED ONLY AT STOICH. The sensor switches at chemically correct mixture and says nothing either
    // side of it, so a target away from lambda 1 is open loop — a 0.85 target under boost cannot be
    // serviced by a narrowband at any setting, and pretending otherwise would servo a saturated rail.
    const bool tgt_stoich = std::fabs(target - 1.0f) <= static_cast<float>(cfg_->nb_stoich_band) * 0.001f;
    // A REFUSED CONFIGURATION DOES NOT RUN. src_fault_ says the ECU cannot make sense of what was
    // asked for, and carrying on regardless is the worst of the options: a banked pair split across a
    // narrowband and a wideband still corrects, from two errors that are not in the same units, into
    // one trim surface. Held off is honest, it is visible (P1754, and the selectors paint red), and it
    // is undone by fixing the configuration rather than by a reset.
    // --- ENTRY / HOLD. When the fast loop may CORRECT, as opposed to when it may teach (learn_permit).
    //     Held — frozen, NOT reset — through anything that makes the reading unrepresentative, so a
    //     learned correction survives it. This loop used to run straight through a fuel cut: the wideband
    //     sweeps lean on overrun, so it added fuel; past the sensor's range it zeroed the trim instead of
    //     freezing it, and every overrun ended in a rich-or-lean spike. The same reset fired on a genuinely
    //     lean reading past the range — dropping the trim at the moment the engine was leanest.
    if (bus.valid(wk::fuel_cut)) last_cut_ms_ = now;
    const bool in_cut     = bus.valid(wk::fuel_cut);
    const bool after_cut  = last_cut_ms_ != 0 && (now - last_cut_ms_) < static_cast<uint32_t>(cfg_->cl_after_cut_ms);
    const bool unsettled  = running_since_ms_ == 0 ||
                            (now - running_since_ms_) < static_cast<uint32_t>(cfg_->cl_start_delay_s) * 1000u;
    const bool cold       = bus.valid(wk::clt) && bus.get(wk::clt, 0.0f) < static_cast<float>(cfg_->cl_min_clt);
    const bool off_range  = src_wide_ && bus.valid(src_sig_) && !measured;   // readable, outside its domain
    // A THROTTLE TRANSIENT, while MAP prediction is standing in for the sensor (Map Source 1). The mixture
    // the wideband sees then is the transient's — the air estimate and the fuel film are catching up — not
    // the operating point's, so correcting it would chase an error the VE table does not have.
    const bool transient  = bus.valid(SIG_MAP_SOURCE) && static_cast<int>(bus.get(SIG_MAP_SOURCE, 0.0f)) == 1;
    const bool cl_hold    = in_cut || after_cut || unsettled || cold || off_range || transient;
    // WHY it is held, first reason wins — a state that said only "open loop" could not tell a fuel cut
    // from a cold engine from a transient. 0 None, 1 Fuel Cut, 2 After Cut, 3 Settling, 4 Cold,
    // 5 Off Range, 6 Transient (schema: lambda_cl_hold).
    bus.set(SIG_LAMBDA_CL_HOLD, static_cast<float>(in_cut ? 1 : after_cut ? 2 : unsettled ? 3 : cold ? 4
                                                   : off_range ? 5 : transient ? 6 : 0), true, now, ttl());

    const bool nb_on = cfg_->enabled != 0 && !src_fault_ && !measured && nb_valid && tgt_stoich && dt_s > 0.0f
                    && !cl_hold;

    const bool stft_on = cfg_->enabled != 0 && !src_fault_ && measured && dt_s > 0.0f && !cl_hold;
    const bool ltft_on = cfg_->ltft_enabled != 0;
    // Either loop can teach the surface; both need the same permit, and neither teaches while off.
    const bool learn   = (stft_on || nb_on) && ltft_on && learn_permit && ltft_ != nullptr;

    // ONE CONTROL LAW, TWO ERROR DOMAINS. A wideband measures lambda and a narrowband measures volts,
    // and that is the whole of the difference: the error is (target - measured) in whichever unit the
    // sensor speaks, and the same PI acts on it. There is no separate narrowband controller — there
    // used to be, a jump-and-ramp integrator living beside this one, and two control laws for one job
    // is two things to tune, two to test, and two to get subtly different.
    //
    // The commanded OSCILLATION is what a switching sensor needs and a wideband may want: the target
    // alternates by +/- amplitude each time the measurement crosses it, so the mixture cycles about
    // stoichiometry deliberately rather than as a by-product of the gains. A three-way cat wants that
    // cycling — alternating O2 storage and release is what lets one brick treat NOx and CO/HC at once.
    if (nb_on) {
        const float amp   = static_cast<float>(cfg_->osc_amplitude) * 0.001f;      // volts here
        const float tgt_v = static_cast<float>(cfg_->nb_target_mv) * 0.001f;
        const bool  rich  = nb_v > tgt_v + (osc_hi_ ? amp : -amp);
        if (nb_seen_ && rich != nb_rich_) osc_hi_ = !osc_hi_;   // crossed: swing the target the other way
        nb_rich_ = rich; nb_seen_ = true;
        err = (tgt_v + (osc_hi_ ? amp : -amp) - nb_v) / std::max(0.05f, tgt_v);   // lean -> positive
    } else {
        nb_seen_ = false;
    }
    if (cfg_->enabled == 0) osc_hi_ = false;   // the commanded swing restarts; the sensor stays hot

    nb_state_ = (cfg_->enabled == 0) ? 0 : measured ? 1 : nb_on ? 2 : 3;

    // SAY SO WHEN THERE IS NOTHING TO LISTEN TO. Enabled + no readable wideband is the case where the
    // switch is on, the page says Closed-Loop Enabled, and the loop publishes x1.000 for ever — which
    // from the driver's seat is indistinguishable from a mixture that needs no correction. Same rule as
    // KNOCK_TPS: "correction is off" and "the mixture is right" must never look identical.
    //
    // Deliberately keyed on READABLE rather than CONFIGURED, so it covers all of it: no wideband in the
    // catalogue enabled, one enabled but unassigned, one assigned but unheated or reading outside the
    // lambda domain. A narrowband cannot satisfy it — it is not a lambda sensor and no longer pretends
    // to be one (see the catalogue).
    if (dtc_) {
        // A narrowband that is READING but off-stoich is correct open loop, not a fault — so this asks
        // whether any O2 sensor can be heard at all, not whether the loop happens to be closed.
        // The configuration itself, recorded rather than only shown: a red widget is gone the moment
        // the studio is unplugged, and this is the fault someone drives away with.
        if (cfg_->enabled != 0 && (src_fault_ || assign_clash_))
            dtc_->raise(ModuleDtc::LAMBDA_CONFIG, DtcSource::CONFIG,
                        ModuleDtc::LAMBDA_CONFIG_SEV, now, dtc_ttl());
        else
            dtc_->heal(ModuleDtc::LAMBDA_CONFIG);

        if (cfg_->enabled != 0 && !measured && !nb_valid)
            dtc_->raise(ModuleDtc::LAMBDA_NO_SENSOR, DtcSource::MODULE,
                        ModuleDtc::LAMBDA_NO_SENSOR_SEV, now, dtc_ttl());
        else
            dtc_->heal(ModuleDtc::LAMBDA_NO_SENSOR);   // reading again, or switched off
    }


    // --- STFT. Off ZEROES rather than freezes: a loop the tuner switched off must not keep correcting
    //     the engine, or an autotune measures an error that is already partly hidden. The SWITCH does
    //     that, not the conditions — a momentary fuel cut still just freezes.
    if (cfg_->enabled == 0) stft_.reset();
    // GAINS FROM THE SURFACE, at this operating point. Read every frame rather than on a config
    // change: the point moves continuously, and the whole reason these are tables is that the plant
    // they drive is different at idle and at load. The stored figure is a PERCENT of fuel per unit of
    // error; the controller works in fractions, hence the /100.
    stft_.kp = tbl::table_eval(stft_kp_table_desc(cfg_), bus) / 100.0f;
    stft_.ki = tbl::table_eval(stft_ki_table_desc(cfg_), bus) / 100.0f;
    stft_.out_min = -static_cast<float>(cfg_->stft_max_disenrich) * 0.1f / 100.0f;
    stft_.out_max =  static_cast<float>(cfg_->stft_max_enrich)    * 0.1f / 100.0f;

    // Open loop corrects nothing and holds nothing — but HELD is not open loop: a hold keeps the trim.
    if (!stft_on && !nb_on && !cl_hold) stft_.reset();
    float stft = stft_.integ;
    if (stft_on || nb_on) stft = stft_.step(err, dt_s)
                               + (nb_on ? static_cast<float>(cfg_->nb_bias_pct) * 0.01f / 100.0f : 0.0f);

    // --- BANK 2'S LOOP. The same law on its own sensor, its own integrator and its own oscillation
    //     state — sharing any of them would make one bank's wind-up the other's starting point.
    float stft2 = stft;                                // unbanked: there is only one answer
    if (banked_) {
        stft2_.kp = stft_.kp; stft2_.ki = stft_.ki;    // one gain surface, read at one operating point
        stft2_.out_min = stft_.out_min; stft2_.out_max = stft_.out_max;
        float err2 = 0.0f; bool run2 = false;
        if (src2_wide_ && bus.valid(src2_sig_)) {
            const float lam2 = bus.get(src2_sig_, 0.0f);
            if (lam2 >= LAMBDA_MIN && lam2 <= LAMBDA_MAX) { err2 = (lam2 - target) / target; run2 = true; }
        } else if (src2_narrow_ && bus.valid(src2_sig_)) {
            const float v2 = bus.get(src2_sig_, 0.0f);
            if (v2 * 1000.0f >= static_cast<float>(cfg_->nb_warm_mv)) nb2_warm_ = true;
            if (nb2_warm_ && tgt_stoich) {
                const float amp   = static_cast<float>(cfg_->osc_amplitude) * 0.001f;
                const float tgt_v = static_cast<float>(cfg_->nb_target_mv) * 0.001f;
                const bool  rich2 = v2 > tgt_v + (osc2_hi_ ? amp : -amp);
                if (nb2_seen_ && rich2 != nb2_rich_) osc2_hi_ = !osc2_hi_;
                nb2_rich_ = rich2; nb2_seen_ = true;
                err2 = (tgt_v + (osc2_hi_ ? amp : -amp) - v2) / std::max(0.05f, tgt_v);
                run2 = true;
            }
        }
        const bool on2 = cfg_->enabled != 0 && !src_fault_ && run2 && dt_s > 0.0f && !cl_hold;
        if (!on2 && !cl_hold) { stft2_.reset(); nb2_seen_ = false; }   // held keeps bank 2's trim too
        stft2 = on2 ? stft2_.step(err2, dt_s) : stft2_.integ;
        if (cfg_->enabled == 0) { nb2_warm_ = false; osc2_hi_ = false; }
    } else {
        stft2_.reset(); nb2_seen_ = false;
    }

    // THE SCOPES TELESCOPE, which is what keeps a correction from being applied twice. The GLOBAL term
    // carries what both banks agree on and the per-bank terms carry only each bank's deviation from
    // it, so their sum at any cylinder is that bank's own answer — and unbanked collapses to exactly
    // the old behaviour, one loop in the global term with the bank terms at zero.
    const float stft_common = banked_ ? (stft + stft2) * 0.5f : stft;
    const float bank_dev[2] = { banked_ ? stft  - stft_common : 0.0f,
                                banked_ ? stft2 - stft_common : 0.0f };
    stft = stft_common;

    // --- LTFT. Applied whenever enabled, conditions or not: helping before the O2 lights is most of
    //     its value. Only LEARNING is gated.
    // READ AND WRITE MUST AGREE ON WHAT A CELL IS. Learning writes ONE cell — a learned value has to
    // land somewhere definite — so applying an INTERPOLATED blend of that cell with its untouched
    // neighbours means the engine never receives what was stored, and the loop keeps topping up a
    // surface whose effect is permanently short. So: nearest cell for both. The correction steps at a
    // cell boundary rather than sliding, which is what a cell-based trim is.
    //
    // The APPLIED cell is where the engine is NOW; the LEARNED cell is where the gas came from.
    float applied = 0.0f;
    if (ltft_on && ltft_) applied = pct_of(ltft_[cell]);
    if (learn) {
        const float gain   = static_cast<float>(cfg_->ltft_gain) / 1000.0f;   // fraction of STFT per step
        const float stored = pct_of(ltft_[learn_cell]);
        // WHAT THE LOOP HAS SETTLED ON — the STFT itself, for either kind of sensor.
        //
        // THIS COMMENT USED TO CLAIM A LOW-PASS THAT IS NOT HERE: "for a narrowband the STFT is a
        // triangle wave by construction, so the surface learns its LOW-PASSED CENTRE instead". There
        // is no filter in this function and never was. The observation behind it is real — a narrowband
        // loop is a limit cycle, so a cell records whichever phase of the swing this step landed in —
        // and the mitigation is that ltft_gain is a small fraction, which averages the phases over many
        // steps rather than filtering them. Whether that is good enough is a measurement nobody has
        // taken. Stating the intention as though it were the implementation is how the learn gate came
        // to be described by two comments for months while doing nothing (see 03f2954).
        // THE SURFACE LEARNS WHAT THE LOOP IS HOLDING, and nothing else.
        //
        // There used to be a RICH BIAS added here: the surface absorbed (stft + bias), so at rest it
        // held the true correction PLUS a few percent while the fast loop held minus the same, the two
        // cancelling. It was meant as an open-loop safety margin. It was a mistake, and Jason named it:
        // either the engine is tuned correctly or it is not, and a tuner who wants three percent more
        // fuel changes the TARGET — the one place richness is expressed, indexed and visible. A
        // constant buried inside a learned store is a fuel adder no map shows.
        //
        // It also cost more than it looked. The standoff spent three points of the fast loop's fifteen
        // holding a number that did nothing while the loop ran, and Apply to Base Table folded the bias
        // into the fuel map — again on every fold, compounding.
        const float want   = clamp_trim(stored + gain * stft * 100.0f + pend_,
                                        cfg_->ltft_max_enrich, cfg_->ltft_max_disenrich);
        // Store, then read BACK. What may be bled out of the STFT is what the cell ACTUALLY took —
        // after the clamp and after quantisation — or rounding leaks into the total correction one
        // step at a time. The sub-count remainder is carried, so a step under half a count still
        // moves the cell eventually instead of stalling.
        ltft_[learn_cell] = raw_of(want);
        const float neu = pct_of(ltft_[learn_cell]);
        pend_ = want - neu;
        // Hand the surface's gain back out of the loop that supplied it, or the same correction is
        // held twice — once in the cell and once in the integrator still winding.
        stft_.bleed((neu - stored) / 100.0f);
        stft -= (neu - stored) / 100.0f;
        if (learn_cell == cell) applied = neu;   // learned into the cell we are in
    }

    // SAY WHICH LOOP IS RUNNING. Off, wideband, narrowband, or open-because-the-target-is-off-stoich —
    // the last of which is a normal state on a narrowband car and is otherwise indistinguishable from
    // a loop that is simply happy.
    bus.set(SIG_LAMBDA_CL_STATE,   static_cast<float>(nb_state_), true, now, ttl());
    bus.set(SIG_O2_ASSIGN_FAULT,   assign_clash_ ? 1.0f : 0.0f,   true, now, ttl());
    bus.set(SIG_O2_SRC_FAULT,      src_fault_    ? 1.0f : 0.0f,   true, now, ttl());
    bus.set(wk::fuel_corr_stft, 1.0f + stft,             true, now, ttl());
    bus.set(wk::fuel_corr_ltft, 1.0f + applied / 100.0f, true, now, ttl());
    bus.set(wk::stft_pct,       stft * 100.0f,           true, now, ttl());
    bus.set(wk::ltft_pct,       applied,                 true, now, ttl());

    // LAMBDA BY ROLE. A forward of whichever sensor holds each job, so a consumer can ask "what is bank
    // 2 reading" without re-deriving the assignment table. Forwarded rather than averaged: the role
    // names ONE sensor (check_assignments() refuses a second claimant), so there is nothing to average
    // and inventing a mean would report a number no sensor ever measured. A role nobody holds — or one
    // whose sensor has gone invalid — publishes nothing and lets the channel expire, which is what keeps
    // "no sensor has that job" distinct from "the mixture is exactly 1.00".
    static constexpr SignalId ROLE_OUT[3] = { SIG_LAMBDA_OVERALL, SIG_LAMBDA_BANK_1, SIG_LAMBDA_BANK_2 };
    for (uint8_t r = 0; r < 3; r++) {
        const SignalId src = role_sig_[r];
        if (src != SIG_NONE && bus.valid(src)) bus.set(ROLE_OUT[r], bus.get(src), true, now, ttl());
    }

    // --- BANK LEARNING. The surface above learned what both banks AGREE on; this learns what they do
    //     not. Two scalars rather than a second surface, because bank difference is dominated by
    //     injector flow variance and manifold bias — close to a constant offset, not a shape over rpm
    //     and load — so a surface would be fifteen hundred cells recording one number.
    //
    //     Each bank's deviation is bled out of ITS OWN integrator, exactly as the surface is bled out
    //     of the fast loop: what the store absorbs, the controller must stop holding, or the same
    //     correction lives in two places and the mixture ends up twice-corrected the moment the engine
    //     next starts. Read BACK before bleeding, so what leaves the loop is what the store actually
    //     took after its clamp and its quantisation.
    if (learn && banked_ && bank_) {
        const float gain = static_cast<float>(cfg_->ltft_gain) / 1000.0f;
        for (uint8_t b = 0; b < MAX_BANKS; b++) {
            const float stored = pct_of(bank_[b]);
            const float want   = clamp_trim(stored + gain * bank_dev[b] * 100.0f,
                                            cfg_->ltft_max_enrich, cfg_->ltft_max_disenrich);
            bank_[b] = raw_of(want);
            const float neu = pct_of(bank_[b]);
            (b == 0 ? stft_ : stft2_).bleed((neu - stored) / 100.0f);
        }
    }

    // --- BANK. Two learned scalars, not a second surface: bank difference is dominated by injector
    //     flow variance and manifold bias, which is close to a constant offset. Published for
    //     FuelCalculator, which delivers them through the injector bank grouping.
    //
    //     UNBANKED APPLIES NEITHER. With one loop for the whole engine there is no measurement of a
    //     difference between banks: bank_dev is zero by construction above, and learning is refused —
    //     but the STORED scalars were still being applied, so a difference learned under a banked
    //     configuration went on skewing one bank's fuelling after the second sensor was taken away,
    //     with nothing able to correct it and nothing on screen saying it was there. That is the
    //     failure this module already legislates against for its two switches: off means 1.000, not
    //     "frozen at the last value". The store is retained, exactly as a disabled LTFT surface is, so
    //     re-enabling bank 2 gets back what it had learned.
    for (uint8_t b = 0; b < MAX_BANKS; b++) {
        const float v = bank_ ? pct_of(bank_[b]) : 0.0f;
        bus.set(STFT_BANK[b], bank_dev[b] * 100.0f, true, now, ttl());   // this bank's deviation, in %
        bus.set(LTFT_BANK[b], (ltft_on && banked_) ? v : 0.0f, true, now, ttl());
    }
}

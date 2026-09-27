#include "test_helpers.h"
#include "../firmware/Engine/Modules/KnockDsp.h"

#include <cmath>
#include <vector>

// Synthesize a sine of `freq` Hz sampled at `fs`, biased to mid-rail, as raw 12-bit ADC counts.
static std::vector<uint16_t> sine(float freq, float fs, unsigned n, float amp = 1000.0f) {
    std::vector<uint16_t> s(n);
    for (unsigned i = 0; i < n; ++i) {
        const float v = 2048.0f + amp * std::sin(2.0f * 3.14159265f * freq * i / fs);
        s[i] = static_cast<uint16_t>(v < 0 ? 0 : (v > 4095 ? 4095 : v));
    }
    return s;
}

// A burst that is SILENT for the first half and rings at `freq` for the second — the shape the phase
// resolution exists to detect. `late` false flips it (loud first, silent second).
static std::vector<uint16_t> halfBurst(float freq, float fs, unsigned n, bool late, float amp = 1000.0f) {
    std::vector<uint16_t> s(n, 2048);
    const unsigned from = late ? n / 2 : 0, to = late ? n : n / 2;
    for (unsigned i = from; i < to; ++i) {
        const float v = 2048.0f + amp * std::sin(2.0f * 3.14159265f * freq * i / fs);
        s[i] = static_cast<uint16_t>(v < 0 ? 0 : (v > 4095 ? 4095 : v));
    }
    return s;
}

// Mean dB across a half-open bucket range.
static float meanDb(const KnockProfile& p, unsigned from, unsigned to) {
    float sum = 0.0f; unsigned n = 0;
    for (unsigned i = from; i < to && i < p.count; ++i) { sum += p.db[i]; ++n; }
    return n ? sum / static_cast<float>(n) : KnockProfile::SILENT_DB;
}

int main() {
    fprintf(stdout, "=== KnockDsp ===\n");
    const float fs = 280000.0f;      // a knock ADC sample rate
    const float fknock = 7000.0f;    // knock frequency
    KnockDsp dsp;
    dsp.configure(fs, fknock, 3.0f);
    KnockProfile p;

    SECTION("a knock-frequency tone reads far higher than an off-frequency tone");
    {
        const auto on  = sine(fknock,   fs, 512);
        const auto off = sine(1000.0f,  fs, 512);   // well below the band
        const float db_on  = dsp.process(on.data(),  on.size(),  p);
        const float db_off = dsp.process(off.data(), off.size(), p);
        fprintf(stdout, "  db_on=%.1f  db_off=%.1f\n", db_on, db_off);
        CHECK(db_on > db_off + 10.0f);   // bandpass rejects the off-band tone by >10 dB
    }

    SECTION("silence reads at the floor");
    {
        const std::vector<uint16_t> flat(512, 2048);
        CHECK(dsp.process(flat.data(), flat.size(), p) < -40.0f);
    }

    SECTION("a louder knock reads higher than a quieter one (monotonic in amplitude)");
    {
        const auto loud = sine(fknock, fs, 512, 1500.0f);
        const auto soft = sine(fknock, fs, 512, 300.0f);
        const float dl = dsp.process(loud.data(), loud.size(), p);
        const float ds = dsp.process(soft.data(), soft.size(), p);
        CHECK(dl > ds);
    }

    SECTION("empty buffer is the floor (no div-by-zero)");
    {
        CHECK(dsp.process(nullptr, 0, p) <= -100.0f);
        CHECK(p.empty());
    }

    // ---- Phase resolution: the whole point of the profile -------------------------------------
    //
    // A single RMS cannot tell these two bursts apart — same tone, same amplitude, same total energy,
    // only the HALF of the window it occupies differs. That is exactly the knock/pre-ignition
    // distinction in miniature, and the reason the scalar had to go.

    SECTION("energy late in the window lands in the late buckets");
    {
        const auto late = halfBurst(fknock, fs, 2048, true);
        dsp.process(late.data(), late.size(), p);
        const float early_db = meanDb(p, 0, p.count / 2);
        const float late_db  = meanDb(p, p.count / 2, p.count);
        fprintf(stdout, "  buckets=%u  early=%.1f  late=%.1f\n", p.count, early_db, late_db);
        CHECK(p.count > 1);
        CHECK(late_db > early_db + 20.0f);
    }

    SECTION("energy early in the window lands in the early buckets");
    {
        const auto early = halfBurst(fknock, fs, 2048, false);
        dsp.process(early.data(), early.size(), p);
        const float early_db = meanDb(p, 0, p.count / 2);
        const float late_db  = meanDb(p, p.count / 2, p.count);
        fprintf(stdout, "  buckets=%u  early=%.1f  late=%.1f\n", p.count, early_db, late_db);
        CHECK(early_db > late_db + 20.0f);
    }

    SECTION("the two halves are indistinguishable by overall level alone");
    {
        // Same energy, opposite phase — the scalar the old DSP returned cannot separate them, which is
        // the bug this profile exists to fix. Assert the blindness explicitly so nobody "simplifies"
        // the profile away later believing the level was ever enough.
        const auto late  = halfBurst(fknock, fs, 2048, true);
        const auto early = halfBurst(fknock, fs, 2048, false);
        const float db_late  = dsp.process(late.data(),  late.size(),  p);
        const float db_early = dsp.process(early.data(), early.size(), p);
        CHECK(std::fabs(db_late - db_early) < 3.0f);
    }

    // ---- Bucket count is derived from the physics, not fixed ------------------------------------

    SECTION("a bucket spans at least MIN_CYCLES of the band, so short bursts get fewer buckets");
    {
        const auto longBurst  = sine(fknock, fs, 2048);
        const auto shortBurst = sine(fknock, fs, 313);    // ~40 deg at 6000 rpm
        dsp.process(longBurst.data(), longBurst.size(), p);
        const unsigned nb_long = p.count;
        dsp.process(shortBurst.data(), shortBurst.size(), p);
        const unsigned nb_short = p.count;
        fprintf(stdout, "  buckets: 2048->%u  313->%u\n", nb_long, nb_short);
        CHECK(nb_long > nb_short);
        CHECK(nb_long <= KnockProfile::MAX_BUCKETS);
        CHECK(nb_short >= 1);

        // The floor that motivates the whole derivation: every bucket holds >= MIN_CYCLES periods.
        const float per_cycle = fs / fknock;
        CHECK(313.0f / static_cast<float>(nb_short) >= KnockProfile::MIN_CYCLES * per_cycle - 1.0f);
    }

    SECTION("a burst too short for even one bucket still yields one");
    {
        const auto tiny = sine(fknock, fs, 8);
        dsp.process(tiny.data(), tiny.size(), p);
        CHECK(p.count == 1);
    }

    SECTION("the DSP leaves the angle domain alone — that is the caller's to stamp");
    {
        p.start_deg = 0.0f; p.step_deg = 0.0f;
        const auto s = sine(fknock, fs, 1024);
        dsp.process(s.data(), s.size(), p);
        CHECK(!p.hasPhase());          // energy without geometry is not yet an angle
    }

    return test_summary();
}

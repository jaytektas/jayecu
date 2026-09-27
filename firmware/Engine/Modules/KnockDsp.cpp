#include "KnockDsp.h"

#include <cmath>

void KnockDsp::configure(float sampleRate, float centerFreq, float Q) {
    sample_rate_ = sampleRate;
    center_freq_ = centerFreq;
    filter_.configureBandpass(sampleRate, centerFreq, Q);
}

// See KnockProfile: a bucket narrower than MIN_CYCLES periods of the band centre reports phase noise,
// not a level, so the count is whatever fits rather than whatever was asked for.
unsigned KnockDsp::bucketsFor(unsigned count) const {
    if (count == 0 || sample_rate_ <= 0.0f || center_freq_ <= 0.0f) return 1u;
    const float per_cycle = sample_rate_ / center_freq_;                  // samples in one knock period
    const float min_bucket = KnockProfile::MIN_CYCLES * per_cycle;
    if (min_bucket <= 1.0f) return KnockProfile::MAX_BUCKETS;             // absurdly oversampled band
    unsigned n = static_cast<unsigned>(static_cast<float>(count) / min_bucket);
    if (n < 1u) n = 1u;
    if (n > KnockProfile::MAX_BUCKETS) n = KnockProfile::MAX_BUCKETS;
    return n;
}

float KnockDsp::process(const uint16_t* samples, unsigned count, KnockProfile& out, float vref) {
    out.count      = 0;
    out.overall_db = KnockProfile::SILENT_DB;
    for (unsigned i = 0; i < KnockProfile::MAX_BUCKETS; ++i) out.db[i] = KnockProfile::SILENT_DB;

    if (samples == nullptr || count == 0)
        return out.overall_db;

    const unsigned nb = bucketsFor(count);
    const float ratio = vref / 4095.0f;
    filter_.primeSteadyState(vref * 0.5f);   // mid-rail steady state -> no edge at sample 0

    // ONE pass. The bucket sums and the whole-window sum are the same filtered samples accumulated
    // twice, so there is no second filter run and no second traversal — and, more importantly, the
    // profile and the level can never disagree about what they measured.
    float bucket_sq[KnockProfile::MAX_BUCKETS] = {};
    unsigned bucket_n[KnockProfile::MAX_BUCKETS] = {};
    float total_sq = 0.0f;

    // THE BUCKET IS WALKED, NOT COMPUTED. This used to derive the index per sample as
    // (uint64_t)i * nb / count — a 64-bit multiply and a 64-bit DIVIDE, which on a Cortex-M7 is a
    // library call of tens of cycles, paid once per SAMPLE. The window is a fixed number of crank
    // DEGREES, so the samples per window fall as the engine speeds up while the windows per second
    // rise: the sample rate through here is roughly constant at ~190 k/s on a twelve-cylinder engine,
    // and that divide was a fixed tax on every one of them. Measured before and after on the bench
    // (tools/bench_cpu_load.py): the knock task went from 23 % of the MCU to [see the commit].
    //
    // Buckets are contiguous and ascending, so the boundary can simply be carried: one divide per
    // BUCKET (at most MAX_BUCKETS per burst) instead of one per sample, and the arithmetic is
    // identical — bucket b starts at ceil-free floor(b * count / nb), exactly what the old expression
    // resolved to.
    unsigned b = 0;
    unsigned next = (nb > 1) ? static_cast<unsigned>((static_cast<uint64_t>(1) * count) / nb) : count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = ratio * static_cast<float>(samples[i]);
        const float f = filter_.filter(v);
        const float sq = f * f;
        total_sq += sq;
        while (i >= next && b + 1u < nb) {
            ++b;
            next = static_cast<unsigned>((static_cast<uint64_t>(b + 1u) * count) / nb);
        }
        bucket_sq[b] += sq;
        ++bucket_n[b];
    }

    auto to_db = [](float mean_sq) -> float {
        if (mean_sq <= 0.0f) return KnockProfile::SILENT_DB;
        const float db = 10.0f * std::log10(mean_sq);
        return db < KnockProfile::SILENT_DB ? KnockProfile::SILENT_DB : (db > 100.0f ? 100.0f : db);
    };

    for (unsigned b = 0; b < nb; ++b)
        out.db[b] = bucket_n[b] ? to_db(bucket_sq[b] / static_cast<float>(bucket_n[b]))
                                : KnockProfile::SILENT_DB;
    out.count      = static_cast<uint8_t>(nb);
    out.overall_db = to_db(total_sq / static_cast<float>(count));
    return out.overall_db;
}

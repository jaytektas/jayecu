// The ADC boxcar fold — the one part of the analog acquisition path that is arithmetic
// rather than register plumbing, and the one that can be wrong in a way that looks like
// noise instead of a fault.
#include "Platform/AdcFold.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main() {
    // --- the stride IS the test ------------------------------------------------------
    // Three ranks, four scans. Rank r holds the constant (r+1)*100 in every scan, so a
    // correct fold returns exactly that. A fold that walked contiguous blocks instead of
    // striding would return 100 for rank 0 (the first four words are 100,200,300,100 ->
    // 175), so this goes red the moment the stride is dropped.
    {
        const uint16_t ring[12] = {100, 200, 300,
                                   100, 200, 300,
                                   100, 200, 300,
                                   100, 200, 300};
        volatile uint16_t out[3] = {0, 0, 0};
        adc_fold_ranks(ring, 3, 4, out);
        CHECK(out[0] == 100, "rank0 = %u, want 100 (a contiguous walk gives 175)", out[0]);
        CHECK(out[1] == 200, "rank1 = %u, want 200", out[1]);
        CHECK(out[2] == 300, "rank2 = %u, want 300", out[2]);
    }

    // --- it averages, it does not merely sample --------------------------------------
    // One rank whose scans are 10,20,30,40: the mean is 25. Taking the newest would give
    // 40, the oldest 10.
    {
        const uint16_t ring[4] = {10, 20, 30, 40};
        volatile uint16_t out[1] = {0};
        adc_fold_ranks(ring, 1, 4, out);
        CHECK(out[0] == 25, "mean = %u, want 25 (newest=40, oldest=10)", out[0]);
    }

    // --- half-ring folding: the caller hands us the half the DMA is NOT filling -------
    // An 8-scan ring of 2 ranks, folded as two halves. The halves differ, so folding the
    // wrong half (or the whole ring) gives a different answer than folding each in turn.
    {
        uint16_t ring[16];
        for (int s = 0; s < 8; ++s) { ring[s * 2 + 0] = (s < 4) ? 1000 : 2000;
                                      ring[s * 2 + 1] = (s < 4) ?   50 :  150; }
        volatile uint16_t out[2] = {0, 0};
        adc_fold_ranks(&ring[0], 2, 4, out);                 // first half
        CHECK(out[0] == 1000 && out[1] == 50, "first half = %u/%u, want 1000/50", out[0], out[1]);
        adc_fold_ranks(&ring[2 * 4], 2, 4, out);             // second half
        CHECK(out[0] == 2000 && out[1] == 150, "second half = %u/%u, want 2000/150", out[0], out[1]);
    }

    // --- full 12-bit scale does not overflow the accumulator --------------------------
    // 16 ranks x 16 scans of 4095 is the worst case the F7 sequencer can present.
    {
        uint16_t ring[16 * 16];
        for (auto& v : ring) v = 4095;
        volatile uint16_t out[16] = {};
        adc_fold_ranks(ring, 16, 16, out);
        for (int c = 0; c < 16; ++c) CHECK(out[c] == 4095, "rank%d = %u, want 4095", c, out[c]);
    }

    // --- degenerate inputs are refused, not dereferenced -------------------------------
    {
        volatile uint16_t out[1] = {7};
        const uint16_t ring[1] = {1};
        adc_fold_ranks(nullptr, 1, 1, out);  CHECK(out[0] == 7, "null base wrote out");
        adc_fold_ranks(ring, 0, 1, out);     CHECK(out[0] == 7, "zero ranks wrote out");
        adc_fold_ranks(ring, 1, 0, out);     CHECK(out[0] == 7, "zero scans wrote out (and would /0)");
    }

    printf(g_fail ? "adc_fold: %d FAILURES\n" : "adc_fold: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

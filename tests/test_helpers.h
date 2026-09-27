#pragma once
// Minimal test harness — no dependencies.

#include <cstdio>
#include <cstdlib>
#include <cmath>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { g_pass++; } \
    else { \
        fprintf(stderr, "FAIL [%s:%d]  %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } \
} while(0)

#define CHECK_NEAR(a, b, tol) do { \
    double _a = (double)(a), _b = (double)(b), _t = (double)(tol); \
    if (fabs(_a - _b) <= _t) { g_pass++; } \
    else { \
        fprintf(stderr, "FAIL [%s:%d]  |%s - %s| = %.6f > %.6f\n", \
                __FILE__, __LINE__, #a, #b, fabs(_a - _b), _t); \
        g_fail++; \
    } \
} while(0)

#define SECTION(name) fprintf(stdout, "\n  [%s]\n", name)

// inline, not static: this header is pulled into every test TU and plenty of them never call it,
// which made `defined but not used` the single noisiest warning in the suite.
inline int test_summary() {
    fprintf(stdout, "\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

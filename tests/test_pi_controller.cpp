// PiController — the shared PI loop under idle, boost, VVT, alternator, cruise, traction and lambda.
// What is pinned: anti-windup. The old back-calculation (integ = out - p) drove the integrator to the
// OPPOSITE sign whenever P alone saturated, so the output slammed to the other limit when the error
// cleared. Conditional integration must never do that.
#include "test_helpers.h"
#include "../firmware/Engine/PiController.h"

int main() {
    fprintf(stdout, "=== PiController ===\n");

    SECTION("P alone saturating does not wind the integrator the wrong way");
    {
        PiController pi; pi.kp = 3.0f; pi.ki = 1.0f; pi.out_min = -10.0f; pi.out_max = 10.0f;
        for (int i = 0; i < 50; ++i) pi.step(10.0f, 0.01f);   // p = +30, far over the ceiling
        fprintf(stdout, "    integ after saturation = %g\n", (double)pi.integ);
        CHECK(pi.integ >= 0.0f);                              // never driven negative by a positive error
        const float out = pi.step(0.0f, 0.01f);               // the error clears
        CHECK(out >= 0.0f);                                   // no slam to the opposite limit
    }

    SECTION("integrator does not grow into a saturated limit, and recovers at once");
    {
        PiController pi; pi.kp = 0.0f; pi.ki = 10.0f; pi.out_min = 0.0f; pi.out_max = 5.0f;
        for (int i = 0; i < 1000; ++i) pi.step(1.0f, 0.1f);  // would integrate to 1000 unchecked
        CHECK(pi.integ <= 5.0f);                              // held at the authority
        const float back = pi.step(-1.0f, 0.1f);              // error reverses
        CHECK(back < 5.0f);                                   // leaves the limit on the first step
    }

    SECTION("an extra (D) term counts toward saturation");
    {
        PiController pi; pi.kp = 0.0f; pi.ki = 1.0f; pi.out_min = 0.0f; pi.out_max = 10.0f;
        for (int i = 0; i < 100; ++i) pi.step(1.0f, 0.1f, 20.0f);   // D alone is past the ceiling
        CHECK_NEAR(pi.integ, 0.0f, 0.001f);                   // so nothing accumulated underneath it
    }

    SECTION("unsaturated: plain PI arithmetic");
    {
        PiController pi; pi.kp = 2.0f; pi.ki = 0.5f; pi.out_min = -100.0f; pi.out_max = 100.0f;
        const float o = pi.step(4.0f, 1.0f);                  // p = 8, integ = 2
        CHECK_NEAR(o, 10.0f, 0.001f);
        CHECK_NEAR(pi.integ, 2.0f, 0.001f);
    }

    return test_summary();
}

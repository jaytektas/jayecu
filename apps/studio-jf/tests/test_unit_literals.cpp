// A THRESHOLD SAYS WHAT IT IS IN — "value > 200ms" and "value > .2s" are the same band.
//
// A band is a sentence about a physical quantity, and a bare number in it is meaningless: the same "20"
// that meant 20 kPa means 20 psi the moment someone switches the display unit, and the band is wrong by a
// factor of seven with nothing on screen to say so. So a literal may carry its unit, and it is folded into
// whatever unit the reading is currently shown in.
//
// The rule TEXT keeps the suffix — "200ms" is what was written and what comes back — so this is a
// representation, not a one-way conversion at authoring time.
//
//   cmake --build build --target unit_literals_test && ./build/unit_literals_test

#include "model/UnitManager.h"
#include "surface/CanvasWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[units] %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
// Fold, then compare as numbers where the result is one — printf's %.17g is exact but ugly.
static double folded(const std::string& expr, const std::string& unit) {
    const std::string out = CanvasWidget::substUnits(expr, unit);
    try { return std::stod(out); } catch (...) { return NAN; }
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    UnitManager& um = UnitManager::instance();
    const std::string sec = um.findQuantityForUnit("s"), ms = um.findQuantityForUnit("ms");
    std::printf("[units] quantity of s = '%s', of ms = '%s'\n", sec.c_str(), ms.c_str());
    if (sec.empty() || sec != ms) {
        std::printf("[units] the unit table has no time quantity — nothing to convert against\n");
        return 77;
    }

    // THE HEADLINE. Two spellings of the same threshold, folded into the same unit, are the same number.
    const double a = folded("200ms", "s"), b = folded(".2s", "s");
    std::printf("[units] 200ms -> %g s, .2s -> %g s\n", a, b);
    ck(near(a, b) && near(a, 0.2), "200ms and .2s are the same threshold in seconds");
    ck(near(folded("200ms", "ms"), 200.0), "...and the same one in milliseconds again",
       std::to_string(folded("200ms", "ms")));

    // A whole expression, not just a literal: only the literals move.
    const std::string e = CanvasWidget::substUnits("value > 200ms && value < 2s", "ms");
    std::printf("[units] \"value > 200ms && value < 2s\" in ms -> \"%s\"\n", e.c_str());
    ck(e.find("value >") != std::string::npos && e.find("ms") == std::string::npos
       && e.find("200") != std::string::npos && e.find("2000") != std::string::npos,
       "both literals fold and the rest of the expression is untouched", e);

    // WHAT MUST NOT MOVE.
    ck(CanvasWidget::substUnits("value > 2e3", "ms") == "value > 2e3", "an exponent is not a unit suffix",
       CanvasWidget::substUnits("value > 2e3", "ms"));
    ck(CanvasWidget::substUnits("egt_1 > 900", "C").find("egt_1") != std::string::npos,
       "a digit inside an identifier is not a literal");
    const std::string plain = CanvasWidget::substUnits("value > 20", "kPa");
    ck(plain == "value > 20", "a bare number is left exactly as written", plain);
    // A unit from another quantity cannot convert into this reading; leaving it to fail loudly beats
    // rescaling milliseconds into volts behind the user's back.
    const std::string cross = CanvasWidget::substUnits("value > 5V", "ms");
    std::printf("[units] \"value > 5V\" read in ms -> \"%s\"\n", cross.c_str());
    ck(cross.find("5") != std::string::npos, "a cross-quantity literal is not silently rescaled", cross);

    std::printf("[units] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

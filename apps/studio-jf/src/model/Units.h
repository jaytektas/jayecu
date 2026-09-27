#pragma once

// Raw and Eng — the two units every number in the tune is in, made into two TYPES.
//
// WHY THIS EXISTS. The bytes hold counts; a page shows engineering units; a field's `scale` converts
// between them. That contract used to live in comments — "RAW out, the widget applies the scale" on
// Cache::configValue, "already cooked" on CanvasWidget::scaleOf — and a `double` says nothing, so a
// function that returned the wrong one compiled perfectly and was only visible on screen.
//
// It happened: Cache::solveTable has to interpolate in ENGINEERING units (its breakpoints are in them,
// and the firmware multiplies a cell by its scale before it lerps, so matching the firmware bit-for-bit
// means cooking first). It then returned that cooked number from configValue, whose callers cook what
// they are given. Every table readout in the studio read a tenth, or a thousandth, of the cell it was
// sitting under, and nothing could have caught it but a person looking at the screen.
//
// A conversion now has to be WRITTEN. cook() and uncook() are the only ways across, they both take the
// scale, and neither can be reached by accident — an Eng where a Raw belongs will not compile.
//
// Deliberately minimal: no arithmetic operators. These are not a units library, they are a label on a
// boundary, and every use of them is meant to be a line that says which side it is on.

struct Raw {
    double v = 0.0;
    constexpr Raw() = default;
    constexpr explicit Raw(double x) : v(x) {}
};

struct Eng {
    double v = 0.0;
    constexpr Eng() = default;
    constexpr explicit Eng(double x) : v(x) {}
};

// counts -> engineering. The scale is the field's, from the meta.
constexpr Eng cook(Raw r, double scale) { return Eng{ r.v * scale }; }

// engineering -> counts. A scale of 0 is not a conversion, it is a missing field; pass the number
// through rather than dividing by it, which is what every caller wants and none should have to write.
constexpr Raw uncook(Eng e, double scale) { return Raw{ scale != 0.0 ? e.v / scale : e.v }; }

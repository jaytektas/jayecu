#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdio>

// Format an axis BREAKPOINT for display. Breakpoints are stored as F32 (they can hold fractions like
// 98.5), but the schema's per-axis `digits` is often 0 — which would show 98.5 as "99" and make a
// fractional value look lost. So show the value at its true precision (trailing zeros trimmed) with
// `minDec` as a floor: 98.5 -> "98.5", 6500 -> "6500", and minDec=1 keeps "6500.0" if a caller wants it.
inline std::string fmtBreak(double v, int minDec) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);      // generous precision, then trim
    std::string s(b);
    const std::size_t dot = s.find('.');
    if (dot == std::string::npos) return s;
    std::size_t end = s.size();
    const std::size_t floorEnd = dot + 1 + static_cast<std::size_t>(minDec > 0 ? minDec : 0);
    while (end > floorEnd && s[end - 1] == '0') --end;
    if (end == dot + 1) end = dot;              // nothing left after the dot -> drop it too
    s.resize(end);
    return s;
}

// Snap a breakpoint to `dec` decimals — the counterpart to fmtBreak above, and what keeps a bin on a
// grid its own channel can land on. An axis tracing app_1 is read to a tenth, so a typed 30.777 was a
// breakpoint the live signal could never reach; worse, fmtBreak printed it back in full, so nothing
// looked wrong. dec < 0 means the channel states no precision — snap nothing rather than invent a grid.
inline double snapBreak(double v, int dec) {
    if (dec < 0 || dec > 9) return v;
    const double p = std::pow(10.0, dec);
    return std::round(v * p) / p;
}

// Studio-side mirror of the firmware's TableDesc (firmware/Engine/TableEngine.h): ONE representation of a
// table resolved to byte OFFSETS in the config image, identical for a module table ("module.table") and a
// per-element table ("module.array[i].table"). Cells are row-major at the LIVE strides — cell[z*(xn*yn) +
// y*xn + x] — exactly as the firmware indexes them. Every table operation (read/write cells, bins,
// src/enable/n, insert/remove bin) runs on this one struct via Cache::ti* — there is no module-path /
// element-path split. Resolve a path to one of these with Cache::resolveTable().
struct TableImage {
    struct Axis {
        int         breaksBase = 0;     // byte offset of the breakpoint array (the headers)
        std::string breakType;          // U08/S08/U16/S16/U32/S32/F32
        int         breakSize  = 0;     // bytes per breakpoint
        double      breakScale = 1.0;
        int         nBase   = -1;       // live bin-count scalar offset (-1 = fixed grid -> use nMax)
        int         srcBase = -1;       // channel-selector (SignalId, int16; -1 = none) offset (-1 = axis has no source scalar)
        int         enBase  = -1;       // enable-toggle (U08) offset (-1 = always on / not optional)
        int         nMax    = 0;        // maximum bins
        int         nMin    = 2;        // minimum bins (schema-driven floor; an axis can't shrink below it)
        int         digits  = 0;        // breakpoint display precision
        std::string units, label;       // schema units / label for the axis
        std::string defaultSig;         // default live channel name (for the dialog's signal combo)
        bool        optional = false;   // has an enable toggle
        double      vmin = 0.0, vmax = 0.0;   // breakpoint VALUE bounds (e.g. a % axis: 0..100)
        bool        hasBounds = false;        // false = unconstrained breakpoint values
    };

    int              cellBase  = 0;     // byte offset of cell[0]
    std::string      cellType;          // cell datatype
    int              cellSize  = 0;     // bytes per cell
    double           cellScale = 1.0;
    double           cellMinV = 0.0, cellMaxV = 0.0;   // cell value bounds in RAW units (min==max ⇒ unset); clamps entry
    // What the CELLS are, when something authoritative says so. A sensor calibration's values are not
    // the table's own: they are readings in the sensor TYPE's units, at its precision, inside its domain
    // (`type_scaled` in the meta). Cache::resolveTable fills these from that type — including a generic
    // input's type, which is a tune byte — so every reader of a TableImage gets the same answer.
    int              cellDigits = -1;                  // -1 = nothing states a precision
    std::string      cellUnits;
    bool             cellTypeScaled = false;           // cells are in the owning element's sensor-type units
    // The base table(s) this one's values roll into ("Apply to Base Table"), from the schema's
    // `apply_to`. A learned trim IS a correction on another map, and this is the only place that
    // relationship is expressed as data rather than prose.
    std::vector<std::string> applyTo;
    bool applyAdd = false;   // fold in by adding (duty offset) rather than multiplying (a percentage)
    std::string      elemArray;                        // "sensors.sensor" when this is an element table
    int              elemIndex = -1;                   // …and which element
    // Per-axis HEADINGS, when the definition gives names instead of numbers (the VVT trim's cams, the
    // bank trim's banks). Empty means the breakpoints are the heading, which is the ordinary case.
    std::vector<std::string> rowLabels, colLabels;
    std::vector<Axis> axes;             // 1..3 (X[, Y[, Z]])
    std::string      label;             // the table's display label
    bool             valid = false;
};

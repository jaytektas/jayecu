#pragma once

// TableFile — a table as TEXT, so a calibration can leave the studio and come back.
//
// A sensor's transfer curve is the one piece of a tune that is NOT about this engine: it is the sensor's
// own data sheet. The same GM open-element sensor is the same curve in every car it is fitted to, and
// typing sixteen points from a PDF is a job worth doing once. So a curve can be saved to a file and
// loaded into another sensor, another tune, another ECU.
//
// The format is a CSV any spreadsheet opens, because that is where the numbers usually come from:
//
//   # jayecu table 1
//   # table: sensors.sensor[clt].cal
//   # x: Sensor Raw [ADC]
//   # value: Reading [C]
//   ,0,1000,2000,3000,4000
//   ,120.0,80.0,40.0,10.0,-20.0
//
// The leading blank field on each row is where a 2-D table's Y breakpoint goes, so a curve and a map are
// the same shape on disk and the same reader handles both:
//
//   ,500,1000,1500          <- x breakpoints
//   20,12.0,14.0,16.0       <- y breakpoint, then that row's cells
//   40,13.0,15.0,17.0
//
// The '#' lines are provenance, not instructions: a file records which table it came from and in what
// units, so a curve saved from a coolant sensor and loaded into an oil-pressure one is visible as a
// mistake rather than a silent unit change. Load reads the numbers regardless — a spreadsheet that drops
// the comments still works.

#include "Cache.h"
#include "TableImage.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tablefile {

struct Result {
    bool        ok = false;
    std::string message;      // what happened, for the status line
    int         rows = 0, cols = 0;
};

// A number as short text: enough digits for the field, never scientific, no trailing noise.
inline std::string num(double v, int digits) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", digits < 0 ? 3 : (digits > 6 ? 6 : digits), v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {                 // trim trailing zeros, but keep one digit
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// WRITE. Live bins only: the allocation is a storage detail, and a file full of unused cells would load
// as data on another table whose live size is different.
inline Result save(Cache& c, const std::string& path, const std::string& file) {
    const TableImage t = c.resolveTable(path);
    if (!t.valid) return { false, "not a table: " + path, 0, 0 };
    std::ofstream f(file, std::ios::binary);
    if (!f) return { false, "cannot write " + file, 0, 0 };

    const int nx = c.tiLiveN(t, 0);
    const int ny = t.axes.size() > 1 ? c.tiLiveN(t, 1) : 1;
    const std::vector<double> xb = c.tiBins(t, 0);
    const std::vector<double> yb = t.axes.size() > 1 ? c.tiBins(t, 1) : std::vector<double>{ 0.0 };

    f << "# jayecu table 1\n";
    f << "# table: " << path << "\n";
    if (!t.axes.empty())
        f << "# x: " << t.axes[0].label << " [" << t.axes[0].units << "]\n";
    if (t.axes.size() > 1)
        f << "# y: " << t.axes[1].label << " [" << t.axes[1].units << "]\n";
    f << "# value: " << t.label << " [" << t.cellUnits << "]\n";

    f << "";                                                 // the empty corner cell
    for (int x = 0; x < nx; ++x) f << "," << num(xb[static_cast<size_t>(x)], t.axes[0].digits);
    f << "\n";
    for (int y = 0; y < ny; ++y) {
        if (t.axes.size() > 1) f << num(yb[static_cast<size_t>(y)], t.axes[1].digits);
        for (int x = 0; x < nx; ++x) f << "," << num(c.tiCell(t, x, y, 0), t.cellDigits);
        f << "\n";
    }
    return { true, "wrote " + std::to_string(nx) + "x" + std::to_string(ny) + " to " + file, ny, nx };
}

// One CSV line -> fields. Comments and blank lines are the caller's to skip.
inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == ',' || ch == ';' || ch == '\t') { out.push_back(cur); cur.clear(); }
        else if (ch != '\r') cur += ch;
    }
    out.push_back(cur);
    return out;
}
inline bool number(const std::string& s, double& v) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    try { size_t used = 0; v = std::stod(s.substr(a), &used); return used > 0; } catch (...) { return false; }
}

// THE AXIS TAKES THE FILE'S SIZE. A table carries a live bin count as well as an allocation, and a
// twelve-point curve loaded into a table currently using two points would otherwise write ten cells
// nothing reads — the file would appear to load and the curve would still be the old two-point line.
// Resizing goes through insert/remove, the same path a hand edit uses, so a fixed-grid axis (no live
// count of its own) is left alone and every other axis ends up exactly as long as the file.
inline bool resize(Cache& c, const TableImage& t, int axis, int n) {
    if (axis >= static_cast<int>(t.axes.size())) return true;
    const TableImage::Axis& a = t.axes[axis];
    if (a.nBase < 0) return true;                            // fixed grid: the count is not ours to change
    n = std::max(n, a.nMin);
    for (int have = c.tiLiveN(t, axis); have > n; --have) c.tiRemoveBin(t, axis, have - 1);
    for (int have = c.tiLiveN(t, axis); have < n; ++have) c.tiInsertBin(t, axis, have);
    return c.tiLiveN(t, axis) == n;
}

// READ. The file decides the SHAPE within what the table can hold, and a file with more points than the
// axis allows is refused rather than silently truncated — a calibration cut short is a calibration that
// lies, and it would read as a working curve.
inline Result load(Cache& c, const std::string& path, const std::string& file) {
    const TableImage t = c.resolveTable(path);
    if (!t.valid) return { false, "not a table: " + path, 0, 0 };
    std::ifstream f(file, std::ios::binary);
    if (!f) return { false, "cannot read " + file, 0, 0 };

    std::vector<double> xb;
    std::vector<std::vector<double>> rows;
    std::vector<double> yb;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> fields = split(line);
        if (fields.size() < 2) continue;
        double v = 0.0;
        if (xb.empty()) {                                    // the header row: x breakpoints
            for (size_t i = 1; i < fields.size(); ++i)
                if (number(fields[i], v)) xb.push_back(v);
            continue;
        }
        std::vector<double> cells;
        for (size_t i = 1; i < fields.size(); ++i)
            if (number(fields[i], v)) cells.push_back(v);
        if (cells.empty()) continue;
        yb.push_back(number(fields[0], v) ? v : 0.0);
        rows.push_back(std::move(cells));
    }
    if (xb.empty() || rows.empty()) return { false, "no table data in " + file, 0, 0 };

    const int nxMax = c.tiAllocN(t, 0);
    const int nyMax = t.axes.size() > 1 ? c.tiAllocN(t, 1) : 1;
    if (static_cast<int>(xb.size()) > nxMax || static_cast<int>(rows.size()) > nyMax)
        return { false, "file is " + std::to_string(xb.size()) + "x" + std::to_string(rows.size()) +
                        ", table holds at most " + std::to_string(nxMax) + "x" + std::to_string(nyMax), 0, 0 };

    const int nx = static_cast<int>(xb.size());
    const int ny = static_cast<int>(rows.size());
    resize(c, t, 0, nx);
    if (t.axes.size() > 1) resize(c, t, 1, ny);
    c.tiWriteBins(t, 0, xb);
    if (t.axes.size() > 1 && yb.size() == rows.size()) c.tiWriteBins(t, 1, yb);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx && x < static_cast<int>(rows[static_cast<size_t>(y)].size()); ++x)
            c.tiSetCell(t, x, y, 0, rows[static_cast<size_t>(y)][static_cast<size_t>(x)]);
    return { true, "loaded " + std::to_string(nx) + "x" + std::to_string(ny) + " from " + file, ny, nx };
}

}   // namespace tablefile

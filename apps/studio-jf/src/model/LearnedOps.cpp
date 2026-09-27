#include "LearnedOps.h"

#include "Cache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace learned {
namespace {

// The base map a trim names, as the studio addresses it. The schema writes it as a full binding path
// ("config.<module>.<table>"); everything in the studio drops the leading segment.
std::string baseOf(const TableImage& t) {
    if (t.applyTo.empty()) return {};
    std::string p = t.applyTo.front();
    if (p.rfind("config.", 0) == 0) p.erase(0, 7);
    return p;
}

// SAME GRID, CHECKED RATHER THAN ASSUMED. A trim shares its base map's axis arrays today, which is what
// makes folding it a cell-for-cell walk; a schema edit that pointed one of them elsewhere would write
// through a mapping that no longer holds, silently and across the whole map.
bool sameGrid(const Cache& c, const TableImage& a, const TableImage& b) {
    for (int ax = 0; ax < 3; ++ax)
        if (c.tiLiveN(a, ax) != c.tiLiveN(b, ax)) return false;
    return true;
}

}  // namespace

bool isLearnedTrim(const std::string& tablePath) {
    if (tablePath.empty() || !Cache::instance().isTable(tablePath)) return false;
    const TableImage t = Cache::instance().resolveTable(tablePath);
    return t.valid && !t.applyTo.empty();
}

Plan planApplyToBase(const std::string& tablePath) {
    Plan p;
    Cache& c = Cache::instance();
    if (!c.isTable(tablePath)) return p;
    const TableImage trim = c.resolveTable(tablePath);
    if (!trim.valid || trim.cellSize <= 0 || trim.applyTo.empty()) return p;
    const std::string basePath = baseOf(trim);
    if (!c.isTable(basePath)) return p;
    const TableImage base = c.resolveTable(basePath);
    if (!base.valid || base.cellSize <= 0 || !sameGrid(c, trim, base)) return p;

    const int nx = c.tiLiveN(trim, 0), ny = c.tiLiveN(trim, 1), nz = std::max(1, c.tiLiveN(trim, 2));
    if (nx <= 0 || ny <= 0) return p;
    p.possible = true;
    p.trimName = trim.label.empty() ? tablePath : trim.label;
    p.baseName = base.label.empty() ? basePath  : base.label;
    for (int z = 0; z < nz; ++z)
        for (int r = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc) {
                const double v = c.tiCell(trim, cc, r, z);
                if (v == 0.0) continue;
                ++p.cells;
                if (std::fabs(v) > std::fabs(p.worst)) p.worst = v;
            }

    char d[360];
    if (p.cells == 0)
        std::snprintf(d, sizeof d, "%s is all zero - there is nothing learned to apply to %s.",
                      p.trimName.c_str(), p.baseName.c_str());
    else
        std::snprintf(d, sizeof d,
                      "Multiply %d cell(s) of %s by their %s (largest %+.2f%%), then reset those cells "
                      "to zero.\n\nThe engine must not be learning while you do this, and %s still has "
                      "to be burned afterwards.",
                      p.cells, p.baseName.c_str(), p.trimName.c_str(), p.worst, p.baseName.c_str());
    p.detail = d;
    return p;
}

Plan planReset(const std::string& tablePath) {
    Plan p;
    Cache& c = Cache::instance();
    if (!isLearnedTrim(tablePath)) return p;
    const TableImage t = c.resolveTable(tablePath);
    const int nx = c.tiLiveN(t, 0), ny = c.tiLiveN(t, 1), nz = std::max(1, c.tiLiveN(t, 2));
    if (nx <= 0 || ny <= 0) return p;
    p.possible = true;
    p.trimName = t.label.empty() ? tablePath : t.label;
    p.baseName = baseOf(t);
    for (int z = 0; z < nz; ++z)
        for (int r = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc) {
                const double v = c.tiCell(t, cc, r, z);
                if (v == 0.0) continue;
                ++p.cells;
                if (std::fabs(v) > std::fabs(p.worst)) p.worst = v;
            }

    char d[360];
    if (p.cells == 0)
        std::snprintf(d, sizeof d, "%s is already all zero.", p.trimName.c_str());
    else
        // WHAT IS BEING LOST, in the numbers. "Reset the trim?" is a question nobody can answer;
        // "forget 84 cells, the largest of them +11.4 %" is one they can.
        std::snprintf(d, sizeof d,
                      "This forgets %d learned cell(s) of %s, the largest of them %+.2f%%.\n\nThe engine has to learn "
                      "them again from scratch. If they are worth keeping, Apply to Base Table first - "
                      "that folds them into the map and resets them in one step.",
                      p.cells, p.trimName.c_str(), p.worst);
    p.detail = d;
    return p;
}

void applyToBase(const std::string& tablePath) {
    Cache& c = Cache::instance();
    const Plan p = planApplyToBase(tablePath);
    if (!p.possible || p.cells == 0) return;
    const TableImage trim = c.resolveTable(tablePath);
    const TableImage base = c.resolveTable(baseOf(trim));
    const int nx = c.tiLiveN(trim, 0), ny = c.tiLiveN(trim, 1), nz = std::max(1, c.tiLiveN(trim, 2));

    // ONE undo step, and BOTH halves inside it: a base map updated without its trim cleared would
    // double the correction on the very next engine cycle.
    c.beginEdit();
    for (int z = 0; z < nz; ++z)
        for (int r = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc) {
                const double pct = c.tiCell(trim, cc, r, z);
                if (pct == 0.0) continue;                       // neutral cell: leave the base alone
                c.tiSetCell(base, cc, r, z, c.tiCell(base, cc, r, z) * (1.0 + pct / 100.0));
                c.tiSetCell(trim, cc, r, z, 0.0);               // consumed - the trim is neutral again
            }
    c.endEdit("Apply to Base Table");
}

void resetToZero(const std::string& tablePath) {
    Cache& c = Cache::instance();
    const Plan p = planReset(tablePath);
    if (!p.possible || p.cells == 0) return;
    const TableImage t = c.resolveTable(tablePath);
    const int nx = c.tiLiveN(t, 0), ny = c.tiLiveN(t, 1), nz = std::max(1, c.tiLiveN(t, 2));
    c.beginEdit();
    for (int z = 0; z < nz; ++z)
        for (int r = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc)
                if (c.tiCell(t, cc, r, z) != 0.0) c.tiSetCell(t, cc, r, z, 0.0);
    c.endEdit("Reset Learned Values");
}

}  // namespace learned

#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// TableEngine — one generic interpolation core for the configurable table engine.
//
// A table is a typed cell grid + up to 3 axes. BOTH cells and axis breakpoints are type-polymorphic
// (uint8/int8/uint16/int16/float): the cell type drives interp<CellT>, the axis type drives locate()
// — each dispatched ONCE per lookup (not per element), so there's no per-breakpoint branch and no
// forced widening. That symmetry is what lets a 1-D "curve" (e.g. an int cal_raw axis + int cells)
// collapse into the very same resolver as a float-axis module table — one path, no special case.
//
// One path covers 1D / 2D / 3D: a dimension with fewer than 2 live bins collapses to index 0 (its
// fraction is 0), so linear / bilinear / trilinear all fall out of the same trilinear expression.
// Cells are stored row-major at the LIVE strides (not the max alloc): cell[z*(xn*yn) + y*xn + x].
//
// The channel-driven layer (read bus.get(axis.channel) for each enabled axis, then call interp())
// lives with the table config; this header is pure + host-testable.
// ---------------------------------------------------------------------------

namespace tbl {

// The scalar type of a cell grid OR an axis's breakpoints. Both use it (symmetric typing).
enum CellType : uint8_t { CELL_U8, CELL_I8, CELL_U16, CELL_I16, CELL_F32 };

// NOTE: float axis/cell pointers must be 4-byte aligned — the codegen aligns every float table in the
// packed EcuConfig (and asserts it), because Cortex-M7 VLDR faults on an unaligned float load (unlike
// the integer LDR/LDRH the hardware fixes up). Keeping the storage aligned lets this stay a plain load.

// Locate v in a sorted axis of n bins (read as float per AxisT): the lower index i and fraction f in
// [0,1]. Clamps below axis[0] / above axis[n-1] (f pinned 0/1). n<2 -> degenerate (i=0,f=0).
template <typename AxisT>
inline void locate_t(const AxisT* axis, int n, float v, int& i, float& f) {
    if (n < 2) { i = 0; f = 0.0f; return; }
    i = 0;
    for (int k = 1; k < n; k++) { if (static_cast<float>(axis[k]) <= v) i = k; else break; }
    if (i > n - 2) i = n - 2;
    const float x0 = static_cast<float>(axis[i]), x1 = static_cast<float>(axis[i + 1]);
    f = (x1 > x0) ? (v - x0) / (x1 - x0) : 0.0f;
    if (f < 0.0f) f = 0.0f;
    else if (f > 1.0f) f = 1.0f;
}

// Dispatch the axis scalar type ONCE, then run the typed search.
inline void locate(const void* axis, CellType t, int n, float v, int& i, float& f) {
    switch (t) {
        case CELL_U8:  locate_t(static_cast<const uint8_t*>(axis),  n, v, i, f); break;
        case CELL_I8:  locate_t(static_cast<const int8_t*>(axis),   n, v, i, f); break;
        case CELL_U16: locate_t(static_cast<const uint16_t*>(axis), n, v, i, f); break;
        case CELL_I16: locate_t(static_cast<const int16_t*>(axis),  n, v, i, f); break;
        case CELL_F32: locate_t(static_cast<const float*>(axis),    n, v, i, f); break;
    }
}

// A resolved position on ONE axis: the lower bin, the fraction into it, and the live bin count the
// search ran against. n < 2 means the axis is degenerate — collapsed to bin 0 with fraction 0.
struct AxisPos {
    int   i = 0;
    float f = 0.0f;
    int   n = 0;
};

// WHERE THE OPERATING POINT IS, resolved once, as distinct from what any table says there.
//
// A value lookup BLENDS the cells around a point; a learned write has to LAND in cells, with the
// weights it was blended by. Those are two questions about the same point, and the module that learns
// had been answering the second one by itself — its own search, against its own axes, arriving at its
// own idea of which cell the engine is in. A Site is that point resolved once, so a reader and a
// writer of the same grid cannot disagree about where they are.
struct Site { AxisPos x, y, z; };

// Locate one axis, returning its position rather than writing through references. Safe for n < 2 and
// for a null axis: locate() returns (0, 0) without dereferencing.
inline AxisPos locate_axis(const void* axis, CellType t, int n, float v) {
    AxisPos p;
    p.n = n;
    locate(axis, t, n, v, p.i, p.f);
    return p;
}

// The nearest bin of an ALREADY-located axis — the same rule nearest() applies, without re-searching.
inline int nearest_of(const AxisPos& p) {
    if (p.n < 2) return 0;
    return (p.f >= 0.5f) ? p.i + 1 : p.i;
}

// The ONE way to turn a value into a bin INDEX, as distinct from an interpolated value.
//
// locate() already answers it: it gives the lower bin and the fraction into it, so the nearest bin is
// that bin, or the next one once the fraction passes halfway. Learned tables need an index rather than
// a blend — a learned cell is WRITTEN, so the value has to land in exactly one bin — and every module
// that learns had grown its own way of getting one: three copies of
// `static_cast<int>(v / SPAN * BINS)` against private SPAN/BINS constants, plus a linear
// nearest-breakpoint search in Idle. Four answers to one question, none of them reading the axis the
// schema declares.
//
// Safe by construction: locate() clamps i to n-2 and f to [0,1], so i+1 never leaves the axis.
inline int nearest(const void* axis, CellType t, int n, float v) {
    return nearest_of(locate_axis(axis, t, n, v));
}

// Up-to-3D interpolation. Cells typed via CellT; axes typed via their CellType tags. A dimension with
// n<2 collapses to index 0.
// THE STRIDE IS THE ALLOCATION, NOT THE LIVE SIZE. A table is a fixed grid — 32x32x6 of storage,
// origin at cell 0 — and <axis>_n says only how much of it is in play: how far a search may run, not
// where a row starts. Addressing with the live count instead made a resize move every cell's MEANING,
// so the tuner's map had to be resampled to stay put, a restore that brought back cells without their
// counts came back scrambled, and the same bytes read differently depending on a number stored
// elsewhere. With the allocation as the stride, changing a size is just changing a search bound.
//
// xalloc/yalloc default to 0 = "no allocation stated, use the live count", which is the old behaviour
// and what a caller whose buffer really is packed to its live size still wants.
template <typename CellT>
inline float interp_at(const CellT* cells, float scale, const Site& s,
                       int xalloc = 0, int yalloc = 0) {
    const int xstride = (xalloc >= 1) ? xalloc : ((s.x.n >= 1) ? s.x.n : 1);
    const int ystride = (yalloc >= 1) ? yalloc : ((s.y.n >= 1) ? s.y.n : 1);
    const int plane   = xstride * ystride;
    auto cell = [&](int x, int y, int z) -> float {
        return static_cast<float>(cells[z * plane + y * xstride + x]) * scale;
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

    // A collapsed dimension reuses its own index instead of reaching for i+1. Its fraction is already
    // 0, so the blend returns the same number either way — but the read stays inside the grid, which
    // for X it previously did not: a 1-wide table located at (0, 0) still addressed cell 1.
    const int x1 = (s.x.n >= 2) ? s.x.i + 1 : s.x.i;
    const int y1 = (s.y.n >= 2) ? s.y.i + 1 : s.y.i;
    const int z1 = (s.z.n >= 2) ? s.z.i + 1 : s.z.i;

    const float z0 = lerp(lerp(cell(s.x.i, s.y.i, s.z.i), cell(x1, s.y.i, s.z.i), s.x.f),
                          lerp(cell(s.x.i, y1,    s.z.i), cell(x1, y1,    s.z.i), s.x.f), s.y.f);
    const float zh = lerp(lerp(cell(s.x.i, s.y.i, z1),    cell(x1, s.y.i, z1),    s.x.f),
                          lerp(cell(s.x.i, y1,    z1),    cell(x1, y1,    z1),    s.x.f), s.y.f);
    return lerp(z0, zh, s.z.f);
}

// Locate + interpolate in one call — the shape every existing caller uses.
template <typename CellT>
inline float interp(const CellT* cells, float scale,
                    const void* xa, CellType xt, int xn, float xv,
                    const void* ya, CellType yt, int yn, float yv,
                    const void* za, CellType zt, int zn, float zv,
                    int xalloc = 0, int yalloc = 0) {
    const Site s{ locate_axis(xa, xt, xn, xv),
                  locate_axis(ya, yt, yn, yv),
                  locate_axis(za, zt, zn, zv) };
    return interp_at(cells, scale, s, xalloc, yalloc);
}

// Convenience for the all-float-axes case (direct callers + host tests) — forwards to the typed path.
template <typename CellT>
inline float interp(const CellT* cells, float scale,
                    const float* xa, int xn, float xv,
                    const float* ya, int yn, float yv,
                    const float* za, int zn, float zv,
                    int xalloc = 0, int yalloc = 0) {
    return interp(cells, scale, xa, CELL_F32, xn, xv, ya, CELL_F32, yn, yv, za, CELL_F32, zn, zv,
                  xalloc, yalloc);
}

}  // namespace tbl

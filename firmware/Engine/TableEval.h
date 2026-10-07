#pragma once
#include <cstdint>
#include <cassert>                // guard: resizable axis active but 0 live bins (fires in test/debug, NDEBUG-out on target)
#include "TableEngine.h"          // tbl::interp — the pure interpolation core
#include "../Signal/SignalBus.h"  // bus.get(SignalId)

// ---------------------------------------------------------------------------
// TableEval — the one channel-driven evaluation path behind EVERY configurable table.
//
// A table is described once (TableDesc, built from the module's live config) as a typed cell grid +
// up to 3 axes. Each axis carries: its float breakpoints, a pointer to its LIVE bin count (the
// resizable <axis>_n scalar), the channel it reads (a SignalId selector the tuner picks), and an
// optional enable. Reading a table is two steps: locate_site() turns the live sizes + channel values
// into a tbl::Site (which bin on each axis, and the fraction into it), and interp_site() blends the
// cells around it at the ALLOCATION strides cell[z*(xa*ya) + y*xa + x]. tableresolve() is the pair,
// which is what a value lookup wants; a module that LEARNS keeps the Site — and must address its cell
// at the same allocation stride. Both the firmware and the tuning app stride by the allocation, so
// resizing an axis moves no cell: <axis>_n only says how far the search runs.
//
// Codegen emits a <table>_desc(const <Module>Config*) builder per table; modules call
//   tbl::table_eval(<table>_desc(cfg_), bus)
// in place of any bespoke lookup. Adding a table is then pure schema.
// ---------------------------------------------------------------------------

namespace tbl {

// CellType (cells AND axis breakpoints) comes from TableEngine.h — one scalar-type enum for both.

struct AxisDesc {
    const void*    breaks  = nullptr;   // breakpoint array (in the live config), typed by break_type
    CellType       break_type = CELL_F32;  // scalar type of the breakpoints (float module axis / int cal_raw)
    const uint8_t* n_live  = nullptr;   // LIVE bin count (the <axis>_n scalar); null => fixed n_fixed
    uint8_t        n_fixed = 0;         // bin count when the axis isn't resizable
    const int16_t* src     = nullptr;   // channel selector (SignalId; int16, -1 = none); value = bus.get(*src)
    const uint8_t* enable  = nullptr;   // optional-axis enable; null => always enabled
    // Breakpoints are stored RAW (the axis's own scalar type); `scale` converts raw -> engineering
    // units, exactly as TableDesc::scale does for cells. Coordinates arrive in engineering units —
    // off the bus or from a module — so the resolver divides by this to compare against the raw
    // breaks. An unscaled axis (the common case) leaves it 1.0 and nothing changes.
    float          scale   = 1.0f;
    // The PHYSICAL bin count — how wide this axis's storage is, whatever <axis>_n currently says. It is
    // the stride the cells are laid out at, so it is what addressing uses; n_live only bounds the search.
    // 0 = not stated (a buffer packed to its live size), which keeps the old addressing.
    uint16_t       alloc   = 0;
};

struct TableDesc {
    const void* cells     = nullptr;
    CellType    cell_type = CELL_U16;
    float       scale     = 1.0f;
    AxisDesc    x, y, z;
};

// Never more than the allocation. The studio holds <axis>_n to it, but the firmware takes tune bytes
// from a raw write or a restored file on trust (CRC and layout hash only), and every search and learned
// write is bounded by this number.
inline int axis_live_n(const AxisDesc& a) {
    const int n = a.n_live ? static_cast<int>(*a.n_live) : static_cast<int>(a.n_fixed);
    return (a.alloc >= 1 && n > static_cast<int>(a.alloc)) ? static_cast<int>(a.alloc) : n;
}

// The nearest bin of ONE axis of a described table. The AxisDesc already knows its breakpoints, their
// scalar type and its live bin count, so a caller supplies only the value — which is the whole reason
// a module no longer needs constants describing its own grid.
inline int nearest_bin(const AxisDesc& a, float v) {
    return nearest(a.breaks, a.break_type, axis_live_n(a), v / a.scale);
}

// Read breakpoint k of an axis as float (typed by break_type) — used only by the debug guard below.
inline float axis_bp(const AxisDesc& a, int k) {
    switch (a.break_type) {
        case CELL_U8:  return static_cast<const uint8_t* >(a.breaks)[k];
        case CELL_I8:  return static_cast<const int8_t*  >(a.breaks)[k];
        case CELL_U16: return static_cast<const uint16_t*>(a.breaks)[k];
        case CELL_I16: return static_cast<const int16_t* >(a.breaks)[k];
        case CELL_F32: return static_cast<const float*   >(a.breaks)[k];
    }
    return 0.0f;
}

// The idle-class config bug: an axis populated with real breakpoints but its live count (<axis>_n) left
// at 0, so interp silently collapses it to a constant (cell 0) instead of interpolating. Distinguish it
// from a DELIBERATE constant table (count 0 AND flat/zero breakpoints) by requiring the first two
// breakpoints to differ. Active + resizable only; NDEBUG compiles the whole check out on target.
inline bool axis_uncounted(const AxisDesc& a, bool on) {
    return on && a.breaks && a.n_live && *a.n_live == 0 && axis_bp(a, 0) != axis_bp(a, 1);
}

// Read a described table at an ALREADY-RESOLVED Site — the cell-type dispatch, and nothing else.
// Separating this from locate_site() is the whole point of the split: two readers of one grid can now
// be handed the same position instead of each searching for it.
inline float interp_site(const TableDesc& d, const Site& s) {
    switch (d.cell_type) {
        case CELL_U16: return interp_at(static_cast<const uint16_t*>(d.cells), d.scale, s, d.x.alloc, d.y.alloc);
        case CELL_I16: return interp_at(static_cast<const int16_t* >(d.cells), d.scale, s, d.x.alloc, d.y.alloc);
        case CELL_U8:  return interp_at(static_cast<const uint8_t* >(d.cells), d.scale, s, d.x.alloc, d.y.alloc);
        case CELL_I8:  return interp_at(static_cast<const int8_t*  >(d.cells), d.scale, s, d.x.alloc, d.y.alloc);
        case CELL_F32: return interp_at(static_cast<const float*   >(d.cells), d.scale, s, d.x.alloc, d.y.alloc);
    }
    return 0.0f;
}

// Evaluate a table at the current bus state. Every axis is symmetric: an axis with no enable pointer
// (not flagged optional) is always active; an optional one collapses to index 0 when its enable is off
// (n -> 0). With ALL axes off the table is a bare 1x1 constant and interp returns cell[0]. So 0D / 1D /
// 2D / 3D all fall out of one path.
// Per-axis coordinate source, dispatched by TYPE at compile time: pass a SignalId to read the channel
// off the bus, or a plain value to use it directly. This lets an axis whose coordinate is NOT a bus
// signal — an instance index (per-ETB / per-cylinder), or a module's own internal state (a slewed
// target) — drop into the same evaluator with no override flag and no faked signal.
inline float as_coord(SignalBus& bus, SignalId s) { return bus.get(s, 0.0f); }   // signal -> look it up
inline float as_coord(SignalBus&,     float v)    { return v; }                  // value  -> use it

// Resolve WHERE a table is being read, from EXPLICIT per-axis coordinates (each a SignalId or a
// value), WITHOUT touching a cell. Every axis is symmetric: an axis with no breakpoints doesn't exist
// (n -> 0); an optional one collapses to index 0 when its enable is off. With all axes off the table
// is a bare 1x1 constant. 0D/1D/2D/3D fall out of one path. Gating is on `breaks` (existence), not
// `src`, so external (value) axes are first-class.
//
// This is the half a LEARNED table needs. It resolves the point against the axes of the table being
// corrected, so a trim cannot end up indexed by a grid of its own that has drifted from the base map.
template <typename X, typename Y, typename Z>
inline Site locate_site(const TableDesc& d, SignalBus& bus, X x, Y y, Z z) {
    const bool x_on = (d.x.enable == nullptr) || (*d.x.enable != 0);
    const bool y_on = (d.y.enable == nullptr) || (*d.y.enable != 0);
    const bool z_on = (d.z.enable == nullptr) || (*d.z.enable != 0);

    // Guard the idle-class bug: a populated axis whose live count (<axis>_n) was left at 0, so the
    // lookup silently collapses to a constant instead of interpolating. Fires in test/debug builds;
    // NDEBUG compiles it out on target. (Deliberate constants — count 0, flat breakpoints — don't trip.)
    assert(!axis_uncounted(d.x, x_on) && "table X axis: breakpoints populated but <axis>_n is 0 — set the live bin count");
    assert(!axis_uncounted(d.y, y_on) && "table Y axis: breakpoints populated but <axis>_n is 0 — set the live bin count");
    assert(!axis_uncounted(d.z, z_on) && "table Z axis: breakpoints populated but <axis>_n is 0 — set the live bin count");

    const int   xn = (x_on && d.x.breaks) ? axis_live_n(d.x) : 0;
    const int   yn = (y_on && d.y.breaks) ? axis_live_n(d.y) : 0;
    const int   zn = (z_on && d.z.breaks) ? axis_live_n(d.z) : 0;
    // Coordinates are engineering units, breakpoints are raw -> divide by the axis scale. Linear
    // interpolation is invariant under scaling both, so this is exact, not an approximation.
    const float xv = (xn > 0) ? as_coord(bus, x) / d.x.scale : 0.0f;
    const float yv = (yn > 0) ? as_coord(bus, y) / d.y.scale : 0.0f;
    const float zv = (zn > 0) ? as_coord(bus, z) / d.z.scale : 0.0f;
    return Site{ locate_axis(d.x.breaks, d.x.break_type, xn, xv),
                 locate_axis(d.y.breaks, d.y.break_type, yn, yv),
                 locate_axis(d.z.breaks, d.z.break_type, zn, zv) };
}

// Locate + read, which is what every value lookup wants. A module that LEARNS calls locate_site()
// and keeps the Site instead.
template <typename X, typename Y, typename Z>
inline float tableresolve(const TableDesc& d, SignalBus& bus, X x, Y y, Z z) {
    return interp_site(d, locate_site(d, bus, x, y, z));
}

// Fewer-axis convenience: a 2-D table needs only X+Y, a 1-D only X. The unused trailing coords are
// value 0 (gated out — those axes have no breaks). So a per-ETB 2-D ff_table is one X,Y call.
template <typename X, typename Y>
inline float tableresolve(const TableDesc& d, SignalBus& bus, X x, Y y) {
    return tableresolve(d, bus, x, y, 0.0f);
}
template <typename X>
inline float tableresolve(const TableDesc& d, SignalBus& bus, X x) {
    return tableresolve(d, bus, x, 0.0f, 0.0f);
}

// The channel-driven path: every axis reads its configured selector signal. Thin shim over
// tableresolve — passes each axis's `src` as a SignalId (a non-existent axis has src null -> SignalId 0,
// gated out by `breaks`). Existing callers (all-channel tables) keep using this unchanged.
inline float table_eval(const TableDesc& d, SignalBus& bus) {
    return tableresolve(d, bus,
                        static_cast<SignalId>(d.x.src ? *d.x.src : 0),
                        static_cast<SignalId>(d.y.src ? *d.y.src : 0),
                        static_cast<SignalId>(d.z.src ? *d.z.src : 0));
}

// The same table, read at an X the CALLER supplies — the sensor-cal shape, where the coordinate comes
// from the value in hand rather than from a bus channel. Y and Z still read their own sources, so a
// 2-D table evaluated this way is "this table, at my x, at whatever load it is now". This is what the
// expression VM's curve opcode resolves to: a curve IS a 1-D table, and the only thing that ever
// distinguished them was where X came from.
inline float table_eval_at(const TableDesc& d, SignalBus& bus, float x) {
    return tableresolve(d, bus, x,
                        static_cast<SignalId>(d.y.src ? *d.y.src : 0),
                        static_cast<SignalId>(d.z.src ? *d.z.src : 0));
}

}  // namespace tbl

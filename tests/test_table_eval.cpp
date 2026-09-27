#include "test_helpers.h"
#include "../firmware/Engine/TableEval.h"
#include "../firmware/Signal/SignalBus.h"
#include "../generated/signal_ids.h"

// Proves the channel-driven descriptor path + LIVE-stride indexing: a 3×3 live table inside a 4×4 max
// allocation must read at stride 3 (the live xn), not stride 4 (the max) — the resizable-table crux.
int main() {
    fprintf(stdout, "=== TableEval ===\n");

    SECTION("2D table_eval reads channels + indexes at the LIVE stride (3 of a 4-wide alloc)");
    {
        // 4×4 max alloc, but only 3×3 live. Cells laid out at LIVE stride 3: cell[y*3 + x].
        uint16_t cells[16] = {0};
        const float xb[3] = {1000.f, 2000.f, 3000.f};   // rpm
        const float yb[3] = {50.f, 75.f, 100.f};        // kPa
        uint8_t xn = 3, yn = 3;
        int16_t xsrc = SIG_RPM, ysrc = SIG_MAP;

        // target cell (xi=1,yi=1) at live stride 3 -> index 4; the SAME (1,1) at max stride 4 -> index 5.
        cells[1 * 3 + 1] = 420;     // = 42.0 at scale 0.1   (correct, live stride)
        cells[1 * 4 + 1] = 990;     // = 99.0                 (the wrong cell if max-stride were used)

        tbl::TableDesc d;
        d.cells = cells; d.cell_type = tbl::CELL_U16; d.scale = 0.1f;
        d.x = {xb, tbl::CELL_F32, &xn, 0, &xsrc, nullptr};
        d.y = {yb, tbl::CELL_F32, &yn, 0, &ysrc, nullptr};            // y always on; z absent

        SignalBus bus{};
        bus.set(SIG_RPM, 2000.0f);   // -> xi=1, xf=0
        bus.set(SIG_MAP,   75.0f);   // -> yi=1, yf=0
        CHECK_NEAR(tbl::table_eval(d, bus), 42.0, 1e-4);   // live stride, not 99.0

        // Resize live to a different stride and confirm it follows: make it 2×2 (stride 2).
        xn = 2; yn = 2;
        cells[1 * 2 + 1] = 150;      // (1,1) at stride 2 -> index 3 -> 15.0
        bus.set(SIG_RPM, 3000.0f);   // clamps to xi=1 (n=2 -> last span)
        bus.set(SIG_MAP, 100.0f);    // clamps to yi=1
        CHECK_NEAR(tbl::table_eval(d, bus), 15.0, 1e-4);
    }

    SECTION("optional Y axis disabled -> collapses to the x=… , y=0 row (1D)");
    {
        uint16_t cells[9] = {100, 200, 300,  10, 20, 30,  1, 2, 3};  // 3×3, row0 = 100..300
        const float xb[3] = {0.f, 50.f, 100.f};
        const float yb[3] = {0.f, 50.f, 100.f};
        uint8_t xn = 3, yn = 3, en = 0;
        int16_t xsrc = SIG_CLT, ysrc = SIG_MAP;

        tbl::TableDesc d;
        d.cells = cells; d.cell_type = tbl::CELL_U16; d.scale = 1.0f;
        d.x = {xb, tbl::CELL_F32, &xn, 0, &xsrc, nullptr};
        d.y = {yb, tbl::CELL_F32, &yn, 0, &ysrc, &en};                // optional, disabled

        SignalBus bus{};
        bus.set(SIG_CLT, 50.0f);     // xi=1
        bus.set(SIG_MAP, 100.0f);    // would be yi=2 if enabled
        CHECK_NEAR(tbl::table_eval(d, bus), 200.0, 1e-4);   // row 0 only (Y collapsed)
        en = 1;                                             // enable Y -> reads x=1 of the TOP row
        // Cells are row-major cell[y*xn + x], and MAP=100 clamps to the top of the y axis (row 2),
        // so this is cells[2*3 + 1] = 2. The old expectation of 30 read the grid transposed —
        // 30 is cells[1*3 + 2], row 1 column 2.
        CHECK_NEAR(tbl::table_eval(d, bus), 2.0, 1e-4);
    }

    SECTION("optional X axis disabled, then all axes off -> the bare 1x1 constant cell[0]");
    {
        uint16_t cells[9] = {100, 200, 300,  10, 20, 30,  1, 2, 3};  // 3×3
        const float xb[3] = {0.f, 50.f, 100.f};
        const float yb[3] = {0.f, 50.f, 100.f};
        uint8_t xn = 3, yn = 3, xen = 0, yen = 1;
        int16_t xsrc = SIG_CLT, ysrc = SIG_MAP;

        tbl::TableDesc d;
        d.cells = cells; d.cell_type = tbl::CELL_U16; d.scale = 1.0f;
        // ALLOC MATTERS THE MOMENT AN AXIS CAN BE TURNED OFF. The stride is the allocation; with it
        // left at 0 the engine falls back to the LIVE count, and disabling X drops that to 0 -> a
        // stride of 1, which silently re-addresses Y (row 1 would read cells[1], not cells[3]).
        // Every codegen'd descriptor states its allocation (see generated/table_descs.h), so this
        // only ever bites a hand-built desc like this one.
        d.x = {xb, tbl::CELL_F32, &xn, 0, &xsrc, &xen, 1.0f, 3};      // optional X, disabled
        d.y = {yb, tbl::CELL_F32, &yn, 0, &ysrc, &yen, 1.0f, 3};      // optional Y, enabled

        SignalBus bus{};
        bus.set(SIG_CLT, 100.0f);    // would be xi=2 if X were enabled
        bus.set(SIG_MAP,  50.0f);    // yi=1
        CHECK_NEAR(tbl::table_eval(d, bus), 10.0, 1e-4);    // X collapsed -> col 0, Y row 1 -> cell[3]=10

        yen = 0;                                            // now NO axis enabled -> 1x1 constant
        CHECK_NEAR(tbl::table_eval(d, bus), 100.0, 1e-4);   // cell[0], independent of either channel
    }

    SECTION("scaled X axis: raw breakpoints, engineering-unit coordinate");
    {
        // The ETB feed-forward shape: breakpoints stored raw in tenths of a percent (uint16, scale 0.1)
        // so the low end can carry sub-percent bins, while the coordinate arrives in percent — off the
        // bus or straight from a module. Without the axis scale the lookup lands at a tenth of the
        // intended position, which is exactly what made the throttle track its spring instead of its
        // target: at a 40 % demand it read the table at 4 %.
        int16_t cells[5] = {-300, -100, 0, 200, 500};                 // scale 0.1 -> -30 -20 0 20 50 %
        const uint16_t xb[5] = {0, 50, 100, 400, 1000};               // raw tenths -> 0 5 10 40 100 %
        uint8_t xn = 5;

        tbl::TableDesc d;
        d.cells = cells; d.cell_type = tbl::CELL_I16; d.scale = 0.1f;
        d.x = {xb, tbl::CELL_U16, &xn, 0, nullptr, nullptr, 0.1f};    // <- the axis scale

        SignalBus bus{};
        CHECK_NEAR(tbl::tableresolve(d, bus,   0.0f), -30.0, 1e-4);   // on a breakpoint
        CHECK_NEAR(tbl::tableresolve(d, bus,   5.0f), -10.0, 1e-4);
        CHECK_NEAR(tbl::tableresolve(d, bus,  10.0f),   0.0, 1e-4);
        CHECK_NEAR(tbl::tableresolve(d, bus,  40.0f),  20.0, 1e-4);   // the case that was reading 4 %
        CHECK_NEAR(tbl::tableresolve(d, bus, 100.0f),  50.0, 1e-4);
        CHECK_NEAR(tbl::tableresolve(d, bus,  25.0f),  10.0, 1e-4);   // interpolated between 10 and 40
        CHECK(tbl::nearest_bin(d.x, 40.0f) == 3);                    // nearest_bin scales too
        CHECK(tbl::nearest_bin(d.x,  4.9f) == 1);
    }

    fprintf(stdout, "ALL TableEval tests passed.\n");
    return test_summary();
}

#include "test_helpers.h"
#include "../firmware/Engine/TableEngine.h"

int main() {
    fprintf(stdout, "=== TableEngine ===\n");

    SECTION("1D linear over a float axis (Y/Z collapsed)");
    {
        const float xa[3] = {-40.0f, 0.0f, 100.0f};   // axis can be negative
        const uint16_t z[3] = {200, 100, 0};          // scale 0.1 -> 20.0, 10.0, 0.0
        // at x=-40 -> 20.0; x=50 -> halfway 0..100 of the second span -> 5.0; clamp below/above
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 3, -40.0f, nullptr, 0, 0, nullptr, 0, 0), 20.0, 1e-4);
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 3,  50.0f, nullptr, 0, 0, nullptr, 0, 0),  5.0, 1e-4);
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 3, -99.0f, nullptr, 0, 0, nullptr, 0, 0), 20.0, 1e-4); // clamp lo
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 3, 999.0f, nullptr, 0, 0, nullptr, 0, 0),  0.0, 1e-4); // clamp hi
    }

    SECTION("2D bilinear (Z collapsed)");
    {
        const float xa[2] = {1000.0f, 2000.0f};       // rpm
        const float ya[2] = {50.0f, 100.0f};          // kPa
        // cell[y*xn + x]: (x0y0,x1y0,x0y1,x1y1) = 10,20,30,40
        const uint16_t z[4] = {100, 200, 300, 400};   // scale 0.1
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 2, 1000.0f, ya, 2, 50.0f, nullptr, 0, 0), 10.0, 1e-4);
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 2, 2000.0f, ya, 2,100.0f, nullptr, 0, 0), 40.0, 1e-4);
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 2, 1500.0f, ya, 2, 75.0f, nullptr, 0, 0), 25.0, 1e-4); // centre
    }

    SECTION("3D trilinear (flex composition as Z)");
    {
        const float xa[2] = {1000.0f, 2000.0f};
        const float ya[2] = {50.0f, 100.0f};
        const float za[2] = {0.0f, 100.0f};           // ethanol %
        // z-plane 0 all 10.0, z-plane 1 all 20.0 (cell scale 0.1)
        const uint16_t cells[8] = {100,100,100,100, 200,200,200,200};
        // at ethanol 0 -> 10.0, at 100 -> 20.0, at 50 -> 15.0 (regardless of x/y since planes flat)
        CHECK_NEAR(tbl::interp(cells, 0.1f, xa,2,1500.0f, ya,2,75.0f, za,2,  0.0f), 10.0, 1e-4);
        CHECK_NEAR(tbl::interp(cells, 0.1f, xa,2,1500.0f, ya,2,75.0f, za,2,100.0f), 20.0, 1e-4);
        CHECK_NEAR(tbl::interp(cells, 0.1f, xa,2,1500.0f, ya,2,75.0f, za,2, 50.0f), 15.0, 1e-4);
    }

    SECTION("int16 cells (signed, e.g. ignition advance)");
    {
        const float xa[2] = {1000.0f, 2000.0f};
        const int16_t z[2] = {-50, 250};              // scale 0.1 -> -5.0, 25.0 deg
        CHECK_NEAR(tbl::interp(z, 0.1f, xa, 2, 1500.0f, nullptr, 0, 0, nullptr, 0, 0), 10.0, 1e-4);
    }

    SECTION("locate_axis / interp_at: a Site is the same answer, resolved separately");
    {
        const float xa[3] = {1000.0f, 2000.0f, 3000.0f};
        const float ya[2] = {50.0f, 100.0f};
        const uint16_t cells[6] = {100, 200, 300, 400, 500, 600};   // scale 0.1, cell[y*3 + x]

        // Locating each axis by hand and interpolating at the Site must equal the combined call, for
        // every kind of point: on a breakpoint, between two, and clamped off each end.
        const float xs[5] = {1000.0f, 1500.0f, 2500.0f, -99.0f, 9999.0f};
        const float ys[3] = {50.0f, 75.0f, 100.0f};
        for (float xv : xs) for (float yv : ys) {
            const tbl::Site s{ tbl::locate_axis(xa, tbl::CELL_F32, 3, xv),
                               tbl::locate_axis(ya, tbl::CELL_F32, 2, yv),
                               tbl::locate_axis(nullptr, tbl::CELL_F32, 0, 0.0f) };
            CHECK_NEAR(tbl::interp_at(cells, 0.1f, s),
                       tbl::interp(cells, 0.1f, xa, 3, xv, ya, 2, yv, nullptr, 0, 0), 1e-6);
        }
    }

    SECTION("nearest_of matches nearest, without re-searching");
    {
        const float xa[3] = {0.0f, 100.0f, 200.0f};
        const float vs[10] = {-10.0f, 0.0f, 40.0f, 49.9f, 50.0f, 60.0f, 100.0f, 149.0f, 151.0f, 999.0f};
        for (float v : vs) {
            const tbl::AxisPos p = tbl::locate_axis(xa, tbl::CELL_F32, 3, v);
            CHECK(tbl::nearest_of(p) == tbl::nearest(xa, tbl::CELL_F32, 3, v));
        }
        // A degenerate axis pins to bin 0 either way.
        CHECK(tbl::nearest_of(tbl::locate_axis(xa, tbl::CELL_F32, 1, 999.0f)) == 0);
    }

    SECTION("a 1-wide X reads only the cell it has");
    {
        // n<2 on X used to still address cell 1 (its fraction was 0, so the VALUE was right, but the
        // read left a packed 1-element grid). The blend must be unchanged and stay in bounds.
        const uint16_t one[1] = {123};
        const float xa[1] = {500.0f};
        const tbl::Site s{ tbl::locate_axis(xa, tbl::CELL_F32, 1, 4000.0f),
                           tbl::locate_axis(nullptr, tbl::CELL_F32, 0, 0.0f),
                           tbl::locate_axis(nullptr, tbl::CELL_F32, 0, 0.0f) };
        CHECK(s.x.i == 0 && s.x.f == 0.0f);
        CHECK_NEAR(tbl::interp_at(one, 0.1f, s), 12.3, 1e-4);
    }

    fprintf(stdout, "ALL TableEngine tests passed.\n");
    return test_summary();
}

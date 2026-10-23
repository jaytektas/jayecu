#pragma once

// AutotuneView — the autotuner's grid, drawn on the map's own axes.
//
// It is the SAME grid as the fuel table's, because a proposal that does not line up cell-for-cell with
// the map it is about cannot be checked by eye. What changes is what each cell is asked to show:
//
//   Base       what the table holds now — the thing being judged.
//   Proposed   what it would hold if the proposal were applied.
//   Change     the difference, as a percent, on a diverging scale. Red adds fuel, blue takes it away,
//              and an untouched cell is neutral rather than "zero-coloured" — the eye should find the
//              cells that moved without reading a single number.
//   Weight     how much evidence each cell has. This is the answer to the question a tuner asks first
//              when an autotune appears to have done nothing: not "is it wrong" but "has it seen
//              anything here at all".
//
// EVERY MODE CARRIES THE COVERAGE BAR, a sliver along the bottom of each cell in proportion to its
// weight. Coverage is not a separate question you should have to switch modes to ask — a +12 % change
// in a cell that saw one record and a +12 % in one that saw fifty are different claims, and they
// should not look identical while you are deciding whether to press Apply.
//
// THREE VIEWS of whichever of those is shown, on the right-click menu as a table has them: the grid,
// a 3D surface (drag to orbit, wheel for height), and a slice — one load row as bars across rpm (wheel
// steps the row; clicking a cell in the grid picks it). The colours are the grid's, so a red hump in
// 3D is the same red cell.
//
// THE LIVE CURSOR is where the last accepted record landed, which is not where the engine is now: the
// gas being read was made some time ago, and the whole point of the delay lookback is that those are
// different cells. Watching it trail the operating point during a pull is how you see the lookback
// working — or see that the delay table is wrong.

#include <j/core/JWidget.h>
#include <j/core/MenuSystem.h>

#include "../model/Autotune.h"

#include <string>

class AutotuneView : public jf::JWidget {
public:
    enum class Mode { Base, Proposed, Change, Weight, Session };
    enum class Layout { Grid, Surface, Slice };

    explicit AutotuneView(jf::JSceneGraph& g);

    // Borrowed, not owned: the panel owns the engine and this draws whatever it currently holds.
    void setEngine(const autotune::Engine* e) { eng_ = e; invalidate(); }
    void setMode(Mode m) { mode_ = m; invalidate(); }
    [[nodiscard]] Mode mode() const { return mode_; }
    void setLayout(Layout l);
    [[nodiscard]] Layout layout() const { return layout_; }
    // What the axes are, in words — an autotuner pointed at a table whose Y axis is MAP and one whose
    // Y axis is airmass are doing different things, and the panel should say which.
    void setAxisLabels(std::string x, std::string y) { xLabel_ = std::move(x); yLabel_ = std::move(y); invalidate(); }
    // Drawn instead of the grid: not connected, no definition, no table. Said outright, because an
    // empty grid reads as "nothing has happened yet" when it means "there is nothing here to draw".
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    void handleMousePress(float mx, float my) override;
    void handleMouseMove(float mx, float my) override;
    void handleMouseRelease(float mx, float my) override;
    bool handleScroll(float mx, float my, float wheel) override;
    void prepareContextMenu(float mx, float my) override;

private:
    struct Cell;   // what one cell shows in the current mode: colour, number, height
    // The ranges every mode is read against, worked out once per frame and shared by all three layouts.
    struct Scales { double lo = 0.0, hi = 1.0, fullW = 0.0, authority = 2.0, sessionMax = 2.0; };
    Scales scales_;
    Cell cellOf(int col, int row) const;
    void drawSurface(jf::JPrimitiveBuffer& buf, const jf::JRect& r);
    void drawSlice(jf::JPrimitiveBuffer& buf, const jf::JRect& r);

    Layout      layout_ = Layout::Grid;
    jf::JMenu   menu_{ "Auto Tune" };
    jf::JMenuItem* viewItems_[3] = {};
    int         sliceRow_ = -1;                 // storage row; -1 follows the last record
    float       yaw_ = 35.f, pitch_ = 28.f, hScale_ = 1.f;
    float       pressX_ = 0.f, pressY_ = 0.f, lastX_ = 0.f, lastY_ = 0.f;
    bool        pressed_ = false, dragged_ = false;
    // The grid's geometry as last drawn, so a click can be turned back into a cell.
    float       gx_ = 0.f, gy_ = 0.f, cw_ = 0.f, ch_ = 0.f;
    const autotune::Engine* eng_ = nullptr;
    Mode        mode_ = Mode::Change;
    std::string xLabel_, yLabel_, notice_;
};

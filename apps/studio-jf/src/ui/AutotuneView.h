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
// THE LIVE CURSOR is where the last accepted record landed, which is not where the engine is now: the
// gas being read was made some time ago, and the whole point of the delay lookback is that those are
// different cells. Watching it trail the operating point during a pull is how you see the lookback
// working — or see that the delay table is wrong.

#include <j/core/JWidget.h>

#include "../model/Autotune.h"

#include <string>

class AutotuneView : public jf::JWidget {
public:
    enum class Mode { Base, Proposed, Change, Weight };

    explicit AutotuneView(jf::JSceneGraph& g) : jf::JWidget(g, "AutotuneView") {}

    // Borrowed, not owned: the panel owns the engine and this draws whatever it currently holds.
    void setEngine(const autotune::Engine* e) { eng_ = e; invalidate(); }
    void setMode(Mode m) { mode_ = m; invalidate(); }
    [[nodiscard]] Mode mode() const { return mode_; }
    // What the axes are, in words — an autotuner pointed at a table whose Y axis is MAP and one whose
    // Y axis is airmass are doing different things, and the panel should say which.
    void setAxisLabels(std::string x, std::string y) { xLabel_ = std::move(x); yLabel_ = std::move(y); invalidate(); }
    // Drawn instead of the grid: not connected, no definition, no table. Said outright, because an
    // empty grid reads as "nothing has happened yet" when it means "there is nothing here to draw".
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

private:
    const autotune::Engine* eng_ = nullptr;
    Mode        mode_ = Mode::Change;
    std::string xLabel_, yLabel_, notice_;
};

#pragma once

// AutotuneView — the autotuner's grid: the map's own table widget, showing what the tuner thinks of it.
//
// It IS the table being tuned — a TableWidget bound to that map — because a proposal that does not line
// up cell-for-cell with the map it is about cannot be checked by eye, and because the table already
// knows how to be read: breakpoints, units, orientation, the grid / 3D / slice views. What changes is
// what each cell is asked to show (TableWidget::CellSource), and that it cannot be typed into:
//
//   Base       what the table holds now — the thing being judged.
//   Proposed   what it would hold if the proposal were applied.
//   Change     the difference, as a percent, on a diverging scale. Red adds fuel, blue takes it away,
//              and an untouched cell is neutral rather than "zero-coloured" — the eye should find the
//              cells that moved without reading a single number.
//   Weight     how much evidence each cell has. This is the answer to the question a tuner asks first
//              when an autotune appears to have done nothing: not "is it wrong" but "has it seen
//              anything here at all".
//   Session    how far each cell has moved since Start (or Reset), however many writes that took.
//
// EVERY MODE CARRIES THE COVERAGE BAR, a sliver along the bottom of each cell in proportion to its
// weight. Coverage is not a separate question you should have to switch modes to ask — a +12 % change
// in a cell that saw one record and a +12 % in one that saw fifty are different claims, and they
// should not look identical while you are deciding whether to press Apply.
//
// THE OUTLINED CELL is where the last accepted record landed, which is not where the engine is now: the
// gas being read was made some time ago, and the whole point of the delay lookback is that those are
// different cells. Watching it trail the operating point during a pull is how you see the lookback
// working — or see that the delay table is wrong.
//
// THE VIEW is on the right-click menu, as a table's is: 2D Grid, 3D Surface, Slice.

#include <j/core/JWidget.h>
#include <j/core/MenuSystem.h>

#include "../model/Autotune.h"
#include "../surface/widgets/TableWidget.h"

#include <string>

class AutotuneView : public jf::JWidget {
public:
    enum class Mode { Base, Proposed, Change, Weight, Session };
    enum class Layout { Grid, Surface, Slice };

    explicit AutotuneView(jf::JSceneGraph& g);

    // Borrowed, not owned: the panel owns the engine and this draws whatever it currently holds.
    void setEngine(const autotune::Engine* e) { eng_ = e; invalidate(); }
    // The table being tuned, and the plane of it the run is about.
    void setTable(const std::string& path);
    void setPlane(int z) { plane_ = z; invalidate(); }
    void setMode(Mode m) { mode_ = m; invalidate(); }
    [[nodiscard]] Mode mode() const { return mode_; }
    void setLayout(Layout l);
    [[nodiscard]] Layout layout() const { return static_cast<Layout>(table_->view()); }
    // Drawn instead of the grid: not connected, no definition, no table. Said outright, because an
    // empty grid reads as "nothing has happened yet" when it means "there is nothing here to draw".
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    void handleMousePress(float mx, float my) override;
    void handleMouseMove(float mx, float my) override;
    void handleMouseRelease(float mx, float my) override;
    bool handleScroll(float mx, float my, float wheel) override;
    bool handleKeyEvent(const jf::JKeyEvent& e) override;
    void prepareContextMenu(float mx, float my) override;

private:
    // What each cell shows, per mode — the table asks, this answers from the engine.
    class Source : public TableWidget::CellSource {
    public:
        explicit Source(const AutotuneView& v) : v_(v) {}
        double value(int i0, int i1) const override;
        bool   fill(int i0, int i1, uint8_t out[4]) const override;
        bool   label(int i0, int i1, std::string& out) const override;
        float  coverage(int i0, int i1) const override;
        bool   marked(int i0, int i1) const override;
        bool   range(double& lo, double& hi) const override;
        std::string units() const override;
        int    plane() const override { return v_.plane_; }
    private:
        const AutotuneView& v_;
    };
    // The ranges every mode is read against, worked out once per frame.
    struct Scales { double lo = 0.0, hi = 1.0, fullW = 0.0, authority = 2.0, sessionMax = 2.0; };
    void computeScales();
    jf::JRect tableRect() const;
    bool ready() const;

    const autotune::Engine* eng_ = nullptr;
    Mode         mode_ = Mode::Change;
    int          plane_ = 0;
    bool         pressed_ = false;      // a button is down: moves are a drag
    std::string  notice_, tablePath_;
    Scales       scales_;
    Source       source_{ *this };
    TableWidget* table_ = nullptr;          // adopted: hides with this view
    jf::JMenu    menu_{ "Auto Tune" };
    jf::JMenuItem* viewItems_[3] = {};
};

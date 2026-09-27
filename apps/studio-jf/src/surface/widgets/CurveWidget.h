#pragma once

// CurveWidget — an editable X/Y curve over a bound table: draggable
// points, axis/value units, a coloured line. A real CanvasWidget subclass carrying its typed properties;
// pixels + point editing still come from the "curve" class render()/onControlInput(), defined in CurveWidget.cpp.

#include "../CanvasWidget.h"

struct TableImage;   // model/TableImage.h — only referenced by curveRow(); the .cpp includes the full type

class CurveWidget : public CanvasWidget {
public:
    explicit CurveWidget(jf::JSceneGraph& g) : CanvasWidget(g, "curve") {}
    std::string elementType() const override { return "curve"; }
    std::string paletteTitle() const override { return "Curve"; }
    float       defaultW()     const override { return 240.f; }
    float       defaultH()     const override { return 160.f; }
    bool        interactive()    const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // Per-element Curve drag state: index of the point being dragged (-1 = none). Keyed by element id, so the
    // render (highlight) and onInput (hit + move) agree.
        // Keyed by UID: an element id is unique within ONE model, and these maps are global — two widgets
    // with the same id on different pages would otherwise share this state. See TableWidget::tableCursors.
    static std::unordered_map<std::string, int>& curveDrags() { static std::unordered_map<std::string, int> m; return m; }
    static int  curveDragIndex(const std::string& id) { auto it = curveDrags().find(id); return it == curveDrags().end() ? -1 : it->second; }
    static void setCurveDragIndex(const std::string& id, int v) { curveDrags()[id] = v; }

    // Persistent selected-point index (survives mouse release, unlike the drag index) so keyboard value
    // nudges (Keymap Increase/Decrease) have a target and render can highlight it. -1 = no selection.
    static std::unordered_map<std::string, int>& curveSels() { static std::unordered_map<std::string, int> m; return m; }
    static int  curveSelIndex(const std::string& id) { auto it = curveSels().find(id); return it == curveSels().end() ? -1 : it->second; }
    static void setCurveSelIndex(const std::string& id, int v) { curveSels()[id] = v; }

    // Plot range FROZEN for the duration of a drag. With no declared xRange/yRange, plotOf() auto-ranges
    // from the live data — so writing the dragged point back moves the very axis used to read the mouse.
    // The same cursor then maps to a new value the next frame and the point ROLLS with the mouse held
    // still. Capture the range at Press and read the mouse against THAT until Release. Keyed by element id.
    struct DragRange { double xlo, xhi, ylo, yhi; };
    static std::unordered_map<std::string, DragRange>& curveDragRanges() { static std::unordered_map<std::string, DragRange> m; return m; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("axisUnit",  this, &CurveWidget::m_axisUnit,  JPropertyMeta{ .label = "Axis Unit", .def = "Auto",  .order = 100 });
        m.add("valueUnit", this, &CurveWidget::m_valueUnit, JPropertyMeta{ .label = "Value Unit", .def = "Auto", .order = 101 });
        m.add("lineColor", this, &CurveWidget::m_lineColor, JPropertyMeta{ .label = "Curve Colour", .editor = "color", .order = 102 });
        // Which Y row of a 2D table this curve edits. A curve is 1D by nature, so a 2D table is shown one
        // row at a time; 0 == the first row (and the only meaningful value for a 1D curve). SIGIL-AWARE: a
        // plain number selects that row, but an expression ($telemetry / #config / @widget) drives the row
        // from a live value, so the curve can follow the operating point. Evaluated + clamped at use.
        m.add("row", this, &CurveWidget::m_row, JPropertyMeta{ .label = "Table Row", .editor = "expr", .order = 103 });
        // Declared axes. Empty = auto (range from the data), which is the behaviour every curve had before
        // these existed. An imported curve states its own, so an untuned one still reads as its real span
        // (0..8000 rpm) instead of a flat line through zero.
        m.add("xLabel", this, &CurveWidget::m_xLabel, JPropertyMeta{ .label = "X Axis Label", .order = 104 });
        m.add("yLabel", this, &CurveWidget::m_yLabel, JPropertyMeta{ .label = "Y Axis Label", .order = 105 });
        m.add("xRange", this, &CurveWidget::m_xRange, JPropertyMeta{ .label = "X Range (lo,hi)", .order = 106 });
        m.add("yRange", this, &CurveWidget::m_yRange, JPropertyMeta{ .label = "Y Range (lo,hi)", .order = 107 });
        m.add("grid",   this, &CurveWidget::m_grid,   JPropertyMeta{ .label = "Grid (cols,rows)", .order = 108 });
    }

    // The 2D-table row this curve is bound to, clamped to [0, liveRows-1]. Shared by render + input so the
    // drawn curve and the edited cells are always the same row.
    int curveRow(const Cache& c, const TableImage& t, const std::string& rowExpr) const;

    // Plot geometry — the ONE source of the plot rect + value ranges, so render (draws the handles) and
    // onControlInput (hit-tests them) can never disagree. ptX/ptY map a data point to a screen pixel;
    // both sides use them. ok is false when the widget is too small or there's no usable data.
    struct Plot {
        float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;   // inner plot rect (inside the label margins)
        double xlo = 0.0, xhi = 1.0, ylo = 0.0, yhi = 1.0;
        bool ok = false;
        float ptX(double x) const { return px + static_cast<float>((x - xlo) / (xhi - xlo)) * pw; }
        float ptY(double y) const { return (py + ph) - static_cast<float>((y - ylo) / (yhi - ylo)) * ph; }
    };
    // xs/ys empty -> the rect is still filled (for the frame/placeholder) but ok=false. With data, the
    // value ranges (incl. the 6% headroom) are filled and ok=true.
    // `decl` is the DECLARED range, "lo,hi" — empty means auto (from the data). `grid` is "cols,rows".
    static Plot plotOf(const jf::JRect& r, const std::vector<double>& xs, const std::vector<double>& ys,
                       const std::string& xDecl = {}, const std::string& yDecl = {});
    // Parse a "lo,hi" pair; false if it is empty or malformed (then the caller keeps its auto range).
    static bool declRange(const std::string& s, double& lo, double& hi);

private:
    std::string m_axisUnit = "Auto";
    std::string m_valueUnit = "Auto";
    std::string m_lineColor;
    std::string m_row;       // selected Y row for a 2D table; "" or a number, or a sigil expression
    std::string m_xLabel, m_yLabel;    // axis titles (an imported curve's columnLabel)
    std::string m_xRange, m_yRange;    // declared "lo,hi"; empty = auto from the data
    std::string m_grid;                // declared "cols,rows" grid divisions; empty = the default 4 rows
};

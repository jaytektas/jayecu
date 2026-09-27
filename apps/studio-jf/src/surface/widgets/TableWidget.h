#pragma once

// TableWidget — a placed 2D/3D tuning map, fully interactive. A real CanvasWidget subclass carrying its typed properties; its (large) render + keymap-driven
// editing still come from the "table" class render()/onControlInput(), defined in TableWidget.cpp. Enum properties store the option INDEX (editor "enum"); index 0 = "global" (follow
// the Preferences default).

#include "../CanvasWidget.h"
#include "../../model/MetaModel.h"   // TableImage (full type for the TableGeom data member + tableGeom)
#include "../../model/Cache.h"       // resolveTable — schema cell min/max default for the Cell Min/Max props

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

class TableWidget : public CanvasWidget {
public:
    explicit TableWidget(jf::JSceneGraph& g) : CanvasWidget(g, "table") {}
    std::string elementType() const override { return "table"; }
    float naturalWidth(float authoredW)  const override;
    float naturalHeight(float authoredH) const override;
    std::string paletteTitle() const override { return "Table"; }
    float       defaultW()     const override { return 380.f; }
    float       defaultH()     const override { return 280.f; }
    double      sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const override;
    std::vector<std::string> sigilNames() const override;
    bool        interactive()    const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // ------------------------------------------------------------------
    // The widget OWNS its view mode (0 = 2D grid, 1 = 3D surface, 2 = slice). Set through this interface;
    // render() and onControlInput() read the member, never the prop bag. It is deserialized ONCE from the
    // element on creation (loadContent) and serialized back on save (saveContent) — the element is the file
    // format, not a live per-frame feed. No external code reaches into a model to change it.
    // ------------------------------------------------------------------
    // Author the view as an OVERRIDE in the owned store (setOwnProp), so it rides the same commit/inheritance
    // path as every other prop: view 0 (grid, the default) clears the override to re-inherit; 1/2 set it. The
    // render reads m_view, which loadContent re-derives from the (resolved) element on every re-source/edit.
    void setView(int v) {
        v = std::clamp(v, 0, 2);
        if (m_view == v) return;
        if (v) setOwnProp("view", std::to_string(v)); else clearOwnProp("view");
        emitModified();
    }
    int  view() const { return m_view; }

    void loadContent(const PanelElement& el) override {
        const std::string s = el.prop("view");           // derive the owned member from the element each re-source
        m_view = s.empty() ? 0 : std::clamp(std::atoi(s.c_str()), 0, 2);
        m_axisColor = el.prop("axisColor");              // "" = follow the JStyle palette default (render resolves)
        m_gridColor = el.prop("gridColor");
    }

    // ------------------------------------------------------------------
    // Shared table state + geometry (the table state + geometry + cell ops (owned here; used by CurveWidget +
    // WidgetSupport.cpp). The table OWNS this: render + run-mode input + the Surface's table context menu ops
    // all reach the SAME per-element cursor map + geometry through these public statics. CurveWidget and the
    // Surface need them, hence public.
    // ------------------------------------------------------------------

    // Per-element edit cursor (screen row 0 = top). Shared between the render (draws the cursor + edit text)
    // and onInput (moves the cursor, writes cells). row/col = the active cell (keyboard focus, edit target).
    // aRow/aCol = the selection anchor; the selected block is the rectangle [min..max] of the two. Single cell
    // when anchor == active. selecting = a press-drag rubber-band is in progress. pendingOp: a value transform
    // armed by the context menu (0 none, 1 Set to, 2 Increase by, 3 Decrease by, 4 % change).
    struct TableCursor { int row = 0, col = 0, aRow = 0, aCol = 0, pendingOp = 0, sliceDrag = -1, axisEdit = 0, axisIdx = 0, z = 0, hoverAxis = -1; bool editing = false, selecting = false; std::string buf; };
    // axisEdit: 0 = editing a cell, 1 = editing an X (horizontal) breakpoint, 2 = editing a Y (vertical) breakpoint; axisIdx = the bin index within that axis.
    // KEYED BY UID, NOT ID. An element id is unique within ONE model — a page, a panel, a surface — and
    // these maps are global. Two tables with the same id on different pages therefore SHARED a cursor:
    // pick a column in one and the focused column moved in the other, which on a document with dozens of
    // authored pages (every one numbering its widgets from 1) is dozens of tables wearing each other's
    // state. The uid is the identity that is unique across the whole document, so it is the key.
    static std::unordered_map<std::string, TableCursor>& tableCursors() { static std::unordered_map<std::string, TableCursor> m; return m; }

    // Grid-view scroll offset, per placed element. Only ever non-zero when the cells DON'T stretch (the
    // Fixed / "Resize to contents" sizing modes), because a stretched grid is defined to fill its widget and
    // so can never overflow. Offsets are <= 0 (content slides up/left under a fixed viewport). drag: 0 none,
    // 1 vertical thumb, 2 horizontal thumb; grab = cursor offset within the thumb when the drag started, so
    // the thumb doesn't jump to the cursor on press.
    struct TableScroll { float x = 0.f, y = 0.f; int drag = 0; float grab = 0.f; };
    static std::unordered_map<std::string, TableScroll>& tableScrolls() { static std::unordered_map<std::string, TableScroll> m; return m; }
    static constexpr float kTSB = 10.f;   // scrollbar gutter, matching Surface::kSB

    // 3D-surface view state (orbit + height) + drag tracking — OWNED by this widget instance (was a global
    // static map keyed by element id, addressed from the outside; now it is simply the widget's own state).
    struct Table3DState { float yaw = 35.f, pitch = 28.f, hScale = 1.f, pressX = 0, pressY = 0, lastX = 0, lastY = 0; bool dragged = false; };

    // Resolved table + grid layout for one placed Table element. render + onInput compute this identically so
    // a click hits the same cell the render drew. Screen row rr (0=top) maps to data row (rows-1-rr): highest
    // Y breakpoint on top, the tuning-table convention.
    struct TableGeom {
        bool ok = false;
        TableImage t;
        int cols = 0, rows = 0, dec = 1, n0 = 1;   // n0 = live count of STORAGE axis 0 (the fast/inner dimension)
        // Breakpoint precision for the horizontal / vertical SCREEN axes (xb / yb). Resolved from the
        Cache::AxisView viewH, viewV;   // how each SCREEN axis is shown (unit + precision) — see axisView
        // storage-indexed decimalsX/decimalsY props and mapped through transpose, so they track the axis
        // their matching scaleX/scaleY prop scales. Independent of `dec`, which is the CELL precision —
        // an RPM axis wants 0 decimals while its cells want 1.
        int decH = 1, decV = 1;
        int z = 0, zn = 1;                         // current display plane + live plane count (>1 only for 3D tables)
        bool heat = true, transpose = false;       // transpose: swap which storage axis runs vertical vs horizontal
        bool yRight = false, xBottom = false;      // label-strip placement (display only, no data effect)
        std::vector<double> xb, yb;
        float gx = 0, gy = 0, cw = 0, ch = 0, hdrW = 0, hdrH = 0;
        // The size a cell needs for the number it shows, before any stretching — the floor the Stretch
        // mode never goes under and the width Fixed pins to. Carried here so the ONE place that works it
        // out is also the one that answers "how wide does this grid want to be" (naturalWidth below).
        float natW = 0, natH = 0;
        float nameH = 0;                            // height of the axis-name bar above the grid (0 = no names)
        // The element's Font ("Font" — the base property EVERY widget inherits from CanvasWidget) resolved
        // ONCE here, because render and the hit-test both derive from this struct: text drawn at one size
        // and clicked at another would land on the wrong cell. lh is the resolved line height (the grid's
        // row metric), scale/face are what the text calls need. Unset font -> the app font, as before.
        float lh = 0, fscale = 1.0f;
        std::string face;
        // Scroll state, resolved by tableGeom. gx/gy ALREADY include sx/sy, so every draw + hit-test that
        // derives from them scrolls for free; sx/sy are kept only for the few positions anchored to the
        // widget edge instead of the grid origin (the pinned header strips). vx/vy/vw/vh is the viewport the
        // content is clipped to (grid + header strips, minus any bar gutter); contentW/H is the full extent.
        float sx = 0, sy = 0, vx = 0, vy = 0, vw = 0, vh = 0, contentW = 0, contentH = 0;
        bool  ovH = false, ovV = false;             // content overflows horizontally / vertically (bars shown)
        std::string vName, hName;                   // signal driving the vertical / horizontal screen axis (grid-view label bar)
        // Screen (row 0 = top, col 0 = left) -> storage linear index (z*plane + i1*n0 + i0). Highest breakpoint
        // on top. Default (transpose=false): vertical = axis0, horizontal = axis1 (matches the AxisLayout). The
        // plane stride is rows*cols cells (== live n0*n1), so a 3D table's current Z plane offsets automatically.
        // Screen position -> storage indices -> address. The ADDRESS half is Cache::tiCellOffset and
        // nothing else: cells are laid out at the ALLOCATION's stride, and this had its own copy of the
        // arithmetic striding by the LIVE counts. Two copies of one layout rule is how a table drawn by
        // this widget disagreed with the same table read by everything else — ten padding cells of drift
        // per row, so the bottom row looked right and the rest walked off it.
        int cellOffset(int screenRow, int col) const {
            const int vIdx = rows - 1 - screenRow, hIdx = col;
            const int i0 = transpose ? hIdx : vIdx, i1 = transpose ? vIdx : hIdx;
            return Cache::instance().tiCellOffset(t, i0, i1, z);
        }
        double breakpointX(int col) const { return col < (int)xb.size() ? xb[col] : 0.0; }
        double breakpointY(int screenRow) const { const int d = rows - 1 - screenRow; return d < (int)yb.size() ? yb[d] : 0.0; }
    };
    // elemCtx: the host viewport's sensor, so a template page's "sensors.sensor[*].cal" resolves. Static
    // (the cell ops call it without an instance), so the context is passed rather than read off `this`.
    // `zoom` is the VIEW SCALE the container is painting at. A table's cells are sized from its font —
    // row height and natural column width are both text metrics — so without it the grid draws at 1:1
    // whatever the surface does: scaling a page gave the table more room and it answered with MORE cells
    // of the same size, never bigger ones. Every other widget already folds renderZoom() into its text.
    // Defaulted, because the hit-test and the tests ask about an unscaled grid.
    static TableGeom tableGeom(const PanelElement& el, const jf::JRect& r, const Cache& c,
                               const std::string& elemCtx = {}, float zoom = 1.f);

    // Run-mode table cell selection (the rubber-band block), read by the Surface's table context menu.
    // Screen-row indices (0 = top row, as drawn); the active cell is the keyboard/edit focus. has == false
    // means no cursor exists for that element yet.
    struct TableCellSelection { int r0 = 0, r1 = 0, c0 = 0, c1 = 0, activeRow = 0, activeCol = 0; bool has = false; };
    // Decimals for one storage axis: an explicit decimalsX/Y/Z choice, else the precision of the CHANNEL
    // that axis reads, else the cells' own. See the definition — this is why a dragged-out table and one
    // whose channel just changed both show the right precision with nothing configured.
    static int axisDecimals(const PanelElement& el, const Cache& c, const TableImage& t,
                            int storageAxis, int cellDec);
    // How one axis is shown: its display unit, the converted domain and the precision that unit needs.
    // The single place the grid, its inline editor and the Axis Setup dialog agree on what a bin READS
    // as, so a value typed in volts and a value drawn in volts cannot mean different things.
    static Cache::AxisView axisView(const PanelElement& el, const Cache& c, const TableImage& t,
                                    int storageAxis);
    // The element's per-axis display-unit prop ("displayUnitX/Y/Z"), or "" past the third axis.
    static const char* axisUnitProp(int storageAxis);
    // Axis name + the unit it is read in, and the unit the CELLS are read in — the name bar's text.
    static std::string _named(const std::string& name, const Cache::AxisView& view);
    static std::string _cellUnits(const PanelElement& el, const TableGeom& g);
    // This element's OWN decimals choice for a prop (0 = it does not say). See the definition for why
    // this is not choiceOf().
    static int ownDecimals(const PanelElement& el, const char* key);
    // Which axis strip the pointer is over, as a STORAGE axis index (-1 = the cells). Tracked on hover so
    // "increase decimals" can act on what you are pointing at rather than always on the grid.
    static int  tableHoverAxis(const std::string& uid);

    static TableCellSelection tableCellSelection(const std::string& uid);

    // Arm a value transform on a table (op: 1 Set-to, 2 Increase-by, 3 Decrease-by, 4 %-change) and put it into
    // inline-entry mode: the user types the operand into the active cell and Enter applies it across the block.
    static void tableBeginOp(const std::string& uid, int op);
    static int  tableCurrentPlane(const std::string& uid);   // the table's active Z plane (0 for a 2D table)
    static void tableSetPlane(const std::string& uid, int z);   // set the active Z plane (caller clamps to the plane count)

    // THE single screen(row,col) -> storage-offset mapping for a bound table, honouring the "transpose" property,
    // shared by the render/input and the Surface's menu ops so the drawn cell == the written cell.
    struct TableDims { int rows = 1, cols = 1, n0 = 1; bool transpose = false; };
    static TableDims tableDims(const Cache& c, const TableImage& t, bool transpose);
    // …and the plane it is on: see the definition. No default — a cell op that forgets which plane it is
    // editing silently edits a different one.
    static int       tableCellOffset(const TableImage& t, const TableDims& d, int screenRow, int col, int z);

    // THE two table-cell accessors, in DISPLAY units — the unit the grid draws and the user types.
    //
    // A table used to read with the cell scale baked in and write through the engineering door, never
    // touching dispV/srcV. That worked, and it also meant a table could not be shown in any unit but the
    // schema's: no F on a coolant map, no psi on a boost target, however the user had set their
    // preferences. Every other widget converts at the glass; these let a table do the same, and they are
    // static so the Surface's own table operations (paste, smooth, interpolate, transform) go through
    // the identical pair rather than a parallel one in engineering units.
    static double cellValue(const PanelElement& el, const TableImage& t, int offset);
    static void   setCellValue(const PanelElement& el, const TableImage& t, int offset, double display);
    static bool      tableTransposed(const PanelElement& el);   // the element's current transpose state (from its axisCombo 0..7)

    // The element's setting for `key`, with choice 0 ("global") answered by Preferences ▸ Globals — the
    // ONE place "global" resolves. Returns a real choice (1..N), or 0 where the global offers a "follow"
    // answer (the axis decimals follow the cell's).
    static int       choiceOf(const PanelElement& el, const char* key);

    // Every property that offers "global", with the value its global falls back to when unset. ONE table:
    // the render reads it through choiceOf, the Preferences page builds itself from it. Each default is
    // exactly the literal the render used before the globals existed, so an unconfigured studio is
    // pixel-identical to what it was.
    struct GlobalDef { const char* key; int def; };
    static const std::vector<GlobalDef>& globalDefs();
    static int       globalDefault(const char* key);

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        const jf::VariantList secSizing = { std::string("global"), std::string("Interactive"), std::string("Stretch"), std::string("Fixed"), std::string("Resize to contents") };
        const jf::VariantList onOff     = { std::string("global"), std::string("Off"), std::string("On") };
        m.add("axisMode", this, &TableWidget::m_axisMode, JPropertyMeta{ .label = "Axis Display", .editor = "enum", .order = 100,
              .choices = { std::string("global"),
                           std::string("transpose left & top"), std::string("transpose right & top"), std::string("transpose left & bottom"), std::string("transpose right & bottom"),
                           std::string("left & top"), std::string("right & top"), std::string("left & bottom"), std::string("right & bottom") } });
        // CELL SIZE, this grid's own. Everything else on a page is sized for reading a label; a table is
        // sized for reading a FIELD OF NUMBERS, and the two want different densities on the same screen.
        // A 22x23 ignition map wants to be compact enough to see as a shape; a 4x4 correction table wants
        // room. This multiplies the grid's text metrics — which is what its row height and column width
        // are made of — on top of the interface scale, so it says "denser/looser THAN the rest of the UI"
        // rather than fighting it.
        m.add("cellScale", this, &TableWidget::m_cellScale, JPropertyMeta{ .label = "Cell size", .editor = "enum", .order = 104,
              .choices = { std::string("global"), std::string("70 %"), std::string("85 %"), std::string("100 %"),
                           std::string("125 %"), std::string("150 %"), std::string("200 %") } });
        m.add("hSectionMode",  this, &TableWidget::m_hSectionMode,  JPropertyMeta{ .label = "Columns sizing", .editor = "enum", .order = 101, .choices = secSizing });
        m.add("vSectionMode",  this, &TableWidget::m_vSectionMode,  JPropertyMeta{ .label = "Rows sizing",    .editor = "enum", .order = 102, .choices = secSizing });
        const jf::VariantList decChoices = { std::string("global"), std::string("0"), std::string("1"), std::string("2"),
                                            std::string("3"), std::string("4"), std::string("5"), std::string("6") };
        m.add("decimals",      this, &TableWidget::m_decimals,      JPropertyMeta{ .label = "Decimal places", .editor = "enum", .order = 103,
              .choices = decChoices });
        // Breakpoint precision per DATA axis (same indexing as X/Y Axis Scale below, so the pair always refers
        // to the same axis under transpose). "global" = follow the cell's Decimal places, which is what these
        // labels did before they were separable. No Z entry: the depth control renders plane INDICES (1..n),
        // not breakpoints, so there is nothing for a Z precision to format.
        // A raw analog axis is ADC counts in the bytes and stays counts — that is what the firmware reads
        // — but a sender is specified in VOLTS, so calibrating against counts by eye is needless
        // arithmetic. These pick the unit each axis is SHOWN and typed in; entry converts back, and the
        // precision follows the unit (one count is 0.8 mV, so volts need four places to stay distinct).
        m.add("displayUnitX",  this, &TableWidget::m_displayUnitX,  JPropertyMeta{ .label = "X Axis Unit", .editor = "unit", .order = 109 });
        m.add("displayUnitY",  this, &TableWidget::m_displayUnitY,  JPropertyMeta{ .label = "Y Axis Unit", .editor = "unit", .order = 109 });
        m.add("displayUnitZ",  this, &TableWidget::m_displayUnitZ,  JPropertyMeta{ .label = "Z Axis Unit", .editor = "unit", .order = 109 });
        m.add("decimalsX",     this, &TableWidget::m_decimalsX,     JPropertyMeta{ .label = "X Axis Decimals", .editor = "enum", .order = 110, .choices = decChoices });
        m.add("decimalsY",     this, &TableWidget::m_decimalsY,     JPropertyMeta{ .label = "Y Axis Decimals", .editor = "enum", .order = 111, .choices = decChoices });
        m.add("heatmap",       this, &TableWidget::m_heatmap,       JPropertyMeta{ .label = "Heat map", .editor = "enum", .order = 104, .choices = onOff });
        m.add("heatmapScheme", this, &TableWidget::m_heatmapScheme, JPropertyMeta{ .label = "Heat map colours", .editor = "enum", .order = 105,
              .choices = { std::string("global"), std::string("Blue \xE2\x86\x92 Red"), std::string("Green \xE2\x86\x92 Red"), std::string("Grayscale"), std::string("Cool"), std::string("Warm") } });
        // Axis / grid colours: blank follows the JStyle palette (PlaceholderText / Border); a set value overrides.
        m.add("axisColor", this, &TableWidget::m_axisColor, JPropertyMeta{ .label = "Axis Colour", .editor = "color", .inheritable = true, .order = 106 });
        m.add("gridColor", this, &TableWidget::m_gridColor, JPropertyMeta{ .label = "Grid Colour", .editor = "color", .inheritable = true, .order = 107 });
        // ONE scale per data axis. The property IS the scale (not a multiplier): it defaults to — and SHOWS — the
        // schema scale, and whatever it holds is the sole scale applied (raw × scale to display, inverted to edit).
        // Cell scale is "scale" (order 22, replacing the base widget property); each axis has its own scaleX/Y/Z.
        // Blank ⇒ the schema scale. tableGeom does the single application. schemaScale(-1)=cell, 0/1/2 = that axis.
        auto schemaScale = [this](int axis) {
            const TableImage t = Cache::instance().resolveTable(bindPath());
            if (axis < 0) return t.cellScale;
            return axis < int(t.axes.size()) ? t.axes[axis].breakScale : 1.0;
        };
        auto scaleProp = [this, schemaScale](const char* name, int axis, const char* label, int order) {
            return jf::JProperty{ .name = name,
                .get = [this, name, schemaScale, axis] { const std::string s = m_el ? m_el->prop(name) : std::string();
                    if (!s.empty()) { try { return jf::JVariant(std::stod(s)); } catch (...) {} }
                    return jf::JVariant(schemaScale(axis)); },
                .set = [](const jf::JVariant&) { return true; },
                .meta = JPropertyMeta{ .label = label, .step = 0.1, .decimals = 4, .order = order } };
        };
        m.add(scaleProp("scale",  -1, "Scale",        22));
        m.add(scaleProp("scaleX",  0, "X Axis Scale", 106));
        m.add(scaleProp("scaleY",  1, "Y Axis Scale", 107));
        m.add(scaleProp("scaleZ",  2, "Z Axis Scale", 108));
        m.add("cellTrace",     this, &TableWidget::m_cellTrace,     JPropertyMeta{ .label = "Live cell cursor", .editor = "enum", .order = 106, .choices = onOff });
        m.add("depthControl",  this, &TableWidget::m_depthControl,  JPropertyMeta{ .label = "Depth control", .editor = "enum", .order = 107,
              .choices = { std::string("global"), std::string("Slider"), std::string("Tabs"), std::string("Stepper"), std::string("Stack") } });
        m.add("depthPosition", this, &TableWidget::m_depthPosition, JPropertyMeta{ .label = "Depth control position", .editor = "enum", .order = 108,
              .choices = { std::string("global"), std::string("Top"), std::string("Bottom"), std::string("Left (Stack only)"), std::string("Right (Stack only)") } });
        m.add("step",          this, &TableWidget::m_step,          JPropertyMeta{ .label = "Key Step (. / ,)", .order = 109 });
        // Row Source: a sigil that DRIVES this table's selected row. Point it at another table's
        // [@other.selectedRow] and this table follows that selection (its slice/2D cursor lands on the
        // matching row). Empty = the table is driven by its own clicks. Sigil-aware editor (fx button).
        m.add("rowSource",     this, &TableWidget::m_rowSource,     JPropertyMeta{ .label = "Row Source (sigil)", .editor = "expr", .order = 110 });
    }

private:
    int         m_view = 0;              // OWNED runtime state: 0 grid / 1 3D / 2 slice (loadContent/saveContent)
    Table3DState m_view3d;              // OWNED 3D orbit/height/drag state (per instance; not serialized — runtime only)
    std::string m_axisColor, m_gridColor;   // local colour overrides ("" = JStyle palette default)
    int         m_axisMode = 0, m_hSectionMode = 0, m_vSectionMode = 0, m_decimals = 0;
    int         m_cellScale = 0;   // 0 = global (100 %); else an index into the Cell size choices
    int         m_decimalsX = 0, m_decimalsY = 0;   // 0 = global (follow m_decimals); else 1+N == N decimals
    // Per-axis display unit ("" / "Auto" = the axis's own; else a unit id of its quantity, e.g. ADC_V).
    std::string m_displayUnitX, m_displayUnitY, m_displayUnitZ;
    int         m_heatmap = 0, m_heatmapScheme = 0, m_cellTrace = 0, m_depthControl = 0, m_depthPosition = 0;
    std::string m_step = "1";
    std::string m_rowSource;   // sigil that drives the selected row (inspector-bound; render reads el.prop)
};

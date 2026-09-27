#pragma once

// Array1DWidget — a strip of one repeated run of values, with per-row labels and inline editing.
//
// The run is a MetaModel::Location of Kind::Run, so this draws a 1-D array ("ignition.ign_rpm_axis") and a nested
// per-element sub-array ("trigger.streams[0].cell[].v") through the same path. That second shape is why
// the widget exists in this form: a stream's cells are a run that BELONGS to one element, and the only
// other way to show it was one widget per index — which puts the shape of the data into the page, where
// hidden widgets still hold layout slots and the panel grows holes at every configuration but one.
//
// First Row / Row Count are EXPRESSIONS, so the window can be data rather than geometry:
//     Data Source  trigger.streams[0].cell[].v
//     Row Count    [#trigger.streams[0].cell_len]
// draws exactly the live cells and nothing else, and reflows by itself when the count changes. Blank
// means "from 0" and "all of it", which is what every existing strip already did.

#include "../CanvasWidget.h"
#include <j/core/JLineEdit.h>
#include <j/core/Validator.h>
#include <memory>

class Array1DWidget : public CanvasWidget {
public:
    explicit Array1DWidget(jf::JSceneGraph& g) : CanvasWidget(g, "array1d") {}
    std::string elementType() const override { return "array1d"; }
    std::string paletteTitle() const override { return "1D Array"; }
    float       defaultW()     const override { return 160.f; }
    float       defaultH()     const override { return 160.f; }
    bool        interactive()    const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;

    // A strip whose run overflows its box IS a scrolling view, so the wheel is its business — see
    // CanvasWidget::wantsWheel. One that fits declines, and the roll scrolls the page as it always has.
    bool        wantsWheel(const jf::JRect& screen, float mx, float my) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // Per-element view state: which row is current, and where the window is scrolled to. Keyed by element id.
    //
    // `sel` is the row within the WINDOW (0..count-1), not the row on screen: a run longer than the box
    // scrolls, and a selection that could only address what happened to be visible could never reach the
    // rest. `scroll` is the first window row drawn; `barDrag` is set while the thumb is held.
    //
    // There is no edit state here any more. The current cell IS a real jf::JDoubleSpinBox (see _cellEditor),
    // so the caret, the selection, what a keystroke does and what a commit means are the framework's, the
    // same as every other editable field on a page. The strip used to keep its own `editing` flag and typed
    // buffer and paint the row inverted in Accent to say so — a look and a set of rules nothing else in the
    // studio had.
    struct Array1DCursor { int sel = 0; int scroll = 0; bool barDrag = false; float barGrab = 0.f; };
    // Keyed by UID: an element id is unique within ONE model, and these maps are global — two widgets
    // with the same id on different pages would otherwise share this state. See TableWidget::tableCursors.
    static std::unordered_map<std::string, Array1DCursor>& array1dCursors() { static std::unordered_map<std::string, Array1DCursor> m; return m; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");   // shows an array of cells, not one readout
        // A strip is a card like a panel is: it names what it lists. Drawn by the shared chrome, so it is
        // the same bar in the same place with the same frame around it (CanvasWidget::drawTitleBar).
        m.add("labelText", this, &Array1DWidget::m_labelText,
              jf::JPropertyMeta{ .label = "Title", .order = 99 });
        m.add("labelTemplate", this, &Array1DWidget::m_labelTemplate,
              jf::JPropertyMeta{ .label = "Row Label", .def = "%INDEX%", .order = 100 });
        m.add("firstIndex", this, &Array1DWidget::m_firstIndex,
              jf::JPropertyMeta{ .label = "First Row", .editor = "expr", .order = 101 });
        m.add("rowCount", this, &Array1DWidget::m_rowCount,
              jf::JPropertyMeta{ .label = "Row Count", .editor = "expr", .order = 102 });
        // Blank means "the precision the FIELD declares", which is right far more often than any fixed
        // default would be — a cell count wants no decimals and a lambda target wants three, and the meta
        // already says which. Set it only to override that.
        m.add("format", this, &Array1DWidget::m_format,
              jf::JPropertyMeta{ .label = "Number Format", .order = 103 });
    }

private:
    std::string m_labelText;                    // the card's own title (element prop "labelText")
    std::string m_labelTemplate = "%INDEX%";
    std::string m_firstIndex;    // "" = from 0        (element prop "firstIndex")
    std::string m_rowCount;      // "" = to the end    (element prop "rowCount")
    std::string m_format;        // "" = the field's own digits (element prop "format")

    // THE CELL EDITOR — one line edit, moved to whichever row is current. A strip of 32 cells does not want
    // 32 live controls, and it does not need them: only one cell is ever being edited. It carries the
    // cell's own range and precision in a validator, so what you can type is what the byte can hold.
    jf::JLineEdit* _cellEditor();
    // THE ROW SPLIT. How wide the value column has to be, and therefore how much is left for the label.
    // Both used to help themselves — the label to half the row, the value to 55% of it — so they overlapped
    // by the difference, and the label was clipped mid-glyph ("after tooth 1" for cell 10). Sized from what
    // the cell CAN hold rather than from what is on screen, so it does not jitter as the strip scrolls.
    float          _valueColW(const MetaModel::Location& cell, int dec, const std::string& unit) const;
    jf::JRect      _editorRect(const jf::JRect& row, float valueW, float unitW) const;
    void           _loadCell(int row);      // put a cell's text in the editor, selected, ready to replace
    void           _commitCell();           // the text in the editor -> the cell it was loaded from

    std::unique_ptr<jf::JLineEdit>      m_edit;
    // Does the STRIP hold the keyboard? The editor is only drawn while it does — an unfocused list should
    // read as a list, not as a page with one boxed row on it. Set by a press or a Tab-in, cleared by the
    // blur the Surface announces when the focus moves on.
    bool m_focused = false;
    std::unique_ptr<jf::JDoubleValidator> m_valid;
    // Which WINDOW row the editor currently holds text for, or -1. The text belongs to the user from the
    // moment a cell is loaded until it is committed, so the paint must not push the stored value back over
    // a half-typed number — it only loads when the row CHANGES.
    int         m_editRow = -1;
    std::string m_editText;   // what the editor was LOADED with — an untouched cell is never written back
};

// Array1DWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "Array1DWidget.h"
#include "../../ui/WrapText.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"
#include "../../model/MetaModel.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace {
// Where element `index` of a run lives. The painter and the editor both need it, and a run's element is
// a Location like any other — same fields, same bounds — so it reads and writes through the same two
// accessors a scalar does.
MetaModel::Location cellLoc(const CanvasWidget& w, const MetaModel::Location& a, int index) {
    MetaModel::Location L;
    L.kind     = MetaModel::Location::Kind::Scalar;
    L.offset   = a.offset + index * a.stride;
    L.datatype = a.datatype;
    L.scale    = a.scale;
    L.minV     = a.minV;
    L.maxV     = a.maxV;
    w.narrowToControl(L);   // the strip's own Min/Max — a GAP cell index is a byte, a SEQUENCE step an angle
    return L;
}
}  // namespace

namespace {
// The window this strip draws: which row of the run it starts at, and how many. Both are expressions,
// so a stream's live cell count or an axis's live bin count can BE the row count — the shape of the
// data driving the widget instead of the page having to describe it. Blank keeps the old behaviour
// (from 0, all of it), and both are clamped to the run so a stale or wrong expression narrows the
// view rather than reading past the end of the array.
// A cell's value as text. The format prop wins when it carries a float conversion, exactly as the shared
// fmtVal rule has it: a printf string is handed a double here, so "%d" would be undefined behaviour rather
// than a wrong-looking number, and the check is what keeps a typo in a property box from being one.
std::string fmtCell(const std::string& fmt, double eng, int digits) {
    char b[48];
    const bool usable = fmt.find('f') != std::string::npos || fmt.find('g') != std::string::npos
                     || fmt.find('e') != std::string::npos;
    if (usable) std::snprintf(b, sizeof(b), fmt.c_str(), eng);
    else        std::snprintf(b, sizeof(b), "%.*f", digits, eng);
    return b;
}

// How many decimals this strip shows. The Number Format prop wins when it states a precision, because
// that is what the ROWS are drawn with; otherwise the field's own digits. The editor and the rows have to
// agree — a box reading 9.00 under rows reading 9 is one value written two ways, and the difference looks
// like the edit changed something.
int decimalsOf(const PanelElement& el, int fieldDigits) {
    const std::string f = el.prop("format");
    if (const size_t dot = f.find('.'); dot != std::string::npos) {
        int d = 0; bool any = false;
        for (size_t i = dot + 1; i < f.size() && f[i] >= '0' && f[i] <= '9'; ++i) { d = d * 10 + (f[i] - '0'); any = true; }
        if (any) return d;
    }
    return fieldDigits >= 0 ? fieldDigits : 2;
}

struct Window { int first, count; };
Window windowOf(const PanelElement& el, int total) {
    auto eval = [&](const char* prop, int fallback) {
        const std::string e = el.prop(prop);
        if (e.empty()) return fallback;
        return static_cast<int>(std::lround(MathEvaluator::instance().evaluate(e)));
    };
    int first = std::clamp(eval("firstIndex", 0), 0, total);
    int count = std::clamp(eval("rowCount", total - first), 0, total - first);
    return { first, count };
}

// How the strip lays out in a given box: how many rows fit, whether the run overflows them, and what the
// scroll bar costs in width. The paint, the input handler and the wheel question all have to agree on this
// down to the row — a paint that shows 15 rows while the input believes 16 puts the selection somewhere the
// user cannot see, and a click lands on the wrong cell. One answer, computed once.
struct Layout {
    Window w;
    int   fit = 0, maxScroll = 0;
    bool  scrolls = false;
    float rowH = 0.f, pad = 6.f, barW = 0.f, bodyW = 0.f;
};
Layout layoutOf(const PanelElement& el, int total, const jf::JRect& r) {
    Layout L;
    L.rowH = jf::JTextHelper::lineHeight() + 4.f;
    L.w    = windowOf(el, total);
    L.fit  = std::max(0, static_cast<int>((r.height - 2.f * L.pad) / L.rowH));
    L.maxScroll = std::max(0, L.w.count - L.fit);
    L.scrolls   = L.maxScroll > 0 && L.fit > 0;
    L.barW      = L.scrolls ? 6.f : 0.f;
    L.bodyW     = r.width - L.barW;
    return L;
}
}  // namespace

// How much room the value column needs: the WIDEST number this cell could hold, in the units it is shown
// in, plus its unit. Taking it from the range rather than from the values on screen keeps the column still
// while you scroll, and keeps the label's share the same on every row.
float Array1DWidget::_valueColW(const MetaModel::Location& cell, int dec, const std::string& unit) const {
    if (!jf::JTextHelper::hasAtlas() || !m_el) return 60.f;
    double lo = 0.0, hi = 0.0;
    Cache::rawBounds(cell, lo, hi);
    double dlo = dispV(*m_el, Raw{lo}), dhi = dispV(*m_el, Raw{hi});
    if (dlo > dhi) std::swap(dlo, dhi);
    char b[64];
    // The longer of the two ends: -127 is wider than 128, and a negative minimum is common (a sentinel).
    std::snprintf(b, sizeof(b), "%.*f", dec, dlo);
    float w = jf::JTextHelper::measureWidth(b);
    std::snprintf(b, sizeof(b), "%.*f", dec, dhi);
    w = std::max(w, jf::JTextHelper::measureWidth(b));
    if (!unit.empty()) w += jf::JTextHelper::measureWidth(" " + unit);
    return w + 16.f;   // the field's own padding, and room for the caret at either end
}

// The value column of a row, less whatever the unit takes: the row still reads "45 deg" while you edit,
// with only the number inside the box.
jf::JRect Array1DWidget::_editorRect(const jf::JRect& row, float valueW, float unitW) const {
    const float w = std::max(32.f, valueW - unitW);
    return { row.x + row.width - w - unitW, row.y, w, row.height };
}

// The cell editor. Constructed once and reused: it is whichever cell is current, not a control per row.
jf::JLineEdit* Array1DWidget::_cellEditor() {
    if (!m_edit) {
        m_edit  = std::make_unique<jf::JLineEdit>(sceneGraph(), "");
        // The rows are right-aligned, so the editor is too — otherwise the number jumps to the other side
        // of the column the moment you click it, which reads as the value having changed.
        m_edit->setAlignment(jf::JLineEdit::AlignRight);
        m_valid = std::make_unique<jf::JDoubleValidator>(-1e12, 1e12, 2);
        m_edit->setValidator(m_valid.get());
        // Return commits where you are and stays: the row step is the strip's, in handleControlInput, so
        // Return does the same thing here as it does in a table.
        m_edit->onReturnPressed.connect([this] { _commitCell(); });
    }
    // ALWAYS NoFocus, and parented here — the same contract HostedControlWidget documents: the canvas is its
    // own focus domain, so a hosted control that joined the window's focus chain would lose its focus (and
    // its caret) on the very next frame.
    if (m_edit->parentWidget() != this) addChild(m_edit.get());
    m_edit->setFocusPolicy(jf::JFocusPolicy::NoFocus);
    return m_edit.get();
}

// Put a cell's value in the editor, selected — so typing replaces it, as landing in any field does.
void Array1DWidget::_loadCell(int row) {
    const MetaModel* m = Cache::instance().meta();
    const std::string bind = bindPath();
    if (!m || !m_el || bind.empty()) return;
    const MetaModel::Location a = m->locate(bind);
    if (a.kind != MetaModel::Location::Kind::Run) return;
    const Window w = windowOf(*m_el, a.count);
    if (row < 0 || row >= w.count) return;
    const MetaModel::Location cell = cellLoc(*this, a, w.first + row);
    const int dec = decimalsOf(*m_el, a.digits);
    jf::JLineEdit* ed = _cellEditor();
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", dec, dispV(*m_el, Raw{Cache::instance().readRaw(cell)}));
    ed->setText(b);
    ed->selectAll();
    m_editRow  = row;
    m_editText = b;
}

// The text in the editor -> the cell it was loaded from. The validator has already fixed the text up to
// the cell's own range on commit, and writeRaw clamps against the same Location, so the number you are
// left looking at is the number in the tune.
void Array1DWidget::_commitCell() {
    if (m_editRow < 0 || !m_edit || !m_el) return;
    // AN UNTOUCHED CELL IS NEVER WRITTEN. Leaving a cell commits it, and re-writing the same number looks
    // harmless — until the control's Min/Max is narrower than what the tune holds, which is a case this
    // page has (a GAP cell index caps at the anomaly ceiling while the stored run is longer). Then merely
    // SELECTING a cell would silently clamp it, and the out-of-range value the strip is meant to show you
    // would be destroyed by looking at it.
    if (m_edit->text() == m_editText) return;
    const MetaModel* m = Cache::instance().meta();
    const std::string bind = bindPath();
    if (!m || bind.empty()) return;
    const MetaModel::Location a = m->locate(bind);
    if (a.kind != MetaModel::Location::Kind::Run) return;
    const Window w = windowOf(*m_el, a.count);
    if (m_editRow >= w.count) return;
    m_edit->commit();                       // validator fixup: clamps and re-formats the text
    try {
        // Typed in DISPLAY units, stored in the field's — srcV is the inverse of the dispV the row was
        // drawn through, so what you type back is what you saw.
        Cache::instance().writeRaw(cellLoc(*this, a, w.first + m_editRow),
                                   srcV(*m_el, std::stod(m_edit->text())), bind);
    } catch (...) {}
}

void Array1DWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& box, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    // The title bar, and then everything below it: the same chrome a panel draws, from the same place, so
    // a strip that names what it lists looks like the card it is rather than like a strip with a caption
    // floating above it. The frame is the base's drawBorder — the same paintBorder a panel's frame is.
    const float top = drawTitleBar(buf, box);
    const jf::JRect r{ box.x, top, box.width, box.height - (top - box.y) };
    uint8_t fgb[4], lcb[4]; const uint8_t* fg = fgOf(el, fgb);
    const std::string bind = bindPath();   // "[*]" resolves against the host viewport's sensor
    const MetaModel* m = c.meta();
    const float lh = jf::JTextHelper::lineHeight();
    auto placeholder = [&] {
        if (jf::JTextHelper::hasAtlas())
            wraptext::drawCentred(buf, r.x + 4.f, r.y + (r.height - wraptext::height("Drop a 1D array here", r.width - 8.f)) * 0.5f,
                                  r.width - 8.f, "Drop a 1D array here", jf::Colors::TextSecondary);   // wraps in a narrow box
    };
    if (!m || bind.empty()) { placeholder(); return; }
    const MetaModel::Location a = m->locate(bind);
    if (a.kind != MetaModel::Location::Kind::Run) { placeholder(); return; }
    const int dec = decimalsOf(el, a.digits);
    std::string tmpl = el.prop("labelTemplate"); if (tmpl.empty()) tmpl = "%INDEX%";
    // A run longer than the box SCROLLS, and the bar takes a gutter off the right — so the layout is asked
    // for once here and every row, label and value lays out inside what it leaves.
    const Layout L = layoutOf(el, a.count, r);
    const Window w = L.w;
    const float rowH = L.rowH, pad = L.pad, barW = L.barW, bodyW = L.bodyW;
    const int fit = L.fit, maxScroll = L.maxScroll;
    const bool scrolls = L.scrolls;
    const uint8_t* lc = elColor(el, "labelColor", jf::Colors::TextSecondary, lcb);
    const Array1DCursor* cur = nullptr;
    { const auto ci = array1dCursors().find(el.uid); if (ci != array1dCursors().end()) cur = &ci->second; }
    const int scroll = cur ? std::clamp(cur->scroll, 0, maxScroll) : 0;
    const int n = std::min(w.count - scroll, fit);
    // The row's split, once, for every row: the value column takes what a value needs and the label takes
    // the rest. Both used to help themselves to a fraction of the row, which overlapped by the difference.
    const std::string du = displayUnitLabelOf(el);
    const std::string unit = !du.empty() ? du : a.units;
    const std::string suffix = unit.empty() ? std::string() : " " + unit;
    const float unitW  = (suffix.empty() || !jf::JTextHelper::hasAtlas())
                       ? 0.f : jf::JTextHelper::measureWidth(suffix) + 4.f;
    const float valueW = std::min(_valueColW(cellLoc(*this, a, w.first), dec, unit), (bodyW - 2.f * pad) * 0.7f);
    const float labelW = std::max(0.f, bodyW - 2.f * pad - valueW - 6.f);   // 6 = the gap between them
    const int selRow = cur ? cur->sel : -1;
    bool editorShown = false;
    for (int i = 0; i < n; ++i) {
        const float ry = r.y + pad + i * rowH;
        const int row = scroll + i;
        // The current row is marked the way every other value in the studio is: a wash behind it, and the
        // editor's own box and focus ring on the value. It used to be filled in Accent with the text
        // inverted — a look nothing else here has, which is why it read as a mode rather than a selection.
        if (row == selRow)
            buf.pushRectangle(r.x + 2.f, ry - 2.f, bodyW - 4.f, rowH, jf::Colors::Surface2, 2.f);
        // The label numbers the row's place in the WHOLE run, not in the window — a strip showing a
        // stream's live cells has to say cell 5 is cell 5. %INDEX% stays 1-based (every existing strip
        // starts at row 0, where it is unchanged); %INDEX0% is the 0-based index, which is what an
        // array subscript actually is.
        std::string lbl = tmpl;
        for (size_t p; (p = lbl.find("%INDEX0%")) != std::string::npos; ) lbl.replace(p, 8, std::to_string(w.first + row));
        for (size_t p; (p = lbl.find("%INDEX%"))  != std::string::npos; ) lbl.replace(p, 7, std::to_string(w.first + row + 1));
        // RAW out of the cache; dispV() cooks the scale and converts the unit. The widget used to read
        // ENGINEERING here and hand it to dispV as well, which was only harmless because
        // configScale() did not know 1-D arrays existed and returned 1.0 — two errors cancelling.
        const MetaModel::Location cell = cellLoc(*this, a, w.first + row);
        const Raw raw = c.readRaw(cell);
        // Through the widget's own Scale / Display Unit, like every other readout. A strip offered both
        // properties and honoured neither, which matters here more than elsewhere: a run's meaning can
        // depend on what its element is configured AS — a trigger stream's cells are tooth indices under
        // one primitive and 0.1° steps under another — so the page, which knows that, is the only place
        // that can say. The field cannot: the meta has one units string, and it would be wrong half the time.
        if (jf::JTextHelper::hasAtlas())
            jf::JTextHelper::pushText(buf, r.x + pad, ry, lbl, lc, labelW);
        if (row == selRow && m_focused) {
            // THE CURRENT CELL IS THE LINE EDIT. Everything about editing it — the caret, the selection,
            // what a digit does, what a commit writes — is the control's, which is the whole point: a cell
            // here behaves like a value anywhere else on the page.
            jf::JLineEdit* ed = _cellEditor();
            const jf::JRect er = _editorRect({ r.x + pad, ry - 2.f, bodyW - 2.f * pad, rowH }, valueW, unitW);
            // What the cell can hold, in the units it is shown in — so the field refuses a keystroke that
            // could not be stored rather than accepting it and clamping behind your back.
            double lo = 0.0, hi = 0.0;
            Cache::rawBounds(cell, lo, hi);
            double dlo = dispV(el, Raw{lo}), dhi = dispV(el, Raw{hi});
            if (dlo > dhi) std::swap(dlo, dhi);
            if (m_valid) { m_valid->setRange(dlo, dhi); m_valid->setDecimals(dec); }
            // The text is the USER'S from the moment a cell is loaded until it is committed, so it is only
            // (re)loaded when the row changes. Pushing the stored value in every frame would wipe the caret
            // and the half-typed number under their hands.
            if (m_editRow != row) _loadCell(row);
            ed->setBounds(er);
            ed->populateRenderPrimitives(buf);
            if (!suffix.empty() && jf::JTextHelper::hasAtlas())
                jf::JTextHelper::pushText(buf, er.x + er.width + 4.f, ry, unit, fg, unitW);
            editorShown = true;
        } else if (jf::JTextHelper::hasAtlas()) {   // …otherwise the row is just its value, like the rest
            std::string vs = fmtCell(el.prop("format"), dispV(el, Raw{raw}), dec);
            if (!unit.empty()) vs += " " + unit;
            jf::JTextHelper::pushText(buf, r.x + bodyW - pad - jf::JTextHelper::measureWidth(vs), ry, vs, fg, bodyW * 0.5f);
        }
    }
    // Scrolled out of view: park the editor so the framework's hover scan does not keep finding it where it
    // was last drawn (the same reason HostedControlWidget insists on a real parent edge).
    if (!editorShown && m_edit) m_edit->setBounds({ r.x, r.y, 0.f, 0.f });
    // The bar, and only when there is something to scroll — a run that fits keeps its full width. It is
    // also what tells you the run continues at all: a strip showing 15 of a stream's 20 cells otherwise
    // looks exactly like a strip showing all 15, which is how you tune the wrong wheel.
    if (scrolls) {
        const float tx = r.x + bodyW, ty = r.y + pad, th = r.height - 2.f * pad;
        static const uint8_t track[4] = { 30, 30, 34, 200 };
        buf.pushRectangle(tx, ty, barW, th, track, 3.f);
        const float thumbH = std::max(18.f, th * (static_cast<float>(fit) / static_cast<float>(w.count)));
        const float t = maxScroll > 0 ? static_cast<float>(scroll) / static_cast<float>(maxScroll) : 0.f;
        buf.pushRectangle(tx + 1.f, ty + t * (th - thumbH), barW - 2.f, thumbH, jf::Colors::TextSecondary, 3.f);
    }
}

bool Array1DWidget::wantsWheel(const jf::JRect& screen, float, float) {
    const MetaModel* m = m_cache ? m_cache->meta() : Cache::instance().meta();
    const std::string bind = bindPath();
    if (!m || !m_el || bind.empty()) return false;
    const MetaModel::Location a = m->locate(bind);
    if (a.kind != MetaModel::Location::Kind::Run) return false;
    const jf::JRect content = contentRect(screen);
    const float top = content.y + titleBarH();
    return layoutOf(*m_el, a.count,
                    { content.x, top, content.width, content.height - (top - content.y) }).scrolls;
}

bool Array1DWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    // THE SAME RECT THE PAINT USED. populateRenderPrimitives hands render() contentRect(bounds()) while
    // every delivery site hands input the full rect, so a padded strip hit-tested against a box bigger than
    // the one it drew: rows were off by the padding, and the scroll bar — a 6px gutter measured from the
    // right edge — was tested where it was not drawn, which is why dragging the thumb did nothing at all.
    const jf::JRect content = contentRect(screen);
    // …and the input starts below the title too, or every row is off by the height of the bar.
    const float top = content.y + titleBarH();
    const jf::JRect r{ content.x, top, content.width, content.height - (top - content.y) };
    const PanelElement& el = *m_el;
    const std::string bind = bindPath();   // "[*]" resolves against the host viewport's sensor
    const MetaModel* m = Cache::instance().meta();
    if (!m || bind.empty()) return false;
    const MetaModel::Location a = m->locate(bind);
    if (a.kind != MetaModel::Location::Kind::Run) return false;
    const Layout L = layoutOf(el, a.count, r);
    const Window w = L.w;
    const float rowH = L.rowH, pad = L.pad, barW = L.barW;
    const int fit = L.fit, maxScroll = L.maxScroll;
    const bool scrolls = L.scrolls;
    const int n = std::min(w.count, fit);
    if (n <= 0) return false;
    Array1DCursor& cur = array1dCursors()[el.uid];
    // The selection addresses the WHOLE window, not the rows that happen to be on screen — clamping it to
    // the visible ones is what made a 20-cell run stop at cell 15 with no way to reach the rest.
    cur.sel    = std::clamp(cur.sel, 0, w.count - 1);
    cur.scroll = std::clamp(cur.scroll, 0, maxScroll);
    // Whatever moved — a key, a click, the wheel — the selected row ends up on screen. Kept in one place
    // so no path can move the cursor out of sight and leave the strip looking like nothing happened.
    auto follow = [&] {
        cur.scroll = std::clamp(cur.scroll, std::max(0, cur.sel - fit + 1), std::min(cur.sel, maxScroll));
    };
    const float trackX = r.x + r.width - barW, trackY = r.y + pad, trackH = r.height - 2.f * pad;
    const float thumbH = scrolls ? std::max(18.f, trackH * (static_cast<float>(fit) / w.count)) : trackH;
    // Where the thumb's TOP must be for the given scroll — the inverse is how a drag reads back.
    auto scrollFromThumbTop = [&](float top) {
        const float span = trackH - thumbH;
        return span <= 0.f ? 0 : static_cast<int>(std::lround((top - trackY) / span * maxScroll));
    };
    // Where the editor sits for a given WINDOW row, in the geometry the paint used. Input has to be able to
    // place it before the next paint does: a click on a row the editor is not on yet must reach the box at
    // the place the user clicked, not where it was drawn a frame ago.
    auto editorRectFor = [&](int row) {
        const float ry = r.y + pad + (row - cur.scroll) * rowH;
        const float bodyW = r.width - barW;
        const std::string du = displayUnitLabelOf(el);
        const std::string unit = !du.empty() ? du : a.units;
        const float unitW = (unit.empty() || !jf::JTextHelper::hasAtlas())
                          ? 0.f : jf::JTextHelper::measureWidth(" " + unit) + 4.f;
        const int dec = decimalsOf(el, a.digits);
        const float valueW = std::min(_valueColW(cellLoc(*this, a, w.first), dec, unit), (bodyW - 2.f * pad) * 0.7f);
        return _editorRect({ r.x + pad, ry - 2.f, bodyW - 2.f * pad, rowH }, valueW, unitW);
    };
    // Moving off a cell commits it, exactly as clicking out of any other field does.
    auto leaveCell = [&] { if (m_editRow >= 0) { _commitCell(); m_editRow = -1; } };
    if (in.kind == ControlInput::Kind::Press) {
        if (scrolls && in.mx >= trackX) {
            const float top = trackY + (maxScroll ? static_cast<float>(cur.scroll) / maxScroll : 0.f) * (trackH - thumbH);
            if (in.my >= top && in.my < top + thumbH) { cur.barDrag = true; cur.barGrab = in.my - top; }
            else cur.scroll = std::clamp(scrollFromThumbTop(in.my - thumbH * 0.5f), 0, maxScroll);
            return true;
        }
        const int vis = static_cast<int>((in.my - (r.y + pad)) / rowH);
        if (vis < 0 || vis >= n) return false;
        leaveCell();
        cur.sel = cur.scroll + vis;
        // Hand the press to the editor on the row that was clicked. Clicking the number puts the caret where
        // you clicked and selects as you drag, like any other field; clicking the label selects the whole
        // value, so typing replaces it. Placed and loaded first — the box is not on this row until the next
        // paint, and a click must reach the text where the user sees it.
        jf::JLineEdit* ed = _cellEditor();
        m_focused = true;
        ed->setBounds(editorRectFor(cur.sel));
        _loadCell(cur.sel);
        ed->setFocused(true);
        if (in.mx >= ed->bounds().x) ed->handleMousePress(in.mx, in.my);
        return true;
    }
    if (in.kind == ControlInput::Kind::Move) {
        if (cur.barDrag) { cur.scroll = std::clamp(scrollFromThumbTop(in.my - cur.barGrab), 0, maxScroll); return true; }
        if (m_edit) { m_edit->handleMouseMove(in.mx, in.my); return true; }   // drag-select inside the number
    }
    if (in.kind == ControlInput::Kind::Release) {
        const bool had = cur.barDrag; cur.barDrag = false;
        if (had) return true;
        if (m_edit) { m_edit->handleMouseRelease(in.mx, in.my); return true; }
    }
    // Focus arrived without a click (Tab): the current cell's value, selected, ready to be replaced — which
    // is what tabbing into any other field does.
    if (in.kind == ControlInput::Kind::Focus) {
        jf::JLineEdit* ed = _cellEditor();
        m_focused = true;
        ed->setBounds(editorRectFor(cur.sel));
        _loadCell(cur.sel);
        ed->setFocused(true);
        return true;
    }
    // Focus left: commit and drop the caret. Nothing else will tell the box — a hosted control is not in the
    // window's focus chain, so no real blur ever reaches it.
    if (in.kind == ControlInput::Kind::Blur) {
        // Commit, drop the caret, and STOP DRAWING THE BOX. A line edit that keeps its frame after the
        // keyboard has gone leaves one row of the strip outlined for good, which reads as an edit still in
        // progress somewhere the user is no longer looking.
        leaveCell();
        m_focused = false;
        if (m_edit) { m_edit->setFocused(false); m_edit->setBounds({ 0.f, 0.f, 0.f, 0.f }); }
        return true;
    }
    // The wheel scrolls the view and leaves the selection alone — the row you were editing does not move
    // because you looked further down the run.
    if (in.kind == ControlInput::Kind::Scroll) {
        if (!scrolls) return false;
        cur.scroll = std::clamp(cur.scroll - static_cast<int>(in.wheel > 0 ? 3 : -3), 0, maxScroll);
        return true;
    }
    if (in.kind == ControlInput::Kind::Key && in.key && in.key->pressed) {
        using K = jf::JKeyEvent::JKey;
        const K k = in.key->key;
        jf::JLineEdit* ed = _cellEditor();
        // ARROWS MOVE THE ROW; everything else belongs to the cell. A strip is a list of values, and a list
        // walks with the arrows — the same rule TableWidget applies to a grid of cells. Moving commits
        // first, exactly as clicking away would, and lands in the next cell with its value selected.
        if (k == K::Up || k == K::Down || k == K::Return) {
            _commitCell();
            cur.sel = (k == K::Up) ? std::max(0, cur.sel - 1) : std::min(w.count - 1, cur.sel + 1);
            follow();
            ed->setBounds(editorRectFor(cur.sel));
            _loadCell(cur.sel);
            ed->setFocused(true);
            return true;
        }
        // Escape puts back what the cell holds — the edit is abandoned, not committed.
        if (k == K::Escape) { _loadCell(cur.sel); return true; }
        return ed->handleKeyEvent(*in.key);   // digits, caret, selection, Backspace, clipboard
    }
    return false;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("array1d", Array1DWidget, 150);

#pragma once

// LabelWidget — a static text caption on the surface. The first widget migrated from a descriptor render
// LAMBDA to a real CanvasWidget subclass .
// Fixed 1:1 px text (canvas-zoom scales it, widget-resize does not), an optional "ghosting"
// LCD backing, and L/C/R alignment. It owns its typed state and declares its editable properties via
// collectProperties() — the JFramework-native Q_PROPERTY the Properties dock introspects. A label paints no
// frame of its own: it relies on the base drawBackground/drawBorder (transparent + no border unless the user
// sets bgColor/borderWidth), matching CanvasElement's paintEvent.

#include "../CanvasWidget.h"   // base class + skin::/fontSpecPx/PanelElement + JTextHelper
#include "../PanelLibrary.h"        // hyperlink::navigator() — a Link makes a caption navigate
#include <j/core/JTextEditCore.h>   // the shared text-editing model behind the inline caption editor

// A LABEL IS A CAPTION, AND A CAPTION WITH A LINK IS A HYPERLINK. There was a separate Hyperlink widget
// that subclassed this one to add a Link; the split cost more than it carried. Every fix to a caption had
// to be applied to a type name in two lists, and the double-click that edits a caption was one of them —
// the Hyperlink was the one caption on a page you could not type into. There is one widget now: set a
// Link and it navigates and underlines on hover; leave it blank and it is a plain caption.

class LabelWidget : public CanvasWidget {
public:
    explicit LabelWidget(jf::JSceneGraph& g) : CanvasWidget(g, "label") {}
    bool editsCaption() const override { return true; }   // …and so does every widget built on one
    std::string elementType()      const override { return "label"; }
    std::string paletteTitle()     const override { return "Label"; }
    // Interactive only when it IS a link — a plain caption must stay click-through, or it swallows presses
    // meant for whatever is behind it.
    bool        interactive()      const override { return !m_link.empty(); }


    const std::string& link() const { return m_link; }
    void setLink(const std::string& l) { if (m_link == l) return; m_link = l; emitModified(); }
    float       defaultW()         const override { return 140.f; }
    float       defaultH()         const override { return 32.f; }
    bool        isLiveValueWidget() const override { return false; }   // static caption, not a telemetry poll

    // ------------------------------------------------------------------
    // Owned typed state. "" / invalid on a colour or font = inherit the scheme. render() reads THESE,
    // never the element prop bag.
    // ------------------------------------------------------------------
    const std::string& text()       const { return m_text; }
    const std::string& unitOf()     const { return m_unitOf; }
    void setUnitOf(const std::string& u) { if (m_unitOf == u) return; m_unitOf = u; emitModified(); }
    void setText(const std::string& t)       { if (m_text == t) return;       m_text = t;       emitModified(); }
    // No own text colour: the label paints with the shared skin fgColor (the base's "Text" picker), so
    // there is ONE colour row for the caption instead of two that silently competed.
    // No own font either: the caption renders from the shared skin fontName (the base's "Font" picker),
    // which is what FieldWidget / ValueWidget / ViewportWidget already use. A private "font" prop meant the
    // base row silently did nothing on a label.
    bool bold()                     const { return m_bold; }           // persisted; single-atlas engine limitation
    void setBold(bool b)                     { if (m_bold == b) return;       m_bold = b;       emitModified(); }
    double rotation()               const { return m_rotation; }       // persisted; axis-aligned engine limitation
    void setRotation(double d)               { if (m_rotation == d) return;   m_rotation = d;   emitModified(); }
    bool ghosting()                 const { return m_ghosting; }
    void setGhosting(bool g)                 { if (m_ghosting == g) return;   m_ghosting = g;   emitModified(); }
    const std::string& ghostColor() const { return m_ghostColor; }
    void setGhostColor(const std::string& c) { if (m_ghostColor == c) return; m_ghostColor = c; emitModified(); }
    bool wrap()                     const { return m_wrap; }           // break the caption to the box width
    void setWrap(bool w)                     { if (m_wrap == w) return;      m_wrap = w;       emitModified(); }
    const std::string& align()      const { return m_align; }          // "Left" | "Center" | "Right"
    void setAlign(const std::string& a)      { if (m_align == a) return;      m_align = a;      emitModified(); }
    // Enabled — a sigil-aware expression (blank = always enabled). Stored in the shared "enableCondition"
    // element prop, so the Surface's run-mode greying (evaluate == 0 → dimmed + inert) applies with no extra
    // render code. A label with no input just shows dimmed, which reads as "disabled/faded".
    const std::string& enabledExpr() const { return m_enabled; }
    void setEnabledExpr(const std::string& e) { if (m_enabled == e) return; m_enabled = e; emitModified(); }
    // Visible — a sigil-aware expression (blank = always visible). Stored in the shared "condition" element
    // prop, which the surface (and the viewport mirror) honour by HIDING the widget when it evaluates 0 —
    // but only in run mode, so a conditionally-hidden widget can still be selected and laid out in the editor.
    const std::string& visibleExpr() const { return m_visible; }
    void setVisibleExpr(const std::string& e) { if (m_visible == e) return; m_visible = e; emitModified(); }

    // ------------------------------------------------------------------
    // In-place caption editing (edit mode). Edit-mode input never reaches a widget — onControlInput is the
    // RUN-mode path — so the Surface owns the double-click trigger and the keyboard, and parks the
    // in-progress string here for render() to draw live with a caret. Keyed by element id, mirroring
    // TableWidget's cursor map.
    // ------------------------------------------------------------------
    // caret/anchor are BYTE offsets into buf (caret == anchor => no selection). tx/ty/scale/lineH are
    // published by render() each frame so the Surface can hit-test a click to a caret index without
    // duplicating the caption layout maths — the same "one geometry, shared by draw and input" contract
    // TableWidget uses.
    // The caption's text/caret/selection live in the shared JTextEditCore — the SAME editor JLineEdit and
    // the tree rename use — so caption editing has identical keys/word-nav/clipboard/select behaviour. Only
    // the drawing is bespoke (in-canvas, scaled/rotated). tx/ty/scale/lineH are published by render() each
    // frame so the Surface can hit-test a click to a caret index without duplicating the caption layout maths.
    struct LabelEdit {
        jf::JTextEditCore core;
        bool  active = false;
        float tx = 0.f, ty = 0.f, scale = 1.f, lineH = 0.f;
    };
    // Keyed by UID, not element id. An id is unique within ONE model and this map is global, so two
    // labels with the same id on different pages shared one open caption. See TableWidget::tableCursors.
    static std::unordered_map<std::string, LabelEdit>& labelEdits() { static std::unordered_map<std::string, LabelEdit> m; return m; }

    // Byte index whose glyph edge sits nearest screen x — click-to-place-caret and drag-select. Delegates the
    // scan to the core with a scaled measure (text-local x = screen x minus the caption origin).
    static int caretAtX(const LabelEdit& e, float x) {
        if (e.scale <= 0.f) return static_cast<int>(e.core.text().size());
        return static_cast<int>(e.core.caretAtX(x - e.tx, [&](size_t end) {
            return jf::JTextHelper::measureWidthScaled(e.core.text().substr(0, end), e.scale);
        }));
    }

    // The Bold checkbox is an OVERRIDE on top of the font spec's own bold field: force field 2 of
    // "family|size|b|i" to "1" so fontSpecFace resolves a REAL bold face through jResolveFontFace, rather
    // than faking weight by smearing the glyphs. Safe on an empty/short spec — the missing fields come back
    // as "", and fontSpecPx falls through to its default size.
    static std::string boldedSpec(const std::string& spec, bool bold) {
        if (!bold) return spec;
        std::string f[4];
        size_t p = 0;
        for (int n = 0; n < 4; ++n) {
            const size_t q = spec.find('|', p);
            f[n] = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        f[2] = "1";
        return f[0] + "|" + f[1] + "|" + f[2] + "|" + f[3];
    }

    // Rotated text. JTextHelper lays glyphs out axis-aligned only, so build the text call into a SCRATCH
    // buffer, then copy its vertices through a rotation about the widget centre and push that. This goes
    // entirely through the public primitive API — no duplicated glyph layout, no drift if the SDK's
    // shaping changes. degrees is clockwise-positive to match the inspector's Rotation field.
    static void pushTextRotated(jf::JPrimitiveBuffer& buf, float x, float y, const std::string& text,
                                const uint8_t color[4], float scale, const std::string& face,
                                float cx, float cy, double degrees) {
        jf::JPrimitiveBuffer scratch;
        jf::JTextHelper::pushTextScaled(scratch, x, y, text, color, scale, 0.f, face);
        const float rad = static_cast<float>(degrees * 3.14159265358979323846 / 180.0);
        const float cs = std::cos(rad), sn = std::sin(rad);
        for (const auto& cmd : scratch.getCommands()) {
            if (cmd.kind != jf::JPrimitiveBuffer::JDrawCommand::JKind::Text) continue;
            jf::JPrimitiveBuffer::JTextCall call = cmd.text;
            for (auto& v : call.verts) {
                const float dx = v.x - cx, dy = v.y - cy;
                v.x = cx + dx * cs - dy * sn;
                v.y = cy + dx * sn + dy * cs;
            }
            buf.pushTextCall(std::move(call));
        }
    }

    // A VALUE RULE CAN SAY THE WORDS. A rule already decides the colours; letting it decide the caption
    // too makes a label with two rules a lamp — "FAULT" in red on one condition, "READY" in green on the
    // other — with no second widget that draws text a second way.
    //
    // Four things can want to be the caption, so the precedence is stated once, here, rather than inside
    // a paint where it can only be read by reading the paint:
    //   typing   — a caption being edited must show what is being typed, empty included
    //   the band — the rule that holds now, which is the whole point of banding the text
    //   the unit — a caption pinned to another widget's display unit
    //   its own  — and failing all of that, the words the label was given ("Label" if it has none)
    // `stated` — the document actually carries a labelText prop, empty or not. A caption the document
    // STATES as empty is empty: an imported spacer is a row of deliberate nothing, and printing the
    // word "Label" into the gap its author asked for is worse than leaving the gap out. The placeholder
    // belongs only to a label that has never been given a caption at all, which is a widget somebody
    // just dropped on a page and has yet to name.
    static std::string chooseCaption(bool editing, const std::string& typed, const std::string& bandText,
                                     bool unitCaption, const std::string& unitCap, const std::string& own,
                                     bool stated = false) {
        if (editing)            return typed;
        if (!bandText.empty())  return bandText;
        if (unitCaption)        return unitCap;      // may be empty: Raw has no unit to print
        if (!own.empty())       return own;
        return stated ? std::string() : std::string("Label");
    }

    // The canvas-unit box that exactly fits `text` — the "resize to the text" measurement, and the single
    // source of truth for it. Mirrors render()'s geometry at zoom 1 (scale = fontPx / lineHeight); the
    // caption is inset by render's 0.025-per-side pad, so the box is the measured width over that 0.95 fit
    // margin. A couple of px of slack keeps the last glyph off the edge.
    // Edge inset for Left/Right alignment, in CANVAS units. Deliberately a constant, not a fraction of the
    // widget width: a proportional inset indents a wide label further than a narrow one, so a column of
    // left-aligned captions of differing length comes out ragged. Overridden per element by the base
    // "Padding" skin property.
    static constexpr float kTextPad = 3.f;

    // rotationDeg expands the result to the ROTATED text's axis-aligned bounding box, because content is
    // clipped to the widget's (unrotated) rect — a box sized for horizontal text would guillotine a label
    // turned toward 90 degrees. At 0 degrees this is exactly the old measurement.
    // Split on '\n'. A caption is one line far more often than not, so this returns a single-element
    // vector for the common case rather than making every caller special-case it.
    // A laid-out line: its text and the BYTE OFFSET it starts at in the caption. The offset is carried
    // rather than derived because what separates two lines varies — an authored '\n' and the space a wrap
    // eats are one byte, a break inside a long word is none — and the caret/selection maths reads it.
    struct Line { std::string text; std::size_t start = 0; };

    // Hard lines first (an authored '\n' always breaks), then as many soft ones as the width needs.
    // Greedy by WORD; a single word wider than the box is broken by character, because the alternative is
    // a caption that runs outside its own widget — which is the thing wrapping exists to stop. An
    // unmeasurable box (avail <= 0) wraps nothing rather than emitting zero-width lines.
    static std::vector<Line> layoutLines(const std::string& t, bool wrap, float scale, float avail) {
        const auto fits = [&](const std::string& s) {
            return jf::JTextHelper::measureWidthScaled(s, scale) <= avail;
        };
        std::vector<Line> out;
        std::size_t para = 0;
        for (;;) {
            const std::size_t nl = t.find('\n', para);
            const std::string p  = t.substr(para, nl == std::string::npos ? nl : nl - para);
            if (!wrap || avail <= 0.f || p.empty() || fits(p)) {
                out.push_back({ p, para });                   // an empty paragraph still owns a row
            } else {
                std::size_t i = 0;
                while (i < p.size()) {
                    // Longest run of whole words that fits. Measured from the line start each time, so a
                    // proportional font's actual widths decide the break, not a character count.
                    std::size_t fit = i, j = i;
                    while (j < p.size()) {
                        const std::size_t sp   = p.find(' ', j);
                        const std::size_t cand = (sp == std::string::npos) ? p.size() : sp;
                        if (!fits(p.substr(i, cand - i))) break;
                        fit = cand;
                        if (sp == std::string::npos) break;
                        j = sp + 1;
                    }
                    if (fit == i) {                            // not even one word fits — break inside it
                        std::size_t k = i;
                        while (k < p.size()) {
                            std::size_t step = 1;              // whole UTF-8 sequence, never half a glyph
                            while (k + step < p.size() && (static_cast<unsigned char>(p[k + step]) & 0xC0) == 0x80) ++step;
                            if (k > i && !fits(p.substr(i, k + step - i))) break;
                            k += step;
                        }
                        fit = std::max(k, i + 1);              // always consume something, or this loops
                    }
                    out.push_back({ p.substr(i, fit - i), para + i });
                    i = fit;
                    if (i < p.size() && p[i] == ' ') ++i;      // the space we broke on is spent on the break
                }
            }
            if (nl == std::string::npos) break;
            para = nl + 1;
        }
        return out;
    }

    static std::vector<std::string> splitLines(const std::string& t) {
        std::vector<std::string> out;
        size_t a = 0;
        for (;;) {
            const size_t b = t.find('\n', a);
            out.push_back(t.substr(a, b == std::string::npos ? b : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }

    // The height a WRAPPED caption needs inside a box of this width — the inverse question to
    // naturalSize's, which asks how wide one unbroken line wants to be. The caption editor asks this one
    // while you type into a wrapping label: its width is the author's, so it can only grow downward.
    static float wrappedHeight(const std::string& text, const std::string& font, float boxW) {
        // No atlas guard, unlike naturalSize: lineHeight() and measureWidth both answer with their own
        // fallbacks, so the arithmetic is defined either way — and it is the same arithmetic the render
        // would do in that state, rather than a made-up 32.
        const float lh0 = jf::JTextHelper::lineHeight();
        if (lh0 <= 0.f) return 32.f;
        const float scale = fontSpecPx(font, 16.f) / lh0;
        const std::string t = text.empty() ? std::string("W") : text;    // never collapse to a zero-height box
        const std::vector<Line> lines = layoutLines(t, true, scale, boxW - 2.f * kTextPad);
        return lh0 * scale * static_cast<float>(lines.size()) + 10.f;    // same slack naturalSize allows
    }

    static void naturalSize(const std::string& text, const std::string& font, double rotationDeg,
                            float& w, float& h) {
        const float lh0 = jf::JTextHelper::lineHeight();
        if (lh0 <= 0.f || !jf::JTextHelper::hasAtlas()) { w = 140.f; h = 32.f; return; }
        const float scale = fontSpecPx(font, 16.f) / lh0;
        const std::string t = text.empty() ? std::string("W") : text;   // never collapse to a zero-width box
        // Widest LINE by width, and as many lines tall — a caption fits its longest line, not its total
        // character count, or a two-line label would size itself as if it were one very long one.
        const std::vector<std::string> lines = splitLines(t);
        float widest = 0.f;
        for (const std::string& ln : lines)
            widest = std::max(widest, jf::JTextHelper::measureWidthScaled(ln, scale));
        w = widest + 2.f * kTextPad + 4.f;                              // both insets + slack
        h = lh0 * scale * static_cast<float>(lines.size()) + 10.f;
        if (std::fabs(rotationDeg) > 0.01) {
            const float rad = static_cast<float>(rotationDeg * 3.14159265358979323846 / 180.0);
            const float cs = std::fabs(std::cos(rad)), sn = std::fabs(std::sin(rad));
            const float rw = w * cs + h * sn, rh = w * sn + h * cs;
            w = rw; h = rh;
        }
    }

    void loadContent(const PanelElement& el) override {
        m_text       = el.prop("labelText");
        // Legacy migration: labels used to carry their OWN "textColor" alongside the skin's fgColor.
        // Fold the old value into fgColor (loadSkin has already run) so panels saved before the
        // duplicate picker was removed keep their caption colour instead of snapping back to default.
        if (fgColor.empty()) fgColor = el.prop("textColor");
        if (fontName.empty()) fontName = el.prop("font");   // legacy per-label font -> the shared skin font
        m_bold       = el.prop("bold") == "1";
        m_rotation   = skin::num(el, "rotation", 0);
        m_ghosting   = el.prop("ghosting") == "1";
        m_ghostColor = el.prop("ghostColor");
        m_align      = el.prop("align");
        m_wrap       = el.prop("wrap") == "1";
        m_enabled    = el.prop("enableCondition");
        m_visible    = el.prop("condition");
        m_link       = el.prop("link");
        m_unitOf     = el.prop("unitOf");
    }
    void saveContent(PanelElement& el) const override {
        el.props["labelText"]  = m_text;
        el.props["bold"]       = m_bold ? "1" : "";
        el.props["rotation"]   = std::to_string(m_rotation);
        el.props["ghosting"]   = m_ghosting ? "1" : "";
        el.props["ghostColor"] = m_ghostColor;
        el.props["align"]      = m_align;
        el.props["wrap"]       = m_wrap ? "1" : "";
        el.props["enableCondition"] = m_enabled;
        el.props["condition"]  = m_visible;
        el.props["link"]       = m_link;
        el.props["unitOf"]     = m_unitOf;
    }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);   // uid, geometry, skin (no binding rows — not a live-value widget)
        using jf::JPropertyMeta;
        m.add("labelText",  this, &LabelWidget::text,       &LabelWidget::setText,       JPropertyMeta{ .label = "Text", .def = "Label", .order = 150 });
        // SHOW THE UNIT INSTEAD OF THE TEXT. A page that wants "RPM" beside a number had to type it, and
        // a typed unit is a lie waiting to happen: change the Display Unit from °C to °F and the caption
        // still says °C. Bound to the same channel as the control it labels, this caption IS that
        // channel's display unit and follows it.
        // THE UNIT OF ANOTHER WIDGET, named by its path: "@<uid>.units", or the bare uid. A page wanting
        // "RPM" beside a number had to type it, and a typed unit is a lie waiting to happen -- change the
        // gauge to °F and the caption still says °C, confidently, for ever.
        //
        // It points at the CONTROL, not at a channel. The unit shown is the control's own setting (its
        // displayUnit pin, or Auto resolving the global preference), so a caption that resolved the same
        // channel independently would answer for itself and could disagree with the gauge beside it --
        // and would not follow a global C/F change the way the gauge does. Nor is it taken from whatever
        // the label was dropped with: a caption should say which widget it describes, not inherit it.
        m.add("unitOf",     this, &LabelWidget::unitOf,    &LabelWidget::setUnitOf,
              JPropertyMeta{ .label = "Unit Of Widget", .editor = "expr", .order = 152 });
        m.add("align",      this, &LabelWidget::align,      &LabelWidget::setAlign,       JPropertyMeta{ .label = "Align", .def = "Center", .order = 151,
              .choices = { std::string("Left"), std::string("Center"), std::string("Right") } });
        m.add("bold",       this, &LabelWidget::bold,       &LabelWidget::setBold,         JPropertyMeta{ .label = "Bold", .order = 154 });
        // OFF by default: a caption that has always run past its box is one an author may have sized the
        // surrounding layout around, and turning wrapping on under it would reflow pages nobody touched.
        m.add("wrap",       this, &LabelWidget::wrap,       &LabelWidget::setWrap,         JPropertyMeta{ .label = "Wrap Text", .order = 153 });
        m.add("rotation",   this, &LabelWidget::rotation,   &LabelWidget::setRotation,     JPropertyMeta{ .label = "Rotation", .min = -180, .max = 180, .suffix = "\xC2\xB0", .order = 155 });
        m.add("ghosting",   this, &LabelWidget::ghosting,   &LabelWidget::setGhosting,     JPropertyMeta{ .label = "Ghosting (LCD)", .order = 156 });
        // Target dashboard node path — blank for an ordinary caption. The "node" editor opens the
        // navigation-tree popup picker; dragging a tree node onto the caption sets it too (Surface::tryDrop_).
        m.add("link",       this, &LabelWidget::link,       &LabelWidget::setLink,         JPropertyMeta{ .label = "Link", .editor = "node", .order = 160 });
        m.add("ghostColor", this, &LabelWidget::ghostColor, &LabelWidget::setGhostColor,   JPropertyMeta{ .label = "Ghost Colour", .editor = "color", .inheritable = true, .order = 157 });
    }

    // A link follows on click, in run mode. A caption with no link never reaches here — interactive() is
    // false — so a plain label cannot swallow a press.
    bool handleControlInput(const jf::JRect& /*r*/, const ControlInput& in) override {
        if (in.kind != ControlInput::Kind::Press || !linkActive()) return false;
        if (hyperlink::navigator()) hyperlink::navigator()(m_link);
        return true;
    }

    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override {
        _renderCaption(buf, r, c);
        // Underline on hover, like a web link — run mode only, only when it is actually an active link, and
        // only while the cursor is over it. Uses the caption geometry the paint published in m_cap.
        if (s_editMode || !m_cap.ok || !linkActive()) return;
        if (s_hoverX < r.x || s_hoverX >= r.x + r.width || s_hoverY < r.y || s_hoverY >= r.y + r.height) return;
        uint8_t ub[4]; const uint8_t* uc = resolvedFg(ub, jf::Colors::TextPrimary);
        const float th = std::max(1.f, m_cap.lineH * 0.06f);      // thickness tracks the text size
        buf.pushRectangle(m_cap.x, m_cap.y + m_cap.lineH - th, m_cap.w, th, uc, 0.f);
    }

    void _renderCaption(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
        m_cap.ok = false;                                      // reset; set once the caption is actually drawn
        uint8_t tcb[4];
        const uint8_t* tc = resolvedFg(tcb, jf::Colors::TextPrimary);
        // While the Surface has this label open for in-place editing, paint the live buffer instead of the
        // stored caption (and never the "Label" placeholder — an empty buffer must read as empty, with just
        // a caret, or clearing the text would look like it snapped back).
        LabelEdit* ed = nullptr;
        if (element()) { const auto it = labelEdits().find(element()->uid);
                         if (it != labelEdits().end() && it->second.active) ed = &it->second; }
        // Enabled expression false → fade the caption toward transparent, so a disabled label reads as
        // greyed/faded rather than getting the opaque dim wash the other controls use. Not while it's open
        // for inline editing — the text must stay legible as you type.
        uint8_t faded[4];
        if (renderDisabled() && !ed) {
            faded[0] = tc[0]; faded[1] = tc[1]; faded[2] = tc[2];
            faded[3] = static_cast<uint8_t>(tc[3] * 0.35f);
            tc = faded;
        }
        // The unit is the caption when asked for, EXCEPT while editing: typing into a caption must show
        // what is being typed. An unbound label, or a channel with no unit, falls back to its text rather
        // than drawing an empty box that gives no clue why.
        // A LABEL THAT IS A UNIT CAPTION SHOWS THE UNIT, INCLUDING WHEN THERE ISN'T ONE. Falling back to
        // the "Label" placeholder made a Raw control's caption read as broken -- Raw means "no unit", and
        // an empty caption is the honest way to draw that. It is also what hid a stale address: the
        // placeholder looks identical whether the widget has no unit or does not exist.
        //
        // So the two are separated. Resolved-but-unitless draws nothing. UNRESOLVED says so, and only
        // while editing: an author needs to see that the address is dead, an operator does not need a
        // diagnostic on their dashboard.
        bool unitCaption = false;
        std::string unitCap;
        if (!m_unitOf.empty() && !ed && widgetsigil::unitOf()) {
            unitCaption = true;
            if (const auto u = widgetsigil::unitOf()(m_unitOf)) unitCap = *u;
            else unitCap = s_editMode ? "?" + m_unitOf : std::string();
        }
        const Range* band = activeRange();
        const std::string text = chooseCaption(ed != nullptr, ed ? ed->core.text() : std::string(),
                                               band ? band->text : std::string(),
                                               unitCaption, unitCap, m_text,
                                               element() && element()->props.count("labelText") != 0);
        const float lh0 = jf::JTextHelper::lineHeight();
        if (!jf::JTextHelper::hasAtlas() || lh0 <= 0.f) return;
        if (text.empty() && !ed) return;                       // nothing to draw, and no caret to place
        const float zoom = renderZoom();                               // the view scale, stated by our container
        const std::string spec = boldedSpec(fontName, m_bold);         // Bold checkbox overrides the spec
        const float fontPx = fontSpecPx(spec, 16.f);                   // 1:1 px; empty/unset font → 16 px
        const float scale = fontPx / lh0 * zoom;
        std::string gs;
        if (m_ghosting) for (char ch : text) gs += (ch >= '0' && ch <= '9') ? '8' : (ch == '.' || ch == '-') ? ch : ' ';
        // Width-INDEPENDENT inset so left/right-aligned captions line up across differing box widths.
        // An explicit Padding wins (0 included); unset (-1) falls back to kTextPad. Scaled by zoom so it
        // stays visually constant as the canvas scales. Measured BEFORE the lines because it is what the
        // caption has to fit inside once it wraps.
        const float pad = (padding >= 0 ? static_cast<float>(padding) : kTextPad) * zoom;
        const bool  rot = std::fabs(m_rotation) > 0.01;
        // MULTI-LINE. The caption is a block of lines centred vertically as a whole; each line is aligned
        // on its own within the box, so a centred two-line caption centres both lines rather than centring
        // the longest and hanging the other off it. One line is the same arithmetic with a count of 1.
        //
        // Wrapping is measured against the SAME inset the drawing uses, so a line that fits by the wrap's
        // arithmetic fits on screen. Not while ROTATED: glyphs still run along the rotated baseline, so
        // the width a turned caption has to live inside is not this box's width, and wrapping to it would
        // break the text at a length that means nothing.
        const std::vector<Line> lines = layoutLines(text, m_wrap && !rot, scale, r.width - 2.f * pad);
        const float vlh = lh0 * scale;
        const float blockH = vlh * static_cast<float>(lines.size());
        const float ty = r.y + (r.height - blockH) * 0.5f;
        auto alignX = [&](float w) -> float {
            if (m_align == "Left")  return r.x + pad;
            if (m_align == "Right") return r.x + r.width - pad - w;
            return r.x + (r.width - w) * 0.5f;                         // Center (default / empty)
        };
        // Rotation pivots on the widget centre, so a label spins in place rather than swinging off its origin.
        const float rcx = r.x + r.width * 0.5f, rcy = r.y + r.height * 0.5f;
        if (m_ghosting) {
            static const uint8_t kGhostDefault[4] = { 0, 50, 0, 50 };  // faint unlit-LCD green
            uint8_t gcb[4]; const uint8_t* gc = skin::parseHex(m_ghostColor, gcb) ? gcb : kGhostDefault;
            const float gw = jf::JTextHelper::measureWidthScaled(gs, scale);
            if (rot) pushTextRotated(buf, alignX(gw), ty, gs, gc, scale, fontSpecFace(spec), rcx, rcy, m_rotation);
            else     jf::JTextHelper::pushTextScaled(buf, alignX(gw), ty, gs, gc, scale, 0.f, fontSpecFace(spec));
        }
        // Each line already knows the byte offset it starts at (layoutLines carries it), so a caret index
        // resolves to a line + an x within it. Deriving it here instead — start + length + 1 per line —
        // was right only while '\n' was the sole way to break, and silently wrong for a wrapped one.
        auto lineX = [&](size_t li) {
            return alignX(jf::JTextHelper::measureWidthScaled(lines[li].text, scale));
        };
        // Caret/selection position of a byte index: which line it falls on, and where along it.
        auto posOf = [&](int byteIdx) {
            const size_t b = static_cast<size_t>(std::clamp(byteIdx, 0, static_cast<int>(text.size())));
            size_t li = 0;
            while (li + 1 < lines.size() && b >= lines[li + 1].start) ++li;
            const size_t off = b - lines[li].start;
            const float x = lineX(li) + jf::JTextHelper::measureWidthScaled(lines[li].text.substr(0, off), scale);
            return std::pair<float, float>{ x, ty + vlh * static_cast<float>(li) };
        };
        const float tw = jf::JTextHelper::measureWidthScaled(lines.empty() ? text : lines[0].text, scale);
        const float tx = alignX(tw);
        // A tinted wash + selection band go UNDER the glyphs so the text stays legible on top.
        if (ed) {
            ed->tx = tx; ed->ty = ty; ed->scale = scale; ed->lineH = lh0 * scale;
            const uint8_t wash[4] = { jf::Colors::Accent[0], jf::Colors::Accent[1], jf::Colors::Accent[2], 28 };
            buf.pushRectangle(r.x, r.y, r.width, r.height, wash, 0.f, 1.f, jf::Colors::Accent);
            const int lo = static_cast<int>(ed->core.selectionStart()), hi = static_cast<int>(ed->core.selectionEnd());
            if (hi > lo) {
                const uint8_t sel[4] = { jf::Colors::Accent[0], jf::Colors::Accent[1], jf::Colors::Accent[2], 110 };
                // A selection spanning a newline is several bands, one per line — a single rectangle from
                // the first byte to the last would paint straight through the gap between them.
                for (size_t li = 0; li < lines.size(); ++li) {
                    const int ls = static_cast<int>(lines[li].start);
                    const int le = ls + static_cast<int>(lines[li].text.size());
                    const int a = std::max(lo, ls), b = std::min(hi, le);
                    if (b <= a) continue;
                    const auto pa = posOf(a), pb = posOf(b);
                    buf.pushRectangle(pa.first, pa.second, pb.first - pa.first, vlh, sel, 0.f);
                }
            }
        }
        if (!text.empty()) {
            for (size_t li = 0; li < lines.size(); ++li) {
                if (lines[li].text.empty()) continue;          // a blank line still occupies its row
                const float lx = lineX(li), ly = ty + vlh * static_cast<float>(li);
                if (rot) pushTextRotated(buf, lx, ly, lines[li].text, tc, scale, fontSpecFace(spec), rcx, rcy, m_rotation);
                else     jf::JTextHelper::pushTextScaled(buf, lx, ly, lines[li].text, tc, scale, 0.f, fontSpecFace(spec));
            }
            // Published for a subclass underline (Hyperlink). The FIRST line: identical to the old value
            // for a one-line caption, and an underline under the whole block would be wrong anyway.
            m_cap = { tx, ty, tw, vlh, !rot };
        }
        if (ed) {
            const auto c = posOf(static_cast<int>(ed->core.caret()));
            buf.pushRectangle(c.first, c.second, std::max(1.f, scale), vlh, jf::Colors::Accent, 0.f);
        }
    }

protected:
    // Screen-space geometry of the caption as last drawn — published by the paint so the hover underline
    // can underline exactly the drawn text without recomputing the align/scale maths. ok=false when nothing
    // was painted (no atlas / empty caption).
    struct CaptionGeom { float x = 0.f, y = 0.f, w = 0.f, lineH = 0.f; bool ok = false; };
    CaptionGeom m_cap;

private:
    std::string m_text;
    bool        m_bold      = false;
    double      m_rotation  = 0.0;
    bool        m_ghosting  = false;
    std::string m_ghostColor;             // "" = faint unlit-LCD green default
    std::string m_align;                  // "" = Center
    bool        m_wrap      = false;      // break the caption to the widget width (authored '\n' still breaks)
    std::string m_enabled;                // "" = always enabled; else a sigil expression (stored as enableCondition)
    std::string m_visible;                // "" = always visible; else a sigil expression (stored as condition)
    std::string m_unitOf;                 // "@<uid>.units" — caption = THAT widget's display unit
    std::string m_link;                   // "" = a plain caption; else a dashboard node path to navigate to

    // "Active" (clickable + underlines) only when it HAS a link and is ENABLED. A caption gated off by its
    // own expression is not a link you can follow — it is a greyed word.
    bool linkActive() const {
        if (m_link.empty()) return false;
        if (m_enabled.empty() ? false : evalSource(m_enabled).v == 0.0) return false;
        // ... and the TARGET has to be reachable. A link to a node the run-mode condition filter has
        // hidden is not a link the operator can follow: it underlined on hover, swallowed the press and
        // navigated to a page their own settings had removed from the menu. Failing here leaves it as
        // plain text -- the same greyed word a disabled caption becomes -- rather than a live-looking
        // control that goes somewhere it should not.
        return hyperlink::canFollow(m_link);
    }
};

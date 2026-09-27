#pragma once

// ValueWidget — a live telemetry readout: the bound channel's value, printf-formatted and unit-converted,
// auto-fit at a fixed 1:1 px font (canvas-zoom scales it, widget-resize does not). A real CanvasWidget
// subclass (the class-hierarchy port). It paints ONLY the value text; the base drawBackground/drawBorder owns
// the (range-aware) frame around it, so a value range that recolours the cell just works. Skin + binding come
// from the typed base members; its one unique property (Number Format) is declared via collectProperties().
// It carries no caption or units of its own (captions are separate grouped Label elements), so this is a
// pure value cell.

#include "../CanvasWidget.h"   // base class + resolvedFg/skin/fontSpecPx/formatValue/PanelElement

#include <algorithm>

class ValueWidget : public CanvasWidget {
public:
    explicit ValueWidget(jf::JSceneGraph& g) : CanvasWidget(g, "value") {}
    std::string elementType() const override { return "value"; }
    std::string paletteTitle() const override { return "Value"; }
    float       defaultW()     const override { return 160.f; }
    float       defaultH()     const override { return 90.f; }

    const std::string& format() const { return m_format; }   // printf format; "" → precision from the unit
    void setFormat(const std::string& f) { if (m_format == f) return; m_format = f; emitModified(); }
    // Whether the reading carries its unit. ON unless turned off, so a page authored before this row
    // existed reads exactly as it did.
    bool showUnit() const { return m_showUnit; }
    void setShowUnit(bool b) { if (m_showUnit == b) return; m_showUnit = b; emitModified(); }

    void loadContent(const PanelElement& el) override {
        m_format = el.prop("format");
        m_showUnit = el.prop("showUnit") != "0";       // absent = show (the old, only, behaviour)
    }
    void saveContent(PanelElement& el) const override {
        el.props["format"] = m_format;
        el.props["showUnit"] = m_showUnit ? "" : "0";  // only the OFF state is worth storing
    }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);   // uid, geometry, binding (Data Source/Display Unit/Current Value), skin
        // BLANK MEANS "LET THE UNIT DECIDE", and that is the default. It used to default to "%.1f",
        // which is not a default at all once the studio saves the page: the value materialises into the
        // element and from then on the widget has an EXPLICIT one-decimal format, overriding the
        // precision the displayed unit implies. That is how a raw pin read as volts printed "3.8V" and
        // stayed there — an installer writing no format, and the studio writing "%.1f" back over it on
        // the next save. Blank survives a save, so the rule keeps applying.
        m.add("format", this, &ValueWidget::format, &ValueWidget::setFormat,
              jf::JPropertyMeta{ .label = "Number Format", .def = "", .order = 24 });
        // Sits right under Display Unit, which is the setting it answers: that row picks WHICH unit the
        // number is in, this one whether the reading says so out loud.
        m.add("showUnit", this, &ValueWidget::showUnit, &ValueWidget::setShowUnit,
              jf::JPropertyMeta{ .label = "Show Unit", .def = "1", .order = 25 });
    }

    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override {
        if (!m_el) return;                                   // the bound element supplies the live value
        uint8_t fgb[4];
        const uint8_t* fg = resolvedFg(fgb, jf::Colors::TextPrimary);   // range → local fgColor → scheme
        // Through the shared rule: this inset is taken from a rect the base has ALREADY padded, so doing
        // it by hand put a cell that survived the first inset back under water — 8px of content less 6
        // per side is -4, and the guard below then declined to draw the number at all.
        const jf::JRect in = insetRect(r, static_cast<float>(std::max(0, borderWidth)) + 6.f);
        const std::string v = CanvasWidget::fmtVal(*m_el, c);         // bound channel, formatted + unit-converted
        if (in.height > 1.f && jf::JTextHelper::hasAtlas()) {
            const float lh0 = jf::JTextHelper::lineHeight();
            const float zoom = renderZoom();                                // the view scale, stated by our container
            float scale = (lh0 > 0.f) ? fontSpecPx(fontName, 32.f) / lh0 * zoom : 1.f;
            // A WORD THAT DOES NOT FIT SHRINKS; it is never guillotined. A state channel reads in
            // words — "Closed Loop", not 5 — and a word too wide for its cell lost its last letters
            // with no ellipsis and nothing on screen to say so ("Advance Map" came out "Advance Mac").
            // Down to a floor, because a reading nobody can read is not a reading either: under that
            // the cell was authored too small for the channel in it, and check_doc says which one.
            //
            // ONLY A WORD. A number is sized by whoever authored the cell, from the widest one the
            // channel can print, and it must not resize itself as the value changes — a column of
            // readouts where each number picks its own size as it moves is unreadable in a different
            // way. This was written for every reading and did exactly that on the idle pages.
            float vw = jf::JTextHelper::measureWidthScaled(v, scale);
            if (vw > in.width && vw > 0.f && readingIsWord(*m_el)) {
                const float floorScale = (lh0 > 0.f) ? 10.f / lh0 * zoom : scale;
                scale = std::max(std::min(scale, floorScale), scale * in.width / vw);
                vw = jf::JTextHelper::measureWidthScaled(v, scale);
            }
            const float vlh = lh0 * scale;
            jf::JTextHelper::pushTextScaled(buf, in.x + (in.width - vw) * 0.5f,
                                            in.y + (in.height - vlh) * 0.5f, v, fg, scale, 0.f, fontSpecFace(fontName));
        }
    }

private:
    std::string m_format;      // "" → the displayed unit's own format (UnitManager::Unit::format)
    bool        m_showUnit = true;   // false → the number alone (element prop "showUnit" == "0")
};

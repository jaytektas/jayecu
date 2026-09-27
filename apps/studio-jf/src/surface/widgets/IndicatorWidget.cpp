// IndicatorWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "IndicatorWidget.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"

void IndicatorWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
    // A lamp fills its WHOLE rect with the state colour and centres the title in the state text colour —
    // no border, no generic skin background (faithful to IndicatorWidget::drawGauge).
    const PanelElement& el = *m_el;
    const bool on = evalSource(bindPath()).v != 0.0;
    static const uint8_t kOnBg[4] = {0x2e, 0xa0, 0x43, 255}, kOnFg[4] = {0, 0, 0, 255};
    // AN UNSTYLED LAMP FOLLOWS THE SCHEME. The LIT colours are the definition's business — a red lamp is
    // red on any theme, and it is red because something is wrong. "Not lit" is not information, it is the
    // absence of it, and it was a hard-coded dark grey with white text: right on the dark theme, wrong on
    // the light one, and there is no third colour that is right on both. A quiet chip in the scheme's own
    // surface reads as a lamp that is not lit, whichever scheme is on.
    uint8_t bgb[4], fgb[4];
    const uint8_t* bg = on ? elColor(el, "onBg", kOnBg, bgb)
                           : elColor(el, "offBg", jf::Colors::Surface2, bgb);
    const uint8_t* fg = on ? elColor(el, "onFg", kOnFg, fgb)
                           : elColor(el, "offFg", jf::Colors::TextSecondary, fgb);
    buf.pushRectangle(r.x, r.y, r.width, r.height, bg);   // whole-rect state fill, no border
    std::string title = on ? (el.prop("onTitle").empty() ? "ON" : el.prop("onTitle"))
                           : (el.prop("offTitle").empty() ? "OFF" : el.prop("offTitle"));
    // A TunerStudio STRING EXPRESSION label (spec §23): the caption is a prefix with a bit-typed channel's
    // value name appended — "ETB" + "TPS error". The names come from the channel's own option list and the
    // index from a live expression, so the lamp says WHICH fault it is rather than merely that there is
    // one. Absent either prop this is an ordinary static caption.
    if (on) {
        const std::string list = el.prop("onTitleList");
        if (!list.empty()) {
            const MetaModel* mm = m_cache ? m_cache->meta() : nullptr;
            const std::vector<std::string> names = mm ? mm->enumOptions(list) : std::vector<std::string>{};
            const int idx = static_cast<int>(evalSource(el.prop("onTitleValue")).v);
            if (idx >= 0 && idx < static_cast<int>(names.size())) {
                if (!title.empty()) title += ": ";
                title += names[idx];
            }
        }
    }
    if (jf::JTextHelper::hasAtlas() && !title.empty())
        jf::JTextHelper::pushText(buf, r.x + (r.width - jf::JTextHelper::measureWidth(title)) * 0.5f,
                                  r.y + (r.height - jf::JTextHelper::lineHeight()) * 0.5f, title, fg, r.width - 8.f);
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("indicator", IndicatorWidget, 120);

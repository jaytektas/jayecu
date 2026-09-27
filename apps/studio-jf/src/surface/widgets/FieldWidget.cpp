// FieldWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "FieldWidget.h"
#include "../../model/Cache.h"

void FieldWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    const jf::JRect box = r;
    uint8_t fgb[4]; const uint8_t* fg = fgOf(el, fgb);
    const std::string v = fmtVal(el, c);
    if (jf::JTextHelper::hasAtlas()) {
        const float lh0 = jf::JTextHelper::lineHeight();
        const float zoom = (el.w > 0.f) ? r.width / el.w : 1.f;
        const float scale = (lh0 > 0.f) ? fontSpecPx(el.prop("fontName"), 16.f) / lh0 * zoom : 1.f;
        const float vw = jf::JTextHelper::measureWidthScaled(v, scale), vlh = lh0 * scale;
        const int align = std::atoi(el.prop("align").c_str());
        const float tx = align == 1 ? box.x + (box.width - vw) * 0.5f
                       : align == 2 ? box.x + 8.f
                                    : box.x + box.width - vw - 8.f;
        jf::JTextHelper::pushTextScaled(buf, tx, box.y + (box.height - vlh) * 0.5f, v, fg, scale, 0.f, fontSpecFace(el.prop("fontName")));
    }
}

double FieldWidget::sigilValue(const std::string& prop, const PanelElement& el, const Cache&) const {
    if (prop == "value") { return evalSource(bindPath()).v; }
    return std::nan("");
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("field", FieldWidget, 60);

// RadioWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "RadioWidget.h"
#include "../../model/Cache.h"
#include <j/graphics/VectorGraphics.h>
#include "../../model/MetaModel.h"

void RadioWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    const jf::JRect box = r;
    const std::string bind = bindPath();
    std::vector<std::string> opts = c.meta() ? c.meta()->enumOptions(bind) : std::vector<std::string>{};
    if (opts.empty()) opts = { "Off", "On" };
    const int cur = static_cast<int>(std::lround(bind.empty() ? -1.0 : evalSource(bind).v));
    const bool vert = el.prop("orientation") != "Horizontal";   // default Vertical
    // Through resolvedAccent, not elColor: a range band's accent is editable in the Range editor and was
    // being written into the element and then ignored — bg/fg/border all honoured their band, accent
    // alone painted the flat property. Same fall-through as the others: band -> local prop -> scheme.
    uint8_t fgb[4], ab[4]; const uint8_t* fg = fgOf(el, fgb);
    const uint8_t* accent = resolvedAccent(ab, jf::Colors::Accent);
    const float rr = 7.f, lh = jf::JTextHelper::lineHeight();
    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
    float x = box.x + 4.f, y = box.y + 4.f;
    for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        const float cxp = x + rr, cyp = y + rr;
        vg.strokeCircle(cxp, cyp, rr, 1.5f, jf::JPaint::solid(jc(jf::Colors::Border)));
        if (i == cur) vg.fillCircle(cxp, cyp, rr * 0.55f, jf::JPaint::solid(jc(accent)));
        if (jf::JTextHelper::hasAtlas()) jf::JTextHelper::pushText(buf, x + 2.f * rr + 6.f, y + (2.f * rr - lh) * 0.5f, opts[i], fg, box.width);
        if (vert) y += 2.f * rr + 8.f;
        else x += 2.f * rr + 10.f + jf::JTextHelper::measureWidth(opts[i]) + 14.f;
    }
    vg.flush(buf);
}

bool RadioWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;
    if (in.kind != ControlInput::Kind::Press) return false;
    const std::string bind = bindPath();
    if (bind.empty()) return false;
    Cache& C = Cache::instance();
    if (!C.isConfig(bind)) return false;
    std::vector<std::string> opts = C.meta() ? C.meta()->enumOptions(bind) : std::vector<std::string>{};
    if (opts.empty()) opts = { "Off", "On" };
    const jf::JRect box = r;
    const bool vert = el.prop("orientation") != "Horizontal";
    const float rr = 7.f;
    int hit = -1;
    if (vert) {
        float y = box.y + 4.f;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) { if (in.my >= y && in.my < y + 2.f * rr + 8.f) { hit = i; break; } y += 2.f * rr + 8.f; }
    } else {
        float x = box.x + 4.f;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) { const float w = 2.f * rr + 10.f + jf::JTextHelper::measureWidth(opts[i]) + 14.f; if (in.mx >= x && in.mx < x + w) { hit = i; break; } x += w; }
    }
    if (hit < 0) return false;
    C.setConfigValue(bind, static_cast<double>(hit));
    return true;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("radio", RadioWidget, 90);

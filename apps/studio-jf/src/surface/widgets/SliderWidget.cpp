// SliderWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "SliderWidget.h"
#include "../../model/Cache.h"
#include "../../model/Keymap.h"
#include "../../model/UnitManager.h"
#include <cstdio>
#include <cmath>

void SliderWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    const jf::JRect box = r;
    const std::string bind = bindPath();
    double lo = 0, hi = 100, val = 0;
    // A writable #config binding drives the track from its meta range; a read-only source just shows its value.
    const std::string path = writableConfigPath(bind);
    if (!path.empty()) {
        // The track spans what this CONTROL accepts: the field's range narrowed by the slider's own
        // Min/Max. Reading the field's declared range alone ignored the narrowing, and read min == max as
        // no range at all — so a slider on a field that declares none had a dead track, when its datatype
        // says perfectly well what it holds.
        Cache::rawBounds(boundLoc(), lo, hi);
        val = c.configValue(path);
    }
    else val = evalSource(bind).v;
    const float frac = (hi > lo) ? static_cast<float>(std::clamp((val - lo) / (hi - lo), 0.0, 1.0)) : 0.f;
    uint8_t ab[4], fgb[4]; const uint8_t* fg = fgOf(el, fgb);
    const uint8_t* accent = resolvedAccent(ab, jf::Colors::Accent);   // band accent, then the prop (see RadioWidget)
    const bool vert = el.prop("orientation") == "Vertical";
    const float lh = jf::JTextHelper::lineHeight();
    char vb[48]; std::snprintf(vb, sizeof(vb), "%.1f", dispV(el, Raw{val}));
    std::string readout = vb;
    { const std::string u = UnitManager::instance().unitLabel(displayUnitOf(el)); if (!u.empty()) readout += " " + u; }
    const float rw = jf::JTextHelper::measureWidth(readout) + 8.f;
    jf::JRect ta = box;                                     // track area (readout carved out)
    if (vert) ta.width = box.width - rw; else ta.height = box.height - lh - 2.f;
    if (vert) {
        const float cx = ta.x + ta.width * 0.5f, ty = ta.y + 8.f, th = ta.height - 16.f;
        buf.pushRectangle(cx - 3.f, ty, 6.f, th, jf::Colors::Surface0, 3.f);
        buf.pushRectangle(cx - 3.f, ty + th * (1.f - frac), 6.f, th * frac, accent, 3.f);
        buf.pushRectangle(cx - 8.f, ty + th * (1.f - frac) - 8.f, 16.f, 16.f, jf::Colors::TextPrimary, 8.f);
        if (jf::JTextHelper::hasAtlas()) jf::JTextHelper::pushText(buf, ta.x + ta.width + 4.f, box.y + (box.height - lh) * 0.5f, readout, fg, rw);
    } else {
        const float tx = ta.x + 12.f, tw = ta.width - 24.f, ty = ta.y + ta.height * 0.5f - 3.f;
        buf.pushRectangle(tx, ty, tw, 6.f, jf::Colors::Surface0, 3.f);
        buf.pushRectangle(tx, ty, tw * frac, 6.f, accent, 3.f);
        buf.pushRectangle(tx + tw * frac - 8.f, ta.y + ta.height * 0.5f - 8.f, 16.f, 16.f, jf::Colors::TextPrimary, 8.f);
        if (jf::JTextHelper::hasAtlas()) jf::JTextHelper::pushText(buf, box.x + (box.width - jf::JTextHelper::measureWidth(readout)) * 0.5f, box.y + box.height - lh, readout, fg, box.width);
    }
}

bool SliderWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;
    const std::string path = writableConfigPath(bindPath());
    if (path.empty()) return false;   // read-only source (telemetry / widget / expression) — not editable
    Cache& C = Cache::instance();

    // Keyboard value nudge (Keymap Increase/Decrease, x10 for "large"). Step is one unit of the config's
    // displayed precision; the result is clamped to the meta range. A slider has no inline typing, so no
    // suppression is needed. Only Key events take this path; drag falls through below.
    if (in.kind == ControlInput::Kind::Key) {
        if (!in.key) return false;
        double mult = 0.0;
        switch (Keymap::instance().action(*in.key)) {
            case Keymap::Action::IncreaseValue:      mult = +1.0;  break;
            case Keymap::Action::DecreaseValue:      mult = -1.0;  break;
            case Keymap::Action::IncreaseValueLarge: mult = +10.0; break;
            case Keymap::Action::DecreaseValueLarge: mult = -10.0; break;
            default: return false;
        }
        // No clamp here: the write clamps, against the same narrowed location the track is drawn from.
        // A second clamp in the widget is a second opinion, and the two drifted the moment either moved.
        const double step = std::pow(10.0, -C.digits(path));
        C.writeRaw(boundLoc(), Raw{C.configValue(path) + mult * step}, path);
        return true;
    }

    if (in.kind != ControlInput::Kind::Press && in.kind != ControlInput::Kind::Move) return false;
    double lo = 0.0, hi = 0.0;
    Cache::rawBounds(boundLoc(), lo, hi);
    if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi)) return false;   // no span to drag along
    const jf::JRect box = r;
    const bool vert = el.prop("orientation") == "Vertical";
    const float frac = vert ? std::clamp(1.f - (in.my - box.y - 8.f)  / std::max(1.f, box.height - 16.f), 0.f, 1.f)
                            : std::clamp((in.mx - box.x - 12.f) / std::max(1.f, box.width - 24.f), 0.f, 1.f);
    C.writeRaw(boundLoc(), Raw{lo + static_cast<double>(frac) * (hi - lo)}, path);   // same location the track spans
    return true;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("slider", SliderWidget, 50);

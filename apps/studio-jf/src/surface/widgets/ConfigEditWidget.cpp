// ConfigEditWidget — supplies a live jf::JDoubleSpinBox to HostedControlWidget + syncs it to the bound config
// scalar. The base paints the control + forwards run-mode mouse/key; this class owns the value round-trip
// (display units in the box, source units in the config) and the read-only / wheel-nudge behaviour.

#include "ConfigEditWidget.h"
#include "../../model/Cache.h"
#include "../../model/UnitManager.h"
#include <j/core/Log.h>
#include <cmath>
#include <cstdlib>

void ConfigEditWidget::loadContent(const PanelElement& el) {
    const std::string d = el.prop("decimals");
    m_decimals = d.empty() ? -1 : std::atoi(d.c_str());
    _adoptLegacyFormat(el);
}

int ConfigEditWidget::decimals() const {
    // Prefer the element's live prop: the inspector writes there, and it is the source of truth whether or
    // not the widget has been reloaded since the edit.
    if (const PanelElement* el = element()) {
        const std::string d = el->prop("decimals");
        if (!d.empty()) { const int v = std::atoi(d.c_str()); if (v >= 0) return v; }
    }
    if (m_decimals >= 0) return m_decimals;          // authored override
    // A FIELD SHOWN IN ANOTHER UNIT READS AT THAT UNIT'S PRECISION. A sensor's raw threshold is stored
    // in ADC counts (no decimals) and shown in volts: at the field's own precision 0.25 V and 4.75 V
    // displayed as "0" and "5". The unit knows what a decimal is worth in it ("%.3f" for volts).
    if (const PanelElement* el = element()) {
        const std::string dst = displayUnitOf(*el);
        const std::string src = bindingUnit(bindPath());
        if (!dst.empty() && !src.empty() && dst != src)
            if (const int ud = UnitManager::instance().unitDigits(dst); ud >= 0) return ud;
    }
    // AUTO: the field's own precision, which Cache::digits() derives from its scale (0.1 -> 1 decimal).
    // Prefer the live bind, but when the index is unresolved (an unused slot -> blank bind) fall back to
    // the FIELD path, so the cell shows its field's precision ("0" for a Bank) rather than a bare "0.00".
    std::string path = writableConfigPath(bindPath());
    if (path.empty()) path = writableConfigPath(fieldPath());
    return path.empty() ? 2 : Cache::instance().digits(path);
}

// Back-compat: elements saved with the old printf "format" prop. A deliberately authored precision is
// adopted as Decimals; the old DEFAULT ("%.2f") is treated as unauthored so the field falls back to auto and
// finally shows its schema precision (a 0.1-scale field reads 98.0, not 98.00).
void ConfigEditWidget::_adoptLegacyFormat(const PanelElement& el) {
    if (m_decimals >= 0) return;                     // already authored in the new form
    const std::string f = el.prop("format");
    if (f.empty() || f == "%.2f") return;            // absent, or the old default -> auto
    const auto dot = f.find('.');
    if (dot == std::string::npos) return;
    int d = 0; bool any = false;
    for (size_t i = dot + 1; i < f.size() && f[i] >= '0' && f[i] <= '9'; ++i) { d = d * 10 + (f[i] - '0'); any = true; }
    if (any) m_decimals = d;
}

jf::JControl* ConfigEditWidget::control() {
    if (!m_spin) {
        // Range/decimals/step are (re)configured every frame in syncControl; construct with placeholders.
        m_spin = std::make_unique<jf::JDoubleSpinBox>(sceneGraph(), -1e12, 1e12, 1.0, 2);
        m_spin->onValueChanged.connect([this](double disp) {
            if (syncing()) return;                              // ignore our own setValue echo
            const PanelElement* el = element();
            if (!el) return;
            if (m_readOnly || writableConfigPath(bindPath()).empty()) return;   // read-only -> no write
            // Through the NARROWED location, so the control's own Min/Max clamp the byte and not merely
            // the spin box: a value can also arrive by paste, by a bus write, or from a stale box that
            // was ranged before the setting its Max depends on changed.
            Cache::instance().writeRaw(boundLoc(), srcV(*el, disp), writableConfigPath(bindPath()));
        });
    }
    return m_spin.get();
}

void ConfigEditWidget::syncControl() {
    const PanelElement* el = element();
    if (!el) return;
    // Focus is the framework's job now (run-mode StrongFocus, see HostedControlWidget): clicking focuses the
    // box, the runner routes keys here, clicking away blurs it -> onFocusEvent(false) -> _commitEdit. No manual
    // focus driving.
    const int dec = decimals();
    m_spin->setDecimals(dec);
    m_spin->setStep(std::pow(10.0, -dec));                     // one ulp of the displayed precision
    // What this control will accept: the FIELD's range (declared, or its datatype's) narrowed by the
    // control's own Min/Max — one answer, from the same Location the write clamps against, so the box
    // and the byte can never disagree about what fits.
    const MetaModel::Location L = boundLoc();
    const double disp = dispV(*el, evalSource(bindPath()));
    m_outOfRange = false;
    if (L.valid()) {
        double rlo = 0.0, rhi = 0.0;
        Cache::rawBounds(L, rlo, rhi);
        double lo = dispV(*el, Raw{rlo}), hi = dispV(*el, Raw{rhi});
        if (lo > hi) std::swap(lo, hi);                        // guard offset units with negative slope
        if (hi > lo && std::isfinite(lo) && std::isfinite(hi)) {  // a float field's range is unbounded
            // SHOW WHAT IS STORED. A range narrower than the value in the tune used to clamp the DISPLAY:
            // the box read 8 while the ECU held 20, so the page showed a number that is nowhere — and
            // silently disagreed with the strip beside it drawing all 20 rows. The range is widened just
            // enough to admit the stored value and the box is marked instead. Nothing about the WRITE
            // changes: the value still clamps to the control's Min/Max on the way back (the same narrowed
            // Location the write goes through), so touching the box is what brings it into range.
            m_outOfRange = std::isfinite(disp) && (disp < lo || disp > hi);
            m_spin->setRange(std::min(lo, m_outOfRange ? disp : lo),
                             std::max(hi, m_outOfRange ? disp : hi));
        }
    }
    // "2500 rpm", "104.0 °C" — the number is what you type into, the unit sits beside it and does not move.
    // The framework draws a suffix OUTSIDE the editable field and reserves its width, so it survives an edit
    // untouched; the box had simply never been given one, and showed a bare number while the strip below it
    // and the caption beside it both named their unit.
    const std::string unit = displayUnitLabelOf(*el);
    m_spin->setSuffix(unit.empty() ? std::string() : " " + unit);
    if (disp != m_spin->value()) m_spin->setValue(disp);   // base SyncScope guards the echo (incl. setRange above)
}

bool ConfigEditWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    if (m_readOnly) return false;                              // display-only: no edit, no nudge
    if (in.kind == ControlInput::Kind::Scroll && m_spin) {
        const double step = std::pow(10.0, -decimals());
        m_spin->setValue(m_spin->value() + (in.wheel > 0 ? step : -step));   // fires onValueChanged -> writes source
        return true;
    }
    return HostedControlWidget::handleControlInput(screen, in);    // press begins edit; keys type; Return commits
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("configedit", ConfigEditWidget, 110);

// The out-of-range mark, drawn OVER the hosted control (a framework JDoubleSpinBox) rather than by tinting
// it: this is the studio's statement about the tune, not a state the control has. It rides render() now, so
// the base template still frames it (background, a value rule’s wash, border) around the mark.
void ConfigEditWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& content, const Cache& cache) {
    HostedControlWidget::render(buf, content, cache);   // paint the control
    if (!m_outOfRange) return;
    static const uint8_t clear[4] = { 0, 0, 0, 0 };
    buf.pushRectangle(content.x, content.y, content.width, content.height, clear, 3.f, 1.5f, jf::Colors::Warning);
}

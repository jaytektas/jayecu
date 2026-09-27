#include "TriggerDiagramWidget.h"
#include "../../ui/WrapText.h"
#include "../WidgetRegistry.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"

#include <cmath>
#include <cstdio>

namespace {
// A stream slot named by a path or a plain number. The element context a viewport pushes down is the
// viewport's own Data Source, which for a stream page is "trigger.streams[2]" — so the slot is the
// subscript. A bare number works too, for a page that names one outright.
//
// Through MathEvaluator::elementKey, which is the ONE place that answers "which element does this context
// name". This had its own copy taking the FIRST bracket, and the expression builder writes a data source
// wrapped in its sigil — "[#trigger.streams[0]]" — where the first bracket is the wrapper. It read
// "#trigger.streams[0", threw, and returned -1, so the page built with the builder was the one whose
// diagram never highlighted anything.
int slotFromKey(const std::string& key) {
    if (key.empty()) return -1;
    try { return std::stoi(MathEvaluator::elementKey(key)); } catch (...) { return -1; }
}
}  // namespace

int TriggerDiagramWidget::focusSlot() const {
    // An explicit Focus wins; "[*]" in it resolves like any binding, so a template page focuses whichever
    // stream its viewport is showing. Otherwise the page itself says, through the element context.
    if (!m_focus.empty())
        return slotFromKey(resolveTemplate(m_focus, elementContext()));
    return slotFromKey(elementContext());
}

void TriggerDiagramWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    // One place to say "nothing to draw, and why" — an empty box reads as a broken widget, which is the
    // wrong thing to conclude when the honest answer is that no stream is switched on.
    auto say = [&](const char* msg) {
        if (!jf::JTextHelper::hasAtlas()) return;
        // wrapped, and centred as a block, so a narrow widget still says all of it
        wraptext::drawCentred(buf, r.x + 4.f, r.y + (r.height - wraptext::height(msg, r.width - 8.f)) * 0.5f,
                              r.width - 8.f, msg, jf::Colors::TextSecondary);
    };
    if (!c.meta()) { say("No ECU definition"); return; }
    if (r.width < 40.f || r.height < 40.f) return;   // too small to say anything in

    // ENGINEERING units. wheelFromConfig wants degrees and tenths as a human states them, while
    // Cache::configValue returns RAW — so the scale has to be applied here. Miss it and every angle is
    // out by the field's scale: a 65 degree TDC offset (scale 0.1) reads as 650 and the whole picture
    // rotates into nonsense while still looking like a wheel.
    const Wheel w = wheelFromConfig([&c](const std::string& p) {
        return c.configValue(p) * c.configScale(p);
    });
    const WheelGeometry g = wheelGeometry(w);
    // Not a failure: no stream is enabled, which is exactly what a freshly flashed ECU looks like.
    if (g.streams.empty()) { say("No trigger stream enabled"); return; }

    // Same arrangement the designer uses: the dial above, one trace lane per stream below, because the
    // two answer different questions — the dial "what shape is this wheel", the trace "how do these
    // streams sit against each other and against TDC".
    const float pad   = 8.f;
    const float lh    = jf::JTextHelper::hasAtlas() ? jf::JTextHelper::lineHeight() : 12.f;
    const float rowH  = lh + 6.f;
    const int   lanes = static_cast<int>(g.streams.size());
    const float traceH = std::clamp(lanes * rowH + 12.f, 30.f, r.height * 0.55f);
    const float traceY = r.y + r.height - traceH - pad;
    const float dialTop = r.y + pad, dialBot = traceY - pad;
    const float cx = r.x + r.width * 0.5f, cy = dialTop + (dialBot - dialTop) * 0.5f;
    const float R  = std::min(r.width, dialBot - dialTop) * 0.42f;

    jf::JVectorCanvas vg;
    vg.setAntiAlias(1.2f);
    if (R > 10.f)
        trigger_ui::drawDial(vg, g, cx, cy, R, w.sensorAngle, 0.0);
    trigger_ui::drawTrace(vg, g, r.x + pad, traceY, r.width - 2.f * pad, traceH, rowH);
    vg.flush(buf);

    // The focused lane, marked. Deliberately a mark ON the full picture rather than a picture of one
    // stream: the question a diagram answers that a form cannot is how a stream sits against the others,
    // and drawing it alone throws exactly that away.
    const int slot = focusSlot();
    if (slot >= 0) {
        for (int i = 0; i < lanes; ++i) {
            if (g.streams[i].displayRole - 1 != slot) continue;
            const float y = traceY + 6.f + i * rowH;
            static const uint8_t clear[4] = { 0, 0, 0, 0 };
            buf.pushRectangle(r.x + pad - 2.f, y - 2.f, r.width - 2.f * pad + 4.f, rowH, clear,
                              3.f, 1.5f, jf::Colors::Accent);
            break;
        }
    }
}

REGISTER_WIDGET("triggerdiagram", TriggerDiagramWidget, 150);

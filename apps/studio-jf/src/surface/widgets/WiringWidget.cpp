// WiringWidget — renders the wiring-row skin (display only for now): Assign / Clear buttons, the
// assigned resource name, a drawn wire in its colour, and the colour code + connector pin.

#include "WiringWidget.h"
#include <algorithm>
#include <cmath>
#include "WireColors.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"
#include "../../model/TableImage.h"
#include "../../model/UnitManager.h"
#include <array>
#include <cstdlib>
#include <string>

namespace {
// Thin aliases onto the shared wire palette (WireColors.h) — one source for the widget and the dialog.
using RGBA = wirecol::RGBA;
inline RGBA shellColorFor(const std::string& name) { return wirecol::shell(name); }
}  // namespace


// Does this wiring row belong to a switch being read through an analog pin? That is the ONE question
// the threshold fields are gated on, and it has two halves the firmware also asks separately (see
// PipelineBuilder): WHAT the sensor is, and HOW it is read. Asking only the second is what let a
// switch on an analog pin be decoded through the cal curve for as long as it was.
WiringWidget::Thresh WiringWidget::_thresh(const Cache& c) const {
    Thresh t;
    // The RESOLVED path, not the raw binding: a widget binds through a sigil ("[#sensors.sensor[*]
    // .source]") whose first bracket is the sigil's own, and whose element index may be the page's
    // "[*]" rather than a slot. writableConfigPath is what every other widget resolves through.
    const std::string bind = writableConfigPath(bindPath());
    const MetaModel* meta = c.meta();
    // The row binds the slot's `source` field: "sensors.sensor[clt].source". Anything else — an
    // unbound placeholder, a wiring row on some other array — has no slot to ask about.
    const size_t lb = bind.find('['), rb = bind.find(']');
    if (!meta || lb == std::string::npos || rb == std::string::npos || rb < lb) return t;
    const std::string arrayKey = bind.substr(0, lb);
    const std::string elemKey  = bind.substr(lb + 1, rb - lb - 1);
    if (arrayKey != "sensors.sensor") return t;

    // The element key may be a catalog id ("clt") or a bare index; resolve it the way the paths do.
    const auto ai = meta->configArrays().find(arrayKey);
    if (ai == meta->configArrays().end()) return t;
    int idx = -1;
    for (size_t i = 0; i < ai->second.elementIds.size(); ++i)
        if (ai->second.elementIds[i] == elemKey) { idx = static_cast<int>(i); break; }
    if (idx < 0 && !elemKey.empty() && (elemKey[0] >= '0' && elemKey[0] <= '9'))
        idx = std::atoi(elemKey.c_str());
    if (idx < 0 || idx >= ai->second.count) return t;

    // WHAT it is: the catalog's type, or for a generic input the type the tune has it set to — the
    // same precedence the firmware's effective_type() applies.
    const MetaModel::SensorType* st = c.sensorTypeOf(arrayKey, idx);
    if (!st || st->id != "switch") return t;
    // HOW it is read: by option ID, not by the enum's position — the ids are what the schema names
    // and a reordered options list would silently repoint a hardcoded number.
    const std::string iface = c.elementOption(arrayKey, idx, "interface");
    if (iface != "analog_voltage" && iface != "engine_sync_voltage") return t;

    // The trip points are the first two BREAKPOINTS of the slot's calibration — an element table, so
    // its axis is addressed through the resolved table image rather than as a path of its own (there
    // is no path form for a single breakpoint). This is the same descriptor the curve editor edits
    // through, which is what keeps the two views of these bytes in step.
    const TableImage ti = c.resolveTable(bind.substr(0, rb + 1) + ".cal");
    if (!ti.valid || ti.axes.empty()) return t;
    const TableImage::Axis& ax = ti.axes[0];
    if (ax.breakSize <= 0 || ax.nMax < 2) return t;
    t.offset = ax.breaksBase; t.stride = ax.breakSize; t.datatype = ax.breakType;

    // Displayed in whatever the user reads raw analog inputs in — counts, mV or volts (the default).
    // The axis is stored in ADC counts, and AnalogRaw is the quantity that converts them.
    UnitManager& um = UnitManager::instance();
    t.unit      = um.preferredUnit("AnalogRaw");
    if (t.unit.empty()) t.unit = "ADC";
    t.unitLabel = um.unitLabel(t.unit);
    t.perCount  = um.convert(1.0, "ADC", t.unit);
    if (t.perCount <= 0.0) t.perCount = 1.0;
    t.show = true;
    return t;
}

// Push the stored breakpoints into the boxes, creating them on first use. The guard is the same one
// HostedControlWidget holds for the same reason: a spin box emits onValueChanged from its own setters,
// so a programmatic set would otherwise write itself straight back to the tune.
void WiringWidget::_syncBoxes(const Thresh& t) {
    Cache& C = Cache::instance();
    auto make = [&](std::unique_ptr<jf::JDoubleSpinBox>& box, int off) {
        if (box) return;
        // Range and precision follow the DISPLAY unit: 0..5 V at two decimals, or 0..4095 counts at
        // none. A box that offers six decimals on an integer count is offering a precision the
        // storage does not have.
        const double hi   = 65535.0 * t.perCount;
        const int    dec  = std::max(0, UnitManager::instance().unitDigits(t.unit));   // the unit's own precision, not a guess here
        const double step = (t.unit == "ADC") ? 1.0 : 0.05;
        box = std::make_unique<jf::JDoubleSpinBox>(sceneGraph(), 0.0, hi, step, dec, 70.f, 22.f);
        box->setFocusPolicy(jf::JFocusPolicy::NoFocus);   // the canvas is its own focus domain
        box->onValueChanged.connect([this, off](double v) {
            if (m_syncing) return;
            // Back to counts on the way out — the axis is stored raw, exactly as the firmware reads it.
            Cache::instance().writeAt(off, m_dtype, 1.0, v / m_perCount);
        });
    };
    m_dtype = t.datatype; m_perCount = t.perCount;
    make(m_off, t.offset);
    make(m_on,  t.offset + t.stride);
    m_syncing = true;
    m_off->setValue(C.readAt(t.offset,             t.datatype, 1.0) * t.perCount);
    m_on ->setValue(C.readAt(t.offset + t.stride,  t.datatype, 1.0) * t.perCount);
    m_syncing = false;
}

// A box's value back in the stored units (ADC counts), which is what the firmware reads.
double WiringWidget::threshRawForTest(bool on) const {
    const jf::JDoubleSpinBox* b = on ? m_on.get() : m_off.get();
    return (b && m_perCount > 0.0) ? b->value() / m_perCount : 0.0;
}

// Is this row's CURRENT pin one the board will never release? `assignable: false` pins (the battery
// sense on AV12, the knock inputs) are listed so a reader can recognise them and refused everywhere
// they could be chosen — so a Clear button beside one offers to break it and nothing can put it back.
static bool _fixedHere(const std::string& bind) {
    const MetaModel* m = Cache::instance().meta();
    if (!m || bind.empty()) return false;
    const std::string path = CanvasWidget::writableConfigPath(bind);
    if (path.empty()) return false;
    return m->pickerValueFixed(path, static_cast<int>(std::lround(Cache::instance().configValue(path))));
}

void WiringWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;
    // The wiring row lays out in what is LEFT after the threshold band, so the buttons, the wire and
    // the pin badge keep their proportions instead of being squashed by a row that is not always there.
    const Thresh th = _thresh(c);
    const float H = th.show ? (r.height - kThreshH) : r.height;

    // Two cases, kept strictly apart so a CLEARED assignment never shows the placeholder props:
    //   • No Data Source  -> authored props (a design-time placeholder while laying out the panel).
    //   • Bound field      -> the resolved resource + its connector wiring; when unassigned/cleared the
    //                         name is "(none)" and there is NO wire or pin (the props do not leak in).
    std::string name, colorCode, connPin, shellName;
    if (bindPath().empty()) {
        name = m_name; colorCode = m_wireColor; connPin = m_connPin;
    } else {
        name = resolveLabel(c);
        if (const MetaModel* meta = c.meta()) {
            std::string p, col;
            if (meta->wiringTrim(name, p, col)) {
                connPin = p; colorCode = col;
                // The terminal names its connector ("CN3-26" -> CN3); its shell colour is that connector's.
                const auto dash = p.find('-');
                if (dash != std::string::npos) {
                    auto it = meta->connectors().find(p.substr(0, dash));
                    if (it != meta->connectors().end()) shellName = it->second.color;
                }
            }
        }
    }

    // --- Assign / Clear buttons (left column, stacked) -----------------------
    // …unless the pin is FIXED, in which case there is nothing to choose and nothing to release, and a
    // button that cannot act must not look like one. The row still shows the pin — that is the point.
    const bool fixedPin = _fixedHere(bindPath());
    static const RGBA kAssign{ 0x2f, 0x6f, 0xd6, 255 }, kClear{ 0x3a, 0x3a, 0x3a, 255 },
                      kBtnTxt{ 0xff, 0xff, 0xff, 255 };
    const float bx = r.x + 4.f, bw = 58.f, bh = (H - 10.f) * 0.5f;
    const float ay = r.y + 4.f, cy = ay + bh + 2.f;
    if (!fixedPin) {
        buf.pushRectangle(bx, ay, bw, bh, kAssign.data(), 4.f);
        buf.pushRectangle(bx, cy, bw, bh, kClear.data(),  4.f);
        if (jf::JTextHelper::hasAtlas()) {
            const float th = jf::JTextHelper::lineHeight();
            jf::JTextHelper::pushText(buf, bx + 10.f, ay + (bh - th) * 0.5f, "Assign", kBtnTxt.data());
            jf::JTextHelper::pushText(buf, bx + 12.f, cy + (bh - th) * 0.5f, "Clear",  kBtnTxt.data());
        }
    } else if (jf::JTextHelper::hasAtlas()) {
        // SAY WHY the buttons are not there. A blank column reads as a page that failed to draw.
        static const RGBA kDim{ 0x8a, 0x8f, 0x98, 255 };
        const float th = jf::JTextHelper::lineHeight();
        jf::JTextHelper::pushText(buf, bx + 2.f, ay + (bh - th) * 0.5f, "fixed", kDim.data());
        jf::JTextHelper::pushText(buf, bx + 2.f, cy + (bh - th) * 0.5f, "input", kDim.data());
    }

    // --- Resource name (above the wire) --------------------------------------
    uint8_t fgb[4];
    const uint8_t* fg = fgOf(el, fgb);
    const float wireX = bx + bw + 16.f;
    if (jf::JTextHelper::hasAtlas())
        jf::JTextHelper::pushText(buf, wireX + 10.f, r.y + 4.f, name, fg);

    // --- Wire bar + colour code + connector pin (below the name) -------------
    // A bar of insulation in the wire colour (no terminal stub — that read as clutter), then the [code]
    // text, then the connector-pin ref (CN3-26) REVERSE-VIDEO: a filled badge in the CONNECTOR's shell
    // colour (CN3 = blue), text in the contrasting shade. Unbound / no connector -> plain foreground text.
    const float wy = r.y + H * 0.62f;
    float wireEnd = wireX + 10.f;
    if (!colorCode.empty()) { wirecol::drawWire(buf, wireX + 10.f, wy, 30.f, colorCode); wireEnd += 30.f + 8.f; }
    if (jf::JTextHelper::hasAtlas()) {
        const float th = jf::JTextHelper::lineHeight();
        const float ty = wy - th * 0.5f;
        float tx = wireEnd;
        if (!colorCode.empty()) {   // "[G/W]" (+ ", " only when a pin follows) — nothing when cleared
            const std::string head = "[" + colorCode + "]" + (connPin.empty() ? "" : ", ");
            jf::JTextHelper::pushText(buf, tx, ty, head, fg);
            tx += jf::JTextHelper::measureWidth(head);
        }
        if (!connPin.empty()) {
            if (shellName.empty()) {
                jf::JTextHelper::pushText(buf, tx, ty, connPin, fg);
            } else {
                const RGBA sh = shellColorFor(shellName);
                connPin = wirecol::terminal(connPin, shellName);   // "CN3-25" -> "CN3 (BLUE) 25"
                const float pad = 5.f, pw = jf::JTextHelper::measureWidth(connPin);
                static const RGBA kBorder{ 0x90, 0x90, 0x90, 255 };
                buf.pushRectangle(tx, ty - 3.f, pw + pad * 2.f, th + 6.f, sh.data(), 3.f, 1.f, kBorder.data());
                // Contrasting text: light shells (white/yellow) get black ink, dark shells get white.
                const int lum = (299 * sh[0] + 587 * sh[1] + 114 * sh[2]) / 1000;
                static const RGBA kBlack{ 0, 0, 0, 255 }, kWhite{ 255, 255, 255, 255 };
                jf::JTextHelper::pushText(buf, tx + pad, ty, connPin, (lum > 140 ? kBlack : kWhite).data());
            }
        }
    }

    // --- the two trip points, when this is a switch on an analog pin ---------
    // Label above box, in a row under the wiring, which is where the connection's own numbers belong:
    // "what voltage does this wire have to reach" is a question about the wire.
    if (th.show) {
        _syncBoxes(th);
        const float ty = r.y + H;
        const float lh = jf::JTextHelper::hasAtlas() ? jf::JTextHelper::lineHeight() : 10.f;
        const float boxY = ty + lh + 1.f;
        // TWO COLUMNS OF THIS WIDGET'S OWN WIDTH, not a hardcoded 132px offset. At 206 wide — which is
        // what the outputs page gives it — the second column started at bx+132 and "Switch Off (V)"
        // ran past the right edge, so it drew as "Switch Off (". The widget knows how wide it is; the
        // page should not have to be built around a constant buried in here.
        const float inset = bx - r.x;
        const float colW  = std::max(74.f, (r.width - inset * 2.f) * 0.5f);
        const float x0 = bx, x1 = bx + colW;
        if (jf::JTextHelper::hasAtlas()) {
            const std::string u = th.unitLabel.empty() ? std::string() : " (" + th.unitLabel + ")";
            // Capped to the column, so a longer unit truncates instead of overrunning its neighbour.
            jf::JTextHelper::pushText(buf, x0, ty, "Switch On" + u, fg, colW - 4.f);
            jf::JTextHelper::pushText(buf, x1, ty, "Switch Off" + u, fg, colW - 4.f);
        }
        const float boxW = std::min(70.f, colW - 4.f);
        m_on ->setBounds({ x0, boxY, boxW, 22.f });
        m_off->setBounds({ x1, boxY, boxW, 22.f });
        m_on ->populateRenderPrimitives(buf);
        m_off->populateRenderPrimitives(buf);
    }
}

std::function<void(std::string)> WiringWidget::onAssign;

bool WiringWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    // EVERYTHING THAT IS NOT A PRESS belongs to the box the last press picked. Without this the widget
    // answered presses only, so a trip-point box could be clicked into and then never typed in — the
    // keys, the drag and the blur that commits the edit all arrived and were dropped on the floor.
    if (in.kind != ControlInput::Kind::Press) {
        if (!m_active) return false;
        switch (in.kind) {
            case ControlInput::Kind::Move:    m_active->handleMouseMove(in.mx, in.my);    return true;
            case ControlInput::Kind::Release: m_active->handleMouseRelease(in.mx, in.my); return true;
            case ControlInput::Kind::Key:     return in.key && m_active->handleKeyEvent(*in.key);
            // Focus left the element entirely: commit the edit and drop the caret. A hosted control is
            // not in the window's focus chain, so no real blur ever reaches it on its own.
            case ControlInput::Kind::Blur:    m_active->endEdit(); m_active->setFocused(false);
                                              m_active = nullptr;                          return true;
            default:                                                                       return false;
        }
    }
    // Same geometry as render(): the CONTENT rect (element minus padding), and the wiring row laid out
    // above the threshold band when there is one — so the buttons are hit where they are drawn.
    const jf::JRect r = contentRect(screen);
    const float H = _thresh(Cache::instance()).show ? (r.height - kThreshH) : r.height;
    const float bx = r.x + 4.f, bw = 58.f, bh = (H - 10.f) * 0.5f;
    const float ay = r.y + 4.f, cy = ay + bh + 2.f;
    auto hit = [&](float x, float y, float w, float h) {
        return in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
    };
    const std::string bind = bindPath();
    if (bind.empty()) return false;
    // NEITHER BUTTON DOES ANYTHING FOR A FIXED PIN, so neither answers. Assign would open a dialog
    // that refuses every row it could offer, and Clear would unassign a pin nothing may pick again.
    if (!_fixedHere(bind)) {
        if (hit(bx, ay, bw, bh)) { if (onAssign) onAssign(bind); return true; }    // Assign -> dialog
        if (hit(bx, cy, bw, bh)) { Cache::instance().setConfigValue(bind, -1.0); return true; }  // Clear
    }
    // A press in one of the trip-point boxes goes to that box. The Surface hands this element ALL its
    // input and tracks one active control per element, so a widget hosting two of them routes its own:
    // the press picks which, and the keys that follow go to the one the press picked.
    for (jf::JControl* box : { static_cast<jf::JControl*>(m_on.get()),
                               static_cast<jf::JControl*>(m_off.get()) }) {
        if (!box) continue;
        const jf::JRect b = box->bounds();
        if (!hit(b.x, b.y, b.width, b.height)) continue;
        if (m_active && m_active != box) { m_active->endEdit(); m_active->setFocused(false); }
        m_active = box;
        box->setFocused(true);
        box->handleMousePress(in.mx, in.my);
        return true;
    }
    return false;
}

// Self-registration — type key, palette order (after the enum picker at 100), factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("wiring", WiringWidget, 101);

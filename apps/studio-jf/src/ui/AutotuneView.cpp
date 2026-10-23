#include "AutotuneView.h"
#include "WrapText.h"

#include "../model/Cache.h"

#include <j/config/Settings.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jf;

namespace {

constexpr float kPad    = 6.f;
constexpr float kTitleH = 18.f;
constexpr char  kUid[]  = "autotune-grid";   // the table's identity: its cursor and scroll state key off it

struct Rgba { uint8_t v[4]; };

Rgba mix(const uint8_t a[4], const uint8_t b[4], double t) {
    t = std::clamp(t, 0.0, 1.0);
    Rgba o{};
    for (int i = 0; i < 4; ++i)
        o.v[i] = static_cast<uint8_t>(a[i] + (b[i] - a[i]) * t);
    return o;
}

// A DIVERGING scale, because the quantity diverges. A change of zero is not "the bottom of a range",
// it is the middle — the cell is right — so it must read as the background does, and the two
// directions must be told apart at a glance rather than by their number's sign.
Rgba changeColour(double pct, double full) {
    static const uint8_t neutral[4] = { 52, 52, 56, 255 };
    static const uint8_t add[4]     = { 210, 70, 60, 255 };    // more fuel
    static const uint8_t take[4]    = { 60, 120, 210, 255 };   // less fuel
    const double t = full > 0.0 ? std::min(1.0, std::fabs(pct) / full) : 0.0;
    return mix(neutral, pct >= 0.0 ? add : take, t);
}

// A plain sequential ramp for a VE surface, the same cold-to-hot reading a fuel map is normally shown
// with, so Base and Proposed can be flicked between and compared.
Rgba heatColour(double v, double lo, double hi) {
    static const uint8_t cold[4] = { 46, 62, 110, 255 };
    static const uint8_t mid[4]  = { 60, 140, 110, 255 };
    static const uint8_t hot[4]  = { 200, 90, 60, 255 };
    if (!(hi > lo)) return Rgba{ { mid[0], mid[1], mid[2], mid[3] } };
    const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
    return t < 0.5 ? mix(cold, mid, t * 2.0) : mix(mid, hot, (t - 0.5) * 2.0);
}

// Rounding noise below a twentieth of a percent is left blank, as an untouched cell is.
double sessionShown(const autotune::Engine& e, int c, int r) {
    const double sp = e.sessionPct(c, r);
    return std::fabs(sp) < 0.05 ? 0.0 : sp;
}

}  // namespace

// ---- the cells -----------------------------------------------------------------------------------------
// Storage indices are the engine's: i0 (axis 0) is its column, i1 (axis 1) its row.

double AutotuneView::Source::value(int i0, int i1) const {
    const autotune::Engine& e = *v_.eng_;
    switch (v_.mode_) {
        case Mode::Base:     return e.base(i0, i1);
        case Mode::Proposed: return e.proposed(i0, i1);
        case Mode::Weight:   return e.weight(i0, i1);
        case Mode::Change:   return e.changePct(i0, i1);
        case Mode::Session:  return sessionShown(e, i0, i1);
    }
    return 0.0;
}

bool AutotuneView::Source::fill(int i0, int i1, uint8_t out[4]) const {
    const Scales& s = v_.scales_;
    Rgba c{};
    switch (v_.mode_) {
        case Mode::Base: case Mode::Proposed: c = heatColour(value(i0, i1), s.lo, s.hi); break;
        case Mode::Weight: {
            static const uint8_t none[4] = { 34, 34, 38, 255 }, some[4] = { 70, 160, 110, 255 };
            c = mix(none, some, s.fullW > 0.0 ? value(i0, i1) / (s.fullW * 2.0) : 0.0);
            break;
        }
        case Mode::Change:  c = changeColour(value(i0, i1), s.authority);  break;
        case Mode::Session: c = changeColour(value(i0, i1), s.sessionMax); break;
    }
    std::copy(c.v, c.v + 4, out);
    return true;
}

bool AutotuneView::Source::label(int i0, int i1, std::string& out) const {
    const double v = value(i0, i1);
    char b[24] = "";
    switch (v_.mode_) {
        case Mode::Base: case Mode::Proposed: std::snprintf(b, sizeof b, "%.1f", v); break;
        case Mode::Weight:                    if (v > 0.0) std::snprintf(b, sizeof b, "%.1f", v); break;
        case Mode::Change: case Mode::Session: if (v != 0.0) std::snprintf(b, sizeof b, "%+.1f", v); break;
    }
    out = b;
    return true;
}

float AutotuneView::Source::coverage(int i0, int i1) const {
    const double w = v_.eng_->weight(i0, i1), full = v_.scales_.fullW;
    return (w > 0.0 && full > 0.0) ? static_cast<float>(std::min(1.0, w / full)) : -1.f;
}

bool AutotuneView::Source::marked(int i0, int i1) const {
    return v_.eng_->lastCol() == i0 && v_.eng_->lastRow() == i1;
}

// Signed modes are read about zero, so a cell that took fuel away sinks in 3D and hangs below the
// middle line in a slice rather than standing on the floor like a small addition.
bool AutotuneView::Source::range(double& lo, double& hi) const {
    const Scales& s = v_.scales_;
    switch (v_.mode_) {
        case Mode::Base: case Mode::Proposed: lo = s.lo; hi = s.hi; return true;
        case Mode::Weight:  lo = 0.0; hi = s.fullW * 2.0; return true;
        case Mode::Change:  lo = -s.authority;  hi = s.authority;  return true;
        case Mode::Session: lo = -s.sessionMax; hi = s.sessionMax; return true;
    }
    return false;
}

std::string AutotuneView::Source::units() const {
    switch (v_.mode_) {
        case Mode::Change: case Mode::Session: return "% change";
        case Mode::Weight:                     return "records";
        default:                               return {};   // the table's own
    }
}

void AutotuneView::computeScales() {
    Scales s;
    const autotune::Engine& e = *eng_;
    const int rows = e.rows(), cols = e.cols();
    // The value range Base/Proposed are shaded against — taken over BOTH so the two modes share one
    // scale. Shading each against its own range would make an unchanged map and a rewritten one look
    // exactly alike, which is the one comparison this view exists to support.
    double lo = 1e300, hi = -1e300;
    if (e.hasBase())
        for (int rr = 0; rr < rows; ++rr)
            for (int cc = 0; cc < cols; ++cc)
                for (double v : { e.base(cc, rr), e.proposed(cc, rr) }) { lo = std::min(lo, v); hi = std::max(hi, v); }
    if (lo < hi) { s.lo = lo; s.hi = hi; }
    s.fullW = autotune::fullWeightOf(e.settings().resistance);
    // THE COLOUR SCALE IS THE RUN'S OWN, not the authority limit. Shading a 6 % change against a 50 %
    // ceiling paints it almost the same grey as an untouched cell — the map looked empty on a bench run
    // that had in fact found exactly what it was set up to find. So the strongest change in the
    // proposal is full colour and everything else is read against it, with a floor so that a map which
    // is nearly right does not get its rounding errors painted like a discovery.
    s.authority = std::max(2.0, e.stats().maxChange);
    // The session has its own scale for the same reason: the biggest move so far is full colour.
    for (int rr = 0; rr < rows; ++rr)
        for (int cc = 0; cc < cols; ++cc) s.sessionMax = std::max(s.sessionMax, std::fabs(e.sessionPct(cc, rr)));
    scales_ = s;
}

// ---- the widget -----------------------------------------------------------------------------------------

AutotuneView::AutotuneView(JSceneGraph& g) : JWidget(g, "AutotuneView") {
    table_ = adopt(std::make_unique<TableWidget>(g));
    table_->setCache(&Cache::instance());
    table_->setCellSource(&source_);
    // The same three views, by the same names, as a table's Display ▸ View.
    static const char* const kViews[3] = { "2D Grid", "3D Surface", "Slice" };
    for (int i = 0; i < 3; ++i) {
        viewItems_[i] = menu_.add(g, kViews[i]);
        viewItems_[i]->setCheckable(true);
        viewItems_[i]->onTriggered.connect([this, i] { setLayout(static_cast<Layout>(i)); });
    }
    setContextMenu(&menu_);
}

void AutotuneView::setTable(const std::string& path) {
    if (path == tablePath_) return;
    tablePath_ = path;
    PanelElement el;
    el.type = "table";
    el.uid  = kUid;
    el.props["signalName"] = path;
    // The grid fills the tab, as it did before it was a table: rows and columns stretch.
    el.props["hSectionMode"] = "2";
    el.props["vSectionMode"] = "2";
    if (const int v = JSettings::instance().get<int>("autotune.view", 0); v > 0 && v <= 2)
        el.props["view"] = std::to_string(v);
    table_->setSource(el);
    table_->loadContent(*table_->element());
    invalidate();
}

void AutotuneView::setLayout(Layout l) {
    table_->setView(static_cast<int>(l));
    table_->loadContent(*table_->element());   // the view is read from the element; refresh the member
    JSettings::instance().set("autotune.view", JVariant(static_cast<int>(l)));
    invalidate();
}

bool AutotuneView::ready() const {
    return eng_ && eng_->cols() >= 1 && eng_->rows() >= 1 && notice_.empty() && !tablePath_.empty();
}

JRect AutotuneView::tableRect() const {
    const JRect r = bounds();
    return { r.x, r.y + kTitleH, r.width, std::max(0.f, r.height - kTitleH) };
}

void AutotuneView::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    const JRect r = bounds();
    buf.pushRectangle(r.x, r.y, r.width, r.height, Colors::Surface0);
    if (!JTextHelper::hasAtlas()) return;

    if (!ready()) {
        const std::string msg = notice_.empty() ? "No table to tune" : notice_;
        const float mh = wraptext::height(msg, r.width - 16.f);   // wrapped, centred as a block
        wraptext::drawCentred(buf, r.x + 8.f, r.y + (r.height - mh) * 0.5f, r.width - 16.f, msg,
                              Colors::TextSecondary);
        return;
    }

    // ---- the title line: what is being shown ---------------------------------------------------------
    const char* modeName = mode_ == Mode::Base     ? "Base"
                         : mode_ == Mode::Proposed ? "Proposed"
                         : mode_ == Mode::Weight   ? "Records per cell"
                         : mode_ == Mode::Session  ? "Session %  (since Start / Reset)"
                                                   : "Change %";
    JTextHelper::pushText(buf, r.x + kPad, r.y + 2.f, modeName, Colors::TextSecondary, r.width - kPad * 2.f);

    computeScales();
    TableWidget::tableSetPlane(kUid, plane_);
    table_->setBounds(tableRect());
    table_->populateRenderPrimitives(buf);
}

// ---- input: the table's own, delivered the way a surface delivers it ----------------------------------
namespace {
ControlInput at(ControlInput::Kind k, float mx, float my) {
    ControlInput in; in.kind = k; in.mx = mx; in.my = my; in.focused = true;
    return in;
}
}  // namespace

// Moves go to the table only while a button is held, as a surface sends them (Surface::handleMouseMove,
// runDrag_): the table treats any Move as part of a drag, so a bare hover would orbit the 3D view.
void AutotuneView::handleMousePress(float mx, float my) {
    pressed_ = true;
    if (ready() && table_->onControlInput(tableRect(), at(ControlInput::Kind::Press, mx, my))) invalidate();
}
void AutotuneView::handleMouseMove(float mx, float my) {
    if (pressed_ && ready() && table_->onControlInput(tableRect(), at(ControlInput::Kind::Move, mx, my))) invalidate();
}
void AutotuneView::handleMouseRelease(float mx, float my) {
    if (!pressed_) return;
    pressed_ = false;
    if (ready() && table_->onControlInput(tableRect(), at(ControlInput::Kind::Release, mx, my))) invalidate();
}
bool AutotuneView::handleScroll(float mx, float my, float wheel) {
    if (!ready()) return false;
    ControlInput in = at(ControlInput::Kind::Scroll, mx, my);
    in.wheel = wheel;
    const bool used = table_->onControlInput(tableRect(), in);
    if (used) invalidate();
    return used;
}
bool AutotuneView::handleKeyEvent(const JKeyEvent& e) {
    if (!ready()) return false;
    ControlInput in = at(ControlInput::Kind::Key, 0.f, 0.f);
    in.key = &e;
    const bool used = table_->onControlInput(tableRect(), in);
    if (used) invalidate();
    return used;
}

void AutotuneView::prepareContextMenu(float, float) {
    const int v = table_->view();
    for (int i = 0; i < 3; ++i) viewItems_[i]->setChecked(v == i);
}

#pragma once

// EngineCycleView — the centre tool tab that draws one engine cycle in DEGREES.
//
// One lane per trace (each coil, each injector, the trigger inputs), laid against a crank-angle axis
// that spans the configured cycle: 360 (two-stroke), 720 (four-stroke) or 1080 (rotary, where the
// three rotor faces are distinct positions rather than overlays). A coil's high span IS its dwell,
// and the span's right-hand edge is the spark; an injector's is the open period.
//
// Opened via SurfaceTabs::openTool, exactly as the Trigger Designer is — the centre area is the only
// place wide enough to put 720 or 1080 degrees against a dozen-plus lanes without either scrolling
// or compressing it into uselessness.
//
// It draws the NATIVE model (enginecycle::Cycle) and nothing else. A foreign ECU is converted into
// that at the comms boundary, so there is no protocol knowledge here — and because a converter marks
// data it had to interpolate, the header can say which it is showing rather than presenting a
// reconstructed angle as a measured one.

#include <j/core/JWidget.h>
#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/EngineCycle.h"

#include <algorithm>
#include <cstdio>
#include <string>

class EngineCycleView : public jf::JWidget {
public:
    explicit EngineCycleView(jf::JSceneGraph& g) : jf::JWidget(g, "EngineCycleView") {}

    // Replace the displayed cycle. Cheap enough to call per captured cycle — the view holds a copy so
    // the producer is free to reuse its buffer.
    void setCycle(const enginecycle::Cycle& c) { cycle_ = c; cycle_.sortTracesForDisplay(); invalidate(); }
    const enginecycle::Cycle& cycle() const { return cycle_; }
    void clear() { cycle_.clear(); invalidate(); }

    // What to say when there are no traces: "Capturing…", "ECU not connected", the error from a
    // failed capture. Without this the empty view reads as "the engine did nothing", which is a
    // different and much more alarming statement than "we have not asked it yet".
    void setStatus(std::string s) { status_ = std::move(s); invalidate(); }

    // A banner shown ALONGSIDE the frames, not instead of them. setStatus only renders on an empty
    // view, so a panel holding real frames could stop updating with nothing on screen saying why —
    // and a paused capture looks exactly like a live one that happens not to be changing. That is
    // the case when the trigger log takes the decoder away: the last measured cycle stays correct
    // and stops being current, and only the transport button hints at it.
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

    // Hover readout. A bar's ANGLES are the whole reason the view exists, but reading them off a
    // 720-degree axis by eye is a guess — the difference between 28 and 29 degrees of dwell is not
    // visible at this scale and is exactly the sort of thing a tuner is checking. So hovering a span
    // states it: where it starts, where it ends, how long in degrees, and how long in MILLISECONDS,
    // which is the unit dwell and injector pulse width are actually configured in.
    void handleMouseMove(float mx, float my) override;

private:
    // THE READOUT IS DRAWN HERE, not left to the framework's tooltip. A tooltip is dwell-based: it
    // appears once the pointer has been STILL for tooltipDelayMs and any real movement takes it away
    // again. That is right for a label on a button and wrong for a positional readout — you move
    // ALONG a bar to read it, which is exactly the gesture that keeps dismissing it, so the numbers
    // were there all along and effectively unreachable. This one appears immediately and tracks.
    // The framework tooltip is deliberately NOT set alongside: two boxes for one readout overlap into
    // what looks like a single 3-D outline.
    void layOut();      // fill plotRect_/lanes_ — geometry only, no drawing (see the .cpp)
    void drawReadout(jf::JPrimitiveBuffer& buf, const jf::JRect& r) const;
    void setReadout(const std::string& text, float ax, float ay);

    std::string hoverText_;              // "" = nothing under the cursor
    float       hoverX_ = 0.f, hoverY_ = 0.f;   // absolute, where the box is anchored

public:
    // What the cursor is over, "" for nothing. Exposed because this view draws its own readout, so
    // this string — not tooltip() — is the observable a test can hold the hover contract to, and it
    // is reachable without a font atlas.
    [[nodiscard]] const std::string& readout() const { return hoverText_; }

private:

    // Geometry constants — kept together so the layout reads in one place.
    static constexpr float kLaneH    = 22.f;   // per-trace lane height
    static constexpr float kLaneGap  = 4.f;
    static constexpr float kLabelW   = 120.f;  // left gutter for trace names
    static constexpr float kHeaderH  = 44.f;   // caption + axis labels
    static constexpr float kPadX     = 12.f;
    static constexpr float kPadY     = 8.f;

    // The header grows by the lines the notice and the capture's note WRAP to; the plot starts below.
    float headerH(float w) const;
    void drawHeader(jf::JPrimitiveBuffer& buf, const jf::JRect& r);
    void drawGrid(jf::JPrimitiveBuffer& buf, const jf::JRect& plot);
    void drawTrace(jf::JPrimitiveBuffer& buf, const jf::JRect& plot,
                   const enginecycle::Trace& t, float y);

    // Degrees → x within the plot area.
    float xOf(const jf::JRect& plot, double deg) const {
        const double span = cycle_.cycleAngle() > 0.0 ? cycle_.cycleAngle() : 720.0;
        return plot.x + static_cast<float>(plot.width * (deg / span));
    }

    // Where each trace was drawn last frame, so a hover can be resolved to a lane without
    // recomputing the layout (and without the two disagreeing when the layout changes).
    struct LaneRect { const enginecycle::Trace* trace; float y, h; };
    std::vector<LaneRect> lanes_;
    jf::JRect             plotRect_{};

    // Degrees -> milliseconds at the capture's own rpm. Returns 0 when the rpm is unknown, and the
    // readout then omits the time rather than printing a confident zero.
    [[nodiscard]] double msPerDegree() const {
        const double rpm = cycle_.rpm();
        return (rpm > 0.0) ? (60000.0 / (rpm * 360.0)) : 0.0;
    }

    enginecycle::Cycle cycle_;
    std::string        status_ = "No cycle captured";
    std::string        notice_;
};

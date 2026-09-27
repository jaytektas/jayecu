#pragma once

// TriggerLogView — the trigger log drawn as what it is: the time between edges.
//
// One bar per edge, its height the microseconds since the previous edge on that stream, laid out in
// capture order. The proportions ARE the picture — a 36-1 draws thirty-five short bars and one
// double — and reading it is the user's job, not the tool's.
//
// THE X AXIS IS AN ORDINAL, NOT AN ANGLE. There is no angle in a trigger log and none can be
// computed: edges/second is (features per revolution) x (revolutions per second), one equation with
// two unknowns, so a one-cylinder distributor at a million rpm and a million-cylinder one at 1 rpm
// give identical captures. This view therefore has no degree axis, no rpm, and no notion of a
// revolution — deliberately, because every one of those would be an invention presented at the same
// confidence as the measurement beside it.
//
// It is the exact complement of EngineCycleView, which draws DEGREES for a wheel the decoder
// resolves. That one is empty without sync; this one is refused with it.

#include <j/core/JWidget.h>
#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/TriggerLog.h"

#include <string>
#include <vector>

class TriggerLogView : public jf::JWidget {
public:
    explicit TriggerLogView(jf::JSceneGraph& g) : jf::JWidget(g, "TriggerLogView") {}

    void setLanes(std::vector<triggerlog::Lane> l) { lanes_ = std::move(l); invalidate(); }
    [[nodiscard]] const std::vector<triggerlog::Lane>& lanes() const { return lanes_; }
    void clear() { lanes_.clear(); invalidate(); }

    // What to say when there is nothing to draw — waiting for edges, an error from the ECU. An
    // empty plot with no caption reads as "the sensor is dead", which is a different and much more
    // alarming statement than "we have not started yet".
    // A one-line notice across the header — the capture ending, chiefly. It goes here rather than in
    // the transport's counter because that label is flexed down to a few dozen pixels and silently
    // truncates: "buffer full — Start to clear and run again" arrived on screen as "b".
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void setStatus(std::string s) { status_ = std::move(s); invalidate(); }

    // The window into the capture: `first` bar and how many. A cranking session is thousands of
    // bars and the screen is hundreds, so what is on screen is a decision, not a fit.
    void setWindow(size_t first, size_t count) { first_ = first; count_ = count; invalidate(); }
    [[nodiscard]] size_t windowCount() const { return count_; }

    // Total bars in the longest lane — what the window is scrolled within.
    [[nodiscard]] size_t barCount() const {
        size_t n = 0;
        for (const auto& l : lanes_) if (l.bars.size() > n) n = l.bars.size();
        return n;
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    void handleMouseMove(float mx, float my) override;

private:
    static constexpr float kLaneGap = 10.f;
    static constexpr float kLabelW  = 110.f;
    // TWO LINES. The description sits on the first, and the second carries the hover readout or a
    // notice. At 40 it only covered one, so everything drawn at y+16 landed INSIDE the plot and
    // overprinted the first lane's axis labels — the hover always did this, and the capture-complete
    // notice joined it.
    static constexpr float kHeaderH = 56.f;
    // The header grows by the lines its two sentences WRAP to (the axis legend, the notice).
    float headerH(float w) const;
    static const char* axisLegend();
    static constexpr float kPadX    = 12.f;
    static constexpr float kPadY    = 8.f;
    static constexpr float kMinBarW = 1.f;

    void drawHeader(jf::JPrimitiveBuffer& buf, const jf::JRect& r);
    void drawLane(jf::JPrimitiveBuffer& buf, const jf::JRect& plot,
                  const triggerlog::Lane& lane, float y, float h,
                  uint32_t t0, uint32_t t1, size_t refBars);

    // Where each lane was drawn last frame, so a hover resolves to a bar without recomputing the
    // layout (and without the two disagreeing when it changes).
    //
    // An INDEX, not a pointer. It held `const Lane*` into lanes_, and setLanes() replaces that whole
    // vector on every poll — five times a second while streaming — so any mouse movement between a
    // poll and the next repaint dereferenced freed storage. That is a use-after-free reachable by
    // doing nothing but moving the mouse across a running log, and it crashed within seconds.
    struct LaneRect { size_t lane; float y, h; };
    std::vector<LaneRect> laneRects_;
    jf::JRect             plotRect_{};

    std::vector<triggerlog::Lane> lanes_;
    std::string                   status_ = "Not started";
    std::string                   notice_;   // header-line notice (capture complete, etc.)
    std::string                   hover_;
    size_t                        first_ = 0;
    size_t                        count_ = 200;
};

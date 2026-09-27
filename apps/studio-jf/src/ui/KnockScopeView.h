#pragma once

// KnockScopeView — one classified knock measurement, drawn so it can be read.
//
// The thing being shown is a PROFILE: band level per bucket across the sampling window, in crank
// degrees. On its own that is an unreadable row of numbers, because whether any of it means anything
// depends on three values resolved on the ECU at the moment of judgement and gone by the time it
// arrives here: the learned NOISE FLOOR this cylinder was measured against, where the SPARK was, and
// what THRESHOLD the operating point demanded. So the firmware records the verdict with its evidence
// and this draws all of it together.
//
// The floor and the threshold are horizontal lines because that is what they are — the bar the
// buckets have to clear. The spark is a vertical line because the whole knock/pre-ignition question
// is which SIDE of it the energy sits on: after the spark is knock and answers to retard, at or
// before it is pre-ignition and answers to a cut. Drawing the spark anywhere other than its live
// angle would quietly change the question, since advance moves with the map.
//
// Buckets past the threshold are tinted, so "this fired and here is why" is one glance rather than a
// comparison the reader has to do themselves.

#include <j/core/JWidget.h>
#include "../comms/EcuLink.h"

#include <string>

class KnockScopeView : public jf::JWidget {
public:
    explicit KnockScopeView(jf::JSceneGraph& g) : jf::JWidget(g, "KnockScopeView") {}

    // TWO traces, and the distinction is the panel's whole usability.
    //
    // The LIVE shot is whatever was classified most recently. It moves, which is what tells a person
    // the thing is running — on a healthy engine that is a quiet window, over and over.
    //
    // The EVENT is the last non-clean verdict, latched. It is the one worth looking at, and it is
    // static by nature: at 50 windows/sec a knock is gone long before any poll or any pair of eyes
    // reaches it. Showing only that made the panel look frozen; showing only the live one would mean
    // the interesting cycle flashes past unread. So: live drawn solid, event held as a ghost behind.
    void setLive (const KnockShot& s) { live_  = s; invalidate(); }
    void setEvent(const KnockShot& s) { event_ = s; invalidate(); }
    [[nodiscard]] const KnockShot& live()  const { return live_;  }
    [[nodiscard]] const KnockShot& event() const { return event_; }

    // A note drawn instead of the plot: not connected, no classifier, nothing measured yet. Said
    // outright rather than shown as an empty grid, which reads as "silent engine" when it means
    // "nothing arrived".
    void setNotice(std::string s) { notice_ = std::move(s); invalidate(); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

private:
    KnockShot   live_;     // newest classification — moves, proves it is running
    KnockShot   event_;    // last knock / pre-ignition — latched, the one worth reading
    std::string notice_;
};

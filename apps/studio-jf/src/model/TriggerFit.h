#pragma once

// What a CAPTURE says about the trigger system that produced it — all of it, not just the crank.
//
// The designer could draw any wheel you could describe and had no way to tell you whether the one
// you had drawn was the one on the engine. This is the measuring half, as pure logic over captured
// lanes — no UI, no link, no config — so it can be tested against signals whose answer is known
// before it is trusted against one whose is not.
//
// IT DESCRIBES, IT DOES NOT ASSUME. The first version of this looked for a missing-tooth crank and
// called everything else a failure, which meant a Nissan CAS — 360 even slits on the crank track and
// a handful of unevenly sized windows on the cam — came back as "this looks like an even wheel"
// twice and produced nothing. An even track is not a failure to fit; it is a fact about the wheel,
// and on that engine it is the WHOLE crank track. So each stream is characterised on its own terms:
// what its edges do, where they depart from doing it, and how that repeats.
//
// There can be six streams (crank primary and secondary, then intake/exhaust cams on two banks), so
// nothing here is written for "the crank lane" — the roles are worked out from what the signals ARE.

#include "TriggerLog.h"

#include <string>
#include <vector>

namespace triggerfit {

// What the edges on one wire do.
enum class Shape {
    Unknown,   // not enough signal to say
    Even,      // a uniform train: every gap the same. No landmark of its own.
    Gap,       // a uniform train with teeth MISSING — the ordinary 36-1/60-2 crank
    Width,     // teeth all present and evenly spaced, but of DIFFERENT WIDTHS — the wide one is the
               // landmark. Only visible when both edges were captured; with one edge armed this
               // wheel is indistinguishable from Even, which is the honest answer in that case.
    Coded,     // repeating, and the departures are not whole missing teeth: spans that carry
               // meaning. A CAS cam track, a Renix crank.
};

// WHERE A STREAM STOPS BEING UNIFORM. Everything interesting about a trigger wheel is an anomaly in
// an otherwise regular train, so they are reported as first-class facts rather than boiled straight
// down into a tooth count: a caller that wants "36-1" gets it, and one looking at a wheel nobody has
// a name for still gets told exactly where the signal departs from regular and by how much.
struct Anomaly {
    int    edge  = 0;        // index within the period, counting from the first edge after it
    double ratio = 0.0;      // this interval measured in median pitches: 2.0 = one tooth missing
    int    missing = 0;      // whole teeth absent, when the ratio is close to a whole number; else 0
    double atDeg = 0.0;      // where it falls in the stream's own period
};

// THE CRANK AS A CLOCK — what angle the engine had reached, at any moment in the capture.
//
// A dense crank track measures more than its own geometry. Every one of its edges is a known angle
// at a known time, so together they say how fast the engine was turning at every instant, exactly,
// with no model of the wave in it at all. That is the thing a cam needs and cannot get for itself:
// a cam has a handful of edges per engine cycle, far too few to separate its own tooth spacing from
// the engine surging underneath it — asked to, it fits the surge to its OWN pattern and reports the
// engine as a wheel.
//
// Held in cumulative teeth rather than degrees because a stream does not know its own tooth count
// until the capture as a whole has been made sense of. fitCapture scales it.
struct AngleTrack {
    std::vector<uint32_t> tUs;        // when each tooth arrived
    std::vector<double>   cumTeeth;   // ...and how many nominal teeth had passed by then
    bool usable() const { return tUs.size() >= 8 && tUs.size() == cumTeeth.size(); }
    // Teeth elapsed at time t, interpolated between the two edges either side of it. Outside the
    // capture it holds at the ends rather than extrapolating: an edge before the crank's first tooth
    // has no angle, and inventing one would place a cam edge on evidence that does not exist.
    double teethAt(uint32_t t) const;
};

struct StreamFit {
    bool        ok = false;
    std::string why;              // why not, when !ok — never empty on failure
    int         stream = 0;       // the capture's stream index
    std::string name;

    Shape  shape = Shape::Unknown;
    double pitchUs  = 0.0;        // median interval — one tooth, whatever a tooth is here
    // BOTH EDGES OR ONE. A capture armed on one edge can measure spacing and nothing else; armed on
    // both it can measure how long each tooth is PRESENT, which is the whole of what a width-coded
    // wheel encodes. Saying which was captured is how a caller knows whether "even" means "even" or
    // "even as far as this capture could see".
    bool   bothEdges = false;
    double widthUs   = 0.0;       // median tooth width (high time); 0 when only one edge was armed
    std::vector<double> widths;   // one per tooth of the period, same order as edgeAngles
    double periodUs = 0.0;        // one repeat of the pattern; 0 when the train has no landmark
    int    edges    = 0;          // edges in one period
    // WHAT WAS ACTUALLY COUNTED, as opposed to what could be concluded. An even train has no period,
    // so its teeth-per-turn is not 0 — it is unknown, and the two read identically in a field that
    // can only hold a number. This one is always filled, so a stream that measured three hundred
    // perfectly regular teeth says so instead of reporting zeros and looking like a failure.
    int    edgesSeen = 0;
    AngleTrack track;             // this stream as a clock — filled for every stream, used for the crank
    int    teeth    = 0;          // nominal count (present + absent) once a period is known
    int    missing  = 0;
    std::vector<int>     gapPos;  // absent nominal indices, from the first tooth after the gap
    std::vector<Anomaly> anomalies;
    std::vector<double>  edgeAngles;   // one period's edges, degrees, for drawing

    // Rate relative to the crank, once the capture as a whole has been made sense of:
    // 1 = turns with the crank, 2 = one period per engine cycle (a cam). 0 = not yet known.
    int cycleRatio = 0;

    // CAN THIS STREAM SAY WHERE IT IS, ON ITS OWN?
    //
    // A landmark is only a landmark if it happens ONCE per period. A 36-1 has one gap per turn and
    // can anchor; a 12-1 pattern that repeats three times round a 36-tooth wheel has three identical
    // gaps and can anchor nothing — after any of them the decoder knows the tooth and not the turn.
    // An even track has no landmark at all.
    //
    // This is what decides who provides sync. When the crank cannot, the cam must — both the
    // position reference AND the half-speed phase — and then the CAM's pattern needs a unique key of
    // its own, or nothing in the system can say where the engine is.
    bool        uniqueKey = false;
    double      keyAngle  = 0.0;    // where that landmark falls in this stream's period
    std::string keyWhat;            // what makes it unique, in words for the designer to show
};

// The capture as a system.
struct CaptureFit {
    bool        ok = false;
    std::string why;
    std::vector<StreamFit> streams;    // one per lane that carried enough edges to say anything
    int    crankIdx = -1;              // index into streams: the crank primary
    double revUs    = 0.0;             // one crank revolution
    double rpm      = 0.0;

    // WHO ANCHORS THE ENGINE. The stream carrying the unique landmark the decoder syncs on — the
    // crank when it has one, otherwise a cam, otherwise nobody and the system cannot be decoded.
    int  syncIdx = -1;
    bool phase   = false;             // a cam is present: the cycle is 720 and sequential is possible
    bool canSync = false;             // some stream carries a unique key
    std::string syncNote;             // how sync is reached, or what is missing
};

// Characterise one lane on its own terms. A uniform train fits — as Even, with no period, because a
// train with no landmark cannot say where it starts or how long it is. The capture as a whole
// supplies that (fitCapture), which is the only place it can come from.
StreamFit fitStream(const triggerlog::Lane& lane);

// Characterise every lane, then work out what they are to each other.
CaptureFit fitCapture(const std::vector<triggerlog::Lane>& lanes);

}  // namespace triggerfit

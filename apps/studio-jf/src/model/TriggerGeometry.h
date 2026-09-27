#pragma once

// TriggerGeometry — a wheel → renderable geometry (tooth/edge angles, missing-tooth gap arcs, cam
// pulse) that mirrors the firmware decode primitives EXACTLY, so the designer's tooth-wheel dial and
// wire-signal trace are truthful, not a cartoon. Pure logic (no UI) — unit-tested headless.
//
// The geometry, per trigger kind:
//   GAP      — `slots` nominal positions, tooth_angle = period/slots; each gap index gets ratio× pitch
//              before it; a gap arc is any inter-tooth spacing > 1.5× nominal (the GapMatcher test).
//   SEQUENCE — edge angles = cumulative sum of cell[]/10 (0.1° spans).
//   WIDTH    — one representative cam pulse per cam revolution (720°).
// Crank = 360° period, cam = 720°; the combined cycle is 720° when a cam stream is present.

#include "TriggerWheel.h"

#include <string>
#include <utility>
#include <vector>

// Cam DISPLAY roles start here: 1 Crank Primary, 2 Crank Secondary, 3.. the cams. One-based, and for
// LABELS only — the slot a stream actually occupies is 0-based and lives in TriggerWheel.h
// (kSlotCamBase = 2). Writing one where the other belongs is what put every designer-authored cam one
// slot too far, so the name now says which it is.
constexpr int kDisplayRoleCamBase = 3;

constexpr double kCrankPeriod = 360.0;
constexpr double kCamPeriod   = 720.0;

struct StreamGeometry {
    int    rate = 0;                                   // 0 crank (360°), 1 cam (720°)
    int    primitive = 0;                              // 0 GAP, 1 SEQUENCE, 2 WIDTH
    // WHICH SLOT this is (0 Crank Primary … 5 Cam Exhaust B2) and which edge the capture takes.
    // Both are facts about the stream, and a painter needs them — the slot to colour and label the
    // lane, the edge to draw the trace the right way up. They live here so a painter can be handed
    // geometry ALONE: while it had to reach back into the Wheel for `edge` and be passed a parallel
    // roles vector, it could only ever draw a wheel from the library, never one read off an ECU.
    // The DISPLAY role — 1 Crank Primary, 2 Crank Secondary, 3+n the cams — which is what a lane's
    // colour and short label key off. Deliberately NOT WheelStream::role, which is the 0-based SLOT and
    // may be -1 for "unstated": two different numberings a single letter apart, and mixing them shifts
    // every lane's colour and caption by one.
    int    displayRole = 0;
    int    edge = 0;                                   // 0 rising / 1 falling / 2 both
    double period = kCrankPeriod;
    int    slots = 0;
    int    present = 0;                                // teeth/edges actually present
    double toothAngle = 0.0;                           // nominal spacing (GAP); mean span (SEQ)
    std::vector<double> teeth;                         // present edge angles within the period
    std::vector<std::pair<double, double>> gaps;       // (start, end) missing arcs
    bool   hasPulse = false;                           // WIDTH only
    std::pair<double, double> pulse = {0.0, 0.0};      // (start, end) cam pulse
};

struct WheelGeometry {
    std::string name;
    std::string sync;
    double cycle = kCrankPeriod;                       // 360 (crank-only) or 720 (cam present)
    std::vector<StreamGeometry> streams;
    int crankIdx = -1;                                 // index into streams of the crank (or -1)
    int camIdx   = -1;                                 // index into streams of the cam (or -1)
    // Can the decoder establish an ABSOLUTE zero on this wheel? A gap wheel can (it recognises the
    // gap); a sequence wheel can (it matches the cell pattern); an even wheel cannot — it locks
    // relatively and never knows which tooth it is on.
    //
    // Where that zero IS needs no field: it is 0.0 by construction — the start of the authored list,
    // and streamGeometry lays teeth out from there. What the decoder anchors ON is the first tooth
    // after SYNC, and sync means different things per primitive: the gap for a GAP stream, any
    // identified event for a SEQUENCE (SequenceMatcher.h:125-127), the reference pulse for a WIDTH.
    // A gap wheel is the case where sync is the ONLY identifiable event, which is why its first gap
    // index is 0 and its origin is the tooth ending the gap (GapMatcher.h:130-135).
    //
    // There used to be a `syncAngle` here holding the MIDPOINT OF THE GAP, which is not the decoder's
    // zero, is not any angle the firmware computes, and put every TDC mark half a gap out (65° on a
    // 2JZ 36-2).
    bool   hasReference = false;
    double tdcOffset = 0.0;                            // crank° from the decoder's zero to TDC #1
};

StreamGeometry streamGeometry(const WheelStream& st);
WheelGeometry  wheelGeometry(const Wheel& w);

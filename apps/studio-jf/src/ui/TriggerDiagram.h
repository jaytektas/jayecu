#pragma once

// TriggerDiagram — the firmware-faithful trigger picture, as a painter over WheelGeometry ALONE.
//
// It draws two things: the DIAL (the gear seen from the front of the engine) and the TRACE (one lane
// per stream on the engine-angle axis, which is what shows how streams relate — a crank pattern
// repeating twice inside a 720 degree cycle, a cam pulse landing between two crank teeth).
//
// It lived inside the designer, and could therefore only ever draw a wheel from the LIBRARY. The same
// picture is what a tuner needs of the configuration actually on the ECU, and those are the same
// geometry: wheelFromConfig() reads the live config into a Wheel and wheelGeometry() turns either into
// the struct below. Nothing here knows or can ask which one it got.
//
// Kept as free functions rather than a widget so both callers stay free to arrange around them — the
// designer places a dial above a trace in its Visualizer tab, a canvas widget fills its own box.

#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/VectorGraphics.h>

#include "../model/TriggerGeometry.h"
#include "../model/TriggerWheel.h"

#include <string>
#include <vector>

namespace trigger_ui {
constexpr double kPi = 3.14159265358979323846;
inline float dialRad(double deg) { return static_cast<float>((deg - 90.0) * kPi / 180.0); }

// The dial is the GEAR, seen from the front of the engine, turning clockwise. A feature the decoder
// meets later has still to travel to the sensor, so it sits further ANTICLOCKWISE — the drawn angle
// runs opposite to the decoder's. Plotting the decoder's angle straight onto the dial drew the
// mirror image, so the tooth after the gap appeared as the tooth before it.
// The flat trace is not a gear: x is the decoder's angle, left to right, and is unchanged.

// Short role labels for the per-stream trace rows (must track schema streams.role order).
inline const char* roleShort(int r) {
    switch (r) {
        case 1: return "Crank 1";   case 2: return "Crank 2";
        case 3: return "Cam IN B1"; case 4: return "Cam EX B1";
        case 5: return "Cam IN B2"; case 6: return "Cam EX B2";
        default: return "Stream";
    }
}
// A distinct-ish colour per role so crank/cam rows read apart at a glance.
inline jf::JColor roleColor(int r) {
    switch (r) {
        case 1: return jf::rgb(120, 200, 255);   case 2: return jf::rgb(90, 160, 220);
        case 3: return jf::rgb(255, 200, 90);    case 4: return jf::rgb(240, 160, 70);
        case 5: return jf::rgb(200, 230, 120);   case 6: return jf::rgb(170, 210, 90);
        default: return jf::rgb(170, 178, 188);
    }
}
inline std::string describe(const Wheel& w) {
    for (const WheelStream& s : w.streams)
        if (s.rate == 0) {
            char t[48];
            const char* cam = (w.sync == "PHASE") ? " + cam" : "";
            if (s.prim == 0 && !s.cell.empty())
                std::snprintf(t, sizeof(t), "%d-%d%s", s.slots, (s.ratio ? s.ratio - 1 : 1), cam);
            else if (s.prim == 0)
                std::snprintf(t, sizeof(t), "%d even%s", s.slots, cam);
            else
                std::snprintf(t, sizeof(t), "sequence%s", cam);
            return t;
        }
    return "(no trigger)";
}

// ---- the painter -----------------------------------------------------------------------------

inline void drawDial(jf::JVectorCanvas& vg, const WheelGeometry& g, float cx, float cy, float R,
                     double sensorAngle, double crankAngle) {
        using trigger_ui::dialRad;
        const jf::JColor rim   = jf::rgb(70, 76, 84);
        const jf::JColor tooth = jf::rgb(185, 194, 204);
        const jf::JColor gapc  = jf::rgb(200, 70, 60);
        const jf::JColor syncc = jf::rgb(0, 200, 255);
        const jf::JColor tdcc  = jf::rgb(255, 150, 40);
        const jf::JColor pick  = jf::rgb(230, 234, 240);

        vg.strokeCircle(cx, cy, R, 2.f, jf::JPaint::solid(rim));

        // TDC #1, fixed at vertical: the direction #1's crank throw points when the piston is at the
        // top. It does not move — the WHEEL turns under it as crankAngle changes — so it is the
        // datum everything else is read against. On a JZ, crankAngle 0 puts tooth 5 on this line,
        // which is the check against the actual engine.
        {
            const float a = dialRad(0.0);
            const float ca = std::cos(a), sa = std::sin(a);
            vg.drawLine(cx + ca * R * 1.06f, cy + sa * R * 1.06f,
                        cx + ca * R * 1.30f, cy + sa * R * 1.30f, 3.f, jf::JPaint::solid(tdcc));
        }

        // The pickup: OUTSIDE the rim, fixed to the engine, not to the wheel. Drawn even when the
        // wheel has nothing to show, because it is the one thing that never moves.
        {
            const float a = dialRad(-sensorAngle);
            const float ca = std::cos(a), sa = std::sin(a);
            vg.drawLine(cx + ca * R * 1.06f, cy + sa * R * 1.06f,
                        cx + ca * R * 1.24f, cy + sa * R * 1.24f, 3.f, jf::JPaint::solid(pick));
            vg.fillCircle(cx + ca * R * 1.24f, cy + sa * R * 1.24f, R * 0.035f, jf::JPaint::solid(pick));
        }

        if (g.crankIdx < 0) { vg.fillCircle(cx, cy, R * 0.06f, jf::JPaint::solid(rim)); return; }
        const StreamGeometry& s = g.streams[g.crankIdx];
        const double base = -sensorAngle + g.tdcOffset + crankAngle;

        // CAM RINGS, inside the crank. A cam turns at half crank speed, so its features are plotted
        // against a 720 span and land on their own smaller circle — the phase relationship between a
        // cam window and the crank teeth is the thing being authored, and it was only visible on the
        // flat trace before. A second crank stream rides the crank's own ring, being at crank rate.
        //
        // Same mapping, per stream period: the wheel turns clockwise, so a feature met later sits
        // further anticlockwise. crankAngle is crank degrees, hence halved on a cam-rate ring.
        int camN = 0;
        for (std::size_t si = 0; si < g.streams.size(); ++si) {
            const StreamGeometry& cs = g.streams[si];
            if (cs.rate != 1) continue;
            const float Rc = R * (0.62f - 0.12f * camN);
            if (Rc < R * 0.18f) break;                       // out of room — four cams is the ceiling
            const jf::JColor cc = trigger_ui::roleColor(3 + camN);
            vg.strokeCircle(cx, cy, Rc, 1.5f, jf::JPaint::solid(rim));
            const double cbase = -sensorAngle + g.tdcOffset + crankAngle * 0.5;
            if (cs.hasPulse) {                               // WIDTH: the window as an arc
                const float a0 = dialRad(cbase - cs.pulse.second * 360.0 / cs.period);
                const float a1 = dialRad(cbase - cs.pulse.first  * 360.0 / cs.period);
                vg.strokeArc(cx, cy, Rc, a0, a1, 5.f, jf::JPaint::solid(cc));
            } else {                                         // SEQUENCE / GAP: one spoke per edge
                for (double th : cs.teeth) {
                    const float a = dialRad(cbase - th * 360.0 / cs.period);
                    vg.drawLine(cx + std::cos(a) * Rc * 0.86f, cy + std::sin(a) * Rc * 0.86f,
                                cx + std::cos(a) * Rc, cy + std::sin(a) * Rc,
                                2.f, jf::JPaint::solid(cc));
                }
            }
            ++camN;
        }

        for (double th : s.teeth) {
            const float a = dialRad(base - th * 360.0 / s.period);
            const float ca = std::cos(a), sa = std::sin(a);
            vg.drawLine(cx + ca * R * 0.80f, cy + sa * R * 0.80f, cx + ca * R, cy + sa * R,
                        2.f, jf::JPaint::solid(tooth));
        }
        for (const auto& gp : s.gaps) {
            // D is negated, so the arc's ends swap: pass the smaller screen angle first or it sweeps
            // the long way round the wheel.
            const float a0 = dialRad(base - gp.second * 360.0 / s.period);
            const float a1 = dialRad(base - gp.first  * 360.0 / s.period);
            vg.strokeArc(cx, cy, R, a0, a1, 4.f, jf::JPaint::solid(gapc));
        }
        if (g.hasReference) {
            // Cyan: the reference tooth (decoder 0 — the first tooth after sync). It points at a
            // TOOTH, never at the gap.
            const float a = dialRad(base);
            vg.drawLine(cx, cy, cx + std::cos(a) * R * 0.9f, cy + std::sin(a) * R * 0.9f,
                        2.f, jf::JPaint::solid(syncc));
            // Orange: the point on the wheel that is under the pickup at TDC #1 (decoder = tdcOffset).
            // At crankAngle 0 it lands exactly on the pickup, and winding away from TDC walks it off
            // by the same angle — so "where am I relative to TDC" is readable without a number.
            const float at = dialRad(-sensorAngle + crankAngle);
            vg.drawLine(cx, cy, cx + std::cos(at) * R, cy + std::sin(at) * R,
                        2.f, jf::JPaint::solid(tdcc));
        }
        vg.fillCircle(cx, cy, R * 0.06f, jf::JPaint::solid(rim));
    }

inline void drawTrace(jf::JVectorCanvas& vg, const WheelGeometry& g,
                          float plotX, float traceY, float plotW, float traceH, float rowH) {
        const jf::JColor axis = jf::rgb(60, 66, 74);
        if (plotW < 8.f || traceH < 8.f) return;
        const double cycle = g.cycle;
        auto X = [&](double deg) { return plotX + static_cast<float>(deg / cycle) * plotW; };
        // x is ENGINE angle: 0 is TDC #1, not tooth 0. Every other angle a tuner works in is
        // referenced to TDC — spark, injection, cam targets — so a trace starting at the wheel's own
        // zero could not be read against any of them, and where TDC actually fell depended on the
        // offset. E(d) is the firmware's own conversion, engine = decoder - offset
        // (EnginePositionHal::engine_angle).
        auto E = [&](double d) { double e = std::fmod(d - g.tdcOffset, cycle); return e < 0.0 ? e + cycle : e; };
        // A block spanning [a0, a0+width] in engine angle, split when it runs off the end and wraps
        // back to the start — at speed the last tooth of the cycle straddles the boundary.
        auto block = [&](double a0, double width, float hi, float lo, const jf::JColor& c) {
            const double end = a0 + width;
            const float x0 = X(a0);
            const float x1 = X(std::min(end, cycle));
            vg.fillRect(x0, hi, std::max(1.f, x1 - x0), lo - hi, jf::JPaint::solid(c));
            if (end > cycle)
                vg.fillRect(X(0.0), hi, std::max(1.f, X(end - cycle) - X(0.0)), lo - hi, jf::JPaint::solid(c));
        };

        const int n = static_cast<int>(g.streams.size());
        for (int i = 0; i < n; ++i) {
            const StreamGeometry& s = g.streams[i];
            const float lo = traceY + i * rowH + rowH - 3.f, hi = traceY + i * rowH + 4.f;
            vg.drawLine(plotX, lo, plotX + plotW, lo, 1.f, jf::JPaint::solid(axis));
            const jf::JColor col = trigger_ui::roleColor(g.streams[i].displayRole);

            // 0 Rising · 1 Falling · 2 Both — the decoder's Capture Edge codes.
            const int edge = g.streams[i].edge;
            const jf::JColor mark = jf::rgb(255, 210, 90);
            const float tick = 5.f;
            // A caret ABOVE the leading edge, BELOW the trailing one, drawn only where the decoder
            // is actually looking. Two identical blocks then read differently at a glance.
            auto edgeMarks = [&](float xa, float xb) {
                if (edge == 0 || edge == 2) vg.drawLine(xa, hi - tick, xa, hi, 1.6f, jf::JPaint::solid(mark));
                if (edge == 1 || edge == 2) vg.drawLine(xb, lo, xb, lo + tick, 1.6f, jf::JPaint::solid(mark));
            };

            if (s.primitive == 2 && s.hasPulse) {
                const double a0 = E(s.pulse.first), wdt = s.pulse.second - s.pulse.first;
                block(a0, wdt, hi, lo, col);
                edgeMarks(X(a0), X(E(s.pulse.second)));
            } else {
                const int reps = std::max(1, static_cast<int>(std::llround(cycle / s.period)));
                const double pw = std::max(1.0, s.toothAngle * 0.35);
                for (int r = 0; r < reps; ++r)
                    for (double th : s.teeth) {
                        const double a0 = E(th + r * s.period);
                        block(a0, pw, hi, lo, col);
                        edgeMarks(X(a0), X(E(th + r * s.period + pw)));
                    }
            }
        }

        // The two references, once per crank revolution. TDC #1 is at 0 by construction now — it is
        // what the axis is measured from — and the reference tooth falls wherever the offset puts it.
        if (g.hasReference && g.crankIdx >= 0) {
            const double period = g.streams[g.crankIdx].period;
            const int reps = std::max(1, static_cast<int>(std::llround(cycle / period)));
            for (int r = 0; r < reps; ++r) {
                const float xs = X(E(r * period));                       // the reference tooth
                vg.drawLine(xs, traceY, xs, traceY + traceH, 1.f, jf::JPaint::solid(jf::rgb(0, 200, 255)));
                const float xt = X(std::fmod(r * period, cycle));        // TDC #1
                vg.drawLine(xt, traceY, xt, traceY + traceH, 1.f, jf::JPaint::solid(jf::rgb(255, 150, 40)));
            }
        }
    }

}  // namespace trigger_ui

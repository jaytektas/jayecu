// DialWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "DialWidget.h"
#include "../../model/Cache.h"
#include "../../model/RangeBands.h"   // ColorRules::Zone — the dial-face spans
#include <j/graphics/VectorGraphics.h>
#include <vector>

void DialWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
    const PanelElement& el = *m_el;   // pure gauge, no value number; base drawBackground painted the card
    // VALUE RANGES REACH THE DIAL. Its colours are its own named props, so they were read through the
    // static elColor() and every band set on a dial was silently ignored -- the one widget where a red
    // zone is the whole point. Each part listens to the band channel that matches what it means:
    //   face  <- bg      the dial's ground
    //   fill  <- accent  the arc that says how much: the part an alert most wants to recolour
    //   bezel <- border  its rim is a border
    //   glow  <- fg      a dial prints no number, so fg would otherwise be dead here; a halo is the most
    //                    alert-like thing it draws, and it is the channel left to give it
    // Peak and centre cap stay local: they are detail, not state, and there is no channel left to spend.
    auto band = [&](const char* key, BandChannel ch, const uint8_t* fb, uint8_t out[4]) {
        return elColorBanded(el, key, ch, fb, out);
    };
    uint8_t tb[4], fb[4], cb[4], bzb[4], gb[4];
    static const uint8_t kTrack[4] = {40, 40, 40, 255}, kFill[4] = {0, 200, 255, 255},
                         kCap[4] = {60, 60, 60, 255}, kBezel[4] = {80, 80, 80, 255};
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    const std::string bind = bindPath();
    const double val = dispV(el, evalSource(bind)), lo = dispV(el, Raw{numOrExpr(el.prop("minValue"), 0)}), hi = dispV(el, Raw{numOrExpr(el.prop("maxValue"), 100)});
    float frac = static_cast<float>((hi > lo) ? std::clamp((val - lo) / (hi - lo), 0.0, 1.0) : 0.0);
    if (el.prop("inverted") == "1") frac = 1.f - frac;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f, half = std::min(r.width, r.height) * 0.5f;
    const float baseR = half * 0.9f;
    if (baseR > 6.f) {
        jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
        constexpr float kPi = 3.14159265358979f;
        auto rad = [&](double deg) { return static_cast<float>(-deg * kPi / 180.0); };
        const float a0 = rad(numD("startAngle", 225)), a1 = rad(numD("endAngle", -45)), total = a1 - a0;
        const float bezelT = static_cast<float>(numD("bezelWidth", 0.02)) * 2.f * half;
        const float R = baseR - bezelT * 0.5f;
        const float rIn = R * static_cast<float>(numD("innerRadius", 0.8));
        const float glow = static_cast<float>(numD("glowRadius", 0));
        if (glow > 0.f) {
            const float gr = baseR + glow * half * 0.33f;
            vg.fillCircle(cx, cy, gr, jf::JPaint::radial(cx, cy, baseR, jc(band("glowColor", BandChannel::Fg, kFill, gb)), gr, jf::rgba(0, 0, 0, 0)));
        }
        if (bezelT > 0.5f) vg.strokeCircle(cx, cy, baseR - bezelT * 0.5f, bezelT, jf::JPaint::solid(jc(band("bezelColor", BandChannel::Border, kBezel, bzb))));
        const jf::JPaint trackP = jf::JPaint::solid(jc(band("dialColor", BandChannel::Bg, kTrack, tb)));
        const uint8_t* fillC = band("fillColor", BandChannel::Accent, kFill, fb);   // banded: a red zone recolours the arc
        const bool showFill = el.prop("showFill") != "0";
        // The track is the ring in the dial colour. It backs a value fill, and it BURIES a scale: drawn
        // across the whole ring at full opacity, it leaves tick marks and numbers sitting on a solid disc
        // of a single colour. Optional, on by default, so a dial that reads as a fill gauge is unchanged
        // and one stacked under a scale and a needle can turn the disc off and keep its zones.
        const bool showBand = el.prop("showDialBand") != "0";

        // ZONES: fixed spans of the SCALE painted into the face — a tacho's red 6500-8000 and the orange
        // below it, there with the engine off. Resolved ONCE here, into fractions along the dial's own
        // span, because both arc styles (continuous and segmented) need the same answer and neither of
        // them should be asking the string parser inside a loop.
        //
        // Through the same lo/hi the fill uses, so a zone lands where the scale stacked over it says it
        // should; a zone reaching past either end is clipped to the dial rather than wrapping round.
        struct DialZone { float f0, f1; uint8_t c[4]; };
        std::vector<DialZone> zones;
        for (const ColorRules::Zone& z : ColorRules::zonesFromCompact(el.prop("zones"))) {
            DialZone d{};
            if (!skin::parseHex(z.color, d.c) || hi <= lo) continue;
            // Through numOrExpr, like minValue and maxValue above it: a bound may be written as an
            // expression ("{ useMetricOnInterface ? 95 : 203 }"), and a band that moves with the display
            // unit has to be evaluated here rather than frozen when the page was authored.
            double z0 = std::clamp((dispV(el, Raw{numOrExpr(z.startExpr, z.start)}) - lo) / (hi - lo), 0.0, 1.0);
            double z1 = std::clamp((dispV(el, Raw{numOrExpr(z.endExpr,   z.end)})   - lo) / (hi - lo), 0.0, 1.0);
            if (el.prop("inverted") == "1") { z0 = 1.0 - z0; z1 = 1.0 - z1; }
            d.f0 = static_cast<float>(std::min(z0, z1)); d.f1 = static_cast<float>(std::max(z0, z1));
            if (d.f1 - d.f0 < 1e-6f) continue;                        // a zone of no width draws nothing
            zones.push_back(d);
        }
        // WHERE THE BAND SITS IN THE RING. zoneWidth is the fraction of the ring's thickness the zones
        // take, anchored at the OUTER edge. At the default 1 they fill the ring and the value fill paints
        // over them, which is the arc-fill gauge everyone knows. Below 1 the zones keep the outer sliver
        // to themselves and the fill gets the rest, so a modern dial can show both at once: a permanent
        // coloured rim over a moving arc.
        const float zoneW = static_cast<float>(std::clamp(numD("zoneWidth", 1.0), 0.0, 1.0));
        const float zoneRIn = R - (R - rIn) * zoneW;
        const float fillR = (zones.empty() || zoneW >= 0.999f) ? R : zoneRIn;
        auto zoneAtFrac = [&](float f) -> const DialZone* {          // FIRST match, as a rule does
            for (const DialZone& z : zones) if (f >= z.f0 && f <= z.f1) return &z;
            return nullptr;
        };
        const int segs = static_cast<int>(numD("segmentCount", 0));
        if (segs > 0) {
            const float spac = (total < 0 ? -1.f : 1.f) * static_cast<float>(numD("segmentSpacing", 0)) * kPi / 180.f;
            const float segSpan = (total - (segs - 1) * spac) / segs;
            const bool segFill = el.prop("segmentedFill") == "1";
            const int lit = static_cast<int>(std::lround(frac * segs));
            for (int i = 0; i < segs; ++i) {
                const float s = a0 + i * (segSpan + spac);
                const bool on = segFill && showFill && i < lit;
                if (showBand) vg.fillRing(cx, cy, rIn, R, s, s + segSpan, trackP);
                // A SEGMENT IS THE UNIT. Its zone is decided by the value at its middle and painted across
                // the whole bar, not clipped to where the zone's number falls inside it — a segmented dial
                // has no sub-segment resolution to spend, and half a lit bar in red reads as a rendering
                // fault. Painting per segment (rather than as one arc over the lot) is also what keeps the
                // spacing between bars empty: the gaps are not part of any segment, so nothing paints there.
                if (const DialZone* z = zoneAtFrac((i + 0.5f) / segs))
                    vg.fillRing(cx, cy, zoneRIn, R, s, s + segSpan, jf::JPaint::solid(jc(z->c)));
                if (on) vg.fillRing(cx, cy, rIn, fillR, s, s + segSpan, jf::JPaint::solid(jc(fillC)));
            }
        } else {
            if (showBand) vg.fillRing(cx, cy, rIn, R, a0, a1, trackP);
            // Zones go on TOP of the track and UNDER the fill, so the reading still reads over them and a
            // dial with the fill switched off (the modern needle-less look, dial + scale + needle stacked)
            // is left showing exactly the zones.
            for (const DialZone& z : zones)
                vg.fillRing(cx, cy, zoneRIn, R, a0 + total * z.f0, a0 + total * z.f1, jf::JPaint::solid(jc(z.c)));
            if (showFill && frac > 1e-4f) vg.fillRing(cx, cy, rIn, fillR, a0, a0 + total * frac, jf::JPaint::solid(jc(fillC)));
        }
        if (el.prop("showPeak") == "1" && hi > lo) {
            const double pk = peakOf(el.uid, val, numD("peakHoldTime", 3000));
            float pf = static_cast<float>(std::clamp((pk - lo) / (hi - lo), 0.0, 1.0));
            if (el.prop("inverted") == "1") pf = 1.f - pf;
            const float pa = a0 + total * pf, w = 1.5f * kPi / 180.f;
            uint8_t pb[4]; static const uint8_t kPeak[4] = {255, 90, 90, 255};
            vg.fillRing(cx, cy, rIn, R, pa - w, pa + w, jf::JPaint::solid(jc(elColor(el, "peakColor", kPeak, pb))));
        }
        const float capR = static_cast<float>(numD("centerCapRadius", 0.05)) * baseR;
        if (capR > 0.5f) vg.fillCircle(cx, cy, capR, jf::JPaint::solid(jc(elColor(el, "centerCapColor", kCap, cb))));
        if (el.prop("showGlass") == "1")
            vg.fillCircle(cx, cy, baseR, jf::JPaint::linear(cx, cy - baseR, jf::rgba(255, 255, 255, 55), cx, cy + baseR * 0.15f, jf::rgba(255, 255, 255, 0)));
        vg.flush(buf);
    }
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("dial", DialWidget, 20);

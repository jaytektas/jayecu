#include "TriggerFit.h"

#include <algorithm>
#include <cmath>

namespace triggerfit {
namespace {

// THE PITCH IS THE MEDIAN, NOT THE MEAN. A mean is dragged up by the very thing being looked for —
// the gap — and by any hiatus where the signal was torn away; on a 36-1 it sits about 3% high, which
// is enough to make a 2x gap read as 1.94 and round to the wrong number of missing teeth. Most
// intervals on a toothed wheel ARE teeth, so their median is one.
double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

// A departure from uniform. 1.5 because a tooth pitch varies by a few percent with acceleration and
// the smallest meaningful anomaly — one missing tooth — is 2x; the band between is empty on any
// wheel a decoder could read either.
constexpr double kAnomalyRatio = 1.5;

// Close enough to a whole number of teeth to BE a whole number of teeth. A missing-tooth gap lands
// within a few percent of an integer; a width-coded pulse does not, and calling it "2 teeth missing"
// would invent a wheel nobody built.
// 0.22, not 0.18: against its neighbours a gap reads 1.98 when the engine is steady and 1.84 when a
// starter is heaving it over, and that is a real 2 both times. Still nowhere near a coded 2.7.
bool nearWhole(double ratio, double tol = 0.22) {
    return std::fabs(ratio - std::round(ratio)) <= tol;
}

// ONLY LONG, AND DELIBERATELY. A landmark can perfectly well be SHORT — the GM 4200's turn is fixed
// by a 10 degree tooth among 50 to 70 degree ones, and a Nissan CAS window track is six slots of
// which one starts early — and the obvious answer is a mirrored threshold at 1/1.5 to catch them.
//
// It was written, and it changed NOTHING. Every wheel on the rig gave byte-identical output with and
// without it, and so did a synthesised 60-tooth wheel carrying a single extra index tooth, which is
// the purest short mark there is. The reason is that a short mark is short against the local wave,
// and the wave is fitted through its neighbours, so it is dragged down with them — the ratio lands
// near 0.7 rather than near 0.5 and the threshold that would catch it would also catch ordinary
// teeth. What actually reads those wheels is sequencePeriod below, which compares a turn to the next
// turn instead of a tooth to its neighbours.
//
// So there is no short threshold here, because an unexercised branch is a claim nobody can check.
inline bool isAnomaly(double ratio) { return ratio >= kAnomalyRatio; }

// ...and one that is LONG by a whole number of teeth, which is the only kind that means teeth are
// missing. A short interval never does: no wheel has minus one tooth.
inline bool isGap(double ratio) { return ratio >= kAnomalyRatio && nearWhole(ratio); }


// The shortest run of `v` that repeats through the whole of it. A pattern's period, in other words —
// [11, 14, 11, 14] has period 2, [12, 12, 12] has period 1.
//
// THE KEY MUST CARRY THE ANOMALY'S SIZE, not only the teeth to the next one. A width-coded track
// whose departures are evenly spaced but DIFFERENT — one at 2.7 pitches, the next at 3.4 — has the
// same tooth count between every pair, so a period found on counts alone comes back as 1 and calls
// half the pattern a whole turn. Both halves of each entry have to match for the run to be a repeat.
// A PATTERN NEED NOT BE A SPIKE. Everything above looks for one interval that stands out from its
// neighbours, which is how a missing-tooth wheel is built and is not how every wheel is built. A
// 6G72 cam runs 190, 170, 195 and 165 degrees: unmistakably patterned, unmistakably unique, and
// never more than 8% from uniform, so no threshold on a single interval will ever see it. It came
// back as an even train, and so did a Nissan CAS window track.
//
// The test that does see it compares a TURN AGAINST THE NEXT TURN rather than a tooth against its
// neighbours. For a candidate length P, each interval is predicted from the one P earlier, scaled by
// the two spans' total durations — so an engine speeding up scales out exactly and only the SHAPE is
// compared. The smallest P that predicts the whole capture is the pattern's length.
//
// This cannot be the first thing tried, because the local wave fitted above deliberately follows the
// engine, and over a short pattern it follows the PATTERN too and flattens the thing being looked
// for. That is why a sequence wheel needs its own question and not a smaller threshold.
//
// Returns 0 when nothing repeats, which includes a genuinely even train: a uniform series predicts
// itself perfectly at EVERY length, so a period is only accepted when the shape within it actually
// varies.
// IS THIS SHAPE THE WHEEL, OR IS IT THE ENGINE?
//
// A periodic variation in tooth spacing and a periodic variation in crank SPEED are the same signal.
// From one stream they cannot be told apart by looking harder — the teeth arrive when they arrive —
// and once the bench could crank the wheel properly that stopped being a theoretical point: a plain
// even 8 cylinder distributor came back as a 2 tooth coded wheel at twice its real speed, because
// one compression is exactly two of its teeth, and a Nissan's 180 slit crank track came back as 90.
// Both had found the engine and called it a wheel.
//
// What separates them is the SHAPE. An engine's speed follows one smooth rise and fall per
// compression — that is what a piston does — so over its period it turns exactly twice and never
// faster than a sinusoid of that period can. A wheel's spacing is under no such obligation: a GM
// 4200 drops from sixty degrees to ten between adjacent teeth, and a 6G72 cam alternates long and
// short every single tooth, which no crankshaft with mass can do.
//
// Judged on the RATE and not on the tooth duration, because it is the rate that is sinusoidal; one
// over a sine is a peaked thing that breaks the step test by itself.
bool isEngineShaped(const std::vector<double>& d, size_t P) {
    if (P < 2 || 2 * P > d.size()) return false;

    // THE SHAPE AS EVERY TURN AGREES ON IT. Taking it from the first turn alone puts the whole
    // decision at the mercy of one bad interval there — a capture opens on whatever the wheel was
    // doing before it was commanded, and a single outlier makes the smoothest sine in the world look
    // like a wheel with a step in it. Each turn is normalised by its own mean, which takes the
    // engine's drift out, and the median across turns is what is judged.
    std::vector<double> r(P, 0.0);
    const size_t turnsN = d.size() / P;
    std::vector<std::vector<double>> got(P);
    for (size_t m = 0; m < turnsN; ++m) {
        double sum = 0.0; size_t n = 0;
        for (size_t j = 0; j < P; ++j) {
            const double v = d[m * P + j];
            if (v > 0.0) { sum += 1.0 / v; ++n; }
        }
        if (!n || sum <= 0.0) continue;
        const double mean = sum / double(n);
        for (size_t j = 0; j < P; ++j) {
            const double v = d[m * P + j];
            if (v > 0.0) got[j].push_back((1.0 / v) / mean);
        }
    }
    for (size_t j = 0; j < P; ++j) {
        if (got[j].empty()) return false;
        std::sort(got[j].begin(), got[j].end());
        r[j] = got[j][got[j].size() / 2];
    }

    double lo = r[0], hi = r[0];
    for (double v : r) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const double amp = 0.5 * (hi - lo);
    if (amp <= 0.0) return true;

    size_t turning = 0; double biggest = 0.0; int was = 0, first = 0;
    for (size_t j = 0; j < P; ++j) {
        const double diff = r[(j + 1) % P] - r[j];
        biggest = std::max(biggest, std::fabs(diff));
        const int sign = (diff > amp * 0.05) ? 1 : (diff < -amp * 0.05 ? -1 : 0);
        if (sign == 0) continue;
        if (first == 0) first = sign;
        if (was != 0 && sign != was) ++turning;
        was = sign;
    }
    if (was != 0 && first != 0 && was != first) ++turning;   // the sequence wraps: so does the count
    if (turning > 2) return false;                           // more turning points than a compression has

    // ...and no step bigger than a sinusoid of this period takes between adjacent samples. This is
    // what saves a three-entry pattern, which can only ever have one peak and one trough however
    // unlike an engine it is: an FE3 cam runs 330, 30 and 360 degrees, and that 330 degree step is
    // far past anything a smooth cycle sampled three times could produce.
    const double sine = 2.0 * amp * std::sin(3.14159265358979 / double(P));
    return biggest <= sine * 1.15;
}

size_t sequencePeriod(const std::vector<double>& d) {
    // ONE PERCENT, measured and not guessed. Across the rig's wheels a true period predicts the next
    // turn to between 0.00% and 0.30%, and the nearest wrong answer — a 6G72 cam read as two teeth
    // instead of four — manages 1.58%. At 4% that wheel came back as half of itself, with the rpm
    // doubled to match. The gap between a real period and a plausible one is an order of magnitude,
    // so there is no need to sit near either edge of it.
    constexpr double kFits   = 0.01;   // how closely one turn must predict the next
    constexpr double kVaries = 1.10;   // ...and how far from uniform the turn must be to be a shape
    const size_t n = d.size();
    for (size_t P = 2; 3 * P <= n; ++P) {
        double err = 0.0; size_t cnt = 0;
        for (size_t k = 0; k + 2 * P <= n; ++k) {
            double cur = 0.0, nxt = 0.0;
            for (size_t j = 0; j < P; ++j) { cur += d[k + j]; nxt += d[k + P + j]; }
            if (cur <= 0.0 || nxt <= 0.0) { cnt = 0; break; }
            const double want = d[k] * nxt / cur;
            if (want <= 0.0) continue;
            err += std::fabs(d[k + P] - want) / want;
            ++cnt;
        }
        if (!cnt || err / double(cnt) > kFits) continue;
        // How much the pattern varies, as the MIDDLE turn has it and not the first. The first teeth
        // of a capture are the worst teeth in it — the engine is coming off rest and the starter is
        // arcing — so a lead-in of six slow teeth is enough to make a flat pattern look varied or a
        // varied one look flat.
        std::vector<double> spread;
        for (size_t m = 0; (m + 1) * P <= n; ++m) {
            double lo = d[m * P], hi = d[m * P];
            for (size_t j = 0; j < P; ++j) { lo = std::min(lo, d[m * P + j]); hi = std::max(hi, d[m * P + j]); }
            if (lo > 0.0) spread.push_back(hi / lo);
        }
        if (spread.empty()) continue;
        std::sort(spread.begin(), spread.end());
        if (spread[spread.size() / 2] < kVaries) continue;
        if (isEngineShaped(d, P)) continue;
        return P;
    }
    return 0;
}

// MOST OF IT, NOT ALL OF IT. Requiring every entry to repeat means one bad mark anywhere in the
// capture hides the turn completely, and a capture of an unknown wheel has bad marks at both ends by
// its nature: it opens with the engine spinning up and closes wherever the buffer filled. On the
// bench, a 36-1 cranked at 60% swing threw three spurious marks where the lead-in met the wave and
// one short count at the very end — fifteen perfect turns in between, and not a period to be found.
//
// So a period is the shortest run that explains four fifths of the sequence. Nothing looser: an
// oscillation with no pattern in it explains nothing at any length, and a real pattern explains
// nearly all of itself at its own length and nearly none at any shorter one.
size_t shortestPeriod(const std::vector<std::pair<int, int>>& v) {
    for (size_t p = 1; 2 * p <= v.size(); ++p) {
        size_t of = 0, run = 0, best = 0;
        for (size_t i = 0; i + p < v.size(); ++i, ++of) {
            if (v[i] == v[i + p]) { ++run; if (run > best) best = run; }
            else                  { run = 0; }
        }
        // Half the sequence, IN ONE UNBROKEN STRETCH. A fraction counted loosely would let a wrong
        // length pass on matches scattered through the capture; the right length matches all the way
        // along the part of the capture that is any good, and the bad part is at the ends.
        if (of >= 1 && best >= (of / 2 > 0 ? of / 2 : 1)) return p;
    }
    return v.size();
}

}  // namespace

    // THE UNIQUE KEY: can this stream, on its own, say which turn it is on?
//
// The easy case is one mark that nothing else in the turn looks like — the 36-1's single gap.
// Anomalies are bucketed by size and a bucket holding exactly one is that mark: a decoder
// recognises it the moment it sees it, from one feature and no history.
//
// BUT IDENTICAL MARKS, UNEVENLY SPACED, ARE ALSO A KEY. A 36-2-2-2 carries three gaps of the
// same size at spacings of 150, 180 and 30 degrees, so no single gap says anything — and after
// any two of them the turn is fixed beyond doubt, which is exactly how the ECU's own decoder
// syncs that wheel. Calling it unsyncable was wrong about a wheel the rig spins and the firmware
// reads. What rules the key out is SYMMETRY: if turning the pattern by one mark leaves it
// looking the same, no amount of history separates the marks.
//
// That symmetry cannot actually occur here, because the period was found as the SHORTEST repeat
// and a rotation that maps the cycle onto itself would be a shorter one. It is still tested
// rather than assumed, because the day the period comes from somewhere else that assumption is
// silent and wrong.
void assignKey(StreamFit& f) {
    std::vector<int> sig;
    for (const Anomaly& a : f.anomalies) sig.push_back(static_cast<int>(std::lround(a.ratio * 10.0)));

    for (size_t i = 0; i < sig.size(); ++i) {
        if (std::count(sig.begin(), sig.end(), sig[i]) != 1) continue;
        f.uniqueKey = true;
        f.keyAngle  = f.anomalies[i].atDeg;
        char w[96];
        std::snprintf(w, sizeof(w), f.anomalies[i].ratio < 1.0
                                        ? "a %.2fx short mark no other mark matches"
                                        : "a %.1fx gap no other gap matches",
                      f.anomalies[i].ratio);
        f.keyWhat = w;
        break;
    }

    // Identical marks: the ARRANGEMENT is the key, as long as it is not symmetric. The cycle is
    // each mark paired with the angle to the next one, wrapping round the turn.
    if (!f.uniqueKey && f.anomalies.size() >= 2) {
        const size_t n = f.anomalies.size();
        std::vector<std::pair<int, int>> cyc(n);
        for (size_t i = 0; i < n; ++i) {
            const double here = f.anomalies[i].atDeg;
            const double next = f.anomalies[(i + 1) % n].atDeg + (i + 1 == n ? 360.0 : 0.0);
            cyc[i] = { sig[i], static_cast<int>(std::lround(next - here)) };
        }
        bool symmetric = false;
        for (size_t r = 1; r < n && !symmetric; ++r) {
            bool same = true;
            for (size_t i = 0; i < n && same; ++i) same = (cyc[i] == cyc[(i + r) % n]);
            symmetric = same;
        }
        if (!symmetric) {
            f.uniqueKey = true;
            f.keyAngle  = f.anomalies[0].atDeg;
            f.keyWhat   = "the marks are alike but unevenly spaced — two of them fix the turn";
        }
    }

    if (!f.uniqueKey && !f.anomalies.empty())
        f.keyWhat = "every mark looks like another — this pattern repeats within its own turn";
}

// Fill in a stream from a repeating SEQUENCE of intervals — a pattern with no spike in it. The
// period's own mean is the baseline here, not the fitted wave: within one turn of a short pattern
// the wave and the pattern are the same signal, and dividing one by the other erases both.
bool fillFromSequence(const triggerlog::Lane& lane, const std::vector<size_t>& at,
                      const std::vector<double>& deltas, const std::vector<double>& widths,
                      size_t P, StreamFit& f) {
    if (P < 2 || P >= deltas.size()) return false;

    // THE MIDDLE TURN, NOT THE FIRST. Everything below — the period, the angles, the widths and the
    // ratio of every interval to its turn's mean — comes out of ONE turn of the pattern, so which
    // turn is chosen is the whole answer. Taking the first is taking the worst: a capture of an
    // unknown wheel begins with the engine coming off rest under a starter, and on the bench six
    // slow teeth at the front moved a GM 4200 from 300 rpm to 268, and twelve turned it into a
    // thirteen tooth wheel with six missing. The gap path was cured of this; this path was not.
    std::vector<std::pair<double, size_t>> turns;
    for (size_t m = 0; (m + 1) * P < at.size(); ++m) {
        const double dur = static_cast<double>(lane.bars[at[(m + 1) * P]].tUs) -
                           static_cast<double>(lane.bars[at[m * P]].tUs);
        if (dur > 0.0) turns.push_back({ dur, m * P });
    }
    if (turns.empty()) return false;
    std::sort(turns.begin(), turns.end());
    const size_t b0 = turns[turns.size() / 2].second, b1 = b0 + P;
    f.periodUs = turns[turns.size() / 2].first;
    if (f.periodUs <= 0.0) return false;

    double mean = 0.0;
    for (size_t i = 0; i < P; ++i) mean += deltas[b0 + i];
    mean /= double(P);
    if (mean <= 0.0) return false;

    f.shape   = Shape::Coded;
    f.edges   = static_cast<int>(P);
    f.teeth   = static_cast<int>(P);
    f.missing = 0;
    for (size_t i = 0; i < P; ++i) {
        const double ang = 360.0 * (static_cast<double>(lane.bars[at[b0 + i]].tUs) -
                                    static_cast<double>(lane.bars[at[b0]].tUs)) / f.periodUs;
        f.edgeAngles.push_back(ang);
        if (f.bothEdges) f.widths.push_back(widths[b0 + i]);
        // Every interval that is not the period's own average is a fact about the wheel. The bar is
        // low — 6% — because on this kind of wheel that IS the pattern; anything louder was already
        // caught upstream and this branch would never have run.
        const double r = deltas[b0 + i] / mean;
        if (std::fabs(r - 1.0) >= 0.06) f.anomalies.push_back({ static_cast<int>(i), r, 0, ang });
    }
    f.ok = true;
    return true;
}

double AngleTrack::teethAt(uint32_t t) const {
    if (tUs.empty()) return 0.0;
    if (t <= tUs.front()) return cumTeeth.front();
    if (t >= tUs.back())  return cumTeeth.back();
    const auto it = std::upper_bound(tUs.begin(), tUs.end(), t);
    const size_t hi = static_cast<size_t>(it - tUs.begin());
    const size_t lo = hi - 1;
    const double span = static_cast<double>(tUs[hi]) - static_cast<double>(tUs[lo]);
    if (span <= 0.0) return cumTeeth[lo];
    const double f = (static_cast<double>(t) - static_cast<double>(tUs[lo])) / span;
    return cumTeeth[lo] + f * (cumTeeth[hi] - cumTeeth[lo]);
}

StreamFit fitStream(const triggerlog::Lane& lane) {
    StreamFit f;
    f.stream = lane.stream;
    f.name   = lane.name;
    if (lane.bars.size() < 6) { f.why = "too few edges to see a pattern"; return f; }

    // ONE TOOTH IS RISING-TO-RISING, when both edges were captured.
    //
    // With both edges armed every tooth produces TWO bars — its leading edge and its trailing one —
    // so the raw intervals alternate "how long the tooth is" and "how long the gap after it is", and
    // a median over the mixture is neither. Worse, a wheel whose teeth differ in WIDTH then looks
    // like one whose teeth differ in SPACING, and the landmark lands in the wrong place.
    //
    // So when the levels alternate, the teeth are the rising edges, the interval is rising-to-rising,
    // and the width is the high time — which is the thing a width-coded wheel actually encodes and
    // which this used to discard.
    int alt = 0, pairs = 0;
    for (size_t i = 1; i < lane.bars.size(); ++i) {
        if (lane.bars[i].gapBefore) continue;
        ++pairs;
        if (lane.bars[i].high != lane.bars[i - 1].high) ++alt;
    }
    f.bothEdges = (pairs > 4 && alt * 4 >= pairs * 3);   // mostly alternating ⇒ both edges armed

    std::vector<double> deltas, widths;
    std::vector<size_t> at;
    if (f.bothEdges) {
        // Walk rising edge to rising edge; the falling edge between them is the tooth's width.
        for (size_t i = 1; i + 1 < lane.bars.size(); ++i) {
            if (!lane.bars[i].high) continue;                       // teeth are the rising edges
            if (lane.bars[i].gapBefore || lane.bars[i + 1].gapBefore) continue;
            size_t nxt = i + 2;
            while (nxt < lane.bars.size() && !lane.bars[nxt].high) ++nxt;
            if (nxt >= lane.bars.size()) break;
            deltas.push_back(static_cast<double>(lane.bars[nxt].tUs) - static_cast<double>(lane.bars[i].tUs));
            widths.push_back(static_cast<double>(lane.bars[i + 1].deltaUs));   // high time
            at.push_back(i);
        }
    } else {
        // Bar 0 carries no interval (there is no previous edge), and a bar after a break in the
        // capture measures the break rather than a tooth.
        for (size_t i = 1; i < lane.bars.size(); ++i) {
            if (lane.bars[i].gapBefore) continue;
            deltas.push_back(static_cast<double>(lane.bars[i].deltaUs));
            widths.push_back(0.0);
            at.push_back(i);
        }
    }
    if (deltas.size() < 4) { f.why = "too few usable intervals — the capture is mostly breaks"; return f; }

    f.edgesSeen = static_cast<int>(deltas.size());
    f.pitchUs   = medianOf(deltas);
    // HOW LONG A TOOTH IS HIGH, which is the whole of what a width-coded wheel encodes and is only
    // in the data when both edges were armed. It was read further down and never once written, so
    // it was always zero, so the Width branch could not be reached and no capture in the history of
    // this fitter has ever come back as Width — including the rig's half-moon cam, which is one.
    if (f.bothEdges) f.widthUs = medianOf(widths);
    if (f.pitchUs <= 0.0) { f.why = "the edges carry no timing"; return f; }

    // THE BASELINE MOVES WITH THE ENGINE, because the engine will be on a starter motor.
    //
    // Every capture of an unknown wheel is taken while cranking, and a cranking engine does not turn
    // at a speed — it surges and stalls against each compression, swinging 50% either side of its
    // mean within a single revolution. Measured against a GLOBAL median a slow patch reads as teeth
    // missing and a fast one hides a real gap: a 36-1 at +-45% swing came back as 35-0 with nine
    // anomalies, at +-60% as 39-4, and a 60-2 as 68-10. All three are the arithmetic working
    // perfectly on the wrong question.
    //
    // So every interval is judged against its OWN NEIGHBOURHOOD — the median of the teeth around it,
    // which rises and falls with the engine. A window wide enough to be steady, narrow enough to
    // follow the surge, and a median so the gap inside the window does not drag its own baseline up.
    // This is what a decoder does too, and for the same reason.
    // THE TWO TEETH EITHER SIDE, AND NOTHING WIDER.
    //
    // A gap is ONE long interval between two ordinary ones. A surge makes MANY consecutive intervals
    // long TOGETHER — which is the whole difference, and the only one that survives a starter motor.
    // Measured against the mean of its immediate neighbours a gap reads ~2 whatever the engine is
    // doing, because the neighbours move with it:
    //
    //              swing      gap      next highest
    //               25%       1.98        1.02
    //               45%       1.92        1.05
    //               60%       1.84        1.08
    //
    // Every wider baseline was tried and measured first. A global median gave 36-1 -> 39-4 at 60%.
    // A nine-tooth centred median closed that to one anomaly but read 1.76/1.55/1.50 against a
    // next-highest of 1.11/1.28/1.50 — separation gone by 60%, because a window that wide spans a
    // quarter of the surge AND contains the gap, so the gap helps set the baseline it is judged by.
    // A trailing median tracked the surge and then missed gaps during acceleration outright.
    // FIRST FIND THE WAVE, THEN FIND THE WHEEL. Two passes, because they are two different questions
    // and answering them together is what kept failing.
    //
    // Pass one marks the obvious spikes using the neighbours, which is a LINEAR de-trend — it assumes
    // the speed between two teeth is the average of them. That holds while the surge is gentle and
    // breaks when it is not: at 60% swing the wave has enough CURVATURE between adjacent teeth that
    // the estimate is wrong by more than a gap is long, and a 36-1 came back as 178-4.
    //
    // Pass two builds the speed curve from the teeth that are NOT spikes — interpolating straight
    // across the ones that are — so the baseline is the engine's own oscillation, uncontaminated by
    // the wheel's geometry. Every interval is then judged against where the engine actually was.
    std::vector<double> local(deltas.size(), f.pitchUs);
    {
        // THE WAVE IS A FUNCTION OF ANGLE, NOT OF INTERVAL NUMBER.
        //
        // A gap is one interval and several teeth. Fitting the engine's oscillation against interval
        // index therefore squeezes the x-axis at exactly the place being measured — the wave appears
        // to run fast across every gap — and the curve comes out wrong where it matters most. On a
        // six at 60% swing that cost a 36-1 its gap: measured 1.61 against a true 2.00, and no
        // window width fixed it because the span was never the problem.
        //
        // So the axis is ANGLE, in teeth: an ordinary interval advances one, a gap advances however
        // many it swallowed. That is not known before the gaps are found, which is why this iterates
        // — rough spans, fit the wave, re-measure the spans against it, fit again. Three passes: the
        // first only has to hand the second an axis straight enough to measure the wave's period on,
        // and the third settles the spans against the wave that period bought.

        // A first, crude guess at the spans, from each interval against its immediate neighbours.
        // It only has to be good enough to keep a gap from posing as several teeth on the axis and
        // from drowning out the compressions when the wave's period is measured; the passes below
        // refine it against the wave itself.
        std::vector<double> span(deltas.size(), 1.0);
        std::vector<char>   seeded(deltas.size(), 0);
        for (size_t k = 1; k + 1 < deltas.size(); ++k) {
            const double n = 0.5 * (deltas[k - 1] + deltas[k + 1]);
            const double r = n > 0.0 ? deltas[k] / n : 1.0;
            if (r >= kAnomalyRatio) { span[k] = std::max(2.0, std::round(r)); seeded[k] = 1; }
        }

        for (int pass = 0; pass < 3; ++pass) {
            // Where each interval SITS on that axis is its own middle, not its leading edge. An
            // ordinary tooth is half a tooth wide; a gap of two is a whole one. Measuring both from
            // their left edges asks the wave for its value a tooth early on precisely the interval
            // whose answer matters, and on a falling wave that reads the gap short.
            std::vector<double> mid(deltas.size(), 0.0);
            double acc = 0.0;
            for (size_t k = 0; k < deltas.size(); ++k) { mid[k] = acc + 0.5 * span[k]; acc += span[k]; }

            // HOW LONG IS ONE COMPRESSION? The window has to be short enough to follow the wave and
            // long enough to average the noise, and the wave's own period is what separates the two.
            // Jason's observation is that the cylinder count and the stroke already give it — a
            // four-stroke pulls cyl/2 compressions per revolution — but the capture need not be told,
            // because the teeth carry it. Resample the PER-TOOTH duration onto the angle axis, so a
            // gap contributes its own teeth at their own rate rather than one huge spike, and
            // autocorrelate: a gap left in as a spike is the most periodic thing on the wheel and
            // the search would lock onto the revolution instead of the compressions.
            //
            // Then take the SMALLEST lag that is within a hair of the best one, not the best. An
            // oscillation correlates just as well against two of its cycles as against one, so the
            // strongest lag is some multiple; the fundamental is the first peak, not the tallest.
            double period = 0.0;
            {
                const size_t n = static_cast<size_t>(acc);
                std::vector<double> y(n, 0.0);
                for (size_t k = 0, a = 0; k < deltas.size(); ++k) {
                    const double per = deltas[k] / span[k];
                    for (double q = 0; q < span[k] && a < n; ++q, ++a) y[a] = per;
                }
                double m = 0; for (double v : y) m += v;
                m = n ? m / n : 0.0;
                double var = 0; for (double v : y) var += (v - m) * (v - m);
                // Not on the first pass. The period is measured off the angle axis, the axis is
                // built from the spans, and on the first pass the spans are only the crude
                // neighbour guess. On a lumpy six that guess stretches the axis enough to hide the
                // compressions completely — the search came back with 39 teeth for a 9-tooth wave.
                // So the first pass runs the plain parabola, whose only job is to give the second
                // pass an axis worth measuring.
                if (pass > 0 && var > 0.0 && n >= 24) {
                    const size_t lo = 4, hi = n / 3;
                    std::vector<double> r(hi + 1, 0.0);
                    double best = 0.0;
                    for (size_t L = lo; L <= hi; ++L) {
                        double acc2 = 0;
                        for (size_t a = 0; a + L < n; ++a) acc2 += (y[a] - m) * (y[a + L] - m);
                        r[L] = (acc2 / double(n - L)) / (var / double(n));
                        best = std::max(best, r[L]);
                    }
                    if (best > 0.35)
                        for (size_t L = lo; L <= hi; ++L)
                            if (r[L] >= 0.85 * best) { period = double(L); break; }
                }
            }
            // FIT THE WAVE AT ITS OWN FREQUENCY, rather than approximating it locally. Once the
            // period is known the compressions are a sinusoid in angle, so that is what is fitted —
            // a constant and a slope for the cranking speed climbing under the starter, plus a
            // cosine and a sine at the compression frequency for the pumping. Four unknowns, one
            // window a period either side, and it tracks a lumpy six as exactly as a lazy four.
            //
            // A parabola was doing this job and it cannot: over a 9-tooth period at 60% swing it has
            // to bend through 120 degrees of sine inside its window, and it cuts the corner by more
            // than a missing tooth is worth. Widening the window made it worse and narrowing it left
            // too few teeth to fit. The error was the model, not the span. Without a period there is
            // nothing to fit a sinusoid TO, so a steady capture keeps the parabola.
            //
            // What is fitted is the RATE — teeth per microsecond — and not the duration of a tooth.
            // It is the crankshaft that swings sinusoidally; a tooth's duration is one over that,
            // and one over a sine is not a sine. At 20% swing the difference hardly shows, but by
            // 60% the duration curve has grown a sharp peak where the engine is slowest and a
            // sinusoid fitted to it misses by more than the thing being measured.
            const bool   wave = period >= 6.0;
            const double kW   = wave ? period : 3.5;
            const double w    = 2.0 * 3.14159265358979 / (wave ? period : 1.0);
            const int    nb   = wave ? 4 : 3;
            for (size_t k = 0; k < deltas.size(); ++k) {
                double m[4][5] = {};
                int rows = 0;
                for (size_t j = 0; j < deltas.size(); ++j) {
                    if (span[j] > 1.5) continue;
                    const double dx = mid[j] - mid[k];
                    if (std::fabs(dx) > kW) continue;
                    const double b[4] = { 1.0, dx, wave ? std::cos(w * dx) : dx * dx,
                                                    wave ? std::sin(w * dx) : 0.0 };
                    const double y = span[j] / deltas[j];          // teeth per microsecond
                    for (int r = 0; r < nb; ++r) {
                        for (int c = 0; c < nb; ++c) m[r][c] += b[r] * b[c];
                        m[r][nb] += b[r] * y;
                    }
                    ++rows;
                }
                // The wanted answer is the fit AT dx = 0, and every basis but the constant and the
                // cosine is zero there — so it is the first unknown plus the third, once solved.
                double sol[4] = {};
                bool ok = rows >= nb + 1;
                for (int c = 0; ok && c < nb; ++c) {
                    int piv = c;
                    for (int r = c + 1; r < nb; ++r)
                        if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
                    if (std::fabs(m[piv][c]) < 1e-12) { ok = false; break; }
                    for (int q = c; q <= nb; ++q) std::swap(m[c][q], m[piv][q]);
                    for (int r = 0; r < nb; ++r) {
                        if (r == c) continue;
                        const double fct = m[r][c] / m[c][c];
                        for (int q = c; q <= nb; ++q) m[r][q] -= fct * m[c][q];
                    }
                }
                if (ok) for (int r = 0; r < nb; ++r) sol[r] = m[r][nb] / m[r][r];
                const double v = ok ? (wave ? sol[0] + sol[2] : sol[0]) : 0.0;
                if (ok && v > 0.0) {
                    local[k] = 1.0 / v;                               // back to a tooth's duration
                } else {
                    double sy = 0; int n = 0;
                    for (size_t j = 0; j < deltas.size(); ++j) {
                        if (span[j] > 1.5 || std::fabs(mid[j] - mid[k]) > kW) continue;
                        sy += span[j] / deltas[j]; ++n;
                    }
                    local[k] = (n && sy > 0.0) ? double(n) / sy : f.pitchUs;
                }
            }

            // Re-measure every interval against the wave and let the spans follow. The axis for the
            // next pass is built from these, which is the whole point of going round again.
            //
            // HOW BIG THE GAPS ARE — NOT WHERE THEY ARE. Which intervals are gaps was settled above,
            // against each interval's immediate neighbours, and that judgement does not depend on the
            // wave at all: a gap is long compared to the teeth either side of it however the engine
            // is behaving. What the wave adds is the SIZE, because a gap spanning two teeth while the
            // engine accelerates is not twice any one tooth near it.
            //
            // Letting these passes also DISCOVER gaps is a runaway. A cranked 36-2-2-2 seeded its 33
            // gaps correctly and then grew to 50 and 86: each false gap is dropped from the next
            // wave fit, which bends the wave, which manufactures the next false gap. It ended up
            // measuring its three 3.00 gaps as 7.05, 5.41 and 5.42.
            //
            // An interval that reads long is still held to be long even when the ratio is not near a
            // whole number. Rounding is how the axis gets built, not how the gap gets judged, and
            // refusing to round an imprecise one straightens the axis and loses the gap for good.
            // Strict to find a new one, loose to keep a known one. A gap the neighbour test misses —
            // and it misses gaps that sit next to other gaps, because then the neighbours are gaps
            // too — has to be findable here, or a 36-2-2-2 loses two of its three. But finding them
            // at the same 1.5 that keeps them is what runs away.
            for (size_t k = 0; k < deltas.size(); ++k) {
                const double r    = (local[k] > 0.0) ? deltas[k] / local[k] : 1.0;
                const double need = seeded[k] ? kAnomalyRatio : 1.9;
                if (r >= need) { span[k] = std::max(2.0, std::round(r)); seeded[k] = 1; }
                else           { span[k] = 1.0; }
            }
        }

        // THE CLOCK, out of the same spans the wave was fitted against. Each tooth's time and how
        // many nominal teeth had gone by when it arrived — a gap contributing the teeth it swallowed,
        // which is the whole reason the spans had to be settled first.
        f.track.tUs.reserve(at.size());
        f.track.cumTeeth.reserve(at.size());
        double acc2 = 0.0;
        for (size_t k = 0; k < at.size(); ++k) {
            f.track.tUs.push_back(lane.bars[at[k]].tUs);
            f.track.cumTeeth.push_back(acc2);
            acc2 += (k < span.size()) ? span[k] : 1.0;
        }
    }

    // Every departure from uniform, in order. Indexed into the TOOTH series (k), not into lane.bars,
    // because with both edges armed those are not the same stride.
    struct Raw { size_t k; double ratio; };
    std::vector<Raw> raw;
    for (size_t k = 0; k < deltas.size(); ++k) {
        const double ratio = deltas[k] / local[k];
        if (isAnomaly(ratio)) raw.push_back({ k, ratio });
    }

    // NO SPIKES. That leaves three things it could still be, and the order they are asked in is the
    // answer: SPACING FIRST, because spacing is the stronger signal and a wheel that repeats a
    // pattern of angles is describing itself with them. Only a stream whose spacing says nothing at
    // all is then asked about its tooth WIDTHS, and only then is it called even.
    //
    // Asked the other way round, a Nissan CAS window track and a 6G72 cam — whose whole identity is
    // their uneven spacing, and whose teeth also happen to differ in width — came back as Width, and
    // a Width stream has no period, so the capture could not find a revolution at all and refused.
    if (raw.size() < 2) {
        if (const size_t P = sequencePeriod(deltas); P && fillFromSequence(lane, at, deltas, widths, P, f)) {
            assignKey(f);
            return f;
        }

        // A WIDTH-CODED WHEEL HAS NO SPACING ANOMALY AT ALL — its teeth are evenly spaced and the
        // wide one is the landmark. Looked for only when both edges were captured, because with one
        // edge armed the width is simply not in the data and guessing at it would invent a wheel.
        if (f.bothEdges && f.widthUs > 0.0 && !widths.empty()) {
            double wMin = widths[0], wMax = widths[0];
            for (double w : widths) { wMin = std::min(wMin, w); wMax = std::max(wMax, w); }
            if (wMax > wMin * 1.5) {                      // two clearly different tooth widths
                f.shape  = Shape::Width;
                f.widths = widths;
                f.ok     = true;

                // AND IT KNOWS WHERE ITS TURN BEGINS. The wide tooth comes round once a period —
                // that is what makes it a landmark — so the teeth between two of them is the tooth
                // count and the time between them is the period. This used to return with neither,
                // on the grounds that fitCapture would supply the turn; fitCapture cannot, because
                // it takes the turn FROM the streams, so a capture whose only patterned wire was
                // width-coded had nothing to measure a revolution against and was refused outright.
                std::vector<size_t> wide;
                for (size_t i = 0; i < widths.size(); ++i)
                    if (widths[i] > wMax * 0.9) wide.push_back(i);
                if (wide.size() >= 2) {
                    std::vector<double> gapT; std::vector<double> gapN;
                    for (size_t i = 0; i + 1 < wide.size(); ++i) {
                        gapT.push_back(static_cast<double>(lane.bars[at[wide[i + 1]]].tUs) -
                                       static_cast<double>(lane.bars[at[wide[i]]].tUs));
                        gapN.push_back(static_cast<double>(wide[i + 1] - wide[i]));
                    }
                    std::sort(gapT.begin(), gapT.end()); std::sort(gapN.begin(), gapN.end());
                    const double per = gapT[gapT.size() / 2];
                    const int    n   = static_cast<int>(std::lround(gapN[gapN.size() / 2]));
                    if (per > 0.0 && n >= 2) {
                        f.periodUs = per;
                        f.edges    = n;
                        f.teeth    = n;
                        f.widths.assign(widths.begin() + wide[0], widths.begin() + wide[0] + n);
                        for (int k = 0; k < n; ++k)
                            f.edgeAngles.push_back(360.0 *
                                (static_cast<double>(lane.bars[at[wide[0] + size_t(k)]].tUs) -
                                 static_cast<double>(lane.bars[at[wide[0]]].tUs)) / per);
                    }
                }
                return f;
            }
        }

        // A UNIFORM TRAIN IS A RESULT. It has no landmark, so it cannot say where it begins or how
        // many teeth it has — that has to come from another stream, or from the person. Reporting it
        // as a failure is what made a Nissan CAS unfittable: its crank track is exactly this.
        f.shape = Shape::Even;
        f.ok    = true;
        f.why   = raw.empty() ? "" : "one departure only — not enough to call it a pattern";
        return f;
    }

    // Teeth between one anomaly and the next, counting absent ones. This is the sequence whose
    // period is one turn of the pattern.
    // Each entry: how big this anomaly is and how many teeth to the next one.
    //
    // An anomaly that measures a whole number of teeth keys on that whole number, because a three
    // tooth gap read as 2.96 here and 3.10 there is one gap, not two. Only an anomaly that is NOT a
    // whole number keys on tenths, which is what tells a width-coded 2.7 from a 3.4 — and those can
    // never be mistaken for a whole one, since anything within rounding of a whole took the other
    // branch. Keying everything on tenths is what left a 36-2-2-2 unable to find its own turn.
    std::vector<std::pair<int, int>> between;
    for (size_t k = 0; k + 1 < raw.size(); ++k) {
        int n = 0;
        for (size_t i = raw[k].k + 1; i <= raw[k + 1].k && i < deltas.size(); ++i) {
            const double ratio = deltas[i] / local[i];
            n += isGap(ratio) ? static_cast<int>(std::lround(ratio)) : 1;
        }
        const double r = raw[k].ratio;
        between.push_back({ isGap(r) ? static_cast<int>(std::lround(r)) * 10
                                     : static_cast<int>(std::lround(r * 10.0)), n });
    }
    // MARKS FOUND, BUT NO TURN MADE OF THEM. A wheel whose every tooth is its own angle — a GM 4200,
    // a CAS window track — throws marks the spike test can see and a spacing sequence it cannot make
    // a period of, because the "teeth between" it counts are not teeth. The sequence test answers
    // the same capture properly, so it is asked before giving up rather than after.
    const auto seqFallback = [&]() -> bool {
        const size_t P = sequencePeriod(deltas);
        if (!P || !fillFromSequence(lane, at, deltas, widths, P, f)) return false;
        f.why.clear();
        assignKey(f);
        return true;
    };
    if (between.empty()) {
        if (seqFallback()) return f;
        f.why = "anomalies do not repeat";
        return f;
    }

    const size_t period = shortestPeriod(between);
    if (period >= raw.size()) {
        if (seqFallback()) return f;
        f.why = "not enough complete turns to measure one";
        return f;
    }

    // THE REPRESENTATIVE TURN, NOT THE FIRST ONE.
    //
    // A capture of an unknown wheel starts with the engine spinning up — that is what cranking IS —
    // so its first turn is the least typical thing in it. Taking it gave a 36-1 at 300 rpm as 358,
    // because the stim's first two dozen teeth went by before the commanded speed took effect, and
    // at a harder swing it defeated the fit completely. A real starter does the same from rest.
    //
    // Every turn in the capture is measured and the MIDDLE one is taken, so a transient at either
    // end is outvoted rather than trusted. It also gives the angles something honest to be drawn
    // against: a turn during spin-up draws a wheel whose teeth are all in the wrong places.
    std::vector<std::pair<double, size_t>> turns;
    for (size_t i = 0; i + period < raw.size(); ++i) {
        const double dur = static_cast<double>(lane.bars[at[raw[i + period].k]].tUs) -
                           static_cast<double>(lane.bars[at[raw[i].k]].tUs);
        if (dur > 0.0) turns.push_back({ dur, i });
    }
    if (turns.empty()) { f.why = "the pattern does not advance in time"; return f; }
    std::sort(turns.begin(), turns.end());
    const size_t pick = turns[turns.size() / 2].second;

    const size_t b0 = raw[pick].k, b1 = raw[pick + period].k;
    f.periodUs = turns[turns.size() / 2].first;
    f.edges = static_cast<int>(b1 - b0);

    // WHOLE MISSING TEETH, OR SOMETHING ELSE. Every anomaly in the period a near-whole multiple of
    // the pitch is a gap wheel; anything else is coded — a width, a span, a deliberate irregularity
    // — and calling that "n teeth missing" would describe a wheel nobody built.
    bool allWhole = true;
    int  idx = 0, absent = 0;
    for (size_t i = b0 + 1; i <= b1 && i < deltas.size(); ++i) {
        const double ratio = deltas[i] / local[i];
        if (isAnomaly(ratio)) {
            const bool whole = isGap(ratio);
            if (!whole) allWhole = false;
            const int miss = whole ? static_cast<int>(std::lround(ratio)) - 1 : 0;
            f.anomalies.push_back({ idx, ratio, miss,
                                    360.0 * (static_cast<double>(lane.bars[at[i]].tUs) -
                                             static_cast<double>(lane.bars[at[b0]].tUs)) / f.periodUs });
            for (int k = 0; k < miss; ++k) { f.gapPos.push_back(idx++); ++absent; }
        }
        ++idx;
    }
    f.shape   = allWhole ? Shape::Gap : Shape::Coded;
    f.missing = absent;
    f.teeth   = f.edges + absent;
    std::sort(f.gapPos.begin(), f.gapPos.end());

    // One period's edges as angles. Time against the period, so an engine turning unevenly draws
    // unevenly rather than being quietly regularised into the answer somebody hoped for.
    for (size_t i = b0; i < b1 && i < deltas.size(); ++i) {
        f.edgeAngles.push_back(360.0 * (static_cast<double>(lane.bars[at[i]].tUs) -
                                        static_cast<double>(lane.bars[at[b0]].tUs)) / f.periodUs);
        if (f.bothEdges) f.widths.push_back(widths[i]);
    }

    if (f.shape == Shape::Gap && f.teeth < 2) { f.ok = false; f.why = "fewer than two teeth per turn"; return f; }

    // IS THIS A GAP WHEEL AT ALL? A missing-tooth wheel loses a SMALL PART of itself: one tooth in
    // thirty-six, two in sixty, and even a 12-3 only a quarter. A wheel read as having lost nearly
    // half its teeth has not lost them — its teeth were never evenly spaced to begin with, and it is
    // a wheel whose every tooth is its own angle.
    //
    // The GM 4200 is the case: fifty, sixty, sixty, ten, fifty, sixty and seventy degrees. Measured
    // against a baseline its ten degree tooth drags down, the rest look like multi-tooth gaps, and it
    // came out as thirteen teeth with six missing — a wheel nobody built. It used to fail to make a
    // turn of its marks and fall through to the sequence test, which reads it exactly; once the turn
    // search learned to tolerate a bad mark it began succeeding here instead, with a worse answer and
    // more confidence. Being able to force an answer is not the same as the answer being right.
    // ...or where most of the teeth are departures. A 36-1 has one mark in thirty-five; an FE3 cam
    // runs 330, 30 and 360 degrees, so two of its three intervals depart and the thing it is being
    // measured against — a regular train — does not exist. Both tests are needed: the GM 4200 trips
    // only the missing-teeth one, the FE3 cam only this one.
    if (f.missing * 3 > f.teeth || f.anomalies.size() * 3 > static_cast<size_t>(f.edges)) {
        StreamFit alt = f;
        alt.anomalies.clear(); alt.gapPos.clear(); alt.edgeAngles.clear(); alt.widths.clear();
        alt.missing = 0; alt.teeth = 0; alt.edges = 0; alt.periodUs = 0.0;
        const size_t P = sequencePeriod(deltas);
        if (P && fillFromSequence(lane, at, deltas, widths, P, alt)) {
            alt.why.clear();
            assignKey(alt);
            return alt;
        }
    }

    assignKey(f);

    f.ok = true;
    return f;
}

CaptureFit fitCapture(const std::vector<triggerlog::Lane>& lanes) {
    CaptureFit c;
    std::vector<size_t> srcOf;              // which lane each fitted stream came from
    for (size_t i = 0; i < lanes.size(); ++i) {
        if (lanes[i].bars.size() < 6) continue;
        c.streams.push_back(fitStream(lanes[i]));
        srcOf.push_back(i);
    }
    if (c.streams.empty()) { c.why = "no stream carried enough edges to read"; return c; }

    // THE CRANK IS THE DENSEST WIRE. Not lane 0, and not "the one that fitted": a Nissan CAS puts
    // its only PATTERN on the cam, and reading that as the crank would call a 720-degree cycle one
    // revolution and halve every angle downstream. Whatever is producing edges fastest is turning
    // with the crank — that is what being the crank means.
    size_t dense = 0;
    for (size_t i = 1; i < c.streams.size(); ++i)
        if (c.streams[i].ok && c.streams[i].pitchUs > 0.0 &&
            (!c.streams[dense].ok || c.streams[i].pitchUs < c.streams[dense].pitchUs)) dense = i;
    c.crankIdx = static_cast<int>(dense);

    // A revolution comes from whichever stream HAS a period. If that is the crank itself, its period
    // is one revolution. If it is a sparser stream — a cam — its period is one engine CYCLE, and a
    // revolution is half of it.
    //
    // Run TWICE: once on what each stream made of itself, and again after the cams have been re-read
    // against the crank's clock, because a cam that changes shape changes what it says about the
    // engine's speed. A re-read stream comes back from fitStream with no rate on it at all — it is a
    // lane, and a lane cannot know whether its period is a revolution or a cycle — so leaving this
    // to the first pass would quietly strip the cams of being cams.
    const auto rate = [&] {
        c.revUs = 0.0;
        for (size_t i = 0; i < c.streams.size(); ++i) {
            StreamFit& s = c.streams[i];
            if (!s.ok || s.periodUs <= 0.0) continue;
            const bool isCrank = (static_cast<int>(i) == c.crankIdx);
            s.cycleRatio = isCrank ? 1 : 2;
            const double rev = isCrank ? s.periodUs : s.periodUs * 0.5;
            if (c.revUs <= 0.0 || rev < c.revUs) c.revUs = rev;
        }
    };
    rate();
    if (c.revUs <= 0.0) {
        // SAY WHAT WAS MEASURED. Edges per second is teeth-per-turn times turns-per-second: one
        // equation and two unknowns, and no capture length or timing resolution produces a third.
        // The pitch is a real measurement even so, and reporting it is the difference between "this
        // cannot be answered from a capture alone" and "this failed".
        char w[192];
        const StreamFit& s0 = c.streams[size_t(c.crankIdx >= 0 ? c.crankIdx : 0)];
        std::snprintf(w, sizeof(w),
                      "every stream is a uniform train — %d even teeth at %.0f us apart, and with no "
                      "landmark the tooth count and the speed cannot be told apart",
                      s0.edgesSeen, s0.pitchUs);
        c.why = w;
        return c;
    }
    c.rpm = 60.0e6 / c.revUs;

    // An EVEN stream now has a period after all: the capture supplied it. Its tooth count is how
    // many of its pitches fit the turn it belongs to — which is the whole reason a 360-slit crank
    // track is readable at all, and it needs the cam to say so.
    const auto evens = [&] {
        for (size_t i = 0; i < c.streams.size(); ++i) {
            StreamFit& s = c.streams[i];
            if (!s.ok || s.shape != Shape::Even || s.pitchUs <= 0.0) continue;
            const bool isCrank = (static_cast<int>(i) == c.crankIdx);
            s.cycleRatio = isCrank ? 1 : 2;
            s.periodUs   = isCrank ? c.revUs : c.revUs * 2.0;
            s.teeth      = static_cast<int>(std::lround(s.periodUs / s.pitchUs));
            s.edges      = s.teeth;
            s.missing    = 0;
        }
    };
    evens();

    // ============ THE CRANK STANDS FOR THE ENGINE, SO THE CAMS ARE READ AGAINST IT ============
    //
    // Jason's point, and it settles something that could not be settled any other way. A cam has a
    // handful of edges per engine cycle. The engine underneath it is surging against compressions
    // several times a revolution. Those two are the same signal on one wire, and a cam has nowhere
    // near enough samples to separate them — asked to fit a wave, it fits the surge to its OWN
    // pattern and hands back the engine dressed as a wheel.
    //
    // It does not have to. The crank is on the same engine and has thirty to a hundred and eighty
    // edges a revolution, so it has already measured what the engine did, instant by instant, with
    // no model of a wave anywhere in it. Put the cam's edges on the crank's angle scale and the
    // surge is not modelled or approximated — it is GONE, because a cam edge at a crank angle is a
    // fact about the wheel and nothing else.
    //
    // The cam is re-timed onto a perfectly steady engine turning at the capture's own mean speed and
    // fitted again, so everything downstream — periods, ratios, the cycle test — reads unchanged.
    if (c.crankIdx >= 0 && c.revUs > 0.0 && srcOf.size() == c.streams.size()) {
        const AngleTrack track = c.streams[size_t(c.crankIdx)].track;
        const int crankTeeth   = c.streams[size_t(c.crankIdx)].teeth;
        // Only a crank worth trusting as a clock. Interpolating between its teeth assumes the speed
        // barely moves from one to the next, which holds for a 36-1 and does not hold for a four
        // tooth distributor — there a whole compression happens between two edges, and the "clock"
        // would carry the very error it exists to remove.
        bool changed = false;
        if (track.usable() && crankTeeth >= 12) {
            const double degPerTooth = 360.0 / static_cast<double>(crankTeeth);
            const double usPerDeg    = c.revUs / 360.0;
            for (size_t i = 0; i < c.streams.size(); ++i) {
                if (static_cast<int>(i) == c.crankIdx) continue;
                const triggerlog::Lane& src = lanes[srcOf[i]];
                if (src.bars.size() < 6) continue;

                // OUTSIDE THE CRANK'S SPAN THERE IS NO ANGLE. Those edges are dropped rather than
                // held at the ends, which would pile them onto one instant and read as a cluster of
                // impossibly short intervals — a pattern, invented at the edge of the capture.
                //
                // Dropped, not refused. The two streams are armed at the same moment but their first
                // teeth do not arrive together, and the crank's usable track starts a tooth or two in
                // besides; on the bench a Nissan CAS lost its whole cam re-read to a single cam edge
                // that preceded the crank's first tooth by a few hundred microseconds.
                triggerlog::Lane flat;
                flat.stream = src.stream;
                flat.name   = src.name;
                double prev = 0.0; bool first = true, dropped = false;
                for (const triggerlog::Interval& b : src.bars) {
                    if (b.tUs < track.tUs.front() || b.tUs > track.tUs.back()) { dropped = true; continue; }
                    const double us = track.teethAt(b.tUs) * degPerTooth * usPerDeg;
                    triggerlog::Interval o = b;
                    o.tUs       = static_cast<uint32_t>(us);
                    o.deltaUs   = first ? 0u : static_cast<uint32_t>(us - prev);
                    // A dropped edge is a HOLE, and an interval measured across it is not a tooth.
                    o.gapBefore = b.gapBefore || (first && dropped);
                    flat.bars.push_back(o);
                    prev = us; first = false;
                }
                if (flat.bars.size() < 6) continue;

                StreamFit re = fitStream(flat);
                // Taken only if it is an ANSWER. A cam read against the crank should be easier, not
                // harder, and if it comes back worse than what it managed on its own then the clock
                // is telling us something we do not understand and the honest thing is not to use it.
                if (re.ok && (re.periodUs > 0.0 || c.streams[i].periodUs <= 0.0)) {
                    re.stream = c.streams[i].stream;
                    re.name   = c.streams[i].name;
                    c.streams[i] = re;
                    changed = true;
                }
            }
            // The cams may have changed shape, so what the capture believes about the engine's speed
            // has to be settled again from what it now holds.
            if (changed) { rate(); evens(); if (c.revUs > 0.0) c.rpm = 60.0e6 / c.revUs; }
        }
    }

    // A WIDTH-CODED STREAM'S KEY IS ITS ODD TOOTH, and that only becomes locatable once a turn is
    // known — which is why it waits until here. The widest tooth, if there is exactly one.
    for (size_t i = 0; i < c.streams.size(); ++i) {
        StreamFit& s = c.streams[i];
        if (!s.ok || s.shape != Shape::Width || s.widths.empty()) continue;

        // IT IS ALSO A STREAM THAT TURNS AT SOME RATE, and it got told nothing. A width-coded track
        // comes out of fitStream with no period — its identity is in how long each tooth is HIGH,
        // and that is measurable without knowing where the turn begins — so it fell through the two
        // loops above, which ask for a period or for an even train. It kept cycleRatio 0, which
        // meant the capture did not count it as phase and the designer did not adopt it as a cam:
        // a width-coded cam simply was not in the trigger system that came out.
        const bool isCrank = (static_cast<int>(i) == c.crankIdx);
        s.cycleRatio = isCrank ? 1 : 2;
        s.periodUs   = isCrank ? c.revUs : c.revUs * 2.0;
        s.teeth      = static_cast<int>(s.widths.size());
        s.edges      = s.teeth;

        const double wMax = *std::max_element(s.widths.begin(), s.widths.end());
        int wide = 0; size_t at = 0;
        for (size_t i = 0; i < s.widths.size(); ++i)
            if (s.widths[i] > wMax * 0.9) { ++wide; at = i; }
        if (wide == 1) {
            s.uniqueKey = true;
            s.keyAngle  = 360.0 * static_cast<double>(at) / static_cast<double>(s.widths.size());
            s.keyWhat   = "one tooth wider than the rest";
        } else {
            s.keyWhat = "several teeth share the widest width — no single landmark";
        }
    }

    // WHO ANCHORS THE ENGINE.
    //
    // The crank first, because a landmark on the crank locates the engine to a revolution and costs
    // nothing. When the crank has no unique key — an even 360 disc, or a pattern that repeats within
    // its own turn — it can count teeth and never say which turn it is on, and then the cam must
    // provide BOTH: the position reference and the half-speed phase. That is only possible if the
    // CAM's own pattern has a unique key, which is why every stream is asked the same question.
    for (const StreamFit& s : c.streams) if (s.cycleRatio == 2) c.phase = true;

    if (c.crankIdx >= 0 && c.streams[size_t(c.crankIdx)].uniqueKey) {
        c.syncIdx  = c.crankIdx;
        c.canSync  = true;
        c.syncNote = c.phase ? "crank locates the turn, cam picks the revolution — sequential"
                             : "crank locates the turn; no cam, so wasted spark only";
    } else {
        int cam = -1;
        for (size_t i = 0; i < c.streams.size(); ++i)
            if (static_cast<int>(i) != c.crankIdx && c.streams[i].ok && c.streams[i].uniqueKey) { cam = int(i); break; }
        if (cam >= 0) {
            c.syncIdx  = cam;
            c.canSync  = true;
            c.syncNote = "the crank carries no unique mark — the cam provides both the reference and the phase";
        } else {
            c.canSync  = false;
            c.syncNote = c.phase
                ? "neither the crank nor the cam carries a mark that happens only once — nothing can sync"
                : "the crank carries no unique mark and there is no cam — nothing can sync";
        }
    }

    c.ok = true;
    return c;
}

}  // namespace triggerfit

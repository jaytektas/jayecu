#include "TriggerWheel.h"
#include "Cache.h"
#include "StudioPaths.h"
#include <algorithm>
#include <system_error>
#include <fstream>
#include <filesystem>
#include <cstdlib>

#include <cmath>
#include <set>

// ---- stream constructors ---------------------------------------------------------------------
WheelStream gapStream(int rate, int slots, int ratio, std::vector<int> cell) {
    WheelStream s; s.rate = rate; s.prim = 0; s.slots = slots; s.ratio = ratio; s.cell = std::move(cell);
    return s;
}
WheelStream seqStream(int rate, std::vector<int> cell) {
    WheelStream s; s.rate = rate; s.prim = 1; s.cell = std::move(cell);
    return s;
}
WheelStream widStream(int rate, int wMin, int wMax, int wTgt) {
    WheelStream s; s.rate = rate; s.prim = 2; s.wMin = wMin; s.wMax = wMax; s.wTgt = wTgt;
    return s;
}

// ---- wheel → config pairs ----------------------------------------------------------------------
namespace {
// THE STREAM INDEX IS THE ROLE — trigger.streams[] has one slot per role. Slots 0-1 are the cranks,
// 2-5 the four cams. There is no role field and no stream count; a slot is live iff `enabled`.
//
// This replaces a role that was carried as a value and seeded from POSITION, which could not
// describe a V engine with phasers on the intakes alone (Intake B1 and Intake B2 are slots 2 and 4,
// never 2 and 3) and which moved a cam to the wrong role when such a config was read off an ECU and
// written back. Position-derived identity is gone because identity is now the position.
constexpr int SLOT_CRANK_PRIMARY = kSlotCrankPrimary;   // the header owns the numbering now
constexpr int SLOT_CAM_BASE      = kSlotCamBase;
constexpr int SLOT_COUNT         = kSlotCount;
bool slotIsCam(int i) { return i >= SLOT_CAM_BASE && i < SLOT_COUNT; }

// Where a wheel's Nth crank / Nth cam stream lives. The wheel model lists streams in the order a
// wheel describes them; the config addresses them by role.
int slotFor(int rate, int crankN, int camN) {
    return (rate == 0) ? (SLOT_CRANK_PRIMARY + crankN) : (SLOT_CAM_BASE + camN);
}
}  // namespace

std::vector<std::pair<std::string, double>> wheelPairs(const Wheel& w) {
    std::vector<std::pair<std::string, double>> pairs;
    auto add = [&](std::string path, double v) { pairs.emplace_back(std::move(path), v); };

    // ENGINEERING UNITS, like wheelFromConfig reads — degrees here, not tenths. The caller converts to
    // raw through the field's own scale (see onApplyToEcu); the Cache does NOT, and believing it did is
    // what put 15.5° in an ECU that had been told 155. Always emitted so a preset is fully deterministic
    // (a wheel with no TDC offset genuinely means 0° = sync ref at TDC).
    add("trigger.trigger_offset_btdc", w.tdcOffset);
    // Display only — the firmware never reads it. Emitted so the ECU can describe its own trigger
    // setup: the wheel it is running may not be in the library, and the dial cannot be drawn as the
    // user sees it without knowing where the pickup is.
    add("trigger.sensor_angle", w.sensorAngle);

    // Every slot is emitted, enabled or not: a preset must be able to turn a role OFF, and a pair
    // list that simply omits the slot would leave whatever the ECU had there before.
    int crankN = 0, camN = 0;
    std::vector<int> used;
    for (const WheelStream& st : w.streams) {
        // A wheel may name its stream's role outright (st.role is a SLOT now). Absent, it takes the
        // next slot of its rate — which is a placement for a wheel that did not say, not a derived
        // identity: once written it is read back as-is.
        const int slot = (st.role >= 0 && st.role < SLOT_COUNT)
                       ? st.role : slotFor(st.rate, crankN, camN);
        if (st.rate == 0) ++crankN; else ++camN;
        if (slot < 0 || slot >= SLOT_COUNT) continue;
        used.push_back(slot);
        const std::string b = "trigger.streams[" + std::to_string(slot) + "].";

        add(b + "enabled",       1);
        add(b + "repeats",       st.repeats);   // 0 = take the slot default
        add(b + "edge",          st.edge);
        add(b + "primitive",     st.prim);
        add(b + "slots",         st.slots);
        add(b + "gap_ratio",     st.ratio ? st.ratio : 2);
        add(b + "cell_len",      static_cast<double>(st.cell.size()));
        add(b + "window_pct",    st.windowPct ? st.windowPct : 25);
        add(b + "window_crank_pct", st.windowCrankPct);
        // ANGLES CROSS THIS BOUNDARY IN DEGREES. applyWheelToConfig divides every pair by the
        // field's own scale, so a pair must carry ENGINEERING units — but WheelStream keeps its
        // angles in 0.1° ints (see the struct, and the designer's nominal*10 / nominal/10.0). Handing
        // the raw int straight over made every scale-0.1 field ten times too large and then silently
        // CLAMPED: a wheel stating a 60-110° cam window wrote 600.0-720.0°, which no real cam pulse
        // can ever match, and a 100.0° VVT reference wrote 720.0°. The comments here used to say
        // "field scale 1.0"; the meta says 0.1 for every one of them.
        add(b + "nominal_angle", st.nominalAngle / 10.0);
        // Emitted for EVERY stream, cam or not, for the same reason every slot is emitted: a wheel
        // that omits a field does not leave the previous wheel's value standing. A crank stream
        // simply has no phaser, and writing 0 there says so.
        add(b + "phased",          st.rate == 1 ? st.phased : 0);
        add(b + "phase_authority", st.phaseAuthority / 10.0);
        add(b + "phase_allowance", st.phaseAllowance / 10.0);
        if (st.prim == 2) {                              // WIDTH (cam pulse window, 0.1° → degrees)
            add(b + "width_min",    st.wMin / 10.0);
            add(b + "width_max",    st.wMax / 10.0);
            add(b + "width_target", st.wTgt / 10.0);
        }
        // Gap positions / sequence spans, into THIS stream's own cells from 0. They used to be packed
        // into one shared pool behind a running index, with each stream told where its run began —
        // which is the arrangement that let two streams describe the same wheel. There is no running
        // index to keep now, and no way for one slot's cells to be another's.
        for (size_t k = 0; k < st.cell.size(); ++k)
            add(b + "cell[" + std::to_string(k) + "].v", st.cell[k]);
    }
    // And every slot this wheel does NOT use is turned off, so applying a wheel replaces the trigger
    // setup rather than merging with it. A 4-cyl crank-only wheel applied over a V6 with two cams
    // would otherwise leave the cams enabled and pointing at pins the new wheel never described.
    for (int s = 0; s < SLOT_COUNT; ++s)
        if (std::find(used.begin(), used.end(), s) == used.end())
            add("trigger.streams[" + std::to_string(s) + "].enabled", 0);
    return pairs;
}

int applyWheelToConfig(const Wheel& w, Cache& cache) {
    int n = 0;
    for (const auto& pr : wheelPairs(w)) {
        // Through the FIELD'S OWN scale, not a hardcoded ×10: nearly every pair is scale 1.0 and passes
        // through untouched, and the angles convert by the same rule the widgets use.
        const double sc = cache.configScale(pr.first);
        cache.setConfigValue(pr.first, sc != 0.0 ? pr.second / sc : pr.second);
        ++n;
    }
    return n;
}

// ---- config → wheel (inverse of wheelPairs) --------------------------------------------------
Wheel wheelFromConfig(const std::function<double(const std::string&)>& get) {
    auto ri = [&](const std::string& p) { return static_cast<int>(std::lround(get(p))); };
    // get() yields ENGINEERING units; WheelStream keeps angles in 0.1°. Rounding the degrees to an
    // int first would also throw away every tenth, so scale before rounding, not after.
    auto rd = [&](const std::string& p) { return static_cast<int>(std::lround(get(p) * 10.0)); };
    Wheel w;
    w.tdcOffset = get("trigger.trigger_offset_btdc");
    w.sensorAngle = get("trigger.sensor_angle");
    bool anyCam = false;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        const std::string b = "trigger.streams[" + std::to_string(i) + "].";
        if (ri(b + "enabled") == 0) continue;      // this role is not on this engine
        const int rate = slotIsCam(i) ? 1 : 0;     // the slot IS the role, so the rate falls out
        const int prim = ri(b + "primitive");
        const int len  = ri(b + "cell_len");
        std::vector<int> cell;
        for (int k = 0; k < len; ++k) cell.push_back(ri(b + "cell[" + std::to_string(k) + "].v"));

        WheelStream st;
        if (prim == 1) {
            st = seqStream(rate, cell);
        } else if (prim == 2) {
            st = widStream(rate, rd(b + "width_min"), rd(b + "width_max"), rd(b + "width_target"));
        } else {
            // gap_ratio is a don't-care when there are no gaps (even/distributor wheel) — wheelPairs
            // emits the default 2 there; normalise back to 0 so an even wheel round-trips cleanly.
            const int ratio = cell.empty() ? 0 : ri(b + "gap_ratio");
            st = gapStream(rate, ri(b + "slots"), ratio, cell);
        }
        // AFTER the primitive branches: each of them assigns a whole fresh WheelStream, so anything
        // set before is thrown away. Keep what the ECU said this stream is — never re-derive it.
        st.repeats      = ri(b + "repeats");       // carried, never re-derived from the slot
        st.role         = i;                       // the slot it came from — carried, never re-derived
        st.edge         = ri(b + "edge");
        st.nominalAngle   = rd(b + "nominal_angle");
        st.windowPct      = ri(b + "window_pct");          // scale 1.0, a percentage
        st.windowCrankPct = ri(b + "window_crank_pct");    // scale 1.0
        st.phased         = ri(b + "phased");              // scale 1.0, a flag
        st.phaseAuthority = rd(b + "phase_authority");
        st.phaseAllowance = rd(b + "phase_allowance");
        if (rate == 1) anyCam = true;
        w.streams.push_back(std::move(st));
    }
    w.sync = anyCam ? "PHASE" : "CRANK";
    return w;
}

// ---- angle helpers ---------------------------------------------------------------------------
std::vector<int> anglesToCell(const std::vector<double>& angles, double period) {
    std::set<double> uniq;
    for (double x : angles) uniq.insert(std::fmod(std::fmod(x, period) + period, period));
    std::vector<double> a(uniq.begin(), uniq.end());
    std::vector<int> cell;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double nxt = (i + 1 < a.size()) ? a[i + 1] : a[0] + period;
        cell.push_back(static_cast<int>(std::lround((nxt - a[i]) * 10.0)));
    }
    return cell;
}
std::vector<double> cellToAngles(const std::vector<int>& cell) {
    std::vector<double> a; double acc = 0.0;
    for (int c : cell) { a.push_back(std::round(acc * 10.0) / 10.0); acc += c / 10.0; }
    return a;
}

// ---- authoring params ⇄ wheel ------------------------------------------------------------------
namespace {
constexpr double CRANK_PERIOD = 360.0;
constexpr double CAM_PERIOD   = 720.0;

// One crank stream from its geometry. Shared by the primary (whose fields live flat on WheelParams)
// and the secondary (which carries a CrankParams), so the two can never drift apart in what they
// support — the decoder does not treat them differently and neither does this.
WheelStream crankStreamFrom(int type, int teeth, int missing, const std::vector<int>& gapPos,
                            int evenTeeth, const std::vector<double>& seqAngles,
                            double cwMin, double cwMax, double cwTgt) {
    if (type == WheelParams::CRANK_MISSING) {
        std::vector<int> gp = gapPos.empty() ? std::vector<int>{0} : gapPos;
        return gapStream(0, teeth, missing + 1, gp);
    }
    if (type == WheelParams::CRANK_EVEN)
        return gapStream(0, evenTeeth, 0, {});
    if (type == WheelParams::CRANK_WIDTH)
        return widStream(0, static_cast<int>(cwMin * 10.0), static_cast<int>(cwMax * 10.0),
                            static_cast<int>(cwTgt * 10.0));
    return seqStream(0, anglesToCell(seqAngles, CRANK_PERIOD));
}
WheelStream crankStream(const WheelParams& p) {
    return crankStreamFrom(p.crankType, p.teeth, p.missing, p.gapPos, p.evenTeeth, p.seqAngles,
                           p.cwMin, p.cwMax, p.cwTgt);
}
WheelStream crankStream(const CrankParams& c) {
    return crankStreamFrom(c.type, c.teeth, c.missing, c.gapPos, c.evenTeeth, c.seqAngles,
                           c.cwMin, c.cwMax, c.cwTgt);
}
}  // namespace

Wheel buildWheel(const WheelParams& p) {
    Wheel w;
    w.name = p.name;
    w.tdcOffset = p.tdcOffset;
    w.sensorAngle = p.sensorAngle;      // visual only; never emitted to config
    if (p.hasCrank) {
        WheelStream cs = crankStream(p);
        cs.edge      = p.crankEdge;
        cs.windowPct = p.windowPct;
        cs.repeats   = p.crankRepeats;
        w.streams.push_back(cs);
    }
    // The second crank goes IMMEDIATELY after the first, before any cam. Order is not what assigns
    // identity any more — every stream carries its role — but keeping cranks together makes the
    // stream list read the way the wheel is built.
    if (p.hasCrank2) {
        WheelStream c2 = crankStream(p.crank2);
        c2.edge      = p.crank2.edge;
        c2.windowPct = p.windowPct;
        c2.repeats   = p.crank2.repeats;
        c2.role      = 1;               // Crank Secondary is slot 1
        w.streams.push_back(c2);
    }
    for (const CamParams& c : p.cams) {
        WheelStream s = (c.type == WheelParams::CAM_PATTERN)
            ? seqStream(1, anglesToCell(c.angles, CAM_PERIOD))
            : (c.type == WheelParams::CAM_EVEN)
                ? gapStream(1, c.evenTeeth > 0 ? c.evenTeeth : 1, 0, {})
                : widStream(1, static_cast<int>(std::lround(c.wMin * 10)),
                               static_cast<int>(std::lround(c.wMax * 10)),
                               static_cast<int>(std::lround(c.wTgt * 10)));
        s.edge         = c.edge;
        s.repeats      = c.repeats;
        s.nominalAngle = static_cast<int>(std::lround(c.nominal * 10));
        s.role         = c.role;        // -1 = let wheelPairs place it by position
        w.streams.push_back(s);
    }
    // A wheel with no crank is phase by construction: its one stream spans the cycle.
    w.sync = (p.cams.empty() && p.hasCrank) ? "CRANK" : "PHASE";
    return w;
}

WheelParams decodeWheel(const Wheel& w) {
    WheelParams p;
    p.name = w.name;
    p.tdcOffset = w.tdcOffset;
    p.sensorAngle = w.sensorAngle;
    const WheelStream* crank  = nullptr;
    const WheelStream* crank2 = nullptr;
    p.hasCrank = false;                     // set below if the wheel actually has one
    for (const WheelStream& s : w.streams) {
        if (s.rate != 0) continue;
        if (!crank)       crank  = &s;
        else if (!crank2) crank2 = &s;
    }
    if (crank) {
        if (crank->prim == 1) {
            p.crankType = WheelParams::CRANK_SEQ;
            p.seqAngles = cellToAngles(crank->cell);
        } else if (crank->prim == 2) {
            p.crankType = WheelParams::CRANK_WIDTH;
            p.cwMin = crank->wMin / 10.0; p.cwMax = crank->wMax / 10.0; p.cwTgt = crank->wTgt / 10.0;
        } else if (crank->prim == 0 && crank->cell.empty()) {
            p.crankType = WheelParams::CRANK_EVEN;
            p.evenTeeth = crank->slots ? crank->slots : 4;
        } else {
            p.crankType = WheelParams::CRANK_MISSING;
            p.teeth   = crank->slots ? crank->slots : 36;
            p.missing = std::max(1, (crank->ratio ? crank->ratio : 2) - 1);
            p.gapPos  = crank->cell.empty() ? std::vector<int>{0} : crank->cell;
        }
        p.hasCrank  = true;
        p.crankEdge = crank->edge;
        p.crankRepeats = crank->repeats;
        p.windowPct = crank->windowPct ? crank->windowPct : 25;
    }
    if (crank2) {
        p.hasCrank2 = true;
        CrankParams& c = p.crank2;
        if (crank2->prim == 1) {
            c.type = WheelParams::CRANK_SEQ;
            c.seqAngles = cellToAngles(crank2->cell);
        } else if (crank2->prim == 2) {
            c.type = WheelParams::CRANK_WIDTH;
            c.cwMin = crank2->wMin / 10.0; c.cwMax = crank2->wMax / 10.0; c.cwTgt = crank2->wTgt / 10.0;
        } else if (crank2->cell.empty()) {
            c.type = WheelParams::CRANK_EVEN;
            c.evenTeeth = crank2->slots ? crank2->slots : 4;
        } else {
            c.type = WheelParams::CRANK_MISSING;
            c.teeth   = crank2->slots ? crank2->slots : 36;
            c.missing = std::max(1, (crank2->ratio ? crank2->ratio : 2) - 1);
            c.gapPos  = crank2->cell;
        }
        c.edge = crank2->edge;
        c.repeats = crank2->repeats;
    }
    for (const WheelStream& s : w.streams) {            // every cam stream, in list order
        if (s.rate != 1) continue;
        CamParams c;
        if (s.prim == 2) {
            c.type = WheelParams::CAM_PULSE;
            c.wMin = s.wMin / 10.0; c.wMax = s.wMax / 10.0; c.wTgt = s.wTgt / 10.0;
        } else if (s.prim == 0) {
            // A GAP-primitive cam: evenly spaced teeth over the cam period, usually the single
            // pulse-per-cycle reference. Reading it back as a PATTERN produced a one-cell sequence,
            // which does not lock — the round trip turned a working cam into a dead one.
            c.type = WheelParams::CAM_EVEN;
            c.evenTeeth = s.slots > 0 ? s.slots : 1;
        } else {
            c.type = WheelParams::CAM_PATTERN;
            c.angles = cellToAngles(s.cell);
        }
        c.edge    = s.edge;
        c.nominal = s.nominalAngle / 10.0;
        c.role    = s.role;                     // carried, not re-derived from position
        c.repeats = s.repeats;                  // ditto: it is the wheel's period, not a label
        p.cams.push_back(std::move(c));
    }
    return p;
}

// ---- seed set (the gen_rebench wheel table, sans the bench stim index) ------------------------
namespace {
Wheel mk(std::string name, std::vector<WheelStream> streams, int camEdge, std::string sync,
         double tdc = 0.0) {
    for (WheelStream& s : streams) if (s.rate == 1) s.edge = (camEdge < 0) ? 0 : camEdge;  // per-cam edge
    Wheel w; w.name = std::move(name); w.streams = std::move(streams);
    w.sync = std::move(sync); w.tdcOffset = tdc;
    return w;
}
}  // namespace


// ---------------------------------------------------------------------------
// The shipped library, from the meta. See definition/ecu.schema.yaml `trigger_wheels`.
// ---------------------------------------------------------------------------
std::vector<Wheel> wheelsFromMetaFile(const std::string &path)
{
    try {
        return wheelsFromMeta(jf::JJson::parseFile(path));
    } catch (const std::exception &) {
        return {};
    }
}

std::vector<Wheel> wheelsFromMeta(const jf::JJson &root)
{
    std::vector<Wheel> out;
    for (const jf::JJson &wv : root["trigger_wheels"].arr()) {
        Wheel w;
        w.name      = wv["name"].str();
        w.id        = wv["id"].str();
        w.sync      = wv["sync"].str();
        w.tdcOffset = wv["tdc_offset"].number(0.0);
        w.sensorAngle = wv["sensor_angle"].number(0.0);
        const int camEdge = wv["cam_edge"].number<int>(-1);
        for (const jf::JJson &sv : wv["streams"].arr()) {
            const std::string kind = sv["kind"].str();
            const int rate = sv["rate"].number<int>(0);
            if (kind == "seq") {
                std::vector<int> cell;
                for (const jf::JJson &c : sv["cell"].arr()) cell.push_back(c.number<int>(0));
                w.streams.push_back(seqStream(rate, std::move(cell)));
            } else if (kind == "width") {
                w.streams.push_back(widStream(rate, sv["width_min"].number<int>(0),
                                                    sv["width_max"].number<int>(7200),
                                                    sv["width_target"].number<int>(0)));
            } else {
                std::vector<int> gaps;
                for (const jf::JJson &g : sv["gaps"].arr()) gaps.push_back(g.number<int>(0));
                w.streams.push_back(gapStream(rate, sv["slots"].number<int>(0),
                                                    sv["ratio"].number<int>(0), std::move(gaps)));
            }
            // A wheel may name each stream's role outright. Absent, it stays 0 and is seeded from
            // position on emit — which is right for a single cam and wrong for two on separate banks,
            // so any wheel with cams on both banks MUST state them.
            w.streams.back().role = sv["role"].number<int>(-1);
            // 0 = take the slot default. A shipped symmetrical wheel states it, and losing it
            // here would rewrite the wheel's angular period rather than drop a label.
            w.streams.back().repeats = sv["repeats"].number<int>(0);
            // WHICH EDGE THIS STREAM COUNTS, per stream. cam_edge below sets the CAM streams, which
            // was enough while every crank wheel's information was in its gaps — a gap is the same
            // gap whichever edge you time it from. It is not enough for a wheel whose teeth are
            // deliberately uneven: an LS1 24x has 24 evenly spaced FALLING edges and rising edges
            // that sit 3 or 12 degrees before them, so counted on the rising edge the same wheel
            // reads as wildly irregular and a 24-slot even pattern never locks. Absent, it stays at
            // the struct default and nothing changes for the wheels shipped before this existed.
            if (!sv["edge"].isNull()) w.streams.back().edge = sv["edge"].number<int>(0);
            // Absent keys keep the struct defaults, so a shipped wheel written before these existed
            // parses to a fixed cam with the schema's own defaults — which is what it always meant.
            WheelStream &bs = w.streams.back();
            bs.windowPct      = sv["window_pct"].number<int>(bs.windowPct);
            bs.windowCrankPct = sv["window_crank_pct"].number<int>(bs.windowCrankPct);
            bs.nominalAngle   = sv["nominal_angle"].number<int>(bs.nominalAngle);
            bs.phased         = sv["phased"].number<int>(bs.phased);
            bs.phaseAuthority = sv["phase_authority"].number<int>(bs.phaseAuthority);
            bs.phaseAllowance = sv["phase_allowance"].number<int>(bs.phaseAllowance);
        }
        // cam_edge is a wheel-level authoring convenience; it lands on each CAM stream, exactly as
        // the old mk() helper did.
        for (WheelStream &st : w.streams)
            if (st.rate == 1) st.edge = (camEdge < 0) ? 0 : camEdge;
        out.push_back(std::move(w));
    }
    return out;
}

// ---------------------------------------------------------------------------
// User wheels. Same on-disk root as the ECU folders (see Ecu::ecuRoot), one file.
// ---------------------------------------------------------------------------
std::string userWheelsPath()
{
    return StudioPaths::dataFile("trigger_wheels.json");
}

static jf::JJson wheelToJson(const Wheel &w)
{
    jf::JJson o = jf::JJson::object();
    o["name"] = w.name;  o["sync"] = w.sync;  o["id"] = w.id;
        int camEdge = -1;
    for (const WheelStream &st : w.streams) if (st.rate == 1) { camEdge = st.edge; break; }
    o["cam_edge"] = camEdge;
    o["tdc_offset"] = w.tdcOffset;
    o["sensor_angle"] = w.sensorAngle;   // drawing only — the firmware stores it but never acts on it
    jf::JJson arr = jf::JJson::array();
    for (const WheelStream &s : w.streams) {
        jf::JJson so = jf::JJson::object();
        so["rate"] = s.rate;
        if (s.role >= 0) so["role"] = s.role;   // omitted when the wheel did not state a slot
        if (s.repeats > 0) so["repeats"] = s.repeats;   // omitted when it is the slot default
        // A USER wheel is written and read back by this pair, so anything omitted here is LOST on
        // save. window_pct and nominal_angle were being dropped that way already; the cam-phase
        // fields would have joined them.
        so["window_pct"]       = s.windowPct;
        so["window_crank_pct"] = s.windowCrankPct;
        so["nominal_angle"]    = s.nominalAngle;
        if (s.rate == 1) {
            so["phased"]          = s.phased;
            so["phase_authority"] = s.phaseAuthority;
            so["phase_allowance"] = s.phaseAllowance;
        }
        if (s.prim == 1) {
            so["kind"] = std::string("seq");
            jf::JJson c = jf::JJson::array();
            for (int v : s.cell) c.push(v);
            so["cell"] = c;
        } else if (s.prim == 2) {
            so["kind"] = std::string("width");
            so["width_min"] = s.wMin;
            so["width_max"] = s.wMax;
            so["width_target"] = s.wTgt;
        } else {
            so["kind"] = std::string("gap");
            so["slots"] = s.slots;
            so["ratio"] = s.ratio;
            jf::JJson g = jf::JJson::array();
            for (int v : s.cell) g.push(v);
            so["gaps"] = g;
        }
        arr.push(so);
    }
    o["streams"] = arr;
    return o;
}

std::vector<Wheel> loadUserWheels()
{
    // Same shape as the meta's trigger_wheels, so one parser serves both and a user wheel is
    // indistinguishable from a shipped one once loaded.
    //
    // A missing store is the NORMAL first-run state, and a corrupt one must not take the studio down
    // with it — parseFile throws on both. Either way the user simply has no wheels of their own yet;
    // the shipped library is unaffected.
    const std::string path = userWheelsPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    try {
        return wheelsFromMeta(jf::JJson::parseFile(path));
    } catch (const std::exception &) {
        return {};
    }
}

bool saveUserWheels(const std::vector<Wheel> &wheels)
{
    jf::JJson root = jf::JJson::object();
    jf::JJson arr = jf::JJson::array();
    for (const Wheel &w : wheels) arr.push(wheelToJson(w));
    root["trigger_wheels"] = arr;
    std::ofstream out(userWheelsPath(), std::ios::trunc);
    if (!out) return false;
    out << root.dump(2);
    return static_cast<bool>(out);
}

bool deleteUserWheel(const std::string &name)
{
    std::vector<Wheel> all = loadUserWheels();
    const size_t before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const Wheel &w){ return w.name == name; }), all.end());
    if (all.size() == before) return false;          // not one of the user's own
    return saveUserWheels(all);
}

std::vector<Wheel> readWheelFile(const std::string &path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    try {
        return wheelsFromMeta(jf::JJson::parseFile(path));
    } catch (const std::exception &) {
        return {};
    }
}

bool exportWheels(const std::string &path, const std::vector<Wheel> &wheels)
{
    jf::JJson root = jf::JJson::object();
    jf::JJson arr  = jf::JJson::array();
    for (const Wheel &w : wheels) arr.push(wheelToJson(w));
    root["trigger_wheels"] = arr;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << root.dump(2);
    return static_cast<bool>(out);
}

int importWheels(const std::string &path)
{
    const std::vector<Wheel> incoming = readWheelFile(path);
    if (incoming.empty()) return -1;                  // unreadable, or nothing in it
    std::vector<Wheel> all = loadUserWheels();
    int touched = 0;
    for (const Wheel &w : incoming) {
        bool replaced = false;
        for (Wheel &e : all)
            if (e.name == w.name) { e = w; replaced = true; break; }
        if (!replaced) all.push_back(w);
        ++touched;
    }
    return saveUserWheels(all) ? touched : -1;
}

bool saveUserWheel(const Wheel &w)
{
    std::vector<Wheel> all = loadUserWheels();
    // Replace by name so re-saving an edited wheel updates it instead of accumulating duplicates.
    for (Wheel &e : all)
        if (e.name == w.name) { e = w; return saveUserWheels(all); }
    all.push_back(w);
    return saveUserWheels(all);
}

// ---- validateWheel ------------------------------------------------------------------------------
//
// Judged on the BUILT wheel, which is what would be sent. Each check names the thing the decoder
// needs and the field the designer would change, because "invalid" on its own sends somebody back to
// guessing — which is the state this exists to end.
std::vector<WheelIssue> validateWheel(const Wheel& w) {
    std::vector<WheelIssue> out;
    auto add = [&out](WheelIssue::Level l, std::string t) { out.push_back({ l, std::move(t) }); };

    if (w.streams.empty()) {
        add(WheelIssue::Error, "no streams — a wheel needs at least a crank");
        return out;
    }

    // SYNC AND THE CAMS MUST AGREE. "PHASE" promises the decoder can tell which revolution it is on,
    // and only a cam can tell it that; "CRANK" with a cam present throws away the phase information
    // the engine is actually providing and silently halves what the tune can do (wasted spark only).
    // A CAM IS KNOWN BY ITS RATE, not its role. role == -1 is legal and ordinary — it means "the
    // wheel did not say, let wheelPairs place it by position" — so keying the question on role >= 2
    // called every unplaced cam a crank, and a wheel with one cam and CRANK sync raised nothing.
    auto isCam = [](const WheelStream& s) { return s.rate == 1 || s.role >= 2; };
    const bool anyCam = std::any_of(w.streams.begin(), w.streams.end(), isCam);
    if (w.sync == "PHASE" && !anyCam)
        add(WheelIssue::Error, "sync is PHASE but no cam stream — nothing can say which revolution it is");
    if (w.sync == "CRANK" && anyCam)
        add(WheelIssue::Warn, "a cam is defined but sync is CRANK — the phase it gives is unused");

    // Two streams cannot hold one slot: the slot IS the role, so the second silently overwrites the
    // first when the config is written.
    std::vector<int> seen;
    for (const WheelStream& s : w.streams) {
        if (s.role < 0) continue;
        if (std::find(seen.begin(), seen.end(), s.role) != seen.end())
            add(WheelIssue::Error, "two streams claim the same slot (role " + std::to_string(s.role) + ")");
        seen.push_back(s.role);
    }

    for (size_t i = 0; i < w.streams.size(); ++i) {
        const WheelStream& s = w.streams[i];
        const std::string who = (isCam(s) ? "cam" : "crank") + std::string(" stream ") + std::to_string(i);

        if (s.prim == 0 && s.slots > 0) {              // GAP: a toothed wheel with a gap in it
            if (s.slots < 2)
                add(WheelIssue::Error, who + ": fewer than 2 teeth cannot be decoded");
            const int missing = (s.ratio > 0 ? s.ratio - 1 : 1);
            if (missing >= s.slots)
                add(WheelIssue::Error, who + ": " + std::to_string(missing) + " missing of " +
                                       std::to_string(s.slots) + " teeth leaves nothing to count");
            // A gap position outside the wheel is a tooth that does not exist; the decoder looks for
            // a gap that can never arrive and never syncs.
            for (int c : s.cell)
                if (c < 0 || c >= s.slots) {
                    add(WheelIssue::Error, who + ": gap at tooth " + std::to_string(c) +
                                           " is outside a " + std::to_string(s.slots) + "-tooth wheel");
                    break;
                }
            for (size_t a = 0; a < s.cell.size(); ++a)
                for (size_t b = a + 1; b < s.cell.size(); ++b)
                    if (s.cell[a] == s.cell[b]) {
                        add(WheelIssue::Error, who + ": tooth " + std::to_string(s.cell[a]) +
                                               " is listed as a gap twice");
                        a = s.cell.size(); break;
                    }
        } else if (s.prim == 0 && s.slots == 0 && !s.cell.empty()) {   // SEQUENCE: inter-edge spans
            if (s.cell.size() < 2)
                add(WheelIssue::Error, who + ": a sequence needs at least two edges");
            long sum = 0;
            for (int c : s.cell) {
                if (c <= 0) { add(WheelIssue::Error, who + ": a sequence span must be greater than zero"); break; }
                sum += c;
            }
            // The spans are what the decoder matches ratios against; if they do not close the period
            // the pattern never repeats where it is expected to.
            const long period = isCam(s) ? 7200 : 3600;
            if (sum > 0 && std::labs(sum - period) > period / 20)
                add(WheelIssue::Warn, who + ": spans total " + std::to_string(sum / 10) +
                                      "\xC2\xB0, not the " + std::to_string(period / 10) + "\xC2\xB0 of its cycle");
        } else if (s.wMax > 0 && s.wMin >= s.wMax) {   // WIDTH: a window with nothing in it
            add(WheelIssue::Error, who + ": width window min is not below max");
        }

        if (s.windowPct <= 0 || s.windowPct >= 100)
            add(WheelIssue::Error, who + ": match window \xC2\xB1" + std::to_string(s.windowPct) +
                                   "% cannot match anything");
        else if (s.windowPct > 45)
            add(WheelIssue::Warn, who + ": match window \xC2\xB1" + std::to_string(s.windowPct) +
                                  "% is wide enough to match the wrong tooth");

        if (s.edge < 0 || s.edge > 2)
            add(WheelIssue::Error, who + ": capture edge is not rising, falling or both");

        if (isCam(s) && s.phased && s.phaseAuthority <= 0)
            add(WheelIssue::Warn, who + ": phased but given no authority to travel");
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const WheelIssue& a, const WheelIssue& b) { return a.level > b.level; });
    return out;
}

bool applyMeasured(const triggerfit::CaptureFit& m, WheelParams& p) {

        if (!m.ok || m.crankIdx < 0) return false;
        const triggerfit::StreamFit& cs = m.streams[size_t(m.crankIdx)];
        
        // THE CRANK TRACK AS WHAT IT MEASURED AS. An even track is an even wheel — writing it as a
        // missing-tooth wheel with one absent would be describing a wheel that is not there.
        if (cs.shape == triggerfit::Shape::Even) {
            p.crankType = WheelParams::CRANK_EVEN;
            p.evenTeeth = cs.teeth;
        } else if (cs.shape == triggerfit::Shape::Gap) {
            p.crankType = WheelParams::CRANK_MISSING;
            p.teeth     = cs.teeth;
            p.missing   = std::max(1, cs.missing);
            p.gapPos    = cs.gapPos;
        } else {
            return false;    // coded: its marks are widths, and a width window is not derivable yet
        }

        // THE CAMS, AS MANY AS THE CAPTURE FOUND. A stream is a cam when its pattern takes an engine
        // cycle to come round — that is what cycleRatio 2 means, and it is decided by the capture as
        // a whole rather than by any lane on its own, because a lane cannot know whether its period
        // is a revolution or a cycle.
        //
        // The whole list is replaced rather than merged. A capture is a statement about the engine
        // in front of you: if it shows one cam, adopting it must not leave a second one behind from
        // whatever was in the editor before.
        std::vector<CamParams> cams;
        for (size_t i = 0; i < m.streams.size(); ++i) {
            if (static_cast<int>(i) == m.crankIdx) continue;
            const triggerfit::StreamFit& ms = m.streams[i];
            if (!ms.ok || ms.cycleRatio != 2) continue;      // crank-rate: not a cam

            CamParams c;
            c.role = -1;                                     // from its position; the user re-assigns
            c.edge = ms.bothEdges ? 2 : 0;
            // Angles measured on a cam are degrees of ITS OWN period, and its period is the cycle,
            // so they are already cycle degrees once scaled by the ratio. Getting this wrong puts
            // every cam edge at half the angle it belongs at, which still LOOKS like a cam.
            const double toCycle = 360.0 * static_cast<double>(ms.cycleRatio) / 360.0;
            switch (ms.shape) {
                case triggerfit::Shape::Even:
                    // N evenly spaced teeth over the cycle. One of them is the ordinary single pulse.
                    c.type      = WheelParams::CAM_EVEN;
                    c.evenTeeth = std::max(1, ms.teeth);
                    break;
                case triggerfit::Shape::Width:
                    // Its identity is in how long the tooth is HIGH, which is what a pulse window is.
                    c.type = WheelParams::CAM_PULSE;
                    c.wMin = 0.0; c.wMax = 720.0; c.wTgt = 0.0;
                    break;
                default: {
                    // Gap or Coded: both are a pattern of edges at particular angles, which is the
                    // one description that covers either without inventing missing teeth on a cam.
                    c.type = WheelParams::CAM_PATTERN;
                    c.angles.clear();
                    for (double a : ms.edgeAngles) c.angles.push_back(a * toCycle);
                    if (c.angles.size() < 2) continue;       // a pattern of one has nothing to match
                    break;
                }
            }
            cams.push_back(c);
            if (cams.size() >= 4) break;                     // slots 2..5, and no more exist
        }
        p.cams = cams;

        return true;
}

// Headless test for the studio-jf trigger logic core: wheel -> config pairs -> wheel, and the geometry
// the designer draws from.
//
// The wheels come from the SHIPPED meta (shared/tuneit-meta.json, generated from the schema's
// trigger_wheels block) — not from a list in this file and not from one in the studio binary. That is
// the point of the library living in the schema: the wheels a studio offers belong to the firmware it
// is talking to, so this test asserts against the same document the studio reads.
#include "TriggerWheel.h"
#include "TriggerGeometry.h"

#include <j/config/Json.h>

#include <cstdio>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef META_PATH
#error "META_PATH (shared/tuneit-meta.json) must be defined by the build"
#endif

// The meta carries a trailing 4-byte CRC32 (codegen writes meta_bytes + crc; MetaModel::loadFile
// checks and strips it). Drop it before parsing — the JSON ends where the CRC begins.
static std::vector<Wheel> shippedWheels() {
    std::ifstream f(META_PATH, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() > 4) raw.resize(raw.size() - 4);
    return wheelsFromMeta(jf::JJson::parse(raw));
}

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { ++g_fail; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a,b) (std::fabs((double)(a)-(double)(b)) < 1e-6)

static std::map<std::string,double> pairMap(const Wheel& w) {
    std::map<std::string,double> m;
    for (auto& pr : wheelPairs(w)) m[pr.first] = pr.second;   // last wins (paths are unique here)
    return m;
}
static double P(const std::map<std::string,double>& m, const std::string& k) {
    auto it = m.find(k); return it == m.end() ? -1e9 : it->second;
}
static const Wheel& seed(const std::vector<Wheel>& s, const std::string& n) {
    for (auto& w : s) if (w.name == n) return w;
    static Wheel none; return none;
}

static bool sameStreams(const Wheel& a, const Wheel& b) {
    if (a.streams.size() != b.streams.size()) return false;
    for (size_t i = 0; i < a.streams.size(); ++i) {
        const WheelStream &x = a.streams[i], &y = b.streams[i];
        if (x.rate!=y.rate || x.prim!=y.prim || x.slots!=y.slots || x.ratio!=y.ratio || x.cell!=y.cell) return false;
        if (x.edge!=y.edge || x.nominalAngle!=y.nominalAngle) return false;
        if (x.prim==2 && (x.wMin!=y.wMin || x.wMax!=y.wMax || x.wTgt!=y.wTgt)) return false;
    }
    return true;
}

int main() {
    std::printf("=== trigger core ===\n");
    auto seeds = shippedWheels();
    std::printf("  %zu wheels shipped in the meta\n", seeds.size());
    CHECK(!seeds.empty());

    // --- the geometry carries what a PAINTER needs ------------------------------------------------
    // drawTrace used to be handed the geometry AND the Wheel AND a parallel roles vector, and reached
    // into all three: the Wheel for each stream's capture edge, the roles vector for the lane colour and
    // caption. So it could only ever draw a wheel from the library — never one read off an ECU, which
    // has no Wheel to reach into. Both facts live in StreamGeometry now.
    //
    // displayRole is the trap worth pinning: WheelStream::role is a 0-based SLOT that may be -1, while
    // the colour and the caption key off a 1-based DISPLAY role. Two numberings one apart, and passing
    // the wrong one shifts every lane's colour and label by one without failing anything.
    for (const Wheel& w : seeds) {
        const WheelGeometry g = wheelGeometry(w);
        CHECK(g.streams.size() == w.streams.size());
        int crankN = 0, camN = 0;
        for (std::size_t i = 0; i < g.streams.size(); ++i) {
            const int want = (g.streams[i].rate == 0) ? (crankN++ ? 2 : 1) : (kDisplayRoleCamBase + camN++);
            CHECK(g.streams[i].displayRole == want);      // 1 Primary, 2 Secondary, 3.. the cams
            CHECK(g.streams[i].displayRole >= 1);         // never the 0-based slot, never -1
            CHECK(g.streams[i].edge == w.streams[i].edge);
        }
    }

    // --- wheelPairs: 36-1 ---
    {
        auto m = pairMap(seed(seeds, "36-1"));
        CHECK(NEAR(P(m,"trigger.streams[0].enabled"), 1));   // the slot IS the role
        CHECK(NEAR(P(m,"trigger.streams[2].enabled"), 0));   // no cam: that slot is OFF
        CHECK(NEAR(P(m,"trigger.trigger_offset_btdc"), 0));
        CHECK(NEAR(P(m,"trigger.streams[0].primitive"), 0));
        CHECK(NEAR(P(m,"trigger.streams[0].slots"), 36));
        CHECK(NEAR(P(m,"trigger.streams[0].gap_ratio"), 2));
        CHECK(NEAR(P(m,"trigger.streams[0].edge"), 0));
                CHECK(NEAR(P(m,"trigger.streams[0].cell_len"), 1));
        CHECK(NEAR(P(m,"trigger.streams[0].window_pct"), 25));
        CHECK(NEAR(P(m,"trigger.streams[0].cell[0].v"), 0));
    }
    // --- 2JZ 36-2 (crank only): tdc offset 155, gap at index 0, ratio 3 ---
    {
        auto m = pairMap(seed(seeds, "2JZ 36-2 (crank only)"));
        CHECK(NEAR(P(m,"trigger.trigger_offset_btdc"), 155));     // display degrees (scale 0.1 applied by Cache)
        CHECK(NEAR(P(m,"trigger.streams[0].slots"), 36));
        CHECK(NEAR(P(m,"trigger.streams[0].gap_ratio"), 3));
        CHECK(NEAR(P(m,"trigger.streams[0].cell[0].v"), 0));
    }
    // --- every shipped gap wheel anchors on the tooth after the gap -------------------------------
    // The decoder can only identify one feature, and that is where it establishes position. A gap
    // index other than 0 does not move the sync point; it renames that tooth and drags the angle
    // origin back with it, so the wheel smuggles an `index * tooth_angle` offset in beside
    // trigger_offset_btdc. The 2JZ rows shipped as gaps:[6] — 80 degrees of hidden offset. Codegen
    // rejects it now; this is the studio-side half of the same contract.
    {
        int offenders = 0;
        for (const Wheel& w : seeds)
            for (const WheelStream& s : w.streams)
                if (s.prim == 0 && !s.cell.empty() && s.cell[0] != 0) {
                    std::printf("  FAIL first gap index %d on '%s'\n", s.cell[0], w.name.c_str());
                    ++offenders;
                }
        CHECK(offenders == 0);
    }
    // --- odd-fire sequence: primitive 1, two spans into the pool ---
    {
        auto m = pairMap(seed(seeds, "odd-fire 0/135"));
        CHECK(NEAR(P(m,"trigger.streams[0].primitive"), 1));
        CHECK(NEAR(P(m,"trigger.streams[0].cell_len"), 2));
        CHECK(NEAR(P(m,"trigger.streams[0].cell[0].v"), 1350));
        CHECK(NEAR(P(m,"trigger.streams[0].cell[1].v"), 2250));
    }
    // --- 60-2 + cam: two streams; cam is WIDTH role 3, edge Both, each stream carrying its own cells ---
    {
        auto m = pairMap(seed(seeds, "60-2 + cam"));
        CHECK(NEAR(P(m,"trigger.streams[0].enabled"), 1));
        // The cam lands at slot 2 — Cam Intake B1 — NOT slot 1, which is Crank Secondary. Under the
        // old scheme it went to array index 1 and carried role 3; the slot now says both at once.
        CHECK(NEAR(P(m,"trigger.streams[1].enabled"), 0));   // no second crank
        CHECK(NEAR(P(m,"trigger.streams[0].cell[0].v"), 0));       // crank gap index 0
        CHECK(NEAR(P(m,"trigger.streams[2].primitive"), 2)); // WIDTH
        CHECK(NEAR(P(m,"trigger.streams[2].edge"), 2));      // Both (camEdge 2)
        CHECK(NEAR(P(m,"trigger.streams[2].cell_len"), 0));   // WIDTH uses no cells
        // The PAIR carries degrees, not 0.1 deg: applyWheelToConfig divides by the field's own
        // scale (0.1 here) to reach the raw units. Asserting 7200 here asserted a pair that
        // would have been written as 72000 and clamped.
        CHECK(NEAR(P(m,"trigger.streams[2].width_max"), 720.0));
    }

    // --- geometry: 36-1 → 35 present teeth, exactly one gap arc, first tooth at 0 ---
    {
        auto g = wheelGeometry(seed(seeds, "36-1"));
        CHECK(g.crankIdx == 0);
        CHECK(NEAR(g.cycle, 360));
        CHECK(g.streams[0].present == 35);                   // 36 - 1*(2-1)
        CHECK(g.streams[0].gaps.size() == 1);
        CHECK(NEAR(g.streams[0].teeth[0], 0));
        CHECK(g.hasReference);                               // the gap gives it an absolute zero
    }

    // --- the decoder's zero, and TDC #1 measured from it -----------------------------------------
    // The reference the studio draws must be the one the FIRMWARE uses: angle 0 is tooth 0 of the
    // list, and TDC #1 is trigger_offset_btdc from there (EnginePositionHal::engine_angle). It used
    // to be drawn from the midpoint of the gap, which is not the decoder's zero and is not an angle
    // the firmware computes anywhere — on a 2JZ 36-2 that put the TDC mark 65° out.
    {
        // A wheel whose sync gap is NOT index 0: the origin stays at tooth 0, so the gap sits some
        // way round from it. That is the whole reason to draw the origin rather than the gap.
        Wheel w;
        w.name = "gap at index 6"; w.sync = "CRANK"; w.tdcOffset = 155.0;
        w.streams.push_back(gapStream(0, 36, 3, { 6 }));
        auto g = wheelGeometry(w);
        CHECK(g.hasReference);
        CHECK(NEAR(g.streams[0].teeth[0], 0));               // the origin, and it is a TOOTH
        CHECK(NEAR(g.streams[0].teeth[6], 80));              // first tooth after the gap: 5*10 + 30
        CHECK(NEAR(g.tdcOffset, 155));                       // carried through unmodified — no gap term
        CHECK(g.streams[0].gaps.size() == 1);
        CHECK(NEAR(g.streams[0].gaps[0].first, 55));         // the gap arc is nowhere near the origin
        CHECK(NEAR(g.streams[0].gaps[0].second, 75));
    }
    {
        // An even wheel has no absolute reference at all, so neither mark is drawn.
        Wheel w;
        w.name = "even"; w.sync = "CRANK";
        w.streams.push_back(gapStream(0, 36, 0, {}));
        CHECK(!wheelGeometry(w).hasReference);
    }
    // --- geometry: odd-fire sequence → teeth at 0 and 135, cycle 360 ---
    {
        auto g = wheelGeometry(seed(seeds, "odd-fire 0/135"));
        CHECK(g.streams[0].teeth.size() == 2);
        CHECK(NEAR(g.streams[0].teeth[0], 0));
        CHECK(NEAR(g.streams[0].teeth[1], 135));
    }
    // --- geometry: 60-2 + cam → 720 cycle, crank 58 present, cam has a pulse ---
    {
        auto g = wheelGeometry(seed(seeds, "60-2 + cam"));
        CHECK(NEAR(g.cycle, 720));
        CHECK(g.crankIdx == 0 && g.camIdx == 1);
        CHECK(g.streams[0].present == 58);                   // 60 - 1*(3-1)
        CHECK(g.streams[1].hasPulse);
    }

    // --- VVT: crank + 2 cams with nominal (reference) angles + per-cam edge ---
    {
        WheelParams p;                                   // default = 36-1 missing crank
        CamParams c1; c1.type = WheelParams::CAM_PULSE;   c1.edge = 2; c1.nominal =  20.0;  // intake, +20°
        CamParams c2; c2.type = WheelParams::CAM_PATTERN; c2.edge = 0; c2.nominal = -15.0;  // exhaust, -15°
        c2.angles = { 0.0, 180.0, 360.0, 540.0 };
        p.cams = { c1, c2 };
        Wheel w = buildWheel(p);
        CHECK(w.streams.size() == 3);                    // crank + 2 cams
        CHECK(w.sync == "PHASE");
        auto m = pairMap(w);
        CHECK(NEAR(P(m,"trigger.streams[0].enabled"), 1));            // crank
        CHECK(NEAR(P(m,"trigger.streams[2].enabled"), 1));            // Cam Intake B1  (slot = role)
        CHECK(NEAR(P(m,"trigger.streams[3].enabled"), 1));            // Cam Exhaust B1
        CHECK(NEAR(P(m,"trigger.streams[1].enabled"), 0));            // no second crank
        CHECK(NEAR(P(m,"trigger.streams[2].edge"), 2));               // Both
        CHECK(NEAR(P(m,"trigger.streams[2].nominal_angle"),  20.0));  // the pair is in DEGREES
        CHECK(NEAR(P(m,"trigger.streams[3].nominal_angle"), -15.0));  // scale 0.1 applies on write
        // params round-trip preserves the cams
        WheelParams rp = decodeWheel(w);
        CHECK(rp.cams.size() == 2);
        CHECK(rp.cams[0].type == WheelParams::CAM_PULSE   && NEAR(rp.cams[0].nominal,  20.0) && rp.cams[0].edge == 2);
        CHECK(rp.cams[1].type == WheelParams::CAM_PATTERN && NEAR(rp.cams[1].nominal, -15.0) && rp.cams[1].edge == 0);
        // config round-trip preserves streams (incl. edge + nominal_angle)
        std::map<std::string,double> cm; for (auto& pr : m) cm[pr.first] = pr.second;
        Wheel rc = wheelFromConfig([&](const std::string& q){ auto it=cm.find(q); return it==cm.end()?0.0:it->second; });
        CHECK(sameStreams(w, rc));
    }

    // --- stream ROLE is stored, not derived from position ----------------------------------------
    // Position made the first cam Intake B1 and the second Exhaust B1, which cannot describe a V
    // engine with phasers on the intakes alone — Intake B1 + Intake B2, roles 3 and 5. Worse, the
    // config round trip re-derived it: reading such an engine off an ECU and writing it back moved
    // its second intake cam onto the exhaust role, silently.
    {
        WheelParams p;
        p.teeth = 36; p.missing = 1; p.gapPos = {0};
        CamParams a, b;
        a.type = WheelParams::CAM_PULSE; a.role = 2;      // Cam Intake B1 = SLOT 2
        b.type = WheelParams::CAM_PULSE; b.role = 4;      // Cam Intake B2 = SLOT 4, NOT Exhaust B1
        p.cams = { a, b };
        const Wheel w = buildWheel(p);
        auto m = pairMap(w);
        // The slot IS the identity: enabled at 2 and 4, and 3 (Exhaust B1) left OFF. Under the old
        // scheme these were array indices 1 and 2 carrying role values 3 and 5, and a round trip
        // renumbered the second by position onto role 4 — the exhaust cam.
        CHECK(NEAR(P(m,"trigger.streams[2].enabled"), 1));
        CHECK(NEAR(P(m,"trigger.streams[4].enabled"), 1));
        CHECK(NEAR(P(m,"trigger.streams[3].enabled"), 0));

        // Through the config and back: the roles must survive, not be renumbered by position.
        std::map<std::string,double> cm; for (auto& pr : m) cm[pr.first] = pr.second;
        Wheel rc = wheelFromConfig([&](const std::string& q){ auto it=cm.find(q); return it==cm.end()?0.0:it->second; });
        CHECK(rc.streams.size() == 3);
        CHECK(rc.streams[1].role == 2);
        CHECK(rc.streams[2].role == 4);
        auto m2 = pairMap(rc);
        CHECK(NEAR(P(m2,"trigger.streams[4].enabled"), 1));   // the trip that used to land on 4=Exh B1
        CHECK(NEAR(P(m2,"trigger.streams[3].enabled"), 0));

        // And out through the params the form edits.
        WheelParams rp = decodeWheel(rc);
        CHECK(rp.cams.size() == 2);
        CHECK(rp.cams[0].role == 2 && rp.cams[1].role == 4);
    }
    {
        // -1 means "the wheel did not say", so an unstated single cam is PLACED at the first cam
        // slot. NOT 0: zero is Crank Primary now, and a cam defaulting to it would silently take the
        // crank's slot — which is exactly why the sentinel had to move off zero.
        WheelParams p;
        CamParams c; c.type = WheelParams::CAM_PULSE;      // role left -1
        p.cams = { c };
        auto m = pairMap(buildWheel(p));
        CHECK(NEAR(P(m,"trigger.streams[2].enabled"), 1));   // Cam Intake B1
        CHECK(NEAR(P(m,"trigger.streams[0].enabled"), 1));   // and the crank kept slot 0
    }

    // --- params round-trip: buildWheel(decodeWheel(seed)) preserves every seed's streams ---
    int rtFail = 0;
    for (auto& w : seeds) {
        Wheel rt = buildWheel(decodeWheel(w));
        if (!sameStreams(w, rt) || !NEAR(rt.tdcOffset, w.tdcOffset) || rt.sync != w.sync) {
            std::printf("  FAIL round-trip: %s  (streams %zu->%zu sync %s->%s)\n", w.name.c_str(),
                        w.streams.size(), rt.streams.size(), w.sync.c_str(), rt.sync.c_str());
            ++rtFail;
        }
    }
    CHECK(rtFail == 0);

    // --- the REPEAT COUNT survives the designer, and reaches the config --------------------------
    // The three wheels whose period is not the slot default. Losing `repeats` anywhere along
    // library -> decodeWheel -> buildWheel -> wheelPairs does not drop a label: it silently rewrites
    // the wheel's angular period, and the ECU then decodes a different wheel from the one named.
    {
        struct Want { const char* name; const char* path; double v; };
        const Want want[] = {
            { "Renix 4cyl symmetrical",   "trigger.streams[0].repeats", 4 },
            { "Renix 6cyl symmetrical",   "trigger.streams[0].repeats", 6 },
            { "Daihatsu 3+1 distributor", "trigger.streams[2].repeats", 1 },
        };
        for (const Want& x : want) {
            const Wheel* seed = nullptr;
            for (const auto& w : seeds) if (w.name == x.name) seed = &w;
            CHECK(seed != nullptr);
            if (!seed) continue;
            // THROUGH the designer's params, which is the path a user's edit takes.
            auto m2 = pairMap(buildWheel(decodeWheel(*seed)));
            CHECK(NEAR(P(m2, x.path), x.v));
        }
        // And the distributor keeps having NO crank stream: buildWheel used to emit one
        // unconditionally, describing a sensor the engine does not have.
        for (const auto& w : seeds)
            if (w.name == "Daihatsu 3+1 distributor") {
                const WheelParams dp = decodeWheel(w);
                CHECK(!dp.hasCrank);
                CHECK(buildWheel(dp).streams.size() == 1);
            }
    }

    // --- config round-trip: wheelFromConfig(wheelPairs(seed)) preserves every seed ---
    int cfgFail = 0;
    for (auto& w : seeds) {
        std::map<std::string, double> m;
        for (auto& pr : wheelPairs(w)) m[pr.first] = pr.second;
        auto rd = [&](const std::string& p) { auto it = m.find(p); return it == m.end() ? 0.0 : it->second; };
        Wheel rt = wheelFromConfig(rd);
        if (!sameStreams(w, rt) || !NEAR(rt.tdcOffset, w.tdcOffset) || rt.sync != w.sync) {
            std::printf("  FAIL config round-trip: %s\n", w.name.c_str());
            ++cfgFail;
        }
    }
    CHECK(cfgFail == 0);

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

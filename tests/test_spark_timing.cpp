// Which SIDE of TDC does the spark land on?
//
// An ignition table holds ADVANCE: positive degrees BTDC. The default table in the schema runs from
// 10.0 deg at idle to 42.4 deg up top — every cell advance, nothing ATDC. The firmware used ONE
// converter, `wrap(tdc + offset)`, for both the BTDC values (spark, injection) and the ATDC one
// (the knock window), so a commanded 34 deg advance was scheduled 34 deg AFTER TDC — 68 deg from
// where the tuner asked, in the retarded direction.
//
// Nothing caught it because every existing test asserts the advance REACHES the scheduler
// (test_ignition_controller checks spark_btdc_x10 == advance*10) and none asserted where it lands.
// A stim bench has no combustion and no timing light, so only an engine would have shown it.
//
// The subtraction is the whole point: timing is quoted BEFORE TDC and events are scheduled AFTER
// it, so the two run opposite ways and the conversion has to negate.
//
// These tests are about DIRECTION, not arithmetic.
#include "test_helpers.h"

// The key gate the capture path now consults: with the key off an edge stops before the decoder, so
// nothing syncs and no rpm is produced on USB-only power. This test drives a running engine, so it
// asserts the key is on — the state it has always implicitly assumed.
bool g_system_active = true;
#include "Scheduler/EnginePositionHal.h"
#include "Scheduler/SchedulerTypes.h"
#include "Scheduler/EventScheduler.h"   // claim_outputs: which coils a cycle actually takes
#include "ecu_config.h"        // g_config — the shipped ignition clamp defaults

// Forward distance from `from` to `to` in the direction of rotation, in a FOUR-STROKE cycle unless
// a span is given (the two-stroke cases below pass 360 explicitly).
static AngleDeg10 fwd(AngleDeg10 from, AngleDeg10 to, AngleDeg10 cycle = ANGLE_720) {
    return angle_dist_forward(from, to, cycle);
}

int main() {
    fprintf(stdout, "=== Spark/injection timing sense (BTDC = advance) ===\n");

    SECTION("advance is BEFORE TDC — the engine reaches the spark, THEN TDC");
    {
        const AngleDeg10 tdc = 1800;          // cylinder TDC at 180 deg
        const AngleDeg10 adv = 340;           // 34.0 deg advance, a real cell from the default table

        const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, adv, ANGLE_720);
        CHECK(spark == 1460);                                  // 180 - 34 = 146 deg
        // The invariant that matters, stated in the domain's own terms: travelling forward from the
        // spark, TDC arrives exactly `advance` later.
        CHECK(fwd(spark, tdc) == adv);
        // ...and NOT the other way round. This is the assertion the old code failed.
        CHECK(fwd(tdc, spark) != adv);
    }

    SECTION("more advance fires EARLIER, not later");
    {
        const AngleDeg10 tdc = 1800;
        const AngleDeg10 s10 = EnginePositionHal::angle_btdc(tdc, 100, ANGLE_720);   // 10 deg
        const AngleDeg10 s34 = EnginePositionHal::angle_btdc(tdc, 340, ANGLE_720);   // 34 deg
        CHECK(s34 < s10);                                      // further from TDC, in the earlier direction
        CHECK(fwd(s34, tdc) > fwd(s10, tdc));
    }

    SECTION("a NEGATIVE value retards — fires after TDC");
    {
        const AngleDeg10 tdc = 1800;
        const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, -100, ANGLE_720);   // -10 deg = 10 ATDC
        CHECK(spark == 1900);
        CHECK(fwd(tdc, spark) == 100);                         // TDC first, then the spark
    }

    SECTION("the full default-table range stays on the advance side");
    {
        const AngleDeg10 tdc = 0;                              // cylinder 1 at the cycle origin
        for (AngleDeg10 adv = 100; adv <= 424; adv = static_cast<AngleDeg10>(adv + 1)) {
            const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, adv, ANGLE_720);
            CHECK(fwd(spark, tdc) == adv);                     // always `adv` before TDC, incl. the wrap
        }
    }

    SECTION("wrapping past the cycle origin keeps the sense");
    {
        const AngleDeg10 tdc = 100;                            // 10 deg — advance takes it below zero
        const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, 340, ANGLE_720);
        CHECK(spark == 6960);                                  // 7200 - 240
        CHECK(fwd(spark, tdc) == 340);
    }

    SECTION("dwell precedes its spark (the relationship that proved the axis direction)");
    {
        const AngleDeg10 tdc   = 1800;
        const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, 340, ANGLE_720);
        const AngleDeg10 dwell = angle_wrap(static_cast<AngleDeg10>(spark - 600), ANGLE_720);   // 60 deg of dwell
        CHECK(fwd(dwell, spark) == 600);                       // charge, THEN fire
    }

    SECTION("the knock window is ATDC — the one place adding is right");
    {
        const AngleDeg10 tdc = 1800;
        const AngleDeg10 win = EnginePositionHal::angle_atdc(tdc, 100, ANGLE_720);   // 10 deg ATDC
        CHECK(win == 1900);
        CHECK(fwd(tdc, win) == 100);                           // after TDC, after the spark
        // The two senses must be opposites — a single shared converter is what went wrong.
        CHECK(EnginePositionHal::angle_atdc(tdc, 340, ANGLE_720) != EnginePositionHal::angle_btdc(tdc, 340, ANGLE_720));
    }

    SECTION("injection firing angle is BTDC too (schema: 'Injection Firing Angle BTDC', default 355)");
    {
        const AngleDeg10 tdc  = 1800;
        const AngleDeg10 open = EnginePositionHal::angle_btdc(tdc, 3550, ANGLE_720);
        CHECK(fwd(open, tdc) == 3550);                          // opens 355 deg before TDC
    }

    // -----------------------------------------------------------------------------------------
    // Trigger offset: the ONE place the wheel-to-engine relationship is stated.
    // -----------------------------------------------------------------------------------------
    SECTION("2JZ 36-2: the gap sits 155 deg BTDC #1");
    {
        const AngleDeg10 off = 1550;                  // 155.0 deg, the value the studio's wheel emits

        // At the trigger reference the engine is 155 deg BEFORE #1 TDC — i.e. 155 deg still to go.
        const AngleDeg10 at_ref = EnginePositionHal::engine_angle(0, off, ANGLE_720);
        CHECK(fwd(at_ref, 0) == 1550);                // 155 deg forward from here reaches TDC #1

        // 155 deg of rotation later the decoder reads 1550 and the engine is exactly at #1 TDC.
        CHECK(EnginePositionHal::engine_angle(1550, off, ANGLE_720) == 0);

        // ...so a per-cylinder TDC becomes a plain engine angle: cylinder 1 is 0, and nothing
        // downstream has to know the wheel exists.
        CHECK(EnginePositionHal::engine_angle(1550 + 1800, off, ANGLE_720) == 1800);   // 180 deg later
    }

    SECTION("offset 0 changes nothing (the field is opt-in)");
    {
        for (AngleDeg10 a = 0; a < ANGLE_720; a = static_cast<AngleDeg10>(a + 137))
            CHECK(EnginePositionHal::engine_angle(a, 0, ANGLE_720) == a);
    }

    SECTION("it wraps into the LIVE cycle span, not always 720");
    {
        // At CRANK sync the grid only knows 360 deg; an engine angle outside that span would be
        // placed on a tooth that does not exist.
        const AngleDeg10 off = 1550;
        for (AngleDeg10 a = 0; a < ANGLE_360; a = static_cast<AngleDeg10>(a + 91)) {
            const AngleDeg10 e = EnginePositionHal::engine_angle(a, off, ANGLE_360);
            CHECK(e >= 0 && e < ANGLE_360);
        }
        CHECK(EnginePositionHal::engine_angle(1550, off, ANGLE_360) == 0);
        // An offset LARGER than the span still lands inside it (a 720 offset on a 360 grid).
        CHECK(EnginePositionHal::engine_angle(0, 5400, ANGLE_360) == 1800);
    }

    SECTION("advancing the offset advances the spark by the same amount");
    {
        // The tuner's actual loop: turn the offset up, watch the timing light move. The spark angle
        // must track it one-for-one — this is what makes the number mean degrees.
        const AngleDeg10 tdc = 0, adv = 340;
        const AngleDeg10 spark_ref = EnginePositionHal::angle_btdc(tdc, adv, ANGLE_720);
        for (AngleDeg10 off = 0; off <= 3000; off = static_cast<AngleDeg10>(off + 100)) {
            // A fixed physical crank position reads `off` earlier in engine angle as the offset grows.
            const AngleDeg10 e0 = EnginePositionHal::engine_angle(2000, off, ANGLE_720);
            CHECK(fwd(e0, EnginePositionHal::engine_angle(2000, 0, ANGLE_720)) == off);
        }
        CHECK(spark_ref == 6860);                     // unchanged by the offset: it is engine-referenced
    }

    // -----------------------------------------------------------------------------------------
    // Engine cycle span. NOT a constant: the whole scheduler wraps against it.
    // -----------------------------------------------------------------------------------------
    SECTION("each cycle type has its own span");
    {
        CHECK(engine_cycle_angle((uint8_t)EngineCycleType::TWO_STROKE)  == 3600);   // 360 deg
        CHECK(engine_cycle_angle((uint8_t)EngineCycleType::FOUR_STROKE) == 7200);   // 720 deg
        // A Wankel's e-shaft turns 3x per rotor rev and each of the 3 faces is its own chamber, so
        // the pattern repeats every 1080 deg — the span that makes a FACE addressable.
        CHECK(engine_cycle_angle((uint8_t)EngineCycleType::ROTARY)      == 10800);  // 1080 deg
    }

    SECTION("the grid has a virtual tooth every 10 deg — for the LONGEST cycle too");
    {
        // MAX_VTEETH must cover the rotary's 108, or its schedule is silently truncated at 720 deg:
        // two thirds of a rotor revolution, with the last third of the faces never dispatched.
        CHECK(MAX_VTEETH >= 10800 / 100);
        CHECK(10800 / 100 == 108);
        CHECK(7200  / 100 == 72);
        CHECK(3600  / 100 == 36);
        // A bucket index must still fit the uint8_t it is stored in.
        CHECK(MAX_VTEETH <= 255);
    }

    SECTION("wrapping respects the configured cycle, not a hardcoded 720");
    {
        // 400 deg into a TWO-STROKE is 40 deg — it has already begun the next cycle. Wrapping that
        // against 720 would place it in a second revolution the engine does not have.
        CHECK(angle_wrap(4000, ANGLE_360)  == 400);
        CHECK(angle_wrap(4000, ANGLE_720)  == 4000);     // same angle, different engine
        CHECK(angle_wrap(4000, ANGLE_1080) == 4000);
        // ...and 800 deg is a real, distinct position on a rotary but not on a four-stroke.
        CHECK(angle_wrap(8000, ANGLE_1080) == 8000);
        CHECK(angle_wrap(8000, ANGLE_720)  == 800);
        // Negative wraps into the right span too.
        CHECK(angle_wrap(-100, ANGLE_360)  == 3500);
        CHECK(angle_wrap(-100, ANGLE_1080) == 10700);
    }

    SECTION("advance still lands before TDC in every cycle");
    {
        const AngleDeg10 adv = 340;
        for (AngleDeg10 cyc : {ANGLE_360, ANGLE_720, ANGLE_1080}) {
            const AngleDeg10 tdc   = static_cast<AngleDeg10>(cyc / 4);   // somewhere mid-cycle
            const AngleDeg10 spark = EnginePositionHal::angle_btdc(tdc, adv, cyc);
            CHECK(fwd(spark, tdc, cyc) == adv);
            // and across the cycle's zero
            const AngleDeg10 s0 = EnginePositionHal::angle_btdc(100, adv, cyc);
            CHECK(fwd(s0, 100, cyc) == adv);
        }
    }

    SECTION("wasted spark: one coil, two plugs, half a cycle apart");
    {
        // Four-stroke, 4-cyl with TDCs at 0/180/360/540: cylinder 1's companion is the one at 360,
        // sitting at TDC on its exhaust stroke.
        CHECK(wasted_companion_tdc(0,    ANGLE_720) == 3600);
        CHECK(wasted_companion_tdc(1800, ANGLE_720) == 5400);

        // TWO-STROKE twin, TDCs at 0/180. Half a cycle is 180 deg, so cylinder 1's companion is the
        // other cylinder — which is at BDC while this one fires. That is exactly the arrangement a
        // single coil running two cylinders relies on.
        CHECK(wasted_companion_tdc(0,    ANGLE_360) == 1800);
        CHECK(wasted_companion_tdc(1800, ANGLE_360) == 0);

        // Rotary: half a rotor revolution, in eccentric-shaft degrees.
        CHECK(wasted_companion_tdc(0, ANGLE_1080) == 5400);

        // The pairing MUST be mutual in every cycle — a coil cannot fire two cylinders that
        // disagree about being partners.
        for (AngleDeg10 cyc : {ANGLE_360, ANGLE_720, ANGLE_1080})
            for (AngleDeg10 tdc = 0; tdc < cyc; tdc = static_cast<AngleDeg10>(tdc + 150))
                CHECK(wasted_companion_tdc(wasted_companion_tdc(tdc, cyc), cyc) == tdc);

        // ...and the companion is never the cylinder itself, or the "pair" would be one plug.
        for (AngleDeg10 cyc : {ANGLE_360, ANGLE_720, ANGLE_1080})
            for (AngleDeg10 tdc = 0; tdc < cyc; tdc = static_cast<AngleDeg10>(tdc + 150))
                CHECK(wasted_companion_tdc(tdc, cyc) != tdc);
    }

    SECTION("the rotor ceiling is DERIVED from the slot pool, not declared beside it");
    {
        // Each face needs its own scheduler slot (own charge, own firing event); three faces per
        // rotor. So rotors are bounded by the cylinder pool, and the two cannot drift apart.
        CHECK(MAX_ROTORS == MAX_CYLINDERS / FACES_PER_ROTOR);
        CHECK(FACES_PER_ROTOR == 3);
        CHECK(MAX_ROTORS * FACES_PER_ROTOR <= MAX_CYLINDERS);
        // Every face of every rotor must have a node in the pool.
        CHECK(MAX_ROTORS * FACES_PER_ROTOR * SCHED_NODES_PER_CYLINDER <= SCHED_MAX_NODES);
        // Leading + trailing coil per rotor.
        CHECK(MAX_ROTORS * 2 <= MAX_IGN_CHANNELS);
    }

    SECTION("a rotor's three faces sit one e-shaft revolution apart");
    {
        // Rotor 1's faces fire at 0, 360 and 720 deg of the 1080 deg cycle — one per e-shaft rev.
        // They share the rotor's coils, so their TDCs are what distinguishes them.
        const AngleDeg10 face[3] = {0, 3600, 7200};
        for (int i = 0; i < 3; ++i)
            CHECK(angle_wrap(face[i], ANGLE_1080) == face[i]);          // all distinct, all in cycle
        CHECK(fwd(face[0], face[1], ANGLE_1080) == 3600);
        CHECK(fwd(face[1], face[2], ANGLE_1080) == 3600);
        CHECK(fwd(face[2], face[0], ANGLE_1080) == 3600);               // and back round
    }

    // -----------------------------------------------------------------------------------------
    // The fuel model's cycle arithmetic. A wrong divisor here is a SILENT fuel error of exactly
    // that ratio — the engine runs, badly, and nothing reports it. (The formulas mirror
    // FuelCalculator; this pins the numbers they must produce.)
    // -----------------------------------------------------------------------------------------
    SECTION("induction events per cycle = revolutions the cycle spans");
    {
        auto revs = [](uint8_t ct) {
            return static_cast<float>(engine_cycle_angle(ct)) / ANGLE_360;
        };
        CHECK(revs((uint8_t)EngineCycleType::FOUR_STROKE) == 2.0f);
        CHECK(revs((uint8_t)EngineCycleType::TWO_STROKE)  == 1.0f);
        CHECK(revs((uint8_t)EngineCycleType::ROTARY)      == 3.0f);

        // A 13B (2 rotors = 6 face-slots) at 6000 rpm: one power pulse every 180 deg of eccentric
        // shaft = 200 per second.
        const float pulses = (6000.0f / 60.0f) * (6 / revs((uint8_t)EngineCycleType::ROTARY));
        CHECK(pulses == 200.0f);

        // A 6-cylinder four-stroke at 6000 rpm: one every 120 deg = 300 per second.
        const float six = (6000.0f / 60.0f) * (6 / revs((uint8_t)EngineCycleType::FOUR_STROKE));
        CHECK(six == 300.0f);
    }

    SECTION("chamber volume: a 13B is 654cc per face, not 218");
    {
        // Displacement is the OVERALL nameplate figure (1308 for a 13B), and the rotary convention
        // counts ONE chamber per rotor — not all three faces. Dividing by the slot count would give
        // 218cc and run it lean by 3x.
        const float disp = 1308.0f;
        const int   slots = 6;                       // 2 rotors x 3 faces
        const float rotors = static_cast<float>(slots) / FACES_PER_ROTOR;
        CHECK(rotors == 2.0f);
        CHECK(disp / rotors == 654.0f);              // correct
        CHECK(disp / slots  == 218.0f);              // the trap
        // A piston engine still divides by cylinders: a 3.0 six is 500cc a pot.
        CHECK(3000.0f / 6 == 500.0f);
    }

    SECTION("the rule reproduces every Mazda rotary's real chamber volume");
    {
        // Published figures for this engine family, which are the TRUE displacement ("required ... for VE
        // tuning") — the same convention Jason confirmed. displacement / rotors must land on the
        // known chamber volume for each, or VE stops reading physically.
        struct R { const char* name; float disp; int rotors; float chamber; };
        const R engines[] = {
            {"10A", 982.0f,  2, 491.0f},
            {"12A", 1146.0f, 2, 573.0f},
            {"13B", 1308.0f, 2, 654.0f},
            {"20B", 1962.0f, 3, 654.0f},   // 3 x 654 — same chamber as a 13B, one more rotor
            {"26B", 2616.0f, 4, 654.0f},   // 4 x 654
        };
        for (const R& e : engines) {
            const int slots = e.rotors * FACES_PER_ROTOR;
            const float rotors = static_cast<float>(slots) / FACES_PER_ROTOR;
            CHECK(rotors == static_cast<float>(e.rotors));
            CHECK(e.disp / rotors == e.chamber);
            // ...and every face of every one of them fits the scheduler's slot pool.
            CHECK(slots <= MAX_CYLINDERS);
        }
        // The 26B is the ceiling case: 4 rotors x 3 faces = 12 slots, the whole pool, nothing spare.
        CHECK(4 * FACES_PER_ROTOR == MAX_CYLINDERS);
        CHECK(MAX_ROTORS == 4);
    }

    // -----------------------------------------------------------------------------------------
    // Rotary leading/trailing. Both plugs fire the same chamber, so the trailing one is an angle
    // OFFSET and nothing more — additive like every other spark trim, so negative retards.
    // -----------------------------------------------------------------------------------------
    SECTION("split is degrees of RETARD: trailing = leading - split");
    {
        const AngleDeg10 tdc  = 0;
        const AngleDeg10 lead = 250;                 // 25.0 deg advance

        for (AngleDeg10 split : {(AngleDeg10)60, (AngleDeg10)100, (AngleDeg10)150}) {  // 6, 10, 15
            const AngleDeg10 trail = static_cast<AngleDeg10>(lead - split);
            CHECK(trail < lead);                                        // retarded = less advance
            const AngleDeg10 lead_a  = EnginePositionHal::angle_btdc(tdc, lead,  ANGLE_1080);
            const AngleDeg10 trail_a = EnginePositionHal::angle_btdc(tdc, trail, ANGLE_1080);
            // The engine reaches the LEADING plug first, then the trailing one `split` later.
            CHECK(fwd(lead_a, trail_a, ANGLE_1080) == split);
        }
    }

    SECTION("worked example: lead -5 BTDC, split 15, trail -20 BTDC");
    {
        // "The OEM timing marks for many rotary engines is at -5 and -20 BTDC (5ATDC and 20ATDC).
        //  A Firing Angle of -5 would be used, with a 15 degree Trailing Split Angle."
        const AngleDeg10 lead  = -50;                // -5.0 deg BTDC == 5 deg ATDC
        const AngleDeg10 split = 150;                // 15.0 deg of retard
        const AngleDeg10 trail = static_cast<AngleDeg10>(lead - split);
        CHECK(trail == -200);                        // -20.0 BTDC == 20 deg ATDC, as documented

        // Both plugs fire AFTER TDC at these numbers, leading first.
        const AngleDeg10 tdc = 0;
        const AngleDeg10 lead_a  = EnginePositionHal::angle_btdc(tdc, lead,  ANGLE_1080);
        const AngleDeg10 trail_a = EnginePositionHal::angle_btdc(tdc, trail, ANGLE_1080);
        CHECK(fwd(tdc, lead_a,  ANGLE_1080) == 50);    // 5 deg after TDC
        CHECK(fwd(tdc, trail_a, ANGLE_1080) == 200);   // 20 deg after TDC
        CHECK(fwd(lead_a, trail_a, ANGLE_1080) == split);
    }

    SECTION("the advance clamp must not forbid a rotary's negative lead");
    {
        // The OEM rotary case commands a NEGATIVE lead (-5 BTDC = 5 ATDC). Ignition clamps advance
        // to [min_adv_deg, max_adv_deg], so if that floor were ever tightened to 0 the lead would be
        // silently clamped to TDC and the whole rotary timing model would quietly stop working.
        CHECK(g_config.ignition.min_adv_deg <= -50);      // room for at least -5.0 deg
        CHECK(g_config.ignition.max_adv_deg >   0);
        // The TRAIL is computed after that clamp, deliberately: a 15 deg split must be free to take
        // it to -20 without being clipped, or the split silently is not the split you asked for.
        const AngleDeg10 lead  = -50, split = 150;
        CHECK(static_cast<AngleDeg10>(lead - split) < g_config.ignition.min_adv_deg);
    }

    SECTION("split 0 fires both plugs together");
    {
        const AngleDeg10 lead = 250;
        CHECK(EnginePositionHal::angle_btdc(0, lead,     ANGLE_1080)
              == EnginePositionHal::angle_btdc(0, static_cast<AngleDeg10>(lead - 0), ANGLE_1080));
    }

    SECTION("the split trails the commanded advance, it does not replace it");
    {
        // Whatever the timing table commands at that operating point, the trailing plug stays
        // `split` behind it — that is what makes it an offset rather than a second timing table.
        const AngleDeg10 split = 60;
        for (AngleDeg10 lead = -100; lead <= 400; lead = static_cast<AngleDeg10>(lead + 50)) {
            const AngleDeg10 trail = static_cast<AngleDeg10>(lead - split);
            CHECK(static_cast<AngleDeg10>(lead - trail) == split);
        }
    }

    // -----------------------------------------------------------------------------------------
    // A TRAILING PLUG COMES FROM THE ENGINE. A Wankel has two plugs per rotor because of how it burns,
    // so the trailing nodes exist for the ROTARY cycle — and which coil a trailing plug fires is an
    // output row (function Ignition, plug Trailing). A row marked Trailing on a piston engine has no plug
    // to fire, so it is not bound and its pin is left free.
    // -----------------------------------------------------------------------------------------
    SECTION("a trailing coil row is claimed on a ROTARY cycle, and left free on a piston engine");
    {
        struct Ch final : ITimerChannel {
            [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return 0; }
            [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return 1; }
            void force_output_now(OutputAction) noexcept override {}
        };
        static Ch pins[OUTPUTS_OUTPUT_COUNT];
        ITimerChannel* pool[OUTPUTS_OUTPUT_COUNT];
        for (unsigned i = 0; i < OUTPUTS_OUTPUT_COUNT; ++i) pool[i] = &pins[i];

        // One rotor: a leading row on IGN1 and a trailing row on IGN(MAX_ROTORS+1), both naming rotor 1 —
        // the rotor's two coils.
        auto claims_trailing = [&](uint8_t cycle_type) {
            EngineConfig eng{};
            eng.cylinder_count = FACES_PER_ROTOR;
            eng.cycle_type     = cycle_type;
            eng.num_inj_stages = 1;
            eng.inj_stage[0].mode                 = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
            eng.inj_stage[0].injections_per_cycle = 1;
            for (int f = 0; f < FACES_PER_ROTOR; ++f)
                eng.firing_order[f].cyl = static_cast<uint8_t>(f + 1);
            OutputMap map{};
            map.ign[0]          = { true, 1, 0, 0, true };
            map.ign[MAX_ROTORS] = { true, 1, 0, 1, true };
            PinArbiter arb;
            arb.bind(pool, OUTPUTS_OUTPUT_COUNT);
            EventScheduler sched;
            sched.assign_pin_arbiter(arb);
            sched.set_config(eng, map, true);
            sched.claim_outputs();
            return arb.owner_of(OUT_ROW_IGN_BASE + MAX_ROTORS) == PinOwner::IGNITION;
        };

        CHECK(claims_trailing((uint8_t)EngineCycleType::ROTARY));
        CHECK(!claims_trailing((uint8_t)EngineCycleType::FOUR_STROKE));
        CHECK(!claims_trailing((uint8_t)EngineCycleType::TWO_STROKE));
    }

    // firing_order_valid — the refuse-to-schedule gate. Valid iff firing_order[0..n-1] is a clean
    // permutation of 1..cylinder_count. A false result means the scheduler must not fire.
    {
        auto mk = [](std::initializer_list<uint8_t> order, uint8_t n) {
            EngineConfig e{};
            e.cylinder_count = n;
            uint8_t i = 0;
            for (uint8_t c : order)
                if (i < ENGINE_CYL_COUNT) e.firing_order[i++].cyl = c;
            return e;
        };
        // Clean permutations of 1..n.
        CHECK(firing_order_valid(mk({1, 3, 4, 2}, 4)));
        CHECK(firing_order_valid(mk({1, 5, 3, 6, 2, 4}, 6)));
        CHECK(firing_order_valid(mk({1}, 1)));
        CHECK(firing_order_valid(mk({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 12)));
        // Duplicate cylinder (5 twice) with 6 missing — the exact silent-dead-cylinder footgun.
        CHECK(!firing_order_valid(mk({1, 5, 3, 5, 2, 4}, 6)));
        // A hole (0) inside the live range.
        CHECK(!firing_order_valid(mk({1, 3, 0, 2}, 4)));
        // In 1..12 but out of range for THIS count (8 on a 6-cyl).
        CHECK(!firing_order_valid(mk({1, 5, 3, 8, 2, 4}, 6)));
        // Above the hardware max, and zero cylinders.
        CHECK(!firing_order_valid(mk({1, 2, 13, 4}, 4)));
        CHECK(!firing_order_valid(mk({}, 0)));
        // The tail (>= cylinder_count) is not examined — garbage there does not invalidate a good order.
        {
            EngineConfig e = mk({1, 3, 4, 2}, 4);
            e.firing_order[5].cyl = 3;   // duplicate, but in the ignored tail
            e.firing_order[7].cyl = 9;
            CHECK(firing_order_valid(e));
        }
    }

    SECTION("dwell is clamped to the SHARED coil's spark spacing (distributor, wasted spark)");
    {
        // V8 distributor, 6000 rpm: sparks 90° apart. A 3 ms dwell is 108° — longer than the gap.
        const uint32_t want = (60000u * 3000u) / 166660u;           // 108° in decidegrees ≈ 1080
        const uint32_t dist = EnginePositionHal::clamp_dwell_dd(want, IgnitionCoilMode::SINGLE_COIL_DISTRIBUTOR,
                                                                ANGLE_720, 8, true, false);
        fprintf(stdout, "    V8 distributor: want %u dd, allowed %u dd (spacing 900)\n", want, dist);
        CHECK(dist < 900u);                                         // inside the 90° spacing
        CHECK(dist <= 675u);                                        // ~75 % duty on the one coil
        const uint32_t cop = EnginePositionHal::clamp_dwell_dd(want, IgnitionCoilMode::COIL_ON_PLUG,
                                                               ANGLE_720, 8, true, false);
        CHECK(cop == want);                                         // a coil per cylinder: unaffected
        const uint32_t ws = EnginePositionHal::clamp_dwell_dd(5000u, IgnitionCoilMode::WASTED_SPARK,
                                                              ANGLE_720, 4, true, false);
        CHECK(ws <= 3400u);                                         // a wasted pair fires every 360°
    }

    SECTION("the knock window opens at Window Start, or LOOK-AHEAD before this cylinder's spark if earlier");
    {
        // Angles BTDC, + advanced. A fixed window angle cannot follow the timing: -40 straddled a 17 deg
        // spark and was after a 45 deg one, which is exactly where pre-ignition is likeliest.
        using EPH = EnginePositionHal;
        CHECK(EPH::knock_open_btdc(-100, 0, 170) == -100);      // no look-ahead: Window Start (10 ATDC)
        CHECK(EPH::knock_open_btdc(-100, 100, 170) == 270);     // 10 before a 17 deg spark
        CHECK(EPH::knock_open_btdc(-100, 100, 450) == 550);     // …and before a 45 deg one: it follows
        CHECK(EPH::knock_open_btdc(-100, 100, -50) == 50);      // a retarded spark (5 ATDC) too
        CHECK(EPH::knock_open_btdc(400, 100, 170) == 400);      // a Window Start already earlier wins
        CHECK(EPH::knock_open_btdc(-100, 200, 800) == 850);     // never earlier than it can be armed
        // and armed where it says: 27 deg BTDC is 27 deg before this cylinder's TDC
        const AngleDeg10 tdc = 1800;
        CHECK(fwd(EPH::angle_btdc(tdc, EPH::knock_open_btdc(-100, 100, 170), ANGLE_720), tdc) == 270);
    }

    return test_summary();
}

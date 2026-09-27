// THE OUTPUT SLOT, END TO END — the module that turns a tune's description of an output into a pin
// that moves.
//
// OutputManager had no test at all: it was compiled only by the firmware link, which is how it
// reached the bench with expr::Result read the wrong way (a compile error nothing on the host ever
// saw) and how its gate could not be exercised without an engine. Everything it does is decided by
// config bytes and bus values, so all of it is testable on a desk.
//
// What is pinned here, in the order a slot lives:
//   build     — a Generic row builds on its own pin; any other function builds nothing here; a pin
//               another owner (the scheduler) still holds is refused, flagged, and drives nothing;
//   gate      — no condition = always on; an ON condition latches; an OFF condition is the other
//               edge of a deadband; min-on / min-off / max-on / re-arm; on_invalid when the VM
//               cannot answer.
//   value     — candidates (arbitrated by role), the slot's own duty map, a fixed constant.
//   encode    — scale/offset/clamp, and the fail-safe when no candidate is valid.
//   rebuild   — region-scoped: an Outputs write rebuilds, any other write does not — and so does the
//               scheduler re-binding its firing pins.
//
//   build: tests/CMakeLists.txt -> ctest -R output_manager
#include "test_helpers.h"

#include "Integration/OutputManager.h"
#include "Signal/EnginePosition.h"
#include "Signal/SignalBus.h"
#include "Engine/EngineFrame.h"
#include "Signal/ExprIsa.h"
#include "../generated/ecu_config.h"
#include "../generated/signal_ids.h"
#include "../generated/shadow_meta.h"
#include "../generated/table_registry.h"

#include <cstring>
#include <vector>

// THE TABLE A SLOT NAMES, resolved by NAME rather than by number. An id is the table's place in the
// schema's own order, so hard-coding one here would make the test a hostage of where a table sits in
// the definition — and the studio resolves a name to an id through this same registry.
static int16_t table_id(const char* name) {
    for (uint16_t i = 0; i < EXPR_TABLE_COUNT; ++i)
        if (std::strcmp(expr_table_name(i), name) == 0) return static_cast<int16_t>(i);
    return -1;
}

// ---- fakes -------------------------------------------------------------------------------------
class FakeAlarm final : public IAlarmTimer {
public:
    uint32_t clock = 0;
    void arm(uint32_t t) noexcept override { deadline_ = t; armed_ = true; }
    void disarm() noexcept override { armed_ = false; }
    uint32_t now() const noexcept override { return clock; }
    void register_callback(MatchCallback cb, void* ud) noexcept override { cb_ = cb; ud_ = ud; }
    void advance(uint32_t dt) {
        const uint32_t target = clock + dt; int guard = 0;
        while (armed_ && (int32_t)(deadline_ - target) <= 0) {
            if ((int32_t)(deadline_ - clock) > 0) clock = deadline_;
            armed_ = false;
            if (cb_) cb_(ud_);
            if (++guard > 1000000) { CHECK(!"runaway"); return; }
        }
        clock = target;
    }
private:
    MatchCallback cb_ = nullptr; void* ud_ = nullptr; uint32_t deadline_ = 0; bool armed_ = false;
};

class FakePin final : public ITimerChannel {
public:
    bool driven = false, high = false;
    int  writes = 0;
    uint32_t get_current_ticks()    const noexcept override { return 0; }
    uint32_t get_ticks_per_second() const noexcept override { return 100000; }
    void enable_output(OutputAction a) noexcept override { driven = true; high = (a == OutputAction::DRIVE_HIGH); }
    void disable_output() noexcept override { driven = false; }
    void force_output_now(OutputAction a) noexcept override {
        ++writes;
        if (a == OutputAction::DRIVE_HIGH) high = true;
        else if (a == OutputAction::DRIVE_LOW) high = false;
    }
};

// The output pool: row i IS pin i.
struct FakePool {
    static constexpr int N = 8;
    FakePin pins[N];
};

// ---- bytecode, hand-assembled ------------------------------------------------------------------
// The studio compiles these; a test writes them directly, which also proves the FORMAT is what the
// firmware expects rather than what the compiler happens to emit.
static void prog_sig_gt(uint8_t* out, size_t cap, SignalId sig, int8_t n) {
    std::memset(out, 0, cap);
    size_t i = 0;
    out[i++] = expr::OP_PUSH_SIG;
    const uint16_t sel = uint16_t(sig) + 1;                 // 0 = None, else SignalId+1
    std::memcpy(out + i, &sel, 2); i += 2;
    out[i++] = expr::OP_PUSH_I8; out[i++] = uint8_t(n);
    out[i++] = expr::OP_GT;
    out[i++] = expr::OP_END;
}

// The starter's on-condition SHAPE: "<sig> > n and engine_state < 2". Two terms and an AND, which is
// what makes it different from prog_sig_gt — the interaction being pinned below is precisely that the
// on side goes false the instant the off side goes true, and one term cannot express that.
static void prog_sig_gt_and_state_lt(uint8_t* out, size_t cap, SignalId sig, int8_t n, int8_t state) {
    std::memset(out, 0, cap);
    size_t i = 0;
    uint16_t sel = uint16_t(sig) + 1;
    out[i++] = expr::OP_PUSH_SIG; std::memcpy(out + i, &sel, 2); i += 2;
    out[i++] = expr::OP_PUSH_I8;  out[i++] = uint8_t(n);
    out[i++] = expr::OP_GT;
    sel = uint16_t(SIG_ENGINE_STATE) + 1;
    out[i++] = expr::OP_PUSH_SIG; std::memcpy(out + i, &sel, 2); i += 2;
    out[i++] = expr::OP_PUSH_I8;  out[i++] = uint8_t(state);
    out[i++] = expr::OP_LT;
    out[i++] = expr::OP_AND;
    out[i++] = expr::OP_END;
}

static void prog_state_ge(uint8_t* out, size_t cap, int8_t state) {
    std::memset(out, 0, cap);
    size_t i = 0;
    const uint16_t sel = uint16_t(SIG_ENGINE_STATE) + 1;
    out[i++] = expr::OP_PUSH_SIG; std::memcpy(out + i, &sel, 2); i += 2;
    out[i++] = expr::OP_PUSH_I8;  out[i++] = uint8_t(state);
    out[i++] = expr::OP_GE;
    out[i++] = expr::OP_END;
}

// THE COMMS MANAGER, WITHOUT THE COMMS. OutputManager needs exactly two things from it — the shadow
// mask and clear_shadow, both inline — but linking the real translation unit drags in the SD card,
// the trigger logger and the position HAL, none of which this is about. The constructor is `=
// default` there, so defining it here costs nothing and keeps the test to the module under test.
namespace Comms { CommsManager::CommsManager() = default; }

// Owned by EnginePositionHal on the target; the manager rebuilds when it moves.
volatile uint32_t g_firing_bind_generation = 0;

// KEY-ON. Every generic output is parked unless the system is active, so that a tune written on the
// bench cannot pulse a fuel pump through a USB cable. main.cpp owns this; the test owns its own copy
// and flips it, because "does it park" is one of the questions.
bool g_system_active = true;

// The stub platform's millisecond clock — the test drives it, because every timing below is about
// what happens BETWEEN frames rather than within one.
extern "C" uint32_t g_stub_tick_ms;

static Comms::CommsManager g_comms;
static DtcManager          g_dtc;

struct Rig {
    FakeAlarm     clk;
    SoftPwm       pwm;
    FakePool      pins;
    PinArbiter    arbiter;
    OutputManager mgr;
    EngineFrame   frame{};
    EnginePosition pos{};

    // THE POOL THE ARBITER ARBITRATES OVER, one entry per row. A row beyond it is claimable by nobody —
    // which is correct (an unbound pin is not an output).
    ITimerChannel* out_pool[FakePool::N] = {};

    Rig() {
        pwm.bind(&clk);
        for (int i = 0; i < FakePool::N; ++i) out_pool[i] = &pins.pins[i];
        arbiter.bind(out_pool, FakePool::N);
        mgr.init(pwm, 1000, arbiter, g_comms, g_dtc);
    }
    void tick(SignalBus& bus) { mgr.update(pos, bus, frame); }
};

static OutputConfig& slot(int i) { return g_config.outputs.output[i]; }

// A plain digital Generic row on pin `row`, driven by one candidate, no condition.
static void simple_slot(int row, SignalId src) {
    OutputConfig& o = slot(row);
    std::memset(&o, 0, sizeof(o));
    o.function = static_cast<uint8_t>(OutputFunction::GENERIC); o.kind = 1 /*digital*/; o.active_high = 1;
    o.n_cand = 1; o.cand[0].sig = uint16_t(src); o.cand[0].role = 0 /*primary*/;   // RAW id, not +1
    o.scale_x1000 = 1000; o.clamp_hi_x10 = 1000;      // x1, clamp 0..100
    o.on_invalid = 0;                                  // unanswerable -> off
    o.value_source = 0;                                // candidates
}

int main() {
    fprintf(stdout, "=== OutputManager ===\n");
    for (unsigned i = 0; i < OUTPUTS_OUTPUT_COUNT; ++i) std::memset(&slot(i), 0, sizeof(OutputConfig));

    SECTION("a Generic row drives its own pin; the same row as None, Ignition or Injector builds nothing");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].driven);
        CHECK(r.pins.pins[2].high);          // digital: value over threshold -> on

        for (OutputFunction fn : { OutputFunction::NONE, OutputFunction::IGNITION, OutputFunction::INJECTOR }) {
            slot(2).function = static_cast<uint8_t>(fn);
            Rig r2;                           // a fresh manager: the build is what is under test
            SignalBus b2; b2.set(SIG_MAP, 80.0f);
            r2.tick(b2);
            CHECK(!r2.pins.pins[2].driven);
        }
        slot(2).function = static_cast<uint8_t>(OutputFunction::GENERIC);
    }

    SECTION("a pin the scheduler still holds is refused, flagged, and never driven");
    {
        Rig r;
        simple_slot(3, SIG_MAP);
        // THE ROW WAS A COIL a moment ago and the scheduler has not reconfigured yet, so it still owns
        // the pin — the one way a row can meet a pin that is not free.
        CHECK(r.arbiter.claim(3, PinOwner::IGNITION) != nullptr);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(r.mgr.has_conflict());
        CHECK(!r.pins.pins[3].driven);       // the OUTPUT's channel is never enabled
    }

    SECTION("no condition = always on (what every output did before conditions existed)");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].high);
    }

    SECTION("an ON condition gates the slot, and the VM decides it from the bus");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);   // rpm > 100
        SignalBus bus; bus.set(SIG_MAP, 80.0f); bus.set(SIG_RPM, 0.0f);
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);                       // condition false -> off
        bus.set(SIG_RPM, 900.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].high);                        // true -> on
        bus.set(SIG_RPM, 0.0f);
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);                       // one condition: off is "not on"
    }

    SECTION("two conditions are a deadband: it latches on, and only the OFF condition releases it");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        prog_sig_gt(slot(2).on_expr,  sizeof(slot(2).on_expr),  SIG_RPM, 100);   // on above 100
        prog_sig_gt(slot(2).off_expr, sizeof(slot(2).off_expr), SIG_CLT, 90);    // off above 90 C
        SignalBus bus; bus.set(SIG_MAP, 80.0f); bus.set(SIG_RPM, 900.0f); bus.set(SIG_CLT, 20.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].high);
        bus.set(SIG_RPM, 0.0f);                            // the ON condition goes away…
        r.tick(bus);
        CHECK(r.pins.pins[2].high);                        // …and it stays on: that is the latch
        bus.set(SIG_CLT, 95.0f);                           // the OFF condition is what releases it
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);
    }

    SECTION("value from a FIXED constant, and from the slot's own duty map");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        slot(2).value_source = 2;            // Fixed
        slot(2).fixed_x10    = 750;          // 75.0 %
        SignalBus bus; bus.set(SIG_MAP, 0.0f);   // the candidate says 0 — the fixed value must win
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        // Table: the slot NAMES one out of the generic pool — two points, 0 % at 0 C, 100 % at 100 C.
        slot(2).value_source   = 1;
        slot(2).duty_table_sel = table_id("generic_tables_table_1");
        CHECK(slot(2).duty_table_sel >= 0);
        auto& gt = g_config.generic_tables;
        gt.table_1_x_src = SIG_CLT;
        gt.t1_x_axis_n = 2; gt.table_1_y_en = 0;
        gt.t1_x_axis[0] = 0.f; gt.t1_x_axis[1] = 100.f;
        gt.table_1[0] = 0.f;  gt.table_1[1] = 100.f;
        Rig r2;
        SignalBus b2; b2.set(SIG_CLT, 100.0f);
        r2.tick(b2);
        CHECK(r2.pins.pins[2].high);
        SignalBus b3; b3.set(SIG_CLT, 0.0f);
        r2.tick(b3);
        CHECK(!r2.pins.pins[2].high);
    }

    SECTION("value from an EXPRESSION: the duty is arithmetic, not a surface to draw");
    {
        // "clt" as a duty: at 60 C the slot commands 60 %. The point is that a duty can be computed
        // from any channel rather than looked up from a map somebody had to fill in.
        Rig r;
        simple_slot(2, SIG_MAP);
        slot(2).value_source = 3;                        // Expression
        std::memset(slot(2).duty_expr, 0, sizeof(slot(2).duty_expr));
        {   size_t i = 0; uint8_t* d = slot(2).duty_expr;
            d[i++] = expr::OP_PUSH_SIG;
            const uint16_t sel = uint16_t(SIG_CLT) + 1; std::memcpy(d + i, &sel, 2); i += 2;
            d[i++] = expr::OP_END;
        }
        SignalBus bus; bus.set(SIG_CLT, 60.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].high);                      // 60 >= the 50 midpoint
        // …and the CHANNEL says the pin, not the 60 that decided it: a digital slot publishes the
        // level it drove. It used to publish the command, so this read 60 for a pin that was simply
        // on — and would have read 40 for one that was off, in the same units, looking the same.
        CHECK_NEAR(bus.get(OUT_SIGNALS[2]), 100.0f, 0.01f);

        bus.set(SIG_CLT, 20.0f);
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);
        CHECK_NEAR(bus.get(OUT_SIGNALS[2]), 0.0f, 0.01f);

        // A program that cannot be answered falls to the FAILSAFE, exactly as a dead candidate does —
        // it does not read as zero, which would be a claim rather than an admission.
        slot(2).failsafe_x10 = 700;                      // 70 %
        std::memset(slot(2).duty_expr, 0, sizeof(slot(2).duty_expr));
        {   size_t i = 0; uint8_t* d = slot(2).duty_expr;
            d[i++] = expr::OP_PUSH_SIG;
            const uint16_t sel = uint16_t(SIG_RPM) + 1;  // never published in this test
            std::memcpy(d + i, &sel, 2); i += 2;
            d[i++] = expr::OP_END;
        }
        Rig r2;
        SignalBus b2; b2.set(SIG_CLT, 60.0f);
        r2.tick(b2);
        CHECK(r2.pins.pins[2].high);            // the 70 % failsafe is over the 50 midpoint…
        CHECK_NEAR(b2.get(OUT_SIGNALS[2]), 100.0f, 0.01f);   // …so the slot reports itself ON
    }

    SECTION("the CARRIER moves too: fixed, from a table, or computed");
    {
        // A PWM slot's frequency is the third question an output answers. Most loads want one number
        // and never think about it again; some — a swept valve, a buzzer whose pitch IS the output —
        // want it to follow something. The pulse engine sees the period, so this checks the waveform.
        Rig r;
        simple_slot(2, SIG_MAP);
        slot(2).kind = 0;                                 // PWM
        slot(2).pwm_freq_hz = 250;                        // fixed carrier
        slot(2).value_source = 2; slot(2).fixed_x10 = 500;   // 50 % duty, so edges are easy to see
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        // tps = 1000 ticks/s in this rig, so 250 Hz is a period of 4 ticks.
        CHECK(r.pwm.period_ticks(0) == 1000u / 250u);

        // From a TABLE — the carrier comes out of the pool too, and by the same id space: 500 Hz.
        slot(2).freq_source    = 1;
        slot(2).freq_table_sel = table_id("generic_tables_table_2");
        CHECK(slot(2).freq_table_sel >= 0);
        auto& gt2 = g_config.generic_tables;
        gt2.table_2_x_src = SIG_CLT;
        gt2.t2_x_axis_n = 2; gt2.table_2_y_en = 0;
        gt2.t2_x_axis[0] = 0.f; gt2.t2_x_axis[1] = 100.f;
        gt2.table_2[0] = 500.f; gt2.table_2[1] = 500.f;
        Rig r2;
        SignalBus b2; b2.set(SIG_MAP, 80.0f); b2.set(SIG_CLT, 50.0f);
        r2.tick(b2);
        CHECK(r2.pwm.period_ticks(0) == 1000u / 500u);

        // …and COMPUTED: the carrier is whatever the program says (clt Hz).
        slot(2).freq_source = 2;
        std::memset(slot(2).freq_expr, 0, sizeof(slot(2).freq_expr));
        {   size_t i = 0; uint8_t* d = slot(2).freq_expr;
            d[i++] = expr::OP_PUSH_SIG;
            const uint16_t sel = uint16_t(SIG_CLT) + 1; std::memcpy(d + i, &sel, 2); i += 2;
            d[i++] = expr::OP_END;
        }
        Rig r3;
        SignalBus b3; b3.set(SIG_MAP, 80.0f); b3.set(SIG_CLT, 100.0f);   // 100 Hz
        r3.tick(b3);
        CHECK(r3.pwm.period_ticks(0) == 1000u / 100u);

        // An unanswerable carrier falls back to the FIXED one rather than stopping the output: a duty
        // with no carrier is not an output at all.
        Rig r4;
        SignalBus b4; b4.set(SIG_MAP, 80.0f);            // clt never published
        r4.tick(b4);
        CHECK(r4.pwm.period_ticks(0) == 1000u / 250u);
    }

    SECTION("an unanswerable condition follows the slot's own policy, not a default");
    {
        // A program referencing a channel nothing publishes cannot be answered. on_invalid says what
        // to do about it, and the two answers must actually differ.
        Rig off;
        simple_slot(2, SIG_MAP);
        prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);
        slot(2).on_invalid = 0;                            // Off
        SignalBus bus; bus.set(SIG_MAP, 80.0f);            // rpm never published -> invalid
        off.tick(bus);
        CHECK(!off.pins.pins[2].high);

        slot(2).on_invalid = 1;                            // On
        Rig on;
        SignalBus b2; b2.set(SIG_MAP, 80.0f);
        on.tick(b2);
        CHECK(on.pins.pins[2].high);
    }

    SECTION("on bench power every output is parked, and returns when the key comes on");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].driven);

        g_system_active = false;                       // USB / bench power
        r.tick(bus);
        CHECK(!r.pins.pins[2].driven);                 // released to Hi-Z, not merely commanded off

        g_system_active = true;
        r.tick(bus);
        CHECK(r.pins.pins[2].driven);                  // and it rebuilds itself
    }

    // ---- the timings: the half of a gate that is about SAFETY rather than logic ------------------
    // A fuel pump's prime and a starter's crank limit are both timings, and both are the reason the
    // gate is stateful at all: the expression VM is pure by design, so "held true for three seconds"
    // cannot live in it. The stub clock is the test's to drive.
    SECTION("minimum ON time holds an output up after its condition has gone");
    {
        g_stub_tick_ms = 1000;
        Rig r;
        simple_slot(2, SIG_MAP);
        prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);
        slot(2).min_on_ms = 500;
        // A GATE THAT HAS A QUESTION IS BORN OFF, and the minimum on time does not apply to a state
        // it was never in. This used to be born ON — an output with no condition is always on, and a
        // freshly built slot inherited that — so a slot whose condition was false still drove its pin
        // until the minimum elapsed. Harmless for a fan; a starter that latches its crank cranked the
        // engine on the first frame after every rebuild, because at rest neither of its conditions is
        // true and nothing was left to turn it off. An output does nothing until its own condition
        // says to. (The always-on case is decided in update_gates before the state is read, so it is
        // unaffected — see the section below.)
        SignalBus bus; bus.set(SIG_MAP, 80.0f); bus.set(SIG_RPM, 0.0f);
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);            // condition false at birth: it never came on

        g_stub_tick_ms = 1700;
        bus.set(SIG_RPM, 900.0f);               // the ON edge, at a known time
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        bus.set(SIG_RPM, 0.0f);                 // the reason to be on is gone…
        g_stub_tick_ms = 1900;                  // …but only 200 ms have passed
        r.tick(bus);
        CHECK(r.pins.pins[2].high);             // still on: that is the minimum

        g_stub_tick_ms = 2300;                  // past 500 ms
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);
    }

    // BORN OFF IS ONLY FOR A SLOT THAT ASKS. An output with no conditions at all is always on, and
    // that answer is reached in update_gates before the gate state is consulted — so the birth rule
    // must not have quietly turned those off too. Both halves, side by side, because the difference
    // between them is the whole rule.
    SECTION("a slot with no condition is on from birth; one with a condition is not");
    {
        g_stub_tick_ms = 5000;
        {
            Rig r;
            simple_slot(2, SIG_MAP);         // no on_expr, no off_expr: it asks nothing
            SignalBus bus; bus.set(SIG_MAP, 80.0f);
            r.tick(bus);
            CHECK(r.pins.pins[2].high);         // always on, from the very first frame
        }
        {
            Rig r;
            simple_slot(2, SIG_MAP);
            prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);
            SignalBus bus; bus.set(SIG_MAP, 80.0f); bus.set(SIG_RPM, 0.0f);
            r.tick(bus);
            CHECK(!r.pins.pins[2].high);        // it asked, and the answer was no
        }
    }

    SECTION("maximum ON time releases the output and locks it out until the re-arm delay");
    {
        // This is a starter motor: crank for at most N seconds, then let go and refuse to try again
        // until the motor has had a rest, whatever the button says.
        g_stub_tick_ms = 10000;
        Rig r;
        simple_slot(2, SIG_MAP);
        prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);
        slot(2).max_on_ms = 3000;
        slot(2).rearm_ms  = 2000;
        SignalBus bus; bus.set(SIG_MAP, 80.0f); bus.set(SIG_RPM, 0.0f);
        r.tick(bus);                            // born off, so the crank has a real start time
        CHECK(!r.pins.pins[2].high);

        bus.set(SIG_RPM, 900.0f);               // "button pressed"
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        g_stub_tick_ms = 12000;                 // 2 s in, still cranking
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        g_stub_tick_ms = 13100;                 // past the 3 s limit
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);            // released, with the condition STILL true

        g_stub_tick_ms = 14000;                 // inside the lockout: the button cannot restart it
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);

        g_stub_tick_ms = 15200;                 // past the re-arm
        r.tick(bus);
        CHECK(r.pins.pins[2].high);             // and now it may crank again
        g_stub_tick_ms = 0;
    }

    SECTION("each built slot SAYS what it is doing, and an unbuilt one says nothing");
    {
        // An output nobody can watch cannot be tuned or tested. out_<slot> carries what the slot is
        // DOING: a PWM slot its duty, a digital slot the level it drove — so a candidate reading 80
        // on a digital slot reports 100, because that is what the pin did with it.
        Rig r;
        simple_slot(2, SIG_MAP);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(bus.valid(OUT_SIGNALS[2]));
        CHECK(r.pins.pins[2].high);
        CHECK_NEAR(bus.get(OUT_SIGNALS[2]), 100.0f, 0.01f);

        // …and a PWM slot keeps the NUMBER, because there it really is a duty. Same candidate, same
        // 80, and this time 80 is the answer — which is the whole reason the digital case had to
        // change rather than the channel being made a flag for everyone.
        {
            Rig rp;
            simple_slot(2, SIG_MAP);
            slot(2).kind = 0;                            // PWM
            slot(2).pwm_freq_hz = 250;
            SignalBus bp; bp.set(SIG_MAP, 80.0f);
            rp.tick(bp);
            CHECK_NEAR(bp.get(OUT_SIGNALS[2]), 80.0f, 0.01f);
        }

        // A slot that was never built has no state to report — and an invalid channel says that,
        // where a zero would claim "off".
        CHECK(!bus.valid(OUT_SIGNALS[1]));

        // Gate it shut: the slot still reports, and reports the off value rather than going quiet.
        prog_sig_gt(slot(2).on_expr, sizeof(slot(2).on_expr), SIG_RPM, 100);
        Rig r2;
        SignalBus b2; b2.set(SIG_MAP, 80.0f); b2.set(SIG_RPM, 0.0f);
        r2.tick(b2);
        CHECK(b2.valid(OUT_SIGNALS[2]));
        CHECK_NEAR(b2.get(OUT_SIGNALS[2]), 0.0f, 0.01f);
    }

    // ---- THE STARTER, END TO END --------------------------------------------------------------
    // The template's own conditions, not a stand-in: on = "button and not running", off = "running".
    // Every rule the button is supposed to obey is an interaction between the deadband, the maximum-on
    // trip and the re-arm lockout, and none of them can be seen one at a time.
    SECTION("a momentary press latches a crank; a held button re-cranks; RUNNING ends it");
    {
        g_stub_tick_ms = 100000;
        Rig r;
        simple_slot(2, SIG_MAP);
        prog_sig_gt_and_state_lt(slot(2).on_expr,  sizeof(slot(2).on_expr),  SIG_START_SW, 0, 2);
        prog_state_ge          (slot(2).off_expr, sizeof(slot(2).off_expr), 2);
        slot(2).max_on_ms = 10000;
        slot(2).rearm_ms  = 3000;
        slot(2).min_off_ms = 500;
        SignalBus bus;
        bus.set(SIG_MAP, 80.0f);
        bus.set(SIG_START_SW, 0.0f);
        bus.set(SIG_ENGINE_STATE, 0.0f);          // STOPPED

        r.tick(bus);
        CHECK(!r.pins.pins[2].high);              // born off, button not pressed

        // A TAP. The press is 200 ms; the crank must outlive it by ten seconds, because the gate holds
        // its state while neither condition is true. This is the whole difference between a button and
        // a key, and it is the deadband doing it — nothing here is counting the press.
        g_stub_tick_ms = 100600;                  // past min_off from birth
        bus.set(SIG_START_SW, 1.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        g_stub_tick_ms = 100800;
        bus.set(SIG_START_SW, 0.0f);              // let go after 200 ms
        r.tick(bus);
        CHECK(r.pins.pins[2].high);               // still cranking: THIS is the latch

        g_stub_tick_ms = 105000;                  // ~4.4 s in
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        g_stub_tick_ms = 110700;                  // past the 10 s limit
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);              // maximum-on released it

        // …and with the button long released, the re-arm expiring must NOT start another crank.
        g_stub_tick_ms = 114000;                  // past the 3 s lockout
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);

        // A HOLD. Same slot, button down and staying down: crank, rest, crank again.
        bus.set(SIG_START_SW, 1.0f);
        g_stub_tick_ms = 115000;
        r.tick(bus);
        CHECK(r.pins.pins[2].high);

        g_stub_tick_ms = 125100;                  // 10.1 s of cranking
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);              // tripped again

        g_stub_tick_ms = 127000;                  // 1.9 s into the 3 s rest
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);              // the motor is cooling; the button is ignored

        g_stub_tick_ms = 128200;                  // past the rest, still holding
        r.tick(bus);
        CHECK(r.pins.pins[2].high);               // and away it goes again

        // RUNNING ENDS IT, mid-crank, whatever the button says.
        g_stub_tick_ms = 130000;
        bus.set(SIG_ENGINE_STATE, 2.0f);          // caught
        r.tick(bus);
        CHECK(!r.pins.pins[2].high);

        // AND THE DIP THAT USED TO RE-ENGAGE IT. RUNNING carries full hysteresis, so a stumble holds
        // the state even as rpm collapses. Reading rpm here instead of the state made the on condition
        // true and the off condition false in the same frame — and step() takes the on branch first,
        // which is a starter meeting a spinning flywheel. The state cannot do that: one term, two
        // answers that are always opposites.
        g_stub_tick_ms = 131000;
        r.tick(bus);                              // still RUNNING, button still held
        CHECK(!r.pins.pins[2].high);

        // A REAL STOP re-opens it, on the next press.
        g_stub_tick_ms = 132000;
        bus.set(SIG_ENGINE_STATE, 0.0f);          // STOPPED
        r.tick(bus);
        CHECK(r.pins.pins[2].high);
    }

    SECTION("a rebuild happens for an Outputs write and for nothing else");
    {
        Rig r;
        simple_slot(2, SIG_MAP);
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(r.pins.pins[2].driven);

        // Move the output to row 4 WITHOUT telling the manager: nothing may move, because a write that
        // did not land in the Outputs region must not disturb an output.
        slot(4) = slot(2);
        slot(2).function = static_cast<uint8_t>(OutputFunction::NONE);
        r.tick(bus);
        CHECK(!r.pins.pins[4].driven);
        CHECK(r.pins.pins[2].driven);

        // …and with the Outputs shadow bit set, it rebuilds: the old pin released, the new one driven.
        // The mask is a BIT PER MODULE, and JAYECU_SHADOW_OUTPUTS is the module's index.
        g_comms.mark_shadow(1u << JAYECU_SHADOW_OUTPUTS);
        r.tick(bus);
        CHECK(r.pins.pins[4].driven);
        CHECK(!r.pins.pins[2].driven);
        std::memset(&slot(4), 0, sizeof(OutputConfig));
    }

    SECTION("the scheduler letting go of a coil row rebuilds, so the row's new function claims it");
    {
        Rig r;
        simple_slot(3, SIG_MAP);
        CHECK(r.arbiter.claim(3, PinOwner::IGNITION) != nullptr);   // still the scheduler's
        SignalBus bus; bus.set(SIG_MAP, 80.0f);
        r.tick(bus);
        CHECK(!r.pins.pins[3].driven);

        r.arbiter.release_owner(PinOwner::IGNITION);                 // the stopped reconfigure
        r.tick(bus);
        CHECK(!r.pins.pins[3].driven);                               // nothing told the manager yet
        ++g_firing_bind_generation;                                  // …which is what the HAL does
        r.tick(bus);
        CHECK(r.pins.pins[3].driven);
        CHECK(r.pins.pins[3].high);
        std::memset(&slot(3), 0, sizeof(OutputConfig));
    }

    return test_summary();
}

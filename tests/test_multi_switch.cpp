// Host test for the MULTI-POSITION SWITCH input (sensor type multi_switch): several buttons on one analog
// wire through a resistor ladder, decoded to WHICH position by voltage bands. Covers what the builder
// composes and refuses, and what decode_bands reports: settling (a press sweeps through other bands),
// crossing a gap vs being out of every band, the 0 V floor, the HAL's no-reading code, a lost 5 V
// reference, and the stuck check that exempts the rest position.
//   build: tests/CMakeLists.txt -> ctest -R multi_switch
#include "test_helpers.h"
#include "Pipeline/Pipeline.h"
#include "Pipeline/Stages.h"
#include "Platform/AcquireHal.h"
#include "Integration/PipelineBuilder.h"
#include "../generated/signal_ids.h"

using namespace pipe;

// A generic analog input (aux_1) — the tune picks its type, and multi_switch is one it may pick.
static SensorDescriptor aux_desc() {
    SensorDescriptor d{};
    d.id = "aux_1"; d.name = "Auxiliary Input 1";
    d.type = SENSOR_TYPE_NONE;
    d.primary_channel = SIG_AUX_1;
    d.interface_mask = IFACE_ANALOG_VOLTAGE | IFACE_CAN_DEVICE;
    return d;
}

// A four-position ladder, bands ascending by voltage (the cal axis must ascend):
//   CANCEL 400-700 -> 4,  RES 1000-1400 -> 3,  SET 2000-2400 -> 2,  REST 3000-3400 -> 0
enum : int { P_REST = 0, P_SET = 2, P_RES = 3, P_CANCEL = 4 };
static SensorConfig ladder_cfg() {
    SensorConfig c{};
    c.enabled = 1; c.type = SENSOR_TYPE_MULTI_SWITCH;
    c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;          // AV1 -> SIG_HW_AV1
    const uint16_t e[8] = { 400, 700, 1000, 1400, 2000, 2400, 3000, 3400 };
    const int16_t  v[8] = { P_CANCEL, P_CANCEL, P_RES, P_RES, P_SET, P_SET, P_REST, P_REST };
    c.cal_n = 8;
    for (int k = 0; k < 8; k++) { c.cal_raw[k] = e[k]; c.cal_val[k] = v[k]; }
    return c;
}

// One pipeline, one bus, one clock: feed a raw count at a time and read back what was published.
struct Rig {
    SensorDescriptor d = aux_desc();
    SensorConfig     c;
    InputCfgPool     pool{};
    Pipeline         p{};
    SignalBus        bus;
    uint32_t         t = 1000;
    Ctx              last{};
    explicit Rig(const SensorConfig& cfg) : c(cfg) { built = build_input(d, c, pool, p); }
    bool built = false;
    // Advance `dt` ms and run once with the pin reading `raw`.
    void at(uint32_t dt, uint32_t raw, bool supply_lost = false) {
        t += dt;
        bus.set_u32(SIG_HW_AV1, raw, true, t, 5);
        last = p.run(bus, t, static_cast<float>(dt), true, supply_lost);
    }
    bool  valid() const { return bus.valid(SIG_AUX_1); }
    float pos()   const { return bus.get(SIG_AUX_1, -1.0f); }
    bool  tripped(DiagSlot s) const { return (last.diag_tripped & (1u << s)) != 0; }
};

int main() {
    fprintf(stdout, "=== Multi-position switch ===\n");

    SECTION("builds acquire | decode_bands | publish — no window unless the user arms one");
    {
        Rig r(ladder_cfg());
        CHECK(r.built);
        CHECK(r.p.n == 3);
        // NO forced raw window. This type used to get one at a hard-coded 200 mV floor whatever
        // diag_enable said, which made its Detect Raw Low tick box the one control in the studio that
        // could not change anything — and forbade a band at 0 V, which is where most cruise stalks put
        // MAIN. With nothing armed there is no window stage at all.
        CHECK(r.c.diag_enable == 0);
        CHECK(r.p.stages[0].fn == acquire_analog);
        CHECK(r.p.stages[1].fn == decode_bands);
        CHECK(r.p.stages[2].fn == publish);
        CHECK(r.pool.bands.bands == 4);
        CHECK(r.pool.bands.edges == r.c.cal_raw);                // points INTO the live config
    }

    SECTION("Raw Min is the user's line, and the only one");
    {
        SensorConfig c = ladder_cfg();
        c.diag_enable = DIAG_RAW_MIN; c.diag_raw_min = 300;
        Rig hi(c);
        CHECK(hi.built && hi.p.stages[1].fn == cond_raw_window);
        CHECK(hi.pool.raw_win.en_min && hi.pool.raw_win.min == 300u);
        // ... and a LOW threshold stays low. Nothing silently raises it to a floor any more.
        c.diag_raw_min = 100;
        Rig lo(c);  CHECK(lo.built && lo.pool.raw_win.min == 100u);
    }

    SECTION("a band at 0 V builds — a grounding MAIN button is a position, not a fault");
    {
        SensorConfig c = ladder_cfg();
        c.cal_n = 2;
        c.cal_raw[0] = 0; c.cal_raw[1] = 40;        // MAIN: the stalk shorts the line to ground
        c.cal_val[0] = 1; c.cal_val[1] = 1;
        Rig r(c);
        CHECK(r.built);                              // the old floor refused this outright
        CHECK(r.pool.bands.bands == 1);
    }

    SECTION("calibrations that cannot be decoded safely build nothing");
    {
        SensorConfig c = ladder_cfg();
        c.cal_n = 7;                                   // half a band
        CHECK(multi_switch_bands(c) == 0);  { Rig r(c); CHECK(!r.built); }

        c = ladder_cfg(); c.cal_raw[2] = 700;          // RES starts where CANCEL ends: 700 is both
        CHECK(multi_switch_bands(c) == 0);  { Rig r(c); CHECK(!r.built); }

        c = ladder_cfg(); c.cal_raw[4] = 2500;         // SET low above its high
        CHECK(multi_switch_bands(c) == 0);

        // A LOW BAND IS NOT A BROKEN CALIBRATION any more. Both of these used to build nothing, on a
        // floor that no longer exists: a band near 0 V is how a grounding MAIN button is described, and
        // Raw Min is the user's own check rather than something that silently voids their calibration.
        c = ladder_cfg(); c.cal_raw[0] = 100;
        CHECK(multi_switch_bands(c) == 4);  { Rig r(c); CHECK(r.built); }

        c = ladder_cfg(); c.diag_enable = DIAG_RAW_MIN; c.diag_raw_min = 500;
        CHECK(multi_switch_bands(c) == 4);

        c = ladder_cfg(); c.interface = IFSEL_DIGITAL_FREQ;   // no voltage to band
        { Rig r(c); CHECK(!r.built); }

        CHECK(multi_switch_bands(ladder_cfg()) == 4);  // …and the good one is accepted
    }

    SECTION("nothing is reported until a band has settled");
    {
        Rig r(ladder_cfg());
        r.at(0, 3200);  CHECK(!r.valid());                       // first sample: not believed yet
        r.at(20, 3200); CHECK(!r.valid());
        r.at(20, 3200); CHECK(r.valid() && r.pos() == P_REST);   // 40 ms in one band
        CHECK(r.last.diag_tripped == 0);                         // not settling is not a fault
    }

    SECTION("a press that sweeps through another band never reports it");
    {
        Rig r(ladder_cfg());
        r.at(0, 3200); r.at(20, 3200); r.at(20, 3200);           // settled at rest
        bool saw_set = false;
        r.at(20, 2200); saw_set |= (r.valid() && r.pos() == P_SET);   // one sample of SET on the way
        CHECK(r.valid() && r.pos() == P_REST);                   // still rest: SET never settled
        for (int k = 0; k < 3; k++) { r.at(20, 1200); saw_set |= (r.valid() && r.pos() == P_SET); }
        CHECK(r.valid() && r.pos() == P_RES);                    // 40 ms of RES
        CHECK(!saw_set);
    }

    SECTION("crossing a gap holds the position; staying out of every band is a fault");
    {
        Rig r(ladder_cfg());
        r.at(0, 1200); r.at(20, 1200); r.at(20, 1200);           // RES
        r.at(20, 1700); CHECK(r.valid() && r.pos() == P_RES);    // 1700 is between RES and SET
        r.at(20, 1700); CHECK(r.valid() && r.pos() == P_RES);
        r.at(20, 1200); CHECK(r.valid() && r.pos() == P_RES);    // back in: never dropped

        for (int k = 0; k < 5; k++) r.at(20, 1700);              // 0..80 ms out: still a crossing
        CHECK(r.valid() && !r.tripped(DIAG_SLOT_NO_BAND));
        r.at(20, 1700);                                          // 100 ms: nowhere at all, not a gap
        // NO_BAND, not RAW_MAX. It used to claim a raw-HIGH fault, which is a lie whenever the pin is
        // sitting low — and it is the sensor, not its consumer, that knows the bands were not matched.
        CHECK(!r.valid() && r.tripped(DIAG_SLOT_NO_BAND));
        CHECK(!r.tripped(DIAG_SLOT_RAW_MAX));

        r.at(20, 3200); CHECK(!r.valid());                       // recovery settles from scratch
        r.at(20, 3200); r.at(20, 3200);
        CHECK(r.valid() && r.pos() == P_REST && r.last.diag_tripped == 0);
    }

    SECTION("0 V is a fault only if the USER says so — Raw Min armed");
    {
        SensorConfig c = ladder_cfg();
        c.diag_enable = DIAG_RAW_MIN; c.diag_raw_min = 200;
        Rig r(c);
        r.at(0, 3200); r.at(20, 3200); r.at(20, 3200);           // rest
        r.at(20, 0);
        CHECK(!r.valid() && r.tripped(DIAG_SLOT_RAW_MIN));
        r.at(20, 150);                                           // still under their threshold
        CHECK(!r.valid() && r.tripped(DIAG_SLOT_RAW_MIN));
        r.at(20, 3200); CHECK(!r.valid());                       // must settle again, not resume
        r.at(20, 3200); r.at(20, 3200); CHECK(r.valid() && r.pos() == P_REST);
    }

    SECTION("... and with Raw Min NOT armed, a band at 0 V is simply that position");
    {
        SensorConfig c = ladder_cfg();
        c.cal_raw[0] = 0; c.cal_raw[1] = 40;                     // MAIN grounds the line
        Rig r(c);
        CHECK(r.built);
        r.at(0, 0); r.at(20, 0); r.at(20, 0);
        CHECK(r.valid());                                        // a press, not a fault
        CHECK(!r.tripped(DIAG_SLOT_RAW_MIN) && !r.tripped(DIAG_SLOT_NO_BAND));
    }

    SECTION("the HAL's 0xFFFF no-reading is not a voltage");
    {
        Rig r(ladder_cfg());
        r.at(0, 3200); r.at(20, 3200); r.at(20, 3200);
        r.at(20, 0xFFFF);
        CHECK(!r.valid() && r.tripped(DIAG_SLOT_RAW_MAX));
    }

    SECTION("a lost 5 V reference invalidates at once and discards the settled position");
    {
        Rig r(ladder_cfg());
        r.at(0, 1200); r.at(20, 1200); r.at(20, 1200);           // RES held
        r.at(20, 1200, /*supply_lost=*/true);
        CHECK(!r.valid());
        CHECK(r.last.diag_tripped == 0);                         // the supply's code names it, not ours
        r.at(20, 1200); CHECK(!r.valid());                       // RES is not simply resumed
        r.at(20, 1200); r.at(20, 1200); CHECK(r.valid() && r.pos() == P_RES);
    }

    SECTION("stuck: a pressed position held too long is a fault; rest may be held forever");
    {
        SensorConfig c = ladder_cfg();
        c.diag_enable = DIAG_STUCK; c.diag_stuck_ms = 500;
        Rig r(c);
        CHECK(r.built && r.p.n == 3);                            // no cond_stuck: the decoder does it
        r.at(0, 3200); r.at(20, 3200); r.at(20, 3200);
        for (int k = 0; k < 150; k++) r.at(20, 3200);            // 3 s at rest
        CHECK(r.valid() && r.pos() == P_REST && !r.tripped(DIAG_SLOT_STUCK));

        r.at(20, 1200); r.at(20, 1200); r.at(20, 1200);          // RES settles at this sample
        for (int k = 0; k < 24; k++) r.at(20, 1200);             // 480 ms held
        CHECK(r.valid() && r.pos() == P_RES);
        r.at(20, 1200);                                          // 500 ms: jammed
        CHECK(!r.valid() && r.tripped(DIAG_SLOT_STUCK));
        r.at(20, 3200); r.at(20, 3200); r.at(20, 3200);          // released: rest settles, fault clears
        CHECK(r.valid() && r.pos() == P_REST && !r.tripped(DIAG_SLOT_STUCK));
    }

    SECTION("publish marks ANY sensor invalid while a 5 V reference is down");
    {
        SensorDescriptor d{};
        d.id = "clt"; d.type = SENSOR_TYPE_TEMPERATURE; d.primary_channel = SIG_CLT;
        d.interface_mask = IFACE_ANALOG_VOLTAGE;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        c.cal_n = 2; c.cal_raw[0] = 0; c.cal_raw[1] = 4095; c.cal_val[0] = 0; c.cal_val[1] = 1000;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        SignalBus bus;
        bus.set_u32(SIG_HW_AV1, 2000u, true, 1000, 5);
        p.run(bus, 1000, 200.0f);
        CHECK(bus.valid(SIG_CLT));
        p.run(bus, 1200, 200.0f, true, /*supply_lost=*/true);
        CHECK(!bus.valid(SIG_CLT));
    }

    return test_summary();
}

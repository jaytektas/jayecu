// Host unit test for the pipeline BUILDER — proves the existing sensor catalog +
// SensorConfig drive a composed Pipeline (the right stages, in order, wired to the
// config), reusing the proven Tier-2/Tier-3 data with no new encoding.
//   build: tests/CMakeLists.txt -> ctest -R pipeline_builder
#include "test_helpers.h"
#include <initializer_list>
#include "Pipeline/Pipeline.h"
#include "Pipeline/Stages.h"
#include "Platform/AcquireHal.h"
#include "Integration/PipelineBuilder.h"
#include "../firmware/Can/CanFrame.h"
#include "../generated/signal_ids.h"

using namespace pipe;

static CanFrame mk_frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
    CanFrame f{}; f.id = id; f.ext = false; f.dlc = (uint8_t)bytes.size();
    uint8_t i = 0; for (uint8_t b : bytes) { if (i < 8) f.data[i++] = b; }
    return f;
}

// A coolant-temp-style analog descriptor (type temperature -> val_scale 0.1, stored int10ths).
static SensorDescriptor temp_desc() {
    SensorDescriptor d{};
    d.id = "clt"; d.name = "CLT";
    d.type = SENSOR_TYPE_TEMPERATURE;
    d.group = SENSOR_GROUP_ENGINE;
    d.primary_channel = SIG_CLT;
    d.interface_mask = IFACE_ANALOG_VOLTAGE;
    d.locked_interface = 0;
    return d;
}

// A 2-point cal: 0 mV -> 0.0, 5000 mV -> 100.0  (stored 0..1000 * val_scale 0.1).
static SensorConfig analog_cfg() {
    SensorConfig c{};
    c.enabled = 1;
    c.interface = IFSEL_ANALOG_VOLTAGE;
    c.source = 0;
    c.cal_n = SENSOR_CAL_POINTS;   // all points live (resizable curve)
    // a 2-point line, tail points collapsed onto the last point
    c.cal_raw[0] = 0;    c.cal_raw[1] = 5000;
    c.cal_val[0] = 0;    c.cal_val[1] = 1000;
    for (int i = 2; i < SENSOR_CAL_POINTS; i++) { c.cal_raw[i] = 5000; c.cal_val[i] = 1000; }
    return c;
}

int main() {
    fprintf(stdout, "=== PipelineBuilder ===\n");

    SECTION("analog input -> acquire_analog | decode_curve | publish");
    {
        SensorDescriptor d = temp_desc();
        SensorConfig c = analog_cfg();
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));

        CHECK(p.signal == SIG_CLT);
        CHECK(p.n == 3);
        CHECK(p.stages[0].fn == acquire_analog);
        CHECK(p.stages[1].fn == decode_curve);
        CHECK(p.stages[2].fn == publish);
        // the curve cfg points INTO the live config (no copy of the arrays)
        CHECK(pool.curve.xs == c.cal_raw);
        CHECK(pool.curve.ys == c.cal_val);
        CHECK(pool.curve.n == SENSOR_CAL_POINTS);   // fixed-size cal curve (codegen)
        CHECK_NEAR(pool.curve.val_scale, 0.1f, 0.0001f);
        CHECK(pool.pub.ttl_ms == 600);          // 3000 / 5 Hz

        // run it: HardwareInput publishes the pin's raw counts as SIG_HW_AV1 (source 0); Acquire reads
        // them off the bus -> curve(0) -> 0.0, published valid.
        SignalBus bus;
        bus.set_u32(SIG_HW_AV1, 0u, true, 1000, 5);
        p.run(bus, 1000, 100.0f);
        CHECK(bus.valid(SIG_CLT));
        CHECK_NEAR(bus.get(SIG_CLT), 0.0f, 0.01f);
    }

    SECTION("raw-window + EMA + op-window appear only when their config is enabled");
    {
        SensorDescriptor d = temp_desc();
        SensorConfig c = analog_cfg();
        c.diag_enable    = DIAG_RAW_MAX | DIAG_OP_MAX;
        c.diag_raw_max = 4800;
        c.diag_op_max     = 1200;               // 120.0 (val_scale 0.1)
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));

        // acquire | raw_window | decode | op_window | publish  (no EMA stage any more)
        CHECK(p.n == 5);
        CHECK(p.stages[0].fn == acquire_analog);
        CHECK(p.stages[1].fn == cond_raw_window);
        CHECK(p.stages[2].fn == decode_curve);
        CHECK(p.stages[3].fn == cond_op_window);
        CHECK(p.stages[4].fn == publish);
        // only the MAX bound enabled on each window
        CHECK(pool.raw_win.max == 4800u && pool.raw_win.en_max && !pool.raw_win.en_min);
        CHECK(pool.op_win.max > 119.0f && pool.op_win.max < 121.0f);
        CHECK(pool.op_win.en_max && !pool.op_win.en_min);
    }

    SECTION("switch input -> acquire_switch | publish (no Decode; value is the state)");
    {
        SensorDescriptor d{};
        d.id = "oilsw"; d.type = SENSOR_TYPE_SWITCH; d.primary_channel = SIG_OIL_PRESSURE_SW;
        d.interface_mask = IFACE_DIGITAL; d.locked_interface = 0;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_DIGITAL; c.source = 0;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.n == 2);
        CHECK(p.stages[0].fn == acquire_switch);
        CHECK(p.stages[1].fn == publish);
    }

    SECTION("switch on an ANALOG pin -> threshold, not the cal curve");
    {
        // start_sw / ac_request / switch_1..4 all offer analog_voltage as well as switch, and this is
        // what that combination has to build: the analog acquire, then a DECODE TO 0/1 against the two
        // trip points — never decode_curve, which would interpolate a contact that has no in-between.
        SensorDescriptor d{};
        d.id = "startsw"; d.type = SENSOR_TYPE_SWITCH; d.primary_channel = SIG_START_SW;
        d.interface_mask = IFACE_ANALOG_VOLTAGE | IFACE_DIGITAL; d.locked_interface = 0;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 1;
        c.cal_n = 2; c.cal_raw[0] = 1000; c.cal_raw[1] = 2000;   // off below 1.00 V, on above 2.00 V
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.n == 3);
        CHECK(p.stages[0].fn == acquire_analog);
        CHECK(p.stages[1].fn == decode_switch_thresh);
        CHECK(p.stages[2].fn == publish);
        // ASCENDING AXIS, so [0] is the lower point: off is the low one, on the high one.
        CHECK(pool.thresh.on_raw == 2000u && pool.thresh.off_raw == 1000u);

        // …and the deadband between them HOLDS, which is the whole reason there are two.
        Ctx x{}; State st{};
        x.raw.u = 2400; x.raw_kind = RAW_U32; decode_switch_thresh(x, &pool.thresh, st);
        CHECK(x.value == 1.0f);
        x.raw.u = 1500;                       decode_switch_thresh(x, &pool.thresh, st);
        CHECK(x.value == 1.0f);               // inside the band: still on
        x.raw.u =  800;                       decode_switch_thresh(x, &pool.thresh, st);
        CHECK(x.value == 0.0f);
        x.raw.u = 1500;                       decode_switch_thresh(x, &pool.thresh, st);
        CHECK(x.value == 0.0f);               // inside the band: still off
    }

    SECTION("…and its stuck check is built, which the interface test used to skip");
    {
        SensorDescriptor d{};
        d.id = "startsw"; d.type = SENSOR_TYPE_SWITCH; d.primary_channel = SIG_START_SW;
        d.interface_mask = IFACE_ANALOG_VOLTAGE | IFACE_DIGITAL; d.locked_interface = 0;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 1;
        c.cal_n = 2; c.cal_raw[0] = 1000; c.cal_raw[1] = 2000;
        c.diag_enable = DIAG_STUCK; c.diag_stuck_ms = 5000;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.n == 4);
        CHECK(p.stages[2].fn == cond_stuck);
    }

    SECTION("on-board voltage -> acquire_onboard_voltage | publish (no source/curve)");
    {
        SensorDescriptor d{};
        d.id = "battery"; d.type = SENSOR_TYPE_VOLTAGE; d.primary_channel = SIG_BATTERY;
        d.interface_mask = IFACE_ON_BOARD; d.locked_interface = IFACE_ON_BOARD;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE;   // ignored: locked_interface wins -> on_board
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.n == 2);
        CHECK(p.stages[0].fn == acquire_onboard_voltage);
        CHECK(p.stages[1].fn == publish);
    }

    SECTION("on-board TEMPERATURE -> acquire_onboard_temp, NOT the barometric acquire");
    {
        // The bug this pins: the builder used to send every on-board sensor that was not a VOLTAGE
        // to acquire_onboard_baro, so ecu_temp (an on-board TEMPERATURE) read barometric pressure
        // and published it through a temperature curve. Measured on hardware as ecu_temp = 94.60,
        // which is kPa. It had never worked.
        SensorDescriptor d{};
        d.id = "ecu_temp"; d.type = SENSOR_TYPE_TEMPERATURE; d.primary_channel = SIG_ECU_TEMP;
        d.interface_mask = IFACE_ON_BOARD; d.locked_interface = IFACE_ON_BOARD;
        SensorConfig c{};
        c.enabled = 1;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.stages[0].fn == acquire_onboard_temp);
        CHECK(p.stages[0].fn != acquire_onboard_baro);
    }

    SECTION("on-board PRESSURE still routes to the barometric acquire");
    {
        SensorDescriptor d{};
        d.id = "baro"; d.type = SENSOR_TYPE_PRESSURE; d.primary_channel = SIG_BARO_KPA;
        d.interface_mask = IFACE_ON_BOARD; d.locked_interface = IFACE_ON_BOARD;
        SensorConfig c{};
        c.enabled = 1;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.stages[0].fn == acquire_onboard_baro);
    }

    SECTION("an on-board type with no acquire is REFUSED, not defaulted to pressure");
    {
        // The point of the switch. A sensor that cannot be built publishes nothing, which is
        // visible; a silent fallthrough publishes a confident wrong number, which is not.
        SensorDescriptor d{};
        d.id = "odd"; d.type = SENSOR_TYPE_FLOW; d.primary_channel = SIG_ECU_TEMP;
        d.interface_mask = IFACE_ON_BOARD; d.locked_interface = IFACE_ON_BOARD;
        SensorConfig c{};
        c.enabled = 1;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(!build_input(d, c, pool, p));
    }

    SECTION("frequency input -> acquire_freq | decode_curve | publish");
    {
        SensorDescriptor d{};
        d.id = "vss"; d.type = SENSOR_TYPE_FREQUENCY; d.primary_channel = SIG_VEHICLE_SPD;
        d.interface_mask = IFACE_DIGITAL_FREQ; d.locked_interface = 0;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_DIGITAL_FREQ; c.source = 3;
        c.cal_raw[0] = 0; c.cal_raw[1] = 1000; c.cal_val[0] = 0; c.cal_val[1] = 1000;
        for (int i = 2; i < SENSOR_CAL_POINTS; i++) { c.cal_raw[i] = 1000; c.cal_val[i] = 1000; }
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK(p.n == 3);
        CHECK(p.stages[0].fn == acquire_freq);
        CHECK(p.stages[1].fn == decode_curve);
        CHECK(p.stages[2].fn == publish);
        CHECK(pool.freq.source == 3);
    }

    SECTION("CAN-device input -> build returns false (needs the device library)");
    {
        SensorDescriptor d{};
        d.id = "lambda_1"; d.type = SENSOR_TYPE_LAMBDA; d.primary_channel = SIG_LAMBDA_1;
        d.interface_mask = IFACE_CAN_DEVICE | IFACE_ANALOG_VOLTAGE; d.locked_interface = 0;
        SensorConfig c{};
        c.enabled = 1; c.interface = IFSEL_CAN_DEVICE;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(!build_input(d, c, pool, p));     // analog/etc. builder doesn't build CAN
    }

    SECTION("CAN interface: build_can_input composes a field -> lambda_1 pipeline");
    {
        // What the sensor adds on top of the decode is the point of routing a CAN reading through it
        // at all: the operating window, the derivative check, its P-codes and its enable.
        GenericCanFieldValue fv{ 1.0f, 1000 };
        SensorDescriptor d{}; d.primary_channel = SIG_LAMBDA_1; d.type = 0;
        SensorConfig c{}; c.enabled = 1; c.type = 0;
        InputCfgPool pool{}; Pipeline p;
        CHECK(build_can_input(&fv, 300, d, c, pool, p));
        CHECK(p.stages[0].fn == acquire_can_field);
        CHECK(p.signal == SIG_LAMBDA_1);
        // The decode is an identity — the field already ran its own multiplier and offset — but it
        // still has to be there, because Publish writes `value` and only a Decode fills it.
        CHECK(p.stages[1].fn == decode_linear);
        CHECK(p.stages[2].fn == publish);
        CHECK(p.n == 3);

        // …and a null field builds nothing rather than a pipeline that reads a dangling pointer.
        Pipeline q;
        CHECK(!build_can_input(nullptr, 300, d, c, pool, q));
    }

    // ---- a generic input takes its type — and therefore its precision — from the TUNE ----
    // This is the whole point of `generic`: it is an input with no type yet, so the engineering
    // scale cannot be baked into its catalog row. A catalogued sensor is the opposite — its type
    // is settled, and whatever the tune stores must be ignored.

    SECTION("generic input typed as pressure decodes at pressure's scale, not generic's");
    {
        SensorDescriptor d{};
        d.id = "aux_1"; d.type = SENSOR_TYPE_NONE; d.primary_channel = SIG_AUX_1;
        d.interface_mask = IFACE_ANALOG_VOLTAGE; d.locked_interface = 0;
        SensorConfig c = analog_cfg();
        c.type = SENSOR_TYPE_PRESSURE;
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        // pressure is 0.1 (one decimal); untyped generic is 0.01. Reading the wrong one is a
        // silent factor-of-ten error in every calibrated value the input produces.
        CHECK_NEAR(pool.curve.val_scale, 0.1f, 0.0001f);
        CHECK(SENSOR_TYPE_NONE >= SENSOR_TYPE_COUNT);   // the sentinel can never index the table
    }

    SECTION("a catalogued sensor ignores the tune's type — the catalog's type wins");
    {
        SensorDescriptor d = temp_desc();          // type temperature -> 0.1
        SensorConfig c = analog_cfg();
        c.type = SENSOR_TYPE_LAMBDA;               // 0.01 — must NOT take effect
        InputCfgPool pool{}; Pipeline p{};
        CHECK(build_input(d, c, pool, p));
        CHECK_NEAR(pool.curve.val_scale, 0.1f, 0.0001f);
        CHECK(effective_type(d, c.type) == SENSOR_TYPE_TEMPERATURE);
    }

    SECTION("an input with no type builds NOTHING — unconfigured is not a reading");
    {
        // The whole point of refusing to make `generic` a type. There is no scale to decode with
        // and no units to publish in, so an untyped input stays silent. A fabricated scale would
        // have let it publish a confident wrong number instead, which is the failure this avoids.
        SensorDescriptor d{};
        d.id = "aux_1"; d.type = SENSOR_TYPE_NONE; d.primary_channel = SIG_AUX_1;
        d.interface_mask = IFACE_ANALOG_VOLTAGE; d.locked_interface = 0;
        SensorConfig c = analog_cfg();
        c.type = SENSOR_TYPE_NONE;                 // no type chosen in the tune
        InputCfgPool pool{}; Pipeline p{};
        CHECK(!build_input(d, c, pool, p));
        CHECK(p.n == 0);                           // not a single stage, so nothing to publish
    }

    SECTION("a generic may only take a type a generic input can actually be built as");
    {
        SensorDescriptor d{};
        d.type = SENSOR_TYPE_NONE;
        // switch/frequency/composition are read by a different acquire, so they are not on offer;
        // asking for one falls back to untyped rather than building a pipeline that cannot work.
        CHECK(effective_type(d, SENSOR_TYPE_SWITCH)      == SENSOR_TYPE_NONE);
        CHECK(effective_type(d, SENSOR_TYPE_FREQUENCY)   == SENSOR_TYPE_NONE);
        CHECK(effective_type(d, SENSOR_TYPE_COMPOSITION) == SENSOR_TYPE_NONE);
        CHECK(effective_type(d, SENSOR_TYPE_PRESSURE)    == SENSOR_TYPE_PRESSURE);
        CHECK(effective_type(d, 200)                     == SENSOR_TYPE_NONE);   // out of range
    }

    return test_summary();
}

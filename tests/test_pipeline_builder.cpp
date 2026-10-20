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
        CHECK(pool.pub.ttl_ms == 3000u / SENSOR_TYPE_CATALOG[SENSOR_TYPE_TEMPERATURE].update_hz);   // three periods

        // run it: HardwareInput publishes the pin's raw counts as SIG_HW_AV1 (source 0); Acquire reads
        // them off the bus -> curve(0) -> 0.0, published valid.
        SignalBus bus;
        bus.set_u32(SIG_HW_AV1, 0u, true, 1000, 5);
        p.run(bus, 1000, 100.0f);
        CHECK(bus.valid(SIG_CLT));
        CHECK_NEAR(bus.get(SIG_CLT), 0.0f, 0.01f);
    }

    SECTION("a thermistor: volts -> ohms -> degrees from three points of the sensor's data");
    {
        // GM coolant, rusEFI's setGmCltSensor: -40 C 100 kohm, 40 C 1459 ohm, 130 C 70 ohm.
        auto gm = [](uint16_t pullup) {
            ThermistorConfig t{};
            t.mode = 1; t.pullup = pullup;
            t.temp_1 = -400; t.res_1 = 100000.0f;
            t.temp_2 =  400; t.res_2 = 1459.0f;
            t.temp_3 = 1300; t.res_3 = 70.0f;
            return t;
        };
        // The count a sensor of resistance r reads through pull-up rp (and, on an AV pin, the board's
        // pull-down in parallel) — worked out here in double, independently of the decode.
        auto count_for = [](double r, double rp, bool av) {
            const double node = av ? 1.0 / (1.0 / r + 1.0 / double(BOARD_AV_PULLDOWN_OHM)) : r;
            return uint32_t(node / (node + rp) * double(BOARD_ADC_FULL_SCALE) + 0.5);
        };
        auto run = [](const ThermistorConfig& t, int8_t src, uint32_t counts, bool& valid) {
            SensorDescriptor d = temp_desc();
            SensorConfig c = analog_cfg(); c.source = src;
            InputCfgPool pool{}; Pipeline p{};
            const bool built = build_input(d, c, pool, p, &t);
            if (!built) { valid = false; return -999.0f; }
            SignalBus bus;
            bus.set_u32(HW_POOL_SIG[uint8_t(src)], counts, true, 1000, 5);
            p.run(bus, 1000, 100.0f);
            valid = bus.valid(SIG_CLT);
            return bus.get(SIG_CLT);
        };
        const int8_t AT1 = int8_t(BOARD_ANALOG_T_BASE), AV1 = 0;
        bool v = false;

        {   // the pipeline is the thermistor decode, not the curve
            SensorDescriptor d = temp_desc(); SensorConfig c = analog_cfg(); c.source = AT1;
            const ThermistorConfig t = gm(2700);
            InputCfgPool pool{}; Pipeline p{};
            CHECK(build_input(d, c, pool, p, &t));
            CHECK(p.stages[1].fn == decode_thermistor);
            CHECK(pool.therm.on_av == 0);
        }
        // AT pin, the board's 2.7 k pull-up: at the sensor's own 40 C point it reads 40 C.
        CHECK_NEAR(run(gm(2700), AT1, count_for(1459.0, 2700.0, false), v), 40.0f, 0.2f);
        CHECK(v);
        CHECK_NEAR(run(gm(2700), AT1, count_for(100000.0, 2700.0, false), v), -40.0f, 0.5f);
        CHECK_NEAR(run(gm(2700), AT1, count_for(70.0, 2700.0, false), v), 130.0f, 0.5f);
        // AV pin with a 2.7 k pull-up fitted in the loom: the board's 470 k to ground sits in parallel.
        // At -40 C that is 100 k against 470 k — left in, it reads several degrees warm.
        CHECK_NEAR(run(gm(2700), AV1, count_for(100000.0, 2700.0, true), v), -40.0f, 0.5f);
        CHECK(v);
        // The rails are no reading: open circuit and short both publish nothing.
        run(gm(2700), AT1, BOARD_ADC_FULL_SCALE, v); CHECK(!v);
        run(gm(2700), AT1, 0u, v);                   CHECK(!v);
        // Data that is not a thermistor, or no pull-up, builds nothing (the config DTC's job to say).
        { ThermistorConfig t = gm(2700); t.temp_2 = -500; run(t, AT1, 2000u, v); CHECK(!v); }
        { ThermistorConfig t = gm(2700); t.res_3 = 2000.0f; run(t, AT1, 2000u, v); CHECK(!v); }
        { ThermistorConfig t = gm(0);                       run(t, AT1, 2000u, v); CHECK(!v); }
        // Mode Voltage Curve keeps the curve, thermistor row or not.
        {
            SensorDescriptor d = temp_desc(); SensorConfig c = analog_cfg(); c.source = AT1;
            ThermistorConfig t = gm(2700); t.mode = 0;
            InputCfgPool pool{}; Pipeline p{};
            CHECK(build_input(d, c, pool, p, &t));
            CHECK(p.stages[1].fn == decode_curve);
        }
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

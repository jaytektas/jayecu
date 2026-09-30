// A setting that applies only at an ENGINE STOP is locked while the engine turns.
//
// The firmware copies a shadowed module (Trigger, Engine) into its working copy only at the next engine
// stop, so an edit made while it turns sits in RAM doing nothing — a trigger stream switched off on a
// running bench engine kept its 2008 rpm. The studio greys those controls out while it is connected and
// the engine is turning. What can silently break is the data under that: the schema's apply rule has to
// reach the meta, MetaModel has to find it for any binding into the module (a struct-array element
// included), and the lock has to need BOTH a live link and a turning engine — never a stale RPM.
//
//   cmake --build build --target engine_stop_lock_test && ./build/engine_stop_lock_test
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++failures;
}

// One telemetry frame with `rpm` set (everything else zero).
static std::vector<uint8_t> frameWithRpm(const MetaModel& m, double rpm) {
    std::vector<uint8_t> f(static_cast<size_t>(m.telemetrySize()), 0);
    const auto it = m.telemetry().find("rpm");
    if (it == m.telemetry().end()) return f;
    const MetaModel::TelemField& t = it->second;
    const double raw = rpm / (t.scale != 0.0 ? t.scale : 1.0);
    if (t.datatype == "F32") { const float v = static_cast<float>(rpm); std::memcpy(&f[t.offset], &v, 4); }
    else if (t.size == 2)    { const uint16_t v = static_cast<uint16_t>(std::lround(raw)); std::memcpy(&f[t.offset], &v, 2); }
    else if (t.size == 4)    { const uint32_t v = static_cast<uint32_t>(std::lround(raw)); std::memcpy(&f[t.offset], &v, 4); }
    return f;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }

    std::printf("=== the rule reaches the studio ===\n");
    check(m.appliesAt("trigger.streams[2].enabled") == "engine_stop", "a trigger stream field applies at an engine stop",
          m.appliesAt("trigger.streams[2].enabled"));
    check(m.appliesAt("engine.cylinder_count") == "engine_stop", "the cylinder count applies at an engine stop");
    check(m.appliesAt("[#engine.cylinder_count]") == "engine_stop", "…in the builder's [#path] form too");
    check(m.appliesAt("trigger.trigger_offset_btdc").empty(),
          "Trigger Offset BTDC applies at once (set with a timing light on a running engine)");
    check(m.appliesAt("trigger.min_full_sync_rpm_x10").empty(), "…and so does the full-sync RPM band");
    check(m.appliesAt("outputs.output[3].function") == "engine_stop",
          "an output's Function (coil / injector / generic) applies at an engine stop");
    check(m.appliesAt("outputs.output[3].pwm_freq_hz").empty(), "…while a generic output's settings apply at once");
    check(m.appliesAt("fuel_calculator.ve_table").empty(), "the VE table applies at once");
    check(m.appliesAt("fuel_calculator.map_predict_enabled").empty(), "an ordinary switch applies at once");

    Cache& c = Cache::instance();
    c.setMeta(&m);
    const std::string STREAM = "trigger.streams[0].enabled";

    std::printf("\n=== locked only when connected AND turning ===\n");
    c.setLinkOpen(false);
    c.ingestTelemetry(frameWithRpm(m, 2000.0));
    check(!c.lockedWhileRunning(STREAM), "disconnected: a last-received 2000 rpm locks nothing");
    c.setLinkOpen(true);
    check(c.lockedWhileRunning(STREAM), "connected at 2000 rpm: the trigger stream is locked");
    check(!c.lockedWhileRunning("fuel_calculator.map_predict_enabled"), "…and an ordinary setting is not");
    c.ingestTelemetry(frameWithRpm(m, 0.0));
    check(!c.lockedWhileRunning(STREAM), "connected, stopped: unlocked");

    std::printf("\n%s\n", failures ? (std::to_string(failures) + " FAILED").c_str() : "ALL PASSED");
    return failures ? 1 : 0;
}

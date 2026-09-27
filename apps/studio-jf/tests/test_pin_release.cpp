// CLEARING A PIN MUST RETURN IT TO THE POOL. claimedPoolPins() decides which pins a picker may offer;
// if a released pin keeps showing as held, the tuner cannot re-use it anywhere and the only way out is
// to guess. Two ways a pin is released, and both must work: the field is set to the unassigned sentinel
// (-1), or the element holding it is disabled (a disabled sensor's pipeline is not built at all).

#include "model/MetaModel.h"
#include "model/Cache.h"
#include "surface/widgets/EnumPickerWidget.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

static int fails = 0;
#define CHECK(x) do { const bool _ok = (x); std::printf("[pool] %-64s %s\n", #x, _ok ? "PASS" : "FAIL"); \
                      if (!_ok) ++fails; } while (0)

static const char *kMeta = R"({
  "meta": { "layout_hash": "pooltest", "config_size": 64 },
  "config": { "sensors": {
     "sensor": { "type": "struct_array", "base_offset": 0, "count": 2, "stride": 4,
        "label": "Sensor",
        "element_ids": ["tps", "clt"],
        "element_labels": ["Throttle Position", "Coolant Temperature"],
        "fields": {
          "enabled":   {"datatype":"U08","rel_offset":0,"size":1,"scale":1,"min":0,"max":1,"default":0},
          "interface": {"datatype":"U08","rel_offset":2,"size":1,"scale":1,"min":0,"max":9,"default":0},
          "source":    {"datatype":"S08","rel_offset":3,"size":1,"scale":1,"min":-1,"max":127,
                        "default":-1,
                        "picker":{"by":"interface","sets":[
                           {"ifaces":[0,1],"options":[{"value":0,"label":"AV1"},{"value":1,"label":"AV2"}]}
                        ]}}
        } }
  } },
  "telemetry": {}
})";

static bool heldAV1()
{
    // Asked from CLT's point of view — its own field is excluded from its own answer, so anything
    // reported here is genuinely held by somebody else.
    const auto claimed = EnumPickerWidget::claimedPoolPins("sensors.sensor[clt].source");
    return claimed.find("AV1") != claimed.end();
}

int main()
{
    const std::string metaPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                               + "/jf_pool_test.meta";
    {
        const std::string body(kMeta);
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : body) {
            crc ^= b;
            for (int i = 0; i < 8; ++i) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        crc ^= 0xFFFFFFFFu;
        std::ofstream f(metaPath, std::ios::binary);
        f << body;
        for (int i = 0; i < 4; ++i) f.put(static_cast<char>((crc >> (8 * i)) & 0xFF));
    }
    MetaModel meta;
    if (!meta.loadFile(metaPath)) { std::printf("[pool] meta failed to load\n"); return 2; }

    Cache &c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(std::vector<uint8_t>(64, 0));

    // Nothing assigned yet: the pool is free.
    c.setConfigValue("sensors.sensor[tps].source", -1);
    c.setConfigValue("sensors.sensor[clt].source", -1);
    CHECK(!heldAV1());

    // Throttle takes AV1.
    c.setConfigValue("sensors.sensor[tps].enabled", 1);
    c.setConfigValue("sensors.sensor[tps].interface", 0);
    c.setConfigValue("sensors.sensor[tps].source", 0);
    CHECK(heldAV1());
    CHECK(EnumPickerWidget::claimedPoolPins("sensors.sensor[clt].source")
              .at("AV1").path == "sensors.sensor[tps].source");

    // CLEARED (the wiring widget's Clear writes the -1 sentinel) -> back in the pool.
    c.setConfigValue("sensors.sensor[tps].source", -1);
    CHECK(!heldAV1());

    // Re-taken, then released the other way: disabling the holder frees its pin too.
    c.setConfigValue("sensors.sensor[tps].source", 0);
    CHECK(heldAV1());
    c.setConfigValue("sensors.sensor[tps].enabled", 0);
    CHECK(!heldAV1());

    // Re-enabled with the pin still stored: it claims again.
    c.setConfigValue("sensors.sensor[tps].enabled", 1);
    CHECK(heldAV1());

    // A pin CLT itself holds is never reported to CLT — it is the field being changed.
    c.setConfigValue("sensors.sensor[tps].source", -1);
    c.setConfigValue("sensors.sensor[clt].enabled", 1);
    c.setConfigValue("sensors.sensor[clt].source", 0);
    CHECK(!heldAV1());

    std::printf("[pool] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}

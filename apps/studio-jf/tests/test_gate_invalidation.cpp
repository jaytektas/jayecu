// A PICKER GATED BY A SIBLING goes stale when that sibling moves, and the stale value is still IN RANGE
// — which is why it was silent. A sensor's `source` stores an index into the pin list its `interface`
// selects: 0 is AV1 under Analog Voltage and DIG1 under Frequency. Change the interface and the same
// byte addresses a different physical pin, with no write and no error. Nothing can catch that by
// validating the number; the test is whether the APPLICABLE SET CHANGED.
//
// Cache::setConfigValue restores the field's own schema default when it does — per element, since the
// schema gives each sensor its own (battery defaults to a pin, everything else to -1 = unassigned).

#include "model/MetaModel.h"
#include "model/Cache.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(x) do { const bool _ok = (x); std::printf("[gate] %-64s %s\n", #x, _ok ? "PASS" : "FAIL"); \
                      if (!_ok) ++fails; } while (0)

// Two sensors sharing one struct: `interface` (the gate) and `source` (gated). The analog set applies to
// interfaces 0 and 1, the digital set to 3 — and BOTH offer value 0, for different hardware, which is the
// whole point. Element 0 defaults to pin 11, element 1 to -1, so the per-element default is exercised.
static const char *kMeta = R"({
  "meta": { "layout_hash": "gatetest", "config_size": 64 },
  "config": { "sensors": {
     "sensor": { "type": "struct_array", "base_offset": 0, "count": 2, "stride": 4,
        "element_ids": ["battery", "clt"],
        "fields": {
          "enabled":   {"datatype":"U08","rel_offset":0,"size":1,"scale":1,"min":0,"max":1,"default":[1,0]},
          "interface": {"datatype":"U08","rel_offset":2,"size":1,"scale":1,"min":0,"max":9,"default":0},
          "source":    {"datatype":"S08","rel_offset":3,"size":1,"scale":1,"min":-1,"max":127,
                        "default":[11,-1],
                        "picker":{"by":"interface","sets":[
                           {"ifaces":[0,1],"options":[{"value":0,"label":"AV1"},{"value":1,"label":"AV2"}]},
                           {"ifaces":[3],  "options":[{"value":0,"label":"DIG1"},{"value":1,"label":"DIG2"}]}
                        ]}}
        } }
  } },
  "telemetry": {}
})";

int main()
{
    const std::string metaPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                               + "/jf_gate_test.meta";
    {   // .meta carries a zlib CRC32 footer; the loader rejects one without it
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
    if (!meta.loadFile(metaPath)) { std::printf("[gate] meta failed to load\n"); return 2; }

    // The defaults reached the model, per element.
    const auto ai = meta.configArrays().find("sensors.sensor");
    if (ai == meta.configArrays().end()) { std::printf("[gate] array missing\n"); return 2; }
    const MetaModel::ArrayField *src = nullptr;
    for (const auto &f : ai->second.fields) if (f.name == "source") src = &f;
    if (!src) { std::printf("[gate] source field missing\n"); return 2; }
    CHECK(src->hasDefault && src->defaults.size() == 2);
    CHECK(MetaModel::fieldDefault(*src, 0, 999) == 11);      // battery: its own pin
    CHECK(MetaModel::fieldDefault(*src, 1, 999) == -1);      // clt: unassigned
    CHECK(src->pickerBy == "interface" && src->pickerSets.size() == 2);

    Cache &c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(std::vector<uint8_t>(64, 0));

    // ---- the gate moves to a DIFFERENT set: the index is stale even though it is still in range ----
    c.setConfigValue("sensors.sensor[clt].interface", 0);
    c.setConfigValue("sensors.sensor[clt].source", 1);              // AV2, under the analog set
    CHECK(c.configValue("sensors.sensor[clt].source") == 1);
    c.setConfigValue("sensors.sensor[clt].interface", 3);           // -> Frequency: a different list
    CHECK(c.configValue("sensors.sensor[clt].source") == -1);       // ...so the pin is released

    // ---- and the case that makes range-checking useless: 0 is valid in BOTH sets ----
    c.setConfigValue("sensors.sensor[clt].interface", 0);
    c.setConfigValue("sensors.sensor[clt].source", 0);              // AV1
    c.setConfigValue("sensors.sensor[clt].interface", 3);           // 0 would now mean DIG1
    CHECK(c.configValue("sensors.sensor[clt].source") == -1);       // not silently re-pointed

    // ---- a move WITHIN one set changes nothing: 0 and 1 select the same list ----
    c.setConfigValue("sensors.sensor[clt].interface", 0);
    c.setConfigValue("sensors.sensor[clt].source", 1);
    c.setConfigValue("sensors.sensor[clt].interface", 1);           // same analog set
    CHECK(c.configValue("sensors.sensor[clt].source") == 1);        // still AV2

    // ---- per-element default: battery restores to ITS pin, not to -1 ----
    c.setConfigValue("sensors.sensor[battery].interface", 0);
    c.setConfigValue("sensors.sensor[battery].source", 1);
    c.setConfigValue("sensors.sensor[battery].interface", 3);
    CHECK(c.configValue("sensors.sensor[battery].source") == 11);

    // ---- writing the gate to the value it already holds must not disturb anything ----
    c.setConfigValue("sensors.sensor[clt].interface", 0);
    c.setConfigValue("sensors.sensor[clt].source", 1);
    c.setConfigValue("sensors.sensor[clt].interface", 0);           // no change
    CHECK(c.configValue("sensors.sensor[clt].source") == 1);

    // ---- a sibling that is NOT the gate must never trigger it ----
    c.setConfigValue("sensors.sensor[clt].enabled", 1);
    CHECK(c.configValue("sensors.sensor[clt].source") == 1);

    std::printf("[gate] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}

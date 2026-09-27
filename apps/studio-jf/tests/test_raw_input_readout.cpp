// THE SENSOR PAGE'S RAW INPUT READOUT. Its binding is authored "[$~]" — the element's raw input channel
// — which resolves through the element context to a SIGIL, "[$hw_av1]". Everything keyed by channel NAME
// misses that form, and bindingUnit() was still asking with the brackets on: Cache::unit("[$hw_av1]")
// found nothing, so the readout believed its channel had no units. It then printed a 0-5 V pin with no
// unit and no decimals — "4" for 4.110 V — because precision comes FROM the unit.

#include "model/MetaModel.h"
#include "model/Cache.h"
#include "model/UnitManager.h"
#include "model/MathEvaluator.h"
#include "surface/CanvasWidget.h"
#include "surface/PanelModel.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

static int fails = 0;
#define CHECK(x) do { const bool _ok = (x); std::printf("[raw] %-66s %s\n", #x, _ok ? "PASS" : "FAIL"); \
                      if (!_ok) ++fails; } while (0)

// hardware.adc + hardware.av are what register the AnalogRaw quantity (counts / mV / V) and therefore
// the precision volts are read at. hw_av1 is declared in ADC counts, as the firmware publishes it.
static const char *kMeta = R"({
  "meta": { "layout_hash": "rawtest", "config_size": 4096 },
  "hardware": { "adc": {"full_scale": 4095, "vref_mv": 3300}, "av": {"fullscale_mv": 5000} },
  "hw_pool_signals": { "analog_voltage": ["hw_av1", "hw_av2"] },
  "enums": { "sensor_interface": [ {"value":0,"id":"analog_voltage","label":"Analogue Voltage"},
                                   {"value":1,"id":"engine_sync_voltage","label":"Engine Sync Voltage"} ] },
  "config": { "sensors": {
     "sensor": { "type": "struct_array", "base_offset": 0, "count": 1, "stride": 4,
        "element_ids": ["tps"],
        "fields": {
          "enabled":   {"datatype":"U08","rel_offset":0,"size":1,"scale":1,"min":0,"max":1,"default":0},
          "interface": {"datatype":"U08","rel_offset":2,"size":1,"scale":1,"min":0,"max":9,"default":0},
          "source":    {"datatype":"S08","rel_offset":3,"size":1,"scale":1,"min":-1,"max":127,"default":-1}
        } }
  } },
  "telemetry": { "hw_av1": {"offset":0,"datatype":"U16","scale":1.0,"units":"ADC","digits":0} }
})";

int main()
{
    const std::string metaPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                               + "/jf_raw_test.meta";
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
    if (!meta.loadFile(metaPath)) { std::printf("[raw] meta failed to load\n"); return 2; }

    Cache &c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(std::vector<uint8_t>(4096, 0));
    c.setConfigValue("sensors.sensor[tps].interface", 0);   // analogue voltage
    c.setConfigValue("sensors.sensor[tps].source", 0);      // ...on AV1

    // The resolver main.cpp installs: element key -> the raw channel its input publishes.
    MathEvaluator::instance().setRawSignalResolver([](const std::string &key) -> std::string {
        const MetaModel *m = Cache::instance().meta();
        if (!m) return {};
        const std::string base = "sensors.sensor[" + key + "]";
        const int iface = static_cast<int>(Cache::instance().configValue(base + ".interface"));
        const int src   = static_cast<int>(Cache::instance().configValue(base + ".source"));
        if (src < 0) return {};
        const auto &ifaces = m->enumIds("sensor_interface");
        if (iface < 0 || iface >= static_cast<int>(ifaces.size())) return {};
        const std::vector<std::string> *pool = m->hwPoolSignals(ifaces[static_cast<size_t>(iface)]);
        if (!pool || src >= static_cast<int>(pool->size())) return {};
        return (*pool)[static_cast<size_t>(src)];
    });

    // The AnalogRaw quantity exists and states its precision: volts to three, counts and mV to none.
    UnitManager &um = UnitManager::instance();
    CHECK(um.findQuantityForUnit("ADC") == "AnalogRaw");
    CHECK(um.unitFormat("ADC_V") == "%.3f");
    CHECK(um.unitFormat("ADC") == "%.0f");

    PanelElement el;                       // exactly what sensor_detail.py authors: no format at all
    el.type = "value";
    el.props["signalName"] = "[$~]";

    MathEvaluator::ElementScope scope("tps");
    CHECK(MathEvaluator::resolveTemplate("[$~]", "tps") == "[$hw_av1]");   // a SIGIL, not a bare name
    CHECK(CanvasWidget::displayUnitOf(el) == "ADC_V");                     // ...which must still find units
    CHECK(CanvasWidget::autoFormat(el) == "%.3f");                         // ...and the unit's precision
    CHECK(CanvasWidget::fmtVal(el, c) == "0.000 V");                       // not "0", and not "0.0 V"

    // ---- AND WHEN THE INPUT IS UNASSIGNED. source = -1, so "$~" has no channel to name and the token
    // is deliberately left alone. There is then no channel and no unit, and the readout must not invent
    // either -- least of all a unit it cannot have converted anything into.
    c.setConfigValue("sensors.sensor[tps].source", -1);
    std::printf("[raw]   unassigned: $~ -> '%s'  unit '%s'  fmt '%s'  value '%s'\n",
                MathEvaluator::resolveTemplate("[$~]", "tps").c_str(),
                CanvasWidget::displayUnitOf(el).c_str(),
                CanvasWidget::autoFormat(el).c_str(),
                CanvasWidget::fmtVal(el, c).c_str());

    std::printf("[raw] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}

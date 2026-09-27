// Do bit-group paths actually read and write, without disturbing their neighbours in the byte?
#include "model/Cache.h"
#include "model/MetaModel.h"
#include <cstdio>
static int fails = 0;
static void ck(bool ok, const char* what, double got = 0) {
    std::printf("  %-52s %s%s\n", what, ok ? "PASS" : "FAIL",
                ok ? "" : ("  (got " + std::to_string(got) + ")").c_str());
    if (!ok) ++fails;
}
int main(int argc, char** argv) {
    MetaModel m;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && m.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[bits] (no meta found — skipped)"); return 0; }
    if (!m.bitGroupOf("sensors.sensor[clt].diag_enable.raw_min")) {
        std::puts("[bits] (meta has no bit groups — skipped)"); return 0;   // pre-regen meta
    }
    Cache& C = Cache::instance(); C.setMeta(&m);
    C.setConfigImage(std::vector<uint8_t>(size_t(m.configSize()), 0));
    const std::string base = "sensors.sensor[clt].";
    const std::string en = base + "diag_enable", sev = base + "diag_severity";
    C.setConfigValue(en + ".raw_min", 1);
    ck(C.configValue(en + ".raw_min") == 1, "set one flag reads back", C.configValue(en + ".raw_min"));
    ck(C.configValue(en + ".raw_max") == 0, "its neighbour is untouched", C.configValue(en + ".raw_max"));
    C.setConfigValue(en + ".stuck", 1);
    ck(C.configValue(en + ".raw_min") == 1, "first flag survives the second write", C.configValue(en + ".raw_min"));
    ck(C.configValue(en) == (1 | (1 << 4)), "the byte holds both bits", C.configValue(en));
    C.setConfigValue(sev + ".op_min", 3);                       // 2-bit group at [4:5]
    ck(C.configValue(sev + ".op_min") == 3, "a 2-bit level reads back", C.configValue(sev + ".op_min"));
    ck(C.configValue(sev + ".raw_min") == 0, "…without disturbing the level beside it", C.configValue(sev + ".raw_min"));
    ck(C.configValue(sev) == (3 << 4), "packed into the right bits", C.configValue(sev));
    // The WIDGET path. A control's click resolves its binding through Cache::isConfig (CanvasWidget::
    // writableConfigPath) before writing — and that gate did not know bit groups, so a checkbox bound to
    // one silently did nothing: no error, no write, the tick just sprang back.
    ck(C.isConfig(en + ".raw_min"), "a bit group is a writable config path");
    ck(C.isConfig(sev + ".op_min"), "…so is a multi-bit level");
    ck(C.configMin(en + ".raw_min") == 0 && C.configMax(en + ".raw_min") == 1,
       "a 1-bit group's range is 0..1", C.configMax(en + ".raw_min"));
    ck(C.configMin(sev + ".op_min") == 0 && C.configMax(sev + ".op_min") == 3,
       "a 2-bit group's range is 0..3", C.configMax(sev + ".op_min"));
    ck(C.digits(en + ".raw_min") == 0, "…and shows no decimals", C.digits(en + ".raw_min"));

    std::printf("\n%s\n", fails ? "FAILURES" : "bit groups read/write correctly");
    return fails ? 1 : 0;
}

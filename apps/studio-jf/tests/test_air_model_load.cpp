// A new air model offers its load axis (AirModelLoad): which maps are off target, and what re-pointing them
// does to their breakpoints. Against the real meta and its default tune (speed-density, Fuel Load rows).
//
//   cmake --build build --target air_model_load_test && ./build/air_model_load_test
//
#include "model/AirModelLoad.h"
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    ck(airload::offTarget(c, 0).empty(), "speed-density: both maps already on Fuel Load, nothing to offer");
    const auto blend = airload::offTarget(c, 3);
    ck(blend.size() == 2, "Blend: both maps are off its load (Charge Load)", std::to_string(blend.size()));

    const auto& ax = m.configTables().at("fuel_calculator.target_lambda_table").axes[1];
    const int n = static_cast<int>(std::lround(c.configValue(ax.nScalar)));
    auto before = c.axisValues(ax.array); before.resize(size_t(n));
    airload::apply(c, blend, "charge_load");
    auto after = c.axisValues(ax.array); after.resize(size_t(n));
    ck(std::lround(c.configValue(ax.srcScalar)) == m.signalMap().at("charge_load"), "Target Lambda now reads Charge Load");
    bool rows = n > 1;
    for (int i = 0; i < n; ++i) rows = rows && std::fabs(after[size_t(i)] - before[size_t(i)] * 100.0 / 101.3) < 0.2;
    ck(rows, "its kPa rows became the % of a full charge they hold (boost rows stay boost rows)",
       std::to_string(before.back()) + " kPa -> " + std::to_string(after.back()) + " %");
    ck(airload::offTarget(c, 3).empty(), "…after which there is nothing left to offer");

    airload::apply(c, airload::offTarget(c, 1), "tps");
    auto thr = c.axisValues(ax.array); thr.resize(size_t(n));
    ck(std::fabs(thr.front()) < 0.01 && std::fabs(thr.back() - 100.0) < 0.01, "Alpha-N: throttle rows spread 0-100 %");
    return fails ? 1 : 0;
}

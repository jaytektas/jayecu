#include "AirModelLoad.h"

#include "Cache.h"
#include "MetaModel.h"

#include <algorithm>
#include <cmath>

namespace airload {

namespace {

constexpr double kStdBaroKpa = 101.3;   // a full charge's pressure, for converting kPa rows to % rows

const MetaModel::Axis* loadAxis(const MetaModel& m, const std::string& table) {
    const auto it = m.configTables().find(table);
    if (it == m.configTables().end() || it->second.axes.size() < 2) return nullptr;
    return &it->second.axes[1];                            // y = the load axis on both maps
}

std::string signalName(const MetaModel& m, int id) {
    for (const auto& [name, sid] : m.signalMap())
        if (sid == id) return name;
    return {};
}

enum class Kind { Pressure, ChargePct, Throttle, Other };
Kind kindOf(const std::string& s) {
    if (s == "fuel_load" || s == "map" || s == "map_est") return Kind::Pressure;
    if (s == "charge_load") return Kind::ChargePct;
    if (s == "tps") return Kind::Throttle;
    return Kind::Other;
}

std::vector<double> spread(int n, double lo, double hi) {
    std::vector<double> v(static_cast<size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) v[size_t(i)] = n > 1 ? lo + (hi - lo) * i / (n - 1) : lo;
    return v;
}

}  // namespace

std::string preferredLoad(int fuelModel) {
    switch (fuelModel) {
        case 0: return "fuel_load";     // speed-density: MAP
        case 1: return "tps";           // Alpha-N
        case 2: return "charge_load";   // MAF
        case 3: return "charge_load";   // Blend: continuous through the crossover
        default: return {};
    }
}

const std::vector<std::string>& loadTables() {
    static const std::vector<std::string> t = { "fuel_calculator.target_lambda_table", "ignition.ign_table" };
    return t;
}

std::vector<std::string> offTarget(const Cache& c, int fuelModel) {
    std::vector<std::string> out;
    const MetaModel* m = c.meta();
    const std::string want = preferredLoad(fuelModel);
    if (!m || want.empty() || !m->signalMap().count(want)) return out;
    for (const std::string& t : loadTables()) {
        const MetaModel::Axis* ax = loadAxis(*m, t);
        if (!ax || ax->srcScalar.empty()) continue;
        const int cur = static_cast<int>(std::lround(c.configValue(ax->srcScalar)));
        if (cur != m->signalMap().at(want)) out.push_back(t);
    }
    return out;
}

std::string loadLabel(const Cache& c, const std::string& signal) {
    const MetaModel* m = c.meta();
    if (m) {
        const auto it = m->telemetry().find(signal);
        if (it != m->telemetry().end() && !it->second.label.empty()) return it->second.label;
    }
    return signal;
}

void apply(Cache& c, const std::vector<std::string>& tables, const std::string& signal) {
    const MetaModel* m = c.meta();
    if (!m || !m->signalMap().count(signal)) return;
    const int id = m->signalMap().at(signal);
    const Kind to = kindOf(signal);
    c.beginEdit();
    for (const std::string& t : tables) {
        const MetaModel::Axis* ax = loadAxis(*m, t);
        if (!ax || ax->srcScalar.empty()) continue;
        const Kind from = kindOf(signalName(*m, static_cast<int>(std::lround(c.configValue(ax->srcScalar)))));
        const int n = ax->nScalar.empty() ? ax->nMax : static_cast<int>(std::lround(c.configValue(ax->nScalar)));
        std::vector<double> v = c.axisValues(ax->array);
        v.resize(static_cast<size_t>(std::max(n, 0)));
        // THE ROWS KEEP THEIR MEANING WHERE THEY CAN: a kPa row becomes the % of a full charge that
        // pressure holds at 100 % VE, so boost rows stay boost rows. Throttle has no such mapping, so its
        // rows are spread evenly, and so are rows from a load this does not know how to convert.
        if (from == Kind::Pressure && to == Kind::ChargePct)
            for (double& x : v) x = x * 100.0 / kStdBaroKpa;
        else if (from == Kind::ChargePct && to == Kind::Pressure)
            for (double& x : v) x = x * kStdBaroKpa / 100.0;
        else if (to == Kind::Throttle)
            v = spread(n, 0.0, 100.0);
        else if (from != to)
            v = (to == Kind::Pressure) ? spread(n, 20.0, 100.0) : spread(n, 10.0, 100.0);
        c.setConfigValue(ax->srcScalar, id);
        if (from != to) c.writeAxisValues(ax->array, n, v);
    }
    c.endEdit("Point the load axes at " + loadLabel(c, signal));
}

}  // namespace airload

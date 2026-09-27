#include "EngineOutputLayout.h"

#include "Cache.h"

#include <algorithm>
#include <set>

namespace engine_outputs {

namespace {

// One output this engine wants, and the pin the standard wiring puts it on.
struct Want {
    int preferred = 0;   // row
    Row row;
};

bool validOrder(const Engine& e) {
    const int n = e.cylinders;
    if (n <= 0 || int(e.firingOrder.size()) < n) return false;
    std::set<int> seen;
    for (int i = 0; i < n; ++i) {
        const int c = e.firingOrder[i];
        if (c < 1 || c > n || !seen.insert(c).second) return false;
    }
    return true;
}

// Wasted spark: each cylinder's companion (0-based), or -1. Half a cycle away: on an even-fire engine
// that is n/2 firing positions later; on an odd-fire engine it is read off the TDCs, which is what the
// firmware compares. A broken firing order pairs nothing.
std::vector<int> companions(const Engine& e) {
    const int n = e.cylinders;
    std::vector<int> comp(std::max(n, 0), -1);
    if (!validOrder(e) || n < 2) return comp;
    if (!e.oddFire) {
        if (n % 2) return comp;
        std::vector<int> pos(n);
        for (int i = 0; i < n; ++i) pos[e.firingOrder[i] - 1] = i;
        for (int c = 0; c < n; ++c) comp[c] = e.firingOrder[(pos[c] + n / 2) % n] - 1;
        return comp;
    }
    const int cyc = e.cycleDeg10 > 0 ? e.cycleDeg10 : 7200;
    auto wrap = [cyc](int a) { a %= cyc; return a < 0 ? a + cyc : a; };
    for (int c = 0; c < n && c < int(e.tdcDeg10.size()); ++c)
        for (int k = 0; k < n && k < int(e.tdcDeg10.size()); ++k)
            if (k != c && wrap(e.tdcDeg10[k]) == wrap(e.tdcDeg10[c] + cyc / 2)) comp[c] = k;
    return comp;
}

std::vector<Want> coilWants(const Engine& e) {
    std::vector<Want> w;
    auto add = [&w](int row, int cyl, int plug) {
        Row r; r.row = row; r.function = Ignition; r.cylinder = cyl; r.plug = plug;
        w.push_back({ row, r });
    };
    if (e.rotary) {
        const int rotors = std::min(e.cylinders / kFacesPerRotor, kMaxRotors);
        if (rotors <= 0) return w;
        if (e.coils == Coils::Distributor) { add(0, CylAll, 0); add(kMaxRotors, CylAll, 1); return w; }
        for (int r = 0; r < rotors; ++r) add(r, r + 1, 0);
        for (int r = 0; r < rotors; ++r) add(kMaxRotors + r, r + 1, 1);
        return w;
    }
    const int n = std::min(e.cylinders, kIgnRows);
    if (n <= 0) return w;
    if (e.coils == Coils::Distributor) { add(0, CylAll, 0); return w; }
    if (e.coils == Coils::CoilOnPlug) { for (int c = 0; c < n; ++c) add(c, c + 1, 0); return w; }
    // WASTED SPARK: one coil per pair, named by its lower cylinder, pairs in cylinder order. A cylinder
    // with no companion (an odd count, a broken order) gets a coil of its own rather than none.
    const std::vector<int> comp = companions(e);
    std::vector<bool> done(n, false);
    int k = 0;
    for (int c = 0; c < n; ++c) {
        if (done[c]) continue;
        done[c] = true;
        if (comp[c] >= 0 && comp[c] < n) done[comp[c]] = true;
        add(k, c + 1, 0);
        ++k;
    }
    return w;
}

std::vector<Want> injectorWants(const Engine& e) {
    std::vector<Want> w;
    const int units = e.rotary ? e.cylinders / kFacesPerRotor : e.cylinders;
    if (units <= 0) return w;
    std::vector<int> present;
    for (int c = 0; c < e.cylinders && c < int(e.banks.size()); ++c)
        if (std::find(present.begin(), present.end(), e.banks[c]) == present.end() && present.size() < 2)
            present.push_back(e.banks[c]);
    std::sort(present.begin(), present.end());
    if (present.empty()) present.push_back(1);

    int base = 0;
    for (int s = 0; s < e.stages && s < kMaxStages; ++s) {
        const int mode = e.modes[s];
        const int outs = perCylinder(mode) ? units : std::max(e.injectors[s], 0);
        for (int i = 0; i < outs; ++i) {
            Row r; r.function = Injector; r.stage = s;
            if (perCylinder(mode))  r.cylinder = i + 1;
            else if (mode == Bank)  r.cylinder = (present[(i * int(present.size())) / outs] == 2) ? CylBank2
                                                                                                  : CylBank1;
            else                    r.cylinder = CylAll;
            r.row = kLsBase + base + i;
            w.push_back({ r.row, r });
        }
        base += outs;
    }
    return w;
}

// Put each want on its preferred pin unless that pin is a Generic output or already taken; the displaced
// ones then take the lowest free pin of the class, in order. Wants that find no pin are dropped.
void place(std::vector<Want>& wants, int first, int count, const std::vector<bool>& generic,
           std::vector<Row>& out) {
    std::vector<bool> used(count, false);
    auto freePin = [&](int row) {
        const int i = row - first;
        return i >= 0 && i < count && !used[i] && !(row < int(generic.size()) && generic[row]);
    };
    std::vector<size_t> displaced;
    for (size_t k = 0; k < wants.size(); ++k) {
        if (freePin(wants[k].preferred)) { used[wants[k].preferred - first] = true; wants[k].row.row = wants[k].preferred; }
        else                             displaced.push_back(k);
    }
    for (size_t k : displaced) {
        wants[k].row.row = -1;
        for (int row = first; row < first + count; ++row)
            if (freePin(row)) { used[row - first] = true; wants[k].row.row = row; break; }
    }
    for (const Want& w : wants)
        if (w.row.row >= 0) out.push_back(w.row);
}

} // namespace

std::vector<Row> layout(const Engine& e, bool coils, bool injectors, const std::vector<bool>& generic) {
    std::vector<Row> out;
    if (coils) {
        std::vector<Want> w = coilWants(e);
        place(w, 0, kIgnRows, generic, out);
    }
    if (injectors) {
        std::vector<Want> w = injectorWants(e);
        place(w, kLsBase, kLsRows, generic, out);
    }
    return out;
}

void triggers(const std::string& path, bool& coils, bool& injectors) {
    coils = injectors = false;
    if (path == "engine.cylinder_count" || path == "engine.cycle_type") { coils = injectors = true; return; }
    if (path == "engine.ign_mode") { coils = true; return; }
    if (path == "engine.num_inj_stages") { injectors = true; return; }
    const std::string pre = "engine.inj_stage[";
    if (path.compare(0, pre.size(), pre) == 0) {
        const auto dot = path.rfind('.');
        const std::string f = dot == std::string::npos ? "" : path.substr(dot + 1);
        if (f == "mode" || f == "num_outputs") injectors = true;
    }
}

std::vector<int> cylinderOptions(const Cache& c, const std::string& bind) {
    static const std::string pre = "outputs.output[", post = "].cylinder";
    if (bind.size() <= pre.size() + post.size() || bind.compare(0, pre.size(), pre) != 0
        || bind.compare(bind.size() - post.size(), post.size(), post) != 0) return {};
    const std::string row = bind.substr(0, bind.size() - post.size() + 1);   // "outputs.output[7]"
    const int fn = int(c.configValue(row + ".function"));
    if (fn != Ignition && fn != Injector) return {};
    const Engine e = readEngine(c);
    const int units = std::min(e.rotary ? e.cylinders / kFacesPerRotor : e.cylinders, 12);
    std::vector<int> out;
    auto cylinders = [&] { for (int n = 1; n <= units; ++n) out.push_back(n); };
    if (fn == Ignition) {
        if (e.coils == Coils::Distributor) out.push_back(CylAll);
        else                               cylinders();
        return out.empty() ? std::vector<int>{ CylNone } : out;
    }
    const int s = std::clamp(int(c.configValue(row + ".inj_stage")), 0, kMaxStages - 1);
    const int mode = e.modes[s];
    if (perCylinder(mode)) cylinders();
    else if (mode == Bank) {
        bool b1 = false, b2 = false;
        for (int k = 0; k < e.cylinders && k < int(e.banks.size()); ++k) { b1 |= e.banks[k] == 1; b2 |= e.banks[k] == 2; }
        if (b1 || !b2) out.push_back(CylBank1);
        if (b2)        out.push_back(CylBank2);
    } else out.push_back(CylAll);
    return out.empty() ? std::vector<int>{ CylNone } : out;
}

Engine readEngine(const Cache& c) {
    auto v = [&c](const std::string& p) { return int(c.configValue(p)); };
    Engine e;
    e.cylinders = v("engine.cylinder_count");
    const int cycle = v("engine.cycle_type");
    e.rotary = cycle == 2;
    e.cycleDeg10 = cycle == 0 ? 3600 : cycle == 2 ? 10800 : 7200;
    e.oddFire = v("engine.odd_fire") != 0;
    e.coils = static_cast<Coils>(v("engine.ign_mode"));
    for (int i = 0; i < 12; ++i) {
        const std::string ix = "[" + std::to_string(i) + "]";
        e.firingOrder.push_back(v("engine.firing_order" + ix + ".cyl"));
        e.tdcDeg10.push_back(v("engine.cyl" + ix + ".tdc_angle"));
        e.banks.push_back(v("engine.cyl" + ix + ".bank"));
    }
    e.stages = v("engine.num_inj_stages");
    for (int s = 0; s < kMaxStages; ++s) {
        const std::string ix = "engine.inj_stage[" + std::to_string(s) + "]";
        e.modes[s] = v(ix + ".mode");
        e.injectors[s] = v(ix + ".num_outputs");
    }
    return e;
}

void apply(Cache& c, bool coils, bool injectors) {
    if (!coils && !injectors) return;
    const int rows = kIgnRows + kLsRows;
    auto path = [](int i, const char* f) { return "outputs.output[" + std::to_string(i) + "]." + f; };
    if (!c.isConfig(path(0, "function")) || !c.isConfig(path(rows - 1, "cylinder"))) return;

    std::vector<bool> generic(rows, false);
    for (int i = 0; i < rows; ++i) generic[i] = int(c.configValue(path(i, "function"))) == Generic;
    const std::vector<Row> laid = layout(readEngine(c), coils, injectors, generic);

    auto set = [&](int i, const char* f, double v) {
        const std::string p = path(i, f);
        if (c.configValue(p) != v) c.setConfigValue(p, v);
    };
    c.beginEdit();
    // Clear the class first: a coil left over from a larger engine is still a coil to the firmware.
    for (int i = 0; i < rows; ++i) {
        const int fn = int(c.configValue(path(i, "function")));
        if ((coils && fn == Ignition) || (injectors && fn == Injector)) set(i, "function", None);
    }
    for (const Row& r : laid) {
        set(r.row, "cylinder", r.cylinder);
        if (r.function == Injector) set(r.row, "inj_stage", r.stage);
        if (r.function == Ignition) set(r.row, "ign_plug", r.plug);
        set(r.row, "function", r.function);
    }
    c.endEdit("Lay out coil and injector outputs");
    JLOGC("model.engine_outputs", jf::JLogLevel::Info)
        << "laid out " << laid.size() << " output row(s)" << (coils ? " [coils]" : "")
        << (injectors ? " [injectors]" : "");
}

void install(Cache& c) {
    c.configEdited.connect([&c](const std::string& p, double, double) {
        bool coils = false, injectors = false;
        triggers(p, coils, injectors);
        apply(c, coils, injectors);
    });
}

} // namespace engine_outputs

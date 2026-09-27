// What does a channel actually read — and who gets to say?
//
// The editor kept asking a channel's TELEMETRY descriptor for its range and its precision. For most
// channels that is right: codegen fills a sensor-backed channel's descriptor from the sensor's type at
// build time, so clt says C, 1 decimal, -40..300 because `temperature` says so.
//
// It is wrong for the fourteen GENERIC inputs (aux_1..6, rotary_trim_1..6, abs_mode, vehicle_dynamics).
// Those have no type until the tune gives them one, so their channel is wire-sized to the UNION of every
// type they could be given — -720..3000 at 0.01 — a number that describes no sensor. Read as precision it
// let an axis bin be typed to a hundredth of nothing; read as a range it would have let a kPa axis be
// clamped to -720. The fix is not a better guess in the studio: the meta now publishes the type catalog
// (sensor_types) and each element's build-time type (element_types, null for a generic row), and
// Cache::channelDomain() resolves a generic input's CONFIGURED type from the tune before answering.
//
// Everything here goes through that one public door, with the real definition and the real default tune.
//
//   cmake --build build --target sensor_domain_test && ./build/sensor_domain_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/TableImage.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }
static std::string show(const Cache::ChannelDomain& d) {
    if (!d.known()) return "unknown";
    char b[96];
    std::snprintf(b, sizeof b, "%g..%g %s @ %d dp", d.lo, d.hi, d.units.c_str(), d.digits);
    return b;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    std::puts("=== channel domain: what a channel reads ===");

    // 0 — the meta has to carry the two things the answer needs.
    {
        ck(!m.sensorTypes().empty(), "the meta publishes the sensor type catalog",
           std::to_string(m.sensorTypes().size()) + " types");
        std::string arr; int idx = 0;
        ck(m.sensorForSignal("clt", arr, idx) && arr == "sensors.sensor",
           "a channel resolves to the sensor that publishes it", arr + "[" + std::to_string(idx) + "]");
        const auto& ca = m.configArrays().at("sensors.sensor");
        int generic = 0;
        for (const auto& t : ca.elementTypes) if (t.empty()) ++generic;
        ck(int(ca.elementTypes.size()) == ca.count && generic == 14,
           "every element states its build-time type; 14 have none",
           std::to_string(generic) + " generic of " + std::to_string(ca.count));
    }

    // 1 — a CATALOGUED sensor: its type was settled at build time, so the descriptor already agrees.
    {
        const auto d = c.channelDomain("clt");
        ck(d.known() && near(d.lo, -40) && near(d.hi, 300) && d.digits == 1 && d.units == "C",
           "clt (temperature) reads C, 1 dp, -40..300", show(d));
        const auto t = c.channelDomain("tps");
        ck(t.known() && d.digits == 1 && t.units == "%", "tps (percent) reads %, 1 dp", show(t));
    }

    // 2 — a channel NO sensor publishes: the descriptor is its own authority.
    {
        const auto d = c.channelDomain("rpm");
        ck(d.known() && near(d.hi, 30000) && d.digits == 0 && d.units == "RPM",
           "rpm is not sensor-backed — its descriptor answers", show(d));
    }

    // 3 — a GENERIC input, unconfigured. Nothing can say what it reads, and saying so is the point:
    //     the union wire (-720..30000; the top is high_pressure's bottle range) must NOT be handed back
    //     as if it were a sensor.
    {
        const auto d = c.channelDomain("aux_1");
        ck(!d.known(), "aux_1 with no type set: unknown, NOT the union wire", show(d));
        double lo = 0, hi = 0;
        const bool wire = m.signalRange("aux_1", lo, hi);
        ck(wire && near(lo, -720) && near(hi, 30000),
           "…and the raw descriptor is indeed that union", std::to_string(lo) + ".." + std::to_string(hi));
    }

    // 4 — the same input, once the tune gives it a type. THIS is the case the old path could not express.
    {
        const auto typeIndex = [&](const std::string& id) {
            for (size_t i = 0; i < m.sensorTypes().size(); ++i) if (m.sensorTypes()[i].id == id) return int(i);
            return -1;
        };
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const auto p = c.channelDomain("aux_1");
        ck(p.known() && near(p.lo, -101.3) && near(p.hi, 3000) && p.digits == 1 && p.units == "kPa",
           "aux_1 set to pressure reads kPa, 1 dp, -101.3..3000", show(p));

        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("lambda"));
        const auto l = c.channelDomain("aux_1");
        // The unit is the SYMBOL now (schema: "lambda is spelled with the lambda sign") — what the
        // gauges print beside the number, not the word.
        // 0.5..1.5, not 0.6..1.6 — the catalogue's own range since "a lambda reading is 0.5 to 1.5, and
        // the module reads that from the catalog" (e80b982). The point of the assertion is that the
        // NUMBER comes from the schema rather than from the widget, so it moves when the schema does.
        ck(l.known() && near(l.lo, 0.5) && near(l.hi, 1.5) && l.digits == 2 && l.units == "\u03bb",
           "…and set to lambda it reads λ, 2 dp, 0.5..1.5", show(l));

        // Out of range = not a type. Better unknown than a confident wrong domain.
        c.setConfigValue("sensors.sensor[aux_1].type", 200);
        ck(!c.channelDomain("aux_1").known(), "a type byte outside the catalog reads as unknown");
    }

    // 5 — a catalogued row IGNORES its type byte, exactly as the firmware does (effective_type()).
    {
        const auto before = c.channelDomain("clt");
        c.setConfigValue("sensors.sensor[clt].type", 6);   // 'lambda' — meaningless on a catalogued row
        const auto after = c.channelDomain("clt");
        ck(after.known() && near(after.lo, before.lo) && near(after.hi, before.hi)
               && after.digits == before.digits && after.units == before.units,
           "writing clt's type byte changes nothing — the catalog owns it", show(after));
    }

    // 6 — a bin is BORN inside its channel's domain too. Insert is not a typed value, so it never met
    //     the entry rule: appending at the end extrapolates last + (last - prev), which on an axis
    //     reading app_1 (0..100) walked straight past the ceiling — 100 -> 110 -> 120, a breakpoint the
    //     pedal cannot reach. Same rule, applied where the value is made rather than where it is typed.
    {
        const TableImage ti = m.resolveTable("ignition.ign_table");
        const int axis = 1;                                   // the load axis — it has a src selector
        const int appId = m.signalMap().count("app_1") ? m.signalMap().at("app_1") : -1;
        ck(appId >= 0 && ti.valid && ti.axes.size() > 1 && ti.axes[axis].srcBase >= 0,
           "ign_table's Y axis has a channel selector");
        Cache::WriteGuard wg(c);
        c.tiSetSrc(ti, axis, appId);                          // point it at app_1, as the dialog would

        std::vector<double> bins = c.tiBins(ti, axis);
        const int n = int(bins.size());
        for (int i = 0; i < n; ++i) bins[i] = 100.0 * i / (n - 1);   // 0..100, the full domain
        c.tiWriteBins(ti, axis, bins);

        c.tiInsertBin(ti, axis, n);                           // append past the end
        const std::vector<double> after = c.tiBins(c.resolveTable("ignition.ign_table"), axis);
        ck(int(after.size()) == n + 1, "the bin was inserted", std::to_string(after.size()));
        ck(!after.empty() && after.back() <= 100.0 + 1e-9,
           "an appended bin cannot land outside what app_1 reads", "got " + std::to_string(after.back()));

        // …and on the channel's grid: a midpoint insert between two tenths must not mint a hundredth.
        std::vector<double> b2 = c.tiBins(c.resolveTable("ignition.ign_table"), axis);
        b2[0] = 10.0; b2[1] = 10.1;
        c.tiWriteBins(c.resolveTable("ignition.ign_table"), axis, b2);
        c.tiInsertBin(c.resolveTable("ignition.ign_table"), axis, 1);   // midpoint of 10.0 and 10.1
        const double mid = c.tiBins(c.resolveTable("ignition.ign_table"), axis)[1];
        ck(std::fabs(mid - 10.1) < 1e-6 || std::fabs(mid - 10.0) < 1e-6,
           "a midpoint insert snaps to the channel's precision", "got " + std::to_string(mid));
    }

    // 7 — CHANGING the channel does NOT touch the bins, and the round trip is exact.
    //
    //     The reference ECU behaves this way for a storage reason — its axis holds a raw count that
    //     each type re-reads through its own scale and floor, so a 0..10000 rpm axis displays as
    //     -273..727 C and comes back as 0..10000. We reach the same property more directly: our bins
    //     are engineering values, so preserving them IS preserving the raw. An earlier version here
    //     stretched the span onto the new channel's domain, which made every bin legal but could not
    //     be undone once its edit was spent. Legality is enforced where a value is authored instead.
    {
        const TableImage ti = m.resolveTable("fuel_calculator.ve_table");
        Cache::WriteGuard wg(c);
        std::vector<double> b = c.tiBins(ti, 1);
        const int n = int(b.size());
        for (int i = 0; i < n; ++i) b[i] = 500.0 * i;         // an rpm-shaped axis: 0 .. 500*(n-1)
        b[1] = 30.444;                                        // and the screenshot's off-grid bin
        c.tiWriteBins(ti, 1, b);
        const std::vector<double> before = c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1);

        c.tiSetSrc(ti, 1, m.signalMap().at("app_1"));         // pedal: 0..100 at a tenth — most bins illegal
        const std::vector<double> asApp = c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1);
        ck(asApp == before, "picking a new channel leaves every bin exactly as it was");

        c.tiSetSrc(ti, 1, m.signalMap().at("rpm"));           // …and back
        ck(c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1) == before,
           "so the round trip returns the original axis, not an approximation of it");

        // The dialog's flag is what makes the illegal ones visible: same predicate, checked here.
        const auto dom = c.channelDomain("app_1");
        int outOfRange = 0, offGrid = 0;
        for (double v : before) {
            if (dom.hasRange && (v < dom.lo || v > dom.hi)) ++outOfRange;
            else if (std::fabs(v - snapBreak(v, dom.digits)) > 1e-3) ++offGrid;
        }
        ck(outOfRange > 0 && offGrid == 1,
           "and the bins the pedal cannot reach are identifiable, to be flagged",
           std::to_string(outOfRange) + " out of range, " + std::to_string(offGrid) + " off-grid");
    }

    // 8 — the wizard: start/end/increment sets the axis SIZE as well as its values, which is the part
    //     no per-bin edit can do. Bins still go through the channel's rule on the way in.
    {
        const TableImage ti = m.resolveTable("fuel_calculator.ve_table");
        Cache::WriteGuard wg(c);
        c.tiSetSrc(ti, 1, m.signalMap().at("app_1"));
        c.beginEdit();
        const bool ok = c.tiBuildAxis(ti, 1, 0.0, 100.0, 5.0);
        c.endEdit("Axis wizard");
        const std::vector<double> a = c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1);
        ck(ok && int(a.size()) == 21, "0..100 every 5 gives 21 bins", std::to_string(a.size()));
        ck(!a.empty() && near(a.front(), 0.0) && near(a.back(), 100.0),
           "…spanning exactly the range asked for", std::to_string(a.front()) + ".." + std::to_string(a.back()));
        bool stepped = true;
        for (size_t i = 1; i < a.size(); ++i) if (std::fabs((a[i] - a[i - 1]) - 5.0) > 1e-3) stepped = false;
        ck(stepped, "…in even steps of 5");

        // A step that does not divide the span stops before overshooting rather than running past the end.
        c.beginEdit(); c.tiBuildAxis(ti, 1, 0.0, 10.0, 3.0); c.endEdit("Axis wizard");
        const std::vector<double> b = c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1);
        ck(int(b.size()) == 4 && near(b.back(), 9.0), "0..10 every 3 ends at 9, not past 10",
           std::to_string(b.size()) + " bins ending " + std::to_string(b.back()));

        // Nonsense is refused, not turned into a one-bin axis.
        ck(!c.tiBuildAxis(ti, 1, 0.0, 100.0, 0.0) && !c.tiBuildAxis(ti, 1, 100.0, 0.0, 5.0),
           "a zero step or a backwards range is refused");

        // The schema's own ceiling still wins: asking for more bins than the axis allows is clamped.
        c.beginEdit(); c.tiBuildAxis(ti, 1, 0.0, 1000.0, 1.0); c.endEdit("Axis wizard");
        const int n = int(c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1).size());
        ck(n <= std::max(1, ti.axes[1].nMax), "and the axis cannot grow past its declared maximum",
           std::to_string(n) + " of " + std::to_string(ti.axes[1].nMax));
    }

    // 9 — a sensor CALIBRATION's cells are readings in that sensor's type: its units, its precision and
    //     its domain. The meta says none of it — the cal declares min 0 / max 0 (unset) and leaves the
    //     scale to a per-element list — so the cells were unbounded (clamped only by S16, ±327 at 0.01)
    //     and rendered at whatever the widget defaulted to.
    {
        const TableImage clt = c.resolveTable("sensors.sensor[clt].cal");
        ck(clt.valid && clt.cellTypeScaled && clt.elemArray == "sensors.sensor",
           "the cal resolves as a type-scaled element table", clt.elemArray);
        ck(clt.axes.size() == 1, "…with ONE axis (raw ADC counts in, a reading out)",
           std::to_string(clt.axes.size()));
        ck(clt.cellDigits == 1 && clt.cellUnits == "C",
           "clt's cells are C to a tenth — from the temperature type",
           clt.cellUnits + " @ " + std::to_string(clt.cellDigits));
        // Bounds are RAW: temperature is -40..300 engineering, and the cal stores at the type's scale.
        ck(clt.cellMaxV > clt.cellMinV && near(clt.cellMinV * clt.cellScale, -40.0)
               && near(clt.cellMaxV * clt.cellScale, 300.0),
           "…and bounded by the type's domain, not by what an int16 happens to hold",
           std::to_string(clt.cellMinV * clt.cellScale) + ".." + std::to_string(clt.cellMaxV * clt.cellScale));

        const TableImage lam = c.resolveTable("sensors.sensor[lambda_1].cal");
        ck(lam.valid && lam.cellDigits == 2 && lam.cellUnits == "\u03bb",
           "a lambda cal reads to hundredths", lam.cellUnits + " @ " + std::to_string(lam.cellDigits));

        // A GENERIC input takes it from the tune, so the same table answers differently once configured.
        const auto typeIndex = [&](const std::string& id) {
            for (size_t i = 0; i < m.sensorTypes().size(); ++i) if (m.sensorTypes()[i].id == id) return int(i);
            return -1;
        };
        c.setConfigValue("sensors.sensor[aux_1].type", 200);            // not a type
        ck(c.resolveTable("sensors.sensor[aux_1].cal").cellDigits < 0,
           "an unconfigured generic input's cal states no precision");
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const TableImage aux = c.resolveTable("sensors.sensor[aux_1].cal");
        ck(aux.cellDigits == 1 && aux.cellUnits == "kPa",
           "…set it to pressure and its cal reads kPa to a tenth",
           aux.cellUnits + " @ " + std::to_string(aux.cellDigits));
        ck(aux.cellMaxV > aux.cellMinV && near(aux.cellMaxV * aux.cellScale, 3000.0),
           "…bounded by pressure's domain", std::to_string(aux.cellMaxV * aux.cellScale));

        // The bound must never be wider than the storage: encodeRaw casts, so a bound past int16 would
        // let a write wrap rather than clamp.
        ck(aux.cellMinV >= -32768.0 && aux.cellMaxV <= 32767.0,
           "…and never wider than the cell's own datatype",
           std::to_string(aux.cellMinV) + ".." + std::to_string(aux.cellMaxV));
    }

    // 10 — the studio and the FIRMWARE must agree about what a cal byte means. The firmware decodes at
    //      SENSOR_TYPE_CATALOG[effective_type].val_scale; the meta's per-element scale list is fixed at
    //      build time, so for a generic input (no type then) it is a guess of 0.01. Taking the scale from
    //      the resolved type is what keeps a stored point reading the same number at both ends.
    {
        const auto typeIndex = [&](const std::string& id) {
            for (size_t i = 0; i < m.sensorTypes().size(); ++i) if (m.sensorTypes()[i].id == id) return int(i);
            return -1;
        };
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const TableImage aux = c.resolveTable("sensors.sensor[aux_1].cal");
        const MetaModel::SensorType* press = m.sensorType("pressure");
        ck(press && near(aux.cellScale, press->scale) && near(aux.cellScale, 0.1),
           "a generic input's cal takes the CONFIGURED type's scale, not the build-time 0.01",
           std::to_string(aux.cellScale));

        // A catalogued sensor was always right; it must stay right.
        const TableImage clt = c.resolveTable("sensors.sensor[clt].cal");
        ck(near(clt.cellScale, m.sensorType("temperature")->scale),
           "…and a catalogued sensor still reads at its own type's scale", std::to_string(clt.cellScale));

        // Which is the whole point: a point written through the door reads back as the same number.
        Cache::WriteGuard wg(c);
        const int off = clt.cellBase;                       // cal point 0
        // …written as ENGINEERING and converted at the door, which is what uncook/cook are for. This
        // used to divide and multiply by cellScale inline, which is the same arithmetic with nothing
        // saying which side of it a number was on.
        c.writeRaw(c.cellLocAt(clt, off), uncook(Eng{90.0}, clt.cellScale));
        ck(near(cook(c.readRaw(c.cellLocAt(clt, off)), clt.cellScale).v, 90.0),
           "a cal point written as 90 C reads back as 90 C");
    }

    // 11 — a CALIBRATION's axis has no channel, and "no channel" was answered with "nothing known": six
    //      decimal places offered on an integer ADC axis, no bound at all, and Insert seeding 5000 on an
    //      input that stops at 4095. An axis without a channel still has facts — its storage, and for a
    //      sensor cal the interface that says what raw MEANS.
    {
        const TableImage cal = c.resolveTable("sensors.sensor[clt].cal");
        const auto d = c.axisDomain(cal, 0);
        ck(d.digits == 0, "an integer ADC axis counts in whole numbers", std::to_string(d.digits));
        ck(d.hasRange && near(d.lo, 0.0) && near(d.hi, 4095.0),
           "…and spans the board's ADC, not an int16",
           std::to_string(d.lo) + ".." + std::to_string(d.hi));
        ck(near(c.snapBin(cal, 0, 5000.0), 4095.0), "so Insert's 5000 lands on 4095");
        // The pin is UNASSIGNED in the default tune, and the answer still has to be the ADC's: every
        // channel in a pool shares one descriptor, so the pool speaks for a pin not yet chosen.
        ck(near(c.configValue("sensors.sensor[clt].source"), -1.0),
           "…with no pin assigned at all", std::to_string(c.configValue("sensors.sensor[clt].source")));
        {
            const auto* pool = m.hwPoolSignals("analog_voltage");
            bool oneShape = pool && !pool->empty();
            if (pool) for (const auto& n : *pool) {
                const auto it = m.telemetry().find(n);
                if (it == m.telemetry().end() || !near(it->second.maxV, 4095.0)) oneShape = false;
            }
            ck(oneShape, "…which is only sound because every pin in the pool reads the same",
               std::to_string(pool ? pool->size() : 0) + " pins");
        }
        ck(near(c.snapBin(cal, 0, 30.7), 31.0), "and 30.7 is not a count");

        // The interface decides what raw means: a frequency input is not counting ADC steps, so the
        // honest bound there is what the storage holds, not the ADC's 4095.
        const auto& ca = m.configArrays().at("sensors.sensor");
        const MetaModel::ArrayField* iface = nullptr;
        for (const auto& f : ca.fields) if (f.name == "interface") iface = &f;
        ck(iface && !iface->optionIds.empty() && iface->optionIds[0] == "analog_voltage",
           "the interface options carry ids, so nothing matches on a label");
        int freqSel = -1;
        for (size_t i = 0; iface && i < iface->optionIds.size(); ++i)
            if (iface->optionIds[i] == "digital_freq") freqSel = int(i);
        Cache::WriteGuard wg(c);
        c.setConfigValue("sensors.sensor[clt].interface", freqSel);
        const auto f = c.axisDomain(c.resolveTable("sensors.sensor[clt].cal"), 0);
        ck(f.hasRange && f.hi > 4095.0, "a frequency input's raw axis is not bounded by the ADC",
           std::to_string(f.hi));
        ck(f.digits == 0, "…and is still whole numbers");
    }

    // 12 — the cells and the bounds have to agree about scale, or the dialog says 200 and the grid can
    //      only reach 20. configScale/unit resolve the type the same way resolveTable does.
    {
        const auto typeIndex = [&](const std::string& id) {
            for (size_t i = 0; i < m.sensorTypes().size(); ++i) if (m.sensorTypes()[i].id == id) return int(i);
            return -1;
        };
        Cache::WriteGuard wg(c);
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("percent"));
        const TableImage aux = c.resolveTable("sensors.sensor[aux_1].cal");
        ck(near(c.configScale("sensors.sensor[aux_1].cal"), aux.cellScale),
           "the display scale is the one the bounds were computed with",
           std::to_string(c.configScale("sensors.sensor[aux_1].cal")) + " vs " + std::to_string(aux.cellScale));
        ck(near(aux.cellMaxV * c.configScale("sensors.sensor[aux_1].cal"), 200.0),
           "so a percent cal reaches 200, not 20",
           std::to_string(aux.cellMaxV * c.configScale("sensors.sensor[aux_1].cal")));
        ck(c.unit("sensors.sensor[aux_1].cal") == "%", "and it reads in the type's units",
           c.unit("sensors.sensor[aux_1].cal"));
    }

    // 13 — the wizard cannot produce duplicate breakpoints, whatever it is asked for. Generating the
    //      ramp and clamping each bin as it is written turns any overshoot into a run of identical
    //      values, and locate() takes the LAST bin equal to a value — so every cell behind a duplicate
    //      is unreachable. The request is brought inside the axis FIRST instead.
    {
        const TableImage ti = m.resolveTable("fuel_calculator.ve_table");
        Cache::WriteGuard wg(c);
        c.tiSetSrc(ti, 1, m.signalMap().at("app_1"));      // pedal: 0..100 at a tenth
        const auto distinct = [&] {
            const std::vector<double> b = c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1);
            for (size_t i = 1; i < b.size(); ++i) if (near(b[i], b[i - 1]) || b[i] < b[i - 1]) return false;
            return true;
        };
        const auto inRange = [&] {
            for (double v : c.tiBins(c.resolveTable("fuel_calculator.ve_table"), 1))
                if (v < -1e-6 || v > 100.0 + 1e-6) return false;
            return true;
        };

        c.beginEdit(); c.tiBuildAxis(ti, 1, 0.0, 500.0, 10.0); c.endEdit("Axis wizard");
        ck(distinct() && inRange(), "an end past the axis (0..500) gives no duplicates and stays in range");

        c.beginEdit(); c.tiBuildAxis(ti, 1, -50.0, 300.0, 5.0); c.endEdit("Axis wizard");
        ck(distinct() && inRange(), "…and so does a start below it (-50..300)");

        c.beginEdit(); c.tiBuildAxis(ti, 1, 0.0, 100.0, 0.001); c.endEdit("Axis wizard");
        ck(distinct(), "a step finer than the axis can express is raised to one step, not duplicated");

        ck(!c.tiBuildAxis(ti, 1, 200.0, 300.0, 5.0),
           "a span entirely outside the axis is refused rather than collapsed onto the ceiling");
    }

    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

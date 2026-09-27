// Does what you type into a sensor calibration come back — and is what got STORED the number the
// firmware will read?
//
// A cal cell is not a free number. It passes through four things on the way to the flash image: the
// widget's display conversion (dispV/srcV), the schema clamp in writeRaw, the integer encode in
// encodeRaw (which ROUNDS), and the sensor type's scale. Each is right on its own; the question this
// asks is whether they agree, for every type an input can be given, at the ends of every range.
//
// It goes through the same doors the grid does — TableWidget::setCellValue / cellValue with a real
// PanelElement — so a pass means the path a tuner uses is sound, not that a private helper is.
//
//   cmake --build build --target sensor_roundtrip_test && ./build/sensor_roundtrip_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/widgets/TableWidget.h"

// The FIRMWARE's own interpolator, compiled here against the same bytes the studio writes. It has no
// dependencies beyond <cstdint>, which is what makes this cross-check possible at all.
#include "Engine/TableEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string num(double v) { char b[48]; std::snprintf(b, sizeof b, "%g", v); return b; }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    Cache::WriteGuard wg(c);

    PanelElement el;                       // a table widget bound to the cal, as the dictionary drops one
    el.type = "table";
    el.props["signalName"] = "sensors.sensor[aux_1].cal";

    const auto typeIndex = [&](const std::string& id) {
        for (size_t i = 0; i < m.sensorTypes().size(); ++i) if (m.sensorTypes()[i].id == id) return int(i);
        return -1;
    };

    std::puts("=== every type an aux input can be given: units, bounds, quantisation ===");
    for (const MetaModel::SensorType& st : m.sensorTypes()) {
        if (!st.selectable) continue;                 // a generic input can only be given these
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex(st.id));
        const TableImage t = c.resolveTable("sensors.sensor[aux_1].cal");
        const std::string tag = st.id;

        // 1. The three facts all come from the type, and the display path agrees with the model.
        const bool described = t.cellUnits == st.units && t.cellDigits == st.digits
                            && std::fabs(t.cellScale - st.scale) < 1e-12
                            && std::fabs(c.configScale("sensors.sensor[aux_1].cal") - st.scale) < 1e-12
                            && c.unit("sensors.sensor[aux_1].cal") == st.units;
        ck(described, tag + ": units/precision/scale are the type's, model and display agreeing",
           t.cellUnits + " @ " + std::to_string(t.cellDigits) + "dp x" + num(t.cellScale));

        // 2. The cell bounds are the type's domain — or the storage's, when the type asks for more than
        //    an int16 at this scale can hold. Whichever is tighter, because a write that exceeds the
        //    datatype wraps rather than clamping.
        const double loEng = t.cellMinV * t.cellScale, hiEng = t.cellMaxV * t.cellScale;
        const double storeLo = -32768.0 * t.cellScale, storeHi = 32767.0 * t.cellScale;
        const double wantLo = std::max(st.minV, storeLo), wantHi = std::min(st.maxV, storeHi);
        ck(std::fabs(loEng - wantLo) < 1e-6 && std::fabs(hiEng - wantHi) < 1e-6,
           tag + ": cells bounded by the type, never past what an int16 holds",
           num(loEng) + ".." + num(hiEng) + " (wanted " + num(wantLo) + ".." + num(wantHi) + ")");

        // 3. QUANTISATION. Write a value the type cannot express exactly (a third of a step past a
        //    breakpoint) and read it back: it must land on the storage grid, within half a step, and be
        //    EXACTLY representable — i.e. writing it again changes nothing.
        const double step = st.scale;
        const double mid  = std::floor(((wantLo + wantHi) * 0.5) / step) * step;
        const double off  = mid + step / 3.0;                 // deliberately off-grid
        TableWidget::setCellValue(el, t, t.cellBase, off);
        const double back = TableWidget::cellValue(el, t, t.cellBase);
        const bool onGrid = std::fabs(back / step - std::round(back / step)) < 1e-6;
        ck(onGrid && std::fabs(back - off) <= step * 0.5 + 1e-9,
           tag + ": an off-grid value quantises to the storage grid, within half a step",
           num(off) + " -> " + num(back));
        TableWidget::setCellValue(el, t, t.cellBase, back);
        ck(std::fabs(TableWidget::cellValue(el, t, t.cellBase) - back) < 1e-9,
           tag + ": …and what came back is stable — writing it again is a no-op", num(back));

        // 4. The ends hold, and nothing beyond them is accepted.
        TableWidget::setCellValue(el, t, t.cellBase, wantHi);
        const double atHi = TableWidget::cellValue(el, t, t.cellBase);
        TableWidget::setCellValue(el, t, t.cellBase, wantHi + 10.0 * step);
        const double overHi = TableWidget::cellValue(el, t, t.cellBase);
        TableWidget::setCellValue(el, t, t.cellBase, wantLo - 10.0 * step);
        const double underLo = TableWidget::cellValue(el, t, t.cellBase);
        ck(std::fabs(atHi - wantHi) <= step * 0.5 + 1e-9, tag + ": the top of the range is reachable",
           num(wantHi) + " -> " + num(atHi));
        ck(overHi <= wantHi + 1e-6 && underLo >= wantLo - 1e-6,
           tag + ": and past either end is clamped, not wrapped",
           num(overHi) + " / " + num(underLo));
    }

    std::puts("");
    std::puts("=== every generic input, not just the first ===");
    {
        // There are fourteen rows with no build-time type — six aux inputs, six rotary trims, an ABS mode
        // and a vehicle-dynamics input. They share one struct and one code path, so what matters is that
        // each is independently resolvable: the type is read from ITS OWN byte, not from a neighbour's.
        const auto& ca = m.configArrays().at("sensors.sensor");
        std::vector<std::string> generic;
        for (size_t i = 0; i < ca.elementTypes.size(); ++i)
            if (ca.elementTypes[i].empty()) generic.push_back(ca.elementIds[i]);
        ck(generic.size() == 14, "fourteen inputs take their type from the tune",
           std::to_string(generic.size()) + " of " + std::to_string(ca.count));

        // Give each a DIFFERENT type, then check every one still reads as the type it was given —
        // a resolver keyed off the wrong element would show the last one written for all of them.
        std::vector<std::string> want;
        static const char* cycle[] = { "pressure", "temperature", "lambda", "voltage", "percent", "flow" };
        for (size_t i = 0; i < generic.size(); ++i) {
            want.push_back(cycle[i % 6]);
            c.setConfigValue("sensors.sensor[" + generic[i] + "].type", typeIndex(want[i]));
        }
        bool allOwn = true;
        std::string firstBad;
        for (size_t i = 0; i < generic.size(); ++i) {
            const TableImage t = c.resolveTable("sensors.sensor[" + generic[i] + "].cal");
            const MetaModel::SensorType* st = m.sensorType(want[i]);
            if (!st || t.cellUnits != st->units || t.cellDigits != st->digits
                || std::fabs(t.cellScale - st->scale) > 1e-12) {
                allOwn = false;
                if (firstBad.empty()) firstBad = generic[i] + " wanted " + want[i] + ", got " + t.cellUnits;
            }
        }
        ck(allOwn, "…and each reads as the type IT was given, not its neighbour's", firstBad);
    }

    std::puts("");
    std::puts("=== a type a generic input cannot BE ===");
    for (const MetaModel::SensorType& st : m.sensorTypes()) {
        if (st.selectable) continue;                  // switch / frequency / composition
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex(st.id));
        const TableImage t = c.resolveTable("sensors.sensor[aux_1].cal");
        // effective_type() refuses these on a generic row — the input is read as an analog voltage
        // through a cal curve, and these are not built that way — so it stays UNCONFIGURED. Saying
        // otherwise here would describe an input the firmware publishes nothing for.
        ck(t.cellDigits < 0 && t.cellUnits.empty(),
           st.id + ": stored on a generic input, it is still unconfigured — no units, no precision",
           t.cellUnits + " @ " + std::to_string(t.cellDigits));
    }

    std::puts("");
    std::puts("=== the same, for a CATALOGUED sensor of every type (including the three a generic");
    std::puts("    input can never be given, which only exist on a catalogued row) ===");
    {
        const auto& ca = m.configArrays().at("sensors.sensor");
        std::vector<std::pair<std::string, std::string>> oneEach;   // type -> first sensor with it
        for (size_t i = 0; i < ca.elementTypes.size(); ++i) {
            const std::string& ty = ca.elementTypes[i];
            if (ty.empty()) continue;
            bool have = false;
            for (const auto& p : oneEach) if (p.first == ty) have = true;
            if (!have) oneEach.emplace_back(ty, ca.elementIds[i]);
        }
        for (const auto& [ty, id] : oneEach) {
            const MetaModel::SensorType* st = m.sensorType(ty);
            const std::string path = "sensors.sensor[" + id + "].cal";
            PanelElement ce; ce.type = "table"; ce.props["signalName"] = path;
            const TableImage t = c.resolveTable(path);
            const std::string tag = ty + " (" + id + ")";
            ck(st && t.valid && t.cellUnits == st->units && t.cellDigits == st->digits,
               tag + ": reads in its own type's units and precision",
               t.cellUnits + " @ " + std::to_string(t.cellDigits) + "dp");

            // The DOMAIN a type declares can be wider than the cal's int16 can hold at that scale —
            // frequency asks for 100 kHz at a scale of 1. The bound has to be the storage's then, or a
            // write would wrap; the type is not free to promise more than the bytes can carry.
            const double hiEng = t.cellMaxV * t.cellScale, storeHi = 32767.0 * t.cellScale;
            const bool capped = st && st->maxV > storeHi;
            ck(std::fabs(hiEng - (capped ? storeHi : st->maxV)) < 1e-6,
               tag + (capped ? ": type asks past int16, so the STORAGE bounds it"
                             : ": bounded by the type's own domain"),
               num(hiEng) + (capped ? " (type wants " + num(st->maxV) + ")" : ""));

            // And the round trip still holds at that bound.
            TableWidget::setCellValue(ce, t, t.cellBase, hiEng + 5.0 * t.cellScale);
            const double atTop = TableWidget::cellValue(ce, t, t.cellBase);
            ck(atTop <= hiEng + 1e-6 && atTop >= hiEng - t.cellScale - 1e-6,
               tag + ": writing past the top lands ON the top, never wrapped", num(atTop));
        }
    }

    std::puts("");
    std::puts("=== how a calibration VIEWS: the geometry the grid actually draws with ===");
    {
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("lambda"));
        PanelElement ve; ve.type = "table"; ve.props["signalName"] = "sensors.sensor[aux_1].cal";
        const jf::JRect r{ 0.f, 0.f, 600.f, 200.f };
        const TableWidget::TableGeom g = TableWidget::tableGeom(ve, r, c);
        ck(g.t.valid && g.t.axes.size() == 1, "a cal resolves to a ONE-axis grid",
           std::to_string(g.t.axes.size()) + " axes");
        ck(g.dec == 2, "its cells draw at the type's precision (lambda: 2)", std::to_string(g.dec));
        const int axisDec = TableWidget::axisDecimals(ve, c, g.t, 0, g.dec);
        ck(axisDec == 0, "its axis draws as whole ADC counts, not at the cells' precision",
           std::to_string(axisDec));
        ck(g.rows >= 1 && g.cols >= 1, "and it has a grid to draw",
           std::to_string(g.rows) + "x" + std::to_string(g.cols));

        // Change the input's type and the view follows, with nothing stored on the widget.
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("temperature"));
        const TableWidget::TableGeom g2 = TableWidget::tableGeom(ve, r, c);
        ck(g2.dec == 1, "…and re-typing the input to temperature redraws the cells at a tenth",
           std::to_string(g2.dec));
    }

    std::puts("");
    std::puts("=== the raw axis those cals are read against ===");
    {
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const TableImage t = c.resolveTable("sensors.sensor[aux_1].cal");
        const auto d = c.axisDomain(t, 0);
        ck(d.hasRange && std::fabs(d.lo) < 1e-9 && std::fabs(d.hi - 4095.0) < 1e-9 && d.digits == 0,
           "an analog input's cal axis is ADC counts, 0..4095, whole numbers",
           num(d.lo) + ".." + num(d.hi) + " @ " + std::to_string(d.digits) + "dp");
        ck(std::fabs(c.snapBin(t, 0, 4096.0) - 4095.0) < 1e-9
               && std::fabs(c.snapBin(t, 0, -1.0)) < 1e-9
               && std::fabs(c.snapBin(t, 0, 2047.6) - 2048.0) < 1e-9,
           "…and a typed breakpoint is clamped and rounded to a count");

        // The axis stores U16 counts: what is written must read back identically, not near enough.
        std::vector<double> bins = c.tiBins(t, 0);
        for (size_t i = 0; i < bins.size(); ++i) bins[i] = c.snapBin(t, 0, 4095.0 * double(i) / double(bins.size() - 1));
        c.tiWriteBins(t, 0, bins);
        const std::vector<double> got = c.tiBins(t, 0);
        bool exact = got.size() == bins.size();
        for (size_t i = 0; exact && i < got.size(); ++i) if (std::fabs(got[i] - bins[i]) > 1e-9) exact = false;
        ck(exact, "every breakpoint written to the axis reads back exactly");
    }

    std::puts("");
    std::puts("=== and the FIRMWARE's own reading of what the studio stored ===");
    {
        // The studio and the ECU disagreeing about what a cal byte means is the failure this whole path
        // has had twice. So: write a calibration through the editor's door, then decode the resulting
        // BYTES with the firmware's own curve evaluation — tbl::interp over the raw arrays at the type's
        // val_scale, exactly as decode_curve does — and require the same number back.
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const TableImage t = c.resolveTable("sensors.sensor[aux_1].cal");
        const MetaModel::SensorType* st = m.sensorType("pressure");

        // A plausible 0.5..4.5 V sender across the ADC: counts in, kPa out.
        const int pts = c.tiLiveN(t, 0);
        std::vector<double> counts(pts), kpa(pts);
        for (int i = 0; i < pts; ++i) {
            counts[i] = c.snapBin(t, 0, 620.0 + (3720.0 - 620.0) * i / double(pts - 1));
            kpa[i]    = -100.0 + (1000.0 - -100.0) * i / double(pts - 1);
        }
        c.tiWriteBins(t, 0, counts);
        for (int i = 0; i < pts; ++i)
            TableWidget::setCellValue(el, t, t.cellBase + i * t.cellSize, kpa[i]);

        // Straight out of the image the studio would flash, as the firmware sees them.
        const std::vector<uint8_t>& img = c.configImage();
        const uint16_t* xs = reinterpret_cast<const uint16_t*>(img.data() + t.axes[0].breaksBase);
        const int16_t*  ys = reinterpret_cast<const int16_t*>(img.data() + t.cellBase);
        bool agrees = true;
        double worst = 0.0;
        for (int i = 0; i < pts; ++i) {
            const float raw = static_cast<float>(counts[i]);
            const float fw  = tbl::interp(ys, static_cast<float>(st->scale),
                                          xs, tbl::CELL_U16, pts, raw,
                                          nullptr, tbl::CELL_F32, 0, 0.0f,
                                          nullptr, tbl::CELL_F32, 0, 0.0f);
            const double studio = TableWidget::cellValue(el, t, t.cellBase + i * t.cellSize);
            worst = std::max(worst, std::fabs(double(fw) - studio));
            if (std::fabs(double(fw) - studio) > st->scale * 0.5 + 1e-6) agrees = false;
        }
        ck(agrees, "at every breakpoint, the firmware decodes exactly what the studio shows",
           "worst disagreement " + num(worst) + " kPa");

        // And BETWEEN breakpoints, where interpolation actually happens.
        const float mid = static_cast<float>((counts[0] + counts[1]) * 0.5);
        const float fwMid = tbl::interp(ys, static_cast<float>(st->scale), xs, tbl::CELL_U16, pts, mid,
                                        nullptr, tbl::CELL_F32, 0, 0.0f, nullptr, tbl::CELL_F32, 0, 0.0f);
        const double want = (kpa[0] + kpa[1]) * 0.5;
        ck(std::fabs(double(fwMid) - want) <= st->scale, "…and halfway between two, it interpolates to the midpoint",
           num(fwMid) + " vs " + num(want));
    }

    std::puts("");
    std::puts("=== showing a raw axis in volts, without moving a byte ===");
    {
        c.setConfigValue("sensors.sensor[aux_1].type", typeIndex("pressure"));
        const TableImage t = c.resolveTable("sensors.sensor[aux_1].cal");
        // "Raw" IS HOW YOU ASK FOR STORAGE UNITS. An empty preference used to mean that and does not any
        // more: empty (or "Auto") is "no page-level opinion", which resolves to the user's preference for
        // the quantity exactly as a reading does — without that, a sensor page showed its live raw readout
        // in volts and the calibration curve underneath it in counts, so the number could not be located
        // on the very table it was there to help fill in.
        const Cache::AxisView counts = c.axisView(t, 0, "Raw");     // as stored
        const Cache::AxisView autoV  = c.axisView(t, 0, "");        // …and what "no opinion" resolves to
        const Cache::AxisView volts  = c.axisView(t, 0, "ADC_V");
        const Cache::AxisView mv     = c.axisView(t, 0, "ADC_mV");

        ck(counts.hasRange && near(counts.lo, 0) && near(counts.hi, 4095) && counts.digits == 0,
           "counts: 0..4095, whole numbers", num(counts.lo) + ".." + num(counts.hi));
        ck(near(autoV.lo, volts.lo) && near(autoV.hi, volts.hi),
           "an empty preference follows the quantity's display unit, not storage",
           num(autoV.lo) + ".." + num(autoV.hi) + " " + autoV.label());
        // The AV front end is a 5 V divider read by a 12-bit ADC — 4095 counts IS 5 V at the pin, not the
        // 3.3 V the ADC reference sits at. The board says so (hardware.av.fullscale_mv), and taking that
        // rather than vref is the difference between a sender reading right and reading 1.5x high.
        ck(volts.hasRange && near(volts.lo, 0) && near(volts.hi, 5.0),
           "volts: the same axis, 0..5 V — the divider's full scale, not the ADC reference",
           num(volts.lo) + ".." + num(volts.hi) + " " + volts.label());
        ck(mv.hasRange && near(mv.hi, 5000.0), "millivolts: 0..5000 mV",
           num(mv.lo) + ".." + num(mv.hi) + " " + mv.label());

        // PRECISION follows the unit. One count is 1.22 mV: in volts that needs three places to keep two
        // counts apart, and in millivolts it needs none — a step bigger than the unit is already whole.
        ck(volts.digits == 3 && mv.digits == 0,
           "…each with exactly the precision its unit needs to separate two counts",
           "V:" + std::to_string(volts.digits) + "dp mV:" + std::to_string(mv.digits) + "dp");

        // ROUND TRIP: a value typed in volts lands on a COUNT, and reads back as the same volts.
        const double typedV = 2.5;
        const double stored = c.snapBin(t, 0, volts.toStorage(typedV));
        ck(std::fabs(stored - std::round(stored)) < 1e-9, "a value typed in volts still stores a count",
           num(stored));
        ck(std::fabs(volts.toDisplay(stored) - typedV) <= std::fabs(volts.toDisplay(1) - volts.toDisplay(0)),
           "…and reads back as the volts you typed, within one count",
           num(volts.toDisplay(stored)) + " V");

        // CLAMPING happens in counts, so it holds whatever unit you type in.
        ck(near(c.snapBin(t, 0, volts.toStorage(99.0)), 4095.0), "99 V clamps to the top count");
        ck(near(c.snapBin(t, 0, mv.toStorage(-500.0)), 0.0), "-500 mV clamps to the bottom count");

        // And an unrelated unit is refused rather than silently applied: an axis is not degrees.
        const Cache::AxisView bogus = c.axisView(t, 0, "C");
        ck(bogus.to.empty() && bogus.digits == 0 && near(bogus.hi, 4095),
           "a unit from another quantity is ignored, not applied");
    }

    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

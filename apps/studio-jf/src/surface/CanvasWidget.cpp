// CanvasWidget — out-of-line definitions of the shared skin/value-format render helpers that touch the
// model layer (Cache / MathEvaluator / UnitManager / RangeBands). Declared on the class in CanvasWidget.h;
// defined here so those heavy includes stay out of the widely-included base header. The bodies are the former
// the shared render helpers verbatim — pure relocation, no logic change. Colours resolve through the skin
// cascade (skin::, in WidgetSupport.h): value band -> local element prop -> scheme fallback.

#include "CanvasWidget.h"
#include <cmath>
#include "../model/ChannelPrefs.h"   // the shared per-channel unit + warning bands
#include "../model/Cache.h"
#include "../model/MathEvaluator.h"
#include "../model/UnitManager.h"
#include "../model/RangeBands.h"
#include "../model/EditorSettings.h"

#include <algorithm>
#include <cstdio>
#include <string>

// Reading order for a tab ring — see the declaration for why element order would not do.
std::vector<int> CanvasWidget::readingOrder(std::vector<TabSlot> slots) {
    // Sort down the page first, so walking the list once groups every row that follows.
    std::stable_sort(slots.begin(), slots.end(),
                     [](const TabSlot& a, const TabSlot& b) { return a.y < b.y; });
    std::vector<int> out;
    out.reserve(slots.size());
    for (size_t i = 0; i < slots.size();) {
        // The band belongs to the control that OPENED the row: anything whose top falls inside half its
        // height shares the row. Half, not the full height, so two genuinely stacked rows of the same
        // control never merge into one.
        const float top  = slots[i].y;
        const float band = std::max(4.f, slots[i].h * 0.5f);
        size_t j = i;
        while (j < slots.size() && slots[j].y - top <= band) ++j;
        std::stable_sort(slots.begin() + i, slots.begin() + j,
                         [](const TabSlot& a, const TabSlot& b) { return a.x < b.x; });
        for (size_t k = i; k < j; ++k) out.push_back(slots[k].id);
        i = j;
    }
    return out;
}

// A widget's data source is an EXPRESSION that resolves to a value — the SAME recipe as a visibility
// test. A bare channel name ("rpm") resolves through the evaluator as a channel (telemetry/config); a
// sigil expression ("[$rpm]*2", "[#a]+[@w.value]") evaluates fully. Empty → 0.
Raw CanvasWidget::evalSource(const std::string& expr) {
    return Raw{ expr.empty() ? 0.0 : MathEvaluator::instance().evaluate(expr) };
}

std::string CanvasWidget::writableConfigPath(const std::string& binding) {
    // Trim, then reduce the binding to the raw path a #config sigil (or a bare config name) points at.
    const size_t a = binding.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const std::string e = binding.substr(a, binding.find_last_not_of(" \t") - a + 1);
    std::string path;
    if (e.size() >= 4 && e.front() == '[' && e.back() == ']') {   // [<sigil><path>] — only '#' maps to a CONFIG write
        // $telemetry is read-only; @widget is read-only today but may gain its own write path (settings/config
        // unlocking widget state) — that would be a separate target, not a config path, so it stays out of here.
        if (e[1] != '#') return {};
        path = e.substr(2, e.size() - 3);                        // strip "[#" and "]" (the builder's writable-path form)
    } else {
        path = e;                                                // a bare token is writable only if it is itself a config path
    }
    return Cache::instance().isConfig(path) ? path : std::string{};
}

// The FIRST band holding the live bound value overrides a colour channel; a blinking band alternates
// override â base. Returns the override in `out`, else nullptr (fall through to the local/scheme skin).
// THE ELEMENT'S BINDING, RESOLVED. These helpers are static — they take an element, not a widget — so they
// cannot ask the instance for its bindPath(). They can ask the SCOPE: a widget paints and takes input inside
// its own MathEvaluator::ElementScope, which is what "[*]" means at that moment.
//
// Without this a templated page had no units at all: Cache::unit("sensors.sensor[*].diag_raw_min") names no
// field, so the source unit came back empty, dispV skipped the conversion and displayUnitOf had nothing to
// label with. A page showing ADC counts where the fixed page beside it showed volts — the same defect the
// tooltip had, in the unit path.
static std::string boundPathOf(const PanelElement& el) {
    return MathEvaluator::instance().resolveIndexed(
        MathEvaluator::resolveTemplate(el.prop("signalName"), MathEvaluator::elementContext()));
}

// THE CHANNEL'S OWN BANDING, as a colour — asked by every control that resolves a colour through the
// cascade, so a band set once against oil pressure turns the watch-list row, the readout and the gauge
// together and nothing has to be told twice.
//
// Two layers, in this order: the channel's RULES (expressions, the general mechanism — same compact form
// and same evaluation as a widget's own "ranges", so they carry blink and every colour channel), then the
// plain warning/alarm numbers, which are the quick way to write the common case and only speak for the
// foreground. A per-widget rule still beats both: that is the specific statement about one control.
const uint8_t* CanvasWidget::channelBandColor(const PanelElement& el, const std::string& key, uint8_t out[4]) {
    const std::string bind = boundPathOf(el);
    if (bind.empty()) return nullptr;
    const ChannelPref* cp = ChannelPrefs::instance().find(bind);
    if (!cp) return nullptr;
    const double v = dispV(el, evalSource(bind));   // bands are in DISPLAY units, like the number itself
    if (!cp->ranges.empty()) {
        for (const ColorRule& b : ColorRules::fromCompact(cp->ranges).rules) {
            if (b.when.empty()) continue;
            const std::string e = MathEvaluator::resolveTemplate(
                substValue(substUnits(b.when, displayUnitOf(el)), v), MathEvaluator::elementContext());
            if (MathEvaluator::instance().evaluate(e) == 0.0) continue;
            if (b.blink && !blinkPhase()) return nullptr;
            const std::string* c = key == "bgColor"     ? &b.bg
                                 : key == "fgColor"     ? &b.fg
                                 : key == "accentColor" ? &b.accent
                                 : key == "borderColor" ? &b.border : nullptr;
            if (!c) return nullptr;
            return skin::parseHex(*c, out) ? out : nullptr;   // first match wins, as everywhere else
        }
    }
    if (key != "fgColor") return nullptr;
    const int sev = cp->severity(v);
    if (sev == 0) return nullptr;
    const uint8_t* c = ChannelPrefs::colorFor(sev, nullptr);
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3];
    return out;
}

const uint8_t* CanvasWidget::rangeColor(const PanelElement& el, const std::string& key, uint8_t out[4]) {
    const std::string rs = el.prop("ranges");
    if (rs.empty()) return nullptr;
    const double v = dispV(el, evalSource(boundPathOf(el)));   // DISPLAY units: a rule means what is shown
    for (const ColorRule& b : ColorRules::fromCompact(rs).rules) {
        // The STATIC path (an element with no live widget behind it): `value` splices the element's own
        // evaluated binding -- the same number the instance path would hold -- and the element key stands
        // in for the widget context. A rule with no expression decides nothing and is skipped.
        if (b.when.empty()) continue;
        const std::string e = MathEvaluator::resolveTemplate(
            substValue(substUnits(b.when, displayUnitOf(el)), v), MathEvaluator::elementContext());
        if (MathEvaluator::instance().evaluate(e) == 0.0) continue;
        if (b.blink && !blinkPhase()) return nullptr;             // off phase â base colours
        const std::string* c = key == "bgColor"     ? &b.bg
                             : key == "fgColor"     ? &b.fg
                             : key == "accentColor" ? &b.accent
                             : key == "borderColor" ? &b.border : nullptr;
        if (!c) return nullptr;                                   // channel ranges don't cover
        return skin::parseHex(*c, out) ? out : nullptr;           // first match wins â no later bands
    }
    return nullptr;
}
// Range-aware colour resolve: value band â local element prop â scheme fallback. Every control in
// this file resolves its colours through here (it replaced the plain skin::color cascade).
const uint8_t* CanvasWidget::elColor(const PanelElement& el, const std::string& key, const uint8_t* fallback, uint8_t buf[4]) {
    if (const uint8_t* rc = rangeColor(el, key, buf)) return rc;
    // …then the CHANNEL's own banding, which every control reading that channel shares.
    if (const uint8_t* cb = channelBandColor(el, key, buf)) return cb;
    return skin::parseHex(el.prop(key), buf) ? buf : fallback;
}

// A value converts from its channel's SOURCE unit (the meta's) to the element's displayUnit pin
// ("Raw" = stay in the source unit; empty/"Auto" = the user's per-quantity preference). Write controls
// convert back with srcV() — the Cache always speaks source units. Ranges/min/max props are authored in
// source units and convert alongside the value, so gauge fractions stay correct for offset units (°F).
// THE UNIT OF A BINDING, whether it is a PATH or a SIGIL.
//
// Cache::unit() answers for a path -- a telemetry channel or a config field. A binding may instead be an
// EXPRESSION: "[$rpm]" names the same channel but is not a key, so the lookup missed and the widget was
// treated as having no units at all. Silently: no conversion at render, and the Display Unit picker
// offering only Auto and Raw for a channel that has four (RPM, rad/s, deg/s, RPS).
//
// The evaluator already resolves "[<sigil><name>]" to the owning resolver's metadata, so ask it second.
// Path first, because that is the common case and the cheaper lookup.
std::string CanvasWidget::bindingUnit(const std::string& bind) {
    if (bind.empty()) return {};
    // ASK BY CHANNEL NAME. A binding resolves to a SIGIL — "[$~]" on a sensor page becomes "[$hw_av1]" —
    // and Cache::unit() is keyed by plain name, so the sigil form silently answered "no units at all".
    // That cost the raw input readout both its unit and the precision the unit carries: it printed a
    // 0-5 V pin with no unit and no decimals instead of "3.824 V". channelDomain() and ChannelPrefs were
    // already given the bare name for the same reason; this was the one left asking with the brackets on.
    const std::string bare = bareChannelOf(bind);
    if (std::string u = Cache::instance().unit(bare); !u.empty()) return u;
    if (bare != bind)
        if (std::string u = Cache::instance().unit(bind); !u.empty()) return u;
    return MathEvaluator::instance().getUnit(bind);   // an expression, not a channel: it knows its own
}

std::string CanvasWidget::displayUnitOf(const PanelElement& el) {
    const std::string bind = boundPathOf(el);
    const std::string src = bindingUnit(bind);
    const std::string pin = el.prop("displayUnit");
    // An EXPLICIT pin stands even when the field declares no units of its own. The field being silent is
    // not a veto: some values only mean something once the page says what they are — a trigger stream's
    // cells are tooth indices under one primitive and 0.1° steps under another, which no single `units`
    // string in the meta could state. Nothing is converted here (there is no source unit to convert
    // FROM), so the pin is a label, which is exactly what those cases need.
    if (!pin.empty() && pin != "Auto" && pin != "Raw") return pin;
    if (src.empty()) return src;             // no source unit and no pin — nothing to say
    // RAW is the number as stored, shown as itself: no conversion, and NO UNIT. Naming the field's unit
    // beside an unconverted value is the one thing Raw is asked for instead of — it says "do not dress this
    // up", and a unit is the dressing.
    if (pin == "Raw") return {};
    // THE CHANNEL'S OWN PREFERENCE comes next: a unit set once against a channel is meant to hold wherever
    // that channel is shown, which is the whole reason it is stored per channel rather than per widget.
    // It sits BELOW an explicit pin (a page that insisted on a unit still gets it) and ABOVE the global
    // per-quantity preference, which is the coarser statement of the same idea.
    if (const ChannelPref* cp = ChannelPrefs::instance().find(bareChannelOf(bind)); cp && !cp->unit.empty()) return cp->unit;
    return UnitManager::instance().displayUnitFor(src);
}
// THE scale a widget applies. The Cache is raw now — this is the single raw<->engineering scale, done
// once here at the base for every widget (config values arrive raw and must be cooked; telemetry is
// already cooked, so configScale() is 1 for it).
//
// There WAS a per-widget "Scale" property layered on top of this, and it is gone. It was an override on
// a system that already knew the answer, and nothing used it: across a 16,211-element layout only
// fourteen elements carried the prop at all, twelve of those held the meaningless "0", and the two real
// values sat on unbound ruler widgets with no channel to scale. Meanwhile it was one more place a
// number's meaning could be quietly changed — the same trap the display FORMAT had, where a value
// stamped on the widget outranked the system that actually knew the field.
double CanvasWidget::scaleOf(const PanelElement& el) {
    const std::string bind = boundPathOf(el);
    return bind.empty() ? 1.0 : Cache::instance().configScale(bind);
}
std::string CanvasWidget::displayUnitLabelOf(const PanelElement& el) {
    return UnitManager::instance().unitLabel(displayUnitOf(el));
}

// RAW -> display: cook to engineering (× the one scale), then convert to the widget's display unit.
// First band that matches. A band with a `when` expression answers that instead of its interval: the
// expression is resolved against this widget's element (so "$*" is its own channel, "[*]" its key, and a
// widget sigil reaches any other control) and anything non-zero counts as inside. A band with no `when`
// keeps the plain interval, which is what every layout authored before this holds.
// `value` is THE WIDGET'S OWN READING, spliced in as a literal before the expression is evaluated.
// "$*" only resolves for a widget bound to a config ELEMENT; one bound to a plain telemetry channel has
// no such context, and a band is exactly the place you want to say "this control's number" without
// naming the channel twice (rename the binding and the band would otherwise still point at the old one).
//
// Whole-word only: `some.value.thing` and `myvalue` are paths and must survive untouched, so a match is
// rejected when the character on either side could continue an identifier or a path.
// UNIT-SUFFIXED LITERALS: "value > 5v", "value > 200ms", "value > .2s".
//
// A band is a sentence about a physical quantity, and the number in it is meaningless without saying what
// it is in. Written bare, a threshold silently changes meaning the moment the reading is displayed in
// another unit — the same "20" that meant 20 kPa now means 20 psi, and the band is wrong by a factor of
// seven with nothing on screen to say so. So a literal may carry its unit, and it is converted here into
// whatever unit the reading is currently in. "200ms" and ".2s" are then the same threshold, because they
// are the same quantity, which is the entire point.
//
// Only a suffix UnitManager actually knows is treated as one ("2e3" keeps its exponent, "5x" stays a
// syntax error). A literal in a unit of a DIFFERENT quantity from the reading keeps its number and loses
// its suffix: there is no conversion from volts to milliseconds, and inventing one would be worse than
// letting a rule that compares them plainly be wrong.
std::string CanvasWidget::substUnits(const std::string& expr, const std::string& displayUnit) {
    if (expr.empty()) return expr;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    auto identCh = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '%'; };
    std::string out;
    size_t i = 0;
    while (i < expr.size()) {
        // A number starts here only if the previous character cannot be part of an identifier — otherwise
        // it is the tail of a name ("egt_1"), not a literal.
        const bool start = (i == 0) || !(std::isalnum(static_cast<unsigned char>(expr[i - 1]))
                                         || expr[i - 1] == '_' || expr[i - 1] == '.');
        if (!start || !(digit(expr[i]) || (expr[i] == '.' && i + 1 < expr.size() && digit(expr[i + 1])))) {
            out += expr[i++];
            continue;
        }
        size_t j = i;
        while (j < expr.size() && (digit(expr[j]) || expr[j] == '.')) ++j;
        if (j < expr.size() && (expr[j] == 'e' || expr[j] == 'E')) {          // an exponent is part of the number
            size_t k = j + 1;
            if (k < expr.size() && (expr[k] == '+' || expr[k] == '-')) ++k;
            if (k < expr.size() && digit(expr[k])) { while (k < expr.size() && digit(expr[k])) ++k; j = k; }
        }
        size_t u = j;
        while (u < expr.size() && identCh(expr[u])) ++u;
        const std::string num = expr.substr(i, j - i);
        const std::string suf = expr.substr(j, u - j);
        std::string keep = num;
        if (!suf.empty()) {
            UnitManager& um = UnitManager::instance();
            const std::string q = um.findQuantityForUnit(suf);
            // Case-insensitively too: nobody types "V" when they mean volts and "ms" when they mean
            // milliseconds with the same care, and "5v" is not a different threshold from "5V".
            std::string unit = q.empty() ? std::string() : suf;
            if (unit.empty())
                for (const std::string& qid : um.quantities())
                    for (const auto& cand : um.getQuantity(qid).units) {
                        std::string a = cand.id, b = suf;
                        for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (a == b) { unit = cand.id; break; }
                    }
            if (!unit.empty() && !displayUnit.empty()
                && um.findQuantityForUnit(unit) == um.findQuantityForUnit(displayUnit)) {
                char lit[32];
                std::snprintf(lit, sizeof lit, "%.17g",
                              um.convert(std::atof(num.c_str()), unit, displayUnit));
                keep = lit;
                i = u;                       // the suffix is consumed: it has been folded into the number
                out += keep;
                continue;
            }
            if (!unit.empty()) { out += num; i = u; continue; }   // a unit we know but cannot convert: drop it
        }
        out += keep;
        i = j;
    }
    return out;
}

std::string CanvasWidget::substValue(const std::string& expr, double v) {
    static const std::string kw = "value";
    auto ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '@';
    };
    char lit[32];
    std::snprintf(lit, sizeof lit, "%.17g", v);
    std::string out;
    size_t p = 0;
    for (;;) {
        const size_t i = expr.find(kw, p);
        if (i == std::string::npos) { out += expr.substr(p); break; }
        const bool lhs = (i == 0) || !ident(expr[i - 1]);
        const size_t after = i + kw.size();
        const bool rhs = (after >= expr.size()) || !ident(expr[after]);
        out += expr.substr(p, i - p);
        out += (lhs && rhs) ? lit : kw;
        p = after;
    }
    return out;
}

// The first rule whose expression is true, else nullptr. `value` becomes what this widget SHOWS (display
// units -- see setValue), "$*"/"[*]" resolve against its element, a widget sigil reaches any other
// control, and anything non-zero applies.
const CanvasWidget::Range* CanvasWidget::activeRange() const {
    for (const Range& b : m_ranges) {
        if (b.when.empty()) continue;      // no expression decides nothing
        // The instance path, same rule: literals carry their unit and are folded into the one this
        // control is reading in, so a band survives the display unit being switched under it.
        const std::string e = resolveTemplate(
            substValue(substUnits(b.when, m_el ? displayUnitOf(*m_el) : std::string()), m_dispValue),
            m_elemContext);
        if (MathEvaluator::instance().evaluate(e) != 0.0) return &b;
    }
    return nullptr;
}

double CanvasWidget::dispV(const PanelElement& el, Raw v) {
    const double eng = cook(v, scaleOf(el)).v;
    const std::string bind = boundPathOf(el);
    const std::string src = bindingUnit(bind);
    const std::string dst = displayUnitOf(el);
    return (src.empty() || dst.empty() || src == dst) ? eng : UnitManager::instance().convert(eng, src, dst);
}
// display -> RAW (the write path): convert back to the source unit, then un-cook (÷ the one scale).
Raw CanvasWidget::srcV(const PanelElement& el, double v) {
    const std::string bind = boundPathOf(el);
    const std::string src = bindingUnit(bind);
    const std::string dst = displayUnitOf(el);
    const double eng = (src.empty() || dst.empty() || src == dst) ? v : UnitManager::instance().convert(v, dst, src);
    return uncook(Eng{eng}, scaleOf(el));
}

const uint8_t* CanvasWidget::fgOf(const PanelElement& el, uint8_t buf[4]) { return elColor(el, "fgColor", jf::Colors::TextPrimary, buf); }

// Value format: honour the printf-style format prop, but only if it carries a float conversion.
//
// AND THE UNIT COMES WITH IT. A number without its unit is not a reading — 98 is a temperature, a percent
// or a count depending on something the page does not say. It is appended here, at the one place a widget
// turns a value into text, so every readout carries it and none has to remember to. The unit shown is the
// one the value is IN (displayUnitOf: the widget's pin, else the quantity's preference, else the field's
// own), so it tracks a preference change with the number rather than contradicting it.
// A DIGITAL OUTPUT IS NOT A PERCENTAGE. out_<n> is one channel shared by all the output slots, so it
// has to be a float in percent — a PWM slot's duty needs it. A DIGITAL slot has two states, and the
// number it publishes is the command the sink was handed BEFORE the sink thresholded it: 60 means the
// pin is on, and 40 would mean it is off, in the same units, at the same width, looking identical.
//
// So a digital slot's readout says which state it is in. The threshold is the sink's own — the midpoint
// of the slot's clamp span (OutputManager::recompute_live) — read from the same config the firmware
// reads, so the screen and the pin cannot disagree.
//
// Returns false when the binding is not a digital output slot, which is every other readout in the app.
// NotAnOutput = every other readout in the app; Unavailable = this output cannot be driving anything.
enum class DigOut { NotAnOutput, Unavailable, Low, High };

static DigOut digitalOutState(const std::string& bind, double value) {
    // "[$out_7]" or a bare "out_7" — the telemetry channel one output slot publishes.
    std::string ch = bind;
    if (ch.size() >= 4 && ch.front() == '[' && ch[1] == '$' && ch.back() == ']')
        ch = ch.substr(2, ch.size() - 3);
    if (ch.compare(0, 4, "out_") != 0) return DigOut::NotAnOutput;
    const std::string digits = ch.substr(4);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
        return DigOut::NotAnOutput;
    const int slot = std::atoi(digits.c_str()) - 1;      // out_1 is output[0]
    if (slot < 0) return DigOut::NotAnOutput;

    Cache& C = Cache::instance();
    const std::string base = "outputs.output[" + std::to_string(slot) + "]";
    if (!C.isConfig(base + ".kind")) return DigOut::NotAnOutput;
    if (C.configValue(base + ".kind") != 1.0) return DigOut::NotAnOutput;   // 0 = PWM: show the duty
    // NOTHING TO REPORT. A row that is not a Generic output drives nothing this channel describes — so
    // LOW is not a reading of it, it is the absence of one, and the two look identical on screen. The
    // channel still carries a number (the expression is still evaluated), which is exactly why this
    // has to be decided from the CONFIG and not from the value.
    if (C.isConfig(base + ".function") && C.configValue(base + ".function") != 3.0)   // 3 = Generic
        return DigOut::Unavailable;
    // THE CHANNEL IS ALREADY THE STATE. The firmware publishes the level a digital slot drove — 0 or
    // 100, decided by the sink's own threshold (OutputManager) — so this does not re-derive that
    // decision from the clamps. Re-deriving it would be a second copy of the rule that disagrees the
    // moment a slot's clamp span is not centred on 50, and it would disagree about the pin.
    return value >= 50.0 ? DigOut::High : DigOut::Low;
}

// HOW MANY DECIMALS A READING DESERVES, when the page has not said. "%.1f" was the answer for every
// value on every page, which is wrong in both directions: it invents a decimal for a channel that only
// carries whole numbers, and it throws away resolution the moment a unit conversion makes the steps
// finer than a tenth. A raw pin read as volts is the extreme case — 0.0012 V per count, so one decimal
// collapses the whole 0..4095 range into five distinct readings.
//
// The rule is the one Cache::axisView already applies to a table's axes: ONE STORAGE STEP, converted to
// whatever unit is being shown, decides the precision. Two decimals is the answer for volts because a
// volt's step lands there — not because volts are special-cased. -1 = the channel says nothing, so the
// caller keeps its own default.
// ASK THE UNIT BEING SHOWN; only if it has no opinion does the channel answer. This used to derive a
// decimal count from one storage step converted into the display unit, which sounds principled and is
// not: a pin quantised to 1.2 mV asked for four decimals of volts, and nothing about the derivation
// knew that a tuner reads volts to three and millivolts to none. The format travels with the unit
// (UnitManager::Unit::format) -- so the SAME readout re-prints itself when the run-mode Units menu
// switches it between V, mV and counts, which a format stored on the widget can never do.
std::string CanvasWidget::autoFormat(const PanelElement& el) {
    const std::string bind = boundPathOf(el);
    if (bind.empty()) return {};
    if (std::string f = UnitManager::instance().unitFormat(displayUnitOf(el)); !f.empty()) return f;
    const int d = Cache::instance().channelDomain(bareChannelOf(bind)).digits;   // no unit: the channel
    return (d >= 0) ? ("%." + std::to_string(d) + "f") : std::string();
}

std::string CanvasWidget::fmtVal(const PanelElement& el, const Cache&) {
    const std::string bind = boundPathOf(el);
    switch (digitalOutState(bind, evalSource(bind).v)) {
        case DigOut::Unavailable: return "NA";
        case DigOut::Low:         return "LOW";
        case DigOut::High:        return "HI";
        case DigOut::NotAnOutput: break;
    }
    // A STATE CHANNEL HAS WORDS, and the number is not one of them. `idle_state` reads 3 on a page and
    // that means nothing to anybody; "Open Loop" is the whole reading. The meta has carried the value
    // set all along — MetaModel::enumOptions resolves a channel's named enum — and every other control
    // that meets one already asks it: the combo, the radio, the indicator. The readout was the one that
    // did not, so EVERY state channel in the application printed an index: idle, cruise, the engine
    // state, which base timing table the spark came from, the CAN controller's state.
    //
    // OUT OF RANGE FALLS THROUGH TO THE NUMBER rather than printing nothing or the wrong word. A value
    // the set does not name is a fault worth seeing, and the digit is the only honest thing to show for
    // it — as is a fractional reading, which is not an index at all.
    if (const MetaModel* mm = Cache::instance().meta(); mm && !bind.empty()) {
        const std::vector<std::string> names = mm->enumOptions(bareChannelOf(bind));
        if (!names.empty()) {
            const double ev = dispV(el, evalSource(bind));
            const long   i  = std::lround(ev);
            if (i >= 0 && i < static_cast<long>(names.size())
                && std::fabs(ev - static_cast<double>(i)) < 0.01)
                return names[static_cast<size_t>(i)];
        }
    }

    // The widget's own format if it states one, else the unit's. No magic constant at the end: "%.1f"
    // as a last resort is what printed a 0-5 V pin as "3.8V". A value whose unit and channel both
    // decline to state a precision has no decimals to show, not one.
    std::string f = el.prop("format");
    if (f.find('f') == std::string::npos && f.find('g') == std::string::npos && f.find('e') == std::string::npos) {
        f = autoFormat(el);
        if (f.empty()) f = "%.0f";
    }
    char v[64]; std::snprintf(v, sizeof(v), f.c_str(), dispV(el, evalSource(bind)));
    // The unit rides with the number unless the control says otherwise. ABSENT means show, so every layout
    // authored before this keeps its units; only an explicit "0" hides them — for a readout whose unit is
    // already said by the caption beside it, or a row of cells where repeating it eight times is noise.
    if (el.prop("showUnit") == "0") return v;
    const std::string u = displayUnitLabelOf(el);
    if (!u.empty()) return std::string(v) + " " + u;
    return v;
}

bool CanvasWidget::readingIsWord(const PanelElement& el) {
    const MetaModel* mm = Cache::instance().meta();
    if (!mm) return false;
    const std::string bind = boundPathOf(el);
    return !bind.empty() && !mm->enumOptions(bareChannelOf(bind)).empty();
}

// The element as text: role = what kind of control it is, name = what a person would call it, value =
// what it currently shows. Enough to find a control by name, read it back, and assert on it without a
// screenshot — which is the whole difference between driving this app and photographing it.
jf::JA11yNode CanvasWidget::a11yNode() const {
    jf::JA11yNode n;
    std::string name = m_el ? m_el->prop("labelText") : std::string();
    if (name.empty()) name = m_el ? m_el->prop("caption") : std::string();
    if (name.empty()) name = m_signalName;      // a bound control with no caption IS its binding
    if (name.empty()) name = elementType();
    // Through the SAME formatter every readout uses, so what the bus reports is what the screen says,
    // down to the rounding and the display unit.
    std::string val;
    if (m_el && !m_signalName.empty()) val = fmtVal(*m_el, Cache::instance());
    _a11yFillCommon(n, jf::JA11yRole::Widget, name, val);
    // The role the framework has no word for: a canvas element's type is the studio's own vocabulary
    // ("configedit", "table", "curve"), and it is what a client filters on.
    const std::string t = elementType();
    std::snprintf(n.role, sizeof n.role, "%s", t.c_str());
    return n;
}

// The bound field's Location, narrowed by this control's Min/Max. See the declaration.
//
// NARROWING ONLY, and deliberately: intersecting means a page can tighten a field but never loosen it,
// so a mis-typed Max cannot let a value through that the storage or the schema forbids. The expressions
// are in the control's DISPLAY unit — that is what the user typed and what the box shows — so they come
// back through srcV() to meet the Location, which is raw.
MetaModel::Location CanvasWidget::boundLoc() const {
    MetaModel::Location L;
    if (!m_el || !Cache::instance().meta())
        return L;
    const std::string bind = bindPath();
    if (bind.empty())
        return L;
    MathEvaluator::ElementScope scope(m_elemContext);   // "[*]" means this control's element
    L = Cache::instance().meta()->locate(bind);
    narrowToControl(L);
    return L;
}

void CanvasWidget::narrowToControl(MetaModel::Location &L) const {
    if (!m_el || !L.valid() || (m_minExpr.empty() && m_maxExpr.empty()))
        return;
    MathEvaluator::ElementScope scope(m_elemContext);
    // Start from what the FIELD allows — its declared range, or its datatype's when it declares none —
    // so an expression on one side alone still cannot widen the other.
    double lo = 0.0, hi = 0.0;
    Cache::rawBounds(L, lo, hi);
    // AN EXPRESSION IS RAW, like the bytes it reads. Cache::configValue returns readRaw() and the firmware
    // VM evaluates the same source against the same bytes, so "[#trigger.streams[0].width_min]" is the
    // stored count in both places — a control's Min/Max is therefore already in the unit rawBounds works in.
    //
    // It was passed through srcV, which converts DISPLAY to raw by dividing by the scale: on a scale-0.1
    // field a Min naming another field came out ten times too large, and the clamp with it. Invisible until
    // a scaled field was named in one, which is why it surfaced the day the WIDTH angles got their units.
    if (!m_minExpr.empty())
        lo = std::max(lo, MathEvaluator::instance().evaluate(m_minExpr));
    if (!m_maxExpr.empty())
        hi = std::min(hi, MathEvaluator::instance().evaluate(m_maxExpr));
    if (lo <= hi) { L.minV = lo; L.maxV = hi; }   // an inverted pair says nothing; leave the field's own
}

bool CanvasWidget::enabledNow() const {
    if (!m_el) return true;
    MathEvaluator::ElementScope scope(m_elemContext);   // "[*]" means THIS widget's element, everywhere
    // A SETTING THAT APPLIES ONLY AT AN ENGINE STOP is locked while the engine turns: the edit would sit in
    // the ECU's RAM doing nothing — a trigger stream switched off on a running engine kept its RPM — and
    // look exactly like one that had taken effect. Greyed out, and the tooltip says why (_syncTooltip).
    if (m_cache && m_cache->lockedWhileRunning(bindPath())) return false;
    const std::string gate = m_el->prop("enableCondition");
    return gate.empty() || MathEvaluator::instance().evaluate(gate) != 0.0;
}

bool CanvasWidget::visibleNow() const {
    if (!m_el) return true;
    MathEvaluator::ElementScope scope(m_elemContext);
    // A SWITCH HAS NO CALIBRATION. Not "an empty one" — the firmware builds no decode stage for a
    // switch at all (PipelineBuilder), so the curve is bytes nothing reads, and offering a curve
    // editor over them says the opposite: that the reading is yours to shape. On the analog interface
    // the same bytes ARE read, but as the two trip points, and those are edited on the wiring row
    // where they describe the connection — a curve drawn over them would quietly break the switch.
    //
    // It is answered here, on the ELEMENT: the calibration is a curve, an axis, a caption and whatever else
    // a page puts around it, and they should appear and disappear together — every element bound to it is
    // asked. Each widget says which kinds it shows for (showsCalibrationOf); the curve's answer is "an
    // ordinary calibration", the band editor's "a multi-position switch's". An authored `condition` still
    // applies on top, because this narrows what is shown and never widens it.
    const std::string calType = _calibrationTypeOf();
    if (!calType.empty() && !showsCalibrationOf(calType)) return false;
    const std::string cond = m_el->prop("condition");
    return cond.empty() || MathEvaluator::instance().evaluate(cond) != 0.0;
}

// The type of the sensor whose CALIBRATION this element binds, or "". typeScaledCells answers both
// halves — a calibration is the one element table whose cells are in the owning sensor's units — and it
// resolves the type the same way the firmware does, catalog first and the tune's byte only for a generic
// input.
std::string CanvasWidget::_calibrationTypeOf() const {
    if (!m_el) return {};
    // NOT writableConfigPath: that one ends on isConfig(), which answers for a scalar and not for a
    // TABLE — so it hands back nothing for the very binding this asks about. boundPathOf resolves the
    // template and the "[*]" element index the same way the paint does; the sigil wrapper is all that
    // has to come off.
    std::string path = boundPathOf(*m_el);
    if (path.size() >= 4 && path.front() == '[' && path[1] == '#' && path.back() == ']')
        path = path.substr(2, path.size() - 3);
    if (path.empty()) return {};
    const MetaModel::SensorType* st = Cache::instance().typeScaledCells(path);
    return st ? st->id : std::string();
}

// The bound field's HELP as this widget's tooltip — see the declaration for why.
void CanvasWidget::_syncTooltip() {
    if (!m_el) return;
    std::string tip = m_el->prop("tooltip");
    // A BIG CANVAS IS NOT A CONTROL. A table, a curve, a strip, a trace, a trigger diagram or a
    // viewport covers most of the page, and hovering anywhere inside one is not a question about the
    // field it is bound to — the page already says what the map is, in its title and the note beside
    // it. Answering it as a hover tip meant a paragraph about volumetric efficiency over every cell
    // of the VE table, again each time the pointer paused. An explicit `tooltip` prop still wins:
    // this suppresses the DERIVED help, not an author's deliberate one.
    if (tip.empty()) {
        const std::string t = elementType();
        if (t == "table" || t == "curve" || t == "array1d" || t == "viewport" ||
            t == "triggerdiagram" || t == "livegraph" || t == "graph" || t == "panel") {
            if (!tooltip().empty()) setTooltip({});
            return;
        }
    }
    // Own binding first, then the group's. The fallback is what gives a CAPTION something to say: it binds
    // nothing itself, so without it the words next to a control answer nothing on hover while the 36px box
    // beside them answers everything — backwards, and the reason a caption used to be handed a frozen copy
    // of the text instead.
    if (tip.empty() && m_cache) {
        // THE RESOLVED PATH. helpFor() looks a field up by name, and a template page's binding is
        // "trigger.streams[*].slots" — a path no field has. Every control on a templated page therefore
        // had an empty tooltip while the identical control on a fixed page explained itself, which reads
        // as the definition having no help rather than as the lookup asking for the wrong thing.
        const std::string bind = bindPath();
        tip = m_cache->help(bind.empty() ? resolveTemplate(m_helpBind, m_elemContext) : bind);
    }
    // WHEN IT TAKES EFFECT, said on the control itself — the reason a greyed one cannot be changed, and
    // the warning on one that can be changed but will not act until a restart.
    if (m_cache && m_cache->meta()) {
        const std::string at = m_cache->meta()->appliesAt(bindPath());
        // Only while it EXPLAINS something: an engine-stop setting says so while it is locked, not on every
        // trigger and engine control the rest of the time.
        const std::string why = (at == "engine_stop" && m_cache->engineTurning())
                                    ? "Applies when the engine stops \xE2\x80\x94 stop the engine to change it."
                              : at == "reboot" ? "Applies at the next restart." : std::string();
        if (!why.empty()) tip = tip.empty() ? why : why + "\n\n" + tip;
    }
    if (tip != tooltip()) setTooltip(tip);
}

// Run-mode focus ring: the element's own "focusRing" choice (1 = On, 2 = Off) if set, else the
// Preferences▸Globals default (stored table-namespaced by that page's builder; default On).
bool CanvasWidget::focusRingEnabled(const PanelElement& el) {
    const std::string s = el.prop("focusRing");
    if (s == "1") return true;
    if (s == "2") return false;
    return EditorSettings::instance().propGlobal("table.focusRing", 1) != 2;   // "global" -> On unless set Off
}

#include "TsDashboardConvert.h"

#include <j/core/JStyle.h>

#include "Cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <sstream>

namespace tsconvert {
namespace {

// Strip TunerStudio's keyboard-mnemonic marker from a menu label: "&Setup" is Alt+S in TS, and we have no
// such shortcut, so the ampersand is noise in a navigation tree.
//
// Only a LEADING '&' before a letter is a mnemonic. Ampersands appear literally elsewhere in these labels
// — "Delay & Dwell", "VVT Configuration & PID", "Useful in Research&Development phase" — and stripping
// those would quietly corrupt the text. "&&" is the escape for a literal one.
std::string stripMnemonic(std::string label) {
    if (label.size() >= 2 && label[0] == '&' && label[1] != '&' &&
        (std::isalnum(static_cast<unsigned char>(label[1])) || label[1] == '_'))
        label.erase(0, 1);
    // Collapse any escaped ampersand to the character it stands for.
    for (size_t i = 0; i + 1 < label.size(); ++i)
        if (label[i] == '&' && label[i + 1] == '&') label.erase(i, 1);
    return label;
}

// TunerStudio marks a help row by prefixing its label with '#' (a heading) or '!' (a warning/note):
//     field = "#The bias table controls how much of the adder table"
//     field = "!Reminder that 4-stroke cycle is 720 degrees"
// Both are rendering hints, not part of the text, so neither may survive into the caption. 169 of the
// jayecu ini's help rows carry one.
// THE TEXT WITHOUT ITS MARKUP. An ini's help and caption strings are written for a renderer that reads
// HTML, so they carry <br> where they mean a new line and the odd entity where they mean a character.
// Passed through, the reader sees the tag: "Fuel amount calculation logic<br>" is what the help box
// showed. A <br> IS a line break, so it becomes one; the rest of the tags are dropped and the handful of
// entities that appear are decoded. A bare link is a different case and stays with htmlLink below, which
// turns it into a hyperlink element rather than text.
std::string plainText(std::string t) {
    auto lower = [](std::string v) { for (char& c : v) c = char(std::tolower(uint8_t(c))); return v; };
    // <br>, <br/>, <br /> and any case of them: a line break, because that is what it was written to mean.
    for (size_t i = 0; (i = lower(t).find("<br", i)) != std::string::npos;) {
        const size_t e = t.find('>', i);
        if (e == std::string::npos) break;
        t.replace(i, e - i + 1, "\n");
        ++i;
    }
    // Everything else in angle brackets is presentation we have no use for (<html>, <b>, <font …>).
    for (size_t i = 0; (i = t.find('<', i)) != std::string::npos;) {
        const size_t e = t.find('>', i);
        if (e == std::string::npos) break;
        t.erase(i, e - i + 1);
    }
    static const std::pair<const char*, const char*> kEntities[] = {
        { "&nbsp;", " " }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
        { "&apos;", "'" }, { "&amp;", "&" },        // last: decoding it first would re-decode the others
    };
    for (const auto& [from, to] : kEntities)
        for (size_t i = 0; (i = t.find(from, i)) != std::string::npos; i += std::strlen(to))
            t.replace(i, std::strlen(from), to);
    return t;
}

std::string headerText(std::string label) {
    if (!label.empty() && (label[0] == '#' || label[0] == '!')) label.erase(0, 1);
    return plainText(std::move(label));
}

// A few help rows are HTML — always the same shape, a bare link:
//     field = "!<html><a href=https://example.com/help>https://example.com/help</a></html>"
// TunerStudio renders the link; showing the markup verbatim is the one thing that is certainly wrong. The
// href becomes a hyperlink element (there is a widget for exactly this) and the tags are dropped.
bool htmlLink(const std::string& text, std::string& url, std::string& caption) {
    const size_t a = text.find("<a href=");
    if (a == std::string::npos) return false;
    const size_t us = a + 8;
    const size_t ue = text.find('>', us);
    if (ue == std::string::npos) return false;
    url = text.substr(us, ue - us);
    if (!url.empty() && (url.front() == '"' || url.front() == '\'')) url = url.substr(1, url.size() - 2);
    const size_t ce = text.find("</a>", ue);
    caption = ce == std::string::npos ? url : text.substr(ue + 1, ce - ue - 1);
    if (caption.empty()) caption = url;
    return !url.empty();
}

// A binding as the native model spells it. A [Constants] field imports under the "ts." module; a
// PcVariable under "pc."; a telemetry channel keeps its bare name.
std::string rewriteExpr(const std::string& expr, const MetaModel& meta, const tsdash::Dashboard& dash);

std::string bindingFor(const std::string& var, const MetaModel& meta, const tsdash::Dashboard& dash) {
    if (var.empty()) return {};
    if (meta.config().count("ts." + var)) return "ts." + var;
    if (meta.config().count("pc." + var)) return "pc." + var;
    if (meta.telemetry().count(var))      return var;
    // A COMPUTED channel has no storage anywhere, so the binding IS its expression — the studio evaluates a
    // binding as an expression, so a gauge on "coolantTemperature" reads the same number TunerStudio shows
    // instead of resolving to nothing.
    if (const auto e = dash.derived.find(var); e != dash.derived.end())
        return rewriteExpr(e->second, meta, dash);
    return {};                                   // unresolved: the report already counted it
}

// TunerStudio writes "cond ? a : b"; the studio's evaluator has no ternary but does have select(c, a, b).
// Rewrites the OUTERMOST ternary and recurses, so a nested one converts too.
std::string ternaryToSelect(const std::string& e) {
    int depth = 0;
    size_t q = std::string::npos;
    for (size_t i = 0; i < e.size(); ++i) {
        if (e[i] == '(') ++depth;
        else if (e[i] == ')') --depth;
        else if (e[i] == '?' && depth == 0) { q = i; break; }
    }
    if (q == std::string::npos) return e;
    depth = 0;
    size_t c = std::string::npos;
    for (size_t i = q + 1; i < e.size(); ++i) {
        if (e[i] == '(') ++depth;
        else if (e[i] == ')') --depth;
        else if (e[i] == '?' && depth == 0) return e;        // nested ternary in the true branch: leave it
        else if (e[i] == ':' && depth == 0) { c = i; break; }
    }
    if (c == std::string::npos) return e;
    return "select(" + e.substr(0, q) + "," + e.substr(q + 1, c - q - 1) + "," +
           ternaryToSelect(e.substr(c + 1)) + ")";
}

// A TunerStudio condition names ini variables — "fanOnTemp > 0" — while the studio evaluates MODEL paths
// ("ts.fanOnTemp"). Rewrite every identifier that resolves to a
// binding and leave everything else (numbers, operators, unknown names) exactly as written. Without this an
// imported condition silently evaluates to 0, so every guarded row reads as permanently off.
std::string rewriteCond(const std::string& expr, const MetaModel& meta, const tsdash::Dashboard& dash) {
    std::string out;
    out.reserve(expr.size());
    for (size_t i = 0; i < expr.size(); ) {
        if (std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_') {
            size_t j = i;
            while (j < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[j])) || expr[j] == '_')) ++j;
            const std::string id = expr.substr(i, j - i);
            const std::string b  = bindingFor(id, meta, dash);
            out += b.empty() ? id : b;
            i = j;
        } else {
            out += expr[i++];
        }
    }
    return out;
}

// A computed channel's expression, in the studio's spelling: ternaries as select(), identifiers as paths.
std::string rewriteExpr(const std::string& expr, const MetaModel& meta, const tsdash::Dashboard& dash) {
    return rewriteCond(ternaryToSelect(expr), meta, dash);
}

// TunerStudio names its lamp colours ("white, black, red, black"). Map the ones an ini actually uses; an
// unknown name means "leave the widget's own default", which is better than guessing a colour.
std::string colourHex(const std::string& name) {
    static const std::map<std::string, std::string> kMap = {
        { "white", "#ffffff" }, { "black", "#000000" }, { "red", "#d64545" }, { "green", "#2ea043" },
        { "yellow", "#e3b341" }, { "blue", "#4184e4" }, { "orange", "#e08c3b" }, { "grey", "#808080" },
        { "gray",  "#808080" }, { "cyan", "#39c5cf" }, { "magenta", "#c74ded" },
    };
    std::string k;
    for (char c : name) k += char(std::tolower(static_cast<unsigned char>(c)));
    const auto it = kMap.find(k);
    return it == kMap.end() ? std::string() : it->second;
}

// A TunerStudio STRING EXPRESSION used as a label — spec §23, "string expressions on indicator text":
//
//     indicator = { faultCode }, "No faults", {Fault: bitStringValue(faultNames, faultCode)}, …
//
// The braced form is literal text with bitStringValue() substituted at runtime: a bit-typed channel's
// value names, indexed by an expression. Carried across as a prefix plus the two things the lookup needs,
// so the lamp reads "Fault: TPS error" the way TunerStudio draws it. Returns false for anything else —
// a caller then falls back to the static label rather than painting the expression itself, which is how
// "ETB OK" came to render as the middle of its own source text.
bool parseStringExpr(const std::string& raw, std::string& prefix, std::string& list, std::string& index) {
    using tsdash::trim;
    std::string t = trim(raw);
    if (t.size() < 2 || t.front() != '{' || t.back() != '}') return false;
    t = trim(t.substr(1, t.size() - 2));
    const size_t fn = t.find("bitStringValue");
    if (fn == std::string::npos) return false;
    const size_t open = t.find('(', fn), comma = t.find(',', open), close = t.find(')', comma);
    if (open == std::string::npos || comma == std::string::npos || close == std::string::npos) return false;
    prefix = trim(t.substr(0, fn));
    if (!prefix.empty() && prefix.back() == ':') prefix.pop_back();      // "Error:" → "Error", spacing below
    prefix = trim(prefix);
    list   = trim(t.substr(open + 1, comma - open - 1));
    index  = trim(t.substr(comma + 1, close - comma - 1));
    return !list.empty() && !index.empty();
}

int nextId = 1;

jf::JJson element(const std::string& type, float x, float y, float w, float h) {
    jf::JJson e = jf::JJson::object();
    // Every converted element gets a UID. It is the identity everything else addresses a widget by — the
    // @-sigil, and the run-mode menus that resolve "the table under the cursor" — and without one an
    // imported control could not be named at all. A panel's children are not elements of any model, so the
    // uid is the ONLY handle they have.
    e["uid"]     = "ts" + std::to_string(nextId);
    e["id"]      = double(nextId++);
    e["type"]    = type;
    e["x"]       = double(x); e["y"] = double(y);
    e["w"]       = double(w); e["h"] = double(h);
    e["groupId"] = 0.0;
    e["props"]   = jf::JJson::object();
    return e;
}

void prop(jf::JJson& e, const std::string& k, const std::string& v) {
    if (!v.empty()) e["props"][k] = v;
}

// A dialog becomes a PANEL, and its items become that panel's children.
//
// The first version flattened everything into one long list of rows with heading labels. That threw away
// structure the app can represent perfectly well: a PanelWidget is a real container with the same seven
// managed layout modes the canvas has, and TS's dialog layouts name the same ideas —
//
//     yAxis  -> Y Axis     xAxis -> X Axis     border -> Border     indicatorPanel -> Grid(columns)
//
// so a nested dialog is a nested panel, arranged by its declared mode, exactly as TunerStudio drew it.
// A control carries its own caption (labelText) rather than having a separate label element beside it,
// which is both what the old Qt importer did and what a managed layout expects.
enum PanelLayout { Free = 0, YAxis = 1, XAxis = 2, Border = 3, Card = 4, IndexCard = 5, Grid = 6, Wrap = 7,
                   Column = 8 };

int layoutOf(const std::string& tsLayout) {
    if (tsLayout == "xAxis")  return XAxis;
    if (tsLayout == "border") return Border;
    // COLUMN, not Y Axis. Both stack fields down the dialog; Y Axis then divides the container's height
    // BETWEEN them, so the same page reads differently depending on how many rows it happens to have —
    // an Ignition Key dialog of two rows inflated until its warning banner filled a third of the screen,
    // while a Trigger Gap dialog of thirty squashed until its labels were unreadable. A field row has a
    // height because it is a control. Column keeps it and lets the page scroll.
    return Column;                      // yAxis and anything unstated
}

// THE GUTTER A STATUS LAMP SITS IN. A lamp fills its whole rect with the state colour, so lamps packed
// edge to edge merge: four green ones in a column become one green block, and two red ones side by side
// one red bar, with nothing to say how many faults that is. Every lamp is inset by this, wherever it is
// laid out — the front-page strip and an indicatorPanel inside a dialog are the same thing in TunerStudio
// and should not look like two different things here.
// The widest a converted dialog may grow to. Wide enough for the widest real dialog in the wild, and
// bounded so a pathological one cannot make a page scroll sideways for ever.
static constexpr float kMaxDialogW = 1256.f;
// How tall an editor may ask to be. Past this a map is taller than any window it will open in, and the
// grid scrolls - which it does well - rather than the window opening off the bottom of the screen.
static constexpr float kMaxEditorH = 620.f;

// HOW FAR A BANNER'S TEXT SITS IN FROM ITS OWN EDGE. A caption on the page background can sit almost
// flush and nobody sees it, because there is no edge to sit against; reverse it out of a solid red or
// blue bar and the same 3 px reads as text about to fall off the left of the bar. The bar is the reason
// for the inset, so the inset belongs to the bar.
static const std::string kBannerPad = "8";

static constexpr float kLampPad = 3.f;
// What a panel keeps for itself before handing the rest of its rect to a child.
static constexpr float kPanelInset = 4.f;

struct Converter {
    const tsdash::Dashboard& dash;
    const MetaModel&         meta;
    const Layout&            lay;
    Report&                  rep;

    // A SPACER: one row of nothing. The caption is set EXPLICITLY empty rather than left unset, because
    // prop() skips an empty value and a label with no labelText falls back to its own default — which is
    // the word "Label", printed in the gap where the author wanted silence.
    jf::JJson spacer(float w) {
        jf::JJson e = element("label", 0.f, 0.f, w, lay.rowH);
        e["props"]["labelText"] = std::string();
        ++rep.widgets;
        return e;
    }

    jf::JJson control(const std::string& type, const std::string& label, const std::string& binding,
                      const std::string& condition, float w, float h, const std::string& region = {}) {
        jf::JJson e = element(type, 0.f, 0.f, w, h);   // a managed layout places it; the extent is what counts
        prop(e, "labelText", label);
        prop(e, "signalName", binding);
        // An item's {expression} is TunerStudio's ENABLE gate for every KIND of control, not just fields: a
        // bench-test page's "Spark #2..#12" buttons are greyed out when the cylinder count rules them out,
        // and importing that as a visibility rule left a column of holes where TS shows disabled buttons.
        prop(e, "enableCondition", rewriteCond(condition, meta, dash));
        // TS's per-item placement (North/South/East/West/Center) IS a Border-layout child's region, so a
        // dialog declared `border` lands the way TunerStudio drew it rather than as an arbitrary stack.
        if (region != "Center") prop(e, "region", region);
        ++rep.widgets;
        return e;
    }

    // A GAUGE here is multi-part, as it is when you draw one by hand: the dial (bezel, track, value fill),
    // the SCALE over it (ticks and their numbers), and the needle on top — all centred and the same size.
    // A lone dial reads as a ring with nothing in it, and a dial with a needle still says nothing about
    // what the needle is pointing AT; TunerStudio's gauge is marked, and so should an imported one be.
    //
    // The pair travels as a frameless panel so it stays ONE child wherever it lands: a managed layout would
    // otherwise place the dial and the needle side by side instead of one on the other.
    // TunerStudio's face colours: amber for the warn spans, red for the danger ones.
    static constexpr const char* kZoneWarn   = "#FFC107";
    static constexpr const char* kZoneDanger = "#E53935";

    jf::JJson gaugeAssembly(const std::string& title, const std::string& binding,
                            const std::string& condition, float size,
                            const tsdash::GaugeDef* gd = nullptr) {
        jf::JJson p = element("panel", 0.f, 0.f, size, size);
        p["props"]["layoutMode"] = std::to_string(int(Free));
        p["props"]["showBorder"] = "0";                       // a grouping container, not a box
        prop(p, "enableCondition", rewriteCond(condition, meta, dash));
        ++rep.widgets;
        jf::JJson kids  = jf::JJson::array();
        jf::JJson dial   = control("dial",   title, binding, {}, size, size);
        jf::JJson scale  = control("scale",  {},    binding, {}, size, size);
        // The marks have to span the CHANNEL's range to mean anything; the widget's own default is 0..100,
        // which would put a coolant gauge's needle somewhere arbitrary on somebody else's scale.
        // The DEFINITION's range first — an RPM gauge sweeps 0..9000 because [GaugeConfigurations] says so,
        // and the channel's storage limits (0..65535 for a U16) put the needle nowhere near where TS has it.
        // A bound may be an expression ({rpmHardLimit + 2000}), which nothing can evaluate at import time;
        // those fall back to the channel's own range, as before.
        double lo = 0, hi = 0;
        std::string loExpr, hiExpr;               // set when the definition states its bounds as expressions
        auto stripBraces = [](std::string v) {
            v = tsdash::trim(v);
            if (v.size() >= 2 && v.front() == '{' && v.back() == '}') v = tsdash::trim(v.substr(1, v.size() - 2));
            return v;
        };
        auto asNum = [](const std::string& s, double& out) {
            if (s.empty()) return false;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            while (end && *end == ' ') ++end;
            if (!end || *end != '\0') return false;
            out = v; return true;
        };
        double glo = 0, ghi = 0;
        // An expression bound ("{ useMetricOnInterface ? 140 : 284 }") is carried across AS an expression
        // and evaluated live by the widgets, so a gauge that scales with the unit setting keeps doing it.
        // Only a definition that states no bounds at all falls through to the channel's own range.
        if (gd && !gd->lo.empty() && !gd->hi.empty() && !(asNum(gd->lo, glo) && asNum(gd->hi, ghi))) {
            loExpr = rewriteExpr(stripBraces(gd->lo), meta, dash);
            hiExpr = rewriteExpr(stripBraces(gd->hi), meta, dash);
        }
        // A [GaugeConfigurations] bound is stated in DISPLAY units, while everything a widget reads is raw
        // and cooked with the binding's scale — which is why the meta-derived bounds below need no
        // conversion and these do. Divide once, here, where the scale is known: a dial told "0..5" for a
        // field stored at 0.005 V per count would otherwise sweep a two-hundredth of its real range.
        // ...BUT ONLY FOR A CONFIG BINDING. A config field is stored raw and cooked by the widget, so a
        // bound stated in display units has to be un-cooked to match. A TELEMETRY channel is not: the Cache
        // decodes it as `word * scale` and hands out engineering units already, and scaleOf() knows that —
        // it multiplies by the CONFIG scale, which is 1 for a channel. Dividing those bounds anyway put the
        // whole gauge in a space nothing else used: a -30..30 % gauge came out -0.3..0.3, its zones with it,
        // and its tick labels printed "-0" and "0" because that is what -0.3 is to the nearest whole number.
        double bsc = 1.0;
        if (meta.telemetry().count(binding) == 0)
            if (const auto c = meta.config().find(binding); c != meta.config().end()) bsc = c->second.scale;
        if (bsc == 0.0) bsc = 1.0;
        // The same for an EXPRESSION bound: it evaluates to display units, so divide the whole of it.
        if (bsc != 1.0 && !loExpr.empty() && !hiExpr.empty()) {
            char d[32]; std::snprintf(d, sizeof d, "%.10g", bsc);
            loExpr = "(" + loExpr + ")/" + d;
            hiExpr = "(" + hiExpr + ")/" + d;
        }
        if (gd && asNum(gd->lo, glo) && asNum(gd->hi, ghi) && ghi > glo) {
            lo = glo / bsc; hi = ghi / bsc;
        } else if (const auto t = meta.telemetry().find(binding); t != meta.telemetry().end() && t->second.maxV > t->second.minV) {
            lo = t->second.minV; hi = t->second.maxV;
        } else if (const auto c = meta.config().find(binding); c != meta.config().end() && c->second.maxV > c->second.minV) {
            lo = c->second.minV; hi = c->second.maxV;
        }
        // THE COLOURED ARCS, from the four bounds the definition states beside the range. TunerStudio
        // paints below loDanger and above hiDanger red, and loDanger..loWarn and hiWarn..hiDanger amber:
        // a tacho's redline, a coolant gauge's overheat. Emitted onto the DIAL, which paints them into the
        // face, and onto the SCALE, which puts the numbers over them in the same colour.
        //
        // GATED ON loWarn < hiWarn, because that is the normal band and a definition that states no bands
        // collapses it. 105 of this ini's 344 gauges are written `0,3, 0,3, 0,3` — the bounds repeated,
        // which reads as "low warn spans the whole scale AND high warn spans the whole scale". Taken at
        // face value that paints the entire face amber; what it means is that nobody set them, and
        // TunerStudio shows no bands there either.
        //
        // In RAW units, like every other bound here: the definition states display units and a widget
        // reads through the binding's scale.
        std::string zonesStr;
        if (gd) {
            // A BOUND HERE MAY BE AN EXPRESSION, like the range above it — and on the gauges where bands
            // matter most it always is: coolant's overheat is `{ useMetricOnInterface ? 95 : 203 }`, a
            // redline is `{ rpmHardLimit }`. Numbers are written as numbers; anything else is carried
            // across as the expression it is and evaluated live by the widgets, so a band moves with the
            // setting it was written against instead of being frozen at import.
            auto bound = [&](const std::string& raw) {
                double n;
                if (asNum(raw, n)) {
                    char b[32]; std::snprintf(b, sizeof b, "%.10g", n / bsc);
                    return std::string(b);
                }
                std::string e = rewriteExpr(stripBraces(raw), meta, dash);
                if (e.empty()) return std::string();
                if (bsc != 1.0) { char d[32]; std::snprintf(d, sizeof d, "%.10g", bsc); e = "(" + e + ")/" + d; }
                return e;
            };
            // ESCAPED, because an expression is full of the commas and semicolons this format splits on.
            auto esc = [](const std::string& t) {
                std::string o;
                for (char c : t) { if (c == '\\' || c == ',' || c == ';') o += '\\'; o += c; }
                return o;
            };
            // THE DEGENERACY TEST, where it can be made. 105 of this ini's gauges repeat their bounds
            // (`0,3, 0,3, 0,3`), which literally says both warn spans cover the whole scale and actually
            // says nobody set them — TunerStudio shows no bands there. That is decidable only when all
            // four are numbers; a definition that went to the trouble of writing an expression meant it.
            double zlo, zhi, dlo, wlo, whi, dhi;
            const bool allNum = asNum(gd->lo, zlo) && asNum(gd->hi, zhi) &&
                                asNum(gd->loDanger, dlo) && asNum(gd->loWarn, wlo) &&
                                asNum(gd->hiWarn, whi) && asNum(gd->hiDanger, dhi);
            const bool anyExpr = !asNum(gd->loDanger, dlo) || !asNum(gd->loWarn, wlo) ||
                                 !asNum(gd->hiWarn, whi) || !asNum(gd->hiDanger, dhi);
            if ((allNum && zhi > zlo && wlo < whi) || anyExpr) {
                auto band = [&](const std::string& a, const std::string& b, const char* colour) {
                    const std::string x = bound(a), y = bound(b);
                    if (x.empty() || y.empty() || x == y) return;   // a bound on top of its neighbour is no band
                    zonesStr += (zonesStr.empty() ? "" : ";") + esc(x) + "," + esc(y) + "," + colour;
                };
                band(gd->lo,       gd->loDanger, kZoneDanger);
                band(gd->loDanger, gd->loWarn,   kZoneWarn);
                band(gd->hiWarn,   gd->hiDanger, kZoneWarn);
                band(gd->hiDanger, gd->hi,       kZoneDanger);
            }
        }
        jf::JJson needle = control("needle", {},    binding, {}, size, size);
        // RANGE AND SWEEP ON ALL THREE PARTS, EXPLICITLY. They are separate widgets stacked on one another
        // and each reads its own properties: the needle defaults to 0..100 and was never given the range,
        // so it swept a scale of its own invention while the dial fill and the tick labels used the real
        // one — on an RPM gauge, a needle pegged at 100 rpm against marks running to 9000.
        //
        // The angles are stated rather than left at the widgets' 0/0 "use the default" fallback: three
        // widgets agreeing by coincidence is not the same as three widgets told the same thing, and a
        // gauge opened in the editor should show the sweep it actually draws.
        for (jf::JJson* e : { &dial, &scale, &needle }) {
            if (!loExpr.empty() && !hiExpr.empty()) {          // the definition's own bounds, evaluated live
                prop(*e, "minValue", loExpr);
                prop(*e, "maxValue", hiExpr);
            } else if (hi > lo) {
                char b[64];
                std::snprintf(b, sizeof b, "%g", lo); prop(*e, "minValue", b);
                std::snprintf(b, sizeof b, "%g", hi); prop(*e, "maxValue", b);
            }
            prop(*e, "startAngle", "225");    // lower-left, sweeping clockwise
            prop(*e, "endAngle",   "-45");    // to lower-right: the 270-degree arc TunerStudio draws
        }
        // THE DIAL IS THE FACE HERE, NOT A FILL GAUGE. This dial carries a scale and a needle on top of
        // it, so the ring in the dial colour and the arc that fills to the value are both a solid disc
        // painted over the very things that carry the reading: ticks and numbers on one flat colour, and
        // the zones buried under it. Off, so what is left of the ring is the zones.
        prop(dial, "showDialBand", "0");
        prop(dial, "showFill",     "0");
        // A THIN RIM, NOT A THICK RING. With the band gone the ring holds nothing but the zones, and at
        // the widget's default inner radius of 0.8 they are a fifth of the face deep — a slab of colour
        // behind the numbers rather than a marked span beside them. The zones take all of what is left,
        // since there is no value fill underneath them to share the ring with.
        prop(dial, "innerRadius",  "0.92");
        prop(dial, "zoneWidth",    "1");
        // A HAIRLINE BEZEL. At the widget's default the ring is thick enough to eat the outer end of every
        // tick, which is where the scale now draws — the reason the scale used to be inset away from it.
        prop(dial, "bezelWidth",   "0.005");
        if (!zonesStr.empty()) { prop(dial, "zones", zonesStr); prop(scale, "zones", zonesStr); }

        // CONCENTRIC, NOT COINCIDENT. Same binding and same arc — stated above, not assumed — but the
        // scale sits a little inside the dial so its numbers ring the zone rim rather than crossing it.
        //
        // AS FRACTIONS OF THE ASSEMBLY, not the flat 200 and 180 they are authored as. This square is
        // min(panel width, gaugeW), so it is 240 where there is room and a great deal less where there is
        // not — eight of this ini's gauges land in readout panels barely 51 px wide. Fixed pixels would
        // put a 200 px dial in a 51 px hole; a ratio keeps the proportion at every size, and at the 240
        // default it IS 200 and 180.
        const float dialSz  = size * (200.f / 240.f);
        const float scaleSz = size * (180.f / 240.f);
        auto concentric = [&](jf::JJson& e, float s) {
            e["x"] = double((size - s) * 0.5f); e["y"] = double((size - s) * 0.5f);
            e["w"] = double(s);                 e["h"] = double(s);
        };
        concentric(dial, dialSz); concentric(needle, dialSz); concentric(scale, scaleSz);
        // The numbers, on the face. The widget's defaults put them 80 out from the ticks at less than half
        // the size these need to be readable on a dial this size.
        prop(scale, "labelOffset",   "60");
        prop(scale, "labelFontSize", "70");
        // THE DECIMALS THE DEFINITION ASKS ITS LABELS FOR. The line ends `vd, ld` — the readout's decimals
        // and the tick labels', which are not the same number and were both being ignored by the scale. Its
        // own default is "%.0f", so a gauge running -1..1 printed a ring of zeroes.
        // A TICK IS NOT COLOURED BY THE BAND IT STANDS ON. The widget's default draws a tick that falls
        // inside a zone in that zone's own colour — which is right when the zone is somewhere else on the
        // face, and invisible when the zone is directly underneath it. Here it always is: the dial's rim
        // is 82.3..89.5 px out on a 240 gauge and the ticks are 81..90, so red ticks land on the red band
        // and amber on amber, and the only ticks you can see are the ones over bare face.
        //
        // The NUMBERS keep their zone colour. That is the half worth having — a redline reads as one
        // because its figures are red — and they sit at 70 px, well inside the rim, on nothing.
        prop(scale, "zoneTicks", "0");
        // NUMBERS ONLY WHERE THEY CAN BE READ. labelFontSize is in the scale's own 1000-unit space, so a
        // face of `size` draws them at 70 * scaleSz/1000 px: 12.6 px on a 240 gauge and 5.6 px on an 80.
        // A gauge squeezed into a readout column is 80 — nine unreadable smudges ringing it, over the top
        // of the ticks they belong to. Below the size where they carry information they are noise, and a
        // clean ring of ticks is a better gauge than a dirty ring of numbers.
        const bool roomForNumbers = scaleSz * (70.f / 1000.f) >= 9.f;      // ~9 px of glyph
        if (!roomForNumbers) prop(scale, "showLabels", "0");
        if (gd && gd->labelDecimals >= 0) {
            char f[16]; std::snprintf(f, sizeof f, "%%.%df", gd->labelDecimals);
            prop(scale, "format", f);
        }
        // Paint order is the stacking order: dial, then its markings, then the pointer on top.
        kids.push(std::move(dial));
        kids.push(std::move(scale));
        kids.push(std::move(needle));
        // WHAT the needle is pointing at, on the face, the way TunerStudio's gauge carries it: the channel's
        // name above the spindle, its live value below, its units under that. Without them an imported page
        // is a row of identical dishes — the pedal page shows three, and nothing on them said which was
        // which or what any of them read. Pushed LAST so the text sits over the pointer.
        // WHERE THE SCALE'S NUMBERS ARE, so the face text can keep out of them. The scale draws its labels
        // at (500 - majorTickLength - labelOffset)/1000 of its own box, and its box is scaleSz — so this is
        // the radius of the ring of numbers, in assembly pixels. The title used to sit at a blind 0.22 of
        // the height, which on a 240 square is 67 px above the spindle with the numbers ringing it at 70:
        // the caption was drawn straight across them, which is what "RPM - engine speed" lying over the
        // tick ring was.
        const float labelR = scaleSz * ((500.f - 50.f - 60.f) / 1000.f);
        const float lh = std::max(11.f, size * 0.085f);
        // A CAPTION NEEDS A FACE BIG ENOUGH TO HOLD ONE. "Air/Fuel Ratio" across an 80 px dial is wider
        // than the dial: it crosses the tick ring, the zone rim and the needle, and says less than the
        // page's own heading beside it already does. The reading is what a small gauge is FOR, so that
        // stays; the caption goes.
        const bool roomForTitle = size >= 130.f;
        // HOW WIDE THE FACE IS AT A GIVEN HEIGHT. A dial is a circle, so the room for a horizontal line of
        // text is the CHORD at that height, not the width of the square it is drawn in — and the caption
        // was being handed the square. "Long Term Fuel Trim Bank 1" is 187 px of text laid across a face
        // whose chord at the caption's radius is 127: it ran out over the tick ring on both sides, and
        // "LTFT learned accumulator: Bank 1" ran clean off the dial.
        auto chordAt = [&](float k) {
            const float d = std::min(std::fabs(k), 0.99f);
            return 2.f * labelR * std::sqrt(1.f - d * d);
        };
        // The caption, SHRUNK TO THE CHORD and dropped if that would make it unreadable. Shrinking is the
        // right first move — these are proper names ("Bank 1" vs "Bank 2" is the whole difference between
        // two gauges side by side), so trimming them is worse than setting them smaller. Below the floor
        // there is nothing useful left to draw and the page's own heading says it anyway.
        // The caption is given the CHORD as its box, not the square, and allowed to wrap inside it.
        auto faceText = [&](const std::string& text, float k, float boxW, float h, float px) {
            jf::JJson e = control("label", {}, {}, {}, boxW, h);
            prop(e, "labelText", text);
            prop(e, "align", "Center");
            prop(e, "showBorder", "0");
            prop(e, "wrap", "1");
            char f[24]; std::snprintf(f, sizeof f, "|%g|", double(px));
            prop(e, "fontName", f);
            e["x"] = double((size - boxW) * 0.5f);
            e["y"] = double(size * 0.5f + k * labelR - h * 0.5f);
            return e;
        };
        // TWO LINES BEFORE SHRINKING, AND SHRINKING BEFORE DROPPING.
        //
        // A caption is a proper name — "Bank 1" against "Bank 2" is the entire difference between two
        // gauges sitting side by side — so losing it costs more than setting it small. Wrapping buys the
        // most: "LTFT learned accumulator: Bank 1" is 223 px of text against a 127 px chord, which is two
        // comfortable lines at the nominal size and an unreadable 6 px on one. Only a caption that will
        // not fit two lines even then is scaled down, and only one still too big at 8 px is dropped.
        // COUNTED THE WAY THE WIDGET WRAPS, which is greedily at spaces. Dividing the total width by the
        // box says "LTFT learned accumulator: Bank 1" is two lines; wrapping it actually gives three,
        // because a line ends where the next WORD will not fit and the leftover is wasted. Sizing against
        // the arithmetic and rendering against the wrap is how that caption came out with its "1" cut off.
        auto linesAt = [&](const std::string& text, float boxW, float px) {
            if (text.empty() || boxW <= 0.f) return 1;
            const float k = px / 16.f;                       // estText is at the 16 px UI font
            int lines = 1; float used = 0.f;
            size_t i = 0;
            while (i < text.size()) {
                size_t sp = text.find(' ', i);
                const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                const float ww = estText(word) * k, spw = estText(" ") * k;
                if (used > 0.f && used + spw + ww > boxW) { ++lines; used = ww; }
                else                                      { used += (used > 0.f ? spw : 0.f) + ww; }
                if (sp == std::string::npos) break;
                i = sp + 1;
            }
            return lines;
        };
        // INSIDE THE RING OF NUMBERS, at a fraction of its radius rather than of the square.
        auto atRadius = [&](float k, float h) { return (size * 0.5f + k * labelR - h * 0.5f) / size; };
        // MIRRORING THE READING, close in to the hub. Out at 0.62 of the label ring the caption is level
        // with the numbers at the ring's widest, so "Coolant temp" runs into its own 20 and 80 — a caption
        // and a ring of numbers competing for one annulus. At 0.42 it sits in the clear space above the
        // spindle with the reading the same distance below, which is where TunerStudio puts both.
        // Shrunk to the chord it sits on, and dropped if that takes it below what is worth reading. On a
        // 240 px face the caption's chord is 127 px, so "Long Term Fuel Trim Bank 1" — 187 px at the
        // nominal size — comes down to about 10 px rather than running out over the tick ring.
        if (!title.empty() && roomForTitle) {
            const float boxW = std::max(24.f, chordAt(-0.42f) - 10.f);
            float px = std::max(8.f, size * 0.062f);
            // Shrink until it wraps into two lines, asking the same counter each time rather than solving
            // for it once: a smaller font changes where the breaks fall, so the answer has to be measured.
            while (px > 8.f && linesAt(title, boxW, px) > 2) px *= 0.94f;
            if (px >= 8.f) {
                const int n = std::min(2, linesAt(title, boxW, px));
                kids.push(faceText(title, -0.42f, boxW, lh * float(n), px));
            }
        }
        jf::JJson val = control("field", {}, binding, {}, size, lh * 1.3f);
        prop(val, "align", "1");                          // 1 = centred, under the spindle
        // AND IT IS SIZED TO THE FACE, which it alone was not. Every other piece of this assembly scales
        // with the square; the readout was left at the app's body size, so on a 240 gauge it was small and
        // on an 80 gauge "0.000" was a third of the width of the whole dial, straight through the spindle.
        { char f[24]; std::snprintf(f, sizeof f, "|%g|", double(std::max(9.f, size * 0.105f))); prop(val, "fontName", f); }
        // BELOW THE SPINDLE, always. Centring the reading when there is no caption above it put it
        // straight on top of the needle's hub and the first third of the needle itself — the pointer
        // drawn through the number it is pointing at. Under the hub is where TunerStudio puts it and
        // where there is nothing else.
        if (gd && gd->decimals >= 0) {
            char f[16]; std::snprintf(f, sizeof f, "%%.%df", gd->decimals);
            prop(val, "format", f);
        }
        val["x"] = 0.0; val["y"] = double(size * atRadius(0.42f, lh * 1.3f));
        kids.push(std::move(val));
        // AND NO SECOND UNITS LABEL. The readout already prints the channel's unit after the number, so a
        // units caption under it said it twice: "0.0 %" with a "%" of its own underneath.
        p["props"]["children"] = kids.dump();
        return p;
    }

    // One dialog -> one panel element carrying its children.
    //
    // A VERTICAL dialog (yAxis, and the ~75% of dialogs that state no layout at all) is laid out with
    // explicit geometry rather than handed to the Y Axis managed mode, for two reasons that are both about
    // matching what TunerStudio actually draws:
    //
    //   * Y Axis divides the panel's height among its children, so six lines of help text in a tall panel
    //     each got a 110px row. TS gives every row its natural height and leaves the slack at the bottom.
    //   * A TS field row is CAPTION + control, and no value widget here draws its own caption (a caption is
    //     a Label element beside the control, as when you draw a page by hand). A row is therefore two
    //     elements side by side, which a one-child-per-row managed stack cannot express.
    //
    // xAxis / border dialogs keep the managed mode: their children are sub-panels and gauges whose placement
    // IS the layout, and there the container's arrangement is exactly what TS means.
    jf::JJson panelFor(const tsdash::Dialog& d, float w, float h, int depth) {
        const int mode = layoutOf(d.layout);
        if (mode != Column) {
            jf::JJson p = element("panel", 0.f, 0.f, w, 0.f);
            prop(p, "labelText", d.title);
            prop(p, "helpText", helpFor(d));
            p["props"]["layoutMode"] = std::to_string(mode);
            ++rep.widgets;
            // Author every child at the extent its SLOT will have. A managed layout sizes strips from the
            // child's own rect, so a child authored at the container's full width claims the whole container:
            // a Border dialog's West panel took all of it, leaving Center and East to pile up on top of each
            // other in what was left. X Axis divides by the sum of child widths, so a column is one share of
            // the width; in Border only West/Center/East share it, while North/South span it.
            // Each child gets its NATURAL share of the width — the columns of a dialog are rarely equal, and
            // splitting evenly squeezed a wide one while leaving a narrow one half empty.
            float shareSum = 0.f;
            for (const auto& it : d.items)
                if (mode == XAxis || (it.placement != "North" && it.placement != "South"))
                    shareSum += itemWidth(it, depth);
            auto childWidth = [&](const tsdash::Item& it) {
                if (mode == Border && (it.placement == "North" || it.placement == "South")) return w;
                if (shareSum <= 0.f) return w;
                return w * itemWidth(it, depth) / shareSum;
            };
            jf::JJson kids = jf::JJson::array();
            for (const auto& it : d.items) kids.push(childFor(it, depth, childWidth(it)));
            // A null child means "nothing to emit" — drop those rather than leaving holes in the layout.
            jf::JJson kept = jf::JJson::array();
            for (const jf::JJson& k : kids.arr()) if (k.isObject()) kept.push(k);
            p["props"]["children"] = kept.dump();   // the panel parses this back into real child widgets
            // Height from the CONTENT, like a stacked panel: a North/South strip stacks, everything else
            // shares the middle. Falling back to the page height instead made a nested strip as tall as the
            // whole page and pushed its siblings off it.
            float stripSum = 0.f, middle = 0.f;
            for (const jf::JJson& k : kept.arr()) {
                const float kh = float(k["h"].number());
                const std::string rg = k["props"].contains("region") ? k["props"]["region"].str() : std::string();
                if (rg == "North" || rg == "South") stripSum += kh;
                else                                middle = std::max(middle, kh);
            }
            p["h"] = double(h > 0.f ? h : stripSum + middle + (d.title.empty() ? 0.f : lay.rowH) + lay.gap);
            return p;
        }
        return stackedPanel(d, w, h, depth);
    }

    // A vertically-stacked dialog at explicit geometry. `h` <= 0 means "as tall as its content" — a nested
    // panel sizes to what it holds, and only the page's root panel is given the page height.
    // A dialog's help topic is the panel's "?" badge — the same place TunerStudio puts it. It arrives as an
    // id pointing at a `help = <id>` block, so the text has to be looked up rather than carried on the item.
    std::string helpFor(const tsdash::Dialog& d) const {
        std::string t;
        if (const auto h = dash.helps.find(d.helpTopic); h != dash.helps.end()) {
            t = plainText(h->second.text);
            if (!h->second.url.empty()) t += (t.empty() ? "" : "\n\n") + h->second.url;
        }
        if (!d.webHelp.empty()) t += (t.empty() ? "" : "\n\n") + d.webHelp;
        return t;
    }

    jf::JJson stackedPanel(const tsdash::Dialog& d, float w, float h, int depth) {
        jf::JJson p = element("panel", 0.f, 0.f, w, 0.f);
        prop(p, "labelText", d.title);
        prop(p, "helpText", helpFor(d));
        // A COLUMN OF ROWS, NOT A BOX OF COORDINATES.
        //
        // This panel used to be Free with its rows placed at explicit y. A Free panel draws its children at
        // min(width ratio, height ratio) of the size it was AUTHORED at — so while the page is wider than
        // the dialog nothing happens (the height ratio is 1 and wins the min), and the moment the window is
        // narrower than the dialog, everything in it scales down. Text included. On this ini that threshold
        // is 760 px for the median dialog and 1232 for the 95 widest, so taking the window off full width
        // started shrinking the type.
        //
        // Column never scales: it hands each row the full width and the row's own height. Nor do the rows,
        // because only a FREE panel scales its contents — a leaf widget handed a narrower box keeps its
        // font and simply has a narrower box. So a window too narrow now squeezes the controls, and
        // eventually clips them, which is honest; it does not quietly shrink the whole dialog.
        p["props"]["layoutMode"] = std::to_string(int(Column));
        ++rep.widgets;

        const float cw = w - 2.f * lay.gap;             // content width, inside the panel's own inset
        jf::JJson kept = jf::JJson::array();
        // Children are placed in the panel's CONTENT space, which already starts below the title bar — the
        // panel insets it. Reserving a title row here too counted the band twice: every row sat a band lower
        // than its slot and the last one fell off the bottom, clipped mid-row.
        float y = lay.gap;
        // Loose FIELDS first, then the sub-panels — TunerStudio's order, not the declaration's. A dialog
        // that declares three fields, two panels, then four more fields is drawn with all seven fields
        // together and the groups beneath them. Following the file put the last four below a couple of
        // framed groups, which reads as a different dialog.
        std::vector<const tsdash::Item*> ordered;
        ordered.reserve(d.items.size());
        // A BLOCK IS ANYTHING THAT ISN'T A LOOSE FIELD. Fields are hoisted above the blocks because that is
        // what TunerStudio draws — three fields, two panels, then four more fields show as all seven
        // together with the groups beneath. But the test was "is it a DIALOG", so a `panel = <tableId>`
        // failed it and got hoisted with the fields: a page came out banner, table, override field where
        // the ini says (and TS shows) banner, override field, table. A table, a lamp grid and a readout block are blocks like any other panel; only a
        // field is loose.
        auto isGroup = [&](const tsdash::Item& it) {
            return it.kind == tsdash::Item::Kind::Panel &&
                   (dash.dialogs.count(it.panelId) || dash.tables.count(it.panelId) ||
                    dash.panels.count(it.panelId)  || dash.readouts.count(it.panelId));
        };
        for (const auto& it : d.items) if (!isGroup(it)) ordered.push_back(&it);
        for (const auto& it : d.items) if (isGroup(it))  ordered.push_back(&it);

        // The caption column: the widest caption among the rows that will BE caption+control rows, capped
        // so one long-winded label cannot squeeze every control on the panel into a sliver.
        float capCol = 0.f;
        for (const tsdash::Item* itp : ordered) {
            const tsdash::Item& it = *itp;
            if (it.kind != tsdash::Item::Kind::Field) continue;
            const std::string b = bindingFor(it.var, meta, dash);
            if (b.empty() && it.var.empty()) continue;            // a help row spans the width, no column
            capCol = std::max(capCol, estText(captionOf(it, b)));
        }
        capCol = std::min(capCol, cw * 0.6f);

        for (const tsdash::Item* itp : ordered) {
            const tsdash::Item& it = *itp;
            const Row row = rowFor(it, depth, cw, d.title, capCol);
            if (row.els.empty()) continue;
            const float rh = row.h + lay.rowGap;        // the gap travels in the row's own height
            if (row.els.size() == 1) {
                jf::JJson el = row.els.front();
                el["x"] = 0.0; el["y"] = 0.0;
                el["w"] = double(std::min(float(el["w"].number()), cw));
                el["h"] = double(rh);
                kept.push(std::move(el));
            } else {
                // X Axis: the row's parts keep their PROPORTIONS of whatever width the row is given, which
                // is what the authored x/width already say. Nothing here is a Free box, so nothing scales.
                jf::JJson line = jf::JJson::array();
                for (const jf::JJson& e : row.els) {
                    jf::JJson el = e;
                    el["x"] = 0.0; el["y"] = 0.0;
                    // A stacked row's children keep their own heights - a Column shares the row out by
                    // them, and a caption given the whole row height would take half of it from the grid.
                    if (!row.stack) el["h"] = double(row.h);
                    el["w"] = double(std::min(float(el["w"].number()), cw - kPanelInset));
                    line.push(std::move(el));
                }
                jf::JJson r = element("panel", 0.f, 0.f, cw, rh);
                r["props"]["layoutMode"] = std::to_string(int(row.stack ? Column : XAxis));
                r["props"]["showBorder"] = "0";
                r["props"]["children"]   = line.dump();
                ++rep.widgets;
                kept.push(std::move(r));
            }
            y += rh;
        }
        p["props"]["children"] = kept.dump();
        // The title band IS part of the panel's height, just not part of its content space.
        p["h"] = double(h > 0.f ? h : y + lay.gap + (d.title.empty() ? 0.f : lay.rowH));
        return p;
    }

    // One line of a vertically-stacked dialog: the elements it places (0, 1 or 2 of them), positioned
    // relative to the row's own origin, and how tall the row is.
    // `stack` says the parts go ONE ABOVE THE OTHER rather than side by side. A caption over a grid is
    // the only case, and it is not a detail: laid out as a side-by-side row, the title and the table each
    // claimed the row's full width, so an X Axis split gave them half each — a 16-column ignition map
    // squeezed into 250 px with a scroll bar, next to its own title.
    struct Row { std::vector<jf::JJson> els; float h = 0.f; bool stack = false; };

    // The caption a bound field row shows: its label plus the field's units, the way TunerStudio writes them
    // ("On temperature(C)"). Built in ONE place because the dialog's WIDTH is estimated from it — a second
    // spelling here would size dialogs for captions they do not have.
    std::string captionOf(const tsdash::Item& it, const std::string& binding) const {
        std::string cap = headerText(it.label);
        if (const auto f = meta.config().find(binding); f != meta.config().end() && !f->second.units.empty())
            cap += "(" + f->second.units + ")";
        return cap;
    }

    float estText(const std::string& s) const { return lay.charW * float(s.size()); }

    // The width a dialog WANTS. TunerStudio sizes a dialog to its content, and a fixed width truncated 102
    // help rows in this ini alone — mid-sentence, which reads as a broken import rather than a long line.
    float naturalWidth(const tsdash::Dialog& d, int depth) const {
        const int mode = layoutOf(d.layout);
        // The dialogW floor is for a PAGE — a one-row dialog 200px wide reads as broken. A nested column
        // has no such claim: giving each of them the same floor made a three-column dialog 1776px wide, so
        // two thirds of it sat off the right of the window with no hint that it was there.
        const float floorW = depth == 0 ? lay.dialogW : lay.minPanelW;
        if (mode == XAxis) {
            float sum = 0.f;
            for (const auto& it : d.items) sum += itemWidth(it, depth);
            return std::max(floorW, sum);
        }
        if (mode == Border) {
            float middle = 0.f, span = floorW;
            for (const auto& it : d.items) {
                const float iw = itemWidth(it, depth);
                if (it.placement == "North" || it.placement == "South") span = std::max(span, iw);
                else                                                    middle += iw;
            }
            return std::max(span, middle);
        }
        // A STACKED DIALOG IS A CAPTION COLUMN AND A VALUE COLUMN, so it needs the widest of each — not
        // the widest ROW. Taking the widest row let a dialog whose longest caption sits beside a tick box
        // come out narrower than its own layout needs: the shared caption column pushed every value box
        // right, and the ends of them - the units, the spinner - were clipped off the edge of the page.
        float wide = floorW, cap = 0.f, ctl = 0.f;
        for (const auto& it : d.items) {
            wide = std::max(wide, itemWidth(it, depth));
            if (it.kind != tsdash::Item::Kind::Field) continue;
            const std::string b = bindingFor(it.var, meta, dash);
            if (b.empty() && it.var.empty()) continue;            // prose spans the width, no columns
            cap = std::max(cap, estText(captionOf(it, b)));
            ctl = std::max(ctl, controlNaturalW(b));
        }
        if (cap > 0.f) wide = std::max(wide, cap + ctl + 4.f * lay.gap);
        return wide;
    }

    // WHAT A CONTROL NEEDS ACROSS, BY WHAT IT IS. Every field used to claim the same 260 px whether it
    // held a tick box, a two-word dropdown or a four-digit number — and since the caption column takes
    // whatever the row does not, that flat claim widened every page by the difference. TunerStudio sizes
    // a value box to the value: a yes/no is a tick, "Speed Density" is as wide as "Speed Density". This
    // is the width the item ASKS for; a row with room to spare still stretches its control up to
    // lay.controlW, so nothing is cramped when the window is wide.
    float controlNaturalW(const std::string& b) const {
        if (b.empty()) return lay.controlW;
        std::string type = Cache::instance().widgetTypeFor(b);
        const std::vector<std::string> opts = meta.enumOptions(b);
        if (type == "checkbox" && opts.size() >= 2) type = "combobox";   // as the row builder decides it
        if (type == "checkbox" || type == "toggle" || type == "radio")
            return jf::JStyle::current().checkHeight + 2.f * lay.gap;
        if (type == "combobox" || type == "enum" || type == "settingselector") {
            float w = 0.f;
            for (const std::string& o : opts) w = std::max(w, estText(o));
            // The arrow and the box's own padding, then floored: a dropdown of "on"/"off" still has to
            // look like a dropdown, and capped, because a channel list of hundreds is not a page width.
            return std::clamp(w + 3.f * lay.gap, 120.f, lay.controlW);
        }
        // A number, plus the units drawn inside the box at its right-hand end: without them "1500 rpm"
        // came out "1500 rp", which reads as a broken field rather than a narrow one.
        float units = 0.f;
        if (const auto f = meta.config().find(b); f != meta.config().end() && !f->second.units.empty())
            units = estText(f->second.units) + lay.gap;
        return std::min(lay.controlW, 140.f + units);
    }

    // What one item needs across, including the panel insets it sits in.
    float itemWidth(const tsdash::Item& it, int depth) const {
        using K = tsdash::Item::Kind;
        if (it.kind == K::Panel && depth < 8) {
            if (const auto sub = dash.dialogs.find(it.panelId); sub != dash.dialogs.end())
                return naturalWidth(sub->second, depth + 1) + 2.f * lay.gap;
            // An embedded editor asks for its plot PLUS the gauge/cells column beside it; claiming a plain
            // dialog width squeezed the plot into whatever was left after that column.
            if (const auto t = dash.tables.find(it.panelId); t != dash.tables.end()) {
                const float plot = t->second.sizeW > 0.f ? t->second.sizeW : gridNaturalSize(t->second).first;
                const bool side = !t->second.gauge.empty() || t->second.textValues;
                return plot + (side ? lay.gaugeW + lay.gap : 0.f) + 4.f * lay.gap;
            }
            return lay.dialogW;
        }
        if (it.kind == K::Field) {
            const std::string b = bindingFor(it.var, meta, dash);
            // A HELP ROW WRAPS, SO IT DOES NOT GET A VOTE ON HOW WIDE THE PAGE IS. Asking for its whole
            // sentence let one line of prose set the width of everything under it: the Vehicle Information
            // page came out 790 px wide because of a 103-character note about creating a new project, and
            // every field row beneath inherited that width — a 493 px label column holding 200 px of text,
            // with the control stranded at the far right. The sentence still says what it says; it takes
            // the lines it needs instead of the width, like prose anywhere else.
            if (b.empty() && it.var.empty())
                return std::min(estText(headerText(it.label)) + 4.f * lay.gap, lay.prosePreferredW);
            return estText(captionOf(it, b)) + controlNaturalW(b) + 4.f * lay.gap;
        }
        // Everything else asks for what it DRAWS. A gauge that claimed a whole dialog width took most of a
        // three-column dialog's share, leaving the columns either side too narrow to hold a caption and its
        // control at once — they overlapped, and the captions were clipped mid-word.
        if (it.kind == K::Gauge)     return lay.gaugeW + 2.f * lay.gap;
        if (it.kind == K::Indicator ||
            it.kind == K::Command)   return lay.controlW + 4.f * lay.gap;
        return lay.dialogW;
    }

    // The ON caption. A plain string is the caption; a string expression becomes a prefix plus the channel
    // whose value names to look up and the expression selecting one. Anything braced we cannot read falls
    // back to the OFF caption — a lamp that reads the same in both states and only changes colour is
    // right far more often than a lamp displaying its own definition.
    void _lampOnTitle(jf::JJson& e, const std::string& onLabel, const std::string& offLabel) {
        std::string prefix, list, index;
        if (parseStringExpr(onLabel, prefix, list, index)) {
            prop(e, "onTitle", prefix);
            prop(e, "onTitleList",  bindingFor(list, meta, dash).empty() ? list : bindingFor(list, meta, dash));
            prop(e, "onTitleValue", bindingFor(index, meta, dash).empty() ? rewriteExpr(index, meta, dash)
                                                                         : bindingFor(index, meta, dash));
            return;
        }
        const bool computed = !onLabel.empty() && onLabel.front() == '{';
        prop(e, "onTitle", (onLabel.empty() || computed) ? offLabel : onLabel);
        if (computed) rep.skippedKinds["indicator string expression -> " + onLabel]++;
    }

    // THE WIDEST TEXT A LAMP CAN DISPLAY — which is not the widest text the definition writes down.
    //
    // A string-expression label is source, not a caption: "{ ETB: bitStringValue(etbCutCodeList,
    // etb1etbErrorCodeBlinker)}" is 63 characters of which the lamp shows "ETB: " and one name out of
    // etbCutCodeList. Measuring the source made one lamp wider than a third of the strip and collapsed a
    // grid of eight columns into two, each cell 616 px of mostly nothing.
    //
    // So this asks what the WIDGET will draw, in each state, and takes the longer: the off caption, or the
    // prefix with the longest name the list can supply. The names come from the meta, so a definition with
    // different lists sizes itself differently rather than to a constant that suits neither.
    float lampTextW(const tsdash::IndicatorLamp& l) {
        float w = estText(l.offLabel);
        std::string prefix, list, index;
        // THE PREFIX, NOT THE WORST NAME IT COULD APPEND. Sizing every cell to hold the longest fault in
        // etbCutCodeList made each one 246 px — a five-column grid seven rows deep, sized for a string that
        // is only on screen when that particular fault is live. The widget already clips text to its rect,
        // so a long fault name costs its own lamp some tail; sizing the whole strip for it costs every
        // lamp, always.
        if (parseStringExpr(l.onLabel, prefix, list, index)) return std::max(w, estText(prefix));
        // A computed label this cannot read falls back to the off caption, exactly as _lampOnTitle does.
        if (l.onLabel.empty() || l.onLabel.front() == '{') return w;
        return std::max(w, estText(l.onLabel));
    }

    // A lamp, with the captions and colours the definition gives it. Shared by an indicatorPanel's grid and
    // by the front-page strip so the two look alike — they are the same thing in TunerStudio.
    // The unlit look every TunerStudio definition writes down: white ground, black text. Named or hex,
    // since an ini may spell either.
    static bool _isPlainOff(const std::string& bg, const std::string& fg) {
        auto is = [](std::string v, const char* name, const char* hex) {
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            v.erase(0, v.find_first_not_of(" \t"));
            if (!v.empty()) v.erase(v.find_last_not_of(" \t") + 1);
            return v == name || v == hex;
        };
        return is(bg, "white", "#ffffff") && (fg.empty() || is(fg, "black", "#000000"));
    }

    jf::JJson lampFor(const tsdash::IndicatorLamp& l, float w, float h) {
        // A lamp's expression is not always a single channel name: some are compound ("enableAemXSeries &&
        // (wb1stateCode >= 3 …)"), and the studio's own lamps address its state through a sigil. Resolve a
        // plain name to its binding; anything else is an EXPRESSION, rewritten into the studio's spelling.
        std::string bind = bindingFor(l.expr, meta, dash);
        if (bind.empty() && !l.expr.empty()) bind = rewriteExpr(l.expr, meta, dash);
        jf::JJson e = control("indicator", l.offLabel, bind, {}, w, h);
        prop(e, "offTitle", l.offLabel);
        _lampOnTitle(e, l.onLabel, l.offLabel);
        // TunerStudio's "not lit" is white on black text, which is not a choice about THIS lamp — every
        // one of this ini's 33 says it, because that is what an unlit lamp looks like in a light UI. Baked
        // in, it becomes 33 bright white chips on the dark scheme: a wall of paper on a black page, and
        // the three that are actually lit no louder than the thirty that are not. Left unset, the widget
        // takes the scheme's own quiet surface and the lamp reads as unlit on either theme.
        //
        // A definition that states something ELSE for its off state means it, and keeps it.
        if (!_isPlainOff(l.offBg, l.offFg)) {
            prop(e, "offBg", colourHex(l.offBg));
            prop(e, "offFg", colourHex(l.offFg));
        }
        prop(e, "onBg",  colourHex(l.onBg));
        prop(e, "onFg",  colourHex(l.onFg));
        return e;
    }

    // A caption is LEFT aligned, unlike a free-standing label: a column of captions that centres each one
    // individually comes out ragged, and TunerStudio aligns them to the left edge.
    // HOW MANY LINES THIS TEXT TAKES ACROSS `w`, wrapped greedily at spaces the way the label widget does
    // it. A one-sentence warning banner can be 650 px of words: told to stay on one line it loses two thirds of
    // itself off the right-hand edge, and told to wrap in a 30 px row it loses the same two thirds off the
    // bottom. Either way the reader is missing the sentence, so it wraps AND gets the height to do it in.
    int wrapLines(const std::string& text, float w) const {
        if (w <= 0.f || text.empty()) return 1;
        const float sp = estText(" ");
        int lines = 1;
        float x = 0.f;
        std::istringstream in(text);
        std::string word;
        while (in >> word) {
            const float ww = estText(word);
            if (x > 0.f && x + sp + ww > w) { ++lines; x = ww; }
            else                            { x += (x > 0.f ? sp : 0.f) + ww; }
        }
        return lines;
    }

    // A caption that is PROSE: it wraps, and the row grows to hold the lines it wraps into.
    jf::JJson proseLabel(const std::string& text, const std::string& condition, float w, float& hInOut) {
        const int lines = wrapLines(text, w - 6.f);          // less LabelWidget's own 3px inset each side
        const float lineH = lay.rowH * 0.7f;                 // the app's line height, near enough to size a box
        hInOut = std::max(hInOut, float(lines) * lineH + 8.f);
        jf::JJson e = control("label", text, {}, condition, w, hInOut);
        e["props"]["align"] = "Left";
        e["props"]["wrap"]  = "1";
        return e;
    }

    jf::JJson captionLabel(const std::string& text, const std::string& condition, float w, float h) {
        jf::JJson e = control("label", text, {}, condition, w, h);
        e["props"]["align"] = "Left";
        return e;
    }

    Row rowFor(const tsdash::Item& it, int depth, float cw, const std::string& ownerTitle = {},
               float capCol = 0.f) {
        using K = tsdash::Item::Kind;
// A BLANK FIELD IS A SPACER, and dropping it loses the grouping it was there to create. TunerStudio
// authors use an empty `field = ""` as vertical whitespace between related rows, so a dialog that reads
// as three groups of four became twelve rows running together — the same fields, and none of the shape
// the author gave them. Emitted as an empty label of one row: nothing to read, exactly the gap the ini
// asked for, and it stacks like any other row.
        Row row;
        if (it.kind == K::Field) {
            const std::string b = bindingFor(it.var, meta, dash);
            if (b.empty() && it.var.empty()) {                       // help / heading text, no control
                if (it.label.empty()) {          // a spacer: the gap IS the content — see the note below
                    row.els.push_back(spacer(cw));
                    row.h = lay.rowH;
                    return row;
                }
                // '#' and '!' are BANNER styles, not just text hints: TunerStudio fills the row — blue for a
                // note, red for a warning — across the width of the dialog, in light text. Stripping the marker and
                // leaving prose behind loses the thing the marker was for: being impossible to miss.
                const bool warn   = it.label[0] == '!';
                const bool banner = warn || it.label[0] == '#';
                const std::string text = headerText(it.label);
                std::string url, caption;
                if (htmlLink(text, url, caption)) {
                    jf::JJson h = control("label", caption, {}, it.condition, cw, lay.rowH);
                    h["props"]["align"] = "Left";
                    prop(h, "link", url);
                    if (banner) {                      // a marked link is bannered like any other help row
                        h["props"]["bgColor"] = warn ? "#c62828" : "#1a4fd8";
                        h["props"]["fgColor"] = "#ffffff";
                        h["props"]["borderRadius"] = "0";
                        h["props"]["padding"] = kBannerPad;
                    }
                    row.els.push_back(std::move(h));
                } else {
                    float proseH = lay.rowH;
                    jf::JJson cap = proseLabel(text, it.condition, cw, proseH);
                    if (banner) {
                        cap["props"]["bgColor"] = warn ? "#c62828" : "#1a4fd8";
                        cap["props"]["fgColor"] = "#ffffff";
                        cap["props"]["borderRadius"] = "0";
                        cap["props"]["padding"] = kBannerPad;
                    }
                    row.h = std::max(row.h, proseH);
                    row.els.push_back(std::move(cap));
                }
                row.h = std::max(row.h, lay.rowH);   // a wrapped banner keeps the height it needs
                return row;
            }
            // Caption left, control right — the shape of every TunerStudio field row. The caption keeps the
            // field's own visible condition so the whole row appears and disappears together.
            // The CAPTION gets the room it needs and the control takes what is left (down to a usable
            // minimum). Fixing the control at its preferred width instead clipped captions mid-word in every
            // panel narrower than caption + 260 — and a truncated caption costs more than a narrow input box.
            // ONE CAPTION COLUMN FOR THE WHOLE PANEL, so the value boxes line up. Sized per row from its
            // own caption, every row put its control at a different x: a column of nine settings had nine
            // different left edges, which reads as broken rather than as a form. TunerStudio lays a group
            // out as a caption column and a value column; so does this now. A row whose caption is longer
            // than the column still gets what it needs - better one ragged row than one clipped caption.
            const float capW = std::max(capCol, estText(captionOf(it, b))) + lay.gap;
            const float ctlW = std::clamp(cw - capW - lay.gap, 80.f, lay.controlW);
            const float labW = std::max(80.f, cw - ctlW - lay.gap);
            // widgetTypeFor, NOT controlFor: the latter answers with a CLASSIFICATION, and a classification
            // used as an element type ("config") is not a registered widget — those rows imported as
            // invisible holes with a caption and nothing beside it.
            std::string type = b.empty() ? std::string("field") : Cache::instance().widgetTypeFor(b);
            // TunerStudio renders every `bits` field as a dropdown of its option labels, including a
            // two-option one. Our 0..1 heuristic would make that a checkbox, which throws the labels away —
            // "4 strokes / 2 strokes" is not a tick box.
            //
            // A COMBO BOX, not the picker. "enum" is the studio's searchable picker dialog, and
            // widgetTypeFor keeps it for the two lists a fixed dropdown cannot express: a signal selector
            // with hundreds of channels, and a pin picker whose options depend on a sibling field. A
            // yes/no is neither. Asking for the picker here put a bare unthemed list on the page where
            // every other choice in the application is a combo — and lost the keyboard select, the
            // type-ahead and the wheel that come with the real control.
            if (type == "checkbox" && meta.enumOptions(b).size() >= 2) type = "combobox";
            jf::JJson ctl = control(type.empty() ? "field" : type, {}, b, it.condition, ctlW, lay.rowH);
            ctl["x"] = double(cw - ctlW);
            jf::JJson cap = captionLabel(captionOf(it, b), it.condition, labW, lay.rowH);
            // The row's HELP hangs off the caption as well as the control: TunerStudio puts a "?" on the
            // row, and the label is what you point at. The control finds the same text through its binding.
            if (const auto f = meta.config().find(b); f != meta.config().end() && !f->second.help.empty())
                prop(cap, "tooltip", f->second.help);
            row.els.push_back(std::move(cap));
            row.els.push_back(std::move(ctl));
            row.h = lay.rowH;
            return row;
        }
        jf::JJson e = childFor(it, depth, cw);        // everything else stays one element
        if (!e.isObject()) return row;
        // A table/curve editor has a TITLE in TunerStudio (centred above the grid). Those widgets draw no caption of their own, so the title is its own label above them —
        // the same caption-beside-control rule a field row follows.
        const std::string ty = e["type"].str();
        if ((ty == "table" || ty == "curve") && e["props"].contains("labelText") &&
            e["props"]["labelText"].str() != ownerTitle) {
            // …unless the panel around it already says the same thing. A dialog whose whole content is one
            // table is usually named after that table, and the page, the panel and the caption then read
            // the same words three times down the top of the window.
            row.els.push_back(captionLabel(e["props"]["labelText"].str(), {}, cw, lay.rowH));
            e["y"] = double(lay.rowH);
            row.h  = lay.rowH + float(e["h"].number());
            row.stack = true;                 // caption ABOVE the grid, not beside it
        } else {
            row.h = float(e["h"].number());
        }
        row.els.push_back(std::move(e));
        return row;
    }

    // THE SIZE A GRID NEEDS TO BE READ AT, from the grid. Every editor used to claim the same 480x350
    // whatever it held, so a 5x5 warmup correction opened in a window built for a 16x16 ignition map with
    // most of it empty, and the ignition map opened too small and scrolled. The widget sizes a cell to the
    // formatted value it holds (TableWidget: measureWidth("-8888") + 10 px across, a line + 6 down) and
    // does NOT stretch below that, so the space it wants is arithmetic, not a guess: a column per bin, a
    // row per bin, plus the axis strips down the side and along the top.
    //
    // Estimated with the same charW the caption widths use — there is no font at convert time — and capped,
    // because a 32x32 map is a window nobody has room for and scrolling is the honest answer there.
    std::pair<float, float> gridNaturalSize(const tsdash::TableRef& t) const {
        const std::string cells = "ts." + t.cells;
        int cols = 0, rows = 0;
        if (const auto ct = meta.configTables().find(cells); ct != meta.configTables().end()) {
            cols = std::max(1, ct->second.cols);
            rows = std::max(1, ct->second.rows);
        } else if (const auto a1 = meta.arrays1d().find(cells); a1 != meta.arrays1d().end()) {
            cols = 1; rows = std::max(1, a1->second.count);       // a 1-D array draws down a column
        }
        if (cols <= 0 || rows <= 0) return { lay.editorW, lay.editorH };
        const float cellW = estText("-8888.8") + 10.f;            // the widget's own natural cell
        const float cellH = lay.rowH * 0.72f;
        const float axisW = cellW, axisH = cellH + lay.rowH;      // y bins beside, x bins + name bar above
        return { std::min(lay.maxDialogW, axisW + float(cols) * cellW + 2.f * lay.gap),
                 std::min(kMaxEditorH,    axisH + float(rows) * cellH + 2.f * lay.gap) };
    }

    jf::JJson tableControl(const tsdash::TableRef& t, float w, float h) {
        const std::string cells = "ts." + t.cells;
        // The EDITOR follows the data: a map is a table, a curve with an axis is a curve, and a plain
        // 1-D array (a curve whose axis array is not in the image) is the 1-D array editor. Asking the
        // cache means an imported page gets the same control a dictionary drag would.
        // The DEFINITION decides curve vs table — a curve is stored as a one-row table, so asking the cache
        // turned every imported curve into a grid of cells. The cache is still the authority on the one case
        // the definition cannot express: cells with no resolvable axis, which are a plain 1-D array.
        const bool known = meta.configTables().count(cells) || meta.arrays1d().count(cells);
        const std::string type = !known                      ? std::string(t.curve ? "curve" : "table")
                               : meta.arrays1d().count(cells) ? std::string("array1d")
                                                              : std::string(t.curve ? "curve" : "table");
        jf::JJson e = control(type, t.title, known ? cells : std::string(), {}, w, h);
        // TunerStudio traces the live cell on a connected table as a matter of course — that white marker
        // walking the map IS the tuning feedback. The native widget has the same cursor but keeps it off by
        // default, so an imported map showed nothing while the ECU was running.
        if (type == "table") prop(e, "cellTrace", "2");   // 2 = on (1 = off, the widget's own default)
        // A curve DECLARES how it is drawn — axis ranges, grid divisions and axis titles. Without them an
        // untuned curve auto-ranges on all-zero data and draws a flat line through nothing, where
        // TunerStudio shows the real span (a 0..8000 rpm grid) you are meant to tune against.
        if (type == "curve") {
            auto pair = [](double a, double b) {
                char buf[64]; std::snprintf(buf, sizeof buf, "%g,%g", a, b); return std::string(buf);
            };
            // A bound may be an EXPRESSION ("-40, {cltHighXaxis}, 9"), so the bounds go across as written,
            // with their identifiers rewritten to model paths — the widget evaluates them. Dropping the axis
            // because one end was an expression left the curve auto-ranging on untuned zeros.
            auto bound = [&](const std::string& raw) { return rewriteExpr(raw, meta, dash); };
            if (t.hasXAxis) prop(e, "xRange", bound(t.xMinExpr) + "," + bound(t.xMaxExpr));
            if (t.hasYAxis) prop(e, "yRange", bound(t.yMinExpr) + "," + bound(t.yMaxExpr));
            if (t.xDiv > 0 || t.yDiv > 0) prop(e, "grid", pair(t.xDiv, t.yDiv));
            prop(e, "xLabel", t.xLabel);
            prop(e, "yLabel", t.yLabel);
        }
        return e;
    }

    // A curve editor is not always just the plot. TunerStudio draws a `gauge = <id>` in its top-right corner
    // (a live readout of the channel the curve tracks) and, when `showTextValues = true`, the editable cells
    // in a column under it. Both belong BESIDE the curve, not instead of it, so the set goes into a panel:
    // the plot takes the room that is left.
    // THE SIDE COLUMN AN EDITOR CARRIES: the gauge, and under it the editable cells. Its width and the
    // height it needs, in one place, because two callers have to agree about it — editorWithGauge, which
    // BUILDS it, and the page sizing, which has to leave room for it. They did not: the page asked
    // gridNaturalSize for the grid alone, so a curve whose gauge column needs 500 px was given a page
    // 217 px tall and its own contents hung 400 px out of it. Same arithmetic, asked twice.
    std::pair<float, float> sideColumn(const tsdash::TableRef& t, float w) const {
        const auto g = dash.gauges.find(t.gauge);
        const bool haveGauge = !t.gauge.empty() && g != dash.gauges.end();
        const std::string cells = "ts." + t.cells;
        const bool haveCells = (t.textValues || haveGauge) &&
                               (meta.configTables().count(cells) || meta.arrays1d().count(cells));
        if (!haveGauge && !haveCells) return { 0.f, 0.f };
        const float sideW  = std::min(std::max(lay.gaugeW, 140.f), w * 0.32f);
        const float gaugeH = haveGauge ? sideW : 0.f;
        float cellRows = 0.f;
        if (haveCells) {
            if (const auto ct = meta.configTables().find(cells); ct != meta.configTables().end())
                cellRows = float(std::max(1, ct->second.cols));
            else if (const auto a1 = meta.arrays1d().find(cells); a1 != meta.arrays1d().end())
                cellRows = float(std::max(1, a1->second.count));
        }
        const float cellsNeed = cellRows > 0.f
                              ? std::min((cellRows + 1.f) * lay.rowH * 0.72f, 12.f * lay.rowH * 0.72f) : 0.f;
        return { sideW, gaugeH + (cellsNeed > 0.f ? lay.gap + cellsNeed : 0.f) };
    }

    jf::JJson editorWithGauge(const tsdash::TableRef& t, float w, float h) {
        const auto g = dash.gauges.find(t.gauge);
        const bool haveGauge = !t.gauge.empty() && g != dash.gauges.end();
        const std::string cells = "ts." + t.cells;
        // `showTextValues = true` asks for the editable cells beside the plot. A curve's cells are ONE ROW
        // of storage, and the table widget lays a one-axis table DOWN a column unless it is transposed — so
        // it draws the Nx1 column TunerStudio shows down the side. It could always do this; it just had to
        // be asked for the right orientation.
        // A GAUGE implies the cells too: "this gauge will be displayed in the top right corner and the text
        // cells will be laid out vertically under the gauge". That is why TunerStudio shows the values beside
        // the Engine Temperature RPM Limit curve, which never says showTextValues.
        const bool haveCells = (t.textValues || haveGauge) &&
                               (meta.configTables().count(cells) || meta.arrays1d().count(cells));
        if (!haveGauge && !haveCells) return tableControl(t, w, h);

        // ONE side column holds the gauge and, under it, the cells — the layout TunerStudio uses.
        const auto [sideW, sideH] = sideColumn(t, w);
        const float gaugeH = haveGauge ? sideW : 0.f;
        (void)sideH;
        // The editor has to be TALL ENOUGH for that column. An embedded one is 220px by default, and a gauge
        // eats 160 of it — the cells were left with 48px to draw nine rows in, a striped smear rather than
        // numbers. Ask for the height the contents need: a row per point, plus the header.
        float cellRows = 0.f;
        if (haveCells) {
            if (const auto ct = meta.configTables().find(cells); ct != meta.configTables().end())
                cellRows = float(std::max(1, ct->second.cols));
            else if (const auto a1 = meta.arrays1d().find(cells); a1 != meta.arrays1d().end())
                cellRows = float(std::max(1, a1->second.count));
        }
        // Capped: a 32-point curve would otherwise grow the editor to 790px to show every cell, pushing the
        // page past its own height for a column of numbers. Longer arrays show what fits — every point is
        // still editable on the plot itself, by dragging it.
        const float cellsNeed = cellRows > 0.f
                              ? std::min((cellRows + 1.f) * lay.rowH * 0.72f, 12.f * lay.rowH * 0.72f) : 0.f;
        const float hh = std::max(h, gaugeH + (cellsNeed > 0.f ? lay.gap + cellsNeed : 0.f));
        // AGAINST THE CONTENT BOX, NOT THE PANEL. A panel hands its children its rect less a 2px inset on
        // each side, so a column placed at (w - sideW) ends exactly ON the panel's edge and four pixels
        // past the box the children actually get. Everything here is laid out inside `cw`.
        const float cw = w - 4.f;
        const float curveW = cw - sideW - lay.gap;
        const float ch     = hh - 4.f;      // the content box down the page, for the same reason as cw
        const float curveH = ch;

        jf::JJson curve = tableControl(t, curveW, curveH);
        jf::JJson p = element("panel", 0.f, 0.f, w, hh);
        p["props"]["layoutMode"] = std::to_string(int(Free));
        ++rep.widgets;
        curve["x"] = 0.0; curve["y"] = 0.0;
        jf::JJson kids = jf::JJson::array();
        kids.push(std::move(curve));

        if (haveGauge) {
            // The units go on the FACE, not into the title — see gaugeAssembly.
            jf::JJson dial = gaugeAssembly(g->second.title, bindingFor(g->second.channel, meta, dash), {},
                                           sideW, &g->second);
            dial["x"] = double(cw - sideW); dial["y"] = 0.0;
            kids.push(std::move(dial));
        }
        if (haveCells) {
            // widgetTypeFor, not a guess: a curve's cells are a one-row TABLE here, and an array1d editor
            // bound to one would have shown "drop a 1D array here" next to a curve that had its data.
            const float cellsY = haveGauge ? gaugeH + lay.gap : 0.f;
            jf::JJson cellsEl = control(Cache::instance().widgetTypeFor(cells), {}, cells, {},
                                        sideW, ch - cellsY);
            // axisMode 5 = "left & top", the UNtransposed layout: for a one-axis table that puts the single
            // strip DOWN the column (rows = the axis count). Transposing is what lays it out as a row.
            cellsEl["props"]["axisMode"] = "5";
            cellsEl["x"] = double(cw - sideW); cellsEl["y"] = double(cellsY);
            kids.push(std::move(cellsEl));
        }
        p["props"]["children"] = kids.dump();
        return p;
    }

    // A help topic as a page: its title on the panel, its text one label per line, its link at the bottom.
    jf::JJson helpPage(const tsdash::HelpTopic& h, float w, float x, float y) {
        jf::JJson p = element("panel", x, y, w, 0.f);
        prop(p, "labelText", h.title);
        p["props"]["layoutMode"] = std::to_string(int(Free));
        ++rep.widgets;
        const float cw = w - 2.f * lay.gap;
        jf::JJson kids = jf::JJson::array();
        float cy = lay.gap;
        auto row = [&](jf::JJson e) {
            e["x"] = double(lay.gap); e["y"] = double(cy);
            kids.push(std::move(e));
            cy += lay.rowH + lay.rowGap;
        };
        std::string line;
        // plainText first, so a <br> becomes the line break it was written as and getline splits on it —
        // the help page reads as the paragraphs its author wrote rather than one run-on line with tags in.
        std::istringstream in(plainText(h.text));
        while (std::getline(in, line)) {
            std::string url, caption;
            if (htmlLink(line, url, caption)) {
                jf::JJson e = control("label", caption, {}, {}, cw, lay.rowH);
                e["props"]["align"] = "Left";
                prop(e, "link", url);
                row(std::move(e));
            } else if (!line.empty() && line.find('<') == std::string::npos) {   // drop bare markup (an <img>)
                row(captionLabel(line, {}, cw, lay.rowH));
            }
        }
        if (!h.url.empty()) {
            jf::JJson e = control("label", h.url, {}, {}, cw, lay.rowH);
            e["props"]["align"] = "Left";
            prop(e, "link", h.url);
            row(std::move(e));
        }
        p["props"]["children"] = kids.dump();
        p["h"] = double(cy + lay.gap + (h.title.empty() ? 0.f : lay.rowH));
        return p;
    }

    // A table/curve editor in a panel that carries its title — what a TunerStudio table window looks like.
    jf::JJson titledEditor(const tsdash::TableRef& t, float w, float h, bool threeD = false) {
        // BUILD THE EDITOR FIRST, THEN THE PANEL AROUND IT. The panel used to be sized before the thing it
        // holds existed, and editorWithGauge grows past the height it is offered whenever the gauge and its
        // cells need more — so the panel came out 418 px tall around a composite of 442, and the bottom of
        // the map hung out of the frame that was supposed to contain it. A frame is measured from its
        // contents, not the other way round.
        jf::JJson e = editorWithGauge(t, w - 2.f * lay.gap, h - lay.rowH - 2.f * lay.gap);
        if (threeD && e["type"].str() == "table") e["props"]["view"] = "1";   // 1 = 3D surface
        // SAID ONCE. The panel about to be built around this carries the map's name in its title bar, and
        // the editor drew the same name again in its own — "Injector dead time Battery(x) vs Fuel
        // Pressure(y)" twice over, and clipped in both because neither bar is that wide.
        if (e.contains("props")) e["props"]["labelText"] = std::string();
        const float need = float(e["h"].number()) + lay.rowH + 2.f * lay.gap;
        // …AND AT LEAST AS WIDE AS ITS OWN NAME. A panel's title is drawn in its title bar and clipped to
        // it, so "Injector dead time Battery(x) vs Fuel Pressure(y)" in a 205 px bar loses half of itself
        // and the reader cannot tell which of the two deadtime maps they are looking at.
        const float titleW = estText(t.title) + 24.f;      // the bar's own inset each side
        jf::JJson p = element("panel", 0.f, 0.f, std::max(w, titleW), std::max(h, need));
        prop(p, "labelText", t.title);
        // BORDER, SO THE EDITOR FILLS THE WINDOW. Free keeps a child at the rect it was authored with, so
        // a table, a curve or a gauge sat at its imported size in the middle of however much room the
        // window had — while every plain settings row beside it widened. That is not what a table editor
        // does anywhere: in TunerStudio the grid takes the window, and the window is dragged to suit the
        // map being read. A Border centre is "everything left over", which is the same rule.
        p["props"]["layoutMode"] = std::to_string(int(Border));
        ++rep.widgets;
        e["x"] = double(lay.gap); e["y"] = double(lay.gap);
        jf::JJson kids = jf::JJson::array();
        kids.push(std::move(e));
        p["props"]["children"] = kids.dump();
        return p;
    }

    jf::JJson childFor(const tsdash::Item& it, int depth, float width = 0.f) {
        using K = tsdash::Item::Kind;
        const float w = width > 0.f ? width : lay.labelW + lay.controlW;
        switch (it.kind) {
            case K::Field: {
                const std::string b = bindingFor(it.var, meta, dash);
                if (b.empty() && it.var.empty()) {
                    if (it.label.empty()) return spacer(w);   // a spacer: the gap IS the content
                    return control("label", headerText(it.label), {}, it.condition, w, lay.rowH);   // header row
                }
                const std::string type = b.empty() ? std::string("field")
                                                   : Cache::instance().widgetTypeFor(b);
                return control(type.empty() ? "field" : type, headerText(it.label), b, it.condition, w, lay.rowH);
            }
            case K::Panel: {
                if (depth > 8) { ++rep.skippedItems; rep.skippedKinds["panel nested too deep"]++;
                                 return jf::JJson(); }
                if (const auto sub = dash.dialogs.find(it.panelId); sub != dash.dialogs.end()) {
                    jf::JJson nested = panelFor(sub->second, w, 0.f, depth + 1);
                    if (it.placement != "Center") prop(nested, "region", it.placement);
                    // A sub-panel's condition GREYS it, exactly like a field's. Hiding it instead was a
                    // guess, and the wrong one: a dialog can gate a whole region on a setting that is
                    // off by default, and TunerStudio still draws that region — curve, gauge and all —
                    // with its contents disabled. Hidden, the page simply
                    // lost its curve editor, which reads as a broken import.
                    prop(nested, "enableCondition", rewriteCond(it.condition, meta, dash));
                    return nested;
                }
                if (const auto ip = dash.panels.find(it.panelId); ip != dash.panels.end()) {
                    // A lamp grid: a panel in Grid mode with the declared column count.
                    // A lamp grid has to state its HEIGHT: a zero-height panel is an invisible one, which is
                    // how a border dialog's whole Center region ("Status") came out blank.
                    // Explicit rows and columns, NOT the Grid mode: a managed grid divides its container's
                    // height among the rows, and this panel is usually handed a whole border region — so
                    // five lamps came out 90px tall each where TunerStudio draws a compact block.
                    const int cols = std::max(1, ip->second.columns);
                    jf::JJson kids = jf::JJson::array();
                    const float cellW = (w - 2.f * lay.gap) / float(cols);
                    int i = 0;
                    for (const auto& lamp : ip->second.lamps) {
                        // A lamp NAMES both of its states ("cranking No" / "cranking Yes"); the widget's
                        // defaults are a bare ON/OFF, so without carrying the labels across every lamp in a
                        // grid read "OFF" and the panel said nothing about what was off.
                        jf::JJson e = lampFor(lamp, cellW - 2.f * kLampPad, lay.rowH - 2.f * kLampPad);
                        e["x"] = double(lay.gap + float(i % cols) * cellW + kLampPad);
                        e["y"] = double(lay.gap + float(i / cols) * lay.rowH + kLampPad);
                        kids.push(std::move(e));
                        ++i;
                    }
                    const int rows = (i + cols - 1) / cols;
                    jf::JJson p = element("panel", 0.f, 0.f, w, float(rows) * lay.rowH + 2.f * lay.gap);
                    p["props"]["layoutMode"] = std::to_string(int(Free));
                    ++rep.widgets;
                    p["props"]["children"] = kids.dump();
                    return p;
                }
                if (const auto rp = dash.readouts.find(it.panelId); rp != dash.readouts.end()) {
                    // A readout panel is a grid of LIVE values — the same shape as a lamp grid, with a
                    // caption and a value per row instead of a two-state lamp.
                    const int cols = std::max(1, rp->second.columns);
                    const float cellW = (w - 2.f * lay.gap) / float(cols);
                    int i = 0;
                    jf::JJson kids = jf::JJson::array();
                    for (const auto& r : rp->second.rows) {
                        // A readout names either a live channel or a GAUGE, exactly as `gauge =` does.
                        const std::string ch = dash.gaugeChannel(r.var);
                        std::string cap = r.title;
                        if (cap.empty()) {
                            const auto g = dash.gauges.find(r.var);
                            cap = g != dash.gauges.end() ? g->second.title : r.var;
                        }
                        if (!r.units.empty()) cap += "(" + r.units + ")";
                        jf::JJson e = control("field", cap, bindingFor(ch.empty() ? r.var : ch, meta, dash), {},
                                              cellW, lay.rowH);
                        e["x"] = double(lay.gap + float(i % cols) * cellW);
                        e["y"] = double(lay.gap + float(i / cols) * lay.rowH);
                        kids.push(std::move(e));
                        ++i;
                    }
                    const int rows = (i + cols - 1) / cols;
                    jf::JJson p = element("panel", 0.f, 0.f, w, float(rows) * lay.rowH + 2.f * lay.gap);
                    p["props"]["layoutMode"] = std::to_string(int(Free));
                    ++rep.widgets;
                    p["props"]["children"] = kids.dump();
                    return p;
                }
                if (const auto t = dash.tables.find(it.panelId); t != dash.tables.end())   // `panel = <tableId>`
                    return editorWithGauge(t->second, w,
                                           t->second.sizeH > 0.f ? t->second.sizeH
                                                                 : gridNaturalSize(t->second).second);
                ++rep.skippedItems;
                rep.skippedKinds["panel -> " + it.panelId]++;
                return jf::JJson();
            }
            case K::Gauge: {
                // `gauge = VSSGauge` names a GAUGE, and [GaugeConfigurations] is where its title and units
                // live. The dialog item usually carries no label of its own, so without looking the gauge up
                // the dial came out as an unlabelled dish.
                const auto  g  = dash.gauges.find(it.var);
                const std::string ch = dash.gaugeChannel(it.var);
                std::string title = it.label;
                if (title.empty() && g != dash.gauges.end()) title = g->second.title;
                const float gw = std::min(w, lay.gaugeW);          // square, and never wider than its panel
                jf::JJson assembly = gaugeAssembly(title, bindingFor(ch.empty() ? it.var : ch, meta, dash),
                                                   it.condition, gw,
                                                   g != dash.gauges.end() ? &g->second : nullptr);
                if (it.placement != "Center") prop(assembly, "region", it.placement);
                return assembly;
            }
            case K::Indicator: {
                jf::JJson e = control("indicator", it.label, bindingFor(it.var, meta, dash), it.condition, w, lay.rowH);
                prop(e, "offTitle", it.label);
                _lampOnTitle(e, it.onLabel, it.label);
                return e;
            }
            case K::Command: {
                // Never wider than the panel it sits in: a button authored at its preferred width inside a
                // narrow column hung over the edge and was clipped.
                jf::JJson e = control("command", it.label, {}, it.condition,
                                      std::min(w, lay.controlW), lay.rowH);
                prop(e, "command", it.var);
                // The BYTES, resolved here. A button carried only the command's NAME, which meant something
                // would have to read the ini again at click time to know what to send — and nothing does,
                // by design. [ControllerCommands] entries are raw instructions, so they cross over as hex.
                if (const auto c = dash.commands.find(it.var); c != dash.commands.end()) {
                    std::string hex;
                    for (const std::string& payload : c->second) {
                        if (!hex.empty()) hex += ",";
                        char b[4];
                        for (unsigned char ch : payload) { std::snprintf(b, sizeof b, "%02x", ch); hex += b; }
                    }
                    prop(e, "commandHex", hex);
                }
                for (const std::string& f : it.flags) {
                    if (f == "showMessageOnClick") prop(e, "showMessageOnClick", "1");
                    else if (f == "closeDialogOnClick" || f == "clickOnClose" || f == "clickOnCloseIfEnabled")
                        prop(e, f, "1");
                }
                prop(e, "message", it.message);
                return e;
            }
            case K::Text:
                if (it.label.empty()) return spacer(w);      // a spacer: the gap IS the content
                return control("label", headerText(it.label), {}, it.condition, w, lay.rowH);
            case K::LiveGraph: {
                jf::JJson e = control("livegraph", it.label,
                                      it.lines.empty() ? std::string() : bindingFor(it.lines.front(), meta, dash),
                                      it.condition, w, 180.f, it.placement);
                if (it.lines.size() > 1) {
                    std::string rest;
                    for (size_t i = 1; i < it.lines.size(); ++i) {
                        const std::string b = bindingFor(it.lines[i], meta, dash);
                        if (b.empty()) continue;
                        if (!rest.empty()) rest += ",";
                        rest += b;
                    }
                    prop(e, "lines", rest);
                }
                return e;
            }
            default:
                // A BLANK ITEM IS A SPACER, and it is the only "unconvertible" kind that carries meaning.
                // TunerStudio authors use an empty field as vertical whitespace between related rows, so
                // dropping them turned a dialog that reads as three groups of four into twelve rows
                // running together: the same fields, none of the shape their author gave them. An empty
                // label of one row is nothing to read and exactly the gap the ini asked for.
                if (it.label.empty() && it.var.empty() && it.panelId.empty()) return spacer(w);
                // Anything else really is unconverted, and is named rather than counted: "unrecognised
                // item" could be one keyword nobody implemented or a hundred deliberate spacers.
                ++rep.skippedItems;
                rep.skippedKinds["item kind not converted: '" + it.label + "'"]++;
                return jf::JJson();
        }
    }
};


// NOTHING LEAVES WIDER OR TALLER THAN THE BOX IT IS IN — the same rule the native side enforces in
// author.py's pack(), applied once here on the way out, where every widget from every emitter passes.
//
// A panel hands its children its rect less a 2 px inset on each side, and less the title band when it
// has a title. Emitters compute their children from the row height and the panel width, so a control
// authored at the full row height is two pixels past the box it is actually given, and one authored at
// the panel's full width is four past. Individually invisible; collectively they were 2608 of the
// document's own geometry complaints, and they are the difference between a layout that is right and
// one that merely looks right at the size it was drawn.
static constexpr float kFitPad = 4.f;      // PanelWidget's content inset, both axes
static constexpr float kFitTitle = 22.f;   // the title band a titled panel reserves
static constexpr double kFitSlack = 12.0;  // the biggest overflow that is an inset error rather than a design one
static void fitChildren(jf::JJson& ws, float boxW, float boxH) {
    if (!ws.isArray()) return;
    for (jf::JJson& w : ws.arr()) {
        if (!w.isObject()) continue;
        const double x = w["x"].number(0), y = w["y"].number(0);
        // A hair over is still over. The widths here come out of divisions (a row's share of its
        // container), so an exact fit lands a fraction past its box as often as not — 622.68 in a box of
        // 622 — and a checker that measures in pixels is right to say so. Clamp on any overflow, and
        // round the result down so the arithmetic cannot put it back.
        // ONLY THE INSET-SIZED OVERFLOWS. This exists to absorb the two-and-four-pixel errors that come
        // from an emitter computing a child from the row height or the panel width without subtracting
        // the frame it sits in. A child that wants HUNDREDS of pixels more than its box is a different
        // animal — a curve's gauge column needing 260 px in a 89 px slot — and shrinking it to fit does
        // not fix that page, it destroys it: the gauge, its scale and its cells all collapse to nothing.
        // Those are left alone, and left visible in the document's own geometry report, where they can be
        // fixed where they are made.
        const double slackW = x + w["w"].number(0) - double(boxW);
        const double slackH = y + w["h"].number(0) - double(boxH);
        if (boxW > 0.f && slackW > 0.0 && slackW <= kFitSlack) w["w"] = std::max(8.0, std::floor(double(boxW) - x));
        if (boxH > 0.f && slackH > 0.0 && slackH <= kFitSlack) w["h"] = std::max(8.0, std::floor(double(boxH) - y));
        // Ask for the string rather than asking whether the key is there: contains() answers about the
        // object it is called on, and a widget's children hang two levels down.
        const std::string kidsRaw = w["props"]["children"].str();
        if (kidsRaw.empty()) continue;
        auto kids = jf::JJson::tryParse(kidsRaw);
        if (!kids || !kids->isArray()) continue;
        const bool titled = !w["props"]["labelText"].str().empty();
        fitChildren(*kids, float(w["w"].number(0)) - kFitPad,
                           float(w["h"].number(0)) - kFitPad - (titled ? kFitTitle : 0.f));
        w["props"]["children"] = kids->dump();
    }
}
jf::JJson buildPage(const tsdash::Dashboard& dash, const MetaModel& meta, const Layout& lay,
                    const std::string& dialogId, const std::string& pagePath, Report& rep) {
    Converter conv{dash, meta, lay, rep};
    jf::JJson widgets = jf::JJson::array();

    const float pw = lay.pageW - 2 * lay.marginX;
    const float ph = lay.pageH - 2 * lay.marginY;
    float pageH = lay.pageH;                       // grows if the dialog is taller than one page
    float pageW = lay.pageW;                       // ...and wider, if its content is wider
    float contentBottom = 0.f, contentRight = 0.f; // where the dialog ends — the footer goes just there
    // Is this page ONE editor (a table, a curve, a 3D view)? Such a page is laid out the other way round
    // from a dialog: a dialog is a column of rows that keep their height and scroll when there are too
    // many, an editor is a single thing that should take the whole window and be dragged bigger when the
    // map wants reading. See the layout choice at the foot of this function.
    bool editorPage = false;

    if (const auto d = dash.dialogs.find(dialogId); d != dash.dialogs.end()) {
        // The root panel is sized to its CONTENT, not forced to the page: a dialog with three rows is three
        // rows tall (as in TunerStudio), and one with forty rows makes the page taller and scrolls, instead
        // of having its tail clipped off the bottom.
        //
        // Width is one dialog per COLUMN. A one-column dialog at the page width strands its controls in the
        // middle of an empty row; a two-column one squeezed into a single dialog width has no room left for
        // captions once each column has taken its control, and they came out clipped mid-word.
        const float rootW = std::min(lay.maxDialogW, conv.naturalWidth(d->second, 0));
        jf::JJson root = conv.panelFor(d->second, rootW, 0.f, 0);
        root["x"] = double(lay.marginX); root["y"] = double(lay.marginY);
        pageH = std::max(pageH, float(root["h"].number()) + 2.f * lay.marginY);
        pageW = std::max(pageW, rootW + 2.f * lay.marginX);
        contentBottom = lay.marginY + float(root["h"].number());
        contentRight  = lay.marginX + rootW;
        widgets.push(std::move(root));
    } else if (const auto t = dash.tables.find(dialogId); t != dash.tables.end()) {
        // A menu leaf that names a table/curve directly opens an EDITOR — in a titled window, as TunerStudio
        // opens one. Emitted bare, the page was an unlabelled grid with nothing to say what it edited.
        // Reached through the MAP id? That is TunerStudio's 3D view of the table, not its grid — the same
        // data, the other way of looking at it, and the widget already has the mode.
        // SIZED BY THE GRID, not by the page. A whole-page editor used to be emitted at the page size,
        // which made every map's window the same size as every other map's window: a 5x5 warmup correction
        // opened as big as a 16x16 ignition map, nearly all of it empty. It asks for what it holds now, and
        // the window opens around that; a map bigger than the cap still scrolls.
        const auto [gw, gh] = conv.gridNaturalSize(t->second);
        // …AND THE COLUMN BESIDE IT. A curve carries a gauge with its cells under it; asking only what
        // the GRID needs sized the page for a strip and left the column hanging hundreds of pixels out
        // of the page it is on. editorWithGauge builds that column from the same numbers.
        const float wantW = t->second.sizeW > 0.f ? t->second.sizeW : gw;
        const auto [sideW, sideH] = conv.sideColumn(t->second, wantW);
        const float ew = std::min(pw, wantW + (sideW > 0.f ? sideW + lay.gap : 0.f));
        const float eh = std::min(ph, std::max(gh, sideH) + lay.rowH);
        jf::JJson p = conv.titledEditor(t->second, ew, eh, /*threeD=*/!t->second.mapId.empty() &&
                                                                    dialogId == t->second.mapId);
        p["x"] = double(lay.marginX); p["y"] = double(lay.marginY);
        // THE PAGE IS AS BIG AS WHAT WENT ON IT. titledEditor may return something wider or taller than it
        // was offered — it grows for its own title bar and for a gauge column — and measuring the page
        // from what we ASKED for left the panel hanging over the edge of its own canvas.
        contentBottom = lay.marginY + float(p["h"].number());
        contentRight  = lay.marginX + float(p["w"].number());
        widgets.push(std::move(p));
        editorPage = true;      // its content is one editor, and an editor takes the window
        ++rep.tables;
    } else if (const auto hp = dash.helps.find(dialogId); hp != dash.helps.end()) {
        // A menu leaf may name a HELP TOPIC rather than a dialog. TS opens
        // its help window there; the page is that text, one label per line, with the link if it has one.
        // AS WIDE AS ITS WIDEST LINE. A help topic is prose and one URL per line; a fixed width cut the
        // tail off whichever line happened to be longest — and a URL with its tail cut off is not a link
        // any more, it is a wrong address.
        float helpW = lay.dialogW;
        {
            std::istringstream in(plainText(hp->second.text));
            std::string line;
            while (std::getline(in, line)) helpW = std::max(helpW, conv.estText(line) + 9.f * lay.gap);
            // The topic's URL is a field of its own, not part of the text — and it is the one line that
            // cannot survive being clipped, since half an address is a wrong address.
            if (!hp->second.url.empty())
                helpW = std::max(helpW, conv.estText(hp->second.url) + 9.f * lay.gap);
        }
        helpW = std::min({ pw, lay.maxDialogW, helpW });
        contentRight = lay.marginX + helpW;
        widgets.push(conv.helpPage(hp->second, helpW, lay.marginX, lay.marginY));
    }

    if (widgets.arr().empty()) {
        ++rep.emptyPages;
        rep.emptyNames.push_back(pagePath + "  (dialog '" + dialogId + "')");
    }

    // THE FOOTER TunerStudio puts on every dialog: [\xE2\x80\xB9] [\xE2\x80\xBA] [Burn]. Not decoration — it is what makes a
    // dialog somewhere you can try something. An edit is in the ECU's RAM immediately; flash is the
    // separate commit, so until Burn a reset undoes the lot, and the arrows walk back through THIS page's
    // own changes. No Close: these pages are docked views, not windows that open and shut.
    //
    // Placed at the bottom-right of the DIALOG, where TunerStudio puts it — not the bottom of the page,
    // which for a three-row dialog is most of a screen away from the thing it acts on. A help page has
    // nothing to undo or burn, so it gets none.
    if (contentBottom > 0.f) {
        // The style's button height, so a converted page's footer matches every other button in the app.
        const float bh = jf::JStyle::current().buttonHeight;
        const float bw = bh + 6.f, burnW = bh * 2.f, gap = 6.f;
        // A STRIP OF ITS OWN, not three loose widgets. The page is laid out with Border now, and every
        // top-level element without a region is Centre — three buttons emitted bare would be stacked on
        // top of the dialog rather than under it. Gathered into one panel with region South, they are a
        // strip of their own height across the page's foot, wherever the page's foot turns out to be.
        jf::JJson kids = jf::JJson::array();
        float x = 0.f;
        auto btn = [&](const char* action, float w) {
            jf::JJson b = element("tuneaction", x, 0.f, w, bh);
            b["props"]["action"] = std::string(action);
            b["props"]["scope"]  = pagePath;     // the page whose history the arrows walk
            ++rep.widgets;
            x += w + gap;
            return b;
        };
        kids.push(btn("undo", bw));
        kids.push(btn("redo", bw));
        kids.push(btn("burn", burnW));
        // TALL ENOUGH TO HOLD A BUTTON. A panel insets its content by 2px top and bottom (PanelWidget's
        // content rect: height - 4), so a strip sized to exactly the button height handed its buttons
        // four pixels less than they need and cut their bottoms off.
        const float footChrome = 4.f;
        jf::JJson foot = element("panel", 0.f, 0.f, x, bh + footChrome);
        foot["props"]["layoutMode"] = std::to_string(int(Free));
        // On an editor page the footer is the South strip and the editor is the centre, so the editor
        // gets every pixel the buttons do not. On a dialog page the column stacks them and no region is
        // wanted — one there would take the footer out of the flow it belongs to.
        if (editorPage) prop(foot, "region", "South");
        foot["props"]["children"]   = kids.dump();
        ++rep.widgets;
        widgets.push(std::move(foot));
        pageH = std::max(pageH, contentBottom + gap + bh + footChrome + lay.marginY);
        contentBottom = contentBottom + gap + bh + footChrome;   // the footer is now the lowest thing on it
    }

    jf::JJson page = jf::JJson::object();
    // THE PAGE IS THE VIEW, NOT A CANVAS.
    //
    // A converted dialog is a column of label/value rows. Given a fixed authored width it can only ever
    // be squeezed or scrolled into a window: at 838 wide in a 1458-wide viewport it sat as an island in
    // the middle with each label a third of a screen from its own value, and a tall one (940 in the 590
    // left under the status lamps) was shrunk to 0.63 until its labels were six pixels high. Neither is
    // a layout problem the renderer can solve — the page had no layout, it had coordinates.
    //
    // So it declares canvasStatic 3: it takes the area it is given, at the interface scale, and arranges
    // itself into it. Border does the arranging — the dialog is the Centre and takes the width and what
    // height is left; the footer is a South strip of its own height. More rows than fit scroll, which is
    // the honest answer to a small window and the one an absolute canvas could never give.
    //
    // The authored extent is still written, because it is what the page WANTS: a reflowing page ignores
    // it, and anything that opens this document as a plain canvas still gets a sane size rather than a
    // preference it was never laid out against.
    // The authored extent is still written: a reflowing page ignores it, but anything that opens this
    // document as a plain canvas gets a sane size rather than a preference it was never laid out against.
    const float canvasW = contentRight  > 0.f ? contentRight  + lay.marginX : pageW;
    const float canvasH = contentBottom > 0.f ? contentBottom + lay.marginY : pageH;
    page["canvasWidth"]  = double(canvasW);
    page["canvasHeight"] = double(canvasH);
    // AND THE WIDTH IT REFUSES TO GO BELOW. Reflow answers a small window by handing the layout less to
    // work with, which is right until there is not enough for a caption and its control to sit side by
    // side: past that the rows do not get narrower, they get unreadable — captions clipped mid-word,
    // controls down to slivers. This is that point, and it is the same number the page was laid out to,
    // so below it the surface scrolls instead. A lamp grid states no minimum and reflows all the way
    // down; it has columns to give up, not rows to ruin.
    page["minWidth"]     = double(canvasW);
    page["canvasStatic"] = 3.0;   // reflow: the page is the view
    // COLUMN, not Border. Border's Centre is "whatever is left", so it STRETCHES the dialog panel to the
    // container — and that panel arranges its 57 children by scaling them into its own box, so a page
    // given less height than it was authored with shrank every row inside it. Squashing by another name.
    //
    // Column gives the panel the full WIDTH and its NATURAL height. Rows widen into the space (their
    // text does not stretch, only their boxes, which is what puts a value at the right-hand end of its
    // own row) and keep the height they were authored at. Taller than the view simply scrolls. The
    // footer is the next child down, so it needs no region.
    // COLUMN for a dialog, BORDER for an editor. Column gives every child the full width and its own
    // natural height, which is what a stack of settings rows wants — and exactly what a table does not:
    // its "natural height" is whatever the importer guessed, so the grid sat that tall in a window twice
    // the size with dead space under it. Border hands the centre everything the South strip does not use,
    // so the editor grows with the window on both axes, the way it does in TunerStudio.
    page["layout"]       = editorPage ? 3.0 : 8.0;   // 3 = Border, 8 = Column
    page["guideW"] = 0.0; page["guideH"] = 0.0;
    page["widgets"] = std::move(widgets);
    return page;
}

}  // namespace

jf::JJson buildDashboard(const tsdash::Dashboard& dash, const MetaModel& meta,
                         Report* report, const Layout& layIn) {
    Report local;
    Report& rep = report ? *report : local;
    rep = {};
    nextId = 1;

    // A dialog never grows past the page it is drawn on; the page itself comes from Preferences.
    Layout lay = layIn;
    // THE CAP, AND ONLY THE CAP. A dialog takes its natural width from the ini; this is the most it may
    // grow to, and it is the one thing here the user's own page-width preference decides. A dialog
    // narrower than the cap is unaffected by it, which is nearly all of them — so the same .ini gives
    // the same document on any machine unless a dialog is genuinely wider than someone's page.
    // A CONSTANT, NOT A PREFERENCE. This is the widest a converted dialog may grow to, and it used to be
    // the user's page-size setting — so the same .ini produced a different document on a different
    // machine, and 26 of these 303 pages changed with it. A definition is not a display setting: what a
    // dialog needs is a fact about the dialog, and how much of it fits on screen is now the page's
    // business, since a page reflows to the window and scrolls when it must.
    if (lay.maxDialogW <= 0.f) lay.maxDialogW = std::max(lay.dialogW, kMaxDialogW);

    jf::JJson tree    = jf::JJson::array();
    jf::JJson library = jf::JJson::object();

    // The menu structure becomes the navigation tree; every LEAF that names a dialog (or a table) becomes
    // a page in the library, keyed by its "/"-joined path — the same key the app resolves a node's page by.
    std::function<jf::JJson(const tsdash::MenuNode&, const std::string&)> node =
        [&](const tsdash::MenuNode& m, const std::string& parentPath) {
            // The library key is the DISPLAYED path, so it must use the same stripped label the tree node
            // shows — otherwise a node looks for "Setup/..." while its page is filed under "&Setup/...".
            const std::string label = stripMnemonic(m.label);
            const std::string path  = parentPath.empty() ? label : parentPath + "/" + label;
            jf::JJson o = jf::JJson::object();
            o["name"]     = label;
            o["expanded"] = false;
            if (!m.condition.empty()) o["condition"] = rewriteCond(m.condition, meta, dash);
            if (!m.children.empty()) {
                jf::JJson kids = jf::JJson::array();
                for (const auto& c : m.children) kids.push(node(c, path));
                o["children"] = std::move(kids);
            } else if (!m.dialogId.empty()) {
                library[path] = buildPage(dash, meta, lay, m.dialogId, path, rep);
                ++rep.pages;
            }
            return o;
        };

    for (const auto& m : dash.menus) tree.push(node(m, ""));

    // A page in the library is not visible by itself: a surface shows one through a VIEWPORT element
    // tagged with the node, and the Surface renders only the viewport whose node is active. Converting
    // 273 pages without them produced a tree that selected nodes and a canvas that never changed.
    //
    // So the Main surface gets one viewport per page, all on the same rect — exactly what dragging every
    // node out of the tree by hand would produce, minus the afternoon.
    jf::JJson mainWidgets = jf::JJson::array();

    // [FrontPage] status lamps belong to the WINDOW, not to any page: TunerStudio keeps them along the
    // bottom whichever dialog you are in. So they go straight onto the tab canvas, OUTSIDE every viewport —
    // the viewports change with the tree, this strip does not. The viewports give up the height it takes.
    Converter fp{dash, meta, lay, rep};
    // The strip is a WRAP panel: the lamps keep their size and flow to as many rows as the width needs, so a
    // definition with more (or fewer) of them, or a resized canvas, re-flows at LAYOUT time instead of
    // carrying a row/column count baked in here. The reserved height still has to be authored, so it is the
    // rows the lamps need at this width.
    const int    kOwnLamps = 2;                 // the studio's own: Data Logging, Protocol Error
    const size_t lampCount = dash.frontIndicators.empty() ? 0 : dash.frontIndicators.size() + kOwnLamps;
    const float  stripW    = lay.pageW - 2 * lay.marginX;
    // A CELL IS AS WIDE AS THE WIDEST THING THAT GOES IN ONE, and then the row is shared out evenly.
    //
    // The width used to be a flat 190 px, which is a number about no definition in particular. This ini's
    // longest lamp is "Not decel fuel cut" and wants 130; another ini's could want 300 and be clipped, or
    // 60 and waste half the strip. And 190 into the strip leaves a hundred pixels of nothing on the right,
    // so the grid ends in mid-air — six columns and then a hole.
    //
    // So: measure what the definition actually carries (both states, since a lamp shows either), take as
    // many columns as fit, then divide the width EXACTLY between them. The grid reaches both edges, and a
    // definition with different lamps gets a different grid rather than a clipped one.
    float widest = 0.f;
    for (const auto& l : dash.frontIndicators) widest = std::max(widest, fp.lampTextW(l));
    widest = std::max({ widest, fp.estText("Data Logging"), fp.estText("Protocol Error") });
    const int   lampCols = std::max(1, int(stripW / std::max(90.f, widest + 2.f * lay.gap)));
    const float lampW    = stripW / float(lampCols);
    const int   lampRows = int((lampCount + lampCols - 1) / lampCols);
    const float stripH   = lampRows > 0 ? float(lampRows) * lay.rowH + lay.gap : 0.f;
    // THE PAGE KEEPS ITS FULL HEIGHT. The strip used to be laid on the tab canvas beneath the viewport,
    // so every page in the document gave up 150 px to it whether the reader wanted the lamps or not, and
    // there was no way to put that height back. It is a PAGE of its own now, hosted in a dock the reader
    // can size, hide or close like any other.
    const float  vpH       = lay.pageH - 2 * lay.marginY;

    // NO VIEWPORTS ON THE MAIN SURFACE. This used to hold one per page, all on the same rect, and the
    // Surface drew whichever one the tree had selected — that WAS the page view. A page opens in a window
    // of its own now, so a viewport here draws the same page a second time, behind the window showing it:
    // the same controls in two places, either of which could be clicked. What belongs on this surface is
    // what belongs BEHIND a window — live readouts — with the middle clear for the window to land in.
    (void)vpH;

    if (lampRows > 0) {
        jf::JJson strip = element("panel", 0.f, 0.f, stripW, stripH);
        strip["props"]["layoutMode"] = std::to_string(int(Wrap));
        strip["props"]["showBorder"] = "0";                 // a strip, not a box
        jf::JJson lamps = jf::JJson::array();
        // EACH LAMP IN ITS OWN CELL, INSET. A lamp fills its whole rect with the state colour, and Wrap
        // packs children edge to edge — so two lit neighbours became ONE bar: "Fan on" and "Fan 2 on" ran
        // together into a single green slab, and "CLT error" and "IAT error" into a single red one, with
        // nothing to say they were two faults rather than one wide one. The cell carries the gutter, so
        // the flow still reflows on width and every lit lamp is its own object.
        auto cell = [&](jf::JJson lamp) {
            jf::JJson c = element("panel", 0.f, 0.f, lampW, lay.rowH);
            c["props"]["layoutMode"] = std::to_string(int(Free));
            c["props"]["showBorder"] = "0";
            // THE CELL'S CONTENT BOX, not its outer rect. A panel hands its children w-4 and h-4, so a
            // lamp inset by the gutter on both sides sits one pixel past the box it is actually given —
            // invisible on screen, and a real overflow the moment anything measures it.
            lamp["x"] = double(kLampPad); lamp["y"] = double(kLampPad);
            lamp["w"] = double(lampW - kLampPad - kPanelInset);
            lamp["h"] = double(lay.rowH - kLampPad - kPanelInset);
            jf::JJson one = jf::JJson::array(); one.push(std::move(lamp));
            c["props"]["children"] = one.dump();
            ++rep.widgets;
            return c;
        };
        for (const auto& l : dash.frontIndicators) lamps.push(cell(fp.lampFor(l, lampW, lay.rowH)));
        // TunerStudio's strip ends with two lamps of its OWN — they report on the TOOL, not the controller,
        // so no definition can describe them and ours have to come from the studio's state (the % channels).
        // Data Logging stays off until there is a recorder to turn it on; the lamp is here so the strip is
        // the strip, and so that recorder has somewhere to report.
        for (const auto& [expr, off, on] : { std::tuple<const char*, const char*, const char*>
                                                 { "%logging",   "Data Logging",   "Logging" },
                                             std::tuple<const char*, const char*, const char*>
                                                 { "%linkError", "Protocol Error", "Protocol Error" } }) {
            tsdash::IndicatorLamp l;
            l.expr = expr; l.offLabel = off; l.onLabel = on;
            l.offBg = "white"; l.offFg = "black"; l.onBg = "red"; l.onFg = "black";
            lamps.push(cell(fp.lampFor(l, lampW, lay.rowH)));
        }
        strip["props"]["children"] = lamps.dump();
        // A PAGE OF ITS OWN, under a key no menu can produce. The strip used to be a widget on the tab
        // canvas: fixed there, unhideable, and charging every page in the document the height it took.
        // As a page it can be hosted in a dock — sized, hidden or closed like the Diagnostics dock — and
        // it reflows, so the grid re-wraps to whatever width the dock is given rather than to the width
        // the importer guessed.
        //
        // The '%' is what keeps it out of the way: a menu path is built from labels, so no [Menu] entry
        // can ever collide with this key, and no tree node points at it.
        jf::JJson statusPage = jf::JJson::object();
        statusPage["canvasWidth"]  = double(stripW);
        statusPage["canvasHeight"] = double(stripH);
        statusPage["canvasStatic"] = 3.0;                        // reflow: the page is the view
        statusPage["layout"]       = double(int(Column));        // the strip takes the width, its own height
        statusPage["guideW"] = 0.0; statusPage["guideH"] = 0.0;
        jf::JJson stripWidgets = jf::JJson::array();
        stripWidgets.push(std::move(strip));
        statusPage["widgets"] = std::move(stripWidgets);
        library[kStatusPage] = std::move(statusPage);
    }

    jf::JJson mainModel = jf::JJson::object();
    mainModel["canvasWidth"]  = double(lay.pageW);
    mainModel["canvasHeight"] = double(lay.pageH);
    // THE TAB TAKES THE WINDOW, AND THE VIEWPORT TAKES THE TAB.
    //
    // This surface used to be a fixed 1280x800 canvas, so a wider window showed a page-shaped card
    // floating in the middle of it with dead space all round — and the pages inside, which reflow, were
    // reflowing into 1280 rather than into the window.
    //
    // It is REFLOW now, not scale-to-fit: the distinction matters and is the whole reason the old note
    // here refused. Scale-to-fit multiplies coordinates, so text shrinks with the box; reflow hands the
    // area to the layout and the rows arrange into it at their own size. Nothing here is scaled.
    //
    // CARD, because the children are 303 viewports stacked on one another with the active node's shown.
    // Column would give each of them a slot of its own and stack them down the page; Card gives every one
    // the whole page and lets the run-mode gate pick. The strip that used to be pinned here beside them
    // is in a dock now, so there is nothing left on this surface that wants a fixed coordinate.
    mainModel["canvasStatic"] = 3.0;                 // reflow: the surface is the view
    mainModel["layout"]       = double(int(Card));   // every viewport fills it; the active one shows
    mainModel["guideW"] = 0.0; mainModel["guideH"] = 0.0;
    mainModel["widgets"] = std::move(mainWidgets);

    // MAINTAINED CONSTANTS — config values the tuner keeps up to date from an expression (spec:
    // [ConstantsExtensions] maintainConstantValue). Rewritten into the studio's spelling here, where the
    // meta is available to tell a config path from a channel, and evaluated live while connected.
    jf::JJson maintained = jf::JJson::array();
    for (const auto& mc : dash.maintained) {
        const std::string target = bindingFor(mc.target, meta, dash);
        if (target.empty()) { rep.skippedKinds["maintainConstantValue -> " + mc.target]++; continue; }
        jf::JJson m = jf::JJson::object();
        m["target"] = target;
        m["expr"]   = rewriteExpr(mc.expr, meta, dash);      // ternary → select(), names → paths
        maintained.push(std::move(m));
    }

    jf::JJson entry = jf::JJson::object();
    entry["name"]  = std::string("Main");
    entry["model"] = std::move(mainModel);
    jf::JJson pool = jf::JJson::array(); pool.push(std::move(entry));
    jf::JJson open = jf::JJson::array(); open.push(0.0);
    jf::JJson surfaces = jf::JJson::object();
    surfaces["pool"] = std::move(pool);
    surfaces["open"] = std::move(open);
    surfaces["active"] = 0.0;

    jf::JJson doc = jf::JJson::object();
    doc["tree"] = std::move(tree);
    doc["surfaces"] = std::move(surfaces);
    // THE FIT PASS RUNS HERE, over the finished library. Inside buildPage it ran before some geometry had
    // settled — a composite is re-placed and re-sized by the panel that adopts it — so the nodes that
    // needed it most were measured against boxes that changed afterwards. A page is only really a page
    // once it is in the library, which is exactly where author.py does the same thing on the native side.
    for (auto& [key, page] : library.obj()) {
        (void)key;
        if (!page.isObject() || !page.contains("widgets")) continue;
        fitChildren(page["widgets"], float(page["canvasWidth"].number(0)),
                                     float(page["canvasHeight"].number(0)));
    }
    doc["panelLibrary"] = std::move(library);
    if (!maintained.arr().empty()) doc["maintained"] = std::move(maintained);
    return doc;
}

}  // namespace tsconvert

#pragma once

// CanvasWidget — the base class for every canvas widget. It DERIVES from jf::JWidget, which supplies
// geometry in the scene graph, focus/hit-test/paint, and native change signals (onModified /
// onGeometryChanged / onFocusChanged live on the base).
//
// The port had first flattened widgets into render LAMBDAS over a stringly-typed PanelElement
// prop bag. This class restores the real hierarchy: ONE base owns the TYPED skin/data members and paints the
// frame in ONE place — drawBackground() + a single virtual drawBorder(). Concrete widgets subclass it and
// override render() for their content. The border pixels are emitted by ONE static painter (paintBorder), so
// the base's instance drawBorder() and the transitional free widgetBorder() (which can't construct a
// non-copyable JWidget) both draw the identical frame — no scattered per-descriptor border code.
//
// Staging: this brings the skin cascade, value/binding data, value ranges, resolved accessors, the frame, and
// the content hook onto the JWidget base. Still deferred to a later stage (marked "Stage 2:" below): the
// Surface driving persistent instances through populateRenderPrimitives (retiring the descriptor lambdas +
// widgetBorder), per-element visibility/enable conditions (no NodeCondition in studio-jf yet), the in-place
// label editor, JSON save/load, propertyMetaData (collectProperties already covers the inspector),
// and the single-inheritance global-default overrides.

#include <j/core/JWidget.h>          // base class + native signals + geometry (+ JKeyEvent)
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>       // jf::jResolveFontFace — the fontSpecFace helper
#include <j/graphics/RenderPrimitive.h>
#include "PanelModel.h"              // PanelElement — the serialized element the skin/data members mirror
#include "../model/RangeBands.h"     // compact colour-rule parse (ColorRules::fromCompact)
#include "../model/MathEvaluator.h"  // ElementScope — this widget's element, for its whole paint
#include "../model/MetaModel.h"      // MetaModel::Location — what a bound control reads and writes
#include "../model/Units.h"         // Raw / Eng — which unit a number is in, as a type

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class Cache;   // const Cache& flows through render() / sigilValue() / the shared value helpers (was fwd-declared in WidgetSupport)

// ------------------------------------------------------------------------------------------------------------
// Widget-authoring shared surface (moved here from the deleted WidgetSupport, so a widget needs only its base
// header). The app-service hooks the run-mode input reaches, the skin cascade, the font-spec helpers, and the
// run-mode ControlInput event — all part of writing a CanvasWidget subclass, so all one include now.
// ------------------------------------------------------------------------------------------------------------

// Run-mode pickers for enum / signal-selector controls. A widget's onControlInput has no window, so it asks
// the app through these hooks (wired in main.cpp): a *_src SIGNAL selector opens the searchable signal picker;
// a plain enum opens a compact option menu.
namespace enumpick {
    using SignalFn = std::function<void(std::vector<std::string> paths, std::string current,
                                         std::function<void(std::string)> onPick)>;
    inline SignalFn& signal() { static SignalFn f; return f; }
    // `disabled` (parallel to labels, may be empty) greys an option instead of dropping it: a pin already
    // taken by another sensor still SHOWS, saying who has it, rather than silently not being in the list.
    using MenuFn = std::function<void(std::string title, std::vector<std::string> labels, int cur,
                                      int sx, int sy, std::function<void(int)> onPick,
                                      std::vector<uint8_t> disabled)>;   // sx/sy: window-abs click, anchors the popup
    inline MenuFn& menu() { static MenuFn f; return f; }
    // The CHANNEL SET of a multi-channel control (a watch list, a trace view) — the two-list picker, opened
    // from the control's own context menu. Not the same question as enumpick::signal(): that one asks which
    // ONE signal a binding reads, this one asks which channels a control shows AND IN WHAT ORDER, which is
    // why it hands back a list rather than a name. `onPick` is not called if the user cancels.
    using ChannelsFn = std::function<void(std::string title, std::vector<std::string> current,
                                          std::function<void(std::vector<std::string>)> onPick)>;
    inline ChannelsFn& channels() { static ChannelsFn f; return f; }
    // The PROPERTIES of the channels a control is showing — unit, scale, warning and alarm bands. Bands
    // belong to the channel rather than to the control (ChannelPrefs), so this hands over the channel
    // list and the dialog writes straight into that shared store; the callback only says "it changed".
    using ChanPropsFn = std::function<void(std::string title, std::vector<std::string> channels,
                                           std::function<void()> onChanged)>;
    inline ChanPropsFn& channelProps() { static ChanPropsFn f; return f; }
}

// App-wired RAW CONTROLLER COMMAND sender. An imported TunerStudio project's buttons carry byte payloads
// ([ControllerCommands]) rather than a CLI line, and a widget has no link of its own — the app owns the
// connection and decides which protocol it is speaking. Returns false if nothing could be sent.
namespace tscmd {
    using Fn = std::function<bool(const std::vector<std::vector<uint8_t>>& payloads)>;
    inline Fn& send() { static Fn f; return f; }
}

// App-wired BURN. An edit reaches the ECU's RAM as it is made; flash is a separate, deliberate commit,
// and until it happens a reset takes the tune back to what was last burned. That is the whole point of a
// burn button — an experiment you can throw away — so nothing here burns implicitly. `pending()` answers
// "are there unburned changes?", which is what greys the button out.
namespace tuneburn {
    using BurnFn    = std::function<bool()>;
    using PendingFn = std::function<bool()>;
    inline BurnFn&    burn()    { static BurnFn f;    return f; }
    inline PendingFn& pending() { static PendingFn f; return f; }
}

// App-wired message box for controls that notify (the command button's showMessageOnClick).
namespace appmsg {
    using Fn = std::function<void(std::string title, std::string text)>;
    inline Fn& show() { static Fn f; return f; }
}

// Skin cascade — a control's colour/font resolve LOCAL (an element prop, "#rrggbb") → SCHEME (JStyle). An
// empty/invalid prop means "inherit the scheme". Every control's render pulls its colours through here.
namespace skin {
inline bool parseHex(const std::string& s, uint8_t out[4]) {
    if (s.size() != 7 || s[0] != '#') return false;
    auto hx = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) { v[i] = hx(s[i + 1]); if (v[i] < 0) return false; }
    out[0] = uint8_t(v[0] * 16 + v[1]); out[1] = uint8_t(v[2] * 16 + v[3]);
    out[2] = uint8_t(v[4] * 16 + v[5]); out[3] = 255;
    return true;
}
inline const uint8_t* color(const PanelElement& el, const std::string& key, const uint8_t* fallback, uint8_t buf[4]) {
    return parseHex(el.prop(key), buf) ? buf : fallback;
}
inline int num(const PanelElement& el, const std::string& key, int dflt) {
    const std::string s = el.prop(key);
    if (s.empty()) return dflt;
    try { return static_cast<int>(std::stod(s)); } catch (...) { return dflt; }
}
}  // namespace skin

// Font size in PIXELS from a "family|size|b|i" font spec. Size is field 1; empty/absent/≤0 → dflt.
inline float fontSpecPx(const std::string& spec, float dflt) {
    const size_t a = spec.find('|');
    if (a == std::string::npos) return dflt;
    const size_t b = spec.find('|', a + 1);
    try { const float v = std::stof(spec.substr(a + 1, b == std::string::npos ? std::string::npos : b - (a + 1)));
          return v > 0.f ? v : dflt; }
    catch (...) { return dflt; }
}

// Concrete FACE-FILE path for a "family|size|b|i" spec (family field 0, bold field 2 == "1", italic field 3 ==
// "1"), resolved through jf::jResolveFontFace and memoised per spec string. "" → the app default face.
inline std::string fontSpecFace(const std::string& spec) {
    static std::unordered_map<std::string, std::string> s_cache;
    if (const auto it = s_cache.find(spec); it != s_cache.end()) return it->second;
    std::string family, bold, italic;
    const size_t a = spec.find('|');
    family = (a == std::string::npos) ? spec : spec.substr(0, a);
    if (a != std::string::npos) {
        const size_t b = spec.find('|', a + 1);
        if (b != std::string::npos) {
            const size_t c = spec.find('|', b + 1);
            bold = spec.substr(b + 1, c == std::string::npos ? std::string::npos : c - (b + 1));
            if (c != std::string::npos) italic = spec.substr(c + 1);
        }
    }
    std::string face = jf::jResolveFontFace(family, bold == "1", italic == "1");
    s_cache.emplace(spec, face);
    return face;
}

// Run-mode interaction event routed to an interactive control. The surface delivers these to the element under
// the cursor — or, for Key events, to the element that last took focus. `screen` (passed to onControlInput) is
// the element's window-absolute rect. onControlInput returns true if it consumed the event.
struct ControlInput {
    // Blur — "you are no longer the focused control". Sent to the OUTGOING control when the Surface's
    // activeControl_ moves on, because a hosted framework control cannot learn that any other way: it lives
    // outside the window's widget tree, so the framework's own focus chain never contains it.
    // Focus — "you are now the focused control", sent when the keyboard reaches a control WITHOUT a click
    // (Tab). A text-editing control shows its caret and selects its value, as tabbing into a field does.
    enum class Kind { Press, Move, Release, Scroll, Key, Blur, Focus } kind = Kind::Press;
    float mx = 0.f, my = 0.f;                 // window-absolute cursor (Press/Move/Release/Scroll)
    float wheel = 0.f;                        // Scroll delta
    const jf::JKeyEvent* key = nullptr;       // Key events
    bool  focused = false;                    // is this element the keyboard-focused control?
};

// The default cascade: an element's own props over editor-defaults over prototype-defaults (WidgetRegistry.cpp).
// Forward-declared so a widget can resolve its OWN override set (m_edit) into its render copy (m_data) without
// pulling in WidgetRegistry (which includes this header).
PanelElement resolveElement(const PanelElement& el);

class CanvasWidget : public jf::JWidget {
public:
    explicit CanvasWidget(jf::JSceneGraph& graph, const std::string& name = "")
        : jf::JWidget(graph, name) {}

    // ------------------------------------------------------------------
    // Data (CanvasElement parity). Geometry is NOT re-declared — it lives on JWidget (bounds()/setBounds/
    // setPos/setSize + the inherited onGeometryChanged). The bound value is sampled into m_value each frame by
    // the surface; ranges/paint read it without touching the Cache. onModified is inherited from JWidget.
    // ------------------------------------------------------------------
    double      m_value = 0.0;            // last sampled bound value, RAW (as the bus holds it)
    double      m_dispValue = 0.0;        // ... and in DISPLAY units, which is what a rule compares (setValue)
    std::string m_signalName;            // data source (channel name / expression)
    std::string m_displayUnit = "Auto";
    double      m_scale = 1.0;            // the field's meta scale: shown = value * scale, edit inverts.
                                          // Not a widget property any more — see scaleOf().
    std::string m_uid;
    std::string m_visibleExpr;   // "" = always visible  (element prop "condition")
    std::string m_enabledExpr;   // "" = always enabled  (element prop "enableCondition")
    std::string m_minExpr, m_maxExpr;   // control bounds (element props "minExpr"/"maxExpr") — narrow only
    std::string m_elemContext;           // host viewport's element ("clt") — resolves a template's "[*]"

    double value() const { return m_value; }
    // A RULE'S `value` MEANS WHAT THE CONTROL SHOWS, not what the bus holds. m_value is the raw bound
    // reading; every gauge draws dispV(value) -- the field's scale applied, then converted to the display
    // unit -- so a rule tested against the raw number meant something else entirely the moment either
    // differed. Author "value > 100" against a gauge reading in °F, or against a field stored ×0.1, and
    // the band landed nowhere near where it was drawn. Computed ONCE per frame here, because setValue is
    // the one call that happens exactly once per widget per frame, and dispV does Cache lookups that
    // activeRange() would otherwise repeat for every colour channel it resolves.
    virtual void setValue(double v) { m_value = v; m_dispValue = m_el ? dispV(*m_el, Raw{v}) : v; }
    double displayValue() const { return m_dispValue; }
    const std::string& signalName() const { return m_signalName; }   // AUTHORED, placeholder and all
    virtual void setSignalName(const std::string& n) {
        if (m_signalName == n) return;
        m_signalName = n; emitModified();
    }

    // ---- TEMPLATE PAGES ------------------------------------------------------------------------
    // A page whose bindings name the array ELEMENT as "[*]" — sensors.sensor[*].enabled — is a TEMPLATE:
    // one layout, placed by many viewports, each of which supplies the element it is about through its own
    // Data Source. Draw the group's page once and assign it to every sensor in the group, instead of a
    // near-identical page per sensor that has to be re-pointed by hand and re-fixed on every layout change.
    //
    // The host viewport pushes its element down to the widgets it mirrors; bindPath() is what every consumer
    // reads. signalName() stays AUTHORED so the inspector still shows "[*]" rather than one resolution of it.
    void setElementContext(const std::string& key) { m_elemContext = key; }
    const std::string& elementContext() const { return m_elemContext; }

    // The binding with "[*]" resolved against this widget's host element. Unchanged when there is no
    // placeholder (the ordinary case) or no host — an unresolved template renders as the dead path it is,
    // rather than silently reading some other element's value.
    std::string bindPath() const { return resolveTemplate(m_signalName, m_elemContext); }

    // The FIELD this binding names, as a concrete path — "[*]" resolved and every "[@expr]" pinned to
    // element 0 (see MathEvaluator::fieldPath). Field metadata (decimals/unit/range) is identical across
    // elements, so a cell can still format itself when the live index is unresolved: an unused firing-order
    // slot's Bank cell shows "0" (0 decimals), not a generic "0.00", though its VALUE stays blank.
    std::string fieldPath() const {
        return MathEvaluator::instance().fieldPath(MathEvaluator::resolveTemplate(m_signalName, m_elemContext));
    }

    // Is this widget enabled / shown, right now? The widget ANSWERS rather than handing back a string,
    // because answering means evaluating, and evaluating is what has to happen inside this widget's
    // element scope — a template page's expressions are about the element its viewport is showing.
    //
    // "[*]" used to be substituted per PROPERTY, and only into signalName, so a template page bound
    // correctly and gated on a placeholder the evaluator could not resolve. It is resolved once now,
    // where a path becomes a value (MathEvaluator::ElementScope), so every expression property gets it
    // — a condition, a row count, a data source — without any of them knowing about templates.
    bool enabledNow() const;

    // A visibility condition, same treatment — a template page's "shown when" has to be about the element
    // the viewport is showing, not about the placeholder.
    bool visibleNow() const;
    // …and the one thing it decides on its own: whether it is shown for the KIND of calibration it binds.
    // A switch has none (nothing reads a curve for it), a multi-position switch's bytes are voltage bands
    // edited by the band editor — and a curve, a table or a caption drawn over either says the reading is
    // shaped by something it is not. The default answer is the curve's: an ordinary calibration only. A
    // widget built for one of the others (BandsWidget) says so by overriding it.
    virtual bool showsCalibrationOf(const std::string& typeId) const {
        return typeId != "switch" && typeId != "multi_switch";
    }
    // The sensor TYPE whose calibration this element binds ("switch", "multi_switch", …), or "" when the
    // element binds no sensor calibration at all.
    std::string _calibrationTypeOf() const;

    // Error condition — same element-scope treatment. True -> the widget paints its background the error

    // ONE substitution, on the evaluator — see MathEvaluator::resolveTemplate. A second copy here is how
    // a builder-produced context resolved bindings and not expressions. Also resolves any "[@expr]"
    // indexed subscript (resolveIndexed) so bindPath() hands every consumer a concrete literal path.
    static std::string resolveTemplate(const std::string& binding, const std::string& key) {
        return MathEvaluator::instance().resolveIndexed(MathEvaluator::resolveTemplate(binding, key));
    }

    // ------------------------------------------------------------------
    // Typed skin members (‑1 / "" = inherit the scheme), the real CanvasElement fields — not a string lookup
    // at every draw site. resolved*() below apply the fallback: a matching value range → this local override →
    // the global scheme.
    // ------------------------------------------------------------------
    int         borderWidth  = -1;
    int         borderRadius = -1;
    int         padding      = -1;
    std::string bgColor;
    std::string focusRing;   // run-mode focus ring: "" / "0" = follow the global, "1" = On, "2" = Off
    std::string fgColor;
    std::string accentColor;
    std::string borderColor;
    std::string fontName;
    bool        showBorder = true;

    // ------------------------------------------------------------------
    // Value RANGES (CanvasElement::Range): any number of [start,end] bands, each optionally overriding the skin
    // colours and/or flashing when m_value falls in the band. First match wins. Because every widget paints
    // through resolved*(), ranges give the whole set value-driven colour + alert for free.
    // ------------------------------------------------------------------
    struct Range {
        std::string bg, fg, accent, border;   // "" / invalid = don't override that channel
        bool        blink = false;            // flash (alternate override ↔ base) to alert
        // An EXPRESSION that decides the band instead of start/end. An interval can only ask one question
        // about one number, and the question worth asking is usually narrower: "hot, but only once it is
        // running", "lean under boost", "over target while closed loop is engaged". Resolved against this
        // `value` is this widget's own reading, so "value > 100 && value < 200" says what an interval
        // used to, and anything an interval could not: "&& engine.running", "&& map > 150". A rule with
        // no expression never applies -- there is no longer a second, weaker way to decide one.
        std::string when;
        // What a CAPTION says while this rule holds ("" = the widget keeps its own words). It rides on the
        // rule rather than on the widget because it answers the same question the colours do — "what is
        // true right now" — and a lamp is exactly that answer in words as well as in colour.
        std::string text;
    };
    std::vector<Range> m_ranges;
    // First band that matches, else nullptr. Out-of-line: a `when` band evaluates through MathEvaluator,
    // which this header deliberately does not pull in.
    const Range* activeRange() const;
    // Splice the `value` keyword (this widget's own reading) into a band expression as a literal.
    // Whole-word only, so a path like some.value.thing is left alone. Static + exposed for testing.
    static std::string substValue(const std::string& expr, double v);
    // Fold unit-suffixed literals ("5v", "200ms", ".2s") into the unit the reading is shown in, so a
    // threshold means the same quantity whatever unit the display is switched to. See the definition.
    static std::string substUnits(const std::string& expr, const std::string& displayUnit);
    // Shared ~1.25 Hz flash phase (true = show the override) so every alerting widget blinks in step.
    static bool blinkPhase() {
        using namespace std::chrono;
        return (duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 400) & 1;
    }

    // ------------------------------------------------------------------
    // Shared skin / value-format render helpers (the shared skin / value-format helpers). Every
    // placeable widget's render() and run-mode input lean on these; they live on the base so a subclass
    // inherits them and calls them UNQUALIFIED. The ones that touch the model layer
    // (Cache / MathEvaluator / UnitManager / RangeBands) are DEFINED out-of-line in CanvasWidget.cpp to keep
    // those heavy includes out of this widely-included header; the trivial, dependency-free ones stay inline.
    // Colours resolve through the skin cascade (skin::): value band → local element prop → scheme fallback.
    // ------------------------------------------------------------------
    // RAW. What a binding reads is storage units for a config path and already-engineering for
    // telemetry (whose scale is 1), which is exactly what dispV() cooks — so the one thing that
    // must never reach it is a number some other code has already cooked. The type refuses it.
    static Raw            evalSource(const std::string& expr);
    // A numeric property that a DEFINITION may state as an expression instead of a number — a gauge whose
    // top end is "{ useMetricOnInterface ? 140 : 284 }", an axis scaled to another setting. Parsed as a
    // number when it is one, evaluated when it is not, and the default when it is neither. (The curve
    // editor has needed this since its axes were imported; the gauges need it for their range.)
    static double numOrExpr(const std::string& raw, double def) {
        if (raw.empty()) return def;
        try {
            size_t used = 0;
            const double d = std::stod(raw, &used);
            while (used < raw.size() && std::isspace(static_cast<unsigned char>(raw[used]))) ++used;
            if (used == raw.size()) return d;
        } catch (...) {}
        const double v = evalSource(raw).v;
        return std::isnan(v) ? def : v;
    }
    // The writable config PATH behind a binding, or "" when the binding is read-only. A control pushes edits
    // only through a SINGLE #config reference that is the whole expression (the expression builder's writable
    // test); a $telemetry/@widget sigil, a compound expression, or a non-config field is display-only. Callers
    // read with evalSource() (sigil-aware) and write with setConfigValue(writableConfigPath()).
    static std::string    writableConfigPath(const std::string& binding);
    static const uint8_t* rangeColor(const PanelElement& el, const std::string& key, uint8_t out[4]);
    // The shared per-channel warning/alarm band as a colour, or nullptr when the reading is normal (or the
    // channel has no bands). Sits between the per-widget rules and the authored skin — see ChannelPrefs.
    static const uint8_t* channelBandColor(const PanelElement& el, const std::string& key, uint8_t out[4]);
    static const uint8_t* elColor(const PanelElement& el, const std::string& key, const uint8_t* fallback, uint8_t buf[4]);
    // A JStyle palette ROLE as an RGBA byte array — the themeable default for a widget colour, so a hard-coded
    // literal can be retired in favour of "global JStyle default, local prop override" (pass this as elColor's
    // fallback, or use it directly). Reads the current (global) palette; a JPaletteScope override wins if active.
    static const uint8_t* paletteRole(jf::JColorRole role, uint8_t out[4]) {
        const jf::JColor c = jf::JStyle::current().palette().color(role);
        out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a; return out;
    }
    // The unit of a binding, whether it is a path (Cache) or a sigil expression like "[$rpm]" (the
    // evaluator). Public because the inspector's Display Unit picker asks the same question.
    // Accessor pair for the display-unit prop. The member-pointer form of JPropertyModel::add binds the
    // pointer to the DECLARING class, so a subclass registering the inherited member cannot use it —
    // LabelWidget needs to offer this row without being a live-value widget.
    const std::string& displayUnit() const { return m_displayUnit; }
    void setDisplayUnit(const std::string& u) { if (m_displayUnit == u) return; m_displayUnit = u; emitModified(); }

    static std::string    bindingUnit(const std::string& bind);
    static std::string    displayUnitOf(const PanelElement& el);
    // What a widget PRINTS beside the number: the unit's pretty label ("°C", "°F", "V", "°"), not the id it
    // is converted by ("C", "F", "ADC_V", "deg"). UnitManager::unitLabel existed and nothing called it, so a
    // page that showed a unit at all would have shown the id — "ADC_V" beside a voltage, "C" beside a
    // temperature.
    static std::string    displayUnitLabelOf(const PanelElement& el);
    // RAW IN, DISPLAY OUT: apply the field scale, then convert to the display unit. Taking a Raw
    // is the whole guarantee — dispV(el, cache.solveTable(p)) does not compile, and that call is
    // precisely how every table readout in the studio came to show a tenth of its own cells.
    static double         dispV(const PanelElement& el, Raw v);
    static Raw            srcV(const PanelElement& el, double v);   // display -> raw (the write path)
    static double         scaleOf(const PanelElement& el);   // the bound field's raw<->engineering meta scale
    static const uint8_t* fgOf(const PanelElement& el, uint8_t buf[4]);
    // Precision implied by the unit actually being shown (one storage step, converted). -1 = the
    // channel states no precision, so the caller keeps its own default.
    // "[$hw_av1]" -> "hw_av1". A binding resolves to a SIGIL form, which is what the evaluator wants and
    // what nothing keyed by channel name recognises: Cache::channelDomain and ChannelPrefs both take a
    // plain name, so asking them with the brackets on silently answers "unknown". Anything that is not a
    // channel sigil comes back untouched.
    static std::string    bareChannelOf(const std::string& bind) {
        return (bind.size() > 3 && bind.front() == '[' && bind[1] == '$' && bind.back() == ']')
             ? bind.substr(2, bind.size() - 3) : bind;
    }
    static std::string    autoFormat(const PanelElement& el);
    static std::string    fmtVal(const PanelElement& el, const Cache&);
    // Does this element's reading come out as a WORD? True for a channel whose values the meta names
    // (idle_state, cruise_state, the engine state) — fmtVal prints the label for those. A readout uses
    // it to decide whether the text may be shrunk to fit: a number is sized by whoever authored the
    // cell and must not resize itself under a live value, a word arrives at whatever width the meta
    // says and has nowhere else to go.
    static bool           readingIsWord(const PanelElement& el);

    static jf::JColor jc(const uint8_t* c) { return jf::rgb(c[0], c[1], c[2]); }

    // ------------------------------------------------------------------
    // Studio-specific element flags (CanvasElement).
    // ------------------------------------------------------------------
    bool m_aspectRatioLocked = false;

    // ------------------------------------------------------------------
    // Resolved skin. Border width falls back to 0 (NO border unless one is configured) — a deliberate
    // studio-jf semantic ("if (border) draw, else don't"), NOT CanvasElement's 1px default. Colours resolve
    // range override → local prop → the passed scheme fallback (a jf::Colors:: pointer).
    // ------------------------------------------------------------------
    int resolvedBorderWidth()  const { return (showBorder && borderWidth > 0) ? borderWidth : 0; }
    int resolvedBorderRadius() const { return borderRadius >= 0 ? borderRadius : 6; }
    int resolvedPadding()      const { return padding      >= 0 ? padding      : 0; }

    // THE drawable area of a widget: its element rect inset by the authored padding. Every widget's content
    // — including its own outer box, if it draws one — is laid out in here, so a widget's visual size always
    // follows the size you set on the element. Widgets used to invent their own insets (the enum picker took
    // 2px/3px, so a 30px element drew a 24px box while a hosted control at 30px drew 30px), and `padding`
    // was an authored property nothing honoured. Both are now the same single rule.
    static constexpr float kMinContent = 8.f;   // a widget always keeps this much to draw in

    // THE inset rule, for every widget that takes a bite out of its own rect.
    //
    // An inset bigger than the box cannot be honoured, and honouring it as far as possible ERASES the
    // widget: inset both edges of a 40px cell by 27 and what is left is -14 tall. That is not a
    // hypothetical — a per-type Widget Default of padding=27, sane for the 160x110 cell it was saved
    // from, silently blanked every 40px Value cell that inherited it. The card still painted, so the
    // widget looked alive with nothing in it, which is the worst way for a layout to be wrong.
    //
    // Capped PER AXIS: a wide short cell keeps the horizontal inset it can afford and gives up only the
    // vertical it cannot. Shared rather than re-derived, because the bug repeats at every level that
    // insets — a widget that takes another 6px off an already-capped content rect puts it straight back
    // into the negative, which is exactly what happened to the Value cell's text.
    static jf::JRect insetRect(const jf::JRect& r, float p) {
        if (p <= 0.f) return r;
        const float px = std::min(p, std::max(0.f, (r.width  - kMinContent) * 0.5f));
        const float py = std::min(p, std::max(0.f, (r.height - kMinContent) * 0.5f));
        return jf::JRect{ r.x + px, r.y + py, std::max(0.f, r.width - 2.f * px), std::max(0.f, r.height - 2.f * py) };
    }

    jf::JRect contentRect(const jf::JRect& r) const { return insetRect(r, static_cast<float>(resolvedPadding())); }

    const uint8_t* resolvedBg(uint8_t out[4], const uint8_t* fallback) const {
        return _resolveColor(bgColor, [](const Range& r){ return &r.bg; }, out, fallback);
    }
    const uint8_t* resolvedFg(uint8_t out[4], const uint8_t* fallback) const {
        return _resolveColor(fgColor, [](const Range& r){ return &r.fg; }, out, fallback);
    }
    const uint8_t* resolvedAccent(uint8_t out[4], const uint8_t* fallback) const {
        return _resolveColor(accentColor, [](const Range& r){ return &r.accent; }, out, fallback);
    }
    const uint8_t* resolvedBorderColor(uint8_t out[4], const uint8_t* fallback) const {
        return _resolveColor(borderColor, [](const Range& r){ return &r.border; }, out, fallback);
    }

    // A widget whose colours are its OWN named props rather than the four skin channels -- the dial's
    // face / fill / bezel / glow -- still has to answer to the value bands. elColor() reads the element
    // prop DIRECTLY, so anything painting through it silently ignored every range that was set on it.
    // This is elColor() with the band cascade in front: band channel (blink-aware) -> element prop ->
    // fallback. Which of the four channels a given part listens to is the widget's own decision.
    enum class BandChannel { Bg, Fg, Accent, Border };
    const uint8_t* elColorBanded(const PanelElement& el, const std::string& key, BandChannel ch,
                                 const uint8_t* fallback, uint8_t out[4]) const {
        if (const Range* r = activeRange()) {
            const std::string* s = ch == BandChannel::Bg     ? &r->bg
                                 : ch == BandChannel::Fg     ? &r->fg
                                 : ch == BandChannel::Accent ? &r->accent
                                                             : &r->border;
            if (!(r->blink && !blinkPhase()) && skin::parseHex(*s, out)) return out;
        }
        return elColor(el, key, fallback, out);
    }

    // ------------------------------------------------------------------
    // Type / behaviour virtuals (CanvasElement).
    // ------------------------------------------------------------------
    // ---- ACCESSIBILITY / AI BUS ----------------------------------------------------------------
    // WHAT THIS ELEMENT IS, AS TEXT (defined in CanvasWidget.cpp — it uses fmtVal and the Cache).
    //
    // Every page element is a JWidget, so the framework already publishes one node per element to the
    // accessibility bridge and the AI bus — but with the default a11yNode() they all arrived as
    // [JWidget] "configedit" = "": the element TYPE, repeated a hundred times, with no way to tell one
    // field from another. A page was then legible only as pixels, which is why driving the studio meant
    // clicking at coordinates and reading screenshots back.
    jf::JA11yNode a11yNode() const override;

    virtual std::string          elementType() const        { return "widget"; }
    virtual bool                 isContainer() const         { return false; }

    // HOW WIDE DOES THIS NEED TO BE before it starts scrolling or clipping, given the width it was
    // authored at? Almost everything is exactly what it was drawn as — a caption, a field, a button are
    // their own size. A GRID is not: a table's width is the sum of the columns it has, at the size the
    // numbers in them need, and below that it scrolls however wide the page around it is. Asked of every
    // widget when a page is sized, so a window opens at the size its content is WHOLE (see
    // Surface::preferredSize) rather than at the width the page was drawn on.
    virtual float naturalWidth(float authoredW)  const { return authoredW; }
    virtual float naturalHeight(float authoredH) const { return authoredH; }
    virtual bool                 isLiveValueWidget() const   { return true; }   // polls a telemetry value

    // ------------------------------------------------------------------
    // Palette identity — each concrete widget declares its palette label and its drop size (virtual canvas
    // px). The catalog (widgetTypes/widgetTitle/widgetDefaultSize) reads these off a cached prototype instance;
    // the "one class owns its metadata" replacement for the old registry's title/defW/defH fields.
    // ------------------------------------------------------------------
    virtual std::string paletteTitle() const = 0;   // palette label
    virtual float       defaultW()     const = 0;   // drop width  (virtual canvas px)
    virtual float       defaultH()     const = 0;   // drop height

    // Does this widget paint SCALE ZONES (the "zones" prop — a tacho's red 6500-8000, painted into the
    // gauge face whatever the reading is)? A value RULE is universal, so its editor is offered on every
    // control; a zone only means something where there is a scale to lay it along, so the inspector asks
    // the widget rather than keeping a list of type names beside the ones that answer.
    virtual bool paintsZones() const { return false; }

    // @-sigil live state: the value of [@name.<prop>] for this widget, or NaN if this type doesn't provide
    // `prop` (the caller then falls back to geometry / the bound value). el = the live element, cache = the
    // telemetry snapshot. sigilNames() lists the EXTRA @-addressable names (excludes value/x/y/w/h).
    virtual double sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const {
        (void)prop; (void)el; (void)cache; return std::nan("");
    }
    virtual std::vector<std::string> sigilNames() const { return {}; }

    // ------------------------------------------------------------------
    // Frame — ONE background fill + ONE border, both virtual so a subclass can override its own frame.
    // Transparent background unless bgColor / a range sets one; border drawn only when configured.
    // ------------------------------------------------------------------
    virtual void drawBackground(jf::JPrimitiveBuffer& buf, const jf::JRect& r) const {
        // A matching rule's bg IS the background (self-drawn widgets paint their content ON it) — that is
        // just resolvedBg, which consults the rules first. There used to be a separate errorCondition
        // branch in front of this saying the same thing in its own words; it is a rule now.
        uint8_t out[4];
        const uint8_t* fill = resolvedBg(out, jf::Colors::Transparent);
        buf.pushRectangle(r.x, r.y, r.width, r.height, fill, static_cast<float>(resolvedBorderRadius()));
    }
    virtual void drawBorder(jf::JPrimitiveBuffer& buf, const jf::JRect& r) const {
        const int bw = resolvedBorderWidth();
        if (bw <= 0) return;                       // if (border) draw it — else DON'T
        uint8_t out[4];
        const uint8_t* bc = resolvedBorderColor(out, jf::Colors::Border);
        paintBorder(buf, r, bw, resolvedBorderRadius(), bc);
    }

    // ---- CARD CHROME: the title bar a container draws above its content ------------------------------
    // A panel draws a filled strip with the title in it and starts its content underneath. Anything that
    // presents itself as a card wants exactly that, drawn exactly that way — so it is here rather than
    // copied, and "the same as a panel" is true by construction instead of by eye.
    //
    // The FRAME is not part of this: drawBorder() already emits it through paintBorder(), which is the
    // same transparent-fill-plus-stroke a panel drew for itself.
    float titleBarH() const {
        return (!m_el || m_el->prop("labelText").empty() || !jf::JTextHelper::hasAtlas())
             ? 0.f : jf::JTextHelper::lineHeight() + 6.f;
    }
    // Draws it and answers where the CONTENT starts — r.y when there is no title.
    float drawTitleBar(jf::JPrimitiveBuffer& buf, const jf::JRect& r) const {
        const float h = titleBarH();
        if (h <= 0.f) return r.y;
        buf.pushRectangle(r.x, r.y, r.width, h, jf::Colors::Surface2,
                          static_cast<float>(resolvedBorderRadius()));
        jf::JTextHelper::pushText(buf, r.x + 8.f, r.y + 3.f, m_el->prop("labelText"),
                                  jf::Colors::TextPrimary, r.width - 16.f);
        return r.y + h;
    }

    // The ONE place border pixels are emitted. Both the instance drawBorder() above and the transitional free
    // widgetBorder() (which has no scene graph to construct a JWidget) route through here → one frame, drawn
    // identically whichever path renders the widget.
    static void paintBorder(jf::JPrimitiveBuffer& buf, const jf::JRect& r, int borderWidth,
                            int borderRadius, const uint8_t* color) {
        if (borderWidth <= 0) return;
        buf.pushRectangle(r.x, r.y, r.width, r.height, jf::Colors::Transparent,
                          static_cast<float>(borderRadius), static_cast<float>(borderWidth), color);
    }

    // ------------------------------------------------------------------
    // Content — subclasses paint their body here (the descriptor render lambda's replacement). `screen` is the
    // widget's window-absolute rect (its scene-graph bounds, which the surface sets to the camera-transformed
    // rect). Default draws nothing (a bare framed widget). Reads live values from the Cache.
    // ------------------------------------------------------------------
    // Every concrete widget overrides this with its own drawing. Default draws nothing (a bare framed widget).
    virtual void render(jf::JPrimitiveBuffer& buf, const jf::JRect& screen, const Cache& cache) {
        (void)buf; (void)screen; (void)cache;
    }

    // ------------------------------------------------------------------
    // Run-mode input (CanvasElement interactivity). A widget that responds to click/drag/scroll/key in Run
    // mode overrides interactive() → true and onControlInput() (returns true if it consumed the event). The
    // surface binds m_el (the raw element) before the call. Replaces the old descriptor onInput; display-only
    // widgets leave the defaults. `screen` is the element's window-absolute rect (same as render()).
    // ------------------------------------------------------------------
    virtual bool interactive() const { return false; }

    // Does this widget paint its own keyboard-focus indication? A widget hosting a framework control does (the
    // control draws its focus ring); a self-drawn one does not, so the Surface draws the ring for it. One rule,
    // so every interactive widget shows focus without each having to implement it.
    virtual bool drawsOwnFocus() const { return false; }

    // Whether the run-mode focus ring should draw for THIS element: its per-widget "focusRing" property
    // (On/Off) when set, else the Preferences▸Globals default (On). Surface honours it for widgets that
    // don't draw their own focus.
    static bool focusRingEnabled(const PanelElement& el);
    // THE INPUT DOOR. Every delivery site — the Surface, a panel, a viewport — comes through here, and it
    // puts input in the SAME element scope the paint runs in (see populateRenderPrimitives). Without it a
    // template page's control resolved "[*]" while being drawn and not while being clicked: a strip bound to
    // trigger.streams[*].cell[].v asked for [#trigger.streams[*].cell_len] rows, was told 0, and sat there
    // inert — drawn correctly and dead to the mouse. Widgets override handleControlInput; this is NOT
    // virtual, so no widget can be reached without its element in force.
    bool onControlInput(const jf::JRect& screen, const ControlInput& in) {
        MathEvaluator::ElementScope scope(m_elemContext);
        return handleControlInput(screen, in);
    }
    virtual bool handleControlInput(const jf::JRect& screen, const ControlInput& in) { (void)screen; (void)in; return false; }

    // JWidget paint entry — the framework render loop calls this. Frame → content → frame, in ONE place:
    // background, then the subclass body, then the border on top. Stage 2: the Surface drives persistent
    // instances through this path (setBounds to the screen rect + setCache), retiring the descriptor lambdas.
    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        // THIS widget's element is in scope for everything it does. A template page names its element as
        // "[*]", and a widget evaluates far more than its binding: a row count, a visibility test, a
        // control's Min/Max, a data source. Scoping only the gates left the rest resolving against
        // nothing — a strip bound correctly to trigger.streams[*].cell[].v then asked for
        // [#trigger.streams[*].cell_len] rows, got 0, and drew an empty card beside a page of fields
        // that had all resolved. Set once, here, for the whole paint.
        MathEvaluator::ElementScope scope(m_elemContext);
        const jf::JRect r = bounds();
        _syncTooltip();
        drawBackground(buf, r);
        // Content is CLIPPED to the widget's rect — a widget cannot paint outside its own region (Qt/GTK
        // clip a paintEvent to the widget; the studio does the same here). This contains overflowing content
        // (e.g. a fixed-size label longer than its box) and confines a container's children to the container.
        // The frame (background/border) is drawn unclipped so a border stroke on the edge stays crisp.
        // The subclass paints into the CONTENT rect (element minus padding) — applied here, once, so every
        // widget honours the padding property identically and none can invent its own inset.
        if (m_cache) { buf.pushClip(r.x, r.y, r.width, r.height); render(buf, contentRect(r), *m_cache); buf.popClip(); }
        drawBorder(buf, r);
    }
    void setCache(const Cache* c) { m_cache = c; }   // surface binds the live cache for content paint

    // The bound field's HELP as this widget's tooltip. The definition carries a description for most
    // settings and the import keeps all of it, but nothing ever displayed it — the data was in the model and unreachable from the page. The framework already
    // has the hover mechanism; it only needed telling what the text is. An element that states its own
    // "tooltip" keeps it: an author's words beat the definition's.
    void _syncTooltip();   // defined in CanvasWidget.cpp, where Cache is complete
    // The tooltip is derived during the paint, which a headless test has no reason to run. Exposed so the
    // derivation can be checked on its own — it is a lookup, and lookups are worth pinning.
    void syncTooltipForTest() { _syncTooltip(); }

    // The binding to take help from when THIS element has none of its own — set per frame by the Surface
    // from the group's bound control, and the only reason a caption label ever says anything.
    //
    // A label binds nothing, so a dropped caption was given the help text as a literal "tooltip" prop at
    // CREATION time. That makes it a SNAPSHOT: every caption dropped while its field had no help in the
    // definition is blank for good, and writing the help afterwards changes nothing on a page already
    // drawn — which is exactly what happened to the trigger-stream page (one caption of thirteen had a
    // tooltip, because one field of thirteen had help when the page was built). Resolving through the
    // group each frame makes the definition the live source for the caption as well as the control.

    void setHelpBind(std::string b) { m_helpBind = std::move(b); }


    // Disabled state for THIS frame — the surface sets it from the element's enableCondition (run mode only)
    // just before paint. Most controls get a dim wash drawn over them by the surface; a widget that wants to
    // render its own disabled look (e.g. a label fading its caption) reads this in render() instead.
    void setRenderDisabled(bool d) { m_renderDisabled = d; }
    bool renderDisabled() const { return m_renderDisabled; }

    // May this widget receive run-mode input THIS frame? The single rule behind `enableCondition`,
    // asked by every delivery site (the Surface, a viewport, a panel) so none of them can implement
    // it differently — or, as happened, not at all: the condition greyed a control and the control
    // still took the click, which is worse than not greying it, because the UI then lies about what
    // it will do.
    //
    // Blur is the exception and must always be delivered: it is how a control that was mid-edit
    // when the condition went false learns to commit or drop its caret. Refusing it would strand a
    // caret in a control the user can no longer see is active.
    bool acceptsInput(ControlInput::Kind k) const {
        return !m_renderDisabled || k == ControlInput::Kind::Blur;
    }

    // Is this widget's own TEXT its content, edited in place on the canvas? A Label is, and so is
    // anything built on one — a Hyperlink is a Label that navigates, and it wants the same double-click
    // caret for the same reason: the caption IS the widget. Asked instead of comparing type names, which
    // is how the Hyperlink came to be the one caption on a page you could not type into.
    virtual bool editsCaption() const { return false; }

    // Does the WHEEL belong to this widget, rather than to the page behind it?
    //
    // Hovering is not consent: a container hands the wheel only to the child it last CLICKED, because a
    // page of spin boxes would otherwise swallow every roll the pointer passed over — and worse, quietly
    // re-tune whichever value it rested on. That rule is right for a control that changes a VALUE and wrong
    // for one that scrolls a VIEW, which is the thing a wheel is for. So a view says so, and only a view:
    // the innermost widget under the cursor that answers yes takes the roll, and everything else falls
    // through to the page exactly as before. A container answers for the child under the cursor.
    virtual bool wantsWheel(const jf::JRect& /*r*/, float /*mx*/, float /*my*/) { return false; }

    // The VIEW scale this widget is painted at — the canvas zoom, or a container's content scale for a child.
    // Text sizes multiply by it so a caption grows with the view and nothing else.
    //
    // Widgets used to infer this as (screen width / authored width), which is only the zoom while a widget
    // keeps its authored proportions. Under a managed layout it does not: Y Axis stretches every child to the
    // container's full width, so a 500-wide caption in a 1232-wide panel "inferred" a 2.4x zoom and rendered
    // its text 2.4x too big. The scale is the container's to state, not the child's to guess.
    // The veil drawn over a DISABLED control, taken from the theme rather than hard-coded. It used to be a
    // near-black slab, which reads as "dimmed" on a dark scheme and as a grey blanket on a light one — on the
    // light theme a disabled row came out darker than an enabled one, and a disabled panel buried its own
    // contents. Surface0 is the scheme's own backdrop, so the veil always fades TOWARDS the background.
    static void disabledWash(jf::JPrimitiveBuffer& buf, const jf::JRect& r, float radius = 4.f) {
        const uint8_t* bg = jf::Colors::Surface0;
        const uint8_t veil[4] = { bg[0], bg[1], bg[2], 150 };
        buf.pushRectangle(r.x, r.y, r.width, r.height, veil, radius);
    }


    // One entry in a tab ring: the element's id and the geometry that decides where it comes in the order.
    struct TabSlot { int id; float x, y, h; };

    // Put a tab ring into READING order — top to bottom, left to right — and return the ids.
    //
    // Every ring used to be built in ELEMENT order, which is CREATION order. That is not the order anything
    // is on the screen: it is whatever sequence the page was authored in, it survives every later edit, and
    // Arrange > Front/Back rewrites it outright — so changing one control's z-order silently reshuffles the
    // keyboard. The trigger stream page tabbed Enabled -> Capture -> Edge -> back UP to Primitive, then
    // leapt from Gap ratio down to Cell length and back up to Window, and again from Repeats to Nominal
    // angle. Nothing about the page says why; the page reads straight down.
    //
    // Rows are BANDED rather than compared exactly. A caption sitting at y=282 beside a box at y=280 is one
    // row to the eye, and an exact sort would order the two controls either side of it by a two-pixel
    // accident. The band is half the height of the control that opened it, which is the row it occupies.
    static std::vector<int> readingOrder(std::vector<TabSlot> slots);

    void  setRenderZoom(float z) { m_renderZoom = z > 0.f ? z : 1.f; }
    float renderZoom() const { return m_renderZoom; }

    // Whether the surface is in EDIT mode this frame — set by Surface::populateRenderPrimitives before paint.
    // A container that mirrors a page (ViewportWidget) reads it to honour a child's run-mode-only visibility
    // condition, since it has no other view of the mode. Widgets rendered directly go through the surface's
    // own loop, which already knows the mode.
    inline static bool s_editMode = false;

    // Screen-space cursor position, set by the surface each mouse-move. Run mode has no hover delivery to
    // canvas widgets, so a widget that reacts to hover (a hyperlink underlining itself) hit-tests its own
    // screen rect against this in render(). Far-offscreen sentinel until the first move.
    inline static float s_hoverX = -1e9f, s_hoverY = -1e9f;

    // Bind this instance to its serialized element + the live cache for one frame's paint, refreshing the
    // typed skin/data members from the element. The surface calls this before setBounds()+paint. m_el is the
    // bridge while the model still stores PanelElements — a subclass's render() reads its own props through it;
    // Stage 3 folds those into typed members set in loadSkin so render() stops touching the prop bag.
    void bind(const PanelElement* el, const Cache* c) { m_el = el; m_cache = c; if (el) loadSkin(*el); }
    const PanelElement* element() const { return m_el; }

    // Persistent-tree ownership: the instance OWNS a copy of its element data, so the @-sigil can read its
    // geometry / uid / props between frames (the model's PanelElement vector reallocates; this copy is stable).
    // The Surface (and container widgets, for their children) call setData() each frame to refresh live
    // defaults into the owned copy; bind() then points m_el at m_data, so element() stays valid for the
    // instance's whole lifetime. The stable Widget ID lives in the element's uid MEMBER (not a prop), so lift
    // it into m_uid here for sigil addressing.
    void setData(const PanelElement& e) {
        m_data = e;
        bind(&m_data, m_cache);
        if (!m_data.uid.empty()) m_uid = m_data.uid;
    }

    // ------------------------------------------------------------------
    // Owned-state interface — the widget IS the running state (no per-frame model feed).
    // setSource() hydrates the instance from the model ONCE (creation / undo / load / tab-switch): m_edit takes
    // the model's OVERRIDE set, m_data the resolved render copy. Thereafter the widget owns both; edits mutate
    // m_edit (the authoritative override store) and re-resolve, and commit() flushes m_edit back to the model
    // ONLY at a serialization boundary (save / undo-snapshot). The model is never read back into the widget
    // except through setSource — that is what "widgets are the running state" means here.
    // ------------------------------------------------------------------
    void setSource(const PanelElement& modelEl) {   // hydrate from the model (overrides + resolved render copy)
        m_edit = modelEl;
        setData(resolveElement(m_edit));
    }
    void setOwnProp(const std::string& key, const std::string& val) {   // an edit: set an override, re-resolve
        m_edit.props[key] = val;
        setData(resolveElement(m_edit));
    }
    void clearOwnProp(const std::string& key) {     // ✕ on an inheritable row: drop the override, re-inherit
        m_edit.props.erase(key);
        setData(resolveElement(m_edit));
    }
    const PanelElement& editElement() const { return m_edit; }   // the OVERRIDE set (for override-vs-inherited UI)
    // Flush this instance's owned overrides into `el` (its model element) — props ONLY (the pure override set),
    // so topology/geometry the canvas owns (id/type/rect/uid) are untouched AND inheritance is preserved: an
    // unoverridden default is never materialized. Every authored edit lands in m_edit.props (dock setOwnProp, or
    // a menu setProp that rehydrates into m_edit), so there is nothing else to write — deliberately NOT calling
    // saveContent(), which writes typed members unconditionally and would freeze defaults as explicit overrides.
    void commit(PanelElement& el) const { el.props = m_edit.props; }

    // Live widget tree — a container (panel/viewport) overrides childWidgets() to expose its OWNED child
    // instances; the base is a leaf. findWidget() resolves an @-sigil address (the stable uid, or the legacy
    // "<type>_<id>") against this instance and its whole subtree; collectSigilTokens() enumerates every
    // addressable "<uid>.<prop>" in the subtree for the sigil picker. One recursion, defined once.
    virtual std::vector<CanvasWidget*> childWidgets() { return {}; }

    // The child ids this container's keyboard ring enumerates, in reading order — {} for a leaf. A container
    // is a NESTED focus domain (the Surface sees it as one control), so this is the only way to ask from
    // outside whether a control inside it can be tabbed to. assumeEnabled asks the structural question
    // instead of the live one: would it be in the ring if its condition were true?
    virtual std::vector<int> keyboardRing(bool assumeEnabled = false) { (void)assumeEnabled; return {}; }
    CanvasWidget* findWidget(const std::string& addr) {
        if (const PanelElement* e = element()) {
            if ((!m_uid.empty() && m_uid == addr) || addr == (e->type + "_" + std::to_string(e->id)))
                return this;
        }
        for (CanvasWidget* c : childWidgets())
            if (c) if (CanvasWidget* r = c->findWidget(addr)) return r;
        return nullptr;
    }
    void collectSigilTokens(std::vector<std::string>& out) {
        if (!m_uid.empty()) {
            // "units" is a STRING prop (see widgetsigil::unitOf) -- listed so it is discoverable
            // in the picker even though the numeric sigil path cannot return it.
            for (const char* p : { "value", "x", "y", "w", "h", "units" }) out.push_back(m_uid + "." + p);
            for (const std::string& nm : sigilNames()) out.push_back(m_uid + "." + nm);
        }
        for (CanvasWidget* c : childWidgets()) if (c) c->collectSigilTokens(out);
    }

    // ------------------------------------------------------------------
    // Load the typed members from a PanelElement (the serialized/edited form). Mirrors CanvasElement::load
    // for the skin + data + ranges; the rest of load() (geometry, conditions, overrides) is Stage 2.
    // ------------------------------------------------------------------
    // Visible / Enabled — sigil-aware expressions, blank = always. These live on EVERY widget because
    // the Surface already honours them for every element (it reads "condition" / "enableCondition" off
    // the element when it decides what to show and what to grey). Only three widgets ever OFFERED the
    // rows, so on a button — the one control where "grey it out unless the module is on" is the obvious
    // thing to want — there was simply no way to type the expression the engine was already looking for.
    const std::string& visibleExpr() const { return m_visibleExpr; }
    void setVisibleExpr(const std::string& e) { if (m_visibleExpr == e) return; m_visibleExpr = e; emitModified(); }
    // ---- CONTROL BOUNDS ------------------------------------------------------------------------
    // What THIS control will let you enter, as expressions in the unit the control displays. They may
    // only NARROW what the field allows — the schema's declared range, or its datatype's if it declares
    // none. A page cannot widen a bound: the storage decides what fits and the schema decides what is
    // meaningful, and neither is a layout's business.
    //
    // For the cases a fixed number cannot state, because the answer depends on other settings:
    //   WIDTH max   Min = trigger.streams[*].width_min      the band cannot invert
    //   cell value  Max = select([#...primitive] == 0, 255, 7200)   a GAP index is a byte
    // Blank = whatever the field allows. Evaluated in this widget's element scope like every other
    // expression, so "[*]" works on a template page.
    const std::string& minExpr() const { return m_minExpr; }
    void setMinExpr(const std::string& e) { if (m_minExpr == e) return; m_minExpr = e; emitModified(); }
    const std::string& maxExpr() const { return m_maxExpr; }
    void setMaxExpr(const std::string& e) { if (m_maxExpr == e) return; m_maxExpr = e; emitModified(); }

    // The bound field's Location, NARROWED by this control's Min/Max. Every read and write of a bound
    // value goes through it, so the narrowing reaches a drag, a paste and a nudge — not just typing.
    MetaModel::Location boundLoc() const;

    // Apply this control's Min/Max to a location the caller already built. boundLoc() resolves the bound
    // field and then calls this; a widget that computes its own address — a run's Nth element, a table's
    // cell — calls it directly, so the narrowing is one intersection either way and not a second copy
    // that drifts.
    void narrowToControl(MetaModel::Location &L) const;

    const std::string& enabledExpr() const { return m_enabledExpr; }
    void setEnabledExpr(const std::string& e) { if (m_enabledExpr == e) return; m_enabledExpr = e; emitModified(); }


    void loadSkin(const PanelElement& el) {
        borderWidth  = skin::num(el, "borderWidth",  -1);
        borderRadius = skin::num(el, "borderRadius", -1);
        padding      = skin::num(el, "padding",      -1);
        bgColor      = el.prop("bgColor");
        focusRing    = el.prop("focusRing");
        fgColor      = el.prop("fgColor");
        accentColor  = el.prop("accentColor");
        borderColor  = el.prop("borderColor");
        fontName     = el.prop("fontName");
        showBorder   = el.prop("showBorder") != "No";

        m_signalName     = el.prop("signalName");
        const std::string du = el.prop("displayUnit");
        m_displayUnit    = du.empty() ? "Auto" : du;
        m_scale          = scaleOf(el);   // the field's own meta scale
        m_uid            = el.prop("uid");
        m_visibleExpr    = el.prop("condition");
        m_enabledExpr    = el.prop("enableCondition");
        m_minExpr        = el.prop("minExpr");
        m_maxExpr        = el.prop("maxExpr");

        m_ranges.clear();
        for (const ColorRule& r : ColorRules::fromCompact(el.prop("ranges")).rules)
            m_ranges.push_back(Range{ r.bg, r.fg, r.accent, r.border, r.blink, r.when, r.text });

        loadContent(el);   // subclass loads its OWN typed members (CanvasElement::load parity)
    }

    // ------------------------------------------------------------------
    // Content state hooks (CanvasElement::load / save parity). A subclass that owns typed members
    // overrides loadContent() to pull them from the serialized element, and saveContent() to write
    // them back — so render() reads members, never the prop bag. Default: skin-only widget, no state.
    // ------------------------------------------------------------------
    virtual void loadContent(const PanelElement& /*el*/) {}
    virtual void saveContent(PanelElement& /*el*/) const {}
    // Save-time: a CONTAINER writes its owned children's state back to THEIR source model (a viewport commits
    // to the node's page model, a panel to its children). Called after saveContent so the whole tree is
    // flushed to the models just before serialization. A leaf owns no children → no-op.
    virtual void commitOwnedChildren() {}

    // ------------------------------------------------------------------
    // Editable property surface — the JFramework-native equivalent of CanvasElement's Q_PROPERTY set +
    // propertyMetaData(). The shared rows (identity, geometry, binding, skin) are declared ONCE here; a
    // subclass overrides collectProperties(), chains to this, and adds its own. Binding/value rows appear
    // only on live-value widgets (a static label hides them, exactly as CanvasElement did). Property NAMES
    // match the serialized PanelElement prop keys, so the inspector and save/load share one vocabulary.
    // ------------------------------------------------------------------
    void collectProperties(jf::JPropertyModel& m) override {
        using jf::JPropertyMeta;
        using jf::JProperty;
        using jf::JVariant;

        m.add("uid", this, &CanvasWidget::m_uid, JPropertyMeta{ .label = "Widget ID", .writable = false, .order = -1 });

        // Geometry (virtual canvas coords). get/set bridge the JWidget layout box.
        m.add(JProperty{ .name = "x", .get = [this] { return JVariant(bounds().x); },
            .set = [this](const JVariant& v) { auto b = bounds(); b.x = static_cast<float>(v.toDouble(b.x)); setBounds(b); return true; },
            .meta = JPropertyMeta{ .label = "X", .min = -10000, .max = 10000, .order = 0 } });
        m.add(JProperty{ .name = "y", .get = [this] { return JVariant(bounds().y); },
            .set = [this](const JVariant& v) { auto b = bounds(); b.y = static_cast<float>(v.toDouble(b.y)); setBounds(b); return true; },
            .meta = JPropertyMeta{ .label = "Y", .min = -10000, .max = 10000, .order = 1 } });
        m.add(JProperty{ .name = "w", .get = [this] { return JVariant(bounds().width); },
            .set = [this](const JVariant& v) { auto b = bounds(); b.width = static_cast<float>(v.toDouble(b.width)); setBounds(b); return true; },
            .meta = JPropertyMeta{ .label = "Width", .min = 1, .max = 10000, .order = 2 } });
        m.add(JProperty{ .name = "h", .get = [this] { return JVariant(bounds().height); },
            .set = [this](const JVariant& v) { auto b = bounds(); b.height = static_cast<float>(v.toDouble(b.height)); setBounds(b); return true; },
            .meta = JPropertyMeta{ .label = "Height", .min = 1, .max = 10000, .order = 3 } });
        m.add("aspectRatioLocked", this, &CanvasWidget::m_aspectRatioLocked, JPropertyMeta{ .label = "Lock Aspect Ratio", .order = 4 });

        // Data binding — live-value widgets only (a static caption has no data source).
        if (isLiveValueWidget()) {
            m.add("signalName",  this, &CanvasWidget::signalName, &CanvasWidget::setSignalName, JPropertyMeta{ .label = "Data Source", .editor = "signal", .order = 20 });
            m.add("displayUnit", this, &CanvasWidget::m_displayUnit, JPropertyMeta{ .label = "Display Unit", .editor = "unit", .order = 21 });
            m.addReadOnly("value", this, &CanvasWidget::value, JPropertyMeta{ .label = "Current Value", .order = 23 });
        }

        // Skin cascade — "" / -1 = inherit the scheme.
        m.add("condition",       this, &CanvasWidget::visibleExpr, &CanvasWidget::setVisibleExpr,
              JPropertyMeta{ .label = "Visible", .editor = "expr", .order = 30 });
        m.add("enableCondition", this, &CanvasWidget::enabledExpr, &CanvasWidget::setEnabledExpr,
              JPropertyMeta{ .label = "Enabled", .editor = "expr", .order = 31 });
        m.add("minExpr", this, &CanvasWidget::minExpr, &CanvasWidget::setMinExpr,
              JPropertyMeta{ .label = "Min", .editor = "expr", .order = 32 });
        m.add("maxExpr", this, &CanvasWidget::maxExpr, &CanvasWidget::setMaxExpr,
              JPropertyMeta{ .label = "Max", .editor = "expr", .order = 33 });
        m.add("focusRing",   this, &CanvasWidget::focusRing,   JPropertyMeta{ .label = "Focus Ring", .editor = "enum", .order = 89,
              .choices = { std::string("global"), std::string("On"), std::string("Off") } });
        m.add("bgColor",     this, &CanvasWidget::bgColor,     JPropertyMeta{ .label = "Background", .editor = "color", .inheritable = true, .order = 90 });
        m.add("fgColor",     this, &CanvasWidget::fgColor,     JPropertyMeta{ .label = "Text",       .editor = "color", .inheritable = true, .order = 91 });
        m.add("accentColor", this, &CanvasWidget::accentColor, JPropertyMeta{ .label = "Accent",     .editor = "color", .inheritable = true, .order = 92 });
        m.add("borderColor", this, &CanvasWidget::borderColor, JPropertyMeta{ .label = "Border",     .editor = "color", .inheritable = true, .order = 93 });
        m.add("fontName",    this, &CanvasWidget::fontName,    JPropertyMeta{ .label = "Font",        .editor = "font",  .inheritable = true, .order = 94 });
        m.add("borderWidth",  this, &CanvasWidget::borderWidth,  JPropertyMeta{ .label = "Border Width",  .min = -1, .max = 20, .suffix = " px", .order = 95 });
        m.add("borderRadius", this, &CanvasWidget::borderRadius, JPropertyMeta{ .label = "Border Radius", .min = -1, .max = 80, .suffix = " px", .order = 96 });
        m.add("padding",      this, &CanvasWidget::padding,      JPropertyMeta{ .label = "Padding",       .min = -1, .max = 80, .suffix = " px", .order = 97 });
    }

protected:
    const Cache*        m_cache = nullptr;   // bound by the surface (content paint); null in the lambda path
    bool                m_renderDisabled = false;   // set per-frame by the surface (enableCondition false, run mode)
    float               m_renderZoom = 1.f;         // view scale for this frame's paint (canvas zoom / container scale)
    std::string         m_helpBind;          // group-mate's binding, for a caption's hover help (see setHelpBind)
    const PanelElement* m_el    = nullptr;   // this frame's serialized element (bridge — see bind())
    PanelElement        m_data;              // the instance's OWNED, RESOLVED element copy (render source); m_el points here
    PanelElement        m_edit;              // the instance's OWNED OVERRIDE set (unresolved) — the authoritative edit store

private:
    // Shared colour resolve: a matching range's channel override (honouring the blink phase) → the local
    // prop → the scheme fallback. `pick` selects which Range channel string to read.
    template <class Pick>
    const uint8_t* _resolveColor(const std::string& local, Pick pick, uint8_t out[4],
                                 const uint8_t* fallback) const {
        if (const Range* r = activeRange()) {
            if (!(r->blink && !blinkPhase()) && skin::parseHex(*pick(*r), out)) return out;
        }
        return skin::parseHex(local, out) ? out : fallback;
    }
};

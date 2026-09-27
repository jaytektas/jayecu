// WidgetRegistry — the catalog implementation over the self-registered widget set. makeWidgetInstance is
// a registry lookup (no switch); widgetTypes is the registered types sorted by each widget's declared
// order (no hand-kept list); widgetTitle/widgetDefaultSize + resolveElement read a cached prototype.

#include "WidgetRegistry.h"
#include "CanvasWidget.h"            // prototype instances — paletteTitle / defaultW/H / properties
#include "../model/EditorSettings.h"

#include <j/core/Log.h>             // JLOGC — factory-level widget instance creation (surface.widget)

#include <algorithm>
#include <unordered_map>

std::unique_ptr<CanvasWidget> makeWidgetInstance(const std::string& type, jf::JSceneGraph& graph) {
    const WidgetRegistry::Entry* e = WidgetRegistry::instance().find(type);
    JLOGC("surface.widget", jf::JLogLevel::Debug)
        << "factory make type=" << type << (e ? "" : " (UNKNOWN — no registered entry)");
    return e ? e->make(graph) : nullptr;
}

const std::vector<std::string>& widgetTypes() {
    // Built once, lazily: the registered types sorted by each widget's declared order. Every widget has
    // self-registered at static-init by the time this is first called (palette build, at runtime).
    static const std::vector<std::string> kTypes = [] {
        std::vector<const WidgetRegistry::Entry*> es;
        for (const auto& e : WidgetRegistry::instance().entries()) es.push_back(&e);
        std::stable_sort(es.begin(), es.end(),
                         [](const WidgetRegistry::Entry* a, const WidgetRegistry::Entry* b) { return a->order < b->order; });
        std::vector<std::string> t;
        t.reserve(es.size());
        for (const WidgetRegistry::Entry* e : es) t.push_back(e->type);
        return t;
    }();
    return kTypes;
}

namespace {
// A cached prototype instance per type — built once via the factory, it supplies the type's class-level
// metadata (title / drop size / authored property defaults) without a live element.
CanvasWidget* widgetPrototype(const std::string& type) {
    static jf::JSceneGraph s_protoGraph;
    static std::unordered_map<std::string, std::unique_ptr<CanvasWidget>> s_protos;
    auto it = s_protos.find(type);
    if (it == s_protos.end()) {
        it = s_protos.emplace(type, makeWidgetInstance(type, s_protoGraph)).first;
        // Metadata-only instance: never laid out or painted, but it still enrols in the global widget set
        // (every JWidget does, at construction). Keep the whole cache out of focus/hit scans.
        if (it->second) it->second->setScanExcluded(true);
    }
    return it->second.get();
}
}  // namespace

std::string widgetTitle(const std::string& type) {
    CanvasWidget* p = widgetPrototype(type);
    return p ? p->paletteTitle() : std::string();
}

bool widgetEditsCaption(const std::string& type) {
    CanvasWidget* p = widgetPrototype(type);
    return p && p->editsCaption();
}

bool widgetDefaultSize(const std::string& type, float& w, float& h) {
    CanvasWidget* p = widgetPrototype(type);
    if (!p) return false;
    w = p->defaultW(); h = p->defaultH();
    return true;
}

// Cascade (low→high): widget-class authored defaults < per-type Widget Defaults (EditorSettings) < the
// element's own props. An unset key reads its default LIVE. The class defaults come from a prototype's
// property model (each JProperty carries its JPropertyMeta.def) and are cached per type; the per-type
// defaults are queried live because Preferences edits them at runtime.
PanelElement resolveElement(const PanelElement& el) {
    PanelElement r = el;

    static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_builtin;
    auto dit = s_builtin.find(el.type);
    if (dit == s_builtin.end()) {
        std::unordered_map<std::string, std::string> m;
        if (CanvasWidget* proto = widgetPrototype(el.type))
            for (const jf::JProperty& p : proto->properties().all())
                if (!p.meta.def.empty()) m[p.name] = p.meta.def;
        dit = s_builtin.emplace(el.type, std::move(m)).first;
    }

    // PRECISION IS NOT A TYPE DEFAULT. A `format` on ONE widget is legitimate — a page author saying
    // this readout is a count, not a measurement. The same string saved as the default for every widget
    // of a type is not: it applies to every binding whatever unit that binding is in, and fmtVal prefers
    // an element's own format over the unit's, so it silently overrides the lot.
    //
    // That is what happened. "editor.wdefProps.value" held format=%.1f — captured once from some widget
    // via Preferences > Widget Defaults — and from then on EVERY value readout in the app that did not
    // state its own format read to one decimal: a 0-5 V analog pin as "4.2 V" where its ADC_V unit says
    // %.3f, and lambda, and everything else. The unit owns precision (see UnitManager); a type-wide
    // default cannot know enough to override it, so it is not offered the chance.
    static const char* kNotTypeWide[] = {"format"};
    const std::unordered_map<std::string, std::string> edefs =
        EditorSettings::instance().widgetDefaultProps(el.type);
    for (const auto& [k, v] : edefs) {
        bool banned = false;
        for (const char* b : kNotTypeWide) banned |= (k == b);
        if (!banned && !el.props.count(k)) r.props[k] = v;
    }
    for (const auto& [k, v] : dit->second) if (!r.props.count(k))  r.props[k] = v;
    return r;
}

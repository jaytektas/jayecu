#pragma once

// WidgetRegistry — the ONE string→class map the serialized model requires, plus the small catalog that
// reads a type's metadata off a cached prototype. A saved panel references widgets by TYPE STRING
// ("table", "checkbox"); C++ has no reflection to instantiate a type from a string, so a runtime map is
// unavoidable. What IS avoidable is a central switch + a hand-kept type list: each widget registers
// ITSELF from its own translation unit via REGISTER_WIDGET, so the type key, the factory, and the palette
// order all live in the widget and nowhere else. Adding a widget touches only that widget's files.

#include "PanelModel.h"          // PanelElement (resolveElement return)

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jf { class JSceneGraph; }
class CanvasWidget;

class WidgetRegistry {
public:
    using Factory = std::function<std::unique_ptr<CanvasWidget>(jf::JSceneGraph&)>;
    struct Entry { std::string type; int order; Factory make; };

    static WidgetRegistry& instance() { static WidgetRegistry r; return r; }

    // Called by REGISTER_WIDGET at static-init (before main). Re-registering a type overwrites it.
    static bool add(std::string type, int order, Factory make) {
        auto& es = instance().entries_;
        for (auto& e : es)
            if (e.type == type) { e.order = order; e.make = std::move(make); return true; }
        es.push_back({ std::move(type), order, std::move(make) });
        return true;
    }

    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& type) const {
        for (const auto& e : entries_) if (e.type == type) return &e;
        return nullptr;
    }

private:
    std::vector<Entry> entries_;
};

// One line in a widget's .cpp declares the whole widget to the app — its type key, palette order, and
// factory — with no central list to keep in sync. CLASS must be a complete CanvasWidget subclass here
// (the widget's own header is included above it in the same TU).
#define REGISTER_WIDGET(TYPE, CLASS, ORDER)                                                       \
    namespace {                                                                                    \
        const bool _wreg_##CLASS = WidgetRegistry::add((TYPE), (ORDER),                            \
            [](jf::JSceneGraph& g) -> std::unique_ptr<CanvasWidget> {                              \
                return std::make_unique<CLASS>(g);                                                 \
            });                                                                                     \
    }

// ---- Catalog: prototype-backed answers over the registry -------------------------------------------
// Each answer comes from a cached prototype instance of the type, so a type's palette title, drop size
// and @-sigil live state all live on its class (the "one class owns its metadata" rule). Formerly in
// WidgetSupport; the declarations moved here alongside the registry they read.
std::unique_ptr<CanvasWidget> makeWidgetInstance(const std::string& type, jf::JSceneGraph& graph);  // instance, or null
const std::vector<std::string>& widgetTypes();                    // registered types in palette order
std::string  widgetTitle(const std::string& type);               // palette label ("" if unknown)
bool         widgetDefaultSize(const std::string& type, float& w, float& h);   // drop size (false if unknown)
bool         widgetEditsCaption(const std::string& type);        // its own text IS its content (Label & co)

// Resolve an element's EFFECTIVE props for READING (render + inspector display): under the element's own
// props (its overrides) layer the per-type Widget Defaults (EditorSettings) then the widget CLASS's authored
// defaults (each property's JPropertyMeta.def, read from a prototype instance). An unset property therefore
// reads its default LIVE — edit a default and every non-overriding widget updates. Edits + override state
// still use the RAW element props (a key's PRESENCE = an override).
PanelElement resolveElement(const PanelElement& el);

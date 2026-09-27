#pragma once

// PanelModel — a surface's source of truth. A canvas (virtual size + scaling mode + anchor) and a flat
// list of elements, each a widget-type key + a rect in VIRTUAL coordinates + a prop bag. The view is a
// pure projection: it derives its screen transform from the canvas (never a user pan/zoom) and mutates
// the model through these methods, which emit fine-grained change signals so every observer updates in
// place. No history/back-compat — the model is whatever the current design needs.

#include <j/core/Signal.h>
#include <j/core/SceneGraph.h>   // jf::JRect — pageArea_
#include <j/core/Uuid.h>
#include <j/core/Log.h>      // JLOGC — panel load / instantiation (surface.panel)
#include <j/config/Json.h>
#include "../model/EditorSettings.h"   // the Preferences default a page falls back to

#include <algorithm>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

// Payload for a drag out of the Dictionary dock (one or more binding paths — the dictionary supports
// multi-select). A distinct type so JDragDrop cleanly separates it from the control-palette drag (which
// carries a control-type string): dropped on a control the FIRST path sets that control's source; dropped
// on empty surface it creates one default control per path (bound), tiled from the drop point.
struct DictBinding { std::vector<std::string> paths; };

// Payload for a drag of a NAVIGATION-TREE node out onto a surface. A distinct type so JDragDrop separates
// it from a Dictionary binding (channel path) or a palette control-type. Dropped on a surface it creates a
// "viewport" element referencing the node's page (one node → many viewports).
struct NodePlacement { std::string node; };

struct PanelElement {
    int         id = 0;
    std::string type;                       // widget type key (CanvasWidget subclass)
    float       x = 0, y = 0, w = 160, h = 90;   // VIRTUAL coordinates
    int         groupId = 0;                // 0 = ungrouped; same id => select/move/delete as one unit
    std::unordered_map<std::string, std::string> props;
    std::string uid;                        // stable per-widget UUID (RFC-4122 v4); shown as "Widget ID", persisted

    std::string prop(const std::string& k) const {
        auto it = props.find(k);
        return it == props.end() ? std::string() : it->second;
    }
};

// WHICH PAGE a viewport shows. "page" when set — a pasted copy owns a private canvas keyed by its own uid
// — otherwise "node", the tree node it mirrors. These are separate because "node" ALSO decides WHEN a
// viewport is visible (node == the tree selection). Storing a private canvas key in "node" therefore made
// a pasted viewport invisible forever: a uid can never be the selected tree node, so the gate never opened.
// EVERY BINDING AN ELEMENT DRAWS — itself, and everything nested inside it.
//
// A panel keeps its children as JSON text in its own "children" prop rather than as elements of the
// page, so a walk over elements() sees the panel and none of the controls in it. That is not a corner
// case: on this document the O2 Control page is one checkbox and seven panels, and every other control
// on it is a panel's child. Anything asking "which page shows this setting" and answering from
// elements() alone answers "no page" for most of the document — which is exactly what the tune
// difference report did, listing settings as unpaged while they sat in plain sight on a page.
//
// Depth is not bounded here because it is not bounded in the document: a panel can hold a panel.
inline void collectBindings(const PanelElement& el, const std::function<void(const std::string&)>& fn) {
    const std::string sig = el.prop("signalName");
    if (!sig.empty()) fn(sig);
    const std::string kids = el.prop("children");
    if (kids.empty()) return;
    auto j = jf::JJson::tryParse(kids);
    if (!j || !j->isArray()) return;   // a panel with no parsable children contributes only itself
    for (const jf::JJson& c : j->arr()) {
        PanelElement ce;
        ce.type = c["type"].str();
        const jf::JJson& pr = c["props"];
        if (pr.isObject())
            for (const auto& [k, v] : pr.obj()) ce.props[k] = v.isString() ? v.str() : v.dump();
        collectBindings(ce, fn);
    }
}

inline std::string viewportPage(const PanelElement& el) {
    const std::string p = el.prop("page");
    if (!p.empty()) return p;                     // a pasted copy owns a private canvas
    // "" when the viewport is UNBOUND, which is the ordinary case: it shows whichever page the tree has
    // selected, and WHICH page that is belongs to the surface it is painted on rather than to the
    // element. ViewportWidget::pageKey() answers it from what its Surface pushed down.
    //
    // This briefly answered from a GLOBAL "current node". It works right up until two surfaces are on
    // different pages, and worse, it makes a test pass that is testing nothing: a single-surface test
    // cannot tell the global apart from the per-surface push, so a gate that rejects unbound viewports
    // still looks fine.
    return el.prop("node");                       // non-empty = PINNED to that one page
}

class PanelModel {
public:
    // Change events (carry the element id where relevant). The view + any inspector observe these.
    jf::JSignal<int> elementAdded;
    jf::JSignal<int> elementRemoved;
    jf::JSignal<int> elementChanged;
    jf::JSignal<>    canvasChanged;

    // --- Canvas -------------------------------------------------------------------------------
    // Mirrors the studio's PanelDefinition canvas fields. Colours/fonts are strings ("" = inherit the
    // scheme). canvasStatic: 0 inherit(global default) / 1 fixed / 2 scale-to-fit / 3 REFLOW.
    //
    // REFLOW (3) is the one that is not a canvas at all: the page IS the view. There is no authored width
    // to scale or squeeze — the page takes the space it is given, at the interface scale, and its own
    // layout arranges the content into it. A converted dialog is exactly this: a column of label/value
    // rows that should use the window's width and scroll when there are more rows than fit, rather than
    // being an 838-wide island stranded in the middle of a 1458-wide viewport with the labels and their
    // values a third of a screen apart.
    // Editing guide (guideW/H, 0×0 = none) is a reference frame drawn in the editor. layout 0..6 =
    // Free/YAxis/XAxis/Border/Card/IndexCard/Grid. Title styling drives PanelView's frame + title bar.
    float canvasW() const { return canvasW_; }
    // THE page size: its own override when it has one, else the Preferences default. A page stores a size
    // only when it needs a different one, so changing the preference moves every page that never asked to
    // be special — which is what a default is for.
    float effCanvasW() const { return canvasW_ > 0.f ? canvasW_ : float(EditorSettings::instance().canvasWidth()); }
    // THE WIDTH BELOW WHICH THIS PAGE STOPS REFLOWING AND STARTS SCROLLING. 0 = no floor, reflow all the
    // way down, which is what a lamp grid or a viewport host wants: they have no rows to clip, so more or
    // less room simply means more or fewer columns. A converted dialog is the other case — its rows are a
    // caption beside a control, and squeezing those past the point where the caption fits does not make a
    // narrower page, it makes an unreadable one. The generator that knows the content states the number;
    // it is NOT the canvas, which for a reflow page is only ever the size somebody guessed first.
    float minW() const { return minW_; }
    void  setMinW(float w) { minW_ = w; }

    // THE ELEMENT THIS PAGE IS ABOUT — "sensors.sensor[clt]", "outputs.output[4]" — which is what every
    // "[*]" written on it resolves to. One page BODY serves a whole array (128 sensors, 42 outputs); the
    // element used to be carried by the VIEWPORT that showed the page, and a page opens in a window of
    // its own now with no viewport anywhere in the document. Carried by the page, it survives however the
    // page is reached: a window, a tab, or a viewport that supplies its own (which still wins - see
    // Surface::enterViewport).
    // WHERE A PAGE WINDOW LIVES ON THIS SURFACE, in this surface's own page units: the top-left it opens
    // at and the most room it may take. A tab knows what it leaves beside its own instruments — the idle
    // tab's readouts start at x=920 and its lower graph at y=582, so a page gets (10,92,900,480) — and
    // that is a fact about the tab, not something to be re-derived from widget positions on every frame.
    // 0 width/height mean "to the edge of the area". The window's SIZE is still its own (the page's
    // declared size); this says where it starts and where it must stop and scroll.
    jf::JRect pageArea() const { return pageArea_; }
    void setPageArea(const jf::JRect& r) { pageArea_ = r; pageAreaSet_ = true; }

    // A TAB THAT IS ALL INSTRUMENT TAKES NO PAGE WINDOWS. Diagnostics is a channel table filling its own
    // surface — there is no corner of it a page could sit in without covering the thing you opened the
    // tab to read. Declaring a page area of ZERO says so, and is different from declaring none at all
    // (which means "no reservation, a window may use the whole area").
    bool allowsPageWindows() const { return !(pageAreaSet_ && pageArea_.width <= 0.f && pageArea_.height <= 0.f); }

    const std::string& elementScope() const { return elemScope_; }
    void  setElementScope(std::string s) { elemScope_ = std::move(s); }
    float effCanvasH() const { return canvasH_ > 0.f ? canvasH_ : float(EditorSettings::instance().canvasHeight()); }
    float canvasH() const { return canvasH_; }
    int   canvasStatic() const { return canvasStatic_; }
    // Where a canvas SMALLER than its viewport sits in it: -1 inherit (Preferences ▸ default surface
    // anchor), else 0..8 row-major TL..BR (4 = centre). Per canvas WITH a global default, the same shape
    // canvasStatic uses — one setting could not express "centre my pages but pin this viewport's little
    // canvas top-left", and making it purely local would throw away the one-place default.
    int   canvasAnchor() const { return canvasAnchor_; }
    float guideW() const { return guideW_; }
    float guideH() const { return guideH_; }
    const std::string& title() const { return title_; }
    int   layout() const { return layout_; }
    int   gridColumns() const { return gridColumns_; }   // Grid layout column count (>=1)
    int   focusIndex() const { return focusIndex_; }     // Index Card layout: which child fills the page
    const std::string& borderColor() const { return borderColor_; }
    int   borderWidth()  const { return borderWidth_; }
    int   borderStyle()  const { return borderStyle_; }
    int   borderRadius() const { return borderRadius_; }
    const std::string& titleFont()  const { return titleFont_; }
    const std::string& titleColor() const { return titleColor_; }
    int   titlePadding() const { return titlePadding_; }
    int   titleStyle()   const { return titleStyle_; }
    int   titlePlace()   const { return titlePlace_; }
    int   titleEdge()    const { return titleEdge_; }
    int   titleAlign()   const { return titleAlign_; }

    void setCanvasSize(float w, float h) { canvasW_ = w; canvasH_ = h; canvasChanged.emit(); }
    void setCanvasStatic(int s)          { canvasStatic_ = s; canvasChanged.emit(); }
    void setCanvasAnchor(int a)          { canvasAnchor_ = a; canvasChanged.emit(); }
    void setGuideSize(float w, float h)  { guideW_ = w; guideH_ = h; canvasChanged.emit(); }
    void setTitle(const std::string& t)  { title_ = t; canvasChanged.emit(); }
    void setLayout(int m)                { layout_ = m; canvasChanged.emit(); }
    void setGridColumns(int c)           { gridColumns_ = c < 1 ? 1 : c; canvasChanged.emit(); }
    void setFocusIndex(int i)            { focusIndex_ = i < 0 ? 0 : i; canvasChanged.emit(); }
    void setBorderColor(const std::string& c) { borderColor_ = c; canvasChanged.emit(); }
    void setBorderWidth(int w)           { borderWidth_ = w; canvasChanged.emit(); }
    void setBorderStyle(int s)           { borderStyle_ = s; canvasChanged.emit(); }
    void setBorderRadius(int r)          { borderRadius_ = r; canvasChanged.emit(); }
    void setTitleFont(const std::string& f)  { titleFont_ = f; canvasChanged.emit(); }
    void setTitleColor(const std::string& c) { titleColor_ = c; canvasChanged.emit(); }
    void setTitlePadding(int p)          { titlePadding_ = p; canvasChanged.emit(); }
    void setTitleStyle(int s)            { titleStyle_ = s; canvasChanged.emit(); }
    void setTitlePlace(int p)            { titlePlace_ = p; canvasChanged.emit(); }
    void setTitleEdge(int e)             { titleEdge_ = e; canvasChanged.emit(); }
    void setTitleAlign(int a)            { titleAlign_ = a; canvasChanged.emit(); }

    // Monotonic change counter — bumped by every element mutation (add/remove/setProp/clearProp/setRect/
    // setElements). A renderer holds the widget tree as the running state and re-syncs an instance from its
    // element ONLY when this moves, instead of copying the model into every widget every frame.
    uint64_t generation() const { return generation_; }

    // --- Elements -------------------------------------------------------------------------------
    const std::vector<PanelElement>& elements() const { return elements_; }

    PanelElement*       get(int id)       { for (auto& e : elements_) if (e.id == id) return &e; return nullptr; }
    const PanelElement* get(int id) const { for (auto& e : elements_) if (e.id == id) return &e; return nullptr; }

    int add(const std::string& type, float x, float y, float w, float h,
            std::unordered_map<std::string, std::string> props = {}) {
        PanelElement e;
        e.id = nextId_++;
        e.uid = jf::makeUuid();   // every widget gets a proper UID at creation
        e.type = type; e.x = x; e.y = y; e.w = w; e.h = h; e.props = std::move(props);
        // A NEW WIDGET IS NEW ALL THE WAY DOWN. A panel carries its children as JSON inside its own
        // props, so every caller that builds a widget FROM COPIED PROPS — paste, duplicate, a page
        // duplicated onto another node — minted a fresh uid for the panel and handed its children the
        // source's. Two widgets then answered to one Widget ID, and "[@<uid>.prop]" could resolve to
        // either: the invariant that a uid names exactly one widget held for top-level widgets and
        // quietly did not for anything inside a panel.
        lastChildUids_.clear();
        if (auto it = e.props.find("children"); it != e.props.end() && !it->second.empty())
            it->second = freshChildUids_(it->second, lastChildUids_);
        elements_.push_back(std::move(e));
        const int id = elements_.back().id;
        ++generation_; elementAdded.emit(id);
        return id;
    }

    void remove(int id) {
        for (size_t i = 0; i < elements_.size(); ++i)
            if (elements_[i].id == id) { elements_.erase(elements_.begin() + i); ++generation_; elementRemoved.emit(id); return; }
    }

    void setRect(int id, float x, float y, float w, float h) {
        if (auto* e = get(id)) { e->x = x; e->y = y; e->w = w; e->h = h; ++generation_; elementChanged.emit(id); }
    }

    void setProp(int id, const std::string& k, const std::string& v) {
        if (auto* e = get(id)) { e->props[k] = v; ++generation_; elementChanged.emit(id); }
    }
    // Remove an override entirely so the property re-inherits its resolved default (a key's PRESENCE is the
    // override; setProp("") would leave it present-but-empty, which reads as blank, not the default).
    void clearProp(int id, const std::string& k) {
        if (auto* e = get(id)) { if (e->props.erase(k)) { ++generation_; elementChanged.emit(id); } }
    }

    // Bulk replace (undo/redo restore, reorder). Emits canvasChanged; callers refresh selection.
    void setElements(const std::vector<PanelElement>& v) { elements_ = v; ++generation_; canvasChanged.emit(); }

    // Full canvas-field snapshot (undo/redo of the Surface-canvas properties).
    struct CanvasState {
        float canvasW, canvasH, guideW, guideH;
        int   canvasStatic, canvasAnchor, layout, gridColumns, focusIndex, borderWidth, borderStyle, borderRadius, titlePadding, titleStyle, titlePlace, titleEdge, titleAlign;
        std::string title, borderColor, titleFont, titleColor;
        bool operator==(const CanvasState& o) const {
            return canvasW == o.canvasW && canvasH == o.canvasH && guideW == o.guideW && guideH == o.guideH &&
                   canvasStatic == o.canvasStatic && canvasAnchor == o.canvasAnchor &&
                   layout == o.layout && gridColumns == o.gridColumns &&
                   focusIndex == o.focusIndex && borderWidth == o.borderWidth && borderStyle == o.borderStyle &&
                   borderRadius == o.borderRadius && titlePadding == o.titlePadding && titleStyle == o.titleStyle && titlePlace == o.titlePlace &&
                   titleEdge == o.titleEdge && titleAlign == o.titleAlign && title == o.title && borderColor == o.borderColor &&
                   titleFont == o.titleFont && titleColor == o.titleColor;
        }
    };
    CanvasState canvasState() const {
        return { canvasW_, canvasH_, guideW_, guideH_, canvasStatic_, canvasAnchor_, layout_, gridColumns_, focusIndex_, borderWidth_, borderStyle_, borderRadius_,
                 titlePadding_, titleStyle_, titlePlace_, titleEdge_, titleAlign_, title_, borderColor_, titleFont_, titleColor_ };
    }
    void setCanvasState(const CanvasState& s) {
        canvasW_ = s.canvasW; canvasH_ = s.canvasH; guideW_ = s.guideW; guideH_ = s.guideH; canvasStatic_ = s.canvasStatic;
        canvasAnchor_ = s.canvasAnchor;
        layout_ = s.layout; gridColumns_ = s.gridColumns; focusIndex_ = s.focusIndex;
        borderWidth_ = s.borderWidth; borderStyle_ = s.borderStyle; borderRadius_ = s.borderRadius;
        titlePadding_ = s.titlePadding; titleStyle_ = s.titleStyle; titlePlace_ = s.titlePlace; titleEdge_ = s.titleEdge;
        titleAlign_ = s.titleAlign; title_ = s.title; borderColor_ = s.borderColor; titleFont_ = s.titleFont; titleColor_ = s.titleColor;
        canvasChanged.emit();
    }

    // --- Serialization (layout persistence) -----------------------------------------------------
    const std::string& uid() const { return uid_; }

    // Studio page shape: { canvas fields…, widgets:[{type,x,y,w,h,groupId,props{}}] }.
    jf::JJson toJson() const {
        jf::JJson o = jf::JJson::object();
        o["uid"] = uid_;
        o["canvasWidth"] = static_cast<double>(canvasW_); o["canvasHeight"] = static_cast<double>(canvasH_);
        if (minW_ > 0.f) o["minWidth"] = static_cast<double>(minW_);
        if (!elemScope_.empty()) o["elementScope"] = elemScope_;
        if (pageAreaSet_) {
            jf::JJson pa = jf::JJson::array();
            pa.push(jf::JJson(double(pageArea_.x)));     pa.push(jf::JJson(double(pageArea_.y)));
            pa.push(jf::JJson(double(pageArea_.width))); pa.push(jf::JJson(double(pageArea_.height)));
            o["pageArea"] = std::move(pa);
        }
        o["canvasStatic"] = static_cast<double>(canvasStatic_); o["canvasAnchor"] = static_cast<double>(canvasAnchor_); o["guideW"] = static_cast<double>(guideW_); o["guideH"] = static_cast<double>(guideH_);
        o["title"] = title_; o["layout"] = static_cast<double>(layout_);
        o["gridColumns"] = static_cast<double>(gridColumns_); o["focusIndex"] = static_cast<double>(focusIndex_);
        o["borderColor"] = borderColor_; o["borderWidth"] = static_cast<double>(borderWidth_); o["borderStyle"] = static_cast<double>(borderStyle_); o["borderRadius"] = static_cast<double>(borderRadius_);
        o["titleFont"] = titleFont_; o["titleColor"] = titleColor_; o["titlePadding"] = static_cast<double>(titlePadding_);
        o["titleStyle"] = static_cast<double>(titleStyle_); o["titlePlace"] = static_cast<double>(titlePlace_); o["titleEdge"] = static_cast<double>(titleEdge_); o["titleAlign"] = static_cast<double>(titleAlign_);
        jf::JJson arr = jf::JJson::array();
        for (const auto& e : elements_) arr.push(elementToJson(e));
        o["widgets"] = std::move(arr);
        return o;
    }

    // Re-mint every uid in a "children" blob, all the way down, and re-point the @-sigil references that
    // live INSIDE it so a copied panel's wiring follows the copy — the same rule Surface::relinkPastedRefs
    // applies to a pasted top-level set, applied where that function cannot see: nested in a prop string.
    // old->new lands in `map` so the caller can relink references from OUTSIDE the subtree that were
    // copied along with it. Unparseable JSON is returned untouched: a blob we cannot read is not a blob
    // we should rewrite.
    static std::string freshChildUids_(const std::string& childrenJson,
                                       std::unordered_map<std::string, std::string>& map) {
        auto j = jf::JJson::tryParse(childrenJson);
        if (!j || !j->isArray()) return childrenJson;
        std::function<void(jf::JJson&)> walk = [&](jf::JJson& arr) {
            for (jf::JJson& w : arr.arr()) {
                const std::string old = w["uid"].str();
                if (!old.empty()) { std::string nu = jf::makeUuid(); map[old] = nu; w["uid"] = nu; }
                jf::JJson& props = w["props"];
                const std::string kids = props["children"].str();
                if (kids.empty()) continue;                       // a leaf, or a panel with nothing in it
                if (auto k = jf::JJson::tryParse(kids); k && k->isArray()) { walk(*k); props["children"] = k->dump(); }
            }
        };
        walk(*j);
        // The uid FIELDS already hold their new values, and a uid is only ever written with an "@" in
        // front of it when it is a reference — so this pass rewrites references and cannot touch the
        // identities just assigned.
        std::string out = j->dump();
        for (const auto& [oldU, newU] : map) {
            const std::string needle = "@" + oldU, repl = "@" + newU;
            for (size_t at = out.find(needle); at != std::string::npos; at = out.find(needle, at + repl.size()))
                out.replace(at, needle.size(), repl);
        }
        return out;
    }

    // The old->new child uids minted by the LAST add(), so a paste can relink a reference that crossed
    // from a sibling into a copied panel's children. Empty for a widget that carries none.
    const std::unordered_map<std::string, std::string>& lastAddChildUids() const { return lastChildUids_; }

    // ONE element writer, because a panel's children are stored in this same shape (Surface::exitScope
    // serialises a panel scope back into the panel's "children" prop). That copy was written out by hand
    // and had drifted: it omitted groupId, so every group made INSIDE a panel was silently dissolved the
    // moment you left the panel, and omitted id, which is the key a sigil ref ([@type_id.prop]) names.
    // A field added here now reaches both paths, which is the only way they can stay the same shape.
    static jf::JJson elementToJson(const PanelElement& e) {
        jf::JJson w = jf::JJson::object();
        w["id"] = static_cast<double>(e.id);   // in-memory handle + sigil ref key ([@type_id.prop])
        w["uid"] = e.uid;                      // stable per-widget UUID (the user-facing Widget ID)
        w["type"] = e.type; w["x"] = static_cast<double>(e.x); w["y"] = static_cast<double>(e.y);
        w["w"] = static_cast<double>(e.w); w["h"] = static_cast<double>(e.h);
        w["groupId"] = static_cast<double>(e.groupId);
        jf::JJson p = jf::JJson::object();
        for (const auto& [k, v] : e.props) p[k] = v;
        w["props"] = std::move(p);
        return w;
    }
    void load(const jf::JJson& o) {
        // 0 (or absent) = "follow Preferences"; only a page that needs its own size stores one.
        canvasW_ = o["canvasWidth"].number(0); canvasH_ = o["canvasHeight"].number(0);
        minW_ = float(o["minWidth"].number(0));
        elemScope_ = o["elementScope"].str();
        pageArea_ = {}; pageAreaSet_ = false;
        if (o.contains("pageArea") && o["pageArea"].isArray() && o["pageArea"].arr().size() == 4) {
            const auto& pa = o["pageArea"].arr();
            pageArea_ = { float(pa[0].number()), float(pa[1].number()),
                          float(pa[2].number()), float(pa[3].number()) };
            pageAreaSet_ = true;
        }
        canvasStatic_ = static_cast<int>(o["canvasStatic"].number(0));
        canvasAnchor_ = static_cast<int>(o["canvasAnchor"].number(-1));  // absent = inherit the global
        guideW_ = o["guideW"].number(0); guideH_ = o["guideH"].number(0);
        title_ = o["title"].str(); layout_ = static_cast<int>(o["layout"].number(0));
        gridColumns_ = static_cast<int>(o["gridColumns"].number(2)); focusIndex_ = static_cast<int>(o["focusIndex"].number(0));
        uid_ = o["uid"].str(); if (uid_.empty()) uid_ = jf::makeUuid();   // old panels: mint a UID now (persists on next save)
        borderColor_ = o["borderColor"].str(); borderWidth_ = static_cast<int>(o["borderWidth"].number(0)); borderStyle_ = static_cast<int>(o["borderStyle"].number(1)); borderRadius_ = static_cast<int>(o["borderRadius"].number(4));
        titleFont_ = o["titleFont"].str(); titleColor_ = o["titleColor"].str(); titlePadding_ = static_cast<int>(o["titlePadding"].number(4));
        titleStyle_ = static_cast<int>(o["titleStyle"].number(1)); titlePlace_ = static_cast<int>(o["titlePlace"].number(0)); titleEdge_ = static_cast<int>(o["titleEdge"].number(0)); titleAlign_ = static_cast<int>(o["titleAlign"].number(0));
        elements_.clear(); nextId_ = 1;
        for (const auto& w : o["widgets"].arr()) {
            PanelElement e;
            const int savedId = static_cast<int>(w["id"].number(0));   // restore the persisted uid; fall back for old files
            e.id = savedId > 0 ? savedId : nextId_;
            if (e.id >= nextId_) nextId_ = e.id + 1;                    // keep the counter ahead of every restored id
            e.uid = w["uid"].str();
            if (e.uid.empty()) e.uid = jf::makeUuid();                  // old files: mint a UID now (persists on next save)
            e.type = w["type"].str();
            e.x = w["x"].number(); e.y = w["y"].number(); e.w = w["w"].number(160); e.h = w["h"].number(90);
            e.groupId = static_cast<int>(w["groupId"].number(0));
            for (const auto& [k, v] : w["props"].obj()) e.props[k] = v.str();
            elements_.push_back(std::move(e));
        }
        JLOGC("surface.panel", jf::JLogLevel::Info)
            << "load panel uid=" << uid_ << " title=\"" << title_ << "\" elements=" << elements_.size()
            << " canvas=" << canvasW_ << "x" << canvasH_ << " layout=" << layout_;
        canvasChanged.emit();
    }

    // --- Grouping + z-order ---------------------------------------------------------------------
    void setGroupId(int id, int gid) { if (auto* e = get(id)) { e->groupId = gid; elementChanged.emit(id); } }
    int  nextGroupId() const { int m = 0; for (const auto& e : elements_) m = std::max(m, e.groupId); return m + 1; }

    // Element order in elements_ IS z-order (rendered front-to-back). Move the given ids to the end
    // (front) or the start (back), preserving their relative order; emit canvasChanged for a re-render.
    void bringToFront(const std::vector<int>& ids) { reorder_(ids, true); }
    void sendToBack(const std::vector<int>& ids)   { reorder_(ids, false); }

    // Single-layer z-order steps (Bring Forward / Send Back): shift the selection one place in the draw
    // order (later index = on top), past exactly one unselected neighbour. A contiguous selected block
    // moves together and keeps its internal order.
    void moveForward(const std::vector<int>& ids)  { shiftZ_(ids, +1); }
    void moveBackward(const std::vector<int>& ids) { shiftZ_(ids, -1); }

private:
    void shiftZ_(const std::vector<int>& ids, int dir) {
        auto sel = [&](int id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
        const int n = static_cast<int>(elements_.size());
        bool changed = false;
        if (dir > 0) {                                   // bring forward: sweep top→bottom
            for (int i = n - 2; i >= 0; --i)
                if (sel(elements_[i].id) && !sel(elements_[i + 1].id)) { std::swap(elements_[i], elements_[i + 1]); changed = true; }
        } else {                                         // send back: sweep bottom→top
            for (int i = 1; i < n; ++i)
                if (sel(elements_[i].id) && !sel(elements_[i - 1].id)) { std::swap(elements_[i], elements_[i - 1]); changed = true; }
        }
        if (changed) canvasChanged.emit();
    }
    void reorder_(const std::vector<int>& ids, bool toFront) {
        auto sel = [&](const PanelElement& e) { return std::find(ids.begin(), ids.end(), e.id) != ids.end(); };
        std::vector<PanelElement> moved, rest;
        for (auto& e : elements_) (sel(e) ? moved : rest).push_back(e);
        elements_.clear();
        if (toFront) { elements_ = std::move(rest);  for (auto& e : moved) elements_.push_back(e); }
        else         { elements_ = std::move(moved); for (auto& e : rest)  elements_.push_back(e); }
        canvasChanged.emit();
    }

    std::vector<PanelElement> elements_;
    std::unordered_map<std::string, std::string> lastChildUids_;   // see lastAddChildUids()
    uint64_t generation_ = 0;   // bumped on every element mutation (see generation())
    int   nextId_   = 1;
    float canvasW_  = 1280.f, canvasH_ = 720.f;
    float minW_ = 0.f;
    std::string elemScope_;   // the array element this page is about; "" for an ordinary page
    jf::JRect   pageArea_{};       // where a page window opens on THIS surface (page units); see pageArea()
    bool        pageAreaSet_ = false;   // …declared at all: a zero one means "no page windows here"      // see minW(): the reflow floor, 0 = none
    int   canvasStatic_ = 0;          // 0 inherit / 1 fixed 1:1 / 2 scale-to-fit
    int   canvasAnchor_ = -1;         // -1 inherit the global default / 0..8 row-major TL..BR
    float guideW_   = 0.f, guideH_ = 0.f;
    std::string title_;
    std::string uid_ = jf::makeUuid();   // stable per-panel UUID — the canvas's own Widget ID
    int   layout_       = 0;          // Free
    int   gridColumns_  = 2;          // Grid layout columns (matches the old hard-wired 2)
    int   focusIndex_   = 0;          // Index Card: which child fills the page
    std::string borderColor_;         // "" = inherit scheme
    int   borderWidth_  = 0;
    int   borderStyle_  = 1;          // Solid
    int   borderRadius_ = 4;
    std::string titleFont_;           // "" = inherit
    std::string titleColor_;          // "" = inherit
    int   titlePadding_ = 4;
    int   titleStyle_   = 1;          // Underline
    int   titlePlace_   = 0;          // Inside
    int   titleEdge_    = 0;          // Top
    int   titleAlign_   = 0;          // Left
};

// Re-key every element's "node" prop after a navigation-tree rename/reparent: a viewport (or node-tagged
// control) referencing oldPrefix (or a path under it) is rewritten onto newPrefix, so placements follow the
// node they point at. Ports the effect of PanelLibrary::renamePrefix on placed viewports.
inline void retagNodeProp(PanelModel& m, const std::string& oldPrefix, const std::string& newPrefix) {
    if (oldPrefix.empty() || oldPrefix == newPrefix) return;
    const std::string under = oldPrefix + "/";
    for (const auto& e : m.elements()) {
        const std::string nd = e.prop("node");
        if (nd.empty()) continue;
        if (nd == oldPrefix)              m.setProp(e.id, "node", newPrefix);
        else if (nd.rfind(under, 0) == 0) m.setProp(e.id, "node", newPrefix + nd.substr(oldPrefix.size()));
    }
}

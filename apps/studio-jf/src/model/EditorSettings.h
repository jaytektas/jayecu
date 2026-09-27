#pragma once

// EditorSettings — the surface-editor canvas preferences (grid size, grid visibility, snap-to-grid
// and snap-to-neighbour magnetism, plus the default canvas size/scaling/anchor), shared across every
// surface and edited in Preferences ▸ Surface. A singleton persisted via JSettings, emitting changed()
// so open surfaces update live — same pattern as UnitManager. (The studio's table-editor fields are
// added when the table editor is ported.)

#include <j/core/Signal.h>
#include <j/config/Settings.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>

class EditorSettings {
public:
    static EditorSettings& instance() { static EditorSettings s; return s; }

    jf::JSignal<> changed;

    int  gridSize() const { return gridSize_; }                 // virtual units between grid points
    bool showGrid() const { return showGrid_; }
    bool snapToGrid() const { return snapToGrid_; }
    bool snapToNeighbours() const { return snapToNeighbours_; }  // widget-edge magnetism
    bool staticSurfaceSize() const { return staticSurfaceSize_; }
    // WHAT THE MAXIMISE BUTTON MEANS. True (the default) fills the whole tab, which is what a maximise
    // button is expected to do; false stops at the room left after the live strip across the top and the
    // channel rail down the right, keeping those instruments visible while a page is maximised. Both are
    // defensible, so it is asked rather than decided.
    bool maximiseFillsTab() const { return maximiseFillsTab_; }
    // DEFAULT anchor for a canvas that does not override it (0..8 row-major TL..BR; 4 = centre). A canvas
    // may still pick its own — PanelModel::canvasAnchor, where -1 means "inherit this".
    int  surfaceAnchor() const { return surfaceAnchor_; }
    int  canvasWidth()  const { return canvasWidth_; }
    int  canvasHeight() const { return canvasHeight_; }
    int  defaultDecimals() const { return defaultDecimals_; }    // Preferences▸Appearance; stored, not consumed: value widgets format via their own per-control format prop

    void setGridSize(int px)          { if (px != gridSize_)  { gridSize_ = px;  put("gridSize", px); } }
    void setShowGrid(bool on)         { if (on != showGrid_)  { showGrid_ = on;  put("showGrid", on); } }
    void setSnapToGrid(bool on)       { if (on != snapToGrid_){ snapToGrid_ = on; put("snapToGrid", on); } }
    void setSnapToNeighbours(bool on) { if (on != snapToNeighbours_) { snapToNeighbours_ = on; put("snapToNeighbours", on); } }
    void setStaticSurfaceSize(bool on){ if (on != staticSurfaceSize_) { staticSurfaceSize_ = on; put("staticSurfaceSize", on); } }
    void setMaximiseFillsTab(bool on) { if (on != maximiseFillsTab_) { maximiseFillsTab_ = on; put("maximiseFillsTab", on); } }
    void setSurfaceAnchor(int a)      { if (a != surfaceAnchor_) { surfaceAnchor_ = a; put("surfaceAnchor", a); } }
    void setDefaultDecimals(int d)    { if (d != defaultDecimals_) { defaultDecimals_ = d; put("defaultDecimals", d); } }

    // The schema the studio came up on last time. Without it every launch reverts to the built-in jayecu
    // definition, so the session after an .ini import connects with the WRONG protocol — the native
    // identity handshake, which a TunerStudio ECU answers with silence ("ECU did not identify").
    std::string lastSchema() const { return jf::JSettings::instance().get<std::string>("editor.lastSchema", ""); }
    void setLastSchema(const std::string& path) { jf::JSettings::instance().set("editor.lastSchema", path); }
    void setCanvasSize(int w, int h)  { canvasWidth_ = w; canvasHeight_ = h; auto& s = jf::JSettings::instance();
                                        s.set("editor.canvasWidth", jf::JVariant(w)); s.set("editor.canvasHeight", jf::JVariant(h)); s.saveJson(); changed.emit(); }

    // --- Widget creation defaults (Preferences ▸ Widget Defaults) -------------------------------
    // Per-widget-type size + props a NEW control is born with, layered over the descriptor built-ins.
    // Absent = none stored (use built-ins). Keyed by widget type (the PanelElement / widget-class type key).
    bool widgetDefaultSize(const std::string& type, int& w, int& h) const {
        const std::string s = jf::JSettings::instance().get<std::string>("editor.wdefSize." + type, "");
        if (s.empty()) return false;
        const auto x = s.find('x'); if (x == std::string::npos) return false;
        w = std::atoi(s.substr(0, x).c_str()); h = std::atoi(s.substr(x + 1).c_str()); return true;
    }
    void setWidgetDefaultSize(const std::string& type, int w, int h) {
        putStr("wdefSize." + type, std::to_string(w) + "x" + std::to_string(h));
    }
    std::unordered_map<std::string, std::string> widgetDefaultProps(const std::string& type) const {
        return splitMap(jf::JSettings::instance().get<std::string>("editor.wdefProps." + type, ""));
    }
    void setWidgetDefaultProps(const std::string& type, const std::unordered_map<std::string, std::string>& p) {
        putStr("wdefProps." + type, joinMap(p));
    }
    void clearWidgetDefaults(const std::string& type) {
        auto& s = jf::JSettings::instance();
        s.remove("editor.wdefSize." + type); s.remove("editor.wdefProps." + type); s.saveJson(); changed.emit();
    }

    // Where a NEW control's caption / units Label elements are placed relative to the control at drop —
    // side (Above/Below/Left/Right) + gap px, per widget type. Labels are separate grouped elements now,
    // so this is the "default drop placement per widget". Defaults: caption above, units to the right.
    struct LabelPlacement { std::string side; int gap; };
    LabelPlacement captionPlacement(const std::string& type) const {
        auto& s = jf::JSettings::instance();
        return { s.get<std::string>("editor.capSide." + type, "Above"), s.get<int>("editor.capGap." + type, 2) };
    }
    void setCaptionPlacement(const std::string& type, const std::string& side, int gap) {
        auto& s = jf::JSettings::instance();
        s.set("editor.capSide." + type, jf::JVariant(side)); s.set("editor.capGap." + type, jf::JVariant(gap)); s.saveJson(); changed.emit();
    }

    // --- Property globals (Preferences ▸ Globals) -----------------------------------------------
    // A widget property sitting on choice 0 ("global") resolves HERE, and nowhere else. One store, one
    // meaning: "global" always means "ask the globals", never "borrow a sibling property" and never a
    // literal buried in the render. The value is the property's own choice index (1..N); 0 is reserved
    // for the few keys that offer a "follow" answer (axis decimals follow the cell's). Queried live, so
    // an edit in Preferences redraws without a restart.
    int propGlobal(const std::string& key, int def) const {
        return jf::JSettings::instance().get<int>("editor.global." + key, def);
    }
    void setPropGlobal(const std::string& key, int v) {
        auto& s = jf::JSettings::instance();
        s.set("editor.global." + key, jf::JVariant(v)); s.saveJson(); changed.emit();
    }

    // --- Panel creation defaults (Preferences ▸ Panel Defaults) ---------------------------------
    std::unordered_map<std::string, std::string> panelDefaultProps() const {
        return splitMap(jf::JSettings::instance().get<std::string>("editor.panelDefaults", ""));
    }
    void setPanelDefaultProps(const std::unordered_map<std::string, std::string>& p) { putStr("panelDefaults", joinMap(p)); }

private:
    EditorSettings() { load(); }

    // props serialise as "k=v\x1fk=v" (unit separator avoids clashing with values).
    static std::string joinMap(const std::unordered_map<std::string, std::string>& m) {
        std::string out; for (const auto& [k, v] : m) { if (!out.empty()) out += '\x1f'; out += k; out += '='; out += v; } return out;
    }
    static std::unordered_map<std::string, std::string> splitMap(const std::string& s) {
        std::unordered_map<std::string, std::string> m; std::string tok; std::istringstream ss(s);
        while (std::getline(ss, tok, '\x1f')) { const auto e = tok.find('='); if (e != std::string::npos) m[tok.substr(0, e)] = tok.substr(e + 1); }
        return m;
    }
    void putStr(const std::string& key, const std::string& v) {
        auto& s = jf::JSettings::instance(); s.set("editor." + key, jf::JVariant(v)); s.saveJson(); changed.emit();
    }

    void load() {
        auto& s = jf::JSettings::instance();
        gridSize_         = s.get<int>("editor.gridSize", gridSize_);
        showGrid_         = s.get<bool>("editor.showGrid", showGrid_);
        snapToGrid_       = s.get<bool>("editor.snapToGrid", snapToGrid_);
        snapToNeighbours_ = s.get<bool>("editor.snapToNeighbours", snapToNeighbours_);
        staticSurfaceSize_= s.get<bool>("editor.staticSurfaceSize", staticSurfaceSize_);
        maximiseFillsTab_ = s.get<bool>("editor.maximiseFillsTab", maximiseFillsTab_);
        surfaceAnchor_    = s.get<int>("editor.surfaceAnchor", surfaceAnchor_);
        canvasWidth_      = s.get<int>("editor.canvasWidth", canvasWidth_);
        canvasHeight_     = s.get<int>("editor.canvasHeight", canvasHeight_);
        defaultDecimals_  = s.get<int>("editor.defaultDecimals", defaultDecimals_);
    }
    template <typename T> void put(const char* key, T v) {
        auto& s = jf::JSettings::instance();
        s.set(std::string("editor.") + key, jf::JVariant(v));
        s.saveJson();
        changed.emit();
    }

    int  gridSize_ = 20;
    bool showGrid_ = true, snapToGrid_ = true, snapToNeighbours_ = true, staticSurfaceSize_ = false;
    bool maximiseFillsTab_ = true;    // see maximiseFillsTab()
    int  canvasWidth_ = 1280, canvasHeight_ = 720;
    int  surfaceAnchor_ = 4;          // centre, unless a canvas overrides it
    int  defaultDecimals_ = 1;
};

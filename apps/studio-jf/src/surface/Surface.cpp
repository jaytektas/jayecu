#include "Surface.h"
#include "StyleMetrics.h"
#include <fstream>
#include <filesystem>
#include <j/core/Log.h>
#include <j/core/FocusManager.h>            // JLOGC — trace edit-mode / selection / group / ungroup (debugging aid)
#include "CanvasWidget.h"          // per-element render instances (class-hierarchy port) + makeWidgetInstance
#include "WidgetRegistry.h"
#include "ContainerLayout.h"
#include "widgets/TableWidget.h"   // table state/geometry + cell ops (TableWidget:: statics)
#include "widgets/LabelWidget.h"   // in-place caption editing (LabelWidget::labelEdits / naturalSize)
#include "widgets/PanelWidget.h"
#include "widgets/ViewportWidget.h"   // descend a right-click into a viewport to reach a mirrored table/curve
#include "widgets/ChannelListWidget.h"   // the watch list's channel-set parse/join
#include "../model/LineGraphModel.h"     // the trace view's per-line list
#include "../model/Cache.h"
#include "../model/StudioPaths.h"
#include "../model/LearnedOps.h"
#include "../model/Keymap.h"      // global keyboard bindings — Group/Ungroup on the surface
#include "../model/MetaModel.h"   // arrays1d() for defaultControlFor
#include "../model/EditorSettings.h"
#include "../model/UnitManager.h"      // Units ▸ — the quantity's units for the run-mode menu
#include "../model/ChannelPrefs.h"     // a unit chosen once holds wherever that channel is shown
#include "../model/MathEvaluator.h"   // control visibility = evaluate the stored expression
#include <j/core/Uuid.h>          // a pasted viewport mints its own canvas key
#include "PanelLibrary.h"         // nodeviewport::resolver() — size a dropped viewport to its node's page
#include "../model/TableFile.h"   // a table as CSV: save a calibration once, load it anywhere

#include <j/core/DragDrop.h>
#include <j/platform/Clipboard.h>

#include <algorithm>
#include "../app/Resources.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "../model/Perf.h"

#include <unordered_set>
#include <utility>

using jf::JRect;
using jf::JPrimitiveBuffer;

Surface::Surface(jf::JSceneGraph& g, const Cache& cache, PanelModel* model)
    : jf::JWidget(g, "Surface"), cache_(cache), model_(model), canvas_(g) {
    // A PAGE THAT IS ABOUT ONE ELEMENT SAYS SO, and its "[*]" bindings resolve to that element from the
    // first frame — however the page was reached. This used to arrive only through enterViewport, from the
    // viewport that showed the page; pages open in windows now, and the document has no viewports left.
    if (model_) scopeCtx_ = model_->elementScope();
    addChild(&canvas_);              // the canvas is this surface's child; the instances hang off it in turn
    canvas_.setScanExcluded(true);   // the Surface drives it manually — keep it (and its elements) out of
                                     // the framework's global focus/hit scans, while still painting normally.
                                     // Exclusion travels down the tree, so this one call covers every element.
    // The SURFACE itself is a tab stop, so the canvas is a NESTED focus domain: Tab walks into it, moves across
    // its controls, and walks out the far side into the next dock. Without this the canvas was unreachable —
    // its controls are not in the framework's chain, so once Tab left it there was no way back.
    setFocusPolicy(jf::JFocusPolicy::StrongFocus);
    buildContextMenu();
    buildTableMenu();
    buildCurveMenu();
    buildChannelMenus();
    setContextMenu(&menu_);   // edit is the default mode
}

Surface::~Surface() = default;   // here, where CanvasWidget is complete (instances_ holds unique_ptr<it>)

// Reconcile the persistent instance map to the CURRENT model's element set: create an instance for any new
// element (or one whose id was reused for a different type), and erase instances whose element is gone. A new
// instance is seeded with setData() so its whole subtree (a container's children) exists immediately — the
// @-sigil can then resolve nested widgets even before the first paint. Element counts are small, so a full
// reconcile every call is fine. Called at the top of the render loop and by the sigil/inspector entry points.
void Surface::syncInstances() {
    PERF_SCOPE("Surface::syncInstances");
    if (!model_) { canvas_.clearInstances(); return; }
    std::unordered_set<int> live;
    for (const PanelElement& el : model_->elements()) {
        live.insert(el.id);
        CanvasWidget* inst = canvas_.instanceFor(el.id);
        if (!inst || inst->elementType() != el.type) {
            std::unique_ptr<CanvasWidget> made = makeWidgetInstance(el.type, sceneGraph());
            const bool created = static_cast<bool>(made);
            inst = canvas_.adoptInstance(el.id, std::move(made));   // the CONTAINER owns it (framework adopt)
            // ENTERED a viewport: the surface edits its page directly, so ViewportWidget — which normally
            // pushes its sensor down — is not in the loop at all. Without this the template's "[*]" went
            // unresolved the moment you drilled in to edit it, so a calibration curve showed nothing.
            if (created && inst) {
                inst->setSource(el);   // hydrate the owned override store + resolved render copy
                const std::string bind = !el.prop("signalName").empty() ? el.prop("signalName") : el.prop("node");
                JLOGC("surface.widget", jf::JLogLevel::Debug)
                    << "create " << el.type << " id=" << el.id << " uid=" << el.uid << " bind=" << bind;
            }
        }
        if (inst) inst->setElementContext(scopeCtx_);   // re-applied each pass, so leaving a scope clears it
    }
    // Erase instances whose element is gone (collect first — cannot erase while iterating the index).
    std::vector<int> dead;
    for (const auto& [id, inst] : canvas_.instancesById())
        if (!live.count(id)) {
            JLOGC("surface.widget", jf::JLogLevel::Debug)
                << "destroy " << (inst ? inst->elementType() : std::string("?")) << " id=" << id;
            dead.push_back(id);
        }
    for (int id : dead) canvas_.eraseInstance(id);
}

// REHYDRATE the widget tree from the model — but ONLY on a wholesale model change (generation moved), i.e. an
// undo/redo (setElements), a structural add/remove, or a geometry setRect. NOT on a normal prop edit: those are
// authored directly on the widget (setOwnProp) and never bump the model generation, so the widget stays the
// running state and is never clobbered here. This is safe because every edit commits its owned state into the
// model at its undo-snapshot (commitEdit → commitInstances), so the model is always current when we re-source.
void Surface::refreshInstancesIfChanged() {
    if (!model_ || model_->generation() == syncedGen_) return;
    syncedGen_ = model_->generation();
    for (auto& [id, inst] : canvas_.instancesById())
        if (inst) if (const PanelElement* el = model_->get(id)) inst->setSource(*el);
}

// Save-time: flush every live instance's owned state back into model_ (and, for containers, into their source
// models) just before serialization. saveContent is a no-op for a widget that owns nothing, so this only writes
// truly-owned state (e.g. a table's view). This is the WRITE half of "serialise only at file boundaries".
void Surface::commitInstances() {
    if (!model_) return;
    syncInstances();
    for (auto& [id, inst] : canvas_.instancesById())
        if (inst)
            if (PanelElement* el = model_->get(id)) { inst->commit(*el); inst->commitOwnedChildren(); }
    // …and any panel scope still OPEN. The instances above flushed into the scope's temporary model, which
    // is not what gets serialised — the outer page is — so without this a save made while inside a panel
    // wrote the panel exactly as it was when you entered it.
    flushScopes();
}

// Pure lookup — the persistent instance for an element id, else nullptr. Never creates (syncInstances owns
// the lifetime); the render loop and the inspector run after a sync, so the instance is present.
CanvasWidget* Surface::instanceFor(const PanelElement& el) {
    return canvas_.instanceFor(el.id);   // the container owns the instances now
}

// Child paint, through the framework container. Two passes: pass 1 preps + registers every instance as a
// container node in model order (business visibility rides the framework isVisible() flag; hidden nodes still
// register so managed-layout indices are unchanged); the container then paints the shown/on-viewport nodes
// clipped in z order; pass 2 has the Surface draw the run-mode disabled wash / selection outlines / group-box
// unions ON TOP. Bounds come from the Surface's screenRectOf, so managed panel/grid/card layouts place
// children identically. The frame/title/grid/marquee/breadcrumb/armed-spacing chrome is drawn by the caller.
void Surface::paintElements_(JPrimitiveBuffer& buf, const Xform& t, bool editing,
                             std::unordered_map<int, JRect>& groupBoxes) {
    const auto& els = model_->elements();
    // A caption's help comes from the control it was dropped WITH, and grouping is what records that pairing
    // (addWidgetAt sets one group id across the control and its labels). Resolved once per frame here rather
    // than per element, so a page of captions costs one walk and not a search each.
    std::unordered_map<int, std::string> groupBind;
    for (const PanelElement& e : els) {
        if (e.groupId == 0) continue;
        const std::string sig = e.prop("signalName");
        if (!sig.empty()) groupBind.emplace(e.groupId, sig);   // first bound member wins; a group has one control
    }

    // Pass 1 — prep + register every instance in MODEL ORDER (so managed-layout indices are unchanged).
    canvas_.clearNodes();
    PERF_COUNT_N("surface elements walked", (long long)els.size());
    for (size_t i = 0; i < els.size(); ++i) {
        const PanelElement& el = els[i];
        CanvasWidget* w = instanceFor(el);
        if (!w) continue;
        canvas_.addNode(w);                             // a hidden node still holds its managed-layout slot
        bool shown = true;                              // business visibility: node view-switcher + run condition
        if (nodeHidden_(el)) shown = false;
        // THE CONDITIONS APPLY WHILE EDITING TOO. Alternatives are authored at the SAME coordinates —
        // only one of them is ever on screen — so ignoring the conditions here drew all of them at once
        // and turned a page into a pile of overprinted captions belonging to nothing you could point at.
        // A page edits the way it runs. Show Hidden Widgets brings the rest back when one needs fixing.
        const bool revealing = editing && s_showHiddenInEdit;
        bool hiddenByCond = false;
        if (shown && !revealing) {
            if (!w->visibleNow()) { shown = false; }
        } else if (shown && revealing && !w->visibleNow()) {
            hiddenByCond = true;                        // drawn, and outlined below so it reads as hidden
        }
        (void)hiddenByCond;
        w->setVisible(shown);
        if (!shown) continue;
        const std::string sig = el.prop("signalName");   // prep: live value + cache + enable-condition disabled flag
        w->setValue(sig.empty() ? 0.0 : MathEvaluator::instance().evaluate(sig));
        w->setCache(&cache_);
        if (sig.empty() && el.groupId != 0) {            // a caption borrows its control's help (see setHelpBind)
            const auto gb = groupBind.find(el.groupId);
            w->setHelpBind(gb == groupBind.end() ? std::string() : gb->second);
        } else {
            w->setHelpBind({});
        }
        w->setRenderDisabled(!w->enabledNow());
        w->setRenderZoom(t.scale);                       // the view zoom, so text scales with the view alone
        // …and which page an unbound viewport should show. Pushed for the same reason the zoom is: a
        // widget cannot reach its container, and THIS surface's node is the answer.
        if (auto* vp = dynamic_cast<ViewportWidget*>(w)) vp->setSurfaceNode(activeNode_);
        w->setBounds(screenRectOf(i, t));                // Surface owns the transform (Stage 3 moves it to the canvas)
    }
    canvas_.setCamera(camera());                         // gives the container the viewport (clip + cull)
    canvas_.populateRenderPrimitives(buf);               // paints shown, on-viewport nodes clipped, in z order
    // Pass 2 — overlays OVER the children (identical work to the flag-off loop's overlay half).
    for (size_t i = 0; i < els.size(); ++i) {
        const PanelElement& el = els[i];
        if (nodeHidden_(el)) continue;
        CanvasWidget* iw = instanceFor(el);
        const bool revealing = editing && s_showHiddenInEdit;
        if (!revealing && iw && !iw->visibleNow()) continue;
        const JRect r = screenRectOf(i, t);
        // A REVEALED WIDGET SAYS SO. It is on screen only because Show Hidden Widgets is on; without a
        // mark it looks like part of the page and the author edits a thing no tuner will ever see.
        if (revealing && iw && !iw->visibleNow()) {
            static constexpr uint8_t kNoFill[4] = {0, 0, 0, 0};
            buf.pushRectangle(r.x, r.y, r.width, r.height, kNoFill, 2.f, 1.f, jf::Colors::Accent);
        }
        const bool condFalse = iw && !iw->enabledNow();
        // A container is NOT washed: its children each carry the disabled state down and fade themselves, so
        // the frame and title stay readable — the contents of a disabled group grey, not the group itself.
        // A label fades its own caption, which reads better than a slab over text.
        if (!editing && condFalse && el.type != "label" && el.type != "panel" && el.type != "viewport")
            CanvasWidget::disabledWash(buf, r);
        // Run-mode keyboard focus: one ring, drawn here for every widget that does not draw its own (the
        // hosted framework controls do). Without it a Tab landed on an enum picker or a table invisibly.
        if (!editing && el.id == activeControl_) {
            CanvasWidget* fw = instanceFor(el);
            if (fw && !fw->drawsOwnFocus() && CanvasWidget::focusRingEnabled(el)) {
                static const uint8_t clear[4] = { 0, 0, 0, 0 };
                buf.pushRectangle(r.x - 2.f, r.y - 2.f, r.width + 4.f, r.height + 4.f, clear, 3.f, 2.f,
                                  jf::Colors::Accent);
            }
        }
        if (editing && isSelected(el.id)) {
            drawSelection(buf, r, selection_.size() == 1, el.groupId != 0);
            if (el.groupId) {
                const auto it = groupBoxes.find(el.groupId);
                if (it == groupBoxes.end()) groupBoxes.emplace(el.groupId, r);
                else {
                    JRect& g = it->second;
                    const float x0 = std::min(g.x, r.x), y0 = std::min(g.y, r.y);
                    const float x1 = std::max(g.x + g.width,  r.x + r.width);
                    const float y1 = std::max(g.y + g.height, r.y + r.height);
                    g = JRect{ x0, y0, x1 - x0, y1 - y0 };
                }
            }
        }
    }
}

// Resolve an @-sigil address to a live widget anywhere in the active tree (top-level or nested); syncInstances
// first so the tree is present even before the first paint (e.g. a sigil picker opened right after a load).
CanvasWidget* Surface::widgetByUid(const std::string& addr) {
    syncInstances();
    for (auto& [id, inst] : canvas_.instancesById())
        if (inst) if (CanvasWidget* w = inst->findWidget(addr)) return w;
    return nullptr;
}

bool Surface::hasUid(const std::string& addr) const {
    if (!model_ || addr.empty()) return false;
    // Rebuilt only when the document changes: the token set is a property of the model, and the model is
    // the same object for thousands of resolutions between edits.
    if (model_->generation() != uidGen_) {
        uidGen_ = model_->generation();
        uidIndex_.clear();
        std::vector<std::string> toks;
        for (const auto& [id, inst] : canvas_.instancesById()) if (inst) inst->collectSigilTokens(toks);
        for (const std::string& t : toks) {                  // tokens are "<uid>.<prop>" — index the uid
            const size_t dot = t.rfind('.');
            uidIndex_.insert(dot == std::string::npos ? t : t.substr(0, dot));
        }
    }
    return uidIndex_.count(addr) != 0;
}

void Surface::collectSigilTokens(std::vector<std::string>& out) {
    syncInstances();
    for (auto& [id, inst] : canvas_.instancesById()) if (inst) inst->collectSigilTokens(out);
}

std::vector<PanelElement> Surface::clipboard_;
int Surface::pasteSeq_ = 0;
std::vector<std::vector<double>> Surface::tableClip_;

static bool elementsEqual(const std::vector<PanelElement>& a, const std::vector<PanelElement>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const PanelElement& x = a[i]; const PanelElement& y = b[i];
        if (x.id != y.id || x.type != y.type || x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h
            || x.groupId != y.groupId || x.props != y.props) return false;
    }
    return true;
}

// A mergeable full-elements snapshot swap: consecutive commands sharing a non-negative id fold into
// one (keeps the first `before`, adopts the latest `after`) so a burst of property keystrokes = one
// undo step. mergeId < 0 disables merging (drags / discrete menu ops each stand alone).
namespace {
class ElementsSwap : public jf::JUndoCommand {
public:
    ElementsSwap(PanelModel* m, std::vector<PanelElement> before, std::vector<PanelElement> after, std::string text, int mergeId)
        : m_(m), before_(std::move(before)), after_(std::move(after)), text_(std::move(text)), id_(mergeId) {}
    void redo() override { m_->setElements(after_); }
    void undo() override { m_->setElements(before_); }
    std::string text() const override { return text_; }
    int id() const override { return id_; }
    bool mergeWith(const jf::JUndoCommand* next) override {
        const auto* n = dynamic_cast<const ElementsSwap*>(next);
        if (!n) return false;
        after_ = n->after_;   // keep our before_, adopt the newer after_
        return true;
    }
private:
    PanelModel* m_;
    std::vector<PanelElement> before_, after_;
    std::string text_;
    int id_;
};
}  // namespace

// Apply an undo/redo step, then prune the selection to surviving element ids and refresh observers.
void Surface::doUndoRedo(bool redo) {
    if (!model_) return;
    // Run mode drives the live TUNE-edit history (Cache byte-diff stack: cell edits, smooth, linearise, …),
    // matching the run-mode Ctrl+Z path so the Edit▸Undo/Redo menu and the keyboard agree. Edit mode drives
    // the layout-authoring stack below.
    if (mode_ == Mode::Run) {
        if (jf::JUndoStack* us = Cache::instance().undoStack()) {
            if (redo) { if (us->canRedo()) us->redo(); } else if (us->canUndo()) us->undo();
        }
        invalidate();
        return;
    }
    if (redo) { if (undo_.canRedo()) undo_.redo(); } else { if (undo_.canUndo()) undo_.undo(); }
    std::vector<int> keep;
    for (int id : selection_) if (model_->get(id)) keep.push_back(id);
    selection_.swap(keep); selectionChanged.emit(); invalidate();
}

// Run fn as one undoable step: snapshot before, run, push a snapshot-swap command if it changed. NOTE: no
// commitInstances here. A widget-authored edit (the dock) flushes ITS widget to the model itself (right after
// setOwnProp) so the model is already current; a model-authored edit (a menu setProp inside fn) is current by
// construction. Committing the whole tree here would overwrite a model-authored change with the not-yet-
// rehydrated widget state — silently clobbering it. The whole-tree flush happens only at SAVE (commitAll).
void Surface::edit(const std::string& text, const std::function<void()>& fn) {
    if (!model_) { fn(); return; }
    std::vector<PanelElement> before = model_->elements();
    fn();
    commitEdit(text, before);
}
// ---- In-place label captions (edit mode) -----------------------------------------------------------
// Edit-mode input never reaches a widget (onControlInput is the run-mode path), so the Surface owns the
// trigger and the keyboard and parks the in-progress string in LabelWidget's edit map for render() to draw.
// The live resize writes straight to the model with NO history entry; commit then diffs against the
// snapshot taken at begin, so a whole typing session is one undo step covering both text and size.
void Surface::beginLabelEdit_(int id) {
    const PanelElement* e = model_ ? model_->get(id) : nullptr;
    if (!e) return;
    { CanvasWidget* w = instanceFor(*e); if (!w || !w->editsCaption()) return; }   // not a caption widget
    if (inlineEdit_) commitLabelEdit_();                 // only one caption open at a time
    inlineEdit_    = id;
    inlineBefore_  = model_->elements();
    inlineW0_      = e->w;
    inlineH0_      = e->h;
    auto& ed = LabelWidget::labelEdits()[e->uid];
    ed.core.setMultiline(true);                         // Shift-Enter breaks the line (see the key routing)
    ed.core.setText(e->prop("labelText"));
    ed.core.selectAll();                                // open with everything selected, so the first
    ed.active = true;                                   // keystroke replaces the caption (rename idiom)
    selectOnly(id);
    invalidate();
}

void Surface::fitLabelToText_() {
    if (!inlineEdit_ || !model_) return;
    const PanelElement* e = model_->get(inlineEdit_);
    if (!e) return;
    const auto it = LabelWidget::labelEdits().find(e->uid);
    if (it == LabelWidget::labelEdits().end()) return;
    // A WRAPPING caption keeps the width it was given. That width is the thing the text is being broken
    // against, so re-fitting the box to the text would move the very edge the wrap is measured from — the
    // box would chase the caption sideways across the page instead of the caption flowing inside the box.
    // It still grows DOWNWARD as the wrapped text needs more rows, because content is clipped to the rect
    // and the alternative is typing into a part of the label you cannot see.
    if (e->prop("wrap") == "1") {
        const float wh = LabelWidget::wrappedHeight(it->second.core.text(), e->prop("fontName"), e->w);
        model_->setRect(inlineEdit_, e->x, e->y, e->w, std::max(wh, inlineH0_));
        return;
    }
    float w = 0.f, h = 0.f;
    // Same font AND angle the caption renders with, so the box fits what is actually drawn.
    double rot = 0.0; try { rot = std::stod(e->prop("rotation")); } catch (...) {}
    LabelWidget::naturalSize(it->second.core.text(), e->prop("fontName"), rot, w, h);
    model_->setRect(inlineEdit_, e->x, e->y, w, std::max(h, inlineH0_));   // never shrink below the original height
}

void Surface::commitLabelEdit_() {
    if (!inlineEdit_) return;
    const int id = inlineEdit_;
    const PanelElement* ce = model_ ? model_->get(id) : nullptr;
    const std::string key = ce ? ce->uid : std::string();
    inlineEdit_ = 0;
    auto& map = LabelWidget::labelEdits();
    const auto it = map.find(key);
    if (it == map.end()) return;
    const std::string text = it->second.core.text();
    it->second.active = false;
    if (model_) {
        model_->setProp(id, "labelText", text);
        commitEdit("Edit label", inlineBefore_);          // one entry for the text + every resize step
    }
    // What the caption commit ACTUALLY wrote, and what the model holds a moment later — a caption reported
    // as reverting to "Label" is either not committing or being overwritten, and these two lines say which.
    JLOGC("surface", jf::JLogLevel::Info)
        << "[caption] commit id=" << id << " scope=" << (inScope() ? "panel/page" : "top")
        << " wrote=\"" << text << "\" model=\""
        << (model_ && model_->get(id) ? model_->get(id)->prop("labelText") : std::string("<no element>")) << "\"";
    inlineBefore_.clear();
    invalidate();
}

void Surface::cancelLabelEdit_() {
    if (!inlineEdit_) return;
    const int id = inlineEdit_;
    const PanelElement* ce = model_ ? model_->get(id) : nullptr;
    inlineEdit_ = 0;
    auto& map = LabelWidget::labelEdits();
    const auto it = map.find(ce ? ce->uid : std::string());
    if (it != map.end()) it->second.active = false;
    if (model_) if (const PanelElement* e = model_->get(id))   // undo the live resize; text was never written
        model_->setRect(id, e->x, e->y, inlineW0_, inlineH0_);
    inlineBefore_.clear();
    invalidate();
}

void Surface::commitEdit(const std::string& text, const std::vector<PanelElement>& before, int mergeId) {
    if (!model_) return;
    std::vector<PanelElement> after = model_->elements();
    if (elementsEqual(before, after)) return;                 // no-op edit → no history entry
    undo_.push(new ElementsSwap(model_, before, std::move(after), text, mergeId));
    if (onModified) onModified();                             // mark the document dirty (save-changes prompt)
    if (inScope() && onPageChanged_) onPageChanged_();        // an in-place node-page edit → persist the store
}

namespace {
class CanvasSwap : public jf::JUndoCommand {
public:
    CanvasSwap(PanelModel* m, PanelModel::CanvasState b, PanelModel::CanvasState a, std::string t, int id)
        : m_(m), before_(std::move(b)), after_(std::move(a)), text_(std::move(t)), id_(id) {}
    void redo() override { m_->setCanvasState(after_); }
    void undo() override { m_->setCanvasState(before_); }
    std::string text() const override { return text_; }
    int id() const override { return id_; }
    bool mergeWith(const jf::JUndoCommand* next) override {
        const auto* n = dynamic_cast<const CanvasSwap*>(next); if (!n) return false; after_ = n->after_; return true;
    }
private:
    PanelModel* m_; PanelModel::CanvasState before_, after_; std::string text_; int id_;
};
}  // namespace

void Surface::applyCanvasEdit(const std::string& text, const PanelModel::CanvasState& before, int mergeId) {
    if (!model_) return;
    PanelModel::CanvasState after = model_->canvasState();
    if (before == after) return;
    undo_.push(new CanvasSwap(model_, before, std::move(after), text, mergeId));
    if (onModified) onModified();                             // canvas edits dirty the document too (save prompt)
    if (inScope() && onPageChanged_) onPageChanged_();        // an in-place node-page canvas edit → mark dirty
}

void Surface::buildContextMenu() {
    using jf::JMenu;
    // Add control ▸ (the catalog). NOTE: the studio also offers "Panel (container)" and, on a panel,
    // a layout/title/help/edit-in-place submenu — deferred until nested panels are ported.
    addMenu_ = std::make_unique<JMenu>("Add control");
    for (const std::string& type : widgetTypes()) {
        const std::string title = widgetTitle(type);
        addMenu_->add(m_graph, title.empty() ? type : title)->onTriggered.connect([this, type] { edit("Add", [this, type]{ addWidgetAt(type); }); });
    }
    menu_.add(m_graph, "Add control", {}, addMenu_.get());
    menu_.addSeparator(m_graph);

    // Universal clipboard ops stay at the root — muscle memory, one click away.
    miCut_    = menu_.add(m_graph, "Cut");    miCut_->onTriggered.connect([this]{ edit("Cut", [this]{ cutSelection(); }); });
    miCopy_   = menu_.add(m_graph, "Copy");   miCopy_->onTriggered.connect([this]{ copySelection(); });
    miPaste_  = menu_.add(m_graph, "Paste");  miPaste_->onTriggered.connect([this]{ edit("Paste", [this]{ pasteClipboard(); }); });
    miDelete_ = menu_.add(m_graph, "Delete"); miDelete_->onTriggered.connect([this]{ menuDelete(); });
    menu_.addSeparator(m_graph);

    // Arrange ▸ — everything spatial: stacking order, grouping, align/distribute/size (each a sub-group).
    arrangeMenu_ = std::make_unique<JMenu>("Arrange");
    miFront_ = arrangeMenu_->add(m_graph, "Bring to Front"); miFront_->onTriggered.connect([this]{ edit("Bring to Front", [this]{ if (model_ && !selection_.empty()) { model_->bringToFront(selection_); invalidate(); } }); });
    miBack_  = arrangeMenu_->add(m_graph, "Send to Back");   miBack_->onTriggered.connect([this]{ edit("Send to Back",   [this]{ if (model_ && !selection_.empty()) { model_->sendToBack(selection_);  invalidate(); } }); });
    // Single-layer steps. PanelModel has had them since the z-order work — past exactly one unselected
    // neighbour, a contiguous block moving together — with nothing in the UI to reach them, so the only
    // stacking control was all-the-way-front and all-the-way-back.
    miFwd_   = arrangeMenu_->add(m_graph, "Bring Forward"); miFwd_->onTriggered.connect([this]{ edit("Bring Forward", [this]{ if (model_ && !selection_.empty()) { model_->moveForward(selection_);  invalidate(); } }); });
    miBwd_   = arrangeMenu_->add(m_graph, "Send Backward"); miBwd_->onTriggered.connect([this]{ edit("Send Backward", [this]{ if (model_ && !selection_.empty()) { model_->moveBackward(selection_); invalidate(); } }); });
    arrangeMenu_->addSeparator(m_graph);
    miGroup_   = arrangeMenu_->add(m_graph, "Group");   miGroup_->onTriggered.connect([this]{ edit("Group",   [this]{ groupSelection(); }); });
    miUngroup_ = arrangeMenu_->add(m_graph, "Ungroup"); miUngroup_->onTriggered.connect([this]{ edit("Ungroup", [this]{ ungroupSelection(); }); });
    arrangeMenu_->addSeparator(m_graph);
    alignMenu_ = std::make_unique<JMenu>("Align");
    struct { const char* label; AlignMode mode; } aitems[] = {
        {"Left", AlignMode::Left}, {"Right", AlignMode::Right}, {"Top", AlignMode::Top}, {"Bottom", AlignMode::Bottom},
        {"Centre Horizontally", AlignMode::CenterH}, {"Centre Vertically", AlignMode::CenterV}, {"Centre", AlignMode::Center} };
    for (auto& it : aitems) { AlignMode m = it.mode; alignMenu_->add(m_graph, it.label)->onTriggered.connect([this, m]{ edit("Align", [this, m]{ alignSelection(m); }); }); }
    miAlign_ = arrangeMenu_->add(m_graph, "Align", {}, alignMenu_.get());
    distMenu_ = std::make_unique<JMenu>("Distribute");
    distMenu_->add(m_graph, "Horizontally")->onTriggered.connect([this]{ edit("Distribute", [this]{ distributeSelection(true); }); });
    distMenu_->add(m_graph, "Vertically")->onTriggered.connect([this]{ edit("Distribute", [this]{ distributeSelection(false); }); });
    miDist_ = arrangeMenu_->add(m_graph, "Distribute", {}, distMenu_.get());
    // Set Spacing — Distribute spreads across the existing extent; this lays the selection out at an
    // EXPLICIT step you type. Pitch is origin-to-origin, Gap is edge-to-edge.
    spacingMenu_ = std::make_unique<JMenu>("Set Spacing");
    spacingMenu_->add(m_graph, "Vertical Pitch\xE2\x80\xA6")->onTriggered.connect([this]{ armSpacing(false, true);  });
    spacingMenu_->add(m_graph, "Vertical Gap\xE2\x80\xA6")->onTriggered.connect([this]{ armSpacing(false, false); });
    spacingMenu_->addSeparator(m_graph);
    spacingMenu_->add(m_graph, "Horizontal Pitch\xE2\x80\xA6")->onTriggered.connect([this]{ armSpacing(true, true);  });
    spacingMenu_->add(m_graph, "Horizontal Gap\xE2\x80\xA6")->onTriggered.connect([this]{ armSpacing(true, false); });
    miSpacing_ = arrangeMenu_->add(m_graph, "Set Spacing", {}, spacingMenu_.get());
    sizeMenu_ = std::make_unique<JMenu>("Make Same Size");
    sizeMenu_->add(m_graph, "Width")->onTriggered.connect([this]{ edit("Make Same Size", [this]{ matchSize(SizeMode::Width); }); });
    sizeMenu_->add(m_graph, "Height")->onTriggered.connect([this]{ edit("Make Same Size", [this]{ matchSize(SizeMode::Height); }); });
    sizeMenu_->add(m_graph, "Both")->onTriggered.connect([this]{ edit("Make Same Size", [this]{ matchSize(SizeMode::Both); }); });
    miSize_ = arrangeMenu_->add(m_graph, "Make Same Size", {}, sizeMenu_.get());
    miArrange_ = menu_.add(m_graph, "Arrange", {}, arrangeMenu_.get());
    menu_.addSeparator(m_graph);

    // Edit ▸ — per-control authoring editors. Each opens an app-wired modal seeded with the selected control's
    // current prop; the apply callback writes it back as ONE undo step. Condition/Enable/Ranges work on any
    // single control; the typed items are enabled only for their widget kind (refreshMenuState).
    editMenu_ = std::make_unique<JMenu>("Edit");
    // "Visibility Condition…", not "Condition…": it sits directly above "Enable Condition…" and the two do
    // different things -- this one HIDES the control, that one greys it and keeps its space. A bare
    // "Condition" beside a named one reads as the general case of it, which is exactly backwards.
    miCondition_ = editMenu_->add(m_graph, "Visibility Condition…");
    miCondition_->onTriggered.connect([this]{ openPropEditor(conditionHook(), "condition", "Visibility Condition"); });
    miEnable_ = editMenu_->add(m_graph, "Enable Condition…");   // false → greyed + inert (visibility keeps layout)
    miEnable_->onTriggered.connect([this]{ openPropEditor(conditionHook(), "enableCondition", "Enable Condition"); });
    editMenu_->addSeparator(m_graph);
    miPresets_ = editMenu_->add(m_graph, "Edit Presets…");
    miPresets_->onTriggered.connect([this]{ openPropEditor(onEditPresets, "presets", "Edit Presets"); });
    miLines_ = editMenu_->add(m_graph, "Edit Lines…");
    miLines_->onTriggered.connect([this]{ openPropEditor(onEditLines, "lines", "Edit Lines"); });
    editMenu_->addSeparator(m_graph);
    // Panel Layout ▸ — pick the managed layout mode for a single selected panel (the same 7 modes the canvas
    // and PanelWidget share). Writing "layoutMode" re-lays out its children on the next frame; one undo step.
    panelLayoutMenu_ = std::make_unique<JMenu>("Panel Layout");
    static const char* const kLayouts[8] = { "Free", "Y Axis", "X Axis", "Border", "Card", "Index Card", "Grid", "Wrap" };
    for (int i = 0; i < 8; ++i) {
        panelLayoutItems_[i] = panelLayoutMenu_->add(m_graph, kLayouts[i]);   // checkable: ● the panel's current layout mode
        panelLayoutItems_[i]->setCheckable(true);
        panelLayoutItems_[i]->onTriggered.connect([this, i]{
            if (!model_ || selection_.size() != 1) return;
            const int id = selection_.front();
            edit("Panel Layout", [this, id, i]{ model_->setProp(id, "layoutMode", std::to_string(i)); });
            invalidate();
        });
    }
    miPanel_ = editMenu_->add(m_graph, "Panel Contents…");
    miPanel_->onTriggered.connect([this]{ openPropEditor(onEditPanel, "children", "Panel Contents"); });
    miPanelLayout_ = editMenu_->add(m_graph, "Panel Layout", {}, panelLayoutMenu_.get());
    miPanelEdit_ = editMenu_->add(m_graph, "Edit Panel in Place");   // drill in and lay children out on-canvas
    miPanelEdit_->onTriggered.connect([this]{ if (selection_.size() == 1) enterPanel(selection_[0]); });
    miEdit_ = menu_.add(m_graph, "Edit", {}, editMenu_.get());

    // A selected table/curve gets its FULL run-mode menu here as a submenu — the same ops, targeting the selected control (prepareContextMenu points them at it).
    miTableSub_ = menu_.add(m_graph, "Table", {}, &tableMenu_);
    miCurveSub_ = menu_.add(m_graph, "Curve", {}, &curveMenu_);
}

void Surface::buildTableMenu() {
    using jf::JMenu;
    // The run-mode table menu, in five task groups so
    // the root stays short: Edit Values / Rows & Columns / Clipboard / Display / Setup. Every op is one hop
    // deep. Value transforms arm an inline entry over the block: the operand is typed
    // in the active cell, then applied across the selection on Enter.

    // --- Edit Values ▸ — value transforms + fill (the daily cell edits) ---
    editValuesMenu_ = std::make_unique<JMenu>("Edit Values");
    editValuesMenu_->add(m_graph, "Set to…")->onTriggered.connect([this] { tableArmOp(1); });
    editValuesMenu_->add(m_graph, "Increase by…")->onTriggered.connect([this] { tableArmOp(2); });
    editValuesMenu_->add(m_graph, "Decrease by…")->onTriggered.connect([this] { tableArmOp(3); });
    editValuesMenu_->add(m_graph, "Percentage change…")->onTriggered.connect([this] { tableArmOp(4); });
    editValuesMenu_->addSeparator(m_graph);
    linMenu_ = std::make_unique<JMenu>("Linearise");
    linMenu_->add(m_graph, "Selected Cells")->onTriggered.connect([this] { tableLinearise(true, true); });
    linMenu_->add(m_graph, "Horizontal")->onTriggered.connect([this] { tableLinearise(true, false); });
    linMenu_->add(m_graph, "Vertical")->onTriggered.connect([this] { tableLinearise(false, true); });
    editValuesMenu_->add(m_graph, "Linearise", {}, linMenu_.get());
    editValuesMenu_->add(m_graph, "Smooth")->onTriggered.connect([this] { tableSmooth(); });
    // Offered for ANY table whose schema names a base map (`apply_to`), not for lambda_ltft by name —
    // the relationship is data, so a second learned surface gets the operation for free.
    editValuesMenu_->addSeparator(m_graph);
    miApplyToBase_ = editValuesMenu_->add(m_graph, "Apply to Base Table…");
    miApplyToBase_->onTriggered.connect([this] { tableApplyToBase(); });
    miResetLearned_ = editValuesMenu_->add(m_graph, "Reset Learned Values…");
    miResetLearned_->onTriggered.connect([this] { tableResetToZero(); });
    tableMenu_.add(m_graph, "Edit Values", {}, editValuesMenu_.get());

    // --- Rows & Columns ▸ — grid structure + breakpoint spacing ---
    rowsColsMenu_ = std::make_unique<JMenu>("Rows & Columns");
    rowsColsMenu_->add(m_graph, "Insert Row")->onTriggered.connect([this] { tableBinOp(true, false); });
    rowsColsMenu_->add(m_graph, "Delete Row")->onTriggered.connect([this] { tableBinOp(true, true); });
    rowsColsMenu_->add(m_graph, "Insert Column")->onTriggered.connect([this] { tableBinOp(false, false); });
    rowsColsMenu_->add(m_graph, "Delete Column")->onTriggered.connect([this] { tableBinOp(false, true); });
    rowsColsMenu_->addSeparator(m_graph);
    distBinsMenu_ = std::make_unique<JMenu>("Distribute Bins");   // warp an axis's breakpoints (even / cluster)
    distBinsMenu_->add(m_graph, "X: Even")->onTriggered.connect([this] { tableDistributeBins(0, 0); });
    distBinsMenu_->add(m_graph, "X: Cluster Centre")->onTriggered.connect([this] { tableDistributeBins(0, 1); });
    distBinsMenu_->add(m_graph, "X: Cluster Ends")->onTriggered.connect([this] { tableDistributeBins(0, 2); });
    distBinsMenu_->addSeparator(m_graph);
    distBinsMenu_->add(m_graph, "Y: Even")->onTriggered.connect([this] { tableDistributeBins(1, 0); });
    distBinsMenu_->add(m_graph, "Y: Cluster Centre")->onTriggered.connect([this] { tableDistributeBins(1, 1); });
    distBinsMenu_->add(m_graph, "Y: Cluster Ends")->onTriggered.connect([this] { tableDistributeBins(1, 2); });
    rowsColsMenu_->add(m_graph, "Distribute Bins", {}, distBinsMenu_.get());
    tableMenu_.add(m_graph, "Rows & Columns", {}, rowsColsMenu_.get());

    // --- Clipboard ▸ — cell block vs whole table ---
    clipboardMenu_ = std::make_unique<JMenu>("Clipboard");
    clipboardMenu_->add(m_graph, "Copy Cells")->onTriggered.connect([this] { tableCopyCells(); });
    clipboardMenu_->add(m_graph, "Paste Cells")->onTriggered.connect([this] { tablePasteCells(); });
    clipboardMenu_->addSeparator(m_graph);
    clipboardMenu_->add(m_graph, "Copy Entire Table")->onTriggered.connect([this] { tableCopyTable(); });
    clipboardMenu_->add(m_graph, "Paste Entire Table")->onTriggered.connect([this] { tablePasteTable(); });
    tableMenu_.add(m_graph, "Clipboard", {}, clipboardMenu_.get());
    tableMenu_.addSeparator(m_graph);

    // --- Display ▸ — how the SAME data is shown (no data change) ---
    displayMenu_ = std::make_unique<JMenu>("Display");
    viewMenu_ = std::make_unique<JMenu>("View");
    static const char* const kViews[3] = { "2D Grid", "3D Surface", "Slice" };
    for (int i = 0; i < 3; ++i) {
        viewItems_[i] = viewMenu_->add(m_graph, kViews[i]);   // checkable: ● the table's current view (synced on open)
        viewItems_[i]->setCheckable(true);
        viewItems_[i]->onTriggered.connect([this, i] { tableSetView(i); });
    }
    displayMenu_->add(m_graph, "View", {}, viewMenu_.get());
    orientMenu_ = std::make_unique<JMenu>("Orientation");   // AxisLayout combos 0..7 (the four transposed first)
    static const char* const kOrient[8] = { "Transpose left & top", "Transpose right & top", "Transpose left & bottom",
        "Transpose right & bottom", "Left & top", "Right & top", "Left & bottom", "Right & bottom" };
    for (int i = 0; i < 8; ++i) {
        orientItems_[i] = orientMenu_->add(m_graph, kOrient[i]);   // checkable: ● the table's current orientation
        orientItems_[i]->setCheckable(true);
        orientItems_[i]->onTriggered.connect([this, i] { tableSetOrientation(i); });
    }
    displayMenu_->add(m_graph, "Orientation", {}, orientMenu_.get());
    planeMenu_ = std::make_unique<JMenu>("Plane (Z)");   // 3D tables: navigate + copy/interpolate depth planes
    planeMenu_->add(m_graph, "Next Plane")->onTriggered.connect([this] { tablePlaneStep(+1); });
    planeMenu_->add(m_graph, "Previous Plane")->onTriggered.connect([this] { tablePlaneStep(-1); });
    planeMenu_->addSeparator(m_graph);
    planeMenu_->add(m_graph, "Copy Plane to All")->onTriggered.connect([this] { tableCopyPlaneToAll(); });
    planeCopyToMenu_ = std::make_unique<JMenu>("Copy this plane to…");   // items rebuilt per open (plane count varies)
    planeMenu_->add(m_graph, "Copy this plane to…", {}, planeCopyToMenu_.get());
    planeMenu_->add(m_graph, "Interpolate Planes")->onTriggered.connect([this] { tableInterpolatePlanes(); });
    displayMenu_->add(m_graph, "Plane (Z)", {}, planeMenu_.get());
    displayMenu_->addSeparator(m_graph);
    displayMenu_->add(m_graph, "Increase Decimal Places")->onTriggered.connect([this] { tableBumpDecimals(+1); });
    displayMenu_->add(m_graph, "Decrease Decimal Places")->onTriggered.connect([this] { tableBumpDecimals(-1); });
    miCellTrace_ = displayMenu_->add(m_graph, "Cell Trace");   // live-telemetry cursor (checkable; synced on open)
    miCellTrace_->setCheckable(true);
    miCellTrace_->onTriggered.connect([this] { tableToggleCellTrace(); });
    tableMenu_.add(m_graph, "Display", {}, displayMenu_.get());

    // --- Setup ▸ — axis wiring, key bindings, and restore-to-baseline ---
    setupMenu_ = std::make_unique<JMenu>("Setup");
    setupMenu_->add(m_graph, "Table Axis Setup…")->onTriggered.connect([this] {
        const PanelElement* el = opTableElement();
        if (el && onAxisSetup) onAxisSetup(opBind_(*el), axisUnitsOf_(), tableProp_("displayUnit"), unitSetter_(el->id));
    });
    setupMenu_->add(m_graph, "Key Bindings…")->onTriggered.connect([] { if (onEditTableKeyBindings) onEditTableKeyBindings(); });
    setupMenu_->addSeparator(m_graph);
    // A CALIBRATION IS NOT ABOUT THIS ENGINE. A sensor's transfer curve is the sensor's own data sheet —
    // the same part is the same curve in every car it is fitted to — so it is worth typing once and
    // keeping. Saving and loading works for any table; a curve is just the case that earns it.
    setupMenu_->add(m_graph, "Save to File…")->onTriggered.connect([this] { tableToFile(true); });
    setupMenu_->add(m_graph, "Load from File…")->onTriggered.connect([this] { tableToFile(false); });
    if (!presetMenu_) presetMenu_ = std::make_unique<jf::JMenu>("Apply Preset");
    setupMenu_->add(m_graph, "Apply Preset", {}, presetMenu_.get());
    setupMenu_->addSeparator(m_graph);
    setupMenu_->add(m_graph, "Restore to Connect Point")->onTriggered.connect([this] { tableRestore(true); });
    setupMenu_->add(m_graph, "Restore Defaults")->onTriggered.connect([this] { tableRestore(false); });
    tableMenu_.add(m_graph, "Setup", {}, setupMenu_.get());
}

int Surface::tableElAt(float mx, float my) const {
    opModel_ = model_; opViewportId_ = 0; tableTargetUid_.clear(); tablePanelId_ = 0;
    if (mode_ != Mode::Run || !model_) return 0;
    const int hit = hitTest(mx, my, xform());
    if (!hit) return 0;
    const auto& els = model_->elements();
    const PanelElement* el = model_->get(hit);
    if (el && el->type == "table" && cache_.isTable(bindOf_(*el, scopeCtx_))) { tableTargetUid_ = el->uid; return hit; }
    if (el && el->type == "viewport") {   // descend into the viewport to reach a mirrored table
        int idx = -1; for (size_t i = 0; i < els.size(); ++i) if (els[i].id == hit) { idx = static_cast<int>(i); break; }
        if (idx >= 0)
            if (auto* vp = dynamic_cast<ViewportWidget*>(const_cast<Surface*>(this)->widgetInstance(*el))) {
                PanelModel* pm = nullptr;
                jf::JRect mrect{};
                const int mid = vp->mirroredElementAt(screenRectOf(idx, xform()), mx, my, pm, &mrect);
                if (mid && pm) if (const PanelElement* me = pm->get(mid)) {
                    if (me->type == "table" && cache_.isTable(bindOf_(*me, el->prop("signalName")))) { opModel_ = pm; opViewportId_ = hit; tableTargetUid_ = me->uid; return mid; }
                    // …and through a PANEL to its children: an imported dialog puts every control inside one,
                    // so stopping at the top-level element found the dialog and never the table in it.
                    if (me->type == "panel")
                        if (const PanelElement* ce = _panelChildAt(vp->mirroredWidget(mid), mrect, mx, my, "table", el->prop("signalName")))
                            { opModel_ = pm; opViewportId_ = hit; tableTargetUid_ = ce->uid;
                              tablePanelId_ = me->id; return ce->id; }
                }
            }
    }
    if (el && el->type == "panel")   // a panel drawn straight on the surface (no viewport in between)
        if (const PanelElement* ce = _panelChildAt(const_cast<Surface*>(this)->widgetInstance(*el),
                                                   screenRectOf(_indexOf(hit), xform()), mx, my, "table", scopeCtx_))
            { tableTargetUid_ = ce->uid; tablePanelId_ = hit; return ce->id; }
    return 0;
}

// The child of `panelEl` under the cursor whose type matches and whose binding is a real table/curve.
// Resolution goes through the live PanelWidget, since a panel's children exist only as its own data.
const PanelElement* Surface::_panelChildAt(CanvasWidget* panelWidget, const jf::JRect& rect,
                                           float mx, float my, const char* wantType, const std::string& ctx) const {
    auto* pw = dynamic_cast<PanelWidget*>(panelWidget);
    if (!pw) return nullptr;
    const PanelElement* ce = pw->childElementAt(rect, mx, my);
    if (!ce || (wantType && *wantType && ce->type != wantType)) return nullptr;
    return cache_.isTable(bindOf_(*ce, ctx)) ? ce : nullptr;
}

// The uid of the table the context-menu ops target. Per-widget state is keyed by uid (an element id is
// unique only within one model), so every op that reaches for a cursor, a plane or a selection asks here.
// The uid of the caption currently open for in-place editing ("" when none) — the caption state is
// global and keyed by uid, while inlineEdit_ is an id in THIS surface's model.
std::string Surface::inlineEditUid_() const {
    const PanelElement* e = (inlineEdit_ && model_) ? model_->get(inlineEdit_) : nullptr;
    return e ? e->uid : std::string();
}

std::string Surface::opTableUid_() const {
    const PanelElement* e = opTableElement();
    return e ? e->uid : std::string();
}

const PanelElement* Surface::opTableElement() const {
    if (!tableTargetUid_.empty())
        if (CanvasWidget* w = const_cast<Surface*>(this)->widgetByUid(tableTargetUid_))
            if (const PanelElement* e = w->element()) return e;
    // No uid to go by (an element that predates uids): fall back to the id-in-model lookup.
    return (tableElUnder_ && opModel()) ? opModel()->get(tableElUnder_) : nullptr;
}

const PanelElement* Surface::opCurveElement() const {
    if (!curveTargetUid_.empty())
        if (CanvasWidget* w = const_cast<Surface*>(this)->widgetByUid(curveTargetUid_))
            if (const PanelElement* e = w->element()) return e;
    // No uid to go by (an element that predates uids): fall back to the id-in-model lookup.
    return (curveElUnder_ && opModel()) ? opModel()->get(curveElUnder_) : nullptr;
}

int Surface::_indexOf(int id) const {
    if (!model_) return -1;
    const auto& els = model_->elements();
    for (size_t i = 0; i < els.size(); ++i) if (els[i].id == id) return static_cast<int>(i);
    return -1;
}

int Surface::curveElAt(float mx, float my) const {
    opModel_ = model_; opViewportId_ = 0; curveTargetUid_.clear();
    if (mode_ != Mode::Run || !model_) return 0;
    const int hit = hitTest(mx, my, xform());
    if (!hit) return 0;
    const auto& els = model_->elements();
    const PanelElement* el = model_->get(hit);
    if (el && el->type == "curve" && cache_.isTable(bindOf_(*el, scopeCtx_))) { curveTargetUid_ = el->uid; return hit; }
    if (el && el->type == "viewport") {   // descend into the viewport to reach a mirrored curve
        int idx = -1; for (size_t i = 0; i < els.size(); ++i) if (els[i].id == hit) { idx = static_cast<int>(i); break; }
        if (idx >= 0)
            if (auto* vp = dynamic_cast<ViewportWidget*>(const_cast<Surface*>(this)->widgetInstance(*el))) {
                PanelModel* pm = nullptr;
                jf::JRect mrect{};
                const int mid = vp->mirroredElementAt(screenRectOf(idx, xform()), mx, my, pm, &mrect);
                if (mid && pm) if (const PanelElement* me = pm->get(mid)) {
                    if (me->type == "curve" && cache_.isTable(bindOf_(*me, el->prop("signalName")))) { opModel_ = pm; opViewportId_ = hit; curveTargetUid_ = me->uid; return mid; }
                    if (me->type == "panel")   // …and into the dialog panel an imported curve lives in
                        if (const PanelElement* ce = _panelChildAt(vp->mirroredWidget(mid), mrect, mx, my, "curve", el->prop("signalName")))
                            { opModel_ = pm; opViewportId_ = hit; curveTargetUid_ = ce->uid; return ce->id; }
                }
            }
    }
    if (el && el->type == "panel")
        if (const PanelElement* ce = _panelChildAt(const_cast<Surface*>(this)->widgetInstance(*el),
                                                   screenRectOf(_indexOf(hit), xform()), mx, my, "curve", scopeCtx_))
            { curveTargetUid_ = ce->uid; return ce->id; }
    return 0;
}

// The multi-channel control under the cursor — a watch list or a trace view. Same descent as curveElAt
// (straight hit, into a viewport's mirrored page, into a panel's children) minus the isTable filter: these
// controls read telemetry, not config, so there is no table to resolve. `wantType` picks which of the two.
int Surface::chanElAt(float mx, float my, const char* wantType) const {
    opModel_ = model_; opViewportId_ = 0; chanTargetUid_.clear();
    if (mode_ != Mode::Run || !model_) return 0;
    const int hit = hitTest(mx, my, xform());
    if (!hit) return 0;
    const auto& els = model_->elements();
    const PanelElement* el = model_->get(hit);
    if (!el) return 0;
    const bool anyType = (!wantType || !*wantType);
    // A CONTAINER IS NEVER THE ANSWER. With a wantType this never came up — "viewport"/"panel" is not a
    // channel control, so the walk always fell through to the descent below. With ANY type accepted it
    // matched the frame the cursor was over and returned that, so a right-click anywhere on a mirrored
    // page resolved to the viewport instead of the control inside it: its binding is a page path, the
    // unit lookup came back empty, and the Units menu declined to appear. Descend first; the container
    // is a frame around the thing being asked about, not the thing.
    const auto container = [](const std::string& t) { return t == "viewport" || t == "panel"; };
    if (anyType ? !container(el->type) : el->type == wantType) { chanTargetUid_ = el->uid; return hit; }
    auto childOfType = [&](CanvasWidget* pw, const jf::JRect& rect) -> const PanelElement* {
        auto* p = dynamic_cast<PanelWidget*>(pw);
        if (!p) return nullptr;
        const PanelElement* ce = p->childElementAt(rect, mx, my);
        return (ce && (anyType || ce->type == wantType)) ? ce : nullptr;
    };
    if (el->type == "viewport") {
        int idx = -1; for (size_t i = 0; i < els.size(); ++i) if (els[i].id == hit) { idx = static_cast<int>(i); break; }
        if (idx >= 0)
            if (auto* vp = dynamic_cast<ViewportWidget*>(const_cast<Surface*>(this)->widgetInstance(*el))) {
                PanelModel* pm = nullptr;
                jf::JRect mrect{};
                const int mid = vp->mirroredElementAt(screenRectOf(idx, xform()), mx, my, pm, &mrect);
                if (mid && pm) if (const PanelElement* me = pm->get(mid)) {
                    if (anyType ? !container(me->type) : me->type == wantType)
                        { opModel_ = pm; opViewportId_ = hit; chanTargetUid_ = me->uid; return mid; }
                    if (me->type == "panel")
                        if (const PanelElement* ce = childOfType(vp->mirroredWidget(mid), mrect))
                            { opModel_ = pm; opViewportId_ = hit; chanTargetUid_ = ce->uid; return ce->id; }
                }
            }
    }
    if (el->type == "panel")
        if (const PanelElement* ce = childOfType(const_cast<Surface*>(this)->widgetInstance(*el),
                                                 screenRectOf(_indexOf(hit), xform())))
            { chanTargetUid_ = ce->uid; return ce->id; }
    return 0;
}

const PanelElement* Surface::opChanElement() const {
    if (!chanTargetUid_.empty())
        if (CanvasWidget* w = const_cast<Surface*>(this)->widgetByUid(chanTargetUid_))
            if (const PanelElement* e = w->element()) return e;
    return (chanElUnder_ && opModel()) ? opModel()->get(chanElUnder_) : nullptr;
}

// THE CHANNEL SET IS THE OPERATOR'S. Both of these controls show a list of channels the user picks while
// tuning, so both get the same two-list picker off their own context menu — "Channel Setup" on a watch
// list, "Select Channels" on a trace view, the wording each screen already uses for it.
void Surface::openChannelPicker() {
    const PanelElement* el = opChanElement();
    if (!el || !enumpick::channels()) return;
    const bool graph = (el->type == "livegraph");
    std::vector<std::string> cur;
    LineGraphModel lm;
    if (graph) {
        lm = LineGraphModel::fromCompact(el->prop("lines"));
        for (const auto& l : lm.lines) cur.push_back(l.channel);
        if (cur.empty() && !el->prop("signalName").empty()) cur.push_back(el->prop("signalName"));
    } else {
        cur = ChannelListWidget::parse(el->prop("channels"));
    }
    const std::string uid = el->uid;
    enumpick::channels()(graph ? "Select Channels" : "Channel Setup", std::move(cur),
                         [this, uid, graph, lm](std::vector<std::string> picked) mutable {
        CanvasWidget* w = widgetByUid(uid);
        if (!w) return;
        if (graph) {
            // Keep each surviving line's RANGE and its hidden flag: re-picking the list is not a request to
            // throw away the scales someone set, and a line that was hidden should not come back drawn.
            LineGraphModel out;
            for (const std::string& ch : picked) {
                LineGraphModel::Line l; l.channel = ch;
                for (const auto& o : lm.lines) if (o.channel == ch) { l = o; break; }
                out.lines.push_back(std::move(l));
            }
            w->setOwnProp("lines", out.toCompact());
        } else {
            w->setOwnProp("channels", ChannelListWidget::join(picked));
        }
        if (Surface::onModified) Surface::onModified();
        invalidate();
    });
}

// A trace view can be taken off the page from its own menu — it is the operator's control, added and
// removed while tuning, and reaching for edit mode to delete one is the long way round.
void Surface::removeChanElement() {
    const PanelElement* el = opChanElement();
    PanelModel* pm = opModel();
    if (!el || !pm) return;
    const int id = el->id;
    chanTargetUid_.clear(); chanElUnder_ = 0;
    pm->remove(id);
    if (Surface::onModified) Surface::onModified();
    selectionChanged.emit();
    invalidate();
}

// The channels a multi-channel control is currently showing, in its own order.
std::vector<std::string> Surface::chanChannelsOf_(const PanelElement& el) const {
    if (el.type == "livegraph") {
        std::vector<std::string> out;
        for (const auto& l : LineGraphModel::fromCompact(el.prop("lines")).lines) out.push_back(l.channel);
        if (out.empty() && !el.prop("signalName").empty()) out.push_back(el.prop("signalName"));
        return out;
    }
    return ChannelListWidget::parse(el.prop("channels"));
}

void Surface::openChannelProps() {
    const PanelElement* el = opChanElement();
    if (!el || !enumpick::channelProps()) return;
    std::vector<std::string> chans = chanChannelsOf_(*el);
    if (chans.empty()) return;
    enumpick::channelProps()("Channel Properties", std::move(chans), [this] {
        if (Surface::onModified) Surface::onModified();   // bands live in the document, so it is now dirty
        invalidate();
    });
}

void Surface::buildChannelMenus() {
    chanListMenu_.add(m_graph, "Channel Setup…")->onTriggered.connect([this] { openChannelPicker(); });
    graphMenu_.add(m_graph, "Select Channels…")->onTriggered.connect([this] { openChannelPicker(); });
    graphMenu_.addSeparator(m_graph);
    // "Properties…" on a live control asks about the CHANNELS it is showing — their unit, their scale and
    // the bands that make them amber — not about the widget's geometry. The widget's own properties are
    // the dock's job, and the dock is an edit-mode tool; this is the run-mode question.
    graphMenu_.add(m_graph, "Properties…")->onTriggered.connect([this] { openChannelProps(); });
    chanListMenu_.add(m_graph, "Properties…")->onTriggered.connect([this] { openChannelProps(); });
    graphMenu_.add(m_graph, "Remove Trace View from Page")->onTriggered.connect([this] { removeChanElement(); });
}

// Curve run-mode context menu — Insert/Delete Point + Linearise (all via the shared ti* ops on the curve's
// 1D table) + Axis Setup (reuses the table Axis Setup dialog, since a curve IS a 1D table). Mirrors
// CurveEditor::contextMenuEvent.
void Surface::buildCurveMenu() {
    curveMenu_.add(m_graph, "Insert Point")->onTriggered.connect([this] { curveInsertPoint(); });
    curveMenu_.add(m_graph, "Delete Point")->onTriggered.connect([this] { curveDeletePoint(); });
    curveMenu_.add(m_graph, "Linearise")->onTriggered.connect([this] { curveLinearise(); });
    curveMenu_.addSeparator(m_graph);
    curveMenu_.add(m_graph, "Save to File…")->onTriggered.connect([this] { tableToFile(true); });
    curveMenu_.add(m_graph, "Load from File…")->onTriggered.connect([this] { tableToFile(false); });
    if (!presetMenu_) presetMenu_ = std::make_unique<jf::JMenu>("Apply Preset");
    curveMenu_.add(m_graph, "Apply Preset", {}, presetMenu_.get());
    curveMenu_.addSeparator(m_graph);
    curveMenu_.add(m_graph, "Axis Setup…")->onTriggered.connect([this] {
        const PanelElement* el = opCurveElement();
        if (el && onAxisSetup) onAxisSetup(opBind_(*el), axisUnitsOf_(), tableProp_("displayUnit"), unitSetter_(el->id));
    });
}

// WHERE THE PRESET LIBRARY LIVES. Beside the ECU folders rather than inside one: a sensor's transfer
// curve is the sensor's data sheet, not this engine's tune, so it belongs to the installation and not
// to any particular ECU.
std::string Surface::calibrationDir() {
    return StudioPaths::dataDir("calibrations");
}

// The presets that ship with the studio: beside the executable in an installed build, or the source
// tree's apps/studio-jf/calibrations when run from the build directory. Empty if neither exists.
std::string Surface::shippedCalibrationDir() {
    namespace fs = std::filesystem;
    const fs::path exe = resources::exeDir();
    std::error_code ec;
    for (const fs::path& d : { exe / "calibrations", exe / ".." / "calibrations" })
        if (fs::is_directory(d, ec)) return fs::weakly_canonical(d, ec).string();
    return {};
}

// The unit a saved curve says its VALUES are in, from the provenance line tablefile writes
// ("# value: Reading [C]"). Empty when the file does not say — an older file, or one a spreadsheet
// stripped the comments from — and an unknown unit is offered rather than hidden, because refusing to
// show a file the user put there deliberately is worse than showing one that does not fit.
static std::string presetValueUnit(const std::string& file) {
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("# value:", 0) != 0) { if (!line.empty() && line[0] != '#') break; continue; }
        const auto lb = line.rfind('['), rb = line.rfind(']');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1)
            return line.substr(lb + 1, rb - lb - 1);
        return {};
    }
    return {};
}

// A PRESET IS A SAVED CALIBRATION. Rebuilt on every open because the list depends on the table that was
// clicked: a file whose values are in °C has no business being offered for a lambda sensor, and the
// file already records its own units, so the filter costs a line each rather than a registry.
void Surface::rebuildPresetMenu(const std::string& path) {
    if (!presetMenu_) return;
    presetMenu_->clear();
    if (path.empty() || !cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    const std::string want = t.cellUnits;

    // TWO FOLDERS: the user's own (Save to File puts curves there) and the presets that SHIP with the
    // studio. The shipped ones used to live only in the source tree, so no installed studio ever offered
    // them. The user's copy wins a name clash, so a preset can be corrected locally.
    std::vector<std::filesystem::path> files;
    std::vector<std::string> seen;
    std::error_code ec;
    for (const std::string& dir : { calibrationDir(), shippedCalibrationDir() }) {
        if (dir.empty()) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const auto ext = e.path().extension().string();
            if (ext != ".csv" && ext != ".txt") continue;
            const std::string stem = e.path().stem().string();
            if (std::find(seen.begin(), seen.end(), stem) != seen.end()) continue;
            seen.push_back(stem);
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    int shown = 0;
    for (const auto& f : files) {
        const std::string u = presetValueUnit(f.string());
        if (!u.empty() && !want.empty() && u != want) continue;      // a different quantity entirely
        const std::string file = f.string();
        presetMenu_->add(m_graph, f.stem().string())->onTriggered.connect(
            [this, file] { applyPreset(file); });
        ++shown;
    }
    if (!shown)
        presetMenu_->add(m_graph, "(none in " + calibrationDir() + ")");
}

// Applying one is loading one — the same reader, so a preset and a file a user saved themselves cannot
// diverge, and "save this curve" is how you MAKE a preset.
void Surface::applyPreset(const std::string& file) {
    const PanelElement* el = opTableElement();
    if (!el) el = opCurveElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const tablefile::Result r = tablefile::load(Cache::instance(), path, file);
    statusMessage.emit(r.message);
    if (r.ok) { if (Surface::onModified) Surface::onModified(); invalidate(); }
}

// Save this table to a CSV, or load one into it. The path is whichever table the menu targets — the
// table ops and the curve ops share it, so a curve saved from the curve view is the same file the grid
// would have written.
void Surface::tableToFile(bool save) {
    const PanelElement* el = opTableElement();
    if (!el) el = opCurveElement();
    if (!el || !onPickFile) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const std::string leaf = path.substr(path.rfind('.') + 1);
    onPickFile(save ? "Save " + leaf : "Load " + leaf, save, [this, path, save](std::string file) {
        if (file.empty()) return;
        Cache& c = Cache::instance();
        const tablefile::Result r = save ? tablefile::save(c, path, file) : tablefile::load(c, path, file);
        statusMessage.emit(r.message);
        if (r.ok && !save) { if (Surface::onModified) Surface::onModified(); invalidate(); }
    });
}

void Surface::curveInsertPoint() {
    const PanelElement* el = opCurveElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.axes.empty()) return;
    const int n = cache_.tiLiveN(t, 0);
    Cache::instance().tiInsertBin(cache_.resolveTable(path), 0, std::clamp(n / 2, 1, n));  // interior midpoint (tiInsertBin interpolates X+Y)
    invalidate();
}

void Surface::curveDeletePoint() {
    const PanelElement* el = opCurveElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.axes.empty()) return;
    const int n = cache_.tiLiveN(t, 0);
    Cache::instance().tiRemoveBin(cache_.resolveTable(path), 0, n - 1);  // drop the last point (Cache enforces the 2-bin floor)
    invalidate();
}

void Surface::curveLinearise() {
    const PanelElement* el = opCurveElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.axes.empty()) return;
    const int n = cache_.tiLiveN(t, 0);
    if (n < 2) return;
    std::vector<double> xs = cache_.tiBins(t, 0);
    const double x0 = xs.front(), x1 = xs.back();
    const double y0 = cache_.tiCell(t, 0, 0, 0), y1 = cache_.tiCell(t, n - 1, 0, 0);
    Cache& c = Cache::instance();
    c.beginEdit();
    for (int i = 0; i < n; ++i) xs[i] = x0 + (x1 - x0) * i / (n - 1);   // even-space X…
    c.tiWriteBins(t, 0, xs);
    for (int i = 0; i < n; ++i) c.tiSetCell(t, i, 0, 0, y0 + (y1 - y0) * i / (n - 1));  // …ramp Y straight
    c.endEdit("Linearise curve");
    invalidate();
}

// Read the selection block into a grid, hand it to fn to transform, write it back (read-all-then-write so a
// smooth/interpolate sees the original values, never partially-updated ones).
void Surface::transformTableBlock(int elId, const std::function<void(std::vector<std::vector<double>>&)>& fn) {
    const PanelElement* el = opModel() ? opModel()->get(elId) : nullptr;
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const TableWidget::TableCellSelection s = TableWidget::tableCellSelection(el->uid);
    if (!s.has) return;
    const int r0 = std::clamp(s.r0, 0, d.rows - 1), r1 = std::clamp(s.r1, 0, d.rows - 1);
    const int c0 = std::clamp(s.c0, 0, d.cols - 1), c1 = std::clamp(s.c1, 0, d.cols - 1);
    std::vector<std::vector<double>> g(r1 - r0 + 1, std::vector<double>(c1 - c0 + 1, 0.0));
    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) g[rr - r0][cc - c0] = TableWidget::cellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)));
    fn(g);
    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) TableWidget::setCellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)), g[rr - r0][cc - c0]);
    invalidate();
}


void Surface::tableArmOp(int op) {
    if (!tableElUnder_) return;
    // Route keys to the table so the operand can be typed inline — via the hosting VIEWPORT when the table is
    // mirrored (keys reach a mirror only through the viewport), else straight to the top-level table.
    setActiveControl_(opViewportId_ ? opViewportId_ : tableElUnder_);   // blurs whatever held it (commits its edit)
    TableWidget::tableBeginOp(opTableUid_(), op);
    invalidate();
}

void Surface::tableSetView(int view) {
    // Address the widget by its single identity (uid) and call its interface — the widget owns its view.
    // widgetByUid resolves the live instance across the whole tree, including a table mirrored in a viewport,
    // so there is no model to pick and nothing to clobber.
    if (auto* w = dynamic_cast<TableWidget*>(widgetByUid(tableTargetUid_))) {
        w->setView(view);
        // Flush the owned view into its model element now (commit writes saveContent → "view"), so the change
        // survives a tab/node round-trip that re-sources the instance, not only the save-time commit.
        if (auto* pm = opModel()) if (PanelElement* el = pm->get(tableElUnder_)) w->commit(*el);
        invalidate();
    }
}

void Surface::tableSetOrientation(int combo) {
    if (!tableElUnder_ || !opModel()) return;
    // ONE orientation prop: "axisMode" (1..8 = combo 0..7, 0 = global) — the same key the render (tableGeom)
    // and the inspector's Axis Display use. The menu previously wrote a SECOND key ("axisCombo") that nothing
    // in the render read, so it never transposed the view.
    opModel()->setProp(tableElUnder_, "axisMode", std::to_string(std::clamp(combo, 0, 7) + 1));
    invalidate();
}

void Surface::tableCopyCells() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const TableWidget::TableCellSelection s = TableWidget::tableCellSelection(opTableUid_());
    if (!s.has) return;
    const int r0 = std::clamp(s.r0, 0, d.rows - 1), r1 = std::clamp(s.r1, 0, d.rows - 1);
    const int c0 = std::clamp(s.c0, 0, d.cols - 1), c1 = std::clamp(s.c1, 0, d.cols - 1);
    tableClip_.assign(r1 - r0 + 1, std::vector<double>(c1 - c0 + 1, 0.0));
    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc)
        tableClip_[rr - r0][cc - c0] = TableWidget::cellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)));
}

// After a bulk cell op: tell the user if the dictionary bounds changed any of what they supplied.
// `what` names the operation ("Pasted", "Smoothed"). Silent on a clean run.
void Surface::reportClamped_(const char* what) {
    const int n = Cache::instance().clampedCount(), total = Cache::instance().writtenCount();
    if (n <= 0) return;
    char b[160];
    std::snprintf(b, sizeof(b), "%s %d cells - %d clamped to the allowed range", what, total, n);
    statusMessage.emit(b);
}

void Surface::tablePasteCells() {
    if (tableClip_.empty()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const TableWidget::TableCellSelection s = TableWidget::tableCellSelection(opTableUid_());
    const int pr = s.has ? s.activeRow : 0, pc = s.has ? s.activeCol : 0;   // anchor the paste at the active cell
    Cache::instance().beginEdit();
    for (int i = 0; i < static_cast<int>(tableClip_.size()); ++i)
        for (int j = 0; j < static_cast<int>(tableClip_[i].size()); ++j) {
            const int rr = pr + i, cc = pc + j;
            if (rr < d.rows && cc < d.cols) TableWidget::setCellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)), tableClip_[i][j]);
        }
    Cache::instance().endEdit("Paste cells");
    reportClamped_("Pasted");
    invalidate();
}

// Effective display decimals for a table element: prop "decimals" encodes 1..7 = 0..6 places, 0 = auto (the
// render falls back to 1). Mirrors the table descriptor so a copied grid reads exactly like what's on screen.
static int tableEffDecimals(const PanelElement& el) {
    const int decRaw = std::atoi(el.prop("decimals").c_str());
    return decRaw > 0 ? decRaw - 1 : 1;
}

// Copy the WHOLE grid (display order, formatted to the table's decimals) to the system clipboard as TSV, so it
// round-trips through a spreadsheet. Mirrors TableEditor::copyTable (headers omitted — paste tolerates them).
void Surface::tableCopyTable() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const int dec = tableEffDecimals(*el);
    std::string out;
    char cell[48];
    for (int rr = 0; rr < d.rows; ++rr)
        for (int cc = 0; cc < d.cols; ++cc) {
            std::snprintf(cell, sizeof(cell), "%.*f", dec,
                          TableWidget::cellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid))));
            out += cell;
            out += (cc + 1 < d.cols) ? '\t' : '\n';
        }
    jf::JClipboard::setText(out);
}

// Paste a TSV grid from the system clipboard into the cells. Tolerates a leading header row / column (a
// non-numeric leading token → NaN placeholder, then dropped by a size match). Mirrors TableEditor::pasteTable.
void Surface::tablePasteTable() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const std::string text = jf::JClipboard::getText();
    if (text.empty()) return;

    auto parseNum = [](const std::string& s, bool& ok) -> double {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) { ok = false; return 0.0; }
        const std::string tk = s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
        const char* c = tk.c_str(); char* end = nullptr;
        const double v = std::strtod(c, &end);
        ok = (end != c && *end == '\0');
        return v;
    };

    std::vector<std::vector<double>> grid;
    for (size_t pos = 0; pos <= text.size(); ) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        std::vector<double> nums;
        for (size_t tp = 0; tp <= line.size(); ) {
            const size_t sep = line.find_first_of("\t,;", tp);
            const std::string tok = line.substr(tp, sep == std::string::npos ? std::string::npos : sep - tp);
            tp = (sep == std::string::npos) ? line.size() + 1 : sep + 1;
            bool ok = false; const double v = parseNum(tok, ok);
            if (ok) nums.push_back(v);
            else if (nums.empty() && tok.find_first_not_of(" \t\r") != std::string::npos)
                nums.push_back(std::nan(""));                 // a non-numeric leading token = a row-header cell
        }
        if (!nums.empty()) grid.push_back(std::move(nums));
    }
    if (grid.empty()) return;

    const int rowOff = (static_cast<int>(grid.size()) == d.rows + 1) ? 1 : 0;   // drop a header row if it lines up
    Cache::instance().beginEdit();
    for (int r = 0; r < d.rows && r + rowOff < static_cast<int>(grid.size()); ++r) {
        const std::vector<double>& src = grid[r + rowOff];
        const int colOff = (static_cast<int>(src.size()) == d.cols + 1) ? 1 : 0;
        for (int c = 0; c < d.cols && c + colOff < static_cast<int>(src.size()); ++c)
            if (!std::isnan(src[c + colOff]))
                TableWidget::setCellValue(*el, t, TableWidget::tableCellOffset(t, d, r, c, TableWidget::tableCurrentPlane(el->uid)), src[c + colOff]);
    }
    Cache::instance().endEdit("Paste table");
    reportClamped_("Pasted");
    invalidate();
}

// The display unit each STORAGE axis of this table is being shown in, so the axis editor opens in the
// same units as the grid that launched it.
// Hands the axis editor a way to write a unit back onto the widget that opened it. The target is
// CAPTURED NOW, not read when the dialog answers: by then another hit-test has reset the menu's target,
// and the write would go to the page model with a panel child's id — silently nowhere.
std::function<void(std::string, std::string)> Surface::unitSetter_(int elementId) {
    PanelModel* model = opModel();
    const int panelId = tablePanelId_;
    const std::string childUid = tableTargetUid_;
    return [this, model, elementId, panelId, childUid](std::string prop, std::string unitId) {
        if (!prop.empty()) setTablePropOn_(model, elementId, panelId, childUid, prop, std::move(unitId));
    };
}

std::vector<std::string> Surface::axisUnitsOf_() const {
    std::vector<std::string> u;
    for (int a = 0; a < 3; ++a) u.push_back(tableProp_(TableWidget::axisUnitProp(a)));
    return u;
}

// Read a property of the table the menu targets — from wherever setTableProp_ would WRITE it. A panel
// child is not reachable through the model (its uid resolves only through a live widget, and a mirrored
// page's panel is not among the surface's instances), so reading it off the element an id lookup returns
// gives an unrelated widget's value. Symmetry with the write is the whole point.
std::string Surface::tableProp_(const std::string& key) const {
    if (tablePanelId_ && opModel())
        if (const PanelElement* panel = opModel()->get(tablePanelId_)) {
            jf::JJson kids = jf::JJson::parse(panel->prop("children"));
            if (kids.isArray())
                for (const jf::JJson& k : kids.arr())
                    if (k["uid"].str() == tableTargetUid_) return k["props"][key].str();
        }
    const PanelElement* el = opTableElement();
    return el ? el->prop(key) : std::string();
}

// Set a property on the table the menu targets. A page element is one call; a PANEL CHILD is not an
// element at all — a panel keeps its children as JSON in its own "children" prop, with an id space of
// their own — so writing there means editing that JSON. Doing it the naive way (setProp on the page
// model with the child's id) does not merely fail: page ids and child ids are both small integers, so it
// lands on whichever page element happens to share the number, silently changing an unrelated widget.
void Surface::setTableProp_(const std::string& key, const std::string& value) {
    setTablePropOn_(opModel(), tableElUnder_, tablePanelId_, tableTargetUid_, key, value);
}

// The same write against a target captured EARLIER. Everything above resolves its target from mutable
// state that the next hit-test clears — fine for a menu item firing immediately, wrong for a dialog that
// answers later, by which time tablePanelId_ can be 0 and the write goes silently astray.
void Surface::setTablePropOn_(PanelModel* model, int elementId, int panelId, const std::string& childUid,
                              const std::string& key, const std::string& value) {
    if (!model) return;
    if (!panelId) { model->setProp(elementId, key, value); invalidate(); return; }

    const PanelElement* panel = model->get(panelId);
    if (!panel) return;
    jf::JJson kids = jf::JJson::parse(panel->prop("children"));
    if (!kids.isArray()) return;
    bool hit = false;
    for (jf::JJson& k : kids.arr()) {
        if (k["uid"].str() != childUid) continue;          // by UID: the child's id is the panel's business
        jf::JJson props = k["props"];
        if (!props.isObject()) props = jf::JJson::object();
        props[key] = value;
        k["props"] = props;
        hit = true;
        break;
    }
    if (!hit) return;
    model->setProp(panelId, "children", kids.dump());      // one undoable edit, as any prop is
    invalidate();
}

// Increase/decrease displayed decimal places (a view setting) — for
// WHATEVER IS UNDER THE POINTER. Over an axis strip it is that axis's precision; over the grid it is the
// cells'. One menu item used to mean "the cells" wherever you opened it, so an axis showing the wrong
// precision could only be fixed through the properties dock, and bumping from the axis silently
// reformatted the cells instead.
//
// The prop encodes 1..7 = 0..6 places, and writing it is an explicit override: an axis that was
// following its channel stops following it, which is what asking for a specific precision means.
void Surface::tableBumpDecimals(int delta) {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el || el->type != "table") return;
    const int axis = TableWidget::tableHoverAxis(opTableUid_());
    if (axis >= 0 && axis < 3) {
        static const char* kAxisDec[3] = { "decimalsX", "decimalsY", "decimalsZ" };
        const TableImage t = cache_.resolveTable(bindOf_(*el, scopeCtx_));
        // Bump from what is ON SCREEN, so the first press always moves by one from what you can see —
        // whether that came from an earlier override or from the channel the axis reads.
        const int cur = TableWidget::axisDecimals(*el, cache_, t, axis, tableEffDecimals(*el));
        const int places = std::clamp(cur + delta, 0, 6);
        setTableProp_(kAxisDec[axis], std::to_string(places + 1));
        return;
    }
    const int places = std::clamp(tableEffDecimals(*el) + delta, 0, 6);
    setTableProp_("decimals", std::to_string(places + 1));
}

// Distribute n breakpoints over [first,last] with a clustering warp.
// mode: 0 linear/even, 1 dense in the middle, 2 dense at both ends, 3 dense around cn (0..1). Strength p>1.
static std::vector<double> warpBins(double first, double last, int n, int mode, double cn = 0.5, double p = 2.4) {
    std::vector<double> v(std::max(0, n));
    if (n <= 0) return v;
    if (n == 1) { v[0] = first; return v; }
    const double span = last - first;
    for (int k = 0; k < n; ++k) {
        const double u = static_cast<double>(k) / (n - 1);
        double w;
        switch (mode) {
            case 1: { const double t = 2.0 * u - 1.0; w = 0.5 + 0.5 * (t < 0 ? -1.0 : 1.0) * std::pow(std::fabs(t), p); break; }
            case 2: { const double t = 2.0 * u - 1.0; w = 0.5 + 0.5 * (t < 0 ? -1.0 : 1.0) * std::pow(std::fabs(t), 1.0 / p); break; }
            case 3: { const double c = std::clamp(cn, 1e-3, 1.0 - 1e-3); const double t = u - c;
                      w = (t >= 0.0) ? c + (1.0 - c) * std::pow(t / (1.0 - c), p) : c - c * std::pow(-t / c, p); break; }
            default: w = u; break;
        }
        v[k] = first + span * w;
    }
    return v;
}

void Surface::tableDistributeBins(int axis, int mode) {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || axis < 0 || axis >= static_cast<int>(t.axes.size())) return;
    const int n = cache_.tiLiveN(t, axis);
    if (n < 2) return;
    const std::vector<double> bins = cache_.tiBins(t, axis);
    Cache& c = Cache::instance();
    c.beginEdit();
    c.tiWriteBins(t, axis, warpBins(bins.front(), bins.back(), n, mode));
    c.endEdit("Distribute bins");
    invalidate();
}

void Surface::tableToggleCellTrace() {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el || el->type != "table") return;
    const int cur = std::atoi(el->prop("cellTrace").c_str());
    opModel()->setProp(tableElUnder_, "cellTrace", (cur == 2) ? "1" : "2");   // 2 = on, 1 = off
    invalidate();
}

void Surface::tablePlaneStep(int delta) {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const int zn = std::max(1, cache_.liveDepth(opBind_(*el)));
    TableWidget::tableSetPlane(opTableUid_(), std::clamp(TableWidget::tableCurrentPlane(opTableUid_()) + delta, 0, zn - 1));
    invalidate();
}

void Surface::tableCopyPlaneToAll() {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    const int zn = std::max(1, cache_.liveDepth(path));
    if (zn < 2) return;
    const int cur = std::clamp(TableWidget::tableCurrentPlane(opTableUid_()), 0, zn - 1);
    Cache& c = Cache::instance();
    c.beginEdit();
    for (int z = 0; z < zn; ++z) if (z != cur) c.copyTablePlane(path, cur, z);
    c.endEdit("Copy plane to all");
    invalidate();
}

// Copy the active Z plane onto ONE specific plane z (the "Copy this plane to…" submenu). One undo step.
void Surface::tableCopyPlaneTo(int z) {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    const int zn = std::max(1, cache_.liveDepth(path));
    const int cur = std::clamp(TableWidget::tableCurrentPlane(opTableUid_()), 0, zn - 1);
    if (z < 0 || z >= zn || z == cur) return;
    Cache& c = Cache::instance();
    c.beginEdit();
    c.copyTablePlane(path, cur, z);
    c.endEdit("Copy plane to " + std::to_string(z + 1));
    invalidate();
}

// Repopulate "Copy this plane to…" with one item per OTHER plane of the table under the cursor (the plane
// count is live, so the submenu is rebuilt each time the run-mode menu opens — JMenu::clear + re-add).
// UNITS ▸ — the run-mode answer to "show me this in something else".
//
// Every bound control gets it, not just tables: a reading is a quantity, and a quantity that has more
// than one unit is a choice the person watching it should be able to make where they are watching it.
// Until now that choice lived only in the edit-mode Properties dock, which means locking the layout,
// switching modes, finding the control, changing a property, and switching back — to look at volts.
//
// The choice is remembered against the CHANNEL, not this widget: a unit set once is meant to hold
// wherever that channel is shown (see CanvasWidget::displayUnitOf, which reads ChannelPrefs between the
// page's explicit pin and the global preference). "Global" clears it and lets the per-quantity
// preference decide again, which is what most controls should be doing most of the time.
void Surface::rebuildUnitsMenu() {
    unitsRootMenu_.clear();
    unitsChannel_.clear();
    JLOGC("units", jf::JLogLevel::Debug) << "rebuild: el=" << unitsElUnder_ << " model=" << (opModel() ? 1 : 0);
    if (!unitsElUnder_ || !opModel()) return;
    // opChanElement(), NOT opModel()->get(). A control inside a PANEL lives in the panel's own child
    // model, so its id means nothing to the surface's model and the lookup came back null — which is
    // most of a sensor page. chanElAt already recorded the target's uid; that is what resolves it,
    // whichever model it belongs to, and it is what every other run-mode channel op uses.
    const PanelElement* el = opChanElement();
    if (!el) { JLOGC("units", jf::JLogLevel::Debug) << "  no element"; return; }
    // opBind_, NOT bindOf_(el, scopeCtx_). A control reached through a viewport belongs to the mirrored
    // page, and its binding resolves against THAT page's element context — "[$~]" means "this sensor's
    // raw channel", and which sensor is the viewport's business, not this surface's. Resolved against the
    // wrong context it stays a sigil, the unit lookup comes back empty, and the menu silently declines to
    // appear. Every other run-mode op on a mirrored element already goes through opBind_.
    const std::string bind = CanvasWidget::bareChannelOf(opBind_(*el));
    JLOGC("units", jf::JLogLevel::Debug) << "  type=" << el->type << " bind='" << bind << "'";
    if (bind.empty()) return;
    const std::string src = CanvasWidget::bindingUnit(bind);
    JLOGC("units", jf::JLogLevel::Debug) << "  srcUnit='" << src << "'";
    if (src.empty()) return;
    UnitManager& um = UnitManager::instance();
    const std::string q = um.findQuantityForUnit(src);
    JLOGC("units", jf::JLogLevel::Debug) << "  quantity='" << q << "'";
    if (q.empty()) return;
    const UnitManager::Quantity qd = um.getQuantity(q);
    JLOGC("units", jf::JLogLevel::Debug) << "  units=" << qd.units.size();
    if (qd.units.size() < 2) return;              // one unit is not a choice worth a menu

    unitsChannel_ = bind;
    const ChannelPref* cp = ChannelPrefs::instance().find(bind);
    const std::string pinned = cp ? cp->unit : std::string();

    auto* gi = unitsRootMenu_.add(m_graph, "Global");
    gi->setCheckable(true);
    gi->setChecked(pinned.empty());
    gi->onTriggered.connect([this] {
        if (unitsChannel_.empty()) return;
        ChannelPref p = ChannelPrefs::instance().find(unitsChannel_)
                      ? *ChannelPrefs::instance().find(unitsChannel_) : ChannelPref{};
        p.unit.clear();
        ChannelPrefs::instance().set(unitsChannel_, p);
        invalidate();
    });
    unitsRootMenu_.addSeparator(m_graph);
    for (const UnitManager::Unit& u : qd.units) {
        auto* it = unitsRootMenu_.add(m_graph, u.label.empty() ? u.id : u.label);
        it->setCheckable(true);
        it->setChecked(pinned == u.id);
        const std::string uid = u.id;
        it->onTriggered.connect([this, uid] {
            if (unitsChannel_.empty()) return;
            ChannelPref p = ChannelPrefs::instance().find(unitsChannel_)
                          ? *ChannelPrefs::instance().find(unitsChannel_) : ChannelPref{};
            p.unit = uid;
            ChannelPrefs::instance().set(unitsChannel_, p);
            invalidate();
        });
    }
}

void Surface::rebuildPlaneCopyToMenu() {
    if (!planeCopyToMenu_) return;
    planeCopyToMenu_->clear();
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    const int zn  = std::max(1, cache_.liveDepth(opBind_(*el)));
    const int cur = std::clamp(TableWidget::tableCurrentPlane(opTableUid_()), 0, zn - 1);
    for (int z = 0; z < zn; ++z) {
        if (z == cur) continue;
        planeCopyToMenu_->add(m_graph, "Plane " + std::to_string(z + 1))->onTriggered.connect([this, z] { tableCopyPlaneTo(z); });
    }
}

void Surface::tableInterpolatePlanes() {
    if (!tableElUnder_ || !opModel()) return;
    const PanelElement* el = opTableElement();
    if (!el) return;
    Cache::instance().interpolateTablePlanes(opBind_(*el));   // one undo step; no-op for < 3 planes
    invalidate();
}

void Surface::tableBinOp(bool row, bool del) {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid) return;
    if (row && t.axes.size() < 2) return;                       // a 1D table has no rows
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const TableWidget::TableCellSelection s = TableWidget::tableCellSelection(opTableUid_());
    // "Row" = the vertical display axis, "Column" = the horizontal — which STORAGE axis that is depends on
    // transpose. Vertical bin index is flipped (top = highest); horizontal is straight (left = lowest).
    const int storageAxis = row ? (d.transpose ? 0 : 1) : (d.transpose ? 1 : 0);
    const int at = row ? (d.rows - 1 - std::clamp(s.has ? s.activeRow : 0, 0, d.rows - 1))
                       : std::clamp(s.has ? s.activeCol : 0, 0, d.cols - 1);
    Cache::instance().beginEdit();
    if (del) Cache::instance().removeTableBin(path, storageAxis, at);  // Cache enforces nMax (insert) + a 2-bin floor (remove)
    else     Cache::instance().insertTableBin(path, storageAxis, at);
    Cache::instance().endEdit(del ? "Delete bin" : "Insert bin");
    invalidate();
}

void Surface::tableLinearise(bool horiz, bool vert) {
    if (!tableElUnder_) return;
    transformTableBlock(tableElUnder_, [&](std::vector<std::vector<double>>& g) {
        const int R = static_cast<int>(g.size()), C = R ? static_cast<int>(g[0].size()) : 0;
        if (horiz && vert && R >= 1 && C >= 1) {                 // bilinear from the four block corners
            const double tl = g[0][0], tr = g[0][C - 1], bl = g[R - 1][0], br = g[R - 1][C - 1];
            for (int r = 0; r < R; ++r) for (int c = 0; c < C; ++c) {
                const double fr = R > 1 ? double(r) / (R - 1) : 0.0, fc = C > 1 ? double(c) / (C - 1) : 0.0;
                const double top = tl + (tr - tl) * fc, bot = bl + (br - bl) * fc;
                g[r][c] = top + (bot - top) * fr;
            }
        } else if (horiz) {                                     // each row: left -> right
            for (int r = 0; r < R; ++r) { const double a = g[r][0], b = g[r][C - 1];
                for (int c = 0; c < C; ++c) g[r][c] = C > 1 ? a + (b - a) * double(c) / (C - 1) : a; }
        } else if (vert) {                                      // each column: top -> bottom
            for (int c = 0; c < C; ++c) { const double a = g[0][c], b = g[R - 1][c];
                for (int r = 0; r < R; ++r) g[r][c] = R > 1 ? a + (b - a) * double(r) / (R - 1) : a; }
        }
    });
}

// APPLY TO BASE TABLE — roll a learned correction into the map it corrects, then reset it to neutral.
//
// THE TWO ENDS OF A LEARNED CORRECTION, reached from the table's own menu. The operations themselves
// live in model/LearnedOps.h, addressed by PATH, because the page buttons on the LTFT and Bank Trim
// pages do the same two things to the same tables and there must not be two implementations of
// "fold a trim into its base map". All this adds is the selection: which table the right-click was on.
//
// Both are operator-confirmed — one rewrites a fuel map, the other discards what a drive taught the
// engine — and the question is phrased once, by the plan, so the menu and the button ask it the same way.
void Surface::tableApplyToBase() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    const learned::Plan p = learned::planApplyToBase(path);
    if (!p.possible) return;
    if (p.cells == 0) {
        if (onConfirmAction) onConfirmAction("Nothing to apply", p.detail, "", [] {});
        return;
    }
    auto doApply = [this, path] { learned::applyToBase(path); if (onModified) onModified(); };
    if (onConfirmAction) onConfirmAction("Apply to " + p.baseName + "?", p.detail, "Apply to Base Table", doApply);
    else                 doApply();
}

void Surface::tableResetToZero() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    const learned::Plan p = learned::planReset(path);
    if (!p.possible) return;
    if (p.cells == 0) {
        if (onConfirmAction) onConfirmAction("Nothing to reset", p.detail, "", [] {});
        return;
    }
    auto doReset = [this, path] { learned::resetToZero(path); if (onModified) onModified(); };
    if (onConfirmAction) onConfirmAction("Reset " + p.trimName + "?", p.detail, "Reset", doReset);
    else                 doReset();
}

void Surface::tableSmooth() {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    const TableImage t = cache_.resolveTable(path);
    if (!t.valid || t.cellSize <= 0) return;
    const TableWidget::TableDims d = TableWidget::tableDims(cache_, t, TableWidget::tableTransposed(*el));
    const TableWidget::TableCellSelection s = TableWidget::tableCellSelection(opTableUid_());
    if (!s.has) return;
    const int r0 = std::clamp(s.r0, 0, d.rows - 1), r1 = std::clamp(s.r1, 0, d.rows - 1);
    const int c0 = std::clamp(s.c0, 0, d.cols - 1), c1 = std::clamp(s.c1, 0, d.cols - 1);
    // Snapshot the FULL table first, so a selected cell on the block's border smooths against its real
    // neighbours OUTSIDE the selection — a 3x3 box average, clamped to the TABLE edge.
    std::vector<std::vector<double>> orig(d.rows, std::vector<double>(d.cols, 0.0));
    for (int rr = 0; rr < d.rows; ++rr) for (int cc = 0; cc < d.cols; ++cc)
        orig[rr][cc] = TableWidget::cellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)));
    Cache::instance().beginEdit();
    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) {
        double sum = 0; int n = 0;
        for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc) {
            const int nr = rr + dr, nc = cc + dc;
            if (nr >= 0 && nr < d.rows && nc >= 0 && nc < d.cols) { sum += orig[nr][nc]; ++n; }
        }
        TableWidget::setCellValue(*el, t, TableWidget::tableCellOffset(t, d, rr, cc, TableWidget::tableCurrentPlane(el->uid)), n ? sum / n : orig[rr][cc]);
    }
    Cache::instance().endEdit("Smooth");
    reportClamped_("Smoothed");
    invalidate();
}

void Surface::tableRestore(bool baseline) {
    const PanelElement* el = opTableElement();
    if (!el) return;
    const std::string path = opBind_(*el);
    if (!cache_.isTable(path)) return;
    if (baseline) Cache::instance().restoreFromBaseline(path);
    else          Cache::instance().restoreTableDefaults(path);
    invalidate();
}

void Surface::syncStatefulChecks(const PanelElement* tableEl, const PanelElement* panelEl) {
    // Mirror each widget's CURRENT discrete state as a ● beside the active option — read the very same props the
    // render (tableGeom / PanelWidget) reads, so the tick can never disagree with what's drawn.
    int view = 0, combo = 4;   // defaults: 2D grid, "Left & top" (combo 4) when the props are unset
    if (tableEl) {
        const std::string vs = tableEl->prop("view");
        if (!vs.empty()) view = std::clamp(std::atoi(vs.c_str()), 0, 2);
        // Through the same resolver as the render, so a table left on "global" ticks whatever the global
        // actually says — a literal 4 here would tick "left & top" while the table drew something else.
        combo = std::clamp(TableWidget::choiceOf(*tableEl, "axisMode") - 1, 0, 7);
    }
    for (int i = 0; i < 3; ++i) if (viewItems_[i])   viewItems_[i]->setChecked(tableEl && i == view);
    for (int i = 0; i < 8; ++i) if (orientItems_[i]) orientItems_[i]->setChecked(tableEl && i == combo);
    int lm = 0;   // panel layout: 0 = Free
    if (panelEl) { const std::string ls = panelEl->prop("layoutMode"); if (!ls.empty()) lm = std::clamp(std::atoi(ls.c_str()), 0, 7); }
    for (int i = 0; i < 8; ++i) if (panelLayoutItems_[i]) panelLayoutItems_[i]->setChecked(panelEl && i == lm);
}

void Surface::refreshMenuState() {
    const size_t n = selection_.size();
    const bool any = n > 0;
    miCopy_->setEnabled(any); miCut_->setEnabled(any); miDelete_->setEnabled(any);
    miPaste_->setEnabled(!clipboard_.empty());
    miFront_->setEnabled(any); miBack_->setEnabled(any);
    miFwd_->setEnabled(any);   miBwd_->setEnabled(any);
    miGroup_->setEnabled(n >= 2); miUngroup_->setEnabled(selectionHasGroup());
    // Make Same Size is off whenever a GROUP is in the selection. Align/distribute move a group as one
    // unit, which is meaningful; a SIZE has no equivalent — resizing a group would either stretch its box
    // (and the members with it) or set every member to one size, and neither is what "make these the same
    // size" means. Sizing is a widget operation, so it is offered only on widgets.
    const bool grouped = selectionHasGroup();
    miAlign_->setEnabled(n >= 2); miSize_->setEnabled(n >= 2 && !grouped); miDist_->setEnabled(n >= 3);
    if (miSpacing_) miSpacing_->setEnabled(n >= 2);
    if (miArrange_) miArrange_->setEnabled(any);   // Arrange ▸ needs at least one control (Front/Back); inner items grey per count
    // Authoring editors act on ONE control; the typed ones only on their widget kind.
    const PanelElement* solo = (n == 1 && model_) ? model_->get(selection_.front()) : nullptr;
    if (miEdit_) miEdit_->setEnabled(solo != nullptr);   // Edit ▸ authoring dialogs are all single-control
    miCondition_->setEnabled(solo != nullptr);
    miEnable_->setEnabled(solo != nullptr);
    miPresets_->setEnabled(solo && solo->type == "settingselector");
    miLines_->setEnabled(solo && solo->type == "livegraph");
    miPanel_->setEnabled(solo && solo->type == "panel");
    if (miPanelEdit_) miPanelEdit_->setEnabled(solo && solo->type == "panel");
    miPanelLayout_->setEnabled(solo && solo->type == "panel");
    if (miTableSub_) miTableSub_->setEnabled(solo && solo->type == "table" && cache_.isTable(bindOf_(*solo, scopeCtx_)));
    if (miCurveSub_) miCurveSub_->setEnabled(solo && solo->type == "curve" && cache_.isTable(bindOf_(*solo, scopeCtx_)));
}

// Open an app-wired modal editor for one prop of the single selected control; the modal's apply
// callback writes the new value back as one undoable history step.
std::function<void(std::string, std::function<void(std::string)>)> Surface::conditionHook() {
    if (!onEditCondition) return {};   // unwired: openPropEditor's own !hook guard must still see nothing
    return [this](std::string cur, std::function<void(std::string)> apply) {
        if (onEditCondition) onEditCondition(std::move(cur), scopeCtx_, std::move(apply));
    };
}

void Surface::openPropEditor(const std::function<void(std::string, std::function<void(std::string)>)>& hook,
                             const std::string& prop, const std::string& text) {
    if (!hook || !model_ || selection_.size() != 1) return;
    const PanelElement* el = model_->get(selection_.front());
    if (!el) return;
    const int id = el->id;
    hook(el->prop(prop), [this, id, prop, text](std::string v) {
        if (!model_ || !model_->get(id)) return;                 // control deleted while the modal was up
        const std::vector<PanelElement> before = elementsSnapshot();
        model_->setProp(id, prop, v);
        commitEdit(text, before);
        invalidate();
    });
}

void Surface::prepareContextMenu(float mx, float my) {
    if (mode_ == Mode::Run) {   // run-mode menu targets the table/curve under the cursor
        // Resolve ONE target. tableElAt/curveElAt share opModel_/opViewportId_ and each RESETS them on entry,
        // so running curveElAt after tableElAt found a table wiped the table's resolved (mirror) model — sending
        // every table op to the wrong element. A right-click is a table OR a curve, never both: short-circuit.
        tableElUnder_ = tableElAt(mx, my);
        curveElUnder_ = tableElUnder_ ? 0 : curveElAt(mx, my);
        // …and the preset list follows whichever it was: a folder of saved curves, filtered to the ones
        // whose values are in this table's units. Rebuilt here rather than once at startup because it
        // is a directory, and because the right answer depends on what was clicked.
        {
            const PanelElement* pe = tableElUnder_ ? opTableElement() : curveElUnder_ ? opCurveElement() : nullptr;
            rebuildPresetMenu(pe ? opBind_(*pe) : std::string());
        }
        // …then the multi-channel controls, which are neither: a watch list and a trace view answer with
        // their own short menus. Same short-circuit rule — each resolver resets opModel_ on entry, so only
        // ask the ones that could still be the target.
        chanElUnder_ = 0;
        jf::JMenu* chanMenu = nullptr;
        if (!tableElUnder_ && !curveElUnder_) {
            if ((chanElUnder_ = chanElAt(mx, my, "livegraph"))) chanMenu = &graphMenu_;
            else if ((chanElUnder_ = chanElAt(mx, my, "channels"))) chanMenu = &chanListMenu_;
        }
        // NOTHING SPECIFIC CLAIMED IT? Then it is an ordinary bound control, and the one thing worth
        // offering there is what unit it is read in — previously reachable only by locking the layout,
        // switching to edit mode and finding the control in the Properties dock, to look at volts.
        unitsElUnder_ = 0;
        if (!tableElUnder_ && !curveElUnder_ && !chanElUnder_) {
            unitsElUnder_ = chanElAt(mx, my, nullptr);
            JLOGC("units", jf::JLogLevel::Debug) << "rightclick: hit=" << unitsElUnder_;
            rebuildUnitsMenu();
            if (unitsChannel_.empty()) unitsElUnder_ = 0;   // no quantity, or only one unit: no menu
        }
        // Set the menu from the click position (not a prior hover) so a right-click always finds it.
        setContextMenu(tableElUnder_ ? &tableMenu_ : curveElUnder_ ? &curveMenu_
                     : chanMenu ? chanMenu : (unitsElUnder_ ? &unitsRootMenu_ : nullptr));
        const PanelElement* te = (tableElUnder_ && model_) ? opTableElement() : nullptr;
        if (miCellTrace_ && te) miCellTrace_->setChecked(std::atoi(te->prop("cellTrace").c_str()) == 2);
        // A LEARNED TRIM, OR NOT. Both operations only mean anything for a table that declares the base
        // map it corrects; on an ordinary map they were shown and silently did nothing, which is worse
        // than not offering them. The relationship is data (`apply_to`), so this asks the table.
        {
            const bool learned = te && !cache_.resolveTable(opBind_(*te)).applyTo.empty();
            if (miApplyToBase_)  miApplyToBase_->setVisible(learned);
            if (miResetLearned_) miResetLearned_->setVisible(learned);
        }
        syncStatefulChecks(te, nullptr);   // ● the table's current View + Orientation
        rebuildPlaneCopyToMenu();   // "Copy this plane to…" targets depend on this table's live plane count
        return;
    }
    if (mode_ != Mode::Edit || !model_) return;
    hoverX_ = mx; hoverY_ = my;                 // anchor "Add control" to the exact right-click point
    const int hit = hitTest(mx, my, xform());
    if (hit && !isSelected(hit)) { selectOnly(hit); expandToGroups(); selectionChanged.emit(); invalidate(); }
    menuTargetId_ = hit;   // every arrange op measures from the widget you right-clicked (see arrangeRef)
    // Point the table/curve ops at the selected control so its "Table"/"Curve" submenu operates on it here.
    opModel_ = model_;   // edit-mode selection is always in this surface's model (never a mirror)
    const PanelElement* selEl = (selection_.size() == 1 && model_) ? model_->get(selection_.front()) : nullptr;
    tableElUnder_ = (selEl && selEl->type == "table" && cache_.isTable(bindOf_(*selEl, scopeCtx_))) ? selEl->id : 0;
    curveElUnder_ = (selEl && selEl->type == "curve" && cache_.isTable(bindOf_(*selEl, scopeCtx_))) ? selEl->id : 0;
    if (tableElUnder_) rebuildPlaneCopyToMenu();
    // ● the current state on any stateful submenu of the selected control (Table View/Orientation, Panel Layout).
    syncStatefulChecks(tableElUnder_ ? selEl : nullptr, (selEl && selEl->type == "panel") ? selEl : nullptr);
    refreshMenuState();
}

// --- Grouping ---------------------------------------------------------------------------------
void Surface::expandToGroups() {
    if (!model_) return;
    std::vector<int> gids;
    for (int id : selection_) if (const PanelElement* e = model_->get(id)) if (e->groupId) gids.push_back(e->groupId);
    if (gids.empty()) return;
    for (const auto& e : model_->elements())
        if (e.groupId && std::find(gids.begin(), gids.end(), e.groupId) != gids.end() && !isSelected(e.id)
            && !nodeHidden_(e))          // a group-mate on ANOTHER node is not on screen either
            selection_.push_back(e.id);
}
bool Surface::selectionHasGroup() const {
    if (!model_) return false;
    for (int id : selection_) if (const PanelElement* e = model_->get(id)) if (e->groupId) return true;
    return false;
}
void Surface::groupSelection() {
    if (!model_ || selection_.size() < 2) return;
    const int gid = model_->nextGroupId();
    for (int id : selection_) model_->setGroupId(id, gid);
    JLOGC("surface", jf::JLogLevel::Info) << "[group] " << selection_.size() << " elements -> groupId " << gid;
    invalidate();
}
void Surface::ungroupSelection() {
    if (!model_ || selection_.empty()) return;
    std::vector<int> gids;
    for (int id : selection_) if (const PanelElement* e = model_->get(id)) if (e->groupId) gids.push_back(e->groupId);
    if (gids.empty()) { JLOGC("surface", jf::JLogLevel::Info) << "[ungroup] no group in selection — no-op"; return; }
    JLOGC("surface", jf::JLogLevel::Info) << "[ungroup] dissolving " << gids.size()
                                          << " group(s) across the selection (marks the layout dirty)";
    std::vector<int> ids;                       // snapshot (setGroupId mutates via signals)
    for (const auto& e : model_->elements()) ids.push_back(e.id);
    for (int id : ids) if (const PanelElement* e = model_->get(id))
        if (std::find(gids.begin(), gids.end(), e->groupId) != gids.end()) model_->setGroupId(id, 0);
    invalidate();
}

// --- Align / distribute / match-size ------------------------------------------------------------
// The widget the context menu was invoked on, provided it is actually in the selection — a target from an
// earlier menu must not silently steer this one. Anything else (a shortcut, a stale id) falls back to the
// selection's first element, which is what every op used before.
std::string Surface::bindOf_(const PanelElement& el, const std::string& ctx) const {
    return CanvasWidget::resolveTemplate(el.prop("signalName"), ctx);
}

// The op target's binding. opViewportId_ names the host viewport when the target is mirrored inside one;
// otherwise the subject is whatever page we are editing (empty at the top level).
std::string Surface::opBind_(const PanelElement& el) const {
    std::string ctx = scopeCtx_;
    if (opViewportId_ && model_)
        if (const PanelElement* vp = model_->get(opViewportId_)) ctx = vp->prop("signalName");
    return bindOf_(el, ctx);
}

bool Surface::nodeHidden_(const PanelElement& el) const {
    if (inScope()) return false;                 // inside a page: its content is shown by the host
    const std::string node = el.prop("node");
    return !node.empty() && node != activeNode_;
}

const PanelElement* Surface::arrangeRef() const {
    if (!model_ || selection_.empty()) return nullptr;
    if (menuTargetId_ && isSelected(menuTargetId_)) if (const PanelElement* e = model_->get(menuTargetId_)) return e;
    return model_->get(selection_.front());
}


// A GROUP is one item. The selection already contains every member (selecting one expands to the group),
// so an arrange op that walks elements individually moves each member to the same edge and collapses the
// group's internal layout — which is precisely what "acts as a singular item" is supposed to prevent.
std::vector<std::vector<int>> Surface::arrangeUnits() const {
    std::vector<std::vector<int>> units;
    std::vector<int> seenGroups;
    for (int id : selection_) {
        const PanelElement* e = model_ ? model_->get(id) : nullptr;
        if (!e) continue;
        if (!e->groupId) { units.push_back({ id }); continue; }
        if (std::find(seenGroups.begin(), seenGroups.end(), e->groupId) != seenGroups.end()) continue;
        seenGroups.push_back(e->groupId);
        std::vector<int> members;
        for (int other : selection_)
            if (const PanelElement* o = model_->get(other)) if (o->groupId == e->groupId) members.push_back(other);
        units.push_back(std::move(members));
    }
    return units;
}

JRect Surface::unitBox(const std::vector<int>& ids) const {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : ids) if (const PanelElement* e = model_->get(id)) {
        x0 = std::min(x0, e->x); y0 = std::min(y0, e->y);
        x1 = std::max(x1, e->x + e->w); y1 = std::max(y1, e->y + e->h);
    }
    if (x1 < x0) return JRect{ 0, 0, 0, 0 };
    return JRect{ x0, y0, x1 - x0, y1 - y0 };
}

std::vector<int> Surface::unitOf(int id) const {
    const PanelElement* e = model_ ? model_->get(id) : nullptr;
    if (!e || !e->groupId) return { id };
    std::vector<int> members;
    for (const PanelElement& o : model_->elements()) if (o.groupId == e->groupId) members.push_back(o.id);
    return members;
}

void Surface::alignSelection(AlignMode mode) {
    if (!model_ || selection_.size() < 2) return;
    // Align to the REFERENCE widget's edges, not to the selection's bounding box. Aligning left used to
    // walk everything to the leftmost edge in the selection, which is an emergent property of where the
    // widgets happen to sit; now it lands on the edge of the one you pointed at.
    const PanelElement* ref = arrangeRef();
    if (!ref) return;
    // The reference is the unit you pointed at, not the bare element: point at one member of a group and
    // you mean the group's edge, because that is the thing you can see a box around.
    const JRect r = unitBox(unitOf(ref->id));
    const float minX = r.x, maxR = r.x + r.width;
    const float minY = r.y, maxB = r.y + r.height;
    const float cX = r.x + r.width * 0.5f, cY = r.y + r.height * 0.5f;
    for (const std::vector<int>& unit : arrangeUnits()) {
        const JRect b = unitBox(unit);
        float x = b.x, y = b.y;
        switch (mode) {
            case AlignMode::Left:    x = minX; break;
            case AlignMode::Right:   x = maxR - b.width; break;
            case AlignMode::Top:     y = minY; break;
            case AlignMode::Bottom:  y = maxB - b.height; break;
            case AlignMode::CenterH: x = cX - b.width * 0.5f; break;
            case AlignMode::CenterV: y = cY - b.height * 0.5f; break;
            case AlignMode::Center:  x = cX - b.width * 0.5f; y = cY - b.height * 0.5f; break;
        }
        // Translate the WHOLE unit by one delta, so the members keep their relative layout.
        const float dx = x - b.x, dy = y - b.y;
        if (dx == 0.0f && dy == 0.0f) continue;
        for (int id : unit) if (PanelElement* e = model_->get(id))
            model_->setRect(id, e->x + dx, e->y + dy, e->w, e->h);
    }
    invalidate();
}
void Surface::distributeSelection(bool horizontal) {
    if (!model_) return;
    // Distribute UNITS, not elements: a group is one thing to space out, and spacing its members
    // individually would tear it apart exactly the way aligning them did.
    std::vector<std::vector<int>> units = arrangeUnits();
    if (units.size() < 3) return;
    std::sort(units.begin(), units.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
        const JRect ra = unitBox(a), rb = unitBox(b);
        return horizontal ? ra.x < rb.x : ra.y < rb.y;
    });
    float lo = 1e9f, hi = -1e9f, totalExtent = 0;
    for (const auto& u : units) {
        const JRect r = unitBox(u);
        lo = std::min(lo, horizontal ? r.x : r.y);
        hi = std::max(hi, horizontal ? r.x + r.width : r.y + r.height);
        totalExtent += horizontal ? r.width : r.height;
    }
    const float gap = ((hi - lo) - totalExtent) / (units.size() - 1);
    float cur = lo;
    for (const auto& u : units) {
        const JRect r = unitBox(u);
        const float dx = horizontal ? cur - r.x : 0.0f;
        const float dy = horizontal ? 0.0f : cur - r.y;
        if (dx != 0.0f || dy != 0.0f)
            for (int id : u) if (PanelElement* e = model_->get(id))
                model_->setRect(id, e->x + dx, e->y + dy, e->w, e->h);
        cur += (horizontal ? r.width : r.height) + gap;
    }
    invalidate();
}
void Surface::armSpacing(bool horizontal, bool pitch) {
    if (!model_ || selection_.size() < 2) return;
    spacingAxis_  = horizontal ? 2 : 1;
    spacingPitch_ = pitch;
    spacingBuf_.clear();
    // NB: we do NOT requestFocus() here. armSpacing runs inside the context-menu item's callback, and an
    // open menu is its own popup window with its OWN JFocusManager as s_active — so a focus request here
    // lands on the doomed popup manager, not the main window's. The keyboard grab is done in the paint pass
    // (populateRenderPrimitives) instead, which runs in the main-window focus context once the menu is gone.
    invalidate();                       // the on-canvas prompt appears; keys are captured until Enter/Esc
}

// Lay the selection out along one axis at an explicit step. Sorted along the axis and anchored on the
// reference widget, so that one keeps its position and only the spacing changes.
void Surface::applySpacing(bool horizontal, bool pitch, float value) {
    if (!model_) return;
    // Space UNITS, not elements — a group lays out as one item and keeps its internal geometry.
    std::vector<std::vector<int>> units = arrangeUnits();
    if (units.size() < 2) return;
    std::sort(units.begin(), units.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
        const JRect ra = unitBox(a), rb = unitBox(b);
        return horizontal ? ra.x < rb.x : ra.y < rb.y;
    });
    // The run is anchored on the REFERENCE unit — the one you right-clicked stays exactly where it is
    // and the rest lay out around it, in both directions. Anchoring on the spatially-first element instead
    // meant the widget you pointed at was usually the one that moved.
    const PanelElement* ref = arrangeRef();
    if (!ref) return;
    size_t r = 0;
    for (size_t i = 0; i < units.size(); ++i)
        if (std::find(units[i].begin(), units[i].end(), ref->id) != units[i].end()) { r = i; break; }
    const JRect refBox = unitBox(units[r]);
    const float anchor = horizontal ? refBox.x : refBox.y;

    auto place = [&](const std::vector<int>& unit, float pos) {
        const JRect b = unitBox(unit);
        const float dx = horizontal ? pos - b.x : 0.0f;
        const float dy = horizontal ? 0.0f : pos - b.y;
        if (dx == 0.0f && dy == 0.0f) return;
        for (int id : unit) if (PanelElement* e = model_->get(id))
            model_->setRect(id, e->x + dx, e->y + dy, e->w, e->h);
    };
    // Pitch steps origin-to-origin (independent of each unit's size); gap adds the step to the unit's own
    // extent, so mixed-size controls still end up with even air between them.
    auto extentOf = [&](const std::vector<int>& unit) {
        const JRect b = unitBox(unit); return horizontal ? b.width : b.height;
    };
    float cur = anchor;                                   // forward from the anchor
    for (size_t i = r; i < units.size(); ++i) {
        place(units[i], cur);
        cur += pitch ? value : (extentOf(units[i]) + value);
    }
    cur = anchor;                                         // and backward from it
    for (size_t i = r; i-- > 0; ) {
        cur -= pitch ? value : (extentOf(units[i]) + value);
        place(units[i], cur);
    }
    invalidate();
}

void Surface::matchSize(SizeMode mode) {
    // Belt and braces: the menu item is disabled with a group selected, but matchSize is reachable from a
    // shortcut too, and silently reshaping someone's grouped layout is not a thing to leave to the menu.
    if (selectionHasGroup()) return;
    if (!model_ || selection_.size() < 2) return;
    const PanelElement* ref = arrangeRef();   // the widget you right-clicked supplies the size
    if (!ref) return;
    const float rw = ref->w, rh = ref->h;
    for (int id : selection_) if (PanelElement* e = model_->get(id)) {
        const float w = (mode == SizeMode::Width  || mode == SizeMode::Both) ? rw : e->w;
        const float h = (mode == SizeMode::Height || mode == SizeMode::Both) ? rh : e->h;
        model_->setRect(id, e->x, e->y, w, h);
    }
    invalidate();
}

// --- Clipboard --------------------------------------------------------------------------------
void Surface::copySelection() {
    if (!model_ || selection_.empty()) return;
    clipboard_.clear();
    pasteSeq_ = 0;                 // a fresh copy pastes at +20 again, not wherever the last run got to
    for (int id : selection_) if (const PanelElement* e = model_->get(id)) clipboard_.push_back(*e);
}
void Surface::cutSelection() { copySelection(); deleteSelection(); }
// After a paste/duplicate mints fresh uids, relink INTERNAL @-sigil references so a copied group keeps its
// wiring: a gauge bound to @<tableUid>.max, copied together WITH that table, follows the copy instead of
// still pointing at the original. Rewrites "@<old>" -> "@<new>" for every uid that is itself in the pasted
// set; a reference whose target was NOT copied is left untouched (an intentional external link). Matches the
// sigil form only ("@"+uid), never a bare uid, so a viewport's page/node key is not disturbed.
static void relinkPastedRefs(PanelModel* m, const std::vector<int>& newIds,
                             const std::unordered_map<std::string, std::string>& uidMap) {
    if (!m || uidMap.empty()) return;
    for (int id : newIds) {
        PanelElement* e = m->get(id);
        if (!e) continue;
        std::vector<std::pair<std::string, std::string>> changes;
        for (const auto& [k, v] : e->props) {
            std::string nv = v; bool hit = false;
            for (const auto& [oldU, newU] : uidMap) {
                const std::string needle = "@" + oldU, repl = "@" + newU;
                for (size_t pos = 0; (pos = nv.find(needle, pos)) != std::string::npos; pos += repl.size()) {
                    nv.replace(pos, needle.size(), repl); hit = true;
                }
            }
            if (hit) changes.emplace_back(k, nv);
        }
        for (auto& [k, v] : changes) m->setProp(id, k, v);   // deferred: don't mutate props mid-iteration
    }
}

void Surface::pasteClipboard() {
    if (!model_ || clipboard_.empty()) return;
    selection_.clear();
    // Each paste steps further from the source. The offset was a constant +20 from the CLIPBOARD's own
    // coordinates, so pasting twice put both copies in exactly the same place — the second hidden under
    // the first, and a drag then moved only the one on top.
    const float off = 20.f * static_cast<float>(++pasteSeq_);
    // A pasted copy is a NEW group, not a member of the source's. Reusing src.groupId made the copies
    // group-mates of the widgets they were copied FROM: clicking a copy selected the originals too
    // (expandToGroups), and ungrouping a copy dissolved the original's grouping with it. Remapped per
    // distinct source group, so pasting several groups at once keeps them separate rather than merging.
    std::unordered_map<int, int> gmap;
    std::unordered_map<std::string, std::string> uidMap;   // old uid -> new uid, for relinking @-references
    for (const PanelElement& src : clipboard_) {
        // A pasted VIEWPORT is a NEW VIEWPORT ON THE SELECTED NODE — exactly what dragging that node out
        // of the tree makes — at the same size and position as the one copied. Its canvas IS that node's
        // page; there is no private canvas and no "page" prop. Then the source page's widgets are
        // duplicated INTO that page as new widgets. Nothing else is carried over.
        // …but ONLY at the top level. INSIDE a page (scoped in), activeNode_ IS the page you are editing,
        // so re-pointing the copy at it produced a viewport of the very page it sits on — and then copied
        // the source page's widgets INTO the page being edited. Paste a viewport in there and you got your
        // controls again plus a self-referencing viewport, per pasted viewport. Nested, it is a plain copy:
        // same page as the one it came from (a mirror), nothing duplicated.
        const bool viewportToNode = (src.type == "viewport" && pageResolver_ && !activeNode_.empty() && !inScope());
        const float dx = viewportToNode ? 0.f : off;   // same place as the original, per the spec
        const int id = model_->add(src.type, src.x + dx, src.y + dx, src.w, src.h, src.props);
        if (const PanelElement* ne = model_->get(id); ne && !src.uid.empty())
            uidMap[src.uid] = ne->uid;
        for (const auto& [o, n] : model_->lastAddChildUids()) uidMap[o] = n;   // ...and the copy's children

        if (viewportToNode) {
            model_->setProp(id, "node", activeNode_);   // the node you are on — like a tree drag
            model_->setProp(id, "page", "");            // its canvas is that node's page, nothing private
            // …and titled after that node, exactly as a tree drag titles one. Keeping the copied title left
            // a viewport on Battery Voltage captioned with the node it came from.
            const size_t slash = activeNode_.rfind('/');
            model_->setProp(id, "title", slash == std::string::npos ? activeNode_ : activeNode_.substr(slash + 1));
            const std::string srcPage = viewportPage(src);
            PanelModel* from = pageResolver_(srcPage);
            PanelModel* to   = pageResolver_(activeNode_);
            if (from && to && from != to) {
                // add() per widget, so each gets a fresh id AND uid in the destination page — copying the
                // element structs wholesale would carry the source's ids and collide with what is there.
                // The page's CANVAS comes across too — size, scaling, layout, guides, title. Copying only
                // the widgets left them laid out on a default-sized canvas, so the copy did not look like
                // the thing it was copied from.
                to->setCanvasState(from->canvasState());
                std::unordered_map<int, int> childGroups;
                std::unordered_map<std::string, std::string> childUids;   // old->new, to relink @-refs within the page
                std::vector<int> childNewIds;
                for (const PanelElement& w : from->elements()) {
                    const int nid = to->add(w.type, w.x, w.y, w.w, w.h, w.props);
                    childNewIds.push_back(nid);
                    if (const PanelElement* nw = to->get(nid); nw && !w.uid.empty())
                        childUids[w.uid] = nw->uid;
                    // add() re-mints the uids INSIDE a panel too; taking its map here is what lets a
                    // reference from elsewhere on the page into that panel's children follow the copy.
                    for (const auto& [o, n] : to->lastAddChildUids()) childUids[o] = n;
                    if (w.groupId) {
                        auto g = childGroups.find(w.groupId);
                        if (g == childGroups.end()) g = childGroups.emplace(w.groupId, to->nextGroupId()).first;
                        to->setGroupId(nid, g->second);
                    }
                }
                relinkPastedRefs(to, childNewIds, childUids);   // a gauge that referenced the page's table now follows the copy
                if (onPageChanged_) onPageChanged_();
            }
        }
        if (src.groupId) {
            auto it = gmap.find(src.groupId);
            if (it == gmap.end()) it = gmap.emplace(src.groupId, model_->nextGroupId()).first;
            model_->setGroupId(id, it->second);   // assigned before the next nextGroupId() so ids don't collide
        }
        selection_.push_back(id);
    }
    relinkPastedRefs(model_, selection_, uidMap);   // copied-together links follow the copy; external ones stay
    selectionChanged.emit(); invalidate();
}
void Surface::deleteSelection() {
    if (!model_ || selection_.empty()) return;
    std::vector<std::string> viewportNodes;   // node pages whose placement we're removing (GC candidates)
    for (int id : selection_) {
        if (const PanelElement* e = model_->get(id); e && e->type == "viewport" && !e->prop("node").empty())
            viewportNodes.push_back(viewportPage(*e));
        model_->remove(id);
    }
    selection_.clear(); selectionChanged.emit(); invalidate();
    // After removal: free any node page whose LAST viewport just went.
    if (onViewportRemoved) for (const auto& n : viewportNodes) onViewportRemoved(n);
}

// Widgets whose OWN text IS their content (button text, the Label widget, indicator title, viewport
// title) don't get a separate caption Label placed beside them.
//
// A caption-editing widget answers for itself, so a Label and anything built on one — a Hyperlink — is
// covered without being named here. The rest are types whose text is theirs for other reasons.
static bool typeWantsCaption(const std::string& type) {
    if (widgetEditsCaption(type)) return false;
    return type != "command" && type != "indicator" && type != "viewport" && type != "panel";
}

// Place a new control's caption + units as SEPARATE Label elements (grouped with the control) per the
// widget type's placement prefs. This replaces widgets drawing their own captions.
void Surface::placeLabelsFor(int controlId, const std::string& type, const std::string& caption) {
    if (!model_) return;
    const PanelElement* ctl = model_->get(controlId);
    if (!ctl) return;
    const float cx = ctl->x, cy = ctl->y, cw = ctl->w, ch = ctl->h;
    const int gid = model_->nextGroupId();
    model_->setGroupId(controlId, gid);
    // A dropped label renders at the LABEL TYPE'S OWN font — the Widget Default that resolveElement
    // layers in — not at the app font. Measuring with JTextHelper (the app font) and a hardcoded 18px
    // height sized the box for one font and then painted it in another: with a small app font and a
    // larger label font, the text overflowed the box it was given at birth. Same measurement the inline
    // caption editor already uses when it re-fits a label (naturalSize, with the font spec).
    // THE CAPTION IS A LABEL, SO IT IS BORN LIKE ONE. A dropped CONTROL starts from its type's Preferences
    // ▸ Widget Defaults (see addWidgetAt); its caption started from nothing but its text, so a Label default
    // the user had set — a font, an alignment, a colour, a size — reached every label on the page except the
    // ones the studio placed itself.
    EditorSettings& esL = EditorSettings::instance();
    const auto labelDefs = esL.widgetDefaultProps("label");
    std::string labelFont;
    if (const auto it = labelDefs.find("fontName"); it != labelDefs.end()) labelFont = it->second;
    int ldw = 0, ldh = 0;
    const bool haveLabelSize = esL.widgetDefaultSize("label", ldw, ldh);
    auto place = [&](const std::string& text, const EditorSettings::LabelPlacement& p) {
        if (text.empty()) return;
        float lw = 0.f, lh = 0.f;
        LabelWidget::naturalSize(text, labelFont, 0.0, lw, lh);
        // A default SIZE set for the Label type wins over the measured one, exactly as it does for a
        // dropped control — but never below what the text needs, or the caption the studio placed would be
        // the one label on the page with its own words clipped.
        if (haveLabelSize) { lw = std::max(lw, static_cast<float>(ldw)); lh = std::max(lh, static_cast<float>(ldh)); }
        lw = std::max(20.f, lw);
        float lx = cx, ly = cy;
        if      (p.side == "Above") ly = cy - lh - p.gap;
        else if (p.side == "Below") ly = cy + ch + p.gap;
        else if (p.side == "Left")  { lx = cx - lw - p.gap; ly = cy + (ch - lh) * 0.5f; }
        else                        { lx = cx + cw + p.gap; ly = cy + (ch - lh) * 0.5f; }   // Right
        std::unordered_map<std::string, std::string> lp = labelDefs;   // the Label type's own defaults…
        lp["labelText"] = text;                                        // …and the one thing this caption is
        // The caption explains the control, so it must answer the same question on hover. A checkbox is a
        // 13px box: asking the user to find it with the pointer to read what the setting does, while the
        // words next to it say nothing, is backwards.
        //
        // The text is NOT copied in here. A label carries no binding, so a copy is a snapshot of the
        // definition as it read on the day the page was drawn — and a field whose help was written later
        // left its caption permanently blank, with nothing on the page to say why. The caption is GROUPED
        // with its control, and CanvasWidget resolves help through that group every frame instead
        // (setHelpBind), so the definition stays the live source. The "tooltip" prop is left for what it
        // was always for: an author's own words, which still beat the definition's.
        const int lid = model_->add("label", lx, ly, lw, lh, std::move(lp));
        model_->setGroupId(lid, gid);
    };
    // ONLY THE CAPTION. A widget shows its own unit (CanvasWidget::fmtVal), so a second label naming it
    // would be a copy of something the widget already says — and a copy that has to be kept in step with a
    // preference, moved with the control, and deleted with it.
    place(caption, EditorSettings::instance().captionPlacement(type));
}

void Surface::addWidgetAt(const std::string& type, const std::string& caption) {
    if (mode_ != Mode::Edit || !model_) return;
    // Creation cascade (WidgetFactory parity): size + props from Preferences ▸ Widget Defaults, else the
    // widget class's built-in default size.
    EditorSettings& es = EditorSettings::instance();
    float w = 160.f, h = 90.f;
    widgetDefaultSize(type, w, h);   // widget-class default (unknown type → leaves 160×90)
    int dw = 0, dh = 0;
    if (es.widgetDefaultSize(type, dw, dh)) { w = static_cast<float>(dw); h = static_cast<float>(dh); }
    std::unordered_map<std::string, std::string> props = es.widgetDefaultProps(type);
    const Xform t = xform();
    const float s = (t.scale > 0.f ? t.scale : 1.f);
    const float wx = (hoverX_ - t.viewX - t.offX) / s, wy = (hoverY_ - t.viewY - t.offY) / s;
    const int id = model_->add(type, wx - w * 0.5f, wy - h * 0.5f, w, h, std::move(props));
    // Place the caption (+ units) as separate grouped Label elements at drop — not drawn by the control.
    if (typeWantsCaption(type)) {
        const std::string title = widgetTitle(type);
        placeLabelsFor(id, type, caption.empty() ? (title.empty() ? type : title) : caption);
    }
    selectOnly(id); expandToGroups(); selectionChanged.emit(); invalidate();
}

// The control type best suited to a binding dropped on empty canvas: a table map → Table, a 1D array/curve
// → Array 1D, anything else (scalar / live channel) → a Value readout.
std::string Surface::defaultControlFor(const std::string& bindPath) const {
    // The binding's classification mapped to a REGISTERED widget type, so the dropped control matches its
    // dictionary glyph and actually renders. Cache owns the rule; an ini import applies the same one.
    return cache_.widgetTypeFor(bindPath);
}

void Surface::setMode(Mode m) {
    if (m == mode_) return;
    if (inlineEdit_) commitLabelEdit_();   // never leave a caption open across a mode switch
    JLOGC("surface", jf::JLogLevel::Info) << "[mode] " << (m == Mode::Edit ? "EDIT" : "RUN")
                                          << " (was " << (mode_ == Mode::Edit ? "EDIT" : "RUN") << ")";
    mode_ = m;
    if (mode_ == Mode::Run) {
        // Remember the chain before unwinding it — outermost first, which is the order it has to be
        // re-entered in. Each scope's element lives in its PARENT's model, which is what the scope below
        // it holds; the deepest one's parent is scopes_.back().
        suspended_.clear();
        for (const EditScope& s : scopes_)
            suspended_.push_back({ s.srcElemId, s.panelElemId != 0 });
        while (inScope()) exitScope();                        // in-place viewport editing is edit-mode only
        if (!selection_.empty()) { selection_.clear(); selectionChanged.emit(); } drag_ = Drag::None;
    } else {
        // …and back in. Anything that has since gone — the page was edited elsewhere, the viewport deleted —
        // simply stops the replay: it leaves you one level out rather than somewhere that no longer exists.
        const std::vector<SuspendedScope> chain = std::move(suspended_);
        suspended_.clear();
        for (const SuspendedScope& s : chain) {
            if (s.elemId == 0) break;
            const size_t depth = scopes_.size();
            if (s.panel) enterPanel(s.elemId); else enterViewport(s.elemId);
            if (scopes_.size() == depth) break;               // refused (element gone / wrong type)
        }
    }
    setActiveControl_(0); runDrag_ = false;                   // drop any run-mode interactive focus (commits its edit)
    setContextMenu(mode_ == Mode::Edit ? &menu_ : nullptr);   // run mode has no context menu
    invalidate();
}

void Surface::setActiveNode(const std::string& path) {
    if (path == activeNode_) return;
    // Selecting a different node means we've left whatever viewport/panel was being edited in place — pop out
    // of any drilled-in scope first (serialising its edits), so model_ is the top-level page again and the
    // studio never stays bound to the old node's page while the tree points elsewhere.
    while (inScope()) exitScope();
    activeNode_ = path;
    // PUBLISHED, because an unbound viewport shows whatever this is — on every surface at once, which
    // is what lets one viewport per surface stand in for a stack of one-per-page. Set here, where the
    // selection actually changes, so there is a single writer.
    // Switching the scoped node changes which viewports/controls are shown, so any current widget
    // selection no longer makes sense — clear it (and tell the inspector).
    if (!selection_.empty()) { selection_.clear(); selectionChanged.emit(); }
    invalidate();   // render re-evaluates node-tagged element visibility next frame
}

// --- Derived transform -------------------------------------------------------------------------

// Gather the surface's live state into the pure SurfaceCamera. The breadcrumb inset, effective canvas size,
// fixed-vs-fit + global anchor, managed-layout params, pan and title inset all come from here; the transform
// math itself lives on SurfaceCamera (one source of truth for paint AND hit-test).
SurfaceCamera Surface::camera() const {
    SurfaceCamera c;
    const auto bb = getBoundingBox();
    c.viewport = JRect{ bb.x, bb.y, bb.width, bb.height };
    // While editing an entered viewport in place, a breadcrumb bar occupies the top strip (drawn at b.y,
    // height lineHeight()+8 — kept in lockstep with the breadcrumb render below). Reserve it so the page
    // origin (0,0) sits BELOW the breadcrumb and controls placed at the very top aren't hidden behind it.
    c.crumbH = (inScope() && jf::JTextHelper::hasAtlas()) ? (jf::JTextHelper::lineHeight() + 8.f) : 0.f;
    if (model_) {
        EditorSettings& es = EditorSettings::instance();
        // Effective canvas size: 0 on either axis = inherit the global surface size (Preferences ▸ Surface).
        c.canvasW = model_->effCanvasW();
        c.canvasH = model_->effCanvasH();
        // Scaling: canvasStatic 1 = fixed 1:1; 2 = scale-to-fit; 0 = inherit the global staticSurfaceSize.
        const int cs = model_->canvasStatic();
        // REFLOW: the page is the view. No authored canvas, so nothing to fit or squeeze — it is drawn at
        // the interface scale into whatever area it has, and its layout does the arranging.
        //
        // …WHEN IT IS BEING READ. Editing is the other half of the same rule: a page whose extent is
        // "whatever the window is" is not a page anyone can author against. Drag a widget to x=400 on a
        // rubber canvas and it lands somewhere different in every window, and there is nothing on screen
        // saying where the page runs out. So in EDIT the page pins to its authored canvas — 1:1 at the
        // interface scale, scrolling if the window is smaller — and the card drawn round it (see
        // populateRenderPrimitives) shows exactly the extent the coordinates are in.
        //
        // Run reflows, edit is fixed. That is also what gives the canvas-size preference something to
        // mean: it is the canvas you AUTHOR against, not the size pages are shown at.
        const bool editingNow = (mode_ == Mode::Edit);
        reflow_ = (cs == 3) && !editingNow;
        c.fixed = reflow_ || (cs == 1) || (cs == 3) || (cs == 0 && es.staticSurfaceSize());
        // …and WHAT SCALE a static surface renders that page at: the Interface scale, the same number
        // that sizes every control and glyph in the app. A page's widget positions are authored units
        // that the font scale cannot reach, so without this a scaled-up interface would grow a page's
        // text inside boxes that stayed the size they were authored at.
        c.staticScale = jf::JStyle::uiScale();
        // A SQUEEZE STOPS BEING A FIT at some point. When the user asked for fit-to-window, floor it at
        // something still readable and let the surface scroll below that; a deliberate fit elsewhere
        // (the difference report) sets no floor and is left alone.
        c.minScale = fitToView_ ? 0.f : c.staticScale * SurfaceCamera::kMinReadableScale;
        if (reflow_) {
            // The canvas IS the viewport, expressed in the page's own units so scale * canvas == the area
            // it was given. Everything downstream — page rect, hit-test, scroll bars — keeps working off
            // canvasW/H exactly as it does for an authored page.
            const float sc = c.staticScale > 0.f ? c.staticScale : 1.f;
            // …DOWN TO THE WIDTH THE PAGE NEEDS, AND NO FURTHER. Reflow means the canvas is the viewport,
            // which is right while there is room and wrong the moment there is not: a window narrower than
            // the page kept handing the layout less to work with, so the rows compressed until the captions
            // were clipped mid-word and the controls were slivers - a page squeezed into mush rather than
            // a page you have to scroll. The authored canvas is the floor: it is the width the page was
            // built to fit in, so below it the canvas stops shrinking and the surface scrolls instead.
            // Above it nothing changes - the page still reflows into whatever room it is given.
            c.canvasW = std::max(model_->minW(), std::max(1.f, c.viewport.width / sc));
            // The WIDTH is the view; the HEIGHT is the view or the content, whichever is larger. Making
            // both the view meant the page could never overflow, so it never scrolled and everything past
            // the bottom was quietly clipped — which is not "it fits".
            const float viewH = std::max(1.f, (c.viewport.height - c.crumbH) / sc);
            c.canvasH = std::max(viewH, contentHeightOf(model_->elements(), model_->layout()));
            c.minScale = 0.f;   // no scale floor: a reflow page is drawn 1:1 and scrolled, never shrunk
        }
        // …unless the VIEW insists the whole page be visible. A fixed canvas scrolls when its area is
        // too small, which is right for a tab you can scroll and wrong for the difference report: a
        // setting scrolled off the edge there is a difference the reader never sees and cannot reach,
        // because the report deliberately takes no mouse. See setFitToView().
        if (fitToView_) c.fixed = false;
        // Anchoring: the canvas's own choice, else the global default. One global setting could not say
        // "centre my pages but pin this viewport's little canvas top-left"; a purely local one would
        // throw away the convenience of setting it once. Same inherit shape as canvasStatic above.
        // TOP-LEFT unless the canvas asks for something else. A surface fills its area, so there is no
        // leftover space to align it in; the global "surface anchor" preference it used to consult is gone
        // with the fixed-size card it existed for. A canvas that still states one is still honoured.
        const int ca = model_->canvasAnchor();
        c.anchor = (ca >= 0) ? ca : 0;
        c.layoutMode  = model_->layout();
        c.gridColumns = model_->gridColumns();
        c.focusIndex  = model_->focusIndex();
        const auto [ti, bi] = titleInset();
        c.titleTop = ti; c.titleBottom = bi;
    }
    c.scrollX = scrollX_; c.scrollY = scrollY_;
    c.scrollBarW = kSB;
    return c;
}

Surface::Xform Surface::xform() const {
    // !model_: only the view origin (+ breadcrumb) is meaningful — scale 1, no offset, zero canvas. Keep the
    // exact early-out (a full camera with a zero canvas would apply a bogus anchor offset).
    if (!model_) {
        const auto b = getBoundingBox();
        const float crumbH = (inScope() && jf::JTextHelper::hasAtlas()) ? (jf::JTextHelper::lineHeight() + 8.f) : 0.f;
        Xform t; t.viewX = b.x; t.viewY = b.y + crumbH; return t;
    }
    return camera().xform();
}

JRect Surface::pageRect(const Xform& t) const { return SurfaceCamera::pageRect(t); }
JRect Surface::toScreen(const PanelElement& e, const Xform& t) const {
    return SurfaceCamera::toScreen(JRect{ e.x, e.y, e.w, e.h }, t);
}

// Scroll-bar geometry, present only when the effective canvas overflows the viewport (a fixed / static
// surface bigger than its area — a scaled-to-fit surface never overflows). Vertical bar hugs the right
// edge, horizontal the bottom; each reserves the far corner for the other so they never overlap.
Surface::ScrollBars Surface::scrollBars(const Xform& t) const { return camera().scrollBars(t); }

// Map an in-progress thumb drag to scrollX_/scrollY_ (the cursor keeps its grab offset within the thumb).
void Surface::dragScrollThumb(float mx, float my) {
    const Xform t = xform();
    const ScrollBars sb = scrollBars(t);
    const auto b = getBoundingBox();
    if (drag_ == Drag::ScrollV && sb.hasV) {
        const float overflow = t.ch * t.scale - b.height, travel = sb.vTrack.height - sb.vThumb.height;
        if (travel > 0.f && overflow > 0.f)
            scrollY_ = -std::clamp((my - scrollGrab_ - sb.vTrack.y) / travel, 0.f, 1.f) * overflow;
    } else if (drag_ == Drag::ScrollH && sb.hasH) {
        const float overflow = t.cw * t.scale - b.width, travel = sb.hTrack.width - sb.hThumb.width;
        if (travel > 0.f && overflow > 0.f)
            scrollX_ = -std::clamp((mx - scrollGrab_ - sb.hTrack.x) / travel, 0.f, 1.f) * overflow;
    }
    invalidate();
}

// Wheel: in run mode the interactive control under the cursor gets first refusal (3D-table height, etc.);
// otherwise pan the overflowing canvas — vertically if it overflows, else horizontally.
bool Surface::handleScroll(float mx, float my, float wheel) {
    if (!model_) return false;
    if (mode_ == Mode::Run && routeRunInput_(ControlInput::Kind::Scroll, mx, my, wheel, nullptr)) return true;
    const Xform t = xform();
    const auto b = getBoundingBox();
    const float step = 48.f;
    if (t.ch * t.scale > b.height + 0.5f)     { scrollY_ = std::clamp(scrollY_ + wheel * step, b.height - t.ch * t.scale, 0.f); invalidate(); return true; }
    if (t.cw * t.scale > b.width  + 0.5f)     { scrollX_ = std::clamp(scrollX_ + wheel * step, b.width  - t.cw * t.scale, 0.f); invalidate(); return true; }
    return false;
}

// The element's on-screen rect, honouring the canvas Layout mode. Free = its own rect (toScreen); the managed
// modes arrange children within the page through the ONE shared algorithm (layoutChildRect), reused by every
// PanelWidget for its owned children. Border strips scale by t.scale on both axes (canvas zoom is uniform).
// Content inset (top, bottom) reserved for an inside/outside title bar, so MANAGED layouts don't overlap the
// title. Free mode positions children freely (letterbox only, no title inset), so this is applied to managed modes only.
std::pair<float, float> Surface::titleInset() const {
    if (!model_ || !showPageTitle_) return { 0.f, 0.f };   // the host shows the name; reclaim the band
    const std::string& t = model_->title();
    if (t.empty() || t == ".") return { 0.f, 0.f };
    const float lh   = jf::JTextHelper::lineHeight();
    const float band = (model_->titlePlace() == 1) ? (lh * 0.5f + model_->titlePadding())   // Outside legend
                                                   : (lh + 2.f * model_->titlePadding());    // Inside bar (th)
    return model_->titleEdge() == 1 ? std::make_pair(0.f, band) : std::make_pair(band, 0.f); // 0=Top 1=Bottom
}

// ANCHORS: what a widget does with room the page was not authored for.
//
// A reflow page's canvas IS the window (floored at the authored width), so it grows — and in Free layout
// every widget kept the rect it was drawn at, so the space appeared and nothing claimed it: a maximised
// Target Lambda left five hundred pixels of empty tab beside a table that was scrolling.
//
// The slack is canvas MINUS the width the page was authored for, so it is zero at the compact size and
// below it — where the canvas stops shrinking and the surface scrolls instead. That is what makes this
// safe to apply everywhere: the authored layout IS the minimum layout, and there is no second geometry
// to keep in step.
//
//   (default) left/top : stays where it was drawn
//   right/bottom       : moves with the far edge
//   both               : STRETCHES by the whole difference
//   centre             : keeps its middle in the middle
static float anchorPos(const std::string& how, float pos, float size, float slack, float& outSize) {
    outSize = size;
    if (slack <= 0.f || how.empty() || how == "left" || how == "top") return pos;
    if (how == "both")   { outSize = size + slack; return pos; }
    if (how == "right" || how == "bottom") return pos + slack;
    if (how == "centre" || how == "center") return pos + slack * 0.5f;
    return pos;
}

JRect Surface::screenRectOf(size_t index, const Xform& t) const {
    const auto& els = model_->elements();
    if (model_->layout() == 0 || els.empty()) {                                // Free (fast path)
        const PanelElement& e = els[index];
        const std::string ax = e.prop("anchorX"), ay = e.prop("anchorY");
        if (ax.empty() && ay.empty()) return toScreen(e, t);                   // the common case, untouched
        // The EFFECTIVE canvas (the camera's, after reflow) against what the page was authored to. minW
        // is the reflow floor — the authored width — and the authored height is the model's own canvas.
        const SurfaceCamera cam = camera();
        const float baseW = model_->minW() > 0.f ? model_->minW() : model_->canvasW();
        const float slackX = std::max(0.f, cam.canvasW - baseW);
        const float slackY = std::max(0.f, cam.canvasH - model_->canvasH());
        float w = e.w, h = e.h;
        const float x = anchorPos(ax, e.x, e.w, slackX, w);
        const float y = anchorPos(ay, e.y, e.h, slackY, h);
        return SurfaceCamera::toScreen(JRect{ x, y, w, h }, t);
    }
    std::vector<LayoutChild> children; children.reserve(els.size());            // managed: SurfaceCamera lays it out
    // Height from the STYLE when the document does not state one — see StyleMetrics.h. A control's
    // height is a property of its kind, not of whichever generator wrote the page.
    for (const auto& e : els)
        children.push_back({ JRect{ e.x, e.y, e.w, layoutHeightOf(e.type, e.h) }, e.prop("region") });
    return camera().screenRectOf(t, children, static_cast<int>(index));
}

// --- Selection ---------------------------------------------------------------------------------

bool Surface::isSelected(int id) const { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); }
void Surface::selectOnly(int id) { selection_.clear(); if (id) selection_.push_back(id); }

int Surface::hitTest(float mx, float my, const Xform& t) const {
    if (!model_) return 0;
    const auto& els = model_->elements();
    for (int i = static_cast<int>(els.size()) - 1; i >= 0; --i) {   // topmost first
        // Honour the view-switcher exactly like the render: an element tagged for another node is invisible,
        // so it must not be a hit target — otherwise a hidden viewport intercepts clicks on the visible one.
        if (nodeHidden_(els[i])) continue;
        const JRect r = screenRectOf(i, t);
        if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) return els[i].id;
    }
    return 0;
}

void Surface::selectAlso(int id) {
    if (!model_ || !id || isSelected(id)) return;
    selection_.push_back(id);
    expandToGroups();               // a group joins as a whole, exactly as clicking a member does
}

// The "viewport" element under the cursor (a window onto a tree node's page), else 0. Honours the
// view-switcher: a viewport gated out by the active-node filter isn't a target.
int Surface::viewportElAt(float mx, float my) const {
    if (!model_) return 0;
    const int hit = hitTest(mx, my, xform());
    if (!hit) return 0;
    const PanelElement* el = model_->get(hit);
    // An UNBOUND viewport has no page of its own, so testing viewportPage() alone rejected exactly the
    // viewport under the cursor and the double-click never reached enterViewport(). What matters here is
    // that SOMETHING would open — the same question enterViewport answers.
    if (!el || el->type != "viewport") return 0;
    if (viewportPage(*el).empty() && activeNode_.empty()) return 0;
    if (nodeHidden_(*el)) return 0;   // a hidden viewport isn't clickable
    return hit;
}

// Drill into a viewport: rebind model_ to its node's page (from the store) so the whole surface now edits
// that page in place — no tab. Every viewport of the node shares this page, so edits show everywhere live.
// The parent chain is remembered for exitScope(); a breadcrumb + Esc leave.
void Surface::enterViewport(int elemId) {
    if (mode_ != Mode::Edit || !model_ || !pageResolver_) return;
    const PanelElement* el = model_->get(elemId);
    if (!el || el->type != "viewport") return;
    // The PAGE this viewport shows — its private canvas when it has one, else its node's page. Reading
    // "node" here opened the page for the NODE PATH instead, which the resolver creates empty on demand:
    // the viewport went on rendering its real content while drilling in showed a blank canvas, and any
    // widget added there landed in a page nothing displays.
    std::string node = viewportPage(*el);
    if (node.empty()) node = activeNode_;    // unbound: the page this surface is showing
    if (node.empty()) return;
    PanelModel* page = pageResolver_(node);
    if (!page) return;
    scopes_.push_back(EditScope{ model_, node, nullptr, 0, scopeCtx_, elemId });   // remember the parent's subject
    scopeCtx_ = el->prop("signalName");   // …and adopt this viewport's, so its page's "[*]" resolves
    model_ = page; syncedGen_ = ~0ull;
    selection_.clear();
    scrollX_ = scrollY_ = 0.f;
    undo_.clear();                       // per-scope history (the parent's stack is restored on exit)
    selectionChanged.emit();
    invalidate();
}

int Surface::panelElAt(float mx, float my) const {
    if (!model_) return 0;
    const int hit = hitTest(mx, my, xform());
    if (!hit) return 0;
    const PanelElement* el = model_->get(hit);
    return (el && el->type == "panel") ? hit : 0;
}

// Drill into a panel: build a TEMPORARY model from its "children" JSON (child format == widget format) and
// rebind model_ to it, so the panel's children are edited on-canvas as ordinary elements — like the viewport
// path, but authored inline rather than a shared node page. The temp model's canvas is the panel's virtual
// size (fixed 1:1) so child coords map straight through. exitScope() serialises the temp model back into the
// panel's "children" prop. Ports PanelView's "Edit panel in place".
void Surface::enterPanel(int elemId) {
    if (mode_ != Mode::Edit || !model_) return;
    const PanelElement* el = model_->get(elemId);
    if (!el || el->type != "panel") return;
    auto temp = std::make_unique<PanelModel>();
    jf::JJson doc = jf::JJson::object();
    // THE PAGE IS THE PANEL'S CONTENT BOX, not its whole rect. A child's stored x/y are coordinates in the
    // space PanelWidget lays children out in — the rect less its 2px frame and less the title bar
    // (_childBasis / panelContentScale) — so editing them on a page the size of the WHOLE panel put the
    // editor's origin a title-height above the live one. Everything you placed below the title in here came
    // out a title lower again out there, which is the offset by exactly the title height.
    const float lh      = jf::JTextHelper::lineHeight();
    const bool  titled  = !el->prop("labelText").empty();
    const float chromeH = (titled ? lh + 6.f : 0.f) + 4.f;       // == panelContentScale's chrome
    const float pw = (el->w > 4.f)      ? el->w - 4.f      : (el->w > 0.f ? el->w : 320.f);
    const float ph = (el->h > chromeH)  ? el->h - chromeH  : (el->h > 0.f ? el->h : 240.f);
    doc["canvasWidth"]  = static_cast<double>(pw);
    doc["canvasHeight"] = static_cast<double>(ph);
    doc["canvasStatic"] = 1.0;                                   // fixed 1:1 — child rects are panel-virtual coords
    // The title is NOT drawn on this page. The page is the content box, so anything drawn inside it —
    // a bar at the top, or a legend straddling the top edge — sits on the first row of controls, which
    // is what the title band existed to keep clear of. It is named in the breadcrumb instead, which has
    // a strip of its own, and the inspector's Title row still edits it (exitScope writes it back).
    // THE PANEL'S OWN TITLE COMES IN WITH IT. Inside a panel scope the inspector's canvas rows are the
    // panel's, and "Title" is the row a panel HAS — so it must be the panel's title, not a title on a
    // throwaway model. It was the latter: the temp model started blank, took the edit, and exitScope
    // serialised only the children, so the title you had just typed was gone the moment you pressed Esc.
    doc["title"] = el->prop("labelText");
    // AND THE PANEL'S LAYOUT COMES IN WITH IT. Without this the throwaway model defaulted to Free, so
    // entering a Column or a Grid drew its children at their STORED x/y — coordinates a managed layout
    // never uses and no generator bothers to keep meaningful. Every child landed near the same corner:
    // a page's whole feature list overprinted on its vehicle fields, captions on captions, nothing you
    // could point at. Inside the panel it is now arranged the way it is arranged outside it, which is the
    // only view of it worth editing against.
    auto propNum = [&](const char* key, double dflt) {
        const std::string v = el->prop(key);
        if (v.empty()) return dflt;
        try { return std::stod(v); } catch (...) { return dflt; }
    };
    doc["layout"]       = propNum("layoutMode", 0.0);
    doc["gridColumns"]  = propNum("gridColumns", 2.0);
    doc["focusIndex"]   = propNum("focusIndex", 0.0);
    if (auto j = jf::JJson::tryParse(el->prop("children")); j && j->isArray())
        doc["widgets"] = *j;                                     // children array == widgets array
    temp->load(doc);
    scopes_.push_back(EditScope{ model_, "panel", std::move(temp), elemId, scopeCtx_, elemId });
    model_ = scopes_.back().owned.get(); syncedGen_ = ~0ull;
    selection_.clear();
    scrollX_ = scrollY_ = 0.f;
    undo_.clear();
    selectionChanged.emit();
    invalidate();
}

// Serialise ONE panel scope's temporary model back into its panel's `children`. The write half of
// exitScope, split out because LEAVING is not the only moment it has to happen: a save serialises the outer
// page, and until this ran that page's panel still held whatever it held when the scope was entered — so a
// widget added inside a panel and then saved without leaving it went into the file nowhere at all.
void Surface::writeScopeBack_(const EditScope& s) {
    if (s.panelElemId == 0 || !s.model || !s.owned) return;
    // Through PanelModel's own element writer — the child format IS the widget format, and the hand-rolled
    // copy that used to live here dropped groupId (groups made inside a panel did not survive leaving it)
    // and id (the key a sigil ref names).
    jf::JJson arr = jf::JJson::array();
    for (const auto& e : s.owned->elements()) arr.push(PanelModel::elementToJson(e));
    s.model->setProp(s.panelElemId, "children", arr.dump());       // one model edit on the PARENT (marks dirty)
    s.model->setProp(s.panelElemId, "labelText", s.owned->title());  // …and the title, edited as the scope's own
}

// Every open panel scope, INNERMOST FIRST: each writes into its parent's model, so the innermost has to be
// in its parent before that parent is written into ITS parent. Idempotent — exitScope writes the same bytes
// again on the way out, and a scope that has not changed produces the same string.
void Surface::flushScopes() {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) writeScopeBack_(*it);
}

void Surface::exitScope() {
    if (scopes_.empty()) return;
    EditScope& s = scopes_.back();
    scopeCtx_ = s.elemCtx;   // back to the subject in force outside (empty at the top level)
    writeScopeBack_(s);      // panel scope: serialise the temp model back into "children"
    model_ = s.model; syncedGen_ = ~0ull;
    scopes_.pop_back();
    selection_.clear();
    scrollX_ = scrollY_ = 0.f;
    undo_.clear();
    selectionChanged.emit();
    invalidate();
    if (onPageChanged_) onPageChanged_();   // persist the in-place edit
}
bool Surface::inResizeGrip(float mx, float my, const JRect& r) const {
    return mx >= r.x + r.width - kGrip && mx <= r.x + r.width && my >= r.y + r.height - kGrip && my <= r.y + r.height;
}
void Surface::updateMarquee() {
    if (!model_) return;
    const float x0 = std::min(mqX_, mqCurX_), x1 = std::max(mqX_, mqCurX_);
    const float y0 = std::min(mqY_, mqCurY_), y1 = std::max(mqY_, mqCurY_);
    const Xform t = xform();
    selection_.clear();
    for (const auto& e : model_->elements()) {
        // Honour the view-switcher exactly like hitTest and the render: an element tagged for another node
        // is not on screen, so a band drawn over empty canvas must not sweep it up. It did — and since the
        // band cannot show you what it caught, the first sign was a copy that pasted viewports belonging to
        // nodes you were not even looking at.
        if (nodeHidden_(e)) continue;
        const JRect r = toScreen(e, t);
        if (!(r.x > x1 || r.x + r.width < x0 || r.y > y1 || r.y + r.height < y0)) selection_.push_back(e.id);
    }
    expandToGroups();     // band-touching one group-mate selects the whole group
}

// Snap a dragged rect (for a multi-selection, its whole BOUNDING BOX) to the grid and to neighbour edges,
// returning the virtual-space offset to apply to every selected element.
//
// The two snaps are deliberately different in kind:
//   * neighbour magnetism is THRESHOLDED — 8 screen px of pull, and out of range means no pull, because
//     "aligned with nothing" is a legitimate place to be;
//   * grid snap is UNCONDITIONAL — with snap-to-grid on, an edge always lands on a line. It used to share
//     the threshold, which is measured in SCREEN px: at scale-to-fit (scale < 1) the capture zones of
//     adjacent lines overlapped and everything snapped, while at 1:1 with a 20-unit grid a 4-unit dead band
//     opened between them and the same drag simply didn't snap. Whether the feature worked depended on the
//     dock width — which is what made it feel broken rather than merely coarse.
// Both edges are candidates on each axis (not just the top-left corner), so the edge you are pushing at a
// line is the edge that lands on it. A neighbour match inside the threshold wins over the grid: explicit
// alignment to a sibling is a stronger intent than the background lattice.
std::pair<float, float> Surface::snapDelta(float x, float y, float w, float h) const {
    EditorSettings& es = EditorSettings::instance();
    if (!es.snapToGrid() && !es.snapToNeighbours()) return { 0.f, 0.f };
    const float s = xform().scale > 0.f ? xform().scale : 1.f;
    const float thr = 8.f / s;
    const float gv = static_cast<float>(std::max(2, es.gridSize()));
    auto nearestLine = [&](float lo, float hi) {          // pull whichever edge sits closer to a grid line
        const float dLo = std::round(lo / gv) * gv - lo, dHi = std::round(hi / gv) * gv - hi;
        return std::fabs(dLo) <= std::fabs(dHi) ? dLo : dHi;
    };
    auto pick = [&](const std::vector<float>& cs, float fallback) {
        float best = 0.f, ba = thr + 1.f;
        for (float c : cs) if (std::fabs(c) < ba) { ba = std::fabs(c); best = c; }
        return ba <= thr ? best : fallback;
    };
    std::vector<float> xs, ys;
    if (es.snapToNeighbours() && model_) {
        const float meX[3] = { x, x + w, x + w * 0.5f };
        const float meY[3] = { y, y + h, y + h * 0.5f };
        for (const auto& o : model_->elements()) {
            if (isSelected(o.id)) continue;
            const float oX[3] = { o.x, o.x + o.w, o.x + o.w * 0.5f };
            const float oY[3] = { o.y, o.y + o.h, o.y + o.h * 0.5f };
            for (float me : meX) for (float oe : oX) xs.push_back(oe - me);
            for (float me : meY) for (float oe : oY) ys.push_back(oe - me);
        }
    }
    const float gx = es.snapToGrid() ? nearestLine(x, x + w) : 0.f;
    const float gy = es.snapToGrid() ? nearestLine(y, y + h) : 0.f;
    return { pick(xs, gx), pick(ys, gy) };
}

const PanelElement* Surface::origOf(int id) const {
    for (const auto& e : dragStart_) if (e.id == id) return &e;
    return nullptr;
}

// Resize snap: the grip drags the RIGHT and BOTTOM edges, so those are what must land on grid lines —
// snapping the width/height to a multiple of the grid (what this used to do) only aligns the far edge when
// the element's origin is itself already on a line, which after a free-form move it usually isn't. Given the
// fixed origin (x,y), put x+w and y+h on the nearest line. Unconditional, matching the move snap.
void Surface::snapSize(float x, float y, float& w, float& h) const {
    EditorSettings& es = EditorSettings::instance();
    if (!es.snapToGrid()) return;
    const float gv = static_cast<float>(std::max(2, es.gridSize()));
    auto snapEdge = [&](float origin, float extent, float minExtent) {
        const float edge = std::round((origin + extent) / gv) * gv;
        return std::max(minExtent, edge - origin);
    };
    w = snapEdge(x, w, kMinW); h = snapEdge(y, h, kMinH);
}

// Has this press travelled far enough to BE a drag? A press arms Move/Resize immediately (so the drag is
// live the moment the pointer really moves), but until the cursor clears kDragSlop the geometry is left
// alone — a click that only jitters a pixel would otherwise run the snap with a zero delta and shove the
// widget onto the grid just for being selected. Latches: once a drag, always a drag until release.
bool Surface::armDrag_(float mx, float my) {
    if (dragArmed_) return true;
    const float dx = mx - pressMx_, dy = my - pressMy_;
    if (dx * dx + dy * dy < kDragSlop * kDragSlop) return false;
    dragArmed_ = true;
    return true;
}

// --- Rendering ---------------------------------------------------------------------------------

void Surface::drawGrid(JPrimitiveBuffer& buf, const JRect& page, const Xform& t) const {
    if (!EditorSettings::instance().showGrid()) return;
    const float gs = static_cast<float>(std::max(2, EditorSettings::instance().gridSize())) * t.scale;
    if (gs < 6.f) return;
    const uint8_t line[4] = { 255, 255, 255, 16 };
    buf.pushClip(page.x, page.y, page.width, page.height);
    for (float x = page.x; x <= page.x + page.width; x += gs) buf.pushRectangle(x, page.y, 1.f, page.height, line);
    for (float y = page.y; y <= page.y + page.height; y += gs) buf.pushRectangle(page.x, y, page.width, 1.f, line);
    buf.popClip();
}
static const std::string kNoTitle;   // a panel scope draws no page title — see drawFrame

// The surface's own frame + title, driven by the canvas properties.
// borderWidth 0 with no title = no frame; a title keeps a 1px frame. Border is drawn in device px
// (unscaled). Border Style Solid/Dash/Dot is honoured (see below); the
// title bar does not yet inset element positions.
void Surface::drawFrame(JPrimitiveBuffer& buf, const JRect& page) const {
    if (!model_) return;
    // Inside a PANEL scope the page is the panel's content box (see enterPanel), so a title band drawn
    // here would cover the content it exists to keep clear of. The breadcrumb names it instead.
    const bool inPanel = !scopes_.empty() && scopes_.back().panelElemId != 0;
    const std::string& title = (inPanel || !showPageTitle_) ? kNoTitle : model_->title();
    const int bw0 = model_->borderWidth();
    if (bw0 <= 0 && title.empty()) return;                       // hasBorder()

    const float w = static_cast<float>(bw0 > 0 ? bw0 : 1);       // title-only frame stays 1px
    uint8_t bcbuf[4];
    const uint8_t* bc = skin::parseHex(model_->borderColor(), bcbuf) ? bcbuf : jf::Colors::Border;
    const float half = w * 0.5f;
    // Title font (family|size|b|i): its px height + face drive the band sizing AND the drawn glyphs, so
    // the "Title Font" property actually applies. It was drawn in the base font, ignoring the setting.
    const std::string tfont = model_->titleFont();
    const float basePx  = jf::JTextHelper::lineHeight();
    const float titlePx = fontSpecPx(tfont, basePx);
    const float tscale  = (basePx > 0.f) ? titlePx / basePx : 1.f;
    const std::string tface = fontSpecFace(tfont);
    const int   lh   = static_cast<int>(titlePx + 0.5f);
    const int   pad  = std::max(model_->titlePadding(), 4);
    const int   th   = lh + 2 * model_->titlePadding();
    const bool  titled = !title.empty() && title != ".";
    const bool  bottom = model_->titleEdge() == 1;               // 0=Top 1=Bottom
    const int   band = (titled && model_->titlePlace() == 1) ? (lh / 2 + model_->titlePadding()) : 0;
    const float topI = half + (band && !bottom ? band : 0);
    const float botI = half + (band &&  bottom ? band : 0);

    const float fx = page.x + half, fy = page.y + topI;
    const float fw = page.width - w, fh = page.height - topI - botI;
    // Honour the border Style: 2 and 4+ dashed, 3 dotted, anything else solid.
    // 1=Solid keeps the rounded-corner stroke; 2=Dash / 3=Dot (and 4-5 dash-family) emit segments via the
    // framework's dashed primitive (sharp corners — the dash primitive is straight-edged; only the solid
    // border rounds, which matches how the styles read in practice). Pattern lengths scale by pen width,
    // like Qt (Dash 4-on/2-off, Dot 1-on/2-off).
    const int bs = model_->borderStyle();
    if (bs == 2 || bs >= 4) buf.pushDashedRect(fx, fy, fw, fh, bc, w, 4.f * w, 2.f * w);   // Dash / DashDot(Dot)
    else if (bs == 3)       buf.pushDashedRect(fx, fy, fw, fh, bc, w, 1.f * w, 2.f * w);   // Dot
    else buf.pushRectangle(fx, fy, fw, fh, jf::Colors::Transparent, static_cast<float>(model_->borderRadius()), w, bc);
    if (!titled) return;

    uint8_t tcbuf[4];
    const uint8_t* tc = skin::parseHex(model_->titleColor(), tcbuf) ? tcbuf : jf::Colors::TextPrimary;
    const float tw = jf::JTextHelper::hasAtlas() ? jf::JTextHelper::measureWidthScaled(title, tscale) : 0.f;
    auto alignX = [&](float boxW, float extraPad) {
        if (model_->titleAlign() == 1) return page.x + (page.width - boxW) * 0.5f;   // Centre
        if (model_->titleAlign() == 2) return page.x + page.width - boxW - extraPad; // Right
        return page.x + extraPad;                                                    // Left
    };

    if (model_->titlePlace() == 1) {                             // Outside: legend straddling the border line
        const float boxW = tw + 8.f;
        const float tx = alignX(boxW, static_cast<float>(pad));
        const float cy = bottom ? (fy + fh) : fy;
        const float boxY = cy - lh * 0.5f;
        buf.pushRectangle(tx, boxY, boxW, static_cast<float>(lh), jf::Colors::Surface1, 0.f);   // knock out the line
        if (jf::JTextHelper::hasAtlas()) jf::JTextHelper::pushTextScaled(buf, tx + 4.f, boxY, title, tc, tscale, boxW, tface);
        return;
    }

    // Inside: a title bar at the chosen edge. Style 0=Plain 1=Underline 2=Filled.
    const float barY = bottom ? (page.y + page.height - th) : page.y;
    if (model_->titleStyle() == 2) buf.pushRectangle(page.x, barY, page.width, static_cast<float>(th), bc, 0.f);
    if (model_->titleStyle() == 1) {
        const float y = bottom ? barY : (barY + th);
        buf.pushRectangle(page.x, y - half, page.width, w, bc, 0.f);
    }
    if (jf::JTextHelper::hasAtlas()) {
        const float ty = barY + (th - lh) * 0.5f;
        jf::JTextHelper::pushTextScaled(buf, alignX(tw, static_cast<float>(pad)), ty, title, tc, tscale, page.width - 2.f * pad, tface);
    }
}
void Surface::drawSelection(JPrimitiveBuffer& buf, const JRect& r, bool grip, bool grouped) const {
    // Dashed outline plus the single bottom-right resize grip. No corner handles — only the bottom-right
    // grip resizes a widget. A member of a selected GROUP draws quieter (thinner, half alpha): the group's
    // bounding box carries the emphasis, and N loud outlines inside one box is just noise.
    if (grouped) {
        const uint8_t faint[4] = { jf::Colors::Accent[0], jf::Colors::Accent[1], jf::Colors::Accent[2], 120 };
        buf.pushDashedRect(r.x - 1.f, r.y - 1.f, r.width + 2.f, r.height + 2.f, faint, 1.f, 3.f, 3.f);
    } else {
        buf.pushDashedRect(r.x - 1.f, r.y - 1.f, r.width + 2.f, r.height + 2.f, jf::Colors::Accent, 1.5f, 5.f, 3.f);
    }
    if (grip) buf.pushRectangle(r.x + r.width - kGrip, r.y + r.height - kGrip, kGrip, kGrip, jf::Colors::Accent, 2.f);
}

void Surface::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    PERF_SCOPE("Surface::paint");
    const auto bb = getBoundingBox();
    const JRect b{ bb.x, bb.y, bb.width, bb.height };
    const bool editing = (mode_ == Mode::Edit);
    CanvasWidget::s_editMode = editing;   // mirrored-page containers read this to honour run-mode-only visibility
    const Xform t = xform();

    buf.pushClip(b.x, b.y, b.width, b.height);
    buf.pushRectangle(b.x, b.y, b.width, b.height, jf::Colors::Surface0);
    const JRect page = pageRect(t);

    // A PAGE-SHAPED CARD ONLY WHERE THERE IS A PAGE SHAPE.
    //
    // The card and its drop shadow say "the document is this big, and it ends here" — which is worth
    // saying about a FIXED canvas, and is the only thing telling an author where their coordinates run
    // out while they drag widgets around. It says nothing about a REFLOW page: that page IS the view, so
    // the card is the same rectangle as the area it sits in, and all it draws is a border round the
    // whole of the middle of the window with a shadow falling off the edge of the screen.
    //
    // So: reflowing and running -> the page simply fills what it was given. Fixed, or being edited,
    // keeps the card. Editing is included deliberately even for a reflow page, because an author needs
    // the extent even when the reader will never see it.
    const bool card = editing || !model_ || model_->canvasStatic() != 3;
    if (card) {
        const uint8_t shadow[4] = { 0, 0, 0, 90 };
        buf.pushRectangle(page.x + 6.f, page.y + 6.f, page.width, page.height, shadow, 4.f);
    }
    // FILL now, EDGE last. The page's own 1px edge used to be stroked with this fill, before any element
    // was drawn — so an element at 0,0 (a viewport filling the page is the ordinary case) painted straight
    // over it and the canvas lost its outline along every side the element touched. The authored frame
    // (drawFrame) was already drawn over the content for exactly this reason; the default edge was not.
    if (card) buf.pushRectangle(page.x, page.y, page.width, page.height, jf::Colors::Surface1, 2.f);
    else      buf.pushRectangle(b.x, b.y, b.width, b.height, jf::Colors::Surface1);
    if (editing) drawGrid(buf, page, t);

    std::unordered_map<int, JRect> groupBoxes;   // groupId -> union of its selected members (screen space)
    syncInstances();   // reconcile the persistent instance tree to the current model before painting it
    refreshInstancesIfChanged();
    if (model_) paintElements_(buf, t, editing, groupBoxes);   // child paint through the framework container
    // Group bounding boxes — one solid frame per selected group, drawn OVER its members so a group reads
    // as a single object. Inset padding keeps it clear of the members' own faint outlines.
    if (editing) {
        for (const auto& kv : groupBoxes) {
            const JRect& g = kv.second;
            const float pad = 4.f;
            buf.pushRectangle(g.x - pad, g.y - pad, g.width + 2 * pad, g.height + 2 * pad,
                              jf::Colors::Transparent, 3.f, 1.5f, jf::Colors::Accent);
        }
    }
    buf.pushRectangle(page.x, page.y, page.width, page.height, jf::Colors::Transparent, 2.f,
                      1.f, jf::Colors::Border);        // the page's own edge, over whatever fills it
    drawFrame(buf, page);                              // surface border + title (over the content)
    if (editing && drag_ == Drag::Marquee) {
        const float x = std::min(mqX_, mqCurX_), y = std::min(mqY_, mqCurY_);
        // The rubber-band: a fixed
        // light-blue #6ab0ff 1px DASHED outline, NO fill. Fixed colour — not the theme accent —
        // so it reads identically under any stylesheet (the amber accent from studio.style must not bleed in).
        static const uint8_t band[4] = { 0x6a, 0xb0, 0xff, 0xff };
        buf.pushDashedRect(x, y, std::fabs(mqCurX_ - mqX_), std::fabs(mqCurY_ - mqY_), band, 1.f);
    }
    // Scroll bars, over the content — shown only when the canvas overflows (a fixed / static surface).
    const ScrollBars sb = scrollBars(t);
    auto drawBar = [&](bool on, const JRect& track, const JRect& thumb) {
        if (!on) return;
        buf.pushRectangle(track.x, track.y, track.width, track.height, jf::Colors::Surface0);
        buf.pushRectangle(thumb.x, thumb.y, thumb.width, thumb.height, jf::Colors::Surface3, (kSB - 2.f) * 0.5f);
    };
    drawBar(sb.hasV, sb.vTrack, sb.vThumb);
    drawBar(sb.hasH, sb.hTrack, sb.hThumb);
    // Breadcrumb — shown while editing an entered viewport's page in place. A bar across the top naming the
    // drilled-into node; clicking it (or Esc) exits the scope. Ports PanelView's entered-scope affordance.
    breadcrumbRect_ = {};
    if (inScope() && jf::JTextHelper::hasAtlas()) {
        const float bh = jf::JTextHelper::lineHeight() + 8.f;
        breadcrumbRect_ = { b.x, b.y, b.width, bh };
        buf.pushRectangle(b.x, b.y, b.width, bh, jf::Colors::Surface2, 0.f, 1.f, jf::Colors::Border);
        std::string trail = "< Main";                                 // < Main / A / B …
        for (const auto& s : scopes_) {
            // A panel scope is named by the panel's TITLE where it has one — the page cannot show it
            // (the page is the panel's content box), and "panel" names nothing when there are three.
            if (s.panelElemId != 0 && s.model)
                if (const PanelElement* pe = s.model->get(s.panelElemId); pe && !pe->prop("labelText").empty()) {
                    trail += " / " + pe->prop("labelText");
                    continue;
                }
            const std::string& p = s.nodePath; const size_t k = p.rfind('/');
            trail += " / " + (k == std::string::npos ? p : p.substr(k + 1));
        }
        trail += "    (Esc to leave)";
        jf::JTextHelper::pushText(buf, b.x + 8.f, b.y + 4.f, trail, jf::Colors::TextPrimary, b.width - 16.f);
    }
    // Armed spacing prompt. Drawn LAST so nothing can cover it, and anchored BOTTOM-left because the top
    // edge is owned by the page title (drawFrame) and the in-scope breadcrumb bar — both paint after the
    // content, which is what buried the first version. Lifted clear of the horizontal scrollbar gutter.
    if (editing && spacingAxis_ && jf::JWidget::s_focusHook) {
        // The armed prompt owns the keyboard. The surface is the central widget, so the runner only routes
        // keys to it when NOTHING else is focused (JAppWindow: `if (!m_focus.focused()) central->key`). We
        // can't grab focus at arm time — armSpacing ran inside the context menu's popup, which has its OWN
        // focus manager as s_active. So we clear focus here in the main-window paint pass, every frame the
        // prompt is up: s_focusHook(nullptr) blurs whatever dock field held focus so keys reach handleKeyEvent.
        // setFocus(nullptr) early-returns once focus is already null, so this doesn't thrash after the first
        // clear, and it self-heals if the user clicks a field away while the prompt is still open.
        jf::JWidget::s_focusHook(nullptr);
    }
    if (editing && spacingAxis_ && jf::JTextHelper::hasAtlas()) {
        char hint[128];
        std::snprintf(hint, sizeof(hint), "%s %s:  %s_    (Enter to apply, Esc to cancel)",
                      spacingAxis_ == 2 ? "Horizontal" : "Vertical",
                      spacingPitch_ ? "pitch" : "gap",
                      spacingBuf_.c_str());
        const float lh = jf::JTextHelper::lineHeight();
        const float hw = jf::JTextHelper::measureWidth(hint) + 20.f, hh = lh + 10.f;
        const float hx = b.x + 10.f;
        const float hy = b.y + b.height - hh - 10.f - (sb.hasH ? kSB : 0.f);
        const uint8_t shadow2[4] = { 0, 0, 0, 110 };
        buf.pushRectangle(hx + 2.f, hy + 2.f, hw, hh, shadow2, 4.f);          // lift it off the canvas
        buf.pushRectangle(hx, hy, hw, hh, jf::Colors::Accent, 4.f);
        jf::JTextHelper::pushText(buf, hx + 10.f, hy + 5.f, hint, jf::Colors::Surface0, hw - 12.f);
    }
    buf.popClip();
}

// --- Input (edit mode) -------------------------------------------------------------------------

// A control participates in run-mode input only when it is actually SHOWN (node gate + visibility
// condition) and not disabled by its enableCondition (greyed = inert, original enable-condition parity).
bool Surface::runTarget_(const PanelElement& el) const {
    if (nodeHidden_(el)) return false;
    CanvasWidget* w = const_cast<Surface*>(this)->instanceFor(el);
    if (!w) return false;
    return w->visibleNow() && w->enabledNow();
}

// Move run-mode keyboard focus to the next/previous interactive control (dir +1 / -1), in element order,
// wrapping. Returns false when there is nothing to focus, so the key can bubble out of the surface.
// Framework focus reached (or left) the canvas. Entering hands the keyboard to a control so the ring is
// visible immediately; leaving blurs the active one, which commits whatever it was editing.
void Surface::onFocusEvent(bool focused) {
    if (mode_ != Mode::Run) return;
    if (focused) {
        // Enter at the NEAR end: Tab lands on the first control, Shift+Tab on the last. Entering at the first
        // either way makes Shift+Tab walk straight back out, which reads as focus refusing to move at all.
        const int dir = (jf::JFocusManager::s_active && jf::JFocusManager::s_active->lastTraversalDir() < 0) ? -1 : 1;
        if (!activeControl_) focusNextControl_(dir);
    }
    else         { setActiveControl_(0); }
    invalidate();
}

// The tab ring, without moving focus — the same enumeration focusNextControl_ walks, so the two cannot
// drift apart and say different things about the same page.
// assumeEnabled asks the STRUCTURAL question instead of the live one: would this control be in the ring if
// its enable condition were true? Most of an ECU is switched off in any one tune -- twelve pages of this
// document have five enabled controls between them -- so auditing only what is live today proves almost
// nothing about the rest.
std::vector<int> Surface::keyboardReachable(bool assumeEnabled) {
    std::vector<int> out;
    if (!model_) return out;
    std::vector<CanvasWidget::TabSlot> slots;
    for (const auto& el : model_->elements()) {
        if (assumeEnabled) { if (nodeHidden_(el)) continue; }
        else if (!runTarget_(el)) continue;
        CanvasWidget* w = instanceFor(el);
        if (w && w->interactive() && (assumeEnabled ? w->visibleNow() : true))
            slots.push_back({ el.id, (float)el.x, (float)el.y, (float)el.h });
    }
    return CanvasWidget::readingOrder(std::move(slots));
}

bool Surface::focusNextControl_(int dir) {
    if (!model_) return false;
    const auto& els = model_->elements();
    std::vector<CanvasWidget::TabSlot> slots;
    for (const auto& el : els) {
        if (!runTarget_(el)) continue;
        CanvasWidget* w = instanceFor(el);
        if (w && w->interactive())
            slots.push_back({ el.id, (float)el.x, (float)el.y, (float)el.h });
    }
    // Reading order, not element order — the same ring rule a viewport uses (CanvasWidget::readingOrder).
    const std::vector<int> ids = CanvasWidget::readingOrder(std::move(slots));
    if (ids.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == activeControl_) { cur = (int)i; break; }
    // The canvas CYCLES: past the last control Tab returns to the first, before the first Shift+Tab returns to
    // the last. Handing the key back to the framework at each end reads as focus vanishing -- the chain out
    // there is dock widgets the eye cannot find, and several presses from any way back. The canvas is a form;
    // the keyboard stays in it, and the mouse moves between panes.
    const int n    = (int)ids.size();
    const int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : ((cur + dir) % n + n) % n;
    setActiveControl_(ids[next], /*announce=*/true);   // blurs the outgoing control (commits), focuses the new one
    return true;
}

// Move run-mode keyboard focus, telling the OUTGOING control it lost it. A hosted framework control is not in
// the window's focus chain (nothing can tree-walk into the canvas), so this is the only blur it will ever get:
// without it a clicked spin box keeps its caret and its uncommitted text after you click something else.
void Surface::setActiveControl_(int id, bool announce) {
    if (activeControl_ == id) return;
    const int prev = activeControl_;
    activeControl_ = id;
    if (model_) {
        // The outgoing control commits and drops its caret; the incoming one is told only when the keyboard
        // arrived WITHOUT a click (Tab), since a press begins its own edit at the clicked position.
        if (prev)              _announce_(prev, ControlInput::Kind::Blur);
        if (announce && id)    _announce_(id,   ControlInput::Kind::Focus);
    }
    invalidate();
}

// Deliver a focus-change notification to one element's instance. The rect is the element's real screen rect, so
// a widget that hosts children (a viewport) can position what it forwards.
void Surface::_announce_(int id, ControlInput::Kind kind) {
    if (!model_) return;
    const Xform t = xform();
    const auto& els = model_->elements();
    for (size_t i = 0; i < els.size(); ++i) {
        if (els[i].id != id) continue;
        CanvasWidget* w = instanceFor(els[i]);
        if (!w || !w->interactive()) return;
        if (!w->acceptsInput(kind)) return;      // disabled by enableCondition — no focus, no keys
        ControlInput in; in.kind = kind; in.focused = (kind == ControlInput::Kind::Focus);
        w->setCache(&cache_);
        w->onControlInput(screenRectOf(i, t), in);
        return;
    }
}

jf::JRect Surface::screenRectOfId(int id) const {
    if (!model_) return {};
    const auto& els = model_->elements();
    const Xform t = xform();
    for (size_t i = 0; i < els.size(); ++i)
        if (els[i].id == id) return screenRectOf(i, t);
    return {};
}

bool Surface::routeRunInput_(ControlInput::Kind kind, float mx, float my, float wheel, const jf::JKeyEvent* key) {
    if (!model_) return false;
    syncInstances();   // instanceFor is a pure lookup — make sure the tree matches the model first
    refreshInstancesIfChanged();
    const Xform t = xform();
    const auto& els = model_->elements();
    auto deliver = [&](size_t idx) -> bool {
        CanvasWidget* w = instanceFor(els[idx]);
        if (!w || !w->interactive()) return false;
        if (!w->acceptsInput(kind)) return false;   // enableCondition false: greyed AND inert
        w->setCache(&cache_);   // instance data is current (event-driven refresh); onControlInput reads element()
        ControlInput in; in.kind = kind; in.mx = mx; in.my = my; in.wheel = wheel; in.key = key;
        in.focused = (els[idx].id == activeControl_);
        const bool r = w->onControlInput(screenRectOf(idx, t), in);
        if (r) invalidate();
        return r;
    };
    if (kind == ControlInput::Kind::Key || kind == ControlInput::Kind::Move || kind == ControlInput::Kind::Release) {
        if (!activeControl_) {
            if (kind == ControlInput::Kind::Key)
                JLOGC("surface.key", jf::JLogLevel::Debug) << "  routeRunInput_: no activeControl_ \xE2\x86\x92 dropped";
            return false;                                        // deliver to the focused / press-captured control
        }
        for (size_t i = 0; i < els.size(); ++i)
            if (els[i].id == activeControl_) {
                const bool ok = runTarget_(els[i]);
                if (kind == ControlInput::Kind::Key)
                    JLOGC("surface.key", jf::JLogLevel::Debug) << "  routeRunInput_: active id=" << els[i].id
                        << " type=" << els[i].type << " runTarget=" << ok;
                return ok ? deliver(i) : false;
            }
        if (kind == ControlInput::Kind::Key)
            JLOGC("surface.key", jf::JLogLevel::Debug) << "  routeRunInput_: activeControl_=" << activeControl_
                << " is not in this model \xE2\x86\x92 cleared";
        setActiveControl_(0); return false;
    }
    // Press / Scroll: the topmost interactive control under the cursor takes it (Press also takes focus).
    //
    // A WHEEL only reaches a control that already has focus. Hovering is not consent: a page of spin boxes
    // would otherwise swallow every wheel event the pointer happened to be over — the page stops scrolling,
    // and worse, the roll silently edits whichever value is under the cursor. Click it, then nudge it.
    for (size_t i = els.size(); i-- > 0; ) {
        if (!runTarget_(els[i])) continue;               // hidden / disabled controls are not click targets
        const jf::JRect r = screenRectOf(i, t);
        if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) {
            CanvasWidget* w = instanceFor(els[i]);
            const bool inter = w && w->interactive();
            if (kind == ControlInput::Kind::Press) setActiveControl_(inter ? els[i].id : 0);
            // A WHEEL reaches an unfocused control only if it is a scrolling VIEW — see CanvasWidget::wantsWheel.
            // Without this the page always won at the top level too, exactly as it did inside a viewport.
            else if (els[i].id != activeControl_ && !(w && w->wantsWheel(r, mx, my))) return false;
            return inter ? deliver(i) : false;
        }
    }
    if (kind == ControlInput::Kind::Press) setActiveControl_(0);   // empty space clears focus
    return false;
}

void Surface::handleMousePress(float mx, float my) {
    if (!model_) return;
    // Scroll-bar thumbs take precedence in either mode (they sit over the canvas edges).
    { const ScrollBars sb = scrollBars(xform());
      auto in = [](const jf::JRect& r, float x, float y){ return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height; };
      JLOGC("surface", jf::JLogLevel::Info) << "[press] " << mx << "," << my << " hasV=" << sb.hasV
          << " thumb=" << sb.vThumb.x << "," << sb.vThumb.y << " " << sb.vThumb.width << "x" << sb.vThumb.height
          << " hit=" << in(sb.vThumb, mx, my);
      if (sb.hasV && in(sb.vThumb, mx, my)) { drag_ = Drag::ScrollV; scrollGrab_ = my - sb.vThumb.y; return; }
      if (sb.hasH && in(sb.hThumb, mx, my)) { drag_ = Drag::ScrollH; scrollGrab_ = mx - sb.hThumb.x; return; } }
    if (mode_ == Mode::Run) { lastMx_ = mx; lastMy_ = my; runDrag_ = routeRunInput_(ControlInput::Kind::Press, mx, my, 0.f, nullptr); return; }
    if (mode_ != Mode::Edit) return;
    if (jf::JDragDrop::isDragging()) { tryDrop_(mx, my); return; }   // drop a held drag here; never marquee under it
    const auto bb = getBoundingBox();
    if (mx < bb.x || mx >= bb.x + bb.width || my < bb.y || my >= bb.y + bb.height) return;
    // A click in the breadcrumb bar leaves the current in-place scope (like clicking empty parent space).
    if (inScope() && mx >= breadcrumbRect_.x && mx < breadcrumbRect_.x + breadcrumbRect_.width &&
        my >= breadcrumbRect_.y && my < breadcrumbRect_.y + breadcrumbRect_.height) { exitScope(); return; }
    // Double-click a viewport → drill in to edit its node's page in place (EditTree/PanelView parity).
    const auto now = std::chrono::steady_clock::now();
    const bool dbl = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickT_).count() < 400 &&
                     std::fabs(mx - lastClickX_) < 5.f && std::fabs(my - lastClickY_) < 5.f;
    lastClickT_ = now; lastClickX_ = mx; lastClickY_ = my;
    if (dbl) { if (const int vp = viewportElAt(mx, my)) { lastClickT_ = {}; enterViewport(vp); return; } }
    if (dbl) { if (const int pn = panelElAt(mx, my))    { lastClickT_ = {}; enterPanel(pn);    return; } }
    const Xform t = xform();
    // Double-click a label → edit its caption in place. Checked after the viewport/panel drill-ins (a label
    // is never either) and before the drag/selection logic, so the second click opens the caret instead of
    // starting a move. A single click anywhere else commits whatever caption is already open.
    if (dbl) {
        const int hid = hitTest(mx, my, t);
        // ...but NOT the caption already open. This rule ran first and did not ask, so a double-click
        // inside an open caption — the ordinary way to select a word before retyping it — re-entered the
        // edit instead: beginLabelEdit_ commits, re-reads the caption from the model and selects ALL of
        // it, so the click that was meant to land in the middle of the text took the whole of it. Left to
        // fall through, the open-caption branch below selects the word under the pointer, which is what
        // every other text field in the app does.
        if (hid != inlineEdit_)
            if (const PanelElement* le = hid ? model_->get(hid) : nullptr)
                if (CanvasWidget* lw = instanceFor(*le); lw && lw->editsCaption()) {
                    lastClickT_ = {}; beginLabelEdit_(hid); return;
                }
    }
    // Clicks while a caption is open: INSIDE it place or extend the caret (double-click selects all);
    // anywhere else commits and falls through to the normal canvas handling below.
    if (inlineEdit_) {
        const PanelElement* ie = model_->get(inlineEdit_);
        const jf::JRect ir = ie ? toScreen(*ie, t) : jf::JRect{};
        const bool inside = ie && mx >= ir.x && mx < ir.x + ir.width && my >= ir.y && my < ir.y + ir.height;
        if (inside) {
            auto& ed = LabelWidget::labelEdits()[inlineEditUid_()];
            // CARET PATH INSTRUMENTATION (JF_LOG=surface.caret=debug), for the same reason the key path
            // carries it: a caret that lands in the wrong place is a disagreement between two numbers —
            // where the click was, and where the caption told us it was drawn — and neither is visible
            // from the outside. One line names both, and the index they produced.
            const int at = LabelWidget::caretAtX(ed, mx);
            JLOGC("surface.caret", jf::JLogLevel::Debug)
                << "click mx=" << mx << " tx=" << ed.tx << " scale=" << ed.scale
                << " local=" << (mx - ed.tx) << " -> caret " << at << "/" << ed.core.text().size()
                << (dbl ? " (double: select word)" : "") << " elem=" << inlineEdit_
                << " rect=" << ir.x << "," << ir.y << " " << ir.width << "x" << ir.height;
            if (dbl) { ed.core.selectWordAt(static_cast<size_t>(at)); lastClickT_ = {}; }
            else     { ed.core.setCaret(static_cast<size_t>(at), /*extend=*/false); drag_ = Drag::TextSelect; }
            invalidate(); return;
        }
        commitLabelEdit_();
    }
    lastMx_ = mx; lastMy_ = my; pressMx_ = mx; pressMy_ = my; dragArmed_ = false;

    if (selection_.size() == 1) {                        // resize grip of a lone selection
        if (const PanelElement* e = model_->get(selection_.front()))
            if (inResizeGrip(mx, my, toScreen(*e, t))) { drag_ = Drag::Resize; dragStart_ = model_->elements(); return; }
    }
    const int hit = hitTest(mx, my, t);
    const std::vector<int> before = selection_;
    const bool additive = JWidget::s_ctrlDown || JWidget::s_shiftDown;   // Ctrl/Shift-click multi-select
    if (hit) {
        if (additive) {
            if (isSelected(hit)) selection_.erase(std::remove(selection_.begin(), selection_.end(), hit), selection_.end());
            else selection_.push_back(hit);
            expandToGroups();
            drag_ = Drag::None;   // a modifier-click toggles selection; it doesn't start a drag
        } else {
            if (!isSelected(hit)) {
                selectOnly(hit); expandToGroups();   // select the element + its group-mates
                JLOGC("surface", jf::JLogLevel::Info) << "[select] hit id=" << hit << " -> " << selection_.size()
                    << (selectionHasGroup() ? " selected (GROUP — click expanded to group-mates)"
                                            : " selected (single widget)");
            }
            drag_ = Drag::Move; dragStart_ = model_->elements();  // snapshot for the undoable move
        }
    } else {
        selection_.clear();                              // empty space → rubber-band
        drag_ = Drag::Marquee; mqX_ = mqCurX_ = mx; mqY_ = mqCurY_ = my;
    }
    if (selection_ != before) selectionChanged.emit();
    invalidate();
}

void Surface::handleMouseMove(float mx, float my) {
    hoverX_ = mx; hoverY_ = my;                 // track for context-menu placement
    CanvasWidget::s_hoverX = mx; CanvasWidget::s_hoverY = my;   // canvas-widget hover hit-test (e.g. hyperlink)
    if (drag_ == Drag::ScrollV || drag_ == Drag::ScrollH) { dragScrollThumb(mx, my); return; }
    if (drag_ == Drag::TextSelect) {            // sweeping a selection across an open caption
        if (inlineEdit_) { auto& ed = LabelWidget::labelEdits()[inlineEditUid_()];
                           ed.core.setCaret(static_cast<size_t>(LabelWidget::caretAtX(ed, mx)), /*extend=*/true); invalidate(); }
        return;
    }
    if (mode_ == Mode::Run) {
        // Right-click shows the table menu over a table, the curve menu over a curve, else nothing.
        setContextMenu(tableElAt(mx, my) ? &tableMenu_ : curveElAt(mx, my) ? &curveMenu_ : nullptr);
        if (runDrag_) routeRunInput_(ControlInput::Kind::Move, mx, my, 0.f, nullptr);
        return;
    }
    if (drag_ == Drag::None || !model_) return;
    const float s = xform().scale, sinv = (s > 0 ? 1.f / s : 1.f);
    const float dx = (mx - lastMx_) * sinv, dy = (my - lastMy_) * sinv;
    lastMx_ = mx; lastMy_ = my;
    if (drag_ == Drag::Marquee) {
        mqCurX_ = mx; mqCurY_ = my;
        const std::vector<int> before = selection_;
        updateMarquee();
        if (selection_ != before) selectionChanged.emit();
    } else if (drag_ == Drag::Move) {
        // Raw-accumulate from the PRESS point + original rects (never feed snapped positions back, or
        // the snap fights the drag and creates a dead zone). Snap the leader's raw position; apply the
        // same total offset to every selected element from its original rect.
        if (!armDrag_(mx, my)) return;      // still a click, not a drag — selecting must never snap
        float tdx = (mx - pressMx_) * sinv, tdy = (my - pressMy_) * sinv;
        // Snap the selection's BOUNDING BOX, not selection_.front(). The front element is whatever the
        // selection vector happened to put first — for a marquee that is model/z order, so the group used to
        // align to a member the user never picked and could not identify. The box is the thing being dragged.
        float bx0 = 0.f, by0 = 0.f, bx1 = 0.f, by1 = 0.f; bool any = false;
        for (int id : selection_) if (const PanelElement* o = origOf(id)) {
            if (!any) { bx0 = o->x; by0 = o->y; bx1 = o->x + o->w; by1 = o->y + o->h; any = true; }
            else { bx0 = std::min(bx0, o->x); by0 = std::min(by0, o->y);
                   bx1 = std::max(bx1, o->x + o->w); by1 = std::max(by1, o->y + o->h); }
        }
        if (any) {
            const auto [sdx, sdy] = snapDelta(bx0 + tdx, by0 + tdy, bx1 - bx0, by1 - by0);
            tdx += sdx; tdy += sdy;
        }
        for (int id : selection_) if (const PanelElement* o = origOf(id)) model_->setRect(id, o->x + tdx, o->y + tdy, o->w, o->h);
    } else if (drag_ == Drag::Resize && !selection_.empty()) {
        if (!armDrag_(mx, my)) return;      // a click on the grip must not snap the size either
        if (const PanelElement* o = origOf(selection_.front())) {
            const float tdx = (mx - pressMx_) * sinv, tdy = (my - pressMy_) * sinv;
            float w = std::max(kMinW, o->w + tdx), h = std::max(kMinH, o->h + tdy);
            if (o->prop("aspectRatioLocked") == "1" && o->w > 0.f && o->h > 0.f) {
                const float ar = o->w / o->h;   // Lock Aspect Ratio: derive the other axis from the dominant drag
                if (std::abs(tdx) * o->h >= std::abs(tdy) * o->w) h = w / ar; else w = h * ar;
                w = std::max(kMinW, w); h = std::max(kMinH, h);   // no grid snap — the ratio takes precedence
            } else {
                snapSize(o->x, o->y, w, h);
            }
            model_->setRect(o->id, o->x, o->y, w, h);
        }
    }
    invalidate();
}

// The caption for a control dropped from the Dictionary. The meta's label is the good one, but it only
// covers paths the meta model indexes — a precondition (and anything else outside config/telemetry/arrays)
// yields nothing, and the widget then fell back to its TYPE for a caption: a dropped precondition came out
// labelled "Config Edit". A binding always knows its own field, so fall back to the path's leaf name —
// "module.array[key].field" -> "field" — which is what the viewport drop already does with node names.
static std::string dropCaption(const Cache& c, const std::string& path) {
    std::string cap = c.label(path);
    if (!cap.empty()) return cap;
    const size_t dot = path.find_last_of('.');
    std::string leaf = (dot == std::string::npos) ? path : path.substr(dot + 1);
    if (!leaf.empty() && leaf.back() == ']') {                 // "...[key]" — take the key itself
        const size_t open = leaf.find_last_of('[');
        if (open != std::string::npos) leaf = leaf.substr(open + 1, leaf.size() - open - 2);
    }
    return leaf.empty() ? path : leaf;
}

// Resolve an active drag at (mx,my): a Dictionary binding (onto a control → point its source at it; onto
// empty canvas → create the default control for its type, bound) or a palette control-type (→ new control).
// Called on release AND on a press-while-dragging — so a drag begun in a FLOATING dock (whose release grab
// never reaches the main window) still drops on the next click, and without starting a marquee. Returns
// true if it consumed a drop.
bool Surface::tryDrop_(float mx, float my) {
    if (mode_ != Mode::Edit) return false;
    if (jf::JDragDrop::accept<DictBinding>(mx, my,
            [this](const DictBinding& b, float x, float y) {
                if (b.paths.empty()) return;
                // Dropped onto an existing control → bind its source to the FIRST path (rebinding one
                // control to many paths makes no sense). Only for a single path; a multi-drop always tiles.
                if (b.paths.size() == 1) {
                    const std::string path = b.paths.front();
                    if (path.rfind("cmd:", 0) == 0) {             // command token → drop a CommandButton bound to it
                        const std::string name = path.substr(4);
                        hoverX_ = x; hoverY_ = y;
                        edit("Add command button", [this, name] {
                            addWidgetAt("command");
                            if (!selection_.empty()) {
                                // Just a normal command button with its command pre-filled — identical to one
                                // added by hand and typed. No hidden state distinguishing the two.
                                model_->setProp(selection_.front(), "command", name);
                                model_->setProp(selection_.front(), "labelText", name);
                                selectionChanged.emit();
                            }
                        });
                        return;
                    }
                    const int hit = hitTest(x, y, xform());
                    if (hit) {                                    // dropped onto a control → point its source at the node
                        edit("Bind source", [this, hit, path] { model_->setProp(hit, "signalName", path); });
                        selectOnly(hit); selectionChanged.emit();
                        return;
                    }
                }
                // Empty canvas (or a multi-field drop): create one default control per path, tiled down the
                // canvas from the drop point, as ONE undo step. The new controls become the selection.
                edit("Add bound controls", [this, paths = b.paths, x, y] {
                    std::vector<int> made;
                    const Xform t = xform();
                    const float step = 44.f / (t.scale > 0.f ? t.scale : 1.f);   // ~one row per field, in world units
                    float wy = y;
                    for (const std::string& path : paths) {
                        if (path.empty() || path.rfind("cmd:", 0) == 0) continue;   // commands aren't tiled
                        hoverX_ = x; hoverY_ = wy;
                        const std::string type = defaultControlFor(path);
                        // Help does NOT travel with the drop. The control resolves it from its binding and
                        // the caption resolves it through the group, both every frame — so a definition
                        // edited after the page was drawn reaches the page.
                        addWidgetAt(type, dropCaption(cache_, path));
                        if (!selection_.empty()) {
                            model_->setProp(selection_.front(), "signalName", path);
                            made.push_back(selection_.front());
                        }
                        wy += step;
                    }
                    selection_ = made;   // the whole batch is now selected
                    // …and a dropped field is a GROUP (control + its caption/units labels), so select the
                    // group, not just the control. Leaving one member selected means the very next thing
                    // you do — drag it, align it — silently acts on a fragment of what you can see.
                    expandToGroups();
                    selectionChanged.emit();
                });
            })) { invalidate(); return true; }
    if (jf::JDragDrop::accept<std::string>(mx, my,
            [this](const std::string& type, float x, float y) { hoverX_ = x; hoverY_ = y; edit("Add", [this, type] { addWidgetAt(type); }); })) {
        invalidate(); return true;
    }
    // A navigation-tree node dragged onto the surface → place it as a "viewport" element (one node → many
    // viewports). The viewport carries the node path in its "node" prop, which also gates it in the view-
    // switcher: it shows only when its node is the active one. Sized to the node page's canvas aspect.
    if (jf::JDragDrop::accept<NodePlacement>(mx, my,
            [this](const NodePlacement& np, float x, float y) {
                const std::string node = np.node;
                if (node.empty()) return;
                // Dropped ONTO a caption → set its Link to this node path (instead of placing a viewport).
                // This is how a link's target is wired: drag the tree node onto the caption. A caption with
                // a Link IS the hyperlink — there is no separate widget to aim at any more.
                if (const int hid = hitTest(x, y, xform())) {
                    if (const PanelElement* e = model_->get(hid);
                        e && instanceFor(*e) && instanceFor(*e)->editsCaption()) {
                        edit("Set Link", [this, hid, node] { model_->setProp(hid, "link", node); invalidate(); });
                        return;
                    }
                }
                float w = 400.f, h = 300.f;
                if (nodeviewport::resolver()) if (const PanelModel* pm = nodeviewport::resolver()(node))
                    if (pm->effCanvasW() > 0.f && pm->effCanvasH() > 0.f) { w = 480.f; h = 480.f * pm->effCanvasH() / pm->effCanvasW(); }
                const Xform t = xform();
                const float s = (t.scale > 0.f ? t.scale : 1.f);
                const float wx = (x - t.viewX - t.offX) / s, wy = (y - t.viewY - t.offY) / s;
                const size_t slash = node.rfind('/');
                const std::string leaf = (slash == std::string::npos) ? node : node.substr(slash + 1);
                edit("Add viewport", [this, node, leaf, wx, wy, w, h] {
                    // Default the title to the node's leaf name (readable out of the box); it's a normal prop,
                    // so the user can clear it in Properties for a titleless viewport.
                    const int id = model_->add("viewport", wx - w * 0.5f, wy - h * 0.5f, w, h, {{"node", node}, {"title", leaf}});
                    selectOnly(id); selectionChanged.emit();
                });
            })) { invalidate(); return true; }
    return false;
}

void Surface::handleMouseRelease(float mx, float my) {
    if (drag_ == Drag::ScrollV || drag_ == Drag::ScrollH) { drag_ = Drag::None; invalidate(); return; }
    if (drag_ == Drag::TextSelect) { drag_ = Drag::None; return; }   // caption selection: no layout edit
    if (mode_ == Mode::Run) { if (runDrag_) routeRunInput_(ControlInput::Kind::Release, mx, my, 0.f, nullptr); runDrag_ = false; return; }
    if (tryDrop_(mx, my)) return;                             // Dictionary binding / palette control drop
    if ((drag_ == Drag::Move || drag_ == Drag::Resize) && !dragStart_.empty()) {
        commitEdit(drag_ == Drag::Resize ? "Resize" : "Move", dragStart_);   // one undo step per drag
        dragStart_.clear();
        // A drag mutated the model geometry directly; the Properties dock's x/y/w/h editors now hold
        // stale values. Re-emit so it re-populates — otherwise the next field edit (e.g. typing a title)
        // flushes the STALE geometry back and the element snaps to its pre-drag rect.
        selectionChanged.emit();
    }
    drag_ = Drag::None; invalidate();
}

bool Surface::handleKeyEvent(const jf::JKeyEvent& ke) {
    // KEY PATH INSTRUMENTATION (JF_LOG=surface.key=debug). A canvas control is not in the window's focus
    // chain — the Surface is its own focus domain — so a key has three places to disappear: the window may
    // never hand it here, this may not know which control is active, or the control may refuse it. One line
    // at each, so a key that goes nowhere says WHERE.
    JLOGC("surface.key", jf::JLogLevel::Debug) << "Surface::handleKeyEvent key=" << int(ke.key)
        << " utf8='" << (ke.utf8[0] ? ke.utf8 : "") << "' pressed=" << ke.pressed
        << " mode=" << (mode_ == Mode::Run ? "RUN" : "EDIT")
        << " activeControl=" << activeControl_ << " model=" << (model_ ? "yes" : "NO");
    if (!ke.pressed || !model_) return false;
    using K = jf::JKeyEvent::JKey;
    if (mode_ == Mode::Run) {
        // Run mode Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z undo/redo the live TUNE edits — the Cache's byte-diff
        // history (cell edits, smooth, linearise, value ops, …), which also pushes the reverted bytes to the
        // ECU. This is a different stack from the edit-mode layout undo below. Handle it before the control
        // routing so it works regardless of which control (table/slider/…) has keyboard focus.
        if (ke.ctrl && (ke.key == K::Z || ke.key == K::Y)) {
            doUndoRedo((ke.key == K::Y) || (ke.key == K::Z && ke.shift));   // mode-aware: run mode -> Cache tune stack
            return true;
        }
        // Tab walks the interactive controls, in element order, and commits the one it leaves (setActiveControl_
        // blurs it). The canvas is its own focus domain -- its controls are not in the window's focus chain, so
        // the framework's Tab cannot reach them and would skip straight past the whole surface. Offered to the
        // focused control FIRST (a table's inline op may want it); only an unclaimed Tab moves the focus.
        // jIsTabNav/jTabNavDir, not a hand-rolled Tab test: X11 delivers Shift+Tab as its own key (BackTab)
        // while Windows and macOS deliver Tab+shift, and matching one spelling let every Shift+Tab fall through
        // to the framework -- which moved focus out of the canvas with no way back in.
        if (jf::jIsTabNav(ke) && !ke.ctrl && !ke.alt) {
            if (routeRunInput_(ControlInput::Kind::Key, 0.f, 0.f, 0.f, &ke)) return true;
            if (focusNextControl_(jf::jTabNavDir(ke))) return true;
            return false;   // no interactive controls at all: let the framework have the key
        }
        return routeRunInput_(ControlInput::Kind::Key, 0.f, 0.f, 0.f, &ke);   // interactive controls
    }
    if (mode_ != Mode::Edit) return false;

    // An open label caption owns the keyboard: printable chars extend it, Backspace trims a whole UTF-8
    // character, Enter commits, Escape reverts. This MUST precede the nudge/delete handling below — a bare
    // letter would otherwise fall through and Delete would destroy the widget being renamed.
    if (inlineEdit_) {
        // The caption edits through the SHARED JTextEditCore — same keys/word-nav/clipboard/select as every
        // other field. Escape/Return are the caption's own commit/cancel; the core handles the rest, and any
        // text change refits the box. Every key is swallowed so it can't nudge the selected widget.
        if (ke.key == K::Escape) { cancelLabelEdit_(); return true; }
        // Enter commits; SHIFT-Enter is a line break. The core is set multiline, so letting the shifted
        // Return fall through to it inserts the newline — the caption itself decides which Return means
        // "done", because only the caption knows it is being edited in place rather than in a field.
        if (ke.key == K::Return && !ke.shift) { commitLabelEdit_(); return true; }
        auto& ed = LabelWidget::labelEdits()[inlineEditUid_()];
        const auto res = ed.core.handleKey(ke);
        if (res.changed) fitLabelToText_();                    // caption grew/shrank → resize to fit
        invalidate();
        return true;
    }

    // Armed spacing entry owns the keyboard while it is up: type the step, Enter applies, Esc cancels.
    // Same on-canvas idiom as the table's "Set to…" op — no modal, so no dialog plumbing in the host.
    if (spacingAxis_) {
        if (ke.key == K::Escape) { spacingAxis_ = 0; spacingBuf_.clear(); invalidate(); return true; }
        if (ke.key == K::Return) {
            float v = 0.f; bool ok = true;
            try { v = std::stof(spacingBuf_); } catch (...) { ok = false; }
            const bool horizontal = (spacingAxis_ == 2); const bool pitch = spacingPitch_;
            spacingAxis_ = 0; spacingBuf_.clear();
            if (ok) edit("Set Spacing", [this, horizontal, pitch, v] { applySpacing(horizontal, pitch, v); });
            invalidate(); return true;
        }
        if (ke.key == K::Backspace) { if (!spacingBuf_.empty()) spacingBuf_.pop_back(); invalidate(); return true; }
        const char c = ke.utf8[0];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-') { spacingBuf_ += c; invalidate(); }
        return true;                       // swallow everything else so it cannot nudge the selection
    }

    // Escape leaves an entered viewport scope (back out to the parent page). Ports PanelView::exitScope.
    if (ke.key == K::Escape && inScope()) { exitScope(); return true; }

    // Undo / redo — Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z.
    if (ke.ctrl && (ke.key == K::Z || ke.key == K::Y)) {
        doUndoRedo((ke.key == K::Y) || (ke.key == K::Z && ke.shift));
        return true;
    }
    if (selection_.empty()) return false;

    // Group / Ungroup via the global keymap (defaults: 'g' / 'u', user-rebindable in Preferences).
    switch (Keymap::instance().action(ke)) {
        case Keymap::Action::Group:   edit("Group",   [this]{ groupSelection();   }); return true;
        case Keymap::Action::Ungroup: edit("Ungroup", [this]{ ungroupSelection(); }); return true;
        default: break;
    }

    const float step = ke.shift ? 10.f : 1.f;
    float dx = 0, dy = 0;
    switch (ke.key) {
        case K::Delete: menuDelete(); return true;
        case K::Left:  dx = -step; break;
        case K::Right: dx =  step; break;
        case K::Up:    dy = -step; break;
        case K::Down:  dy =  step; break;
        default: return false;
    }
    edit("Nudge", [this, dx, dy]{
        for (int id : selection_) if (PanelElement* e = model_->get(id)) model_->setRect(id, e->x + dx, e->y + dy, e->w, e->h);
        invalidate();
    });
    return true;
}


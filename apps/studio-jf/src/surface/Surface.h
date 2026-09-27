#pragma once

// Surface — one tuning surface's editor/renderer, hosted in a tab. Renders a PanelModel's widget instances
// under a DERIVED aspect-preserving transform (canvas size + scaling + anchor) — no user
// zoom/pan; the same transform serves run and edit. Edit mode authors the layout (multi-select via
// click / rubber-band, group move/resize/delete/nudge); run mode is the live instrument. Model-is-truth.

#include <j/core/JWidget.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/core/KeyEvent.h>
#include <j/core/MenuSystem.h>
#include <j/core/UndoStack.h>

#include "PanelModel.h"
#include "CanvasWidget.h"
#include "SurfaceCamera.h"   // the pure world->screen transform (Surface builds one from its live state)
#include "SurfaceCanvas.h"   // the custom framework container the instances paint through (Stage 2+)

#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class Cache;
class CanvasWidget;   // per-element render instance (class-hierarchy port); owned by canvas_ (the container)

class Surface : public jf::JWidget {
public:
    enum class Mode { Run, Edit };

    Surface(jf::JSceneGraph& g, const Cache& cache, PanelModel* model);
    ~Surface();   // out-of-line: canvas_ (SurfaceCanvas) owns the CanvasWidget instances

    // SHOULD EDIT MODE DRAW WHAT THE CONDITIONS HIDE? Off by default, and that default is the whole point.
    // A page states alternatives at the SAME coordinates — Motorsport's feature list sits exactly where
    // Vehicle Identity's fields do, because only one of them is ever on screen at once. Edit mode used to
    // ignore the conditions and draw every one of them, so authoring a page meant reading a pile of
    // captions overprinted on each other with no way to tell which widget any of them belonged to. The
    // page now edits as it runs: what you see is what a tuner sees, in the place they see it.
    //
    // Kept reachable, though, because a widget you cannot see is a widget you cannot fix: View ▸ Show
    // Hidden Widgets brings the rest back, outlined so it is obvious they are the ones the conditions
    // are holding back.
    static bool showHiddenInEdit()      { return s_showHiddenInEdit; }
    static void setShowHiddenInEdit(bool on) { s_showHiddenInEdit = on; }

    // DOES THIS SURFACE DRAW THE PAGE'S OWN TITLE? Off when a host already shows it. A page opened in an
    // MDI window has its name in the window's title bar, and the page then drew the same words again
    // immediately underneath: "Engine Configuration" over "Engine Configuration", two header bands deep,
    // on every page in the document. The window is the frame; the page is what is in it.
    // WHICH CONTROLS THE KEYBOARD CAN ACTUALLY REACH, in the order Tab walks them. The canvas is its own
    // focus domain — nothing outside can tree-walk into it — so a control that is not in this list cannot
    // be typed into however interactive it claims to be, and nothing on screen says so. Exposed because
    // that is a property worth ASKING of every page, not one to discover a widget at a time.
    std::vector<int> keyboardReachable(bool assumeEnabled = false);

    void        setPageTitleShown(bool on) { showPageTitle_ = on; invalidate(); }
    bool        pageTitleShown() const     { return showPageTitle_; }

    void        setMode(Mode m);
    Mode        mode() const { return mode_; }
    PanelModel* model() const { return model_; }

    // View-switcher (ports PanelView::setActiveNode/applyNodeFilter): the tree selection scopes the surface.
    // An element carrying a "node" prop is a node viewport — shown ONLY when it is the active node; an element
    // with no "node" prop is a plain control, always visible. Empty path = nothing selected → viewports hide.
    void        setActiveNode(const std::string& path);
    // …and which page this surface is on, so a tab can say where it left the tree.
    const std::string& activeNode() const { return activeNode_; }

    const std::vector<int>& selection() const { return selection_; }
    jf::JSignal<>           selectionChanged;   // the selection set changed
    // Emitted when a bulk table op finished and something needs saying — notably that some cells were
    // clamped to their dictionary bounds. Silent clamping during a paste would hide a mismatch between
    // what was pasted and what landed, so the op reports it. main.cpp wires this to the status bar.
    jf::JSignal<std::string> statusMessage;

    // App-set hook: the table context menu's "Key Bindings…" item requests the modal here (the dialog edits
    // the global Keymap singleton, so it needs no surface state — main.cpp wires this to win.openModal).
    bool showPageTitle_ = true;                      // see setPageTitleShown()
    static inline bool s_showHiddenInEdit = false;   // see showHiddenInEdit()
    static inline std::function<void()> onEditTableKeyBindings;

    // App-set hook: the table context menu's "Table Axis Setup…" item requests the modal here, passing the
    // table's data-source path (the dialog resolves + edits that table's axes live) — wired in main.cpp.
    // The axis editor, opened on a table path plus the per-STORAGE-axis display units the widget that
    // asked for it is using. Without them the dialog would edit an axis in counts while the grid behind
    // it shows volts — the same bin, two numbers.
    // …plus a way back: the dialog offers a per-axis display unit, and the WIDGET owns that choice, so
    // picking one there sets the same property the inspector does and persists with the layout.
    static inline std::function<void(std::string tablePath, std::vector<std::string> axisUnits,
                                     std::string cellUnit,
                                     std::function<void(std::string, std::string)> onUnitPicked)> onAxisSetup;
    std::vector<std::string> axisUnitsOf_() const;   // the target's per-storage-axis display units
    std::function<void(std::string, std::string)> unitSetter_(int elementId);   // dialog -> widget property

    // App-set hook: the edit-mode "Condition…" item opens a dialog to edit the selected control's visibility
    // condition. Called with the current compact string + an apply callback (Surface writes it as one undo
    // step). Wired in main.cpp to win.openModal<ExpressionEditor>.
    // `element` is the element in scope on the page (the entered viewport's data source, "" at the top
    // level) — the dialog's live test evaluates inside it, so a template's "[*]" previews against the
    // element the page is about. Wired in main.cpp to win.openModal<ExpressionEditor>.
    static inline std::function<void(std::string current, std::string element,
                                     std::function<void(std::string)> apply)> onEditCondition;

    // App-set hook: the expression EDITOR for a field whose expression runs on the ECU (a sensor's DTC
    // precondition, a module arm condition). Same dialog and same grammar as onEditCondition — it is told
    // the program block size so its feedback can report compile errors and how much of the block is used.
    static inline std::function<void(std::string current, uint16_t blockSize, std::string element,
                                     std::function<void(std::string)> apply)> onEditExpression;

    // App-set hook: the edit-mode "Edit Presets…" item opens the settingselector preset-options editor (only
    // acts on a selected settingselector). Same shape as onEditCondition — wired to win.openModal.
    static inline std::function<void(std::string current, std::function<void(std::string)> apply)> onEditPresets;

    // App-set hook: the edit-mode "Edit Lines…" item opens the livegraph multi-line editor (acts on a selected
    // livegraph). Same shape — wired to win.openModal<LineEditorDialog>.
    static inline std::function<void(std::string current, std::function<void(std::string)> apply)> onEditLines;
    // "Properties…" on a run-mode control menu: bring the Properties dock up and point it at the selection.
    // A surface has no docks of its own, so the app answers this one.
    static inline std::function<void()> onShowProperties;

    // A WIDGET SAYING WHAT IT JUST DID. A control that acts on the ECU on its own schedule — the script
    // editor's "Apply to ECU", which sends a whole script in one go — has to be able to report that it
    // landed, or refused, and a canvas has no status bar of its own. Static like the rest of these: the
    // message has no state and every surface shares the one window. Unset = silent.
    static inline std::function<void(const std::string& message)> onWidgetStatus;

    // OPERATOR-CONFIRMED actions: rewriting a base map from a learned surface, or discarding one. Neither
    // is something to discover afterwards. Surface has no window of its own, so the confirmation is asked
    // through the app the same way every other dialog here is.
    //
    // `verb` LABELS THE BUTTON, and it is not decoration. A confirmation whose accept button says "OK"
    // or "Apply" makes the reader carry the question in their head down to the buttons; one that says
    // "Reset" or "Apply to Base Table" can be answered by reading the button. Empty falls back to OK.
    static inline std::function<void(std::string title, std::string detail, std::string verb,
                                     std::function<void()> onConfirm)> onConfirmAction;
    // Pick a file for a table save/load. A Surface has no window, so the app opens the dialog — the same
    // arrangement as onAxisSetup and the expression editor.
    static inline std::function<void(std::string title, bool save,
                                     std::function<void(std::string)> then)> onPickFile;

    // App-set hook: the edit-mode "Panel Contents…" item opens the panel child-list editor (acts on a selected
    // panel). Same shape — wired to win.openModal<PanelContentsDialog>.
    static inline std::function<void(std::string current, std::function<void(std::string)> apply)> onEditPanel;

    // App-set hook: the edit-mode "Value Ranges…" item opens the colour-band editor for the selected
    // control's "ranges" prop (any control — the range cascade covers the whole widget set). Same shape.

    // App hook: a viewport's "Open in Separate Tab" item (ports PanelView::viewportActivated → open a tab
    // rooted at the node's page). Wired in main.cpp to SurfaceTabs::openNode. Static — the menu has no state.
    static inline std::function<void(std::string nodePath)> onOpenNodeTab;

    // Fired on any real (non-no-op) surface edit, so the app can mark the document dirty for the
    // save-changes prompt. App-installed; unset = ignored.
    static inline std::function<void()> onModified;

    // Fired for each node whose viewport placement was just deleted, so the app can GC the node's page if
    // that was its LAST viewport anywhere. Unset = ignored.
    static inline std::function<void(const std::string& node)> onViewportRemoved;

    // Access to node pages (viewport targets), set by SurfaceTabs: resolve a node path to its editable page,
    // and persist after an in-place edit. Lets a viewport be ENTERED (double-click) to author its node's page
    // right here — no tab — with every viewport of that node updating live (they share the page).
    // FIT THE WHOLE PAGE INTO THE VIEW, whatever the document says. A page authored as a fixed 1:1
    // canvas overflows a small area and grows scroll bars; the difference report cannot use those (it
    // routes no mouse into the page on purpose) and a difference off the edge would be invisible. So
    // the report asks for scale-to-fit and gets it without editing the document every tab shares.
    void setFitToView(bool f) { fitToView_ = f; invalidate(); }
    bool fitToView() const { return fitToView_; }

    void setPageAccess(std::function<PanelModel*(const std::string&)> resolver, std::function<void()> onChanged) {
        pageResolver_ = std::move(resolver); onPageChanged_ = std::move(onChanged);
    }
    bool inScope() const { return !scopes_.empty(); }   // editing an entered viewport's page in place

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    void handleMousePress(float mx, float my) override;
    void handleMouseMove(float mx, float my) override;
    void handleMouseRelease(float mx, float my) override;
    bool handleScroll(float mx, float my, float wheel) override;   // wheel/drag pan when the canvas overflows
    // THE SIZE THIS PAGE WANTS TO BE: its canvas, which is the size it was authored (or imported) to fit
    // in. A host that opens a window around a page asks this rather than guessing, so the page comes up
    // whole - nothing clipped and nothing to scroll. Ask for less and the page scrolls; ask for more and it has room to spare.
    // The width a scroll bar occupies, mirrored from Surface.cpp's kSB — preferredSize has to leave room
    // for one before it knows whether there will be one.
    static constexpr float kScrollGutter = 10.f;   // = Surface.cpp kSB
    jf::JRect preferredSize() const override {
        if (!model_) return bounds();
        // The declared minimum when the page has one — that IS the size it opens whole at. Without one
        // (a lamp grid, a viewport host) there is nothing to be whole at, so the canvas is the best
        // answer available.
        // DECLARED, NOT MEASURED. This measured the page instead — the far edge of its widgets, each
        // asked what it needed — and that under-reported: a panel reports the box it was drawn in while
        // its children overflow it, so a window opened smaller than its own content and scrolled at once,
        // and opened a DIFFERENT size the second time because the widgets were warm by then. A size that
        // depends on whether the page has been opened before is not a size.
        const float w = model_->minW() > 0.f ? model_->minW() : model_->effCanvasW();
        // …PLUS THE GUTTER A SCROLL BAR WOULD SIT IN. A page opened at exactly its own width has none to
        // spare: the moment it is a few pixels too tall, the vertical bar takes its width out of the
        // content, the content no longer fits across, and a horizontal bar appears to answer for the
        // width the vertical one took. One bar breeding another over ten pixels. A gutter's worth of
        // slack on each axis costs nothing when it is not needed and stops that outright.
        // IN PIXELS, NOT PAGE UNITS. A page's coordinates are authored units that the interface scale
        // multiplies on the way to the screen — 1.04 on this display, more on a hidpi one — so a window
        // sized from the raw numbers came out that fraction too small for its own content, every time.
        // It looked like a rounding error and behaved like one: a scroll bar on a page that fits, the
        // last few pixels of every value box clipped off, "1500 rpm" reading "1500 rp".
        const float sc = jf::JStyle::uiScale() > 0.f ? jf::JStyle::uiScale() : 1.f;
        // NO GUTTER ADDED. A page asked for its own width plus ten pixels "so a scroll bar has somewhere to
        // sit", which guarantees the one thing it was meant to avoid: the space beside the instruments is
        // laid out to fit the page EXACTLY (1280 of a 1600 surface), so asking for 1290 overflowed it by
        // ten and produced the horizontal scroll bar it was supposed to prevent. A page's size is a page's
        // size; if a bar is needed the surface already reserves it out of the content.
        return { 0.f, 0.f, w * sc, model_->effCanvasH() * sc };
    }
    bool handleKeyEvent(const jf::JKeyEvent& ke) override;
    void onFocusEvent(bool focused) override;   // the canvas is a nested focus domain (see Surface.cpp)
    void prepareContextMenu(float mx, float my) override;   // select-the-hit + refresh item enable states

    // Edit operations — used by the context menu + shortcuts.
    enum class AlignMode { Left, Right, Top, Bottom, CenterH, CenterV, Center };
    // Arrange operations + multi-select, driven by the context menu — public so the behaviour can be
    // exercised directly (a group must arrange as ONE item) instead of through synthesised input.
    void      selectOnly(int id);                     // id==0 clears
    void      selectAlso(int id);
    void      alignSelection(AlignMode m);
    void      distributeSelection(bool horizontal);
    void      applySpacing(bool horizontal, bool pitch, float value);
    enum class SizeMode  { Width, Height, Both };
    void      matchSize(SizeMode m);   // widgets only — refuses when a group is selected


    // Undo bridge for external editors (the Properties dock): snapshot the model before an edit, then
    // commit the swap as one history step. mergeId>=0 coalesces a burst (e.g. per-keystroke) into one.
    std::vector<PanelElement> elementsSnapshot() const { return model_ ? model_->elements() : std::vector<PanelElement>{}; }
    void applyExternalEdit(const std::string& text, const std::vector<PanelElement>& before, int mergeId) { commitEdit(text, before, mergeId); }
    // Same, for the Surface-canvas properties (separate model fields, not in elements_).
    PanelModel::CanvasState canvasSnapshot() const { return model_ ? model_->canvasState() : PanelModel::CanvasState{}; }
    void applyCanvasEdit(const std::string& text, const PanelModel::CanvasState& before, int mergeId);

    // Edit-menu entry points (also used by the context menu / shortcuts).
    void menuUndo()   { doUndoRedo(false); }
    void menuRedo()   { doUndoRedo(true); }
    void menuCopy()   { copySelection(); }
    void menuCut()    { edit("Cut",    [this]{ cutSelection(); }); }
    void menuPaste()  { edit("Paste",  [this]{ pasteClipboard(); }); }
    void menuDelete() { edit("Delete", [this]{ deleteSelection(); }); }
    // Drill into a viewport → edit its node's page in place. Reached by double-click; public because it is
    // an edit operation like the menu ones, and because paste behaves differently once you are inside a page.
    void      enterViewport(int elemId);

    void menuSelectAll() { if (!model_) return; selection_.clear(); for (const auto& e : model_->elements()) selection_.push_back(e.id); selectionChanged.emit(); invalidate(); }
    // Add a control (from the palette dock) at the view centre, as one undo step.
    void addControl(const std::string& type) {
        if (mode_ != Mode::Edit || !model_) return;
        const auto b = getBoundingBox();
        hoverX_ = b.x + b.width * 0.5f; hoverY_ = b.y + b.height * 0.5f;   // place at centre
        edit("Add", [this, type] { addWidgetAt(type); });
    }
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }

    // The persistent CanvasWidget instance for a ported element type (else nullptr). The Properties dock
    // introspects it — its JPropertyModel is the authoritative, typed schema for the inspector's rows.
    CanvasWidget* widgetInstance(const PanelElement& el) { syncInstances(); return instanceFor(el); }
    // The live top-level instance for a model element id (the Properties dock authors edits straight onto it).
    CanvasWidget* widgetById(int id) { syncInstances(); return canvas_.instanceFor(id); }
    // EVERY TOP-LEVEL INSTANCE ON THIS SURFACE. A container's own children are not here — they hang off
    // CanvasWidget::childWidgets() — so a caller that wants the whole live tree walks down from these.
    // The difference report does exactly that: it has to find the WIDGET a binding is drawn by, and the
    // model cannot answer that, because a panel's children are JSON inside the panel rather than
    // elements of the page.
    std::vector<CanvasWidget*> instances() {
        syncInstances();
        std::vector<CanvasWidget*> v;
        v.reserve(canvas_.instancesById().size());
        for (const auto& [id, w] : canvas_.instancesById()) if (w) v.push_back(w);
        return v;
    }
    // Which viewport is under a point, or 0. A pure query, public so the double-click path can be
    // tested: it is the FIRST of the two gates a drill-in passes, and testing only the second one
    // proved nothing when the first was rejecting every unbound viewport.
    int       viewportElAt(float mx, float my) const;
    // Where an element sits on screen right now: the same rect the paint places it at and input routing
    // hit-tests against, so a caller outside the surface (a test, a popup anchor) asks the one authority
    // rather than re-deriving the camera transform and drifting from it.
    jf::JRect screenRectOfId(int id) const;

    // The element this surface is editing INSIDE — the data source of the viewport that was entered, or
    // "" at the top level. It is what a template page's "[*]" means here, so anything asked to evaluate
    // an expression authored on this page needs it (the expression builder's live test).
    const std::string& elementScope() const { return scopeCtx_; }

    // @-sigil resolution against the LIVE widget tree (top-level AND nested container children). widgetByUid
    // resolves an address (stable uid / legacy "<type>_<id>") to its widget anywhere in the tree; collectSigilTokens
    // enumerates every addressable "<uid>.<prop>" for the sigil picker. Both sync the tree first.
    CanvasWidget* widgetByUid(const std::string& addr);
    // Is `addr` a UID this surface can resolve? A membership test against an index rebuilt when the model
    // changes — NOT a tree walk. The @-sigil asks this before reading a value, and it is asked for every
    // dotted token in every expression on every frame: as a walk (syncInstances + findWidget over every
    // instance and its mirrored children) it cost 3.7ms a token, which was the whole frame budget.
    bool hasUid(const std::string& addr) const;
    void          collectSigilTokens(std::vector<std::string>& out);

    // WHERE A WIDGET IS ON SCREEN. The world->screen affine and the rect it produces, exposed because a
    // test that synthesises a click has to aim it the way the app does — the transform depends on the
    // live scope (an entered panel/viewport has its own canvas and a breadcrumb strip), so a test that
    // recomputed it would be testing its own arithmetic instead of the surface's. Same reason the
    // selection helper below is public.
    using Xform = SurfaceCamera::Xform;   // the world->screen affine — SurfaceCamera owns the math now
    Xform     xform() const;              // thin wrapper: camera().xform() (keeps the !model_ early-out)
    jf::JRect toScreen(const PanelElement& e, const Xform& t) const;

private:
    SurfaceCamera camera() const;         // gather the surface's live state into the pure transform
    // Scroll bars — present only when the effective canvas overflows the viewport (fixed/static surfaces).
    // Track = the full gutter; thumb = the draggable proportion. All in screen space.
    using ScrollBars = SurfaceCamera::ScrollBars;
    ScrollBars scrollBars(const Xform& t) const;
    void      dragScrollThumb(float mx, float my);   // map a thumb drag to scrollX_/scrollY_
    jf::JRect pageRect(const Xform& t) const;
    jf::JRect screenRectOf(size_t index, const Xform& t) const;   // honours the managed layout mode
    std::pair<float, float> titleInset() const;                   // (top,bottom) content inset for the title bar (managed modes)
    int       hitTest(float mx, float my, const Xform& t) const;
    int       panelElAt(float mx, float my) const;      // id of a "panel" element under the cursor, else 0
    bool      inResizeGrip(float mx, float my, const jf::JRect& r) const;
    bool      isSelected(int id) const;
    // Add one element to the selection (Ctrl-click parity), expanding to its group like every other
    // selection path. Public so the arrange behaviour can be tested without synthesising mouse events.

public:
    // Drill into a panel and edit its children on-canvas in place; exitScope pops back out (Esc /
    // breadcrumb), serialising the scope back into the panel. Public because what a scope carries in and
    // out is worth testing without synthesising a double-click and an Esc.
    void      enterPanel(int elemId);
    void      exitScope();
    // Place a control at the last cursor position, with its caption — the drop path, and what a drop is
    // BORN with (its type's Widget Defaults, and the caption's) is worth testing without a drag.
    void      addWidgetAt(const std::string& type, const std::string& caption = "");
private:
    void      placeLabelsFor(int controlId, const std::string& type, const std::string& caption);   // the caption only
    std::string defaultControlFor(const std::string& bindPath) const;   // best control type for a dropped binding
    bool      tryDrop_(float mx, float my);   // resolve an active JDragDrop (binding/palette) at (mx,my)
    void      buildContextMenu();                     // full edit-mode menu (Add/Copy/…/Align/Distribute/Size)
    void      refreshMenuState();                     // enable/disable items by selection + clipboard
    // Open an app-wired modal (onEditCondition/-Presets/-Lines/-Panel) on the selected control's prop.
    // The condition editor's live test evaluates INSIDE the page's element, so it — alone among the prop
    // editors — has to be told which one. Adapted to the common hook shape here, so the other four are not
    // widened with a parameter they would ignore.
    std::function<void(std::string, std::function<void(std::string)>)> conditionHook();

    void      openPropEditor(const std::function<void(std::string, std::function<void(std::string)>)>& hook,
                             const std::string& prop, const std::string& text);
    void      expandToGroups();                       // add every group-mate of the current selection
    bool      selectionHasGroup() const;
    // The widget every arrange op measures from: the one the context menu was invoked on. Falls back to
    // the selection's first element when the op arrives from anywhere else (a shortcut, a stale target).
    const PanelElement* arrangeRef() const;
    // The selection as ARRANGE UNITS: each group is one unit (its members move together), each ungrouped
    // element is its own. A group is documented as "select/move/delete as one unit", and an arrange op
    // that walks elements individually breaks exactly that — it drags every member to the same edge and
    // flattens the group's internal layout.
    std::vector<std::vector<int>> arrangeUnits() const;
    JRect                         unitBox(const std::vector<int>& ids) const;   // bounding box of a unit
    std::vector<int>              unitOf(int id) const;      // the whole group of `id`, or just `id`
    // Is this element hidden by the node view-switcher? A "node" prop scopes a TOP-LEVEL viewport to the
    // tree selection. INSIDE a page (in scope) the elements ARE that page's content, so a nested viewport
    // is shown by its host, not by the tree — gating those on the global active node is what made a
    // viewport pasted into another node's page permanently invisible: while its own node was active you
    // were looking at a different page, and while you were on this page it was hidden.
    bool      nodeHidden_(const PanelElement& el) const;
    // An element's binding AS IT RESOLVES HERE: "[*]" against the subject in force. For an element of the
    // page being edited that is the entered viewport's subject; for one mirrored inside a viewport it is
    // that viewport's. Every test of "is this a table/curve" and every table/curve OP must ask through
    // this — reading the raw prop meant a template's page had no context menu and no axis setup, because
    // isTable("sensors.sensor[*].cal") is false.
    std::string bindOf_(const PanelElement& el, const std::string& ctx) const;
    std::string opBind_(const PanelElement& el) const;   // …for the op target (uses opViewportId_)
    // Read/write a property of the table the run-mode menu targets, wherever that table actually lives.
    std::string tableProp_(const std::string& key) const;
    void setTableProp_(const std::string& key, const std::string& value);
    // …against a target captured EARLIER, for a caller that answers after the menu state has moved on.
    void setTablePropOn_(PanelModel* model, int elementId, int panelId, const std::string& childUid,
                         const std::string& key, const std::string& value);
    // Explicit spacing: lay the selection out along one axis at a typed step, anchored on the first
    // (topmost / leftmost) element so nothing jumps away from where you put it. pitch = origin-to-origin
    // (a 20 step on 18px rows leaves 2px of air); !pitch = edge-to-edge gap.
    void      armSpacing(bool horizontal, bool pitch);
    // Armed spacing entry: the number is typed on-canvas (same idiom as the table's "Set to…" op) rather
    // than through a modal, so it needs no dialog plumbing in the host. 0 = not armed.
    int         spacingAxis_ = 0;        // 0 none, 1 vertical, 2 horizontal
    bool        spacingPitch_ = true;
    std::string spacingBuf_;
    void      groupSelection();
    void      ungroupSelection();
    void      copySelection();
    void      cutSelection();
    void      pasteClipboard();
    void      deleteSelection();
    void      updateMarquee();                        // select elements intersecting the marquee
    std::pair<float,float> snapDelta(float x, float y, float w, float h) const;   // grid + neighbour magnetism
    void      snapSize(float x, float y, float& w, float& h) const;      // grid snap (resize): edge, not size
    const PanelElement* origOf(int id) const;                            // element's rect at drag start
    void      doUndoRedo(bool redo);                                             // apply + prune selection + refresh
    void      edit(const std::string& text, const std::function<void()>& fn);  // run fn as one undoable step
    void      commitEdit(const std::string& text, const std::vector<PanelElement>& before, int mergeId = -1);
    void      drawGrid(jf::JPrimitiveBuffer& buf, const jf::JRect& page, const Xform& t) const;
    void      drawFrame(jf::JPrimitiveBuffer& buf, const jf::JRect& page) const;   // border + title (PanelView::paintBorder)
    void      drawSelection(jf::JPrimitiveBuffer& buf, const jf::JRect& r, bool grip, bool grouped = false) const;
    // Run-mode interaction: deliver an input event to the interactive control under the cursor (or, for
    // Key events, the focused control). Returns true if a control consumed it.
    bool      routeRunInput_(ControlInput::Kind kind, float mx, float my, float wheel, const jf::JKeyEvent* key);
    bool      runTarget_(const PanelElement& el) const;   // shown (node+condition) and not enable-disabled
    // Run-mode table context menu. Operates on the table
    // under the cursor + its rubber-band cell block.
    void      buildTableMenu();
    int       tableElAt(float mx, float my) const;   // element id of a bound Table under (mx,my), else 0
    void      buildCurveMenu();
    int       curveElAt(float mx, float my) const;   // element id of a bound Curve under (mx,my), else 0
    // The multi-channel controls — a watch list ("channels") or a trace view ("livegraph"). Their menus are
    // short because the question is short: which channels, and (for a trace view) is it still wanted here.
    void      buildChannelMenus();
    int       chanElAt(float mx, float my, const char* wantType) const;
    const PanelElement* opChanElement() const;
    void      openChannelPicker();     // the two-list picker, on whichever of the two was right-clicked
    void      removeChanElement();     // "Remove Trace View from Page"
    void      openChannelProps();      // "Properties…" — the unit/scale/warning bands of ITS channels
    void      tableToFile(bool save);  // "Save to File…" / "Load from File…" on the table it targets
    std::vector<std::string> chanChannelsOf_(const PanelElement& el) const;
    void      curveInsertPoint();                    // insert a midpoint breakpoint next to the selection
    void      curveDeletePoint();                    // delete the last breakpoint (honours the schema floor)
    void      curveLinearise();                      // straight-line the whole curve (X even-spaced, Y ramp)
    void      tableToggleCellTrace();                // flip the live-telemetry cell-trace cursor on/off
    void      tableDistributeBins(int axis, int mode);   // redistribute an axis's breakpoints (0 even/1 centre/2 ends)
    void      tablePlaneStep(int delta);             // change the active Z plane on a 3D table
    void      tableCopyPlaneToAll();                 // copy the current Z plane onto every other plane
    void      tableCopyPlaneTo(int z);               // copy the current Z plane onto one specific plane z
    void      rebuildPlaneCopyToMenu();              // repopulate "Copy this plane to…" for the table under the cursor
    void      syncStatefulChecks(const PanelElement* tableEl, const PanelElement* panelEl);   // ● the active View/Orientation/Layout
    void      tableInterpolatePlanes();              // linear-blend the intermediate Z planes (first→last)
    void      transformTableBlock(int elId, const std::function<void(std::vector<std::vector<double>>&)>& fn);
    void      tableArmOp(int op);                    // arm Set/Increase/Decrease/% + focus the table for inline entry
    void      tableBinOp(bool row, bool del);        // insert/delete a row (vertical axis) or column (horizontal) at the active cell
    void      tableSetOrientation(int combo);         // set the table's axis orientation combo (0..7, AxisLayout)
    void      tableSetView(int view);                 // 0 2D grid, 1 3D surface, 2 slice (property "view")
    void      tableCopyCells();                       // copy the selection block to the cell clipboard
    void      tablePasteCells();                      // paste the clipboard anchored at the active cell
    void      reportClamped_(const char* what);       // emit statusMessage if the last bulk op clamped anything
    void      tableCopyTable();                       // copy the whole grid to the system clipboard as TSV
    void      tablePasteTable();                      // paste a TSV grid from the system clipboard into the cells
    void      tableBumpDecimals(int delta);           // increase/decrease the table's displayed decimal places
    void      tableLinearise(bool horiz, bool vert);
    void      tableSmooth();
    void      tableApplyToBase();     // roll a learned surface into the base map it corrects, then reset it
    // …and throw it away instead. The other half of the same workflow: a surface learned against a
    // map that has since been re-tuned is evidence about an engine that no longer exists, and the
    // only way to be rid of it was to type zeros into every cell by hand.
    void      tableResetToZero();
    void      applyPreset(const std::string& file);
    void      tableRestore(bool baseline);           // baseline=false: defaults; true: connect-point

    // Persistent widget tree: ONE CanvasWidget instance per top-level element, lifetime = the model. Keyed by
    // element id; the instance's elementType() tracks its type, so a paste/undo that reuses an id for a
    // DIFFERENT type rebuilds it. Container instances (panel/viewport) own THEIR children recursively — the
    // whole tree exists as real objects, so off-screen / non-active-tab / nested widgets have a home for the
    // @-sigil and live state (10,000 persistent instances is intended). The CONTAINER (canvas_) OWNS the
    // instances now (Qt model, via the framework adopt() primitive); the Surface drives their reconciliation
    // below and looks them up through canvas_.instanceFor()/instancesById().
    uint64_t syncedGen_ = ~0ull;                         // model generation last synced into the instances (~0 = never)
    void syncInstances();                                // reconcile canvas_'s instances to the current model_->elements()
public:
    void commitInstances();                              // save-time: flush every instance's owned state into the models
    void flushScopes();                                  // …and every OPEN panel scope back into its panel
    void refreshInstancesIfChanged();                    // rehydrate instances when model_->generation() moves (render loop drives this; also callable by tests)
private:
    CanvasWidget* instanceFor(const PanelElement& el);   // PURE lookup — the instance for an id, else nullptr
    // Child paint: prep + register every instance as a container node, paint them through the framework
    // container, then draw the run-mode disabled wash / selection / group-box overlays ON TOP (fills groupBoxes).
    void paintElements_(jf::JPrimitiveBuffer& buf, const Xform& t, bool editing,
                        std::unordered_map<int, jf::JRect>& groupBoxes);

    const Cache&          cache_;
    PanelModel*           model_ = nullptr;
    Mode                  mode_ = Mode::Edit;
    SurfaceCanvas         canvas_;                 // the framework container that owns + paints the instances
    std::string           activeNode_;   // selected dashboard node; gates node-tagged elements (view-switcher)
    std::vector<int>      selection_;

    // In-place viewport editing (ports PanelView's enter-a-nested-viewport). Entering a viewport rebinds
    // model_ to that node's page (from the store) and drills in; the whole surface then edits that page.
    // scopes_ holds the parent chain to restore on exit; each frame draws a breadcrumb + Esc leaves.
    // A suspended parent while editing a container in place. A viewport scope re-enters an existing node page
    // (owned == null, model_ points at the live page). A panel scope edits a TEMPORARY model built from the
    // panel's "children" JSON (owned holds it, panelElemId names the panel to serialise back to on exit).
    struct EditScope { PanelModel* model = nullptr; std::string nodePath; std::unique_ptr<PanelModel> owned; int panelElemId = 0;
                       std::string elemCtx;      // the subject in force before entering, restored on exit
                       // WHICH ELEMENT was drilled into, whatever kind it is. panelElemId says "panel scope,
                       // and which" — a viewport scope recorded only its node path, so the chain could not be
                       // re-entered from what the scope itself knew.
                       int srcElemId = 0; };
    std::vector<EditScope>                         scopes_;         // suspended parents (model_ = deepest child)
    void writeScopeBack_(const EditScope& s);   // one scope's temp model -> its panel's "children" prop
    // WHERE YOU WERE EDITING, across a trip through run mode. In-place editing is an edit-mode thing, so
    // switching to Live pops every scope — and popping it is right, since a scope is a page you are INSIDE
    // and Live shows the dashboard. Coming back put you at the top level, though, which is not where you
    // left: you had to find the viewport and double-click into it again, every time you glanced at the
    // live values. The element ids of the chain are kept here and replayed on the way back.
    struct SuspendedScope { int elemId = 0; bool panel = false; };
    std::vector<SuspendedScope>                    suspended_;      // outermost first; empty unless in Run
    // WHICH SENSOR the page being edited is about — the entered viewport's Data Source. Empty at the top
    // level. Pushed into every instance the surface builds so a template page resolves while you edit it.
    std::string                                    scopeCtx_;
    std::function<PanelModel*(const std::string&)> pageResolver_;   // node path → its editable page
    bool                                           fitToView_ = false;   // see setFitToView()
    mutable bool                                   reflow_    = false;   // canvasStatic 3: the page IS the view
    std::function<void()>                          onPageChanged_;  // persist after an in-place page edit
    jf::JRect                                      breadcrumbRect_{};   // clickable "leave" region (screen)

    // In-place label caption editing (edit mode). inlineEdit_ = the element id whose text is being typed
    // (0 = none). inlineBefore_ is the elements snapshot taken when editing began, so the whole session —
    // text plus every live resize step — collapses into ONE undo entry on commit. inlineW0_/inlineH0_
    // restore the box on Escape, since the live resize writes straight to the model.
    int         inlineEdit_ = 0;
    std::vector<PanelElement> inlineBefore_;
    float       inlineW0_ = 0.f, inlineH0_ = 0.f;
    void        beginLabelEdit_(int id);
    void        commitLabelEdit_();
    void        cancelLabelEdit_();
    void        fitLabelToText_();          // grow/shrink the edited element to the text so far

    enum class Drag { None, Move, Resize, Marquee, ScrollH, ScrollV, TextSelect } drag_ = Drag::None;
    float scrollX_ = 0.f, scrollY_ = 0.f;   // canvas pan (only meaningful when the canvas overflows)
    float scrollGrab_ = 0.f;                // cursor offset within the thumb at drag start
    float lastMx_ = 0.f, lastMy_ = 0.f;
    float pressMx_ = 0.f, pressMy_ = 0.f;   // mouse at drag start (screen) — raw-accumulate the drag
    std::chrono::steady_clock::time_point lastClickT_{};   // double-click detection (enter a viewport)
    float lastClickX_ = 0.f, lastClickY_ = 0.f;
    float hoverX_ = 0.f, hoverY_ = 0.f;                          // last cursor pos (context-menu placement)
    float mqX_ = 0.f, mqY_ = 0.f, mqCurX_ = 0.f, mqCurY_ = 0.f;   // marquee anchor + current (screen)
    jf::JMenu menu_{ "Surface" };                                // edit-mode context menu
    jf::JMenu tableMenu_{ "Table" };                             // run-mode table context menu
    jf::JMenu curveMenu_{ "Curve" };                             // run-mode curve context menu
    jf::JMenu chanListMenu_{ "Channels" };                       // run-mode watch-list menu
    jf::JMenu graphMenu_{ "Trace View" };                        // run-mode trace-view menu
    mutable int chanElUnder_ = 0;                                // the multi-channel control the menu targets
    mutable std::string chanTargetUid_;
    std::unique_ptr<jf::JMenu> addMenu_, alignMenu_, distMenu_, sizeMenu_, panelLayoutMenu_, spacingMenu_;   // submenus (owned)
    std::unique_ptr<jf::JMenu> arrangeMenu_, editMenu_;   // base-menu task groups: Arrange ▸ (spatial) / Edit ▸ (authoring)
    std::unique_ptr<jf::JMenu> linMenu_, viewMenu_, orientMenu_, planeMenu_, distBinsMenu_;   // table submenus (owned)
    std::unique_ptr<jf::JMenu> editValuesMenu_, rowsColsMenu_, clipboardMenu_, displayMenu_, setupMenu_;  // table task groups (owned)
    // A PRESET IS A SAVED CALIBRATION, not a hard-coded table. The curve file format already records
    // which table it came from and in what units, so a folder of them IS the preset library — adding a
    // controller means dropping a file in it, and no code changes. Rebuilt per open, because the list
    // depends on which table the click landed on.
    std::unique_ptr<jf::JMenu> presetMenu_;
    void rebuildPresetMenu(const std::string& path);
    static std::string calibrationDir();
    static std::string shippedCalibrationDir();
    std::unique_ptr<jf::JMenu> planeCopyToMenu_;   // "Copy this plane to…" — items rebuilt per open (plane count varies)
    // Units ▸ for ANY bound control, in run mode. Rebuilt on every open because the choices are the
    // quantity's, and the quantity is whatever the control under the cursor happens to be showing.
    jf::JMenu                  unitsRootMenu_{"Units"};
    int                        unitsElUnder_ = 0;
    std::string                unitsChannel_;      // the channel the choice is remembered against
    void rebuildUnitsMenu();
    int menuTargetId_ = 0;   // the element the edit-mode context menu was invoked ON — the widget you
                             // right-clicked. "Make same size" measures THIS one, so the reference is the
                             // one you pointed at rather than an invisible artefact of selection order.
    int tableElUnder_ = 0;                                       // table the run-mode menu currently targets
    int curveElUnder_ = 0;                                       // curve the run-mode menu currently targets
    // The model the table/curve ops operate on: this surface's model_ for a top-level table, or a viewport's
    // mirrored node-page model when the target lives inside a viewport. Set by tableElAt/curveElAt.
    mutable PanelModel* opModel_ = nullptr;
    PanelModel* opModel() const { return opModel_ ? opModel_ : model_; }
    mutable int opViewportId_ = 0;   // top-level viewport hosting the target when it's mirrored (0 = top-level)
    // ctx: the subject in force where this panel is drawn, so a template page's binding resolves here too.
    const PanelElement* _panelChildAt(CanvasWidget* panelWidget, const jf::JRect& rect,
                                      float mx, float my, const char* wantType, const std::string& ctx) const;
    // THE element a table/curve menu action operates on. Resolved by UID first: a panel's child is not an
    // element of any model, and its id is only unique WITHIN that panel — looking it up in the model would
    // silently return a different element with the same number and operate on the wrong table.
    const PanelElement* opTableElement() const;
    std::string         inlineEditUid_() const;   // uid of the caption open for editing ("" = none)
    std::string         opTableUid_() const;   // …and its uid, which is what per-widget state is keyed by
    const PanelElement* opCurveElement() const;
    mutable std::string curveTargetUid_;
    int _indexOf(int id) const;
    // The PANEL that owns the target table, when the target is a panel CHILD (0 = it is a page element).
    // A panel's children are not model elements — they live as JSON in its "children" prop, with their own
    // id space — so a prop set on the page model using a child's id lands on whatever page element happens
    // to share that number. setTableProp_ / tableProp_ are what keep that from happening.
    mutable int tablePanelId_ = 0;
    mutable std::string tableTargetUid_;   // the table under the cursor, by its OWN uid — the single identity the
                                           // run-mode menu addresses via widgetByUid(); immune to opModel_ clobbering
    jf::JMenuItem* miCellTrace_ = nullptr;                       // checkable "Cell Trace" item (synced on menu open)
    // The two LEARNED-TRIM operations. They apply to a table that declares a base map and to no
    // other, so they are hidden rather than left to no-op: an item that is always present and
    // usually does nothing teaches people that the menu lies.
    jf::JMenuItem* miApplyToBase_ = nullptr;
    jf::JMenuItem* miResetLearned_ = nullptr;
    jf::JMenuItem* miCondition_ = nullptr;                       // edit-mode "Condition…" item
    jf::JMenuItem* miPresets_ = nullptr;                         // edit-mode "Edit Presets…" item (settingselector)
    jf::JMenuItem* miLines_ = nullptr;                           // edit-mode "Edit Lines…" item (livegraph)
    jf::JMenuItem* miPanel_ = nullptr;                           // edit-mode "Panel Contents…" item (panel)
    jf::JMenuItem* miPanelLayout_ = nullptr;                     // edit-mode "Panel Layout ▸" submenu item (panel)
    jf::JMenuItem* miPanelEdit_ = nullptr;                       // edit-mode "Edit Panel in Place" item (panel)
    jf::JMenuItem* miTableSub_  = nullptr;                       // edit-mode "Table ▸" submenu (selected table)
    jf::JMenuItem* miCurveSub_  = nullptr;                       // edit-mode "Curve ▸" submenu (selected curve)
    jf::JMenuItem* miEnable_ = nullptr;                          // edit-mode "Enable Condition…" item (any control)
    jf::JMenuItem* miCopy_ = nullptr; jf::JMenuItem* miCut_ = nullptr; jf::JMenuItem* miPaste_ = nullptr;
    jf::JMenuItem* miDelete_ = nullptr; jf::JMenuItem* miFront_ = nullptr; jf::JMenuItem* miBack_ = nullptr;
    jf::JMenuItem* miFwd_ = nullptr;    jf::JMenuItem* miBwd_ = nullptr;
    jf::JMenuItem* miGroup_ = nullptr; jf::JMenuItem* miUngroup_ = nullptr;
    jf::JMenuItem* miAlign_ = nullptr; jf::JMenuItem* miDist_ = nullptr; jf::JMenuItem* miSize_ = nullptr;
    jf::JMenuItem* miSpacing_ = nullptr;
    jf::JMenuItem* miArrange_ = nullptr; jf::JMenuItem* miEdit_ = nullptr;   // parent items for the two task-group submenus
    // Checkable groups that mirror a widget's CURRENT discrete state (a ● beside the active option; synced on
    // menu open). View/Orientation reflect the table under the cursor; Panel Layout the selected panel.
    jf::JMenuItem* viewItems_[3] = {};          // 2D Grid / 3D Surface / Slice  → table "view"
    jf::JMenuItem* orientItems_[8] = {};        // AxisLayout combos 0..7         → table "axisMode"
    mutable std::unordered_set<std::string> uidIndex_;   // @-addressable uids, by model generation
    mutable uint64_t                        uidGen_ = ~0ull;
    jf::JMenuItem* panelLayoutItems_[8] = {};   // Free/Y/X/Border/Card/Index/Grid/Wrap → panel "layoutMode"
    static std::vector<PanelElement> clipboard_;                 // shared across surfaces (session)
    static int pasteSeq_;   // pastes since the last copy — each one steps further from the source
    static std::vector<std::vector<double>> tableClip_;          // copied table cell block (session)
    jf::JUndoStack             undo_;                            // per-surface edit history (Ctrl+Z/Y)
    std::vector<PanelElement>  dragStart_;                       // element snapshot at drag begin
    int  activeControl_ = 0;   // run-mode: element id of the keyboard-focused interactive control (0 = none)
    void setActiveControl_(int id, bool announce = false);   // move it, blurring the outgoing control (see Surface.cpp)
    void _announce_(int id, ControlInput::Kind kind);        // deliver Blur/Focus to one element's instance
    bool focusNextControl_(int dir);  // Tab/Shift-Tab across the canvas's interactive controls
    bool runDrag_ = false;     // run-mode: a press on an interactive control is dragging

    // A press arms Move/Resize, but the geometry must not change until the pointer has actually TRAVELLED:
    // otherwise the click's own 1px jitter runs the snap path with a zero delta and a plain SELECT yanks the
    // widget onto the nearest grid line. armDrag_ latches true once past kDragSlop and stays true for the drag.
    bool dragArmed_ = false;
    bool armDrag_(float mx, float my);

    static constexpr float kMinW = 48.f, kMinH = 28.f, kGrip = 14.f, kGridV = 20.f, kSB = 10.f;
    static constexpr float kDragSlop = 3.f;   // screen px of travel before a press becomes a drag
};

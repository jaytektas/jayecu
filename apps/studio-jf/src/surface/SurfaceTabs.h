#pragma once

// SurfaceTabs — the centre editor: a dynamic set of surface tabs. Each open tab hosts its own Surface
// (a custom widget in the framework's JTabWidget); closing a tab removes it from view but keeps the
// surface's definition in a pool, so it can be reopened. Edit/Run is app-wide across every surface.
// The framework owns the tab mechanics (add/switch/close/custom content); this owns the pool + docs.

#include <j/core/JWidget.h>
#include <j/core/JTabWidget.h>

#include "PanelLibrary.h"
#include "PanelModel.h"
#include "Surface.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

class Cache;

class SurfaceTabs : public jf::JWidget {
public:
    SurfaceTabs(jf::JSceneGraph& g, const Cache& cache, PanelLibrary& panelLibrary);

    // Create a surface definition and open it as a tab (returns its doc id). closeActive() removes the
    // current tab (the definition survives in the pool).
    int         newSurface(const std::string& name);
    void        closeActive();
    PanelModel* activeModel();
    Surface*    activeSurface();
    // …and make sure the tab in front is one that takes page windows (see the .cpp). False if none does.
    bool        showTabTakingPages();
    // WHERE THE ACTIVE TAB'S PAGE IS DRAWN — the rectangle the tab widget gives its content, asked of the
    // tab widget rather than read off the page. A page widget's bounds are stale until it has been laid
    // out at least once, so a tab shown for the first time answers with whatever it had before: the same
    // tab placed a window 34 px higher on its first visit than on every one after.
    jf::JRect   pageRect() const { return tabs_.contentRect(); }
    // THE PAGE THIS TAB OPENS ON when nothing has been opened on it yet — the "at" a pool entry carries,
    // which is where the tab was last left (seeded per workspace: the idle tab opens on Target RPM). Empty
    // for a tab with no default. Without this a tab that had not been visited since launch came up blank
    // while every other tab remembered its page, which reads as the tab being broken.
    std::string defaultPageOf(const Surface* tab) const;
    std::string activeName();      // the tab in front — the workspace the user is standing on

    // Open (or focus) a tab that edits a navigation-tree node's page — the model is the node's PanelModel in
    // the store, shared with every viewport of that node, so edits here flow to all its viewports live.
    void        openNode(const std::string& nodePath, const std::string& title);

    // Open (or focus) a non-Surface "tool" tab hosting a caller-owned widget (e.g. the Trigger Designer).
    // It is not a Surface, so activeSurface() is null while it's active and it is excluded from the surface
    // pool and from surfacesToJson() persistence.
    void        openTool(const std::string& title, jf::JWidget* content);
    // Is this tool's widget currently a tab? A tool that talks to the ECU has to stop when its tab
    // goes: closing the window is how a user says they are finished with it, and a capture left
    // running on the other side of the link is invisible from here for ever. onTabClosed reports an
    // INDEX, after the removal, so it cannot say which tool went — asking about the widget can.
    [[nodiscard]] bool toolOpen(const jf::JWidget* content) const;

    // A navigation node was renamed/reparented (oldPrefix → newPrefix): re-point every viewport on every
    // surface that referenced it, and follow the label on any open node-page tab.
    void        retagNodeRefs(const std::string& oldPrefix, const std::string& newPrefix);
    // A node subtree was deleted: forget any pooled node-page docs under it (their store pages are gone).
    void        forgetNode(const std::string& prefix);

    // The last viewport of a page was deleted: destroy the page AND its widgets, immediately. A page that
    // outlives its last viewport comes back on the next drag of that node, still holding the widgets you
    // thought you had deleted.
    void        gcNodeIfUnplaced(const std::string& node);
    // The same rule swept over the whole library at save — a backstop for anything the delete path missed.
    // Drop pages nothing can reach. `liveNodes` is the navigation tree's own path set, and it is the
    // criterion: a viewport shows whichever page the tree has selected, so "is some viewport pointed at
    // this page" stopped being an answer the moment one viewport could serve them all. Asked with an
    // empty set, this falls back to the placement rule — which is right only while every page has a
    // viewport of its own.
    void        pruneUnplacedPages(const std::set<std::string>& liveNodes = {});

    void          setMode(Surface::Mode m);          // app-wide edit/run
    void          setActiveNode(const std::string& node);   // tree selection scopes every surface's viewports
    Surface::Mode mode() const { return mode_; }

    // The active surface's selection changed (or the active surface itself changed). Carries the
    // active surface so an inspector can rebuild for it.
    jf::JSignal<Surface*> selectionChanged;
    jf::JSignal<std::string> statusMessage;   // forwarded from any hosted Surface (e.g. "12 cells clamped")
    // A TAB CARRIES ITS OWN PAGE, so arriving on one has to move the tree to match — otherwise the
    // selection and the surface disagree and the next click in the tree looks like it did nothing.
    // Carries the node the newly active tab is on; the app selects it without echoing back.
    jf::JSignal<std::string> nodeFollowed;

    // THE TAB IN FRONT CHANGED. Each tab shows its own page window — the idle tab its idle table, the
    // main tab whatever was last opened there — so the host has to swap the window over when the tab
    // does. Emitted with the surface now in front (null if none), on a real tab change only, so it
    // cannot be confused with a selection changing inside one.
    jf::JSignal<Surface*> tabChanged;

    // Persist / restore the WHOLE surface set — the document's "surfaces" section (the surface
    // pool + the open tabs). Free surfaces carry their model; node tabs carry identity
    // only (their page lives in the panelLibrary, which must be loaded before surfacesFromJson).
    // surfacesFromJson with a non-object resets to a fresh "Main" (project switch / legacy documents).
    jf::JJson surfacesToJson();
    // Save-time: flush every authored surface's widget tree into its model (and page models) before serialization,
    // so widget-owned state persists. Call immediately before surfacesToJson()/panelLibrary.toJson().
    void      commitAll();
    void      surfacesFromJson(const jf::JJson& j);
    // EVERY POOLED SURFACE, and the index IS the pool index — which is the whole point.
    //
    // This was reopen-BY-NAME, and a name is not an identity: View ▸ New Surface Tab makes them all
    // "Surface", so a second one could not be reopened at all. The by-name loop took the first doc that
    // matched, and if that one was already open it focused its tab and returned — so both rows in the
    // picker opened the same surface, and the other was unreachable for as long as it existed.
    struct SurfaceRef { int index; std::string name; bool open; };
    std::vector<SurfaceRef> surfaces();
    void reopenAt(int index);
    // Rename and delete, which the studio had neither of: a surface could be created and closed, and
    // from then on it was a permanent line in a list with no way to name it or be rid of it.
    bool renameAt(int index, const std::string& name);
    bool removeAt(int index);

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    void handleMousePress(float mx, float my) override   { tabs_.handleMousePress(mx, my); }
    void handleMouseMove(float mx, float my) override    { tabs_.handleMouseMove(mx, my); }
    void handleMouseRelease(float mx, float my) override { tabs_.handleMouseRelease(mx, my); }
    bool handleScroll(float mx, float my, float w) override { return tabs_.handleScroll(mx, my, w); }
    bool handleKeyEvent(const jf::JKeyEvent& ke) override { return tabs_.handleKeyEvent(ke); }


private:
    bool        placedAnywhere_(const std::string& node) const;   // any open surface / other page hosts it
    // A SURFACE THAT IS NOT A TAB PAGE IS NOT ON SCREEN — and must not be in the framework's input scans
    // either. JTabWidget hides the pages it owns, but removeTab deliberately hands a closed page back
    // VISIBLE (it is the caller's widget and it must stay paintable wherever the caller puts it next),
    // and a pooled doc that was never opened was never hidden at all. Either way the surface kept its
    // last bounds and its context menu, and the window's right-click walk — newest widget first, first
    // visible hit wins — found it BEFORE the tab actually in front. It answered with its own menu, built
    // from its own empty selection: every item that greys off the selection greyed, and only "Add
    // control" (the one root item whose state is never set) still live.
    void        syncPooledVisibility_();
    // Has the tab at this index any viewport at all — can it display page content?
    bool        tabHasViewport_(int i) const;

    struct Doc {
        int                         id;
        std::string                 name;
        std::string                 nodePath;    // non-empty => a node-page tab (model lives in the store)
        std::unique_ptr<PanelModel> owned;       // free surfaces own their model; node tabs leave this null
        PanelModel*                 model = nullptr;   // the model the Surface renders (owned or store-backed)
        std::unique_ptr<Surface>    view;        // declared after model so it outlives its dependency
        // The page this tab OPENS on, as the file states it. Round-tripped untouched: where the tab is
        // now is view->activeNode(), which is where you have browsed to and is nobody's business to
        // save. Empty = the file said nothing and the tab inherits the current selection.
        std::string                 savedAt;
    };
    Doc* makeDoc(const std::string& name);       // create a doc owning a fresh model, park it in the pool
    Doc* makeNodeDoc(const std::string& nodePath, const std::string& title);   // a doc over the node's store page
    void wireDoc(Doc* d);                        // shared Surface wiring (mode/active node/selection)
    Doc* docFor(jf::JWidget* content);           // the doc whose surface is that tab's content (reorder-safe)

    jf::JSceneGraph&      g_;
    const Cache&          cache_;
    PanelLibrary&       store_;                // node-page pages (viewport targets); persisted on edit
    jf::JTabWidget        tabs_;
    std::vector<std::unique_ptr<Doc>> pool_;    // every surface authored this session (open or closed)
    int                   nextId_ = 1;
    Surface::Mode         mode_ = Surface::Mode::Edit;
    std::string           activeNode_;           // current tree selection, applied to every doc's Surface
};

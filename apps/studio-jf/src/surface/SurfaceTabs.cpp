#include "SurfaceTabs.h"
#include "../model/Cache.h"

#include <j/core/Log.h>   // JLOGC — tab open/close/tool (surface.tab) + deferred doc build (surface.lazy)

SurfaceTabs::SurfaceTabs(jf::JSceneGraph& g, const Cache& cache, PanelLibrary& panelLibrary)
    : jf::JWidget(g, "SurfaceTabs"), g_(g), cache_(cache), store_(panelLibrary), tabs_(g) {
    addChild(&tabs_);      // the tab widget is this widget's child; every page hangs off it in turn
    // Home surface — permanent: no close ×, not draggable; always the first tab. Its definition
    // persists like any pooled surface, but the tab can't be closed away.
    Doc* home = makeDoc("Main");
    tabs_.addTab("Main", home->view.get(), /*closable*/ false, /*draggable*/ false);
    tabs_.setTabRenamable(0, true);
    // A TAB RENAMED IN PLACE IS THE SURFACE RENAMED. Double-click (or F2) edits the label on the bar
    // itself; this is the half that makes it mean something, by putting the new name on the pooled doc
    // so it survives the tab being closed and the project being saved. Only free surfaces are marked
    // renamable — a node-page tab takes its label from its tree node and would lose the edit the next
    // time that node moved.
    tabs_.onTabRenamed.connect([this](int i, const std::string& name){
        if (Doc* d = docFor(tabs_.content(i))) {
            d->name = name;
            // AND THE DOCUMENT IS NOW DIRTY. Without this the new name lives only in memory: nothing
            // asks to save it on the way out and Ctrl+S has nothing to notice, so a rename lasted
            // exactly as long as the session did. Every other edit here goes through the same hook.
            if (Surface::onModified) Surface::onModified();
            JLOGC("surface.tab", jf::JLogLevel::Info) << "surface renamed in place to '" << name << "'";
        }
    });
    // Switching tabs changes the active surface (and thus the active selection).
    tabs_.onTabChanged.connect([this](int){
        // THE TREE FOLLOWS THE TAB. A tab holds its own page, so arriving on one means the tree should
        // point at that page — otherwise the selection says one thing and the surface shows another, and
        // the next click in the tree looks like it did nothing. Emitted only when the node differs from
        // the one the tree is already on, so the two directions cannot echo each other.
        if (Surface* s = activeSurface()) {
            const std::string n = s->activeNode();
            if (!n.empty() && n != activeNode_) { activeNode_ = n; nodeFollowed.emit(n); }
        }
        selectionChanged.emit(activeSurface());
        tabChanged.emit(activeSurface());
    });
}

void SurfaceTabs::wireDoc(Doc* d) {
    d->view->setMode(mode_);
    d->view->setActiveNode(activeNode_);
    d->view->selectionChanged.connect([this]{ selectionChanged.emit(activeSurface()); });
    d->view->statusMessage.connect([this](const std::string& m){ statusMessage.emit(m); });   // bulk-op feedback (clamp report)
    // Let any surface drill INTO a viewport to author that node's page in place. A page edit just marks the
    // document dirty (deferred); the one saveAll writes the whole dashboard.gui — no per-edit disk write.
    d->view->setPageAccess(
        [this](const std::string& path) -> PanelModel* { return &store_.forNode(path); },
        []{ if (Surface::onModified) Surface::onModified(); });
}

SurfaceTabs::Doc* SurfaceTabs::makeDoc(const std::string& name) {
    auto d = std::make_unique<Doc>();
    d->id = nextId_++;
    d->name = name;
    d->owned = std::make_unique<PanelModel>();
    d->model = d->owned.get();
    d->view = std::make_unique<Surface>(g_, cache_, d->model);
    wireDoc(d.get());
    Doc* p = d.get();
    JLOGC("surface.lazy", jf::JLogLevel::Info)
        << "build free-surface doc id=" << p->id << " name=\"" << name << "\" (owned model)";
    pool_.push_back(std::move(d));
    return p;
}

// A tab that edits a navigation-tree node's page. The model is the node's PanelModel in the store (NOT
// owned here) — shared with every viewport of that node, so authoring here updates all of them live.
// Element/canvas edits mark the document dirty (deferred); saveAll flushes the whole dashboard.gui.
SurfaceTabs::Doc* SurfaceTabs::makeNodeDoc(const std::string& nodePath, const std::string& title) {
    auto d = std::make_unique<Doc>();
    d->id = nextId_++;
    d->name = title;
    d->nodePath = nodePath;
    d->model = &store_.forNode(nodePath);
    d->view = std::make_unique<Surface>(g_, cache_, d->model);
    wireDoc(d.get());
    auto dirty = [](int){ if (Surface::onModified) Surface::onModified(); };
    d->model->elementAdded.connect(dirty);
    d->model->elementRemoved.connect(dirty);
    d->model->elementChanged.connect(dirty);
    d->model->canvasChanged.connect([]{ if (Surface::onModified) Surface::onModified(); });
    Doc* p = d.get();
    JLOGC("surface.lazy", jf::JLogLevel::Info)
        << "build node-page doc id=" << p->id << " node=" << nodePath << " title=\"" << title
        << "\" elements=" << (p->model ? p->model->elements().size() : 0);
    pool_.push_back(std::move(d));
    return p;
}

void SurfaceTabs::openNode(const std::string& nodePath, const std::string& title) {
    JLOGC("surface.tab", jf::JLogLevel::Info) << "openNode node=" << nodePath << " title=\"" << title << "\"";
    // Focus the node's tab if it already exists (open or pooled-closed); otherwise create it.
    for (auto& d : pool_) {
        if (d->nodePath != nodePath) continue;
        for (int i = 0; i < tabs_.tabCount(); ++i)
            if (tabs_.content(i) == d->view.get()) { tabs_.setActiveTab(i); invalidate(); return; }
        tabs_.addTab(title, d->view.get(), /*closable*/ true, /*draggable*/ true);
        tabs_.setActiveTab(tabs_.tabCount() - 1); invalidate(); return;
    }
    Doc* d = makeNodeDoc(nodePath, title);
    tabs_.addTab(title, d->view.get(), /*closable*/ true, /*draggable*/ true);
    tabs_.setActiveTab(tabs_.tabCount() - 1);
    syncPooledVisibility_();
    invalidate();
}

// Hide every pooled surface that is not currently a tab page. Only ever HIDES: the tab widget owns
// showing the page in front, and second-guessing it here would fight it. See the declaration for what
// a stray visible surface does to a right-click.
void SurfaceTabs::syncPooledVisibility_() {
    for (auto& d : pool_) {
        if (!d || !d->view) continue;
        bool inTab = false;
        for (int i = 0; i < tabs_.tabCount() && !inTab; ++i) inTab = (tabs_.content(i) == d->view.get());
        if (!inTab && d->view->isVisible()) {
            JLOGC("surface.tab", jf::JLogLevel::Info)
                << "hide pooled-but-closed surface \"" << d->name << "\" (docId=" << d->id << ")";
            d->view->setVisible(false);
        }
    }
}

// Identify a doc by its surface widget (the tab's content) — stays correct across tab reordering.
SurfaceTabs::Doc* SurfaceTabs::docFor(jf::JWidget* content) {
    for (auto& d : pool_) if (d->view.get() == content) return d.get();
    return nullptr;
}

int SurfaceTabs::newSurface(const std::string& name) {
    Doc* d = makeDoc(name);
    tabs_.addTab(name, d->view.get(), /*closable*/ true, /*draggable*/ true);   // user surfaces: closable + reorderable
    tabs_.setTabRenamable(tabs_.tabCount() - 1, true);
    tabs_.setActiveTab(tabs_.tabCount() - 1);
    JLOGC("surface.tab", jf::JLogLevel::Info) << "open Surface tab name=\"" << name << "\" docId=" << d->id
        << " tabs=" << tabs_.tabCount();
    syncPooledVisibility_();
    invalidate();
    return d->id;
}

void SurfaceTabs::openTool(const std::string& title, jf::JWidget* content) {
    if (!content) return;
    for (int i = 0; i < tabs_.tabCount(); ++i)                       // focus if already open
        if (tabs_.content(i) == content) {
            JLOGC("surface.tab", jf::JLogLevel::Info) << "openTool focus existing tool tab title=\"" << title << "\"";
            tabs_.setActiveTab(i); invalidate(); return;
        }
    // A non-Surface tool tab: it isn't in pool_, so docFor()→null keeps activeSurface() null while it's
    // active, and surfacesToJson() (which matches pool surfaces) naturally omits it from persistence.
    tabs_.addTab(title, content, /*closable*/ true, /*draggable*/ true);
    tabs_.setActiveTab(tabs_.tabCount() - 1);
    JLOGC("surface.tab", jf::JLogLevel::Info) << "open tool tab title=\"" << title << "\" tabs=" << tabs_.tabCount();
    invalidate();
}

bool SurfaceTabs::toolOpen(const jf::JWidget* content) const {
    if (!content) return false;
    for (int i = 0; i < tabs_.tabCount(); ++i)
        if (tabs_.content(i) == content) return true;
    return false;
}

void SurfaceTabs::closeActive() {
    const int a = tabs_.activeTab();
    JLOGC("surface.tab", jf::JLogLevel::Info) << "close active tab index=" << a
        << (a >= 0 ? "" : " (none active — no-op)");
    if (a >= 0) tabs_.removeTab(a);   // the tab goes; the definition survives in the pool
    syncPooledVisibility_();          // ...and stops answering clicks: removeTab hands it back visible
    invalidate();
}

void SurfaceTabs::retagNodeRefs(const std::string& oldPrefix, const std::string& newPrefix) {
    if (oldPrefix.empty() || oldPrefix == newPrefix) return;
    const std::string under = oldPrefix + "/";
    for (auto& d : pool_) {
        // Free surfaces own their model — retag their placed viewports here. Node-page docs render a store
        // page (already retagged by the store); for those we only follow the moved label.
        if (d->owned) retagNodeProp(*d->owned, oldPrefix, newPrefix);
        if (d->nodePath.empty()) continue;
        if (d->nodePath == oldPrefix)              d->nodePath = newPrefix;
        else if (d->nodePath.rfind(under, 0) == 0) d->nodePath = newPrefix + d->nodePath.substr(oldPrefix.size());
        else continue;
        const size_t s = d->nodePath.rfind('/');
        d->name = (s == std::string::npos) ? d->nodePath : d->nodePath.substr(s + 1);
        for (int i = 0; i < tabs_.tabCount(); ++i)
            if (tabs_.content(i) == d->view.get()) { tabs_.setTabLabel(i, d->name); break; }
    }
    invalidate();
}

void SurfaceTabs::forgetNode(const std::string& prefix) {
    if (prefix.empty()) return;
    const std::string under = prefix + "/";
    for (auto it = pool_.begin(); it != pool_.end(); ) {
        Doc* d = it->get();
        const bool gone = !d->nodePath.empty() && (d->nodePath == prefix || d->nodePath.rfind(under, 0) == 0);
        if (!gone) { ++it; continue; }
        for (int i = 0; i < tabs_.tabCount(); ++i)
            if (tabs_.content(i) == d->view.get()) { tabs_.removeTab(i); break; }
        it = pool_.erase(it);
    }
    syncPooledVisibility_();
    invalidate();
}

// The last viewport of a page just went: destroy the page and its widgets. Immediately — not at save.
// Deferring it meant a node whose viewport you had deleted still had its old page in memory, so dragging
// that node back out produced a viewport that "magically" contained the widgets you thought you deleted.
void SurfaceTabs::gcNodeIfUnplaced(const std::string& node) {
    if (node.empty() || !placedAnywhere_(node)) store_.remove(node);
}

// Does any OPEN surface, or any other library page, still host a viewport of this page?
bool SurfaceTabs::placedAnywhere_(const std::string& node) const {
    for (const auto& d : pool_)
        if (d->model)
            for (const auto& e : d->model->elements())
                if (e.type == "viewport" && viewportPage(e) == node) return true;
    return store_.anyPlaces(node);
}

void SurfaceTabs::pruneUnplacedPages(const std::set<std::string>& liveNodes) {
    // THE TREE DECIDES, when the caller can say what is in it. A page whose node still exists is
    // reachable — select it and a viewport shows it — so deleting it because no viewport happens to
    // NAME it would throw away the entire library the moment viewports stopped naming pages. That is
    // not hypothetical: with one unbound viewport per surface, placedAnywhere_ is false for every page
    // in the document, and this runs inside buildDoc() on every save.
    for (const std::string& node : store_.pageKeys()) {
        if (!liveNodes.empty() && liveNodes.count(node)) continue;   // the tree still has it
        if (placedAnywhere_(node)) continue;                          // …or a pinned viewport names it
        store_.remove(node);
    }
}

void SurfaceTabs::commitAll() {
    for (const auto& d : pool_) if (d && d->view) d->view->commitInstances();
}

jf::JJson SurfaceTabs::surfacesToJson() {
    jf::JJson out = jf::JJson::object();
    jf::JJson arr = jf::JJson::array();
    for (const auto& d : pool_) {
        jf::JJson e = jf::JJson::object();
        e["name"] = d->name;
        if (!d->nodePath.empty()) e["node"] = d->nodePath;   // page content lives in the panelLibrary
        else if (d->owned)        e["model"] = d->owned->toJson();
        // "at" IS READ, NEVER WRITTEN. It is the page this tab opens on — configuration, seeded in the
        // file — and writing back wherever the tab happened to be would quietly replace that default
        // with the last place you browsed to, on any save made for any other reason.
        if (!d->savedAt.empty()) e["at"] = d->savedAt;
        arr.push(std::move(e));
    }
    out["pool"] = std::move(arr);
    jf::JJson open = jf::JJson::array();                     // open tabs as pool indices, in tab order
    for (int i = 0; i < tabs_.tabCount(); ++i)
        for (size_t p = 0; p < pool_.size(); ++p)
            if (pool_[p]->view.get() == tabs_.content(i)) { open.push(static_cast<double>(p)); break; }
    out["open"] = std::move(open);
    out["active"] = static_cast<double>(tabs_.activeTab());
    return out;
}

void SurfaceTabs::surfacesFromJson(const jf::JJson& j) {
    while (tabs_.tabCount() > 0) tabs_.removeTab(0);         // tabs first (they reference pool surfaces)
    pool_.clear();
    if (j.isObject() && j.contains("pool") && !j["pool"].arr().empty()) {
        for (const jf::JJson& e : j["pool"].arr()) {
            const std::string node = e["node"].str();
            Doc* made = nullptr;
            if (!node.empty()) {
                made = makeNodeDoc(node, e["name"].str());
            } else {
                Doc* d = makeDoc(e["name"].str().empty() ? std::string("Surface") : e["name"].str());
                if (e.contains("model")) d->owned->load(e["model"]);
                made = d;
            }
            if (made && e.contains("at")) {
                made->savedAt = e["at"].str();               // kept verbatim so a save round-trips it
                if (made->view) made->view->setActiveNode(made->savedAt);
            }
        }
        bool anyOpen = false;
        for (const jf::JJson& oi : j["open"].arr()) {
            const size_t p = static_cast<size_t>(oi.number(-1));
            if (p >= pool_.size()) continue;
            Doc* d = pool_[p].get();
            const bool home = (p == 0 && d->nodePath.empty());   // first free surface = the permanent home
            tabs_.addTab(d->name, d->view.get(), /*closable*/ !home, /*draggable*/ !home);
            tabs_.setTabRenamable(tabs_.tabCount() - 1, d->nodePath.empty());
            anyOpen = true;
        }
        if (!anyOpen && !pool_.empty())                       // never restore to an empty centre
            tabs_.addTab(pool_.front()->name, pool_.front()->view.get(), false, false);
            tabs_.setTabRenamable(tabs_.tabCount() - 1, pool_.front()->nodePath.empty());
        const int act = static_cast<int>(j["active"].number(0));
        if (act >= 0 && act < tabs_.tabCount()) tabs_.setActiveTab(act);
    } else {
        // Reset (project switch / legacy document): a fresh permanent home surface, like construction.
        Doc* home = makeDoc("Main");
        tabs_.addTab("Main", home->view.get(), /*closable*/ false, /*draggable*/ false);
        tabs_.setTabRenamable(tabs_.tabCount() - 1, true);
    }
    syncPooledVisibility_();   // a restore builds a doc for EVERY pooled surface; only the open ones show
    invalidate();
    selectionChanged.emit(activeSurface());
}

std::vector<SurfaceTabs::SurfaceRef> SurfaceTabs::surfaces() {
    std::vector<SurfaceRef> out;
    int i = 0;
    for (const auto& d : pool_) {
        bool open = false;
        for (int t = 0; t < tabs_.tabCount() && !open; ++t) open = tabs_.content(t) == d->view.get();
        out.push_back({ i++, d->name, open });
    }
    return out;
}

// BY INDEX, NOT BY NAME. Two surfaces called "Surface" are two surfaces; the pool index is the only
// thing that tells them apart, and the caller already has it because it listed them.
void SurfaceTabs::reopenAt(int index) {
    if (index < 0 || index >= static_cast<int>(pool_.size())) return;
    Doc* d = pool_[static_cast<size_t>(index)].get();
    for (int i = 0; i < tabs_.tabCount(); ++i)
        if (tabs_.content(i) == d->view.get()) { tabs_.setActiveTab(i); invalidate(); return; }
    tabs_.addTab(d->name, d->view.get(), true, true);
    tabs_.setTabRenamable(tabs_.tabCount() - 1, d->nodePath.empty());
    tabs_.setActiveTab(tabs_.tabCount() - 1);
    syncPooledVisibility_();
    invalidate();
}

bool SurfaceTabs::renameAt(int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(pool_.size()) || name.empty()) return false;
    Doc* d = pool_[static_cast<size_t>(index)].get();
    // A node-page tab takes its label FROM the node (retagNodeRefs resets it on every rename of that
    // node), so naming one here would be overwritten the next time the tree moved. Rename the node.
    if (!d->nodePath.empty()) return false;
    d->name = name;
    for (int i = 0; i < tabs_.tabCount(); ++i)
        if (tabs_.content(i) == d->view.get()) { tabs_.setTabLabel(i, name); break; }
    JLOGC("surface.tab", jf::JLogLevel::Info) << "surface " << index << " renamed to '" << name << "'";
    invalidate();
    return true;
}

bool SurfaceTabs::removeAt(int index) {
    if (index < 0 || index >= static_cast<int>(pool_.size())) return false;
    // NEVER THE LAST ONE. A workspace with no surface at all has no canvas to put anything back on, and
    // nothing in the UI would offer to make one that was not itself on a surface.
    if (pool_.size() <= 1) return false;
    Doc* d = pool_[static_cast<size_t>(index)].get();
    for (int i = 0; i < tabs_.tabCount(); ++i)
        if (tabs_.content(i) == d->view.get()) { tabs_.removeTab(i); break; }
    JLOGC("surface.tab", jf::JLogLevel::Info) << "surface " << index << " ('" << d->name << "') deleted";
    pool_.erase(pool_.begin() + index);
    syncPooledVisibility_();
    invalidate();
    return true;
}

PanelModel* SurfaceTabs::activeModel() {
    Doc* d = docFor(tabs_.activeContent());
    return d ? d->model : nullptr;
}

Surface* SurfaceTabs::activeSurface() {
    Doc* d = docFor(tabs_.activeContent());
    return d ? d->view.get() : nullptr;
}

// Move to a tab that will HAVE a page window, and say whether there is one. A tab that is all instrument
// declares a zero page area (PanelModel::allowsPageWindows) — opening a page over it would cover the very
// thing it exists to show — so a page opened while such a tab is in front goes to the first tab that takes
// one, which is the Main surface in every document this ships with. Doing nothing instead would make a
// click on the tree silently do nothing at all, which is worse than moving.
bool SurfaceTabs::showTabTakingPages() {
    if (Surface* cur = activeSurface(); cur && cur->model() && cur->model()->allowsPageWindows()) return true;
    for (int i = 0; i < tabs_.tabCount(); ++i) {
        Doc* d = docFor(tabs_.content(i));
        if (!d || !d->view || !d->view->model() || !d->view->model()->allowsPageWindows()) continue;
        tabs_.setActiveTab(i);
        invalidate();
        return true;
    }
    return false;
}

std::string SurfaceTabs::defaultPageOf(const Surface* tab) const {
    for (const auto& d : pool_) if (d->view.get() == tab) return d->savedAt;
    return {};
}

std::string SurfaceTabs::activeName() {
    for (const auto& d : pool_) if (d->view.get() == tabs_.activeContent()) return d->name;
    return {};
}

void SurfaceTabs::setMode(Surface::Mode m) {
    mode_ = m;
    for (auto& d : pool_) d->view->setMode(m);   // edit/run is app-wide across every surface
    invalidate();
}

// Does the tab at this index have a viewport on it at all — i.e. can it display page content of any kind?
// Not "does it host THIS page": a workspace's viewports are its own business, and a focused tab is not
// asking to be second-guessed about which pages it chose to carry.
bool SurfaceTabs::tabHasViewport_(int i) const {
    if (i < 0 || i >= tabs_.tabCount()) return false;
    const Doc* d = nullptr;
    for (const auto& p : pool_) if (p->view.get() == tabs_.content(i)) { d = p.get(); break; }
    if (!d) return false;
    if (!d->nodePath.empty()) return true;               // a node-page tab IS page content
    if (!d->model) return false;
    for (const auto& e : d->model->elements()) if (e.type == "viewport") return true;
    return false;
}

void SurfaceTabs::setActiveNode(const std::string& node) {
    activeNode_ = node;                                   // remembered so tabs opened later inherit it
    // THE ACTIVE TAB ONLY. Every surface used to be pushed the same node, so all tabs showed one page
    // and switching tabs changed the framing and nothing else. A tab keeps its own page now: park the
    // VE table on one and the trigger scope on another, and switching between them switches the page.
    // NOT AN EDIT. Which page a tab is looking at is where you are, not what the document says — so
    // moving between pages must not dirty it and must not raise a save prompt on the way out. The tab
    // defaults in the file are configuration; navigating away from them is not a change to them.
    if (Surface* s = activeSurface()) s->setActiveNode(node);
    // A TAB WITH NO VIEWPORT CANNOT SHOW A PAGE. Diagnostics is instruments only — it answers "is the
    // hardware alright", which needs no editing surface — so picking a node in the tree or following a
    // link while standing on it used to change nothing visible at all: the tree moved, the surface did
    // not, and the user is left wondering where the page they asked for went. So hand them back to Main,
    // the general tuning tool, which carries a viewport for every page.
    //
    // Deliberately NOT per-node. A focused workspace (Idle Control, Boost Control) has viewports and keeps
    // its selections, including for pages it does not happen to carry — which tabs hold which pages is the
    // workspace's own business, and yanking the user out of one mid-task would be the worse surprise.
    if (!node.empty() && !tabHasViewport_(tabs_.activeTab())) {
        for (int i = 0; i < tabs_.tabCount(); ++i)
            if (tabHasViewport_(i)) { tabs_.setActiveTab(i); syncPooledVisibility_(); break; }
    }
    invalidate();
}

void SurfaceTabs::populateRenderPrimitives(jf::JPrimitiveBuffer& buf) {
    const auto b = getBoundingBox();
    tabs_.setBounds({ b.x, b.y, b.width, b.height });
    tabs_.populateRenderPrimitives(buf);
}


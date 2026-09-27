#pragma once

// SurfaceCanvas — the custom framework container that holds a tuning surface's widget instances as
// framework-driven children. Modeled on JScrollArea, NOT JContainer: it bakes the world->screen camera into
// each child's scene-graph boundingBox every frame and NEVER calls m_graph.addChild / computeLayout — which
// would let the Flex/Grid/Form solver overwrite the camera transform. The framework is single-space: a widget
// gets focus / Tab / keys / click-to-focus / mouse purely by being constructed on the shared JSceneGraph and
// carrying a boundingBox, so the children need no parent registration to be driven the framework way.
//
// The container OWNS the per-element widget instances (keyed by id, via the framework adopt() primitive); the
// Surface holds the model, drives their reconciliation, positions them through the camera, and draws the
// authoring/overlay chrome ON TOP.
//
// The instances ARE parented: adopt() calls JWidget::addChild, so each one carries a widget-tree edge back to
// this container. That edge is not the layout solver — JWidget::addChild only maintains m_children/m_parent,
// while layout comes from m_graph.addChild + computeLayout, which this container never calls. So the camera
// still owns their bounds, AND everything that walks the widget tree can see them: effective visibility, focus
// traversal, and the per-window tooltip search. (This comment used to say the instances were adopted "for
// LIFETIME only — NOT addChild-ed". That was true of an older adopt(); it parents now, and reading the stale
// version suggests a canvas control is invisible to every tree walk, which would invite re-breaking it.)

#include <string>
#include <j/core/JWidget.h>

#include "SurfaceCamera.h"

#include <vector>
#include <memory>
#include <unordered_map>

class CanvasWidget;

class SurfaceCanvas : public jf::JWidget {
public:
    explicit SurfaceCanvas(jf::JSceneGraph& g) : jf::JWidget(g, "SurfaceCanvas") {}

    // Node set — non-owning during the migration. Vector order IS paint order (back-to-front); hit-testing is
    // the reverse (topmost first), matching the Surface's z-order.
    void addNode(CanvasWidget* w) { if (w) m_nodes.push_back(w); }
    void clearNodes()             { m_nodes.clear(); }
    const std::vector<CanvasWidget*>& nodes() const { return m_nodes; }

    // The Surface feeds its live world->screen transform each frame.
    void setCamera(const SurfaceCamera& cam) { m_camera = cam; }

    // --- Owned instances (Qt model) --------------------------------------------------------------------
    // THIS container owns the per-element widget instances, keyed by element id, via the framework adopt()
    // primitive (RAII). The Surface holds the model and drives reconciliation (create/erase/re-source); it
    // hands each new instance here and looks them up through these accessors — "the app parents, the
    // container owns the lifetime", exactly like Qt.
    CanvasWidget* instanceFor(int id) const;                                // lookup, else nullptr
    CanvasWidget* adoptInstance(int id, std::unique_ptr<CanvasWidget> w);   // take ownership (adopt) + index by id
    void          eraseInstance(int id);                                    // destroy the instance + unindex
    void          clearInstances();                                         // destroy all owned instances + clear index
    const std::unordered_map<int, CanvasWidget*>& instancesById() const { return m_byId; }

    // Bake the camera into every child's boundingBox and cull off-viewport children (setVisible(false) removes
    // a child from paint AND framework focus/input). Called at the top of paint, and (Stage 3) at the top of
    // the mouse handlers, so paint and hit-test read the SAME bounds (input runs a frame before paint).
    void applyCamera();

    // The topmost node whose (camera-set) boundingBox contains (mx,my), else nullptr. Reverse paint order.

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

    // WHY THE GAUGES ARE ALL READING ZERO, said on the canvas rather than left to be deduced.
    //
    // Two states produce a screen full of zeros and look identical: the link is down, or the link is
    // fine and the ECU's key is off — with the key off every non-bootstrap sensor is skipped before its
    // pipeline is even built, so every channel is genuinely zero and the ECU is behaving correctly.
    // Nothing on screen distinguished those from a broken sensor, and an afternoon went into chasing a
    // wideband that was reading 0 for the second reason.
    //
    // WHERE THAT NOTICE LIVES NOW: JAppWindow::setNotice, a strip above the toolbar (main.cpp's
    // refreshLinkOverlay). It used to be a banner drawn across the middle of this canvas with the
    // whole surface dimmed behind it — unmissable once, and thereafter a box sitting on top of the
    // gauges being read, with no way to move it aside. The reason for saying it at all is unchanged
    // and is written out above; only the place changed.

private:
    std::vector<CanvasWidget*>             m_nodes;   // transient per-frame paint list (raw; a view over m_byId in z order)
    std::unordered_map<int, CanvasWidget*> m_byId;    // id -> OWNED instance (lifetime via JWidget::adopt / m_ownedChildren)
    SurfaceCamera                          m_camera;
};

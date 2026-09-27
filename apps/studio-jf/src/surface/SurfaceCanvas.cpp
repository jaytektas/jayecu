// SurfaceCanvas — see SurfaceCanvas.h. Camera-baked bounds + cull + clipped child paint; never touches the
// scene-graph layout engine. Non-owning + unmounted in Stage 1.

#include "SurfaceCanvas.h"
#include "StyleMetrics.h"
#include <string>
#include <algorithm>

#include "CanvasWidget.h"
#include "PanelModel.h"                    // PanelElement (raw rect + region)
#include <j/graphics/RenderPrimitive.h>    // jf::JPrimitiveBuffer

CanvasWidget* SurfaceCanvas::instanceFor(int id) const {
    const auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : it->second;
}

CanvasWidget* SurfaceCanvas::adoptInstance(int id, std::unique_ptr<CanvasWidget> w) {
    eraseInstance(id);                                   // replace any existing (type-reuse rebuild)
    CanvasWidget* p = w ? adopt(std::move(w)) : nullptr; // framework RAII ownership; null factory result -> null slot
    m_byId[id] = p;
    return p;
}

void SurfaceCanvas::eraseInstance(int id) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) return;
    CanvasWidget* p = it->second;
    m_byId.erase(it);
    if (p) disown(p);                                    // destroys it (removes from m_ownedChildren)
}

void SurfaceCanvas::clearInstances() {
    m_byId.clear();
    disownAll();
}

void SurfaceCanvas::applyCamera() {
    const SurfaceCamera::Xform t = m_camera.xform();
    // Managed layouts need every sibling's raw rect + region; build the child list once.
    std::vector<LayoutChild> children; children.reserve(m_nodes.size());
    for (CanvasWidget* w : m_nodes) {
        const PanelElement* e = w ? w->element() : nullptr;
        children.push_back({ e ? jf::JRect{ e->x, e->y, e->w, layoutHeightOf(e->type, e->h) } : jf::JRect{},
                             e ? e->prop("region") : std::string{} });
    }
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        CanvasWidget* w = m_nodes[i];
        if (!w) continue;
        w->setBounds(m_camera.screenRectOf(t, children, static_cast<int>(i)));   // bake the camera into the node
    }
    // NOTE: this only sets BOUNDS. Business visibility (node view-switcher + run-mode condition) is the
    // Surface's call and stays on the framework isVisible() flag; off-viewport culling is a paint-time skip
    // below (the viewport clip already confines off-viewport content — the skip is just to avoid the work).
}

// Paint the nodes clipped to the viewport, in z (vector) order. Node BOUNDS are expected to be current — the
// caller sets them, via the Surface's screenRectOf (Stage 2) or applyCamera() (Stage 3) — so paint does not
// recompute them. Shown (business-visible) + on-viewport nodes only.
void SurfaceCanvas::populateRenderPrimitives(jf::JPrimitiveBuffer& buf) {
    const jf::JRect& v = m_camera.viewport;
    buf.pushClip(v.x, v.y, v.width, v.height);
    for (CanvasWidget* w : m_nodes) {
        if (!w || !w->isVisible()) continue;
        const auto b = w->getBoundingBox();
        if (SurfaceCamera::rectVisible(jf::JRect{ b.x, b.y, b.width, b.height }, v))
            w->populateRenderPrimitives(buf);
    }
    buf.popClip();
}

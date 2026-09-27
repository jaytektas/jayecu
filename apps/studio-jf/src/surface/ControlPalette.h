#pragma once

// ControlPalette — the "Widgets" dock: every registered control type as a row, as a drag-source
// list. Drag a row onto the surface to place that control
// at the drop point (via JDragDrop, payload = the type key); a plain click adds it at the view centre.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/DragDrop.h>

#include "WidgetRegistry.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

class ControlPalette : public jf::JWidget {
public:
    std::function<void(const std::string&)> onAdd;   // click-to-add at the surface centre

    explicit ControlPalette(jf::JSceneGraph& g) : jf::JWidget(g, "ControlPalette") {
        for (const std::string& type : widgetTypes()) {
            const std::string title = widgetTitle(type);
            types_.push_back(type);
            titles_.push_back(title.empty() ? type : title);
        }
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, jf::Colors::Surface1);
        const float lh = jf::JTextHelper::lineHeight();
        for (size_t i = 0; i < types_.size(); ++i) {
            const float ry = b.y + kPad + i * (kRowH + kGap);
            const bool hov = static_cast<int>(i) == hoverRow_;
            buf.pushRectangle(b.x + kPad, ry, b.width - 2 * kPad, kRowH, hov ? jf::Colors::Surface3 : jf::Colors::Surface2, 4.f, 1.f, jf::Colors::Border);
            if (jf::JTextHelper::hasAtlas()) {
                uint8_t tc[4]; std::copy(jf::Colors::TextPrimary, jf::Colors::TextPrimary + 4, tc);
                jf::JTextHelper::pushText(buf, b.x + kPad + 8.f, ry + (kRowH - lh) * 0.5f, titles_[i], tc, b.width - 2 * kPad - 12.f);
            }
        }
    }

    void handleMouseMove(float mx, float my) override {
        hoverRow_ = rowAt(mx, my);
        if (pressRow_ >= 0 && !dragging_ && (std::fabs(mx - pressX_) + std::fabs(my - pressY_)) > 4.f) {
            jf::JDragDrop::start<std::string>(types_[pressRow_], mx, my, titles_[pressRow_]);
            dragging_ = true;
        }
        if (dragging_) jf::JDragDrop::update(mx, my);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void handleMousePress(float mx, float my) override {
        pressRow_ = rowAt(mx, my); pressX_ = mx; pressY_ = my; dragging_ = false;
    }
    void handleMouseRelease(float mx, float my) override {
        if (pressRow_ >= 0 && !dragging_ && rowAt(mx, my) == pressRow_ && onAdd) onAdd(types_[pressRow_]);   // click
        if (dragging_ && jf::JDragDrop::isDragging()) jf::JDragDrop::cancel();   // released off the surface
        pressRow_ = -1; dragging_ = false;
    }


private:
    int rowAt(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx < b.x + kPad || mx > b.x + b.width - kPad) return -1;
        const int i = static_cast<int>((my - b.y - kPad) / (kRowH + kGap));
        return (i >= 0 && i < static_cast<int>(types_.size())) ? i : -1;
    }
    static constexpr float kRowH = 22.f, kGap = 4.f, kPad = 8.f;
    std::vector<std::string> types_, titles_;
    int   hoverRow_ = -1, pressRow_ = -1;
    float pressX_ = 0.f, pressY_ = 0.f;
    bool  dragging_ = false;
};

#pragma once

// ChannelListWidget — a watch list the USER builds while the engine is running.
//
// Every other live control on a page is placed and bound by whoever authored the page: it shows what they
// decided mattered. Tuning does not work like that. What you need on screen changes with what you are
// chasing — oil pressure this hour, knock the next — and it changes while the engine is running, which is
// exactly when you cannot stop to edit a layout.
//
// So this widget owns a LIST of channels rather than one binding, and the list is editable in run mode:
// click a row to change what it watches, click the empty row at the bottom to add one, and the list is
// stored in the element's own "channels" prop, so it is still there tomorrow. It is the one control on a
// page whose contents are the operator's rather than the author's.
//
// The row is name / value / unit, the same shape the live strip uses, so a watch list dropped beside a
// table reads like the strip above it.

#include "SkinableWidget.h"

#include <string>
#include <vector>

class ChannelListWidget : public SkinableWidget {
public:
    explicit ChannelListWidget(jf::JSceneGraph& g) : SkinableWidget(g, "channels") {}
    std::string elementType() const override { return "channels"; }
    std::string paletteTitle() const override { return "Channel List"; }
    float       defaultW()     const override { return 260.f; }
    float       defaultH()     const override { return 220.f; }
    bool        isLiveValueWidget() const override { return true; }
    bool        interactive()  const override { return true; }   // the list is edited where it is used
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // The list, as a prop: "rpm;map;clt". One string keeps it a plain element property — copyable,
    // undoable and savable like every other, with no side table to keep in step.
    static std::vector<std::string> parse(const std::string& s);
    static std::string              join(const std::vector<std::string>& v);

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        m.remove("value");                       // a list, not a reading
        using jf::JPropertyMeta;
        m.add("channels",  this, &ChannelListWidget::m_channels,
              JPropertyMeta{ .label = "Channels", .order = 100 });
        m.add("labelText", this, &ChannelListWidget::m_title,
              JPropertyMeta{ .label = "Title", .order = 99 });
        m.add("rowHeight", this, &ChannelListWidget::m_rowH,
              JPropertyMeta{ .label = "Row Height", .def = "22", .min = 12, .max = 60, .order = 101 });
        m.add("showUnits", this, &ChannelListWidget::m_showUnits,
              JPropertyMeta{ .label = "Show Units", .def = "1", .order = 102 });
        m.add("editable",  this, &ChannelListWidget::m_editable,
              JPropertyMeta{ .label = "Editable in Run Mode", .def = "1", .order = 103 });
    }

private:
    std::string m_channels;                      // "rpm;map;clt"
    std::string m_title;
    double      m_rowH = 22;
    bool        m_showUnits = true;
    bool        m_editable = true;

    // Which row the pointer is over, so a click knows what it hit and the paint can show it. Screen-space
    // geometry is derived the same way in both, from this one helper.
    static int rowAt(const jf::JRect& r, float titleH, float rowH, float my);
};

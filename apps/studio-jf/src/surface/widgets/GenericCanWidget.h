#pragma once
//
// GenericCanWidget — the generic CAN editor, ON the page.
//
// Not a read-out with an Edit button: the frames ARE what this page is for, so the page is the
// editor. It hosts a GenericCanPanel and hands it the widget's own box — two lists (receive and
// transmit), the frame's settings, a signal table edited in place, and the bit grid that is the only
// part able to say which bits a field occupies and whether it collides with its neighbour.
//
// A page widget that owns a cluster of real controls is unusual here — most host one, through
// HostedControlWidget — so the ownership is explicit: this widget owns the panel, places it, paints
// it and forwards input to it. The panel positions its own children (see its header for why it is a
// JWidget rather than a JContainer).
//
#include "../CanvasWidget.h"
#include "../../ui/GenericCanPanel.h"

#include <memory>
#include <string>

class GenericCanWidget : public CanvasWidget {
public:
    explicit GenericCanWidget(jf::JSceneGraph& g)
        : CanvasWidget(g, "genericcan"), m_panel(std::make_unique<GenericCanPanel>(g)) {
        addChild(m_panel.get());          // tree edge only — this widget decides the geometry
    }

    // WITHOUT THESE THE PAGE IS A PICTURE. The Surface only routes run-mode input to a widget that
    // says it is interactive, so every click on Add Frame went nowhere at all.
    bool interactive()   const override { return true; }
    bool drawsOwnFocus() const override { return true; }   // the hosted controls draw their own rings

    std::string elementType()  const override { return "genericcan"; }
    std::string paletteTitle() const override { return "Generic CAN"; }
    float       defaultW()     const override { return 1240.f; }
    float       defaultH()     const override { return 640.f; }

    // Which bus and which direction this page is about. The page says both by being the page it is;
    // a Bus picker and a direction switch would be two controls re-asking what the tree already
    // answered, and two more states to get out of step with it.
    const std::string& bus() const { return m_bus; }
    void setBus(const std::string& b) { if (m_bus != b) { m_bus = b; emitModified(); } }
    const std::string& dir() const { return m_dir; }
    void setDir(const std::string& d) { if (m_dir != d) { m_dir = d; emitModified(); } }

protected:
    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) override {
        m_panel->configure(busIndex(), m_dir != "Receive");
        m_panel->setBox(r);
        m_panel->populateRenderPrimitives(buf);
    }

    // Run-mode input, the way every hosted control on the canvas gets it.
    bool handleControlInput(const jf::JRect&, const ControlInput& in) override {
        switch (in.kind) {
            case ControlInput::Kind::Press:   return m_panel->press(in.mx, in.my);
            case ControlInput::Kind::Move:    m_panel->move(in.mx, in.my);    return true;
            case ControlInput::Kind::Release: m_panel->release(in.mx, in.my); return true;
            case ControlInput::Kind::Scroll:  return m_panel->scroll(in.mx, in.my, in.wheel);
            case ControlInput::Kind::Key:     return in.key && m_panel->key(*in.key);
            case ControlInput::Kind::Blur:    m_panel->blur();                return true;
            default:                          return false;
        }
    }

    void loadContent(const PanelElement& el) override {
        m_bus = el.prop("bus");
        m_dir = el.prop("dir");
        m_panel->attach();               // the frames are config, and config changes without asking
    }
    void saveContent(PanelElement& el) const override {
        el.props["bus"] = m_bus;
        el.props["dir"] = m_dir;
    }
    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");
        using jf::JPropertyMeta;
        m.add("bus", this, &GenericCanWidget::bus, &GenericCanWidget::setBus,
              JPropertyMeta{ .label = "Bus", .def = "0", .editor = "expr", .order = 150 });
        m.add("dir", this, &GenericCanWidget::dir, &GenericCanWidget::setDir,
              JPropertyMeta{ .label = "Direction", .def = "Transmit", .order = 151,
                             .choices = { std::string("Transmit"), std::string("Receive") } });
    }

private:
    int busIndex() const;

    std::unique_ptr<GenericCanPanel> m_panel;
    std::string m_bus = "0";
    std::string m_dir = "Transmit";
};

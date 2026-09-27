#pragma once

// TextFieldWidget — an editable STRING config field (a VIN, an engine name, a script's label), hosting a
// real framework jf::JLineEdit through HostedControlWidget exactly as ConfigEditWidget hosts a spin box.
//
// A string field is an ordinary config path whose bytes are TEXT: Cache reads and writes it through
// configString/setConfigString, so the tune, the ECU write queue and the undo stack all treat it like any
// other edit. Nothing here knows about ASCII storage beyond asking the cache for the string.

#include "HostedControlWidget.h"
#include <j/core/JLineEdit.h>
#include <memory>
#include <string>

class TextFieldWidget : public HostedControlWidget {
public:
    explicit TextFieldWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "text") {}
    std::string elementType() const override { return "text"; }
    std::string paletteTitle() const override { return "Text Field"; }
    float       defaultW()     const override { return 220.f; }
    float       defaultH()     const override { return 30.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("readOnly", this, &TextFieldWidget::m_readOnly, JPropertyMeta{ .label = "Read Only", .order = 105 });
    }

protected:
    void loadContent(const PanelElement& el) override { m_readOnly = el.prop("readOnly") == "1"; }
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JLineEdit> m_edit;
    bool m_readOnly = false;
};

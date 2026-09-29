#include "PinWireWidget.h"
#include "../../ui/WrapText.h"

#include "WireColors.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"
#include "../WidgetRegistry.h"

#include <cctype>
#include <string>
#include <cctype>

void PinWireWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    if (!jf::JTextHelper::hasAtlas()) return;
    const MetaModel* meta = c.meta();
    if (!meta || m_resource.empty()) return;

    std::string pin, code;
    if (!meta->wiringTrim(m_resource, pin, code)) {
        // NOT IN THE CONNECTOR MAP. Say so rather than drawing an empty row, which reads as a page
        // that failed: an internal pin with no terminal is a fact about the board, not a fault.
        static const wirecol::RGBA kDim{ 0x8a, 0x8f, 0x98, 255 };
        const std::string why = m_resource + " — not on a connector";   // short: it shares a row with a reading
        wraptext::draw(buf, r.x + 4.f, r.y + std::max(0.f, (r.height - wraptext::height(why, r.width - 8.f)) * 0.5f),
                       why, kDim.data(), r.width - 8.f);
        return;
    }

    // "CN2-25" names its connector, and the connector names its shell colour.
    std::string shellName;
    if (const auto dash = pin.find('-'); dash != std::string::npos)
        if (const auto it = meta->connectors().find(pin.substr(0, dash)); it != meta->connectors().end())
            shellName = it->second.color;
    const std::string shown = wirecol::terminal(pin, shellName);

    const float th = jf::JTextHelper::lineHeight();
    const float ty = r.y + (r.height - th) * 0.5f;
    float x = r.x + 4.f;

    uint8_t fgb[4];
    static const wirecol::RGBA kFg{ 0xe6, 0xe6, 0xe6, 255 };
    const uint8_t* fg = element() ? fgOf(*element(), fgb) : kFg.data();
    const std::string head = m_resource + " - ";
    jf::JTextHelper::pushText(buf, x, ty, head, fg);
    x += jf::JTextHelper::measureWidth(head);

    // The connector, reverse-video in its own shell colour — the thing you look for on the loom.
    const wirecol::RGBA sh  = wirecol::shell(shellName);
    const float pad = 4.f, pw = jf::JTextHelper::measureWidth(shown);
    static const wirecol::RGBA kBorder{ 0x90, 0x90, 0x90, 255 };
    buf.pushRectangle(x, ty - 2.f, pw + pad * 2.f, th + 4.f, sh.data(), 3.f, 1.f, kBorder.data());
    jf::JTextHelper::pushText(buf, x + pad, ty, shown, wirecol::ink(sh).data());
    x += pw + pad * 2.f + 10.f;

    // …and the wire itself, drawn exactly as the assign widget draws it.
    if (!code.empty()) wirecol::drawWire(buf, x, r.y + r.height * 0.5f, 34.f, code);
}

REGISTER_WIDGET("pinwire", PinWireWidget, 102);

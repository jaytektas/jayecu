#include "ChannelListWidget.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"
#include "../Surface.h"   // Surface::onModified — a run-mode layout edit still has to be saved

#include <algorithm>
#include <cstdio>

std::vector<std::string> ChannelListWidget::parse(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string ChannelListWidget::join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& c : v) { if (!s.empty()) s += ';'; s += c; }
    return s;
}

int ChannelListWidget::rowAt(const jf::JRect& r, float titleH, float rowH, float my) {
    if (rowH <= 0.f) return -1;
    const float top = r.y + titleH + 2.f;
    if (my < top) return -1;
    return static_cast<int>((my - top) / rowH);
}

void ChannelListWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;
    const std::vector<std::string> chans = parse(el.prop("channels"));
    const float lh = jf::JTextHelper::lineHeight();
    const std::string title = el.prop("labelText");
    const float titleH = title.empty() ? 4.f : lh + 6.f;
    const float rowH = std::max(12.f, static_cast<float>(std::atof(el.prop("rowHeight").empty()
                                                                   ? "22" : el.prop("rowHeight").c_str())));
    uint8_t fb[4], db[4];
    const uint8_t* fg = elColor(el, "fgColor", jf::Colors::TextPrimary, fb);
    const uint8_t* dim = elColor(el, "accentColor", jf::Colors::TextSecondary, db);

    if (!title.empty() && jf::JTextHelper::hasAtlas())
        jf::JTextHelper::pushText(buf, r.x + 6.f, r.y + 3.f, title, dim);

    const MetaModel* meta = c.meta();
    float y = r.y + titleH + 2.f;
    for (size_t i = 0; i < chans.size() && y + rowH <= r.y + r.height; ++i, y += rowH) {
        if (!jf::JTextHelper::hasAtlas()) break;
        const std::string& ch = chans[i];
        // The name as the meta gives it, so a row says what the channel IS rather than what it is called
        // in the bus — "Coolant Temperature", not "clt".
        // Name and unit as the TELEMETRY DESCRIPTOR gives them, so a row says what the channel is
        // ("Coolant Temperature", °C) rather than what the bus calls it ("clt").
        std::string label = ch, unit;
        if (meta) {
            const auto it = meta->telemetry().find(ch);
            if (it != meta->telemetry().end()) {
                if (!it->second.label.empty()) label = it->second.label;
                if (m_showUnits) unit = it->second.units;
            }
        }
        char vb[32] = "—";
        if (c.has(ch)) {
            const std::string fmt = el.prop("format").empty() ? "%.2f" : el.prop("format");
            std::snprintf(vb, sizeof vb, fmt.c_str(), c.value(ch));
        }
        const float vw = jf::JTextHelper::measureWidth(vb);
        const float uw = unit.empty() ? 0.f : jf::JTextHelper::measureWidth(unit) + 6.f;
        jf::JTextHelper::pushText(buf, r.x + 6.f, y + (rowH - lh) * 0.5f, label, fg,
                                  r.width - vw - uw - 18.f);
        jf::JTextHelper::pushText(buf, r.x + r.width - 6.f - uw - vw, y + (rowH - lh) * 0.5f, vb, fg);
        if (!unit.empty())
            jf::JTextHelper::pushText(buf, r.x + r.width - 4.f - uw + 2.f, y + (rowH - lh) * 0.5f, unit, dim);
    }
    // The invitation to add one, only while the list can be edited and only if there is room to draw it.
    if (m_editable && jf::JTextHelper::hasAtlas() && y + rowH <= r.y + r.height)
        jf::JTextHelper::pushText(buf, r.x + 6.f, y + (rowH - lh) * 0.5f, "+ add channel", dim);
}

bool ChannelListWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    if (in.kind != ControlInput::Kind::Press || !m_el) return false;
    const PanelElement& el = *m_el;
    if (el.prop("editable") == "0") return false;

    std::vector<std::string> chans = parse(el.prop("channels"));
    const float lh = jf::JTextHelper::lineHeight();
    const float titleH = el.prop("labelText").empty() ? 4.f : lh + 6.f;
    const float rowH = std::max(12.f, static_cast<float>(std::atof(el.prop("rowHeight").empty()
                                                                   ? "22" : el.prop("rowHeight").c_str())));
    const int row = rowAt(screen, titleH, rowH, in.my);
    if (row < 0 || row > static_cast<int>(chans.size())) return false;

    const MetaModel* meta = Cache::instance().meta();
    if (!meta || !enumpick::signal()) return false;
    std::vector<std::string> names;
    for (const auto& [nm, id] : meta->signalMap()) names.push_back(nm);
    std::sort(names.begin(), names.end());

    const std::string current = row < static_cast<int>(chans.size()) ? chans[row] : std::string();
    // The picker's empty answer means "none", which on an existing row is a delete — the only way to take
    // a channel off a list you built by adding to it.
    enumpick::signal()(std::move(names), current, [this, row, chans](std::string picked) mutable {
        if (row < static_cast<int>(chans.size())) {
            if (picked.empty()) chans.erase(chans.begin() + row);
            else                chans[row] = picked;
        } else if (!picked.empty()) {
            chans.push_back(picked);
        }
        // The operator's choice is kept like any other property: the instance owns the override, and the
        // surface commits it with everything else at save. Marking the document modified is what makes
        // the save happen at all — this is a layout edit made in run mode, which nothing else does.
        setOwnProp("channels", join(chans));
        if (Surface::onModified) Surface::onModified();
        invalidate();
    });
    return true;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory).
#include "../WidgetRegistry.h"
REGISTER_WIDGET("channels", ChannelListWidget, 165);

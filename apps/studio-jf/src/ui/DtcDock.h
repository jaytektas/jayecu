#pragma once

// DtcDock — the right-side "Diagnostic Trouble Codes" dock. Decodes the serialized DtcManager image (EcuLink
// 'G' read → dtcImageReady) into a 7-column table — Code · Status · Severity · Source · Count · First seen ·
// Last seen: active codes red, stored-only grey; Level 1/2/3
// severities; "N active · M stored" summary; Refresh / Clear All. Image format: magic "DTC1" @0, count u16 @8,
// then 36-byte records from offset 10 (code u16@0, source@2, severity@3, status@4 [bit0 active/bit1 stored],
// count u16@6, first-seen boot u16@8 + ms u32@10, last-seen boot u16@14 + ms u32@16).

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JButton.h>
#include <j/core/JContainer.h>
#include <j/core/JDataGrid.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLabel.h>

#include "../model/MetaModel.h"   // dtcDescription(code) — the hover explanation for each trouble code
#include "../model/Cache.h"       // the live protection level (prot_level)

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

// DTC rows on the toolkit's JDataGrid. This used to be a hand-painted jf::JWidget because the grid was
// string-only and could not colour a row — the grid now does per-row tints and exposes drawRowCell, so the
// bespoke renderer (and with it: no sorting, no column resize, no scrolling, no selection) is gone.
// Only the two things that are genuinely DTC-specific remain: severity-driven text colour and the per-row
// description tooltip.
class DtcTable : public jf::JDataGrid {
public:
    struct Row { std::string code, status, severity, source, count, first, last, desc, freeze; bool active = false, level3 = false; };

    explicit DtcTable(jf::JSceneGraph& g)
        : jf::JDataGrid(g, { "Code", "Status", "Severity", "Source", "Count", "First seen", "Last seen" })
    {
        setSortable(true);          // click a header — most useful on Severity, Count and Last seen
        setColumnsResizable(true);  // drag a divider; double-click one to fit the column
        setColumnAlignment(4, ColAlign::Right);   // Count is a number
    }

    // WIDTHS FROM THE CONTENT, not from the dock. With no explicit widths the grid divides the
    // available width equally, and this dock is ~250 px across seven columns — 35 px each, which
    // clipped every header to two or three letters ("Co", "Sta", "Se") and every cell with it. The
    // grid scrolls horizontally, so overflow is a scrollbar rather than a guess at what "Fir" meant.

    void setRows(std::vector<Row> r) {
        m_meta = std::move(r);
        std::vector<std::vector<std::string>> cells;
        cells.reserve(m_meta.size());
        for (const Row& x : m_meta)
            cells.push_back({ x.code, x.status, x.severity, x.source, x.count, x.first, x.last });
        jf::JDataGrid::setRows(cells);
        autoFitAllColumns();
        _refreshTooltip(lastMy_);   // rows changed under a stationary pointer -> recompute, don't clear
    }

    // Per-row hover explanation. rowAtY() is the grid's own row mapping, so the tooltip cannot drift out of
    // step with the drawn rows the way the old hand-rolled geometry could — and it stays correct once the
    // rows are sorted or scrolled, which the previous version did not handle at all.
    void handleMouseMove(float mx, float my) override {
        jf::JDataGrid::handleMouseMove(mx, my);
        lastMy_ = my;
        _refreshTooltip(my);
    }

private:
    // The row explanation for the row under `y`. Shared by hover and by a row rebuild, so a refresh under
    // a stationary pointer keeps the tooltip instead of dropping it.
    void _refreshTooltip(float y) {
        const int r = rowAtY(y);
        std::string tip;
        if (r >= 0 && r < static_cast<int>(m_meta.size())) {
            const Row& row = m_meta[r];
            tip = row.code + (row.desc.empty() ? "  \xE2\x80\x94  no description" : "  \xE2\x80\x94  " + row.desc);
            if (!row.freeze.empty()) tip += "\n" + row.freeze;
        }
        if (tip != tooltip()) setTooltip(tip);
    }
    float lastMy_ = -1.f;   // last pointer y, so a rebuild can recompute the hover tip
public:

protected:
    // Active faults read red; stored-only read muted. The first three columns (code/status/severity) go
    // bold-ish for a level 3 so the severe ones stand out at a glance — same emphasis the old renderer had.
    void drawRowCell(jf::JPrimitiveBuffer& buf, int rowIdx, int colIdx, const jf::JRect& bounds,
                     const std::string& val, bool selected) override {
        if (!jf::JTextHelper::hasAtlas()) return;
        const bool active = (rowIdx >= 0 && rowIdx < (int)m_meta.size()) && m_meta[rowIdx].active;
        const bool level3 = (rowIdx >= 0 && rowIdx < (int)m_meta.size()) && m_meta[rowIdx].level3;
        const uint8_t* fg = active ? redCol() : jf::Colors::TextSecondary;
        const uint8_t* tc = (colIdx <= 2 && level3) ? (active ? redCol() : jf::Colors::TextPrimary) : fg;
        if (selected) tc = jf::Colors::TextPrimary;
        const float ty = bounds.y + (bounds.height - jf::JTextHelper::lineHeight()) * 0.5f;
        jf::JTextHelper::pushText(buf, alignedTextX(colIdx, bounds, val), ty, val, tc,
                                  bounds.width - cellPadding() * 2.f);
    }

private:
    static const uint8_t* redCol() { static const uint8_t c[4] = { 214, 64, 64, 255 }; return c; }
    std::vector<Row> m_meta;   // decoded rows, parallel to the grid's strings (indexed by SOURCE row)
};

class DtcDock : public jf::JContainer {
public:
    std::function<void()> onRefresh, onClear;

    // The descriptor supplies DTC hover text (code → human description). Set once after meta loads.
    void setMeta(const MetaModel* m) { meta_ = m; }

    explicit DtcDock(jf::JSceneGraph& g) : jf::JContainer(g) {
        setLayoutMode(jf::JLayoutMode::Flex)->setDirection(jf::JFlexDirection::Column)
            ->setGap(6.f)->setPadding(jf::JEdges{ 8.f, 8.f, 8.f, 8.f })
            ->setAlignItems(jf::JAlignItems::Stretch);   // children fill the dock width (table + summary + button row)
        row_ = std::make_unique<jf::JContainer>(g);
        row_->setLayoutMode(jf::JLayoutMode::Flex)->setDirection(jf::JFlexDirection::JRow)->setGap(6.f);
        row_->setBounds({ 0.f, 0.f, 300.f, 22.f });
        refresh_ = std::make_unique<jf::JButton>(g, "Refresh", 90.f, 22.f);
        clear_   = std::make_unique<jf::JButton>(g, "Clear All", 90.f, 22.f);
        refresh_->onClicked.connect([this] { if (onRefresh) onRefresh(); });
        clear_->onClicked.connect([this] { if (onClear) onClear(); });
        row_->add(refresh_.get()); row_->add(clear_.get());
        summary_ = std::make_unique<jf::JLabel>(g, "No DTC data", 240.f, 20.f);
        table_   = std::make_unique<DtcTable>(g);
        table_->setBounds({ 0.f, 0.f, 300.f, 400.f });
        // Claim the dock's leftover height — without a policy the grid defaults to Preferred/stretch-0 and
        // sat at ~half the dock, the rest empty. Expanding (like TriggerLogPanel/EngineCyclePanel's view)
        // grows it to fill below the fixed button row + summary.
        table_->setVSizePolicy(jf::JSizePolicyMode::Expanding, 1);
        add(row_.get()); add(summary_.get()); add(table_.get());
    }

    void setImage(const std::vector<uint8_t>& img) {
        // The 1 Hz background poll re-reads this image continuously and it is almost always identical.
        // Rebuilding the table on every poll threw away the hover tooltip (and churned sort/selection),
        // which is why a stationary hover made the tooltip flash once a second. Ignore an unchanged image.
        if (img == lastImg_) return;
        lastImg_ = img;
        // Magic is the firmware's DTC_MAGIC (0x44544331, "DTC1"): memcpy'd LE on the MCU → wire bytes
        // 31 43 54 44 → our LE u32() reads them back as 0x44544331. (A prior port used 0x31435444 here,
        // which never matches, so every real image was rejected as "No DTC data".)
        if (img.size() < 10 || u32(img, 0) != 0x44544331u) { summary_->setText("No DTC data"); table_->setRows({}); return; }
        const uint16_t n = u16(img, 8);
        int active = 0, stored = 0;
        std::vector<DtcTable::Row> rows;
        char tmp[48];
        for (size_t i = 0, off = 10; i < n && off + 36 <= img.size(); ++i, off += 36) {
            const uint16_t code = u16(img, off); if (!code) continue;
            const uint8_t source = img[off + 2], severity = img[off + 3], status = img[off + 4];
            const uint16_t count = u16(img, off + 6);
            const uint16_t fboot = u16(img, off + 8);  const uint32_t fms = u32(img, off + 10);
            const uint16_t lboot = u16(img, off + 14); const uint32_t lms = u32(img, off + 16);
            const bool a = status & 0x01, st = status & 0x02;
            if (a) ++active;
            if (st) ++stored;
            DtcTable::Row r;
            std::snprintf(tmp, sizeof(tmp), "P%04X", code);   r.code = tmp;
            r.status   = (a && st) ? "Active+Stored" : a ? "Active" : st ? "Stored" : "-";
            r.severity = sev(severity);
            r.source = sourceName(source);
            std::snprintf(tmp, sizeof(tmp), "%u", count);     r.count = tmp;
            r.first = uptime(fboot, fms);
            r.last  = uptime(lboot, lms);
            r.desc  = meta_ ? meta_->dtcDescription(code) : std::string();   // exact phrase → OBD category → ""
            r.active = a; r.level3 = (severity >= 3);
            // THE FREEZE FRAME. The ECU snapshots rpm, MAP, coolant and battery at the latest activation
            // edge (DtcRecord::ff, bytes 20..35) and it was decoded by nothing — the one piece of a code
            // that says what the engine was doing when it happened. Metric, as the ECU records it.
            if (off + 36 <= img.size()) {
                const float rpm = f32(img, off + 20), map = f32(img, off + 24);
                const float clt = f32(img, off + 28), bat = f32(img, off + 32);
                if (rpm != 0.f || map != 0.f || clt != 0.f || bat != 0.f) {
                    char ffb[128];
                    std::snprintf(ffb, sizeof(ffb),
                                  "When it last went active: %.0f RPM \xC2\xB7 %.1f kPa \xC2\xB7 %.1f \xC2\xB0" "C \xC2\xB7 %.1f V",
                                  rpm, map, clt, bat);
                    r.freeze = ffb;
                }
            }
            rows.push_back(std::move(r));
        }
        char sum[64]; std::snprintf(sum, sizeof(sum), "%d active \xC2\xB7 %d stored", active, stored);
        // THE PROTECTION LEVEL IN FORCE, live, beside the codes that put it there — and after they have
        // gone, since a level can hold past its fault or act on stored ones. Nothing else in the studio
        // said the engine was in limp.
        std::string line = rows.empty() ? std::string("No trouble codes") : std::string(sum);
        const Cache& c = Cache::instance();
        if (c.linkOpen() && c.has("prot_level")) {
            const int lv = static_cast<int>(c.value("prot_level") + 0.5);
            line += lv > 0 ? " \xC2\xB7 Protection level " + std::to_string(lv) + " in force"
                           : std::string(" \xC2\xB7 no protection");
        }
        summary_->setText(line);
        table_->setRows(std::move(rows));
    }

    // Clear the table on disconnect. On connect we leave the
    // last image up until the 1 Hz poll refreshes it, so there's no flash of "not connected".
    void setConnected(bool connected) {
        if (!connected) { summary_->setText("not connected"); table_->setRows({}); }
    }

private:
    static uint16_t u16(const std::vector<uint8_t>& b, size_t o) { return static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); }
    static uint32_t u32(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24); }
    static float f32(const std::vector<uint8_t>& b, size_t o) { const uint32_t v = u32(b, o); float f; std::memcpy(&f, &v, 4); return f; }
    static const char* sev(uint8_t s) { switch (s) { case 1: return "Level 1"; case 2: return "Level 2"; case 3: return "Level 3"; default: return "-"; } }
    static std::string uptime(uint16_t boot, uint32_t ms) { char b[32]; std::snprintf(b, sizeof(b), "boot %u \xC2\xB7 %.1fs", boot, ms / 1000.0); return b; }

    // WHO RAISED IT, in words. This column used to print the firmware's raw id — "98", "94" — which is
    // the one number in the row that means nothing to the person reading it. The ids come from
    // DtcSource in firmware/Diagnostics/Dtc.h; the meta does not carry them, so this mapping is a copy
    // and has to be updated with that header. Anything below SUBSYS_BASE is not a subsystem at all —
    // it is SENSOR_BASE + the sensor's catalog index, so the sensor gets named from the meta.
    std::string sourceName(uint8_t s) const {
        switch (s) {
            case 200: return "Trigger";
            case 201: return "Protection";
            case 202: return "Pin arbiter";
            case 203: return "Config";
            case 204: return "CAN bus";
            case 205: return "Lua";
            case 206: return "Throttle";
            case 207: return "Module";
            case 208: return "Sensor supply";
            default: break;
        }
        if (meta_) {
            const auto& arrs = meta_->configArrays();
            const auto it = arrs.find("sensors.sensor");
            if (it != arrs.end() && s < it->second.elementLabels.size()
                && !it->second.elementLabels[s].empty())
                return it->second.elementLabels[s];
        }
        return "Sensor " + std::to_string(static_cast<unsigned>(s));
    }

    const MetaModel* meta_ = nullptr;   // DTC code → description for row hover text
    std::unique_ptr<jf::JContainer> row_;
    std::unique_ptr<jf::JButton>    refresh_, clear_;
    std::unique_ptr<jf::JLabel>     summary_;
    std::unique_ptr<DtcTable>       table_;
    std::vector<uint8_t>            lastImg_;   // last DTC image applied — an identical poll is ignored
};

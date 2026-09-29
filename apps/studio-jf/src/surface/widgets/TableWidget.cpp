// TableWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "TableWidget.h"

#include <algorithm>
#include <iterator>
#include "../../model/Cache.h"
#include "../../model/EditorSettings.h"
#include <j/graphics/VectorGraphics.h>
#include "../../model/MetaModel.h"
#include "../../model/UnitManager.h"

// Display units in, display units out; raw in the cache. dispV/srcV are static on CanvasWidget and touch
// nothing but their element, so the Surface's table operations use these same two rather than a parallel
// pair working in engineering units.
double TableWidget::cellValue(const PanelElement& el, const TableImage& t, int offset) {
    return dispV(el, Raw{Cache::instance().readRaw(Cache::instance().cellLocAt(t, offset))});
}
void TableWidget::setCellValue(const PanelElement& el, const TableImage& t, int offset, double display) {
    Cache::instance().writeRaw(Cache::instance().cellLocAt(t, offset), srcV(el, display));
}
#include "../../model/Keymap.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// HSV (h deg, s/v 0..1) -> RGBA, for the 3D surface's height + shading colour (the table's own concern).
static void hsv2rgb(float h, float s, float v, uint8_t out[4]) {
    h = std::fmod(h, 360.f); if (h < 0) h += 360.f;
    const float c = v * s, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; } else if (h < 120) { rr = x; gg = c; } else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; } else if (h < 300) { rr = x; bb = c; } else { rr = c; bb = x; }
    out[0] = static_cast<uint8_t>((rr + m) * 255); out[1] = static_cast<uint8_t>((gg + m) * 255);
    out[2] = static_cast<uint8_t>((bb + m) * 255); out[3] = 255;
}

void TableWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    const bool cells = jf::JTextHelper::hasAtlas();
    const TableGeom g = tableGeom(el, r, c, elementContext(), renderZoom());
    // Font comes from the geom, so what is DRAWN and what is HIT-TESTED can never disagree.
    const float lh = g.lh > 0.f ? g.lh : jf::JTextHelper::lineHeight();
    const float fsc = g.fscale;
    const std::string& fface = g.face;
    // Every text call in this widget goes through these two, so the Font property reaches all of them
    // rather than the handful someone remembered to convert.
    auto text = [&](jf::JPrimitiveBuffer& b, float x, float y, const std::string& s,
                    const uint8_t col[4], float maxW = 0.f) {
        jf::JTextHelper::pushTextScaled(b, x, y, s, col, fsc, maxW, fface);
    };
    auto textW = [&](const std::string& s) { return jf::JTextHelper::measureWidthScaled(s, fsc); };
    if (g.ok) {
        // Row Source: a sigil DRIVES this table's selected row — point it at another table's
        // [@other.selectedRow] and this one follows. The value is a STORAGE axis-1 index (the space
        // selectedRow returns); convert to THIS table's screen row via transpose so the slice/2D cursor
        // lands on the matching row, and mirror it into the anchor so selectedRow chains through. Empty =
        // driven by the user's own clicks.
        const std::string rs = el.prop("rowSource");
        if (!rs.empty()) {
            const double v = evalSource(rs).v;
            if (std::isfinite(v)) {
                const int i1 = static_cast<int>(std::lround(v));
                const int screenRow = std::clamp(g.transpose ? (g.rows - 1 - i1) : i1, 0, std::max(0, g.rows - 1));
                TableCursor& cur = tableCursors()[el.uid];
                cur.row = screenRow; cur.aRow = screenRow;
            }
        }
    }
    if (!g.ok) {
        // A table that NAMES one this meta does not have is not an empty slot: it is a page newer than
        // the firmware (the ECU needs updating), and "Drop a table here" sent the user looking for a
        // wrong binding. Say which table and why.
        const std::string bound = el.prop("signalName");
        const std::string msg = bound.empty() ? std::string("Drop a table here")
                                              : "Not in this ECU's firmware: " + bound + " (update the firmware)";
        if (cells) text(buf, r.x + std::max(0.f, (r.width - textW(msg)) * 0.5f),
                        r.y + (r.height - lh) * 0.5f, msg, jf::Colors::TextSecondary, r.width);
        return;
    }
    uint8_t fgb[4]; const uint8_t* fg = fgOf(el, fgb);
    char vb[128];   // the slice caption: 43 chars of format before a single number lands in it
    const int hmScheme = choiceOf(el, "heatmapScheme") - 1;   // global defaults to the first scheme
    auto heatColor = [hmScheme](double n, uint8_t o[4]) {
        n = std::clamp(n, 0.0, 1.0);
        float h, s, v;
        switch (hmScheme) {
            case 1:  h = 120.f * (1.f - n); s = 180.f/255.f; v = 220.f/255.f;         break;  // Green -> Red
            case 2:  h = 0.f;               s = 0.f;         v = (60.f + 170.f*n)/255.f; break;  // Grayscale
            case 3:  h = 240.f - 120.f * n; s = 180.f/255.f; v = 220.f/255.f;         break;  // Cool
            case 4:  h = 60.f  * (1.f - n); s = 200.f/255.f; v = 230.f/255.f;         break;  // Warm
            default: h = 240.f * (1.f - n); s = 180.f/255.f; v = 220.f/255.f;         break;  // Blue -> Red
        }
        hsv2rgb(h, s, v, o); o[3] = 235;
    };
    std::vector<double> cell(static_cast<size_t>(g.rows) * g.cols, 0.0);
    double mn = 1e300, mx = -1e300;
    for (int rr = 0; rr < g.rows; ++rr)
        for (int cc = 0; cc < g.cols; ++cc) {
            const double v = cellValue(el, g.t, g.cellOffset(rr, cc));
            cell[rr * g.cols + cc] = v; mn = std::min(mn, v); mx = std::max(mx, v);
        }
    if (!(mx > mn)) mx = mn + 1.0;
    const int view = m_view;   // the widget owns its view mode
    if (view == 2) {   // Slice
        uint8_t sbg[4] = {30, 30, 34, 255}; buf.pushRectangle(r.x, r.y, r.width, r.height, sbg, 0.f);
        const TableCursor* scur = nullptr;
        { const auto it = tableCursors().find(el.uid); if (it != tableCursors().end()) scur = &it->second; }
        const int sr = scur ? std::clamp(scur->row, 0, g.rows - 1) : 0, scc = scur ? std::clamp(scur->col, 0, g.cols - 1) : 0;
        const float plotX = r.x + 46.f, plotY = r.y + lh + 4.f, plotW = r.width - 54.f, plotH = r.height - lh * 2.f - 10.f;
        if (plotW < 4.f || plotH < 4.f) return;
        const float bcw = plotW / g.cols;
        auto yOf = [&](double v) { return plotY + plotH - static_cast<float>((v - mn) / (mx - mn)) * plotH; };
        jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
        if (cells) for (int i = 0; i <= 4; ++i) {
            const float yy = plotY + plotH * i / 4.f;
            vg.strokePolyline({ {plotX, yy}, {plotX + plotW, yy} }, 0.6f, jf::JPaint::solid(jf::rgba(60, 60, 70, 255)), false);
            std::snprintf(vb, sizeof(vb), "%.*f", g.dec, mx - (mx - mn) * i / 4.0);
            text(buf, r.x + 2.f, yy - lh * 0.5f, vb, jf::Colors::TextSecondary, 42.f);
        }
        uint8_t hl[4] = {255, 235, 60, 255};
        uint8_t span[4] = {255, 235, 60, 44};
        const int ssc0 = scur ? std::clamp(std::min(scur->col, scur->aCol), 0, g.cols - 1) : scc;
        const int ssc1 = scur ? std::clamp(std::max(scur->col, scur->aCol), 0, g.cols - 1) : scc;
        for (int cc = 0; cc < g.cols; ++cc) {
            const float y = yOf(cell[sr * g.cols + cc]);
            if (cc >= ssc0 && cc <= ssc1) buf.pushRectangle(plotX + cc * bcw, plotY, bcw, plotH, span, 0.f);
            uint8_t hc[4]; heatColor(static_cast<float>((cell[sr * g.cols + cc] - mn) / (mx - mn)), hc); hc[3] = 150;
            buf.pushRectangle(plotX + cc * bcw + 1.f, y, bcw - 2.f, plotY + plotH - y, hc, 1.f);
            if (cc == scc) buf.pushRectangle(plotX + cc * bcw, plotY, bcw, plotH, jf::Colors::Transparent, 0.f, 1.5f, hl);
        }
        std::vector<jf::JVectorCanvas::JVec2> line; line.reserve(g.cols);
        for (int cc = 0; cc < g.cols; ++cc) line.push_back({ plotX + cc * bcw + bcw * 0.5f, yOf(cell[sr * g.cols + cc]) });
        if (line.size() >= 2) vg.strokePolyline(line, 1.8f, jf::JPaint::solid(jf::rgba(120, 200, 120, 255)), false);
        vg.flush(buf);
        if (cells) {
            const int cstep = std::max(1, g.cols / 8);
            for (int cc = 0; cc < g.cols; cc += cstep) { std::snprintf(vb, sizeof(vb), "%s", g.viewH.text(g.breakpointX(cc)).c_str());
                text(buf, plotX + cc * bcw, plotY + plotH + 2.f, vb, jf::Colors::TextSecondary, bcw * 2.f); }
            if (scur && scur->editing)
                std::snprintf(vb, sizeof(vb), "Slice - row %d   editing = %s_", sr, scur->buf.c_str());
            else if (ssc1 > ssc0)
                std::snprintf(vb, sizeof(vb), "Slice - row %d (Y=%.*f)   cols %d..%d   cell = %.*f", sr, g.decV, g.viewV.toDisplay(g.breakpointY(sr)), ssc0, ssc1, g.dec, cell[sr * g.cols + scc]);
            else
                std::snprintf(vb, sizeof(vb), "Slice - row %d (Y=%.*f)   cell = %.*f", sr, g.decV, g.viewV.toDisplay(g.breakpointY(sr)), g.dec, cell[sr * g.cols + scc]);
            text(buf, r.x + 6.f, r.y + 3.f, vb, jf::Colors::TextPrimary, r.width - 12.f);
        }
        return;
    }
    if (view == 1) {                                  // 3D
        uint8_t bg3[4] = {20, 20, 23, 255}; buf.pushRectangle(r.x, r.y, r.width, r.height, bg3, 0.f);
        if (g.rows < 2 || g.cols < 2) {
            if (cells) text(buf, r.x + 8.f, r.y + r.height * 0.5f, "3D needs a 2-axis table", jf::Colors::TextSecondary, r.width);
            return;
        }
        const Table3DState& st = m_view3d;
        const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f + r.height * 0.16f;
        const float sxy = std::min(r.width, r.height) * 0.82f / std::max(std::max(g.cols, g.rows), 2);
        const float hPix = r.height * 0.38f * st.hScale;
        const float yaw = st.yaw * 3.14159265f / 180.f, pitch = st.pitch * 3.14159265f / 180.f;
        const float cyaw = std::cos(yaw), syaw = std::sin(yaw), spitch = std::sin(pitch);
        auto h01 = [&](double v) { return static_cast<float>(std::clamp((v - mn) / (mx - mn), 0.0, 1.0)); };
        auto proj = [&](float col, float row, float hv, jf::JVectorCanvas::JVec2& o, float& odepth) {
            const float X = col - (g.cols - 1) * 0.5f, Y = row - (g.rows - 1) * 0.5f;
            const float rx = X * cyaw - Y * syaw, ry = X * syaw + Y * cyaw;
            o.x = cx + rx * sxy; o.y = cy + ry * sxy * spitch - hv * hPix; odepth = ry;
        };
        jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
        { jf::JVectorCanvas::JVec2 p0, p1; float dd;
          const jf::JPaint gc = jf::JPaint::solid(jf::rgba(58, 58, 70, 255));
          for (int cc = 0; cc < g.cols; cc += 2) { proj((float)cc, 0.f, 0.f, p0, dd); proj((float)cc, (float)g.rows - 1, 0.f, p1, dd); vg.strokePolyline({ p0, p1 }, 0.8f, gc, false); }
          for (int rr = 0; rr < g.rows; rr += 2) { proj(0.f, (float)rr, 0.f, p0, dd); proj((float)g.cols - 1, (float)rr, 0.f, p1, dd); vg.strokePolyline({ p0, p1 }, 0.8f, gc, false); } }
        const float lx = 0.35f, ly = -0.45f, lz = 0.82f, hModel = std::max(g.rows, g.cols) * 0.45f * st.hScale;
        const TableCursor* c3 = nullptr;
        { const auto it = tableCursors().find(el.uid); if (it != tableCursors().end()) c3 = &it->second; }
        const int cdr = c3 ? std::clamp(c3->row, 0, g.rows - 1) : -1;
        struct Q { float depth; jf::JVectorCanvas::JVec2 p[4]; uint8_t col[4]; int outline; };
        std::vector<Q> quads; quads.reserve(static_cast<size_t>(g.rows - 1) * (g.cols - 1));
        for (int rr = 0; rr < g.rows - 1; ++rr)
            for (int cc = 0; cc < g.cols - 1; ++cc) {
                const float z00 = h01(cell[rr * g.cols + cc]), z10 = h01(cell[rr * g.cols + cc + 1]);
                const float z11 = h01(cell[(rr + 1) * g.cols + cc + 1]), z01 = h01(cell[(rr + 1) * g.cols + cc]);
                Q q; float dep = 0, dt;
                proj((float)cc, (float)rr, z00, q.p[0], dt); dep += dt;
                proj((float)cc + 1, (float)rr, z10, q.p[1], dt); dep += dt;
                proj((float)cc + 1, (float)rr + 1, z11, q.p[2], dt); dep += dt;
                proj((float)cc, (float)rr + 1, z01, q.p[3], dt); dep += dt;
                q.depth = dep * 0.25f;
                const float uz = (z10 - z00) * hModel, vz = (z01 - z00) * hModel;
                const float nl = std::sqrt(uz * uz + vz * vz + 1.f) + 1e-6f;
                const float diff = std::clamp((-uz * lx - vz * ly + lz) / nl, 0.f, 1.f), bright = 0.55f + 0.45f * diff;
                const float hn = (z00 + z10 + z11 + z01) * 0.25f;
                hsv2rgb(240.f * (1.f - hn), 0.70f, std::clamp(0.9f * bright, 0.16f, 1.f), q.col);
                q.outline = 0;
                if (c3) {
                    const int adr = std::clamp(c3->aRow, 0, g.rows - 1);
                    const int r0 = std::min(cdr, adr), r1 = std::max(cdr, adr), k0 = std::min(c3->col, c3->aCol), k1 = std::max(c3->col, c3->aCol);
                    if (rr <= r1 && rr + 1 >= r0 && cc <= k1 && cc + 1 >= k0) q.outline = 2;
                    if ((rr == cdr || rr + 1 == cdr) && (cc == c3->col || cc + 1 == c3->col)) q.outline = 3;
                }
                quads.push_back(q);
            }
        std::sort(quads.begin(), quads.end(), [](const Q& x, const Q& y) { return x.depth < y.depth; });
        for (const Q& q : quads) {
            vg.fillConvex({ q.p[0], q.p[1], q.p[2], q.p[3] }, jf::JPaint::solid(jf::rgba(q.col[0], q.col[1], q.col[2], 255)));
            const std::vector<jf::JVectorCanvas::JVec2> ol{ q.p[0], q.p[1], q.p[2], q.p[3] };
            if (q.outline == 3)      vg.strokePolyline(ol, 2.2f, jf::JPaint::solid(jf::rgba(255, 235, 60, 255)), true);
            else if (q.outline == 2) vg.strokePolyline(ol, 1.3f, jf::JPaint::solid(jf::rgba(255, 224, 64, 255)), true);
            else                     vg.strokePolyline(ol, 0.5f, jf::JPaint::solid(jf::rgba(0, 0, 0, 60)), true);
        }
        vg.flush(buf);
        if (cells) {
            const int rstep = std::max(1, g.rows / 8), cstep = std::max(1, g.cols / 8);
            jf::JVectorCanvas::JVec2 pp; float dd; char lb[32];
            for (int rr = 0; rr < g.rows; rr += rstep) { proj(-0.6f, (float)rr, 0.f, pp, dd);
                std::snprintf(lb, sizeof(lb), "%s", g.viewV.text(g.breakpointY(rr)).c_str()); text(buf, pp.x - 28.f, pp.y - lh * 0.5f, lb, jf::Colors::TextSecondary, 40.f); }
            for (int cc = 0; cc < g.cols; cc += cstep) { proj((float)cc, (float)g.rows + 0.2f, 0.f, pp, dd);
                std::snprintf(lb, sizeof(lb), "%s", g.viewH.text(g.breakpointX(cc)).c_str()); text(buf, pp.x - 10.f, pp.y, lb, jf::Colors::TextSecondary, 40.f); }
            text(buf, r.x + 8.f, r.y + 6.f, "3D - drag rotate / wheel height / click+type to edit", jf::Colors::TextSecondary, r.width - 12.f);
        }
        return;
    }
    const TableCursor* cur = nullptr;
    { const auto it = tableCursors().find(el.uid); if (it != tableCursors().end()) cur = &it->second; }
    // The far-edge placements already ride the scrolled grid origin; the widget-edge ones are anchored to r,
    // so they take the scroll offset explicitly — the header strips scroll WITH the cells rather than staying
    // pinned (they'd otherwise sit still while the bins they label slid away underneath).
    const float xHdrY = g.xBottom ? (g.gy + g.rows * g.ch + 2.f) : (r.y + 3.f + g.nameH + g.sy);
    const float yHdrX = g.yRight ? (g.gx + g.cols * g.cw + 3.f) : (r.x + 3.f + g.sx);
    const bool axEditX = cur && cur->axisEdit == 1 && cur->editing;
    const bool axEditY = cur && cur->axisEdit == 2 && cur->editing;
    // Axis + grid colours: a local prop override (axisColor / gridColor) else the JStyle palette default —
    // PlaceholderText for the muted breakpoint labels, Highlight / HighlightedText for a selected bin, Border
    // for the cell grid. Retires the old hard-coded #bbbbc2 / #3dbdff / grey literals; theme-driven now.
    uint8_t axisDB[4], axisOB[4], selDB[4], selTxDB[4], gridDB[4], gridOB[4];
    const uint8_t* kBinMuted = skin::parseHex(m_axisColor, axisOB) ? static_cast<const uint8_t*>(axisOB)
                                                                   : paletteRole(jf::JColorRole::PlaceholderText, axisDB);
    const uint8_t* kBinSel   = paletteRole(jf::JColorRole::Highlight, selDB);
    const uint8_t* kBinSelTx = paletteRole(jf::JColorRole::HighlightedText, selTxDB);
    const uint8_t* kGrid     = skin::parseHex(m_gridColor, gridOB) ? static_cast<const uint8_t*>(gridOB)
                                                                   : paletteRole(jf::JColorRole::Border, gridDB);
    // Axis-name bar across the top: the signal driving the vertical
    // (↕) and horizontal (↔) screen axes. The arrows stay correct under transpose (unlike X/Y). "—" for a
    // disabled / unnamed axis. ↕/↔/· are all in the font atlas (arrow range 0x2190..0x2195 + Latin-1 ·).
    if (cells && g.nameH > 0.f) {
        // AND THE UNITS COME WITH THEM. A number without its unit is not a reading: a pressure cal
        // displays in whichever unit the user prefers — 3000 kPa shows as 30.0 where that preference is
        // bar — and with nothing on screen saying so it reads as kPa and is wrong by a hundred. The
        // axis names are where a unit belongs, since they already say what each direction IS.
        // Only the directions the table actually HAS. A curve has one axis, and a table whose optional
        // axis is switched off has one in play — naming the other with an em-dash and a unit ("— (°C)")
        // describes a direction the grid does not have. The strips are the same question, so the test is
        // the same: no breakpoints, no name.
        std::vector<std::string> parts;
        if (!g.yb.empty()) parts.push_back("\xE2\x86\x95 " + _named(g.vName, g.viewV));
        if (!g.xb.empty()) parts.push_back("\xE2\x86\x94 " + _named(g.hName, g.viewH));
        if (const std::string cu = _cellUnits(el, g); !cu.empty()) parts.push_back(cu);
        std::string nb;
        for (std::size_t pi = 0; pi < parts.size(); ++pi)
            nb += (pi ? "     \xC2\xB7     " : "") + parts[pi];   // a separator only ever BETWEEN parts
        text(buf, r.x + 4.f, r.y + 2.f, nb.c_str(), kBinMuted, r.width - 8.f);
    }
    const int selC0 = cur ? std::min(cur->col, cur->aCol) : 1, selC1 = cur ? std::max(cur->col, cur->aCol) : -1;
    const int selR0 = cur ? std::min(cur->row, cur->aRow) : 1, selR1 = cur ? std::max(cur->row, cur->aRow) : -1;
    // Scroll viewport: the header strips + grid are clipped to the widget minus the axis-name bar and the
    // bar gutters, so scrolled-out cells stop at the edge instead of painting over the name bar or the bars.
    // (CanvasWidget already clips to r; nested clips intersect, so this only ever tightens.)
    const float clipY = r.y + g.nameH, clipW = r.width - (g.ovV ? kTSB : 0.f);
    const float clipH = r.height - g.nameH - (g.ovH ? kTSB : 0.f);
    buf.pushClip(r.x, clipY, clipW, clipH);
    // A HEADING THAT IS A NAME, NOT A NUMBER. Some axes have no quantity to print — the VVT trim's
    // rows are the four cams, the bank trim's columns are banks — and the definition says so. The
    // storage axis is worked out the same way the live cursor works it out, so a transposed view puts
    // the names on the side the data is actually on.
    auto headLabel = [&g](int storageAxis, int idx) -> const std::string* {
        const std::vector<std::string>& v = (storageAxis == 0) ? g.t.colLabels : g.t.rowLabels;
        return (idx >= 0 && idx < static_cast<int>(v.size())) ? &v[static_cast<size_t>(idx)] : nullptr;
    };
    if (cells && g.hdrH > 0.f)
        for (int cc = 0; cc < g.cols && cc < static_cast<int>(g.xb.size()); ++cc) {
            const bool ed = axEditX && cur->axisIdx == cc;
            const bool sel = !ed && cc >= selC0 && cc <= selC1;
            const std::string* hl = headLabel(g.transpose ? 0 : 1, cc);
            if (ed)      std::snprintf(vb, sizeof(vb), "%s_", cur->buf.c_str());
            else if (hl) std::snprintf(vb, sizeof(vb), "%s", hl->c_str());
            else         std::snprintf(vb, sizeof(vb), "%s", g.viewH.text(g.xb[cc]).c_str());
            const float x = g.gx + cc * g.cw;
            if (ed)       buf.pushRectangle(x, xHdrY - 1.f, g.cw, lh + 2.f, jf::Colors::Accent, 2.f);
            else if (sel) buf.pushRectangle(x, xHdrY - 1.f, g.cw, lh + 2.f, kBinSel, 2.f);
            text(buf, x + (g.cw - textW(vb)) * 0.5f, xHdrY, vb, ed ? jf::Colors::Surface0 : (sel ? kBinSelTx : kBinMuted), g.cw);
        }
    for (int rr = 0; rr < g.rows; ++rr) {
        const int dr = g.rows - 1 - rr;
        const float y = g.gy + rr * g.ch;
        if (cells && g.hdrW > 0.f && dr < static_cast<int>(g.yb.size())) {
            const bool ed = axEditY && cur->axisIdx == dr;
            const bool sel = !ed && rr >= selR0 && rr <= selR1;
            const std::string* hl = headLabel(g.transpose ? 1 : 0, dr);
            if (ed)      std::snprintf(vb, sizeof(vb), "%s_", cur->buf.c_str());
            else if (hl) std::snprintf(vb, sizeof(vb), "%s", hl->c_str());
            else         std::snprintf(vb, sizeof(vb), "%s", g.viewV.text(g.yb[dr]).c_str());
            if (ed)       buf.pushRectangle(yHdrX - 1.f, y + (g.ch - lh) * 0.5f - 1.f, g.hdrW, lh + 2.f, jf::Colors::Accent, 2.f);
            else if (sel) buf.pushRectangle(yHdrX - 1.f, y + (g.ch - lh) * 0.5f - 1.f, g.hdrW, lh + 2.f, kBinSel, 2.f);
            text(buf, yHdrX, y + (g.ch - lh) * 0.5f, vb, ed ? jf::Colors::Surface0 : (sel ? kBinSelTx : kBinMuted), g.hdrW - 2.f);
        }
        for (int cc = 0; cc < g.cols; ++cc) {
            const float x = g.gx + cc * g.cw;
            const double v = cell[rr * g.cols + cc];
            uint8_t hc[4];
            if (g.heat) { heatColor((v - mn) / (mx - mn), hc); buf.pushRectangle(x, y, g.cw - 1.f, g.ch - 1.f, hc, 0.f, 1.f, kGrid); }
            else         buf.pushRectangle(x, y, g.cw - 1.f, g.ch - 1.f, jf::Colors::Transparent, 0.f, 1.f, kGrid);
            const bool active = cur && cur->row == rr && cur->col == cc;
            bool selected = false;
            if (cur) {
                const int r0 = std::min(cur->row, cur->aRow), r1 = std::max(cur->row, cur->aRow);
                const int c0 = std::min(cur->col, cur->aCol), c1 = std::max(cur->col, cur->aCol);
                selected = rr >= r0 && rr <= r1 && cc >= c0 && cc <= c1;
                if (selected) {
                    const uint8_t selBg[4] = { jf::Colors::Accent[0], jf::Colors::Accent[1], jf::Colors::Accent[2], 150 };
                    buf.pushRectangle(x, y, g.cw - 1.f, g.ch - 1.f, selBg, 0.f, 1.f, jf::Colors::Accent);
                }
            }
            if (active) buf.pushRectangle(x, y, g.cw - 1.f, g.ch - 1.f, jf::Colors::Transparent, 0.f, 2.f, jf::Colors::Accent);
            if (cells) {
                if (active && cur->editing) std::snprintf(vb, sizeof(vb), "%s", cur->buf.c_str());
                else                     std::snprintf(vb, sizeof(vb), "%.*f", g.dec, v);
                static const uint8_t kBlack[4] = {0,0,0,255}, kWhite[4] = {255,255,255,255};
                const uint8_t* tc = fg;
                if (g.heat) { const int L = (std::max(hc[0], std::max(hc[1], hc[2])) + std::min(hc[0], std::min(hc[1], hc[2]))) / 2; tc = L > 127 ? kBlack : kWhite; }
                if (selected) tc = kWhite;
                // CENTRED, BUT NEVER STARTING OUTSIDE ITS OWN CELL. maxW clips the RUN, not the
                // origin, so a string wider than the cell got a NEGATIVE centring offset and began
                // left of x — the clipped run then painted across the neighbour instead of this cell.
                // Typing into a cell is exactly when the text is over-wide (a half-typed "-99999990"
                // is not a value yet), so the editor smeared over the columns beside it.
                const float tw = textW(vb);
                text(buf, x + std::max(1.f, (g.cw - tw) * 0.5f), y + (g.ch - lh) * 0.5f, vb, tc, g.cw - 2.f);
            }
        }
    }
    buf.popClip();
    if (cur && cur->pendingOp) {
        const char* opn = cur->pendingOp == 1 ? "Set to" : cur->pendingOp == 2 ? "Increase by"
                        : cur->pendingOp == 3 ? "Decrease by" : "% change";
        char hint[80]; std::snprintf(hint, sizeof(hint), "%s: %s", opn, cur->buf.empty() ? "_" : cur->buf.c_str());
        const float hw = textW(hint) + 14.f, hh = lh + 6.f;
        buf.pushRectangle(r.x + 2.f, r.y + 2.f, hw, hh, jf::Colors::Accent, 3.f);
        text(buf, r.x + 9.f, r.y + 5.f, hint, jf::Colors::Surface0, hw);
    }
    if (g.ok && choiceOf(el, "cellTrace") == 2 && m_view == 0) {   // global defaults to "Off", as before
        buf.pushClip(r.x, clipY, clipW, clipH);   // the live-cell marker is grid content — scrolls + clips with it
        if (c.meta()) {                    // no definition, no channel to trace
            auto nearest = [](const std::vector<double>& b, double v) { int best = -1; double bd = 1e300;
                for (int i = 0; i < static_cast<int>(b.size()); ++i) { const double d = std::fabs(b[i] - v); if (d < bd) { bd = d; best = i; } } return best; };
            const int xAxis = g.transpose ? 0 : 1, yAxis = g.transpose ? 1 : 0;
            // Which live channel an axis traces — Cache::axisChannel, the same question the autotuner
            // asks of the same table. It was a lambda here, which made it the studio's second answer.
            auto traceChan = [&](int k) { return c.axisChannel(g.t, k); };
            int tc = -1, tr = -1;
            { const std::string nm = traceChan(xAxis); if (!nm.empty() && c.has(nm)) tc = nearest(g.xb, c.value(nm)); }
            if (g.t.axes.size() >= 2) { const std::string nm = traceChan(yAxis);
                if (!nm.empty() && c.has(nm)) { const int d = nearest(g.yb, c.value(nm)); if (d >= 0) tr = g.rows - 1 - d; } }
            if (tc >= 0 && tr >= 0) {
                const float cx = g.gx + tc * g.cw, cy = g.gy + tr * g.ch;
                static const uint8_t kWhite[4] = { 255, 255, 255, 255 }, kBlack[4] = { 0, 0, 0, 255 };
                buf.pushRectangle(cx, cy, g.cw - 1.f, g.ch - 1.f, kWhite, 0.f);
                if (cells) { char tvb[32]; std::snprintf(tvb, sizeof(tvb), "%.*f", g.dec, cell[tr * g.cols + tc]);
                    text(buf, cx + (g.cw - textW(tvb)) * 0.5f, cy + (g.ch - lh) * 0.5f, tvb, kBlack, g.cw - 2.f); }
            } else if (tc >= 0 || tr >= 0) {
                const float cx = g.gx + (tc >= 0 ? tc : 0) * g.cw, cwd = (tc >= 0 ? g.cw : g.cols * g.cw);
                const float cy = g.gy + (tr >= 0 ? tr : 0) * g.ch, chd = (tr >= 0 ? g.ch : g.rows * g.ch);
                const uint8_t tf[4] = { 255, 255, 255, 45 };
                buf.pushRectangle(cx, cy, cwd, chd, tf, 2.f);
                buf.pushDashedRect(cx, cy, cwd, chd, jf::Colors::Accent, 1.5f);
            }
        }
        buf.popClip();
    }
    // Scroll bars, over the content — same track/thumb styling as the canvas bars in Surface::render.
    if (g.ovV || g.ovH) {
        auto bar = [&](bool on, float tx, float ty, float tw, float th, float travel, float thumbLen, float pos, bool vert) {
            if (!on) return;
            buf.pushRectangle(tx, ty, tw, th, jf::Colors::Surface0);
            const float off = pos * (travel - thumbLen);
            if (vert) buf.pushRectangle(tx + 1.f, ty + off, kTSB - 2.f, thumbLen, jf::Colors::Surface3, (kTSB - 2.f) * 0.5f);
            else      buf.pushRectangle(tx + off, ty + 1.f, thumbLen, kTSB - 2.f, jf::Colors::Surface3, (kTSB - 2.f) * 0.5f);
        };
        if (g.ovV) {
            const float ovf = g.contentH - g.vh, len = std::max(24.f, g.vh * (g.vh / g.contentH));
            bar(true, r.x + r.width - kTSB, g.vy, kTSB, g.vh, g.vh, len, ovf > 0.f ? std::clamp(-g.sy, 0.f, ovf) / ovf : 0.f, true);
        }
        if (g.ovH) {
            const float ovf = g.contentW - g.vw, len = std::max(24.f, g.vw * (g.vw / g.contentW));
            bar(true, g.vx, r.y + r.height - kTSB, g.vw, kTSB, g.vw, len, ovf > 0.f ? std::clamp(-g.sx, 0.f, ovf) / ovf : 0.f, false);
        }
    }
    if (g.zn > 1 && jf::JTextHelper::hasAtlas()) {
        const int depthCtl = choiceOf(el, "depthControl");   // global defaults to "Stepper" (the old else-branch), as before
        if (depthCtl == 2) {
            const float tw = 24.f, th = lh + 6.f, tabsW = g.zn * tw, tx0 = r.x + r.width - tabsW - 4.f, ty = r.y + 4.f;
            for (int z = 0; z < g.zn; ++z) {
                const float tx = tx0 + z * tw; const bool curp = (z == g.z);
                buf.pushRectangle(tx, ty, tw - 2.f, th, curp ? jf::Colors::Accent : jf::Colors::Surface2, 2.f, 1.f, jf::Colors::Border);
                char zn[12]; std::snprintf(zn, sizeof(zn), "%d", z + 1);
                text(buf, tx + (tw - 2.f - textW(zn)) * 0.5f, ty + 3.f, zn, curp ? jf::Colors::Surface0 : jf::Colors::TextPrimary, tw);
            }
        } else if (depthCtl == 1) {
            const float sh = lh + 6.f, sw = 110.f, sx = r.x + r.width - sw - 4.f, sy = r.y + 4.f;
            char zt[24]; std::snprintf(zt, sizeof(zt), "Z %d/%d", g.z + 1, g.zn);
            buf.pushRectangle(sx, sy, sw, sh, jf::Colors::Surface2, 3.f, 1.f, jf::Colors::Border);
            const float trackX = sx + 6.f, trackW = sw - 46.f, trackY = sy + sh * 0.5f - 1.f;
            buf.pushRectangle(trackX, trackY, trackW, 2.f, jf::Colors::Border);
            const float f = g.zn > 1 ? static_cast<float>(g.z) / (g.zn - 1) : 0.f;
            buf.pushRectangle(trackX + f * trackW - 3.f, sy + 3.f, 6.f, sh - 6.f, jf::Colors::Accent, 2.f);
            text(buf, sx + sw - 38.f, sy + 3.f, zt, jf::Colors::TextPrimary, 36.f);
        } else if (depthCtl == 4) {
            const float tw = 30.f, th = lh + 4.f, sx = r.x + r.width - tw - 4.f, sy0 = r.y + 4.f;
            for (int z = 0; z < g.zn; ++z) {
                const float ty = sy0 + z * (th + 2.f); const bool curp = (z == g.z);
                buf.pushRectangle(sx, ty, tw, th, curp ? jf::Colors::Accent : jf::Colors::Surface2, 2.f, 1.f, jf::Colors::Border);
                char zc[12]; std::snprintf(zc, sizeof(zc), "%d", z + 1);
                text(buf, sx + (tw - textW(zc)) * 0.5f, ty + 2.f, zc, curp ? jf::Colors::Surface0 : jf::Colors::TextPrimary, tw);
            }
        } else {
            const float chipH = lh + 6.f;
            char zt[24]; std::snprintf(zt, sizeof(zt), "Z %d/%d", g.z + 1, g.zn);
            const float zw = textW(zt);
            const float chipW = zw + 44.f, chipX = r.x + r.width - chipW - 4.f, chipY = r.y + 4.f;
            buf.pushRectangle(chipX, chipY, chipW, chipH, jf::Colors::Surface2, 3.f, 1.f, jf::Colors::Border);
            text(buf, chipX + 5.f, chipY + 3.f, "<", jf::Colors::Accent, 12.f);
            text(buf, chipX + 20.f, chipY + 3.f, zt, jf::Colors::TextPrimary, zw + 2.f);
            text(buf, chipX + chipW - 12.f, chipY + 3.f, ">", jf::Colors::Accent, 12.f);
        }
    }
}

bool TableWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;
            const TableGeom g = tableGeom(el, r, Cache::instance(), elementContext(), renderZoom());
            // Z-plane depth control (top-right) — tested before cell selection so a click navigates.
            if (g.zn > 1 && in.kind == ControlInput::Kind::Press && jf::JTextHelper::hasAtlas()) {
                const float lh = g.lh > 0.f ? g.lh : jf::JTextHelper::lineHeight();   // the geom font, so the hit-test matches what was drawn
                const int depthCtl = choiceOf(el, "depthControl");   // global defaults to "Stepper" (the old else-branch), as before
                if (depthCtl == 2) {   // tabs
                    const float tw = 24.f, th = lh + 6.f, tabsW = g.zn * tw, tx0 = r.x + r.width - tabsW - 4.f, ty = r.y + 4.f;
                    if (in.my >= ty && in.my < ty + th) {
                        const int z = static_cast<int>((in.mx - tx0) / tw);
                        if (z >= 0 && z < g.zn) { tableSetPlane(el.uid, z); return true; }
                    }
                } else if (depthCtl == 1) {   // slider
                    const float sh = lh + 6.f, sw = 110.f, sx = r.x + r.width - sw - 4.f, sy = r.y + 4.f;
                    const float trackX = sx + 6.f, trackW = sw - 46.f;
                    if (in.my >= sy && in.my < sy + sh && in.mx >= sx && in.mx < sx + sw) {
                        const float f = std::clamp((in.mx - trackX) / trackW, 0.f, 1.f);
                        tableSetPlane(el.uid, static_cast<int>(std::lround(f * (g.zn - 1))));
                        return true;
                    }
                } else if (depthCtl == 4) {   // stack (vertical)
                    const float tw = 30.f, th = lh + 4.f, sx = r.x + r.width - tw - 4.f, sy0 = r.y + 4.f;
                    if (in.mx >= sx && in.mx < sx + tw) {
                        const int z = static_cast<int>((in.my - sy0) / (th + 2.f));
                        if (z >= 0 && z < g.zn) { tableSetPlane(el.uid, z); return true; }
                    }
                } else {   // stepper chip
                    const float chipH = lh + 6.f;
                    char zt[24]; std::snprintf(zt, sizeof(zt), "Z %d/%d", g.z + 1, g.zn);
                    const float chipW = jf::JTextHelper::measureWidthScaled(zt, g.fscale) + 44.f, chipX = r.x + r.width - chipW - 4.f, chipY = r.y + 4.f;
                    if (in.my >= chipY && in.my <= chipY + chipH) {
                        if (in.mx >= chipX && in.mx < chipX + 18.f)                  { tableSetPlane(el.uid, std::max(0, g.z - 1)); return true; }
                        if (in.mx >= chipX + chipW - 18.f && in.mx <= chipX + chipW) { tableSetPlane(el.uid, std::min(g.zn - 1, g.z + 1)); return true; }
                    }
                }
            }
            if (!g.ok) return false;
            const int view = m_view;   // the widget owns its view mode
            if (view == 1) {   // 3D surface: drag orbits, wheel scales height, click picks a cell — keys then edit it
                Table3DState& st = m_view3d;
                if (in.kind == ControlInput::Kind::Press) { st.pressX = in.mx; st.pressY = in.my; st.lastX = in.mx; st.lastY = in.my; st.dragged = false; return true; }
                if (in.kind == ControlInput::Kind::Move) {
                    if (std::abs(in.mx - st.pressX) + std::abs(in.my - st.pressY) > 4.f) st.dragged = true;
                    if (st.dragged) { st.yaw -= (in.mx - st.lastX) * 0.5f; st.pitch = std::clamp(st.pitch + (in.my - st.lastY) * 0.4f, 5.f, 85.f); st.lastX = in.mx; st.lastY = in.my; }
                    return true;
                }
                if (in.kind == ControlInput::Kind::Scroll) { st.hScale = std::clamp(st.hScale * (in.wheel > 0 ? 1.12f : 1.f / 1.12f), 0.2f, 4.f); return true; }
                if (in.kind == ControlInput::Kind::Release) {
                    if (!st.dragged) {   // pick the cell whose TOP (projected at its height, like the render) is nearest the cursor
                        double mn = 1e300, mx = -1e300; std::vector<double> cv(static_cast<size_t>(g.rows) * g.cols);
                        for (int rr = 0; rr < g.rows; ++rr) for (int cc = 0; cc < g.cols; ++cc) {
                            const double v = cellValue(el, g.t, g.cellOffset(rr, cc));
                            cv[rr * g.cols + cc] = v; mn = std::min(mn, v); mx = std::max(mx, v);
                        }
                        if (!(mx > mn)) mx = mn + 1.0;
                        const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f + r.height * 0.16f;
                        const float sxy = std::min(r.width, r.height) * 0.82f / std::max(std::max(g.cols, g.rows), 2), hPix = r.height * 0.38f * st.hScale;
                        const float yaw = st.yaw * 3.14159265f / 180.f, pitch = st.pitch * 3.14159265f / 180.f;
                        const float cyaw = std::cos(yaw), syaw = std::sin(yaw), spitch = std::sin(pitch);
                        double best = 1e18; int br = 0, bc = 0;
                        for (int rr = 0; rr < g.rows; ++rr) for (int cc = 0; cc < g.cols; ++cc) {
                            const float h = static_cast<float>((cv[rr * g.cols + cc] - mn) / (mx - mn));
                            const float X = cc - (g.cols - 1) * 0.5f, Y = rr - (g.rows - 1) * 0.5f;
                            const float sx = cx + (X * cyaw - Y * syaw) * sxy, sy = cy + (X * syaw + Y * cyaw) * sxy * spitch - h * hPix;
                            const double d = (sx - in.mx) * (sx - in.mx) + (sy - in.my) * (sy - in.my);
                            if (d < best) { best = d; br = rr; bc = cc; }
                        }
                        // br is a SCREEN row (cell[]/render are screen-indexed) — set the cursor directly, no flip.
                        TableCursor& tc = tableCursors()[el.uid]; tc.row = std::clamp(br, 0, g.rows - 1); tc.col = std::clamp(bc, 0, g.cols - 1); tc.aRow = tc.row; tc.aCol = tc.col;
                    }
                    return true;
                }
                // Key events fall through to the shared 2D cell-editing handler below — the 3D surface is editable.
            }
            TableCursor& cur = tableCursors()[el.uid];
            cur.row = std::clamp(cur.row, 0, g.rows - 1);   cur.col  = std::clamp(cur.col, 0, g.cols - 1);
            cur.aRow = std::clamp(cur.aRow, 0, g.rows - 1); cur.aCol = std::clamp(cur.aCol, 0, g.cols - 1);
            auto commit = [&] {
                if (!cur.editing) return;
                cur.editing = false;
                if (cur.axisEdit) {                              // committing an axis breakpoint edit (X or Y strip)
                    double val = 0; bool okv = true;
                    try { val = std::stod(cur.buf); } catch (...) { okv = false; }
                    if (okv) {
                        const int axis = (cur.axisEdit == 1) ? (g.transpose ? 0 : 1) : (g.transpose ? 1 : 0);
                        std::vector<double> vals = Cache::instance().tiBins(g.t, axis);
                        if (cur.axisIdx >= 0 && cur.axisIdx < static_cast<int>(vals.size())) {
                            // Typed in whatever unit the strip is SHOWING, so convert before storing —
                            // the bytes are always the axis's own quantity. Then the axis's channel
                            // bounds it and states its precision, the same rule the Axis Setup dialog
                            // applies, because this is the same bin typed somewhere else. Monotonicity
                            // is still the tuner's call, as it always was.
                            const Cache::AxisView& av = (cur.axisEdit == 1) ? g.viewH : g.viewV;
                            vals[cur.axisIdx] = Cache::instance().snapBin(g.t, axis, av.toStorage(val));
                            Cache::instance().beginEdit();
                            Cache::instance().tiWriteBins(g.t, axis, vals);
                            Cache::instance().endEdit("Edit axis breakpoints");
                        }
                    }
                    cur.axisEdit = 0; cur.buf.clear();
                    return;
                }
                double operand = 0; bool ok = true;
                try { operand = std::stod(cur.buf); } catch (...) { ok = false; }
                if (ok) {
                    // Apply over the WHOLE selection block (a single cell if nothing is ranged). A plain entry
                    // (pendingOp 0) sets every selected cell to the value; a menu transform maps each cell. One
                    // undo step (beginEdit/endEdit is nestable, so the inner writeAt calls coalesce).
                    const int r0 = std::min(cur.row, cur.aRow), r1 = std::max(cur.row, cur.aRow);
                    const int c0 = std::min(cur.col, cur.aCol), c1 = std::max(cur.col, cur.aCol);
                    Cache::instance().beginEdit();
                    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) {
                        const int off = g.cellOffset(rr, cc);
                        double nv = operand;
                        if (cur.pendingOp) {                     // a menu transform (Set/Increase/Decrease/%) over the block
                            const double old = cellValue(el, g.t, off);
                            switch (cur.pendingOp) {
                                case 1: nv = operand; break;                          // Set to
                                case 2: nv = old + operand; break;                    // Increase by
                                case 3: nv = old - operand; break;                    // Decrease by
                                case 4: nv = old * (1.0 + operand / 100.0); break;    // Percentage change
                            }
                        }
                        setCellValue(el, g.t, off, nv);
                    }
                    Cache::instance().endEdit(cur.pendingOp ? "Table transform" : "Set cells");
                }
                cur.pendingOp = 0; cur.buf.clear();
            };
            auto cellAt = [&](float mx, float my, int& oc, int& orr) -> bool {
                // Reject outside the scroll viewport first: once the grid origin can sit left of / above the
                // visible area, a press on a header strip or a bar gutter would otherwise land on a cell that
                // is scrolled out of sight.
                if (mx < g.vx || my < g.vy || mx > g.vx + g.vw || my > g.vy + g.vh) return false;
                if (mx < g.gx || my < g.gy) return false;
                const int cc = static_cast<int>((mx - g.gx) / g.cw), rr = static_cast<int>((my - g.gy) / g.ch);
                if (cc < 0 || cc >= g.cols || rr < 0 || rr >= g.rows) return false;
                oc = cc; orr = rr; return true;
            };
            if (view == 2) {   // Slice: click a column selects it; drag near the bar top edits the value; drag the body extends
                const float lh = g.lh > 0.f ? g.lh : jf::JTextHelper::lineHeight();   // geom font (matches the slice render)
                const float plotX = r.x + 46.f, plotY = r.y + lh + 4.f, plotW = r.width - 54.f, plotH = r.height - lh * 2.f - 10.f;
                const float bcw = (g.cols > 0) ? plotW / g.cols : plotW;
                auto colAt = [&](float mx) { return std::clamp(static_cast<int>((mx - plotX) / bcw), 0, g.cols - 1); };
                // Value axis = the WHOLE table's min/max — the SAME range the slice render scales its bars to
                // (not the current row). Using the row range here put the bar-top hit-test at the wrong Y, so a
                // click on the visible bar top missed the 12px window and fell through to select instead of edit.
                auto plotMinMax = [&](double& mn, double& mx) {
                    mn = 1e300; mx = -1e300;
                    for (int rr = 0; rr < g.rows; ++rr) for (int cc = 0; cc < g.cols; ++cc) {
                        const double v = cellValue(el, g.t, g.cellOffset(rr, cc));
                        mn = std::min(mn, v); mx = std::max(mx, v);
                    }
                    if (!(mx > mn)) mx = mn + 1.0;
                };
                if (in.kind == ControlInput::Kind::Press && plotW >= 4.f && plotH >= 4.f) {
                    const int c = colAt(in.mx); cur.col = c; cur.aRow = cur.row; cur.aCol = c;
                    double mn, mx; plotMinMax(mn, mx);
                    const float barTopY = plotY + plotH - static_cast<float>((cellValue(el, g.t, g.cellOffset(cur.row, c)) - mn) / (mx - mn)) * plotH;
                    cur.sliceDrag = (std::abs(in.my - barTopY) < 12.f) ? c : -1;   // near the top = drag the value; else column-range select
                    return true;
                }
                if (in.kind == ControlInput::Kind::Move && plotH >= 4.f) {
                    if (cur.sliceDrag >= 0) { double mn, mx; plotMinMax(mn, mx);
                        setCellValue(el, g.t, g.cellOffset(cur.row, cur.sliceDrag),
                                     mn + (mx - mn) * std::clamp((plotY + plotH - in.my) / plotH, 0.f, 1.f)); }
                    else cur.col = colAt(in.mx);   // extend the column range (anchor fixed)
                    return true;
                }
                if (in.kind == ControlInput::Kind::Release) { cur.sliceDrag = -1; return true; }
            }
            // Grid scrolling (only reachable when the cells don't stretch — see tableGeom). Handled BEFORE the
            // cell hit-tests so a press on a bar pans instead of starting a rubber-band selection behind it.
            if (view == 0 && (g.ovV || g.ovH)) {
                TableScroll& sc = tableScrolls()[el.uid];
                const float vTx = r.x + r.width - kTSB, hTy = r.y + r.height - kTSB;
                const float vLen = std::max(24.f, g.vh * (g.vh / g.contentH));
                const float hLen = std::max(24.f, g.vw * (g.vw / g.contentW));
                if (in.kind == ControlInput::Kind::Scroll) {   // wheel pans the overflowing axis, vertical first
                    if (g.ovV) sc.y = std::clamp(sc.y + in.wheel * g.ch * 3.f, std::min(0.f, g.vh - g.contentH), 0.f);
                    else       sc.x = std::clamp(sc.x + in.wheel * g.cw * 3.f, std::min(0.f, g.vw - g.contentW), 0.f);
                    return true;
                }
                if (in.kind == ControlInput::Kind::Press) {
                    if (g.ovV && in.mx >= vTx && in.my >= g.vy && in.my <= g.vy + g.vh) {
                        const float ovf = g.contentH - g.vh, pos = ovf > 0.f ? std::clamp(-sc.y, 0.f, ovf) / ovf : 0.f;
                        sc.drag = 1; sc.grab = in.my - (g.vy + pos * (g.vh - vLen)); return true;
                    }
                    if (g.ovH && in.my >= hTy && in.mx >= g.vx && in.mx <= g.vx + g.vw) {
                        const float ovf = g.contentW - g.vw, pos = ovf > 0.f ? std::clamp(-sc.x, 0.f, ovf) / ovf : 0.f;
                        sc.drag = 2; sc.grab = in.mx - (g.vx + pos * (g.vw - hLen)); return true;
                    }
                }
                if (in.kind == ControlInput::Kind::Move && sc.drag) {   // keep the grab offset so the thumb doesn't jump
                    if (sc.drag == 1) { const float travel = g.vh - vLen, ovf = g.contentH - g.vh;
                        sc.y = travel > 0.f ? -std::clamp((in.my - sc.grab - g.vy) / travel, 0.f, 1.f) * ovf : 0.f; }
                    else              { const float travel = g.vw - hLen, ovf = g.contentW - g.vw;
                        sc.x = travel > 0.f ? -std::clamp((in.mx - sc.grab - g.vx) / travel, 0.f, 1.f) * ovf : 0.f; }
                    return true;
                }
                if (in.kind == ControlInput::Kind::Release && sc.drag) { sc.drag = 0; return true; }
            }
            // Which strip the pointer is over, in STORAGE axis terms — remembered so a context-menu
            // action can act on what is under the cursor. Same anchors as the press hit-test below; they
            // are computed here once and reused, so the hover zone and the clickable strip cannot drift.
            const float lh0 = g.lh > 0.f ? g.lh : jf::JTextHelper::lineHeight();
            const float xHdrY0 = g.xBottom ? (g.gy + g.rows * g.ch + 2.f) : (r.y + 3.f + g.nameH + g.sy);
            const float yHdrX0 = g.yRight ? (g.gx + g.cols * g.cw + 3.f) : (r.x + 3.f + g.sx);
            const bool overX = g.hdrH > 0.f && in.my >= xHdrY0 - 2.f && in.my <= xHdrY0 + lh0 + 2.f
                            && in.mx >= g.gx && in.mx < g.gx + g.cols * g.cw;
            const bool overY = g.hdrW > 0.f && in.mx >= yHdrX0 - 2.f && in.mx <= yHdrX0 + g.hdrW + 2.f
                            && in.my >= g.gy && in.my < g.gy + g.rows * g.ch;
            if (in.kind == ControlInput::Kind::Move || in.kind == ControlInput::Kind::Press)
                cur.hoverAxis = overX ? (g.transpose ? 0 : 1)      // screen horizontal -> storage axis
                              : overY ? (g.transpose ? 1 : 0)      // screen vertical   -> storage axis
                                      : -1;                        // the cells
            if (in.kind == ControlInput::Kind::Press) {
                // Axis header hit-test: click a breakpoint label to edit it inline (writes via tiWriteBins on commit).
                // These MUST match the render's anchors exactly (including the axis-name bar and the scroll
                // offset) or the clickable strip drifts off the drawn one.
                const float lh = lh0;
                const float xHdrY = xHdrY0;
                const float yHdrX = yHdrX0;
                if (g.hdrH > 0.f && in.my >= xHdrY - 2.f && in.my <= xHdrY + lh + 2.f && in.mx >= g.gx && in.mx < g.gx + g.cols * g.cw) {
                    commit(); cur.axisEdit = 1; cur.axisIdx = std::clamp(static_cast<int>((in.mx - g.gx) / g.cw), 0, g.cols - 1);
                    cur.editing = true; cur.buf.clear(); return true;
                }
                if (g.hdrW > 0.f && in.mx >= yHdrX - 2.f && in.mx <= yHdrX + g.hdrW + 2.f && in.my >= g.gy && in.my < g.gy + g.rows * g.ch) {
                    const int rr = std::clamp(static_cast<int>((in.my - g.gy) / g.ch), 0, g.rows - 1);
                    commit(); cur.axisEdit = 2; cur.axisIdx = g.rows - 1 - rr;   // screen row -> storage bin (Y increases upward)
                    cur.editing = true; cur.buf.clear(); return true;
                }
                int cc, rr;
                if (cellAt(in.mx, in.my, cc, rr)) {              // start a fresh rubber-band selection at the cell
                    commit();
                    cur.row = rr; cur.col = cc; cur.aRow = rr; cur.aCol = cc; cur.selecting = true;
                }
                return true;   // consume the press (takes keyboard focus for the grid)
            }
            if (in.kind == ControlInput::Kind::Move) {          // drag extends the block from the anchor (hover ignored)
                if (cur.selecting) { int cc, rr; if (cellAt(in.mx, in.my, cc, rr)) { cur.row = rr; cur.col = cc; } }
                return cur.selecting;
            }
            if (in.kind == ControlInput::Kind::Release) { cur.selecting = false; return true; }
            if (in.kind == ControlInput::Kind::Key && in.key) {
                using K = jf::JKeyEvent::JKey;
                // PageUp/PageDown step the Z plane on a 3D table.
                if (g.zn > 1 && (in.key->key == K::PageUp || in.key->key == K::PageDown)) {
                    cur.z = std::clamp(cur.z + (in.key->key == K::PageUp ? 1 : -1), 0, g.zn - 1);
                    return true;
                }
                // Selection semantics per action: plain arrow = translate the whole block; Extend = grow the
                // pressed edge; Reduce = shrink the far edge. The action for each keystroke comes from the
                // user-editable Keymap — see the resolve below.
                auto collapse = [&] { cur.aRow = cur.row; cur.aCol = cur.col; };
                // Plain arrow: slide the entire selection block by (dR,dC), preserving its size and stopping at
                // the grid edge (both cursor + anchor move together — a nudge of the selection, not a collapse).
                // KEEP THE CURSOR IN VIEW. A grid that overflows scrolls, and the keyboard could walk the
                // cursor straight out of the visible area — you were then editing a cell you could not
                // see, with the arrow keys apparently doing nothing. Nudge the scroll by however much
                // the cell sticks out, so it follows the cursor rather than jumping to centre it.
                //
                // The offsets are what the NEXT frame's geometry reads (sc.x/sc.y are <= 0 and clamped
                // there), which is also why this cannot use the g it was computed from.
                auto reveal = [&](int row, int col) {
                    if (!g.ovH && !g.ovV) return;                 // nothing scrolls: nothing to reveal
                    TableScroll& sc = tableScrolls()[el.uid];
                    const float cx = g.gx + col * g.cw, cy = g.gy + row * g.ch;
                    if (g.ovH) {
                        if (cx < g.vx)                     sc.x += g.vx - cx;
                        else if (cx + g.cw > g.vx + g.vw)  sc.x -= (cx + g.cw) - (g.vx + g.vw);
                    }
                    if (g.ovV) {
                        if (cy < g.vy)                     sc.y += g.vy - cy;
                        else if (cy + g.ch > g.vy + g.vh)  sc.y -= (cy + g.ch) - (g.vy + g.vh);
                    }
                };
                auto translate = [&](int dR, int dC) {
                    const int rLo = std::min(cur.row, cur.aRow), rHi = std::max(cur.row, cur.aRow);
                    const int cLo = std::min(cur.col, cur.aCol), cHi = std::max(cur.col, cur.aCol);
                    if (dR < 0) dR = std::max(dR, -rLo); else if (dR > 0) dR = std::min(dR, (g.rows - 1) - rHi);
                    if (dC < 0) dC = std::max(dC, -cLo); else if (dC > 0) dC = std::min(dC, (g.cols - 1) - cHi);
                    cur.row += dR; cur.aRow += dR; cur.col += dC; cur.aCol += dC;
                    reveal(cur.row, cur.col);
                };
                // Shift+arrow: push the edge in the pressed direction OUTWARD — the selection only ever grows;
                // the opposite edge never moves. (dR/dC: -1 = top/left edge, +1 = bottom/right edge.)
                auto grow = [&](int dR, int dC) {
                    int rLo = std::min(cur.row, cur.aRow), rHi = std::max(cur.row, cur.aRow);
                    int cLo = std::min(cur.col, cur.aCol), cHi = std::max(cur.col, cur.aCol);
                    if (dR < 0) rLo = std::max(0, rLo - 1); else if (dR > 0) rHi = std::min(g.rows - 1, rHi + 1);
                    if (dC < 0) cLo = std::max(0, cLo - 1); else if (dC > 0) cHi = std::min(g.cols - 1, cHi + 1);
                    cur.aRow = rLo; cur.row = rHi; cur.aCol = cLo; cur.col = cHi;
                    reveal(dR < 0 ? rLo : rHi, dC < 0 ? cLo : cHi);   // the edge that just moved
                };
                // Ctrl+arrow: pull the FAR edge inward, in the direction of the arrow — the selection only ever
                // shrinks (clamped so it never collapses past a single cell); the near/pressed-side edge stays.
                // Mirror of grow: Ctrl+Left retreats the right edge left, Ctrl+Up retreats the bottom edge up, etc.
                auto shrink = [&](int dR, int dC) {
                    int rLo = std::min(cur.row, cur.aRow), rHi = std::max(cur.row, cur.aRow);
                    int cLo = std::min(cur.col, cur.aCol), cHi = std::max(cur.col, cur.aCol);
                    if (dR < 0) rHi = std::max(rLo, rHi - 1); else if (dR > 0) rLo = std::min(rHi, rLo + 1);
                    if (dC < 0) cHi = std::max(cLo, cHi - 1); else if (dC > 0) cLo = std::min(cHi, cLo + 1);
                    cur.aRow = rLo; cur.row = rHi; cur.aCol = cLo; cur.col = cHi;
                    reveal(dR < 0 ? rHi : rLo, dC < 0 ? cHi : cLo);   // the edge that just retreated
                };
                // Apply fn to every cell in the selection block, as one undo step (the keymap value ops).
                auto applyBlock = [&](const std::function<double(double)>& fn) {
                    const int r0 = std::min(cur.row, cur.aRow), r1 = std::max(cur.row, cur.aRow);
                    const int c0 = std::min(cur.col, cur.aCol), c1 = std::max(cur.col, cur.aCol);
                    Cache::instance().beginEdit();
                    for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) {
                        const int off = g.cellOffset(rr, cc);
                        setCellValue(el, g.t, off, fn(cellValue(el, g.t, off)));
                    }
                    Cache::instance().endEdit("Table value");
                };
                // Navigation / edit-buffer keys are not part of the editable keymap (their handling is
                // fixed), so resolve them directly.
                switch (in.key->key) {
                    case K::Return: commit(); cur.row = std::min(g.rows - 1, cur.row + 1); collapse(); return true;
                    case K::Tab:    commit(); cur.col = (cur.col + 1 < g.cols) ? cur.col + 1 : 0; collapse(); return true;
                    case K::Escape: cur.editing = false; cur.buf.clear(); cur.pendingOp = 0; cur.axisEdit = 0; return true;
                    case K::Backspace: if (cur.editing && !cur.buf.empty()) cur.buf.pop_back(); return true;
                    default: break;
                }
                // Resolve the keystroke through the user-editable keymap. Selection moves apply even mid-entry
                // (they commit first); value nudges only when not typing a number into a cell, so '.'/',' can
                // still serve as a decimal point / be ignored while editing.
                const double step = [&] { const std::string s = el.prop("step"); if (s.empty()) return 1.0; try { return std::stod(s); } catch (...) { return 1.0; } }();
                using A = Keymap::Action;
                const A act = Keymap::instance().action(*in.key);
                const bool valueOp = act == A::IncreaseValue || act == A::DecreaseValue
                                  || act == A::IncreaseValueLarge || act == A::DecreaseValueLarge;
                // Axis breakpoint nudge: while an axis cell is selected and no digits have been typed, value
                // keys step the breakpoint. The generic value op below targets the CELL block, so axis cells
                // need their own path (mirrors the axis-commit write). A typed digit (buf non-empty) falls
                // through so '.' can be a decimal point.
                if (cur.axisEdit && valueOp && cur.buf.empty()) {
                    const double mult = (act == A::IncreaseValue) ? 1.0 : (act == A::DecreaseValue) ? -1.0
                                      : (act == A::IncreaseValueLarge) ? 10.0 : -10.0;
                    const int axis = (cur.axisEdit == 1) ? (g.transpose ? 0 : 1) : (g.transpose ? 1 : 0);
                    std::vector<double> vals = Cache::instance().tiBins(g.t, axis);
                    if (cur.axisIdx >= 0 && cur.axisIdx < static_cast<int>(vals.size())) {
                        // A nudge steps by what is VISIBLE: one step of the displayed unit, converted.
                        const Cache::AxisView& av = (cur.axisEdit == 1) ? g.viewH : g.viewV;
                        const double stepped = av.toStorage(av.toDisplay(vals[cur.axisIdx]) + mult * step);
                        vals[cur.axisIdx] = Cache::instance().snapBin(g.t, axis, stepped);
                        Cache::instance().beginEdit();
                        Cache::instance().tiWriteBins(g.t, axis, vals);
                        Cache::instance().endEdit("Edit axis breakpoints");
                    }
                    return true;
                }
                if (act != A::None && !(valueOp && cur.editing)) {
                    switch (act) {
                        case A::CursorLeft:  commit(); translate(0, -1); return true;
                        case A::CursorRight: commit(); translate(0, +1); return true;
                        case A::CursorUp:    commit(); translate(-1, 0); return true;
                        case A::CursorDown:  commit(); translate(+1, 0); return true;
                        case A::ExtendLeft:  commit(); grow(0, -1);  return true;
                        case A::ExtendRight: commit(); grow(0, +1);  return true;
                        case A::ExtendUp:    commit(); grow(-1, 0);  return true;
                        case A::ExtendDown:  commit(); grow(+1, 0);  return true;
                        case A::ShrinkLeft:  commit(); shrink(0, -1); return true;
                        case A::ShrinkRight: commit(); shrink(0, +1); return true;
                        case A::ShrinkUp:    commit(); shrink(-1, 0); return true;
                        case A::ShrinkDown:  commit(); shrink(+1, 0); return true;
                        case A::IncreaseValue:      applyBlock([step](double o) { return o + step; });        return true;
                        case A::DecreaseValue:      applyBlock([step](double o) { return o - step; });        return true;
                        case A::IncreaseValueLarge: applyBlock([step](double o) { return o + step * 10.0; }); return true;
                        case A::DecreaseValueLarge: applyBlock([step](double o) { return o - step * 10.0; }); return true;
                        case A::None: break;
                    }
                }
                const char ch = in.key->utf8[0];
                if (!cur.editing) {                               // non-rebindable ops: smooth / linearise
                    const int r0 = std::min(cur.row, cur.aRow), r1 = std::max(cur.row, cur.aRow);
                    const int c0 = std::min(cur.col, cur.aCol), c1 = std::max(cur.col, cur.aCol);
                    auto rd = [&](int rr, int cc) { return cellValue(el, g.t, g.cellOffset(rr, cc)); };
                    auto wr = [&](int rr, int cc, double v) { setCellValue(el, g.t, g.cellOffset(rr, cc), v); };
                    if (ch == 's' || ch == 'S') {   // Smooth: 3x3 box average, reading full-table neighbours (border cells use their real outside neighbours)
                        std::vector<double> orig(static_cast<size_t>(g.rows) * g.cols);
                        for (int rr = 0; rr < g.rows; ++rr) for (int cc = 0; cc < g.cols; ++cc)
                            orig[static_cast<size_t>(rr) * g.cols + cc] = rd(rr, cc);
                        Cache::instance().beginEdit();
                        for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) {
                            double sum = 0; int n = 0;
                            for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc) {
                                const int nr = rr + dr, nc = cc + dc;
                                if (nr >= 0 && nr < g.rows && nc >= 0 && nc < g.cols) { sum += orig[static_cast<size_t>(nr) * g.cols + nc]; ++n; }
                            }
                            wr(rr, cc, n ? sum / n : orig[static_cast<size_t>(rr) * g.cols + cc]);
                        }
                        Cache::instance().endEdit("Smooth");
                        return true;
                    }
                    if (ch == '/') {                // Linearise: ramp between the block's edge values (bilinear for a 2D block, else along the long axis)
                        const int R = r1 - r0 + 1, C = c1 - c0 + 1;
                        Cache::instance().beginEdit();
                        if (R > 1 && C > 1) {
                            const double tl = rd(r0, c0), tr = rd(r0, c1), bl = rd(r1, c0), br = rd(r1, c1);
                            for (int rr = r0; rr <= r1; ++rr) for (int cc = c0; cc <= c1; ++cc) {
                                const double fr = double(rr - r0) / (R - 1), fc = double(cc - c0) / (C - 1);
                                const double top = tl + (tr - tl) * fc, bot = bl + (br - bl) * fc;
                                wr(rr, cc, top + (bot - top) * fr);
                            }
                        } else if (C > 1) {
                            for (int rr = r0; rr <= r1; ++rr) { const double a = rd(rr, c0), b = rd(rr, c1);
                                for (int cc = c0; cc <= c1; ++cc) wr(rr, cc, a + (b - a) * double(cc - c0) / (C - 1)); }
                        } else if (R > 1) {
                            for (int cc = c0; cc <= c1; ++cc) { const double a = rd(r0, cc), b = rd(r1, cc);
                                for (int rr = r0; rr <= r1; ++rr) wr(rr, cc, a + (b - a) * double(rr - r0) / (R - 1)); }
                        }
                        Cache::instance().endEdit("Linearise");
                        return true;
                    }
                }
                if ((ch >= '0' && ch <= '9') || ch == '-' || (ch == '.' && cur.editing)) { if (!cur.editing) { cur.editing = true; cur.buf.clear(); } cur.buf += ch; return true; }
                return false;
            }
            return false;
}

double TableWidget::sigilValue(const std::string& prop, const PanelElement& el, const Cache& c) const {
    if (prop == "rowcount")    { const std::string b = bindPath(); return b.empty() ? 0.0 : static_cast<double>(c.liveRows(b)); }
    // selectedRow / selectedCol are the active cell's STORAGE indices — exactly the (r, c) a curve uses
    // in tiCell(t, c, r, z). A curve holds axis-1 fixed as its "row" and varies axis-0 as its X, so
    // [@table.selectedRow] must be that axis-1 index, and [@table.selectedCol] the axis-0 index. Then
    // clicking a cell drives a bound curve to the exact line through it, whatever the table's transpose.
    // The cursor stores SCREEN coords (row 0 = top); the grid draws highest-on-top, so the vertical
    // screen position flips to storage (rows-1-screenRow) and the horizontal maps straight through —
    // mapped onto the storage axes by transpose, matching TableGeom::cellOffset. No selection -> 0.
    if (prop == "selectedRow" || prop == "selectedCol") {
        const TableCellSelection sel = tableCellSelection(el.uid);
        const std::string bind = bindPath();
        if (!sel.has || bind.empty() || !c.isTable(bind))
            return static_cast<double>(prop == "selectedRow" ? sel.activeRow : sel.activeCol);
        const bool tr = tableTransposed(el);
        const TableDims d = tableDims(c, c.resolveTable(bind), tr);
        const int vIdx = std::max(0, d.rows - 1 - sel.activeRow);   // vertical screen -> storage (flipped)
        const int hIdx = sel.activeCol;                             // horizontal maps straight through
        const int i0 = tr ? hIdx : vIdx;   // storage axis 0 (curve's X / column)
        const int i1 = tr ? vIdx : hIdx;   // storage axis 1 (curve's row)
        return static_cast<double>(prop == "selectedRow" ? i1 : i0);
    }
    // Curve RANGE, for driving a gauge's Scale Min/Max off the calibration it displays. `min`/`max` span the
    // cell VALUES (a sensor cal's engineering output — what the eng-value gauge should read full-scale); axis
    // `axismin`/`axismax` span the axis-0 BREAKPOINTS (the raw-ADC input the cal is fed — the raw gauge's
    // range). Computed live from the bound table each frame, so both track as the user adds columns/bins.
    if (prop == "min" || prop == "max" || prop == "axismin" || prop == "axismax") {
        const std::string bind = bindPath();
        if (bind.empty() || !c.isTable(bind)) return std::nan("");
        const TableImage t = c.resolveTable(bind);
        if (!t.valid) return std::nan("");
        if (prop == "axismin" || prop == "axismax") {
            const std::vector<double> bins = c.tiBins(t, 0);   // axis-0 breakpoints = the raw input axis
            if (bins.empty()) return std::nan("");
            double lo = bins[0], hi = bins[0];
            for (double v : bins) { lo = std::min(lo, v); hi = std::max(hi, v); }
            return prop == "axismin" ? lo : hi;
        }
        const int nx = c.tiLiveN(t, 0), ny = c.tiLiveN(t, 1), nz = c.tiLiveN(t, 2);
        bool any = false; double lo = 0.0, hi = 0.0;
        for (int z = 0; z < nz; ++z) for (int r = 0; r < ny; ++r) for (int cc = 0; cc < nx; ++cc) {
            const double v = c.tiCell(t, cc, r, z);
            if (!any) { lo = hi = v; any = true; } else { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
        if (!any) return std::nan("");
        return prop == "min" ? lo : hi;
    }
    return std::nan("");
}

std::vector<std::string> TableWidget::sigilNames() const {
    return { "rowcount", "selectedRow", "selectedCol", "min", "max", "axismin", "axismax" };
}

// ---- Shared table state + geometry (the table state + geometry + cell ops (verbatim)) ---------------
// How many decimals an AXIS shows. An explicit choice wins; otherwise the axis follows the CHANNEL it
// reads, because that is what the numbers on it are — a pedal axis read to a tenth should say 30.4, and
// an rpm axis whole numbers, without anyone configuring either. That makes it right the moment a table is
// dragged out of the dictionary, and right again the moment the channel is changed in Axis Setup, with no
// stored property to go stale. Only when no channel can answer does it fall back to the cells' own
// decimals, which is what every axis did before.
// THE ELEMENT'S OWN choice for a decimals prop (1..7 == 0..6 places), or 0 for "this widget does not
// say". Deliberately not choiceOf(): that resolves "does not say" through the GLOBAL preference, whose
// default is one decimal place — so asking it can never distinguish "the user wants 1" from "nobody has
// said anything", and the data's own precision could never get a word in. A table dropped from the
// dictionary has no prop at all, which is exactly the case that matters.
int TableWidget::ownDecimals(const PanelElement& el, const char* key) {
    const int v = std::atoi(el.prop(key).c_str());
    return v > 0 ? v : 0;
}

// "fuel_load (kPa)" — an axis's name with the unit it is currently READ in, which is not always the unit
// it is stored in: a raw input shown in volts says V, and a channel displayed in the user's preferred
// unit says that. "—" for an axis that has no name.
std::string TableWidget::_named(const std::string& name, const Cache::AxisView& view) {
    const std::string n = name.empty() ? "\xE2\x80\x94" : name;
    const std::string u = view.label();
    return u.empty() ? n : n + " (" + u + ")";
}

// What the CELLS read in — the unit a value typed into the grid is in. For a sensor calibration that is
// the sensor's own type, converted to whatever the user prefers to see that quantity in.
std::string TableWidget::_cellUnits(const PanelElement& el, const TableGeom& g) {
    const std::string bind = MathEvaluator::resolveTemplate(el.prop("signalName"),
                                                            MathEvaluator::elementContext());
    const std::string src = g.t.cellUnits.empty() ? Cache::instance().unit(bind) : g.t.cellUnits;
    if (src.empty()) return {};
    const std::string dst = displayUnitOf(el);
    const std::string shown = (dst.empty() || dst == "Auto" || dst == "Raw") ? src : dst;
    const std::string lbl = UnitManager::instance().unitLabel(shown);
    return lbl.empty() ? shown : lbl;
}

const char* TableWidget::axisUnitProp(int storageAxis) {
    static const char* kProps[3] = { "displayUnitX", "displayUnitY", "displayUnitZ" };
    return (storageAxis >= 0 && storageAxis < 3) ? kProps[storageAxis] : "";
}

Cache::AxisView TableWidget::axisView(const PanelElement& el, const Cache& c, const TableImage& t,
                                      int storageAxis) {
    const char* prop = axisUnitProp(storageAxis);
    Cache::AxisView v = c.axisView(t, storageAxis, *prop ? el.prop(prop) : std::string());
    // An explicit per-axis decimals choice still outranks the precision the unit implies — the same
    // "what the widget says, first" rule the rest of the precision chain follows.
    static const char* kDec[3] = { "decimalsX", "decimalsY", "decimalsZ" };
    if (storageAxis >= 0 && storageAxis < 3)
        if (const int own = ownDecimals(el, kDec[storageAxis]); own > 0) v.digits = own - 1;
    return v;
}

int TableWidget::axisDecimals(const PanelElement& el, const Cache& c, const TableImage& t,
                              int storageAxis, int cellDec) {
    static const char* kAxisDec[3] = { "decimalsX", "decimalsY", "decimalsZ" };
    if (storageAxis < 0 || storageAxis >= 3) return cellDec;
    if (const int own = ownDecimals(el, kAxisDec[storageAxis]); own > 0) return own - 1;
    const Cache::ChannelDomain d = c.axisDomain(t, storageAxis);   // the channel, or the axis's own facts
    if (d.digits >= 0) return d.digits;
    if (const int global = choiceOf(el, kAxisDec[storageAxis]); global > 0) return global - 1;
    return cellDec;
}

// THE WIDTH THE GRID IS WHOLE AT: the bin strip plus one natural cell per column. Below this a table
// scrolls — Stretch never shrinks a cell past the number it has to show — so it is exactly the width at
// which a page carrying a table stops needing a horizontal scroll bar. Measured through the SAME geometry
// the paint uses, at the rect the table was authored with, so the font, the cell scale and the canvas zoom
// are the ones that will actually be drawn.
//
// FULLY EXPANDED, not clipped to the box it was drawn in. A grid authored at 1260 px that only needs 850
// wants 850; one that needs 1500 wants 1500, and a window opened at that size (or as much of it as the
// area allows) is one the reader can GROW into the whole map rather than one that starts by scrolling.
float TableWidget::naturalWidth(float authoredW) const {
    const PanelElement* el = element();
    if (!el || authoredW <= 0.f) return authoredW;
    const jf::JRect probe{ 0.f, 0.f, authoredW, std::max(40.f, float(el->h)) };
    const TableGeom g = tableGeom(*el, probe, Cache::instance(), elementContext(), renderZoom());
    if (!g.ok || g.cols <= 0 || g.natW <= 0.f) return authoredW;
    return g.hdrW + g.cols * g.natW + 10.f;
}

float TableWidget::naturalHeight(float authoredH) const {
    const PanelElement* el = element();
    if (!el || authoredH <= 0.f) return authoredH;
    const jf::JRect probe{ 0.f, 0.f, std::max(40.f, float(el->w)), authoredH };
    const TableGeom g = tableGeom(*el, probe, Cache::instance(), elementContext(), renderZoom());
    if (!g.ok || g.rows <= 0 || g.natH <= 0.f) return authoredH;
    return g.nameH + g.hdrH + g.rows * g.natH + 10.f;
}

TableWidget::TableGeom TableWidget::tableGeom(const PanelElement& el, const jf::JRect& r, const Cache& c,
                                             const std::string& elemCtx, float zoom) {
    TableGeom g;
    const std::string bind = resolveTemplate(el.prop("signalName"), elemCtx);
    if (!c.meta() || bind.empty() || !c.isTable(bind)) return g;
    g.t = c.resolveTable(bind);
    if (!g.t.valid || g.t.axes.empty() || g.t.cellSize <= 0) return g;
    // ONE scale per data axis, applied once at the single resolve point (every downstream cell/axis read+write
    // goes through g.t.cellScale / axes[].breakScale — display uses it, edit inverts it). The widget property IS
    // the scale, not a multiplier: it defaults to the schema scale (already in g.t from resolveTable) and, once
    // set, replaces it outright. Cell = "scale"; each axis = scaleX/Y/Z. Blank ⇒ keep the schema scale.
    auto scaleOne = [&el](const char* key, double schema) {
        const std::string s = el.prop(key);
        if (s.empty()) return schema;
        try { const double n = std::stod(s); return n == 0.0 ? schema : n; } catch (...) { return schema; }
    };
    g.t.cellScale = scaleOne("scale", g.t.cellScale);
    static const char* kAxisScale[3] = { "scaleX", "scaleY", "scaleZ" };
    for (size_t a = 0; a < g.t.axes.size() && a < 3; ++a) g.t.axes[a].breakScale = scaleOne(kAxisScale[a], g.t.axes[a].breakScale);
    const bool has2 = g.t.axes.size() >= 2;
    // Axis orientation combo 0..7 (AxisLayout::COMBOS). Default 4 = "left & top": no transpose, Y down the
    // left, X across the top. Combos 0..3 are transposed; yRight/xBottom move the label strips only.
    static const struct { bool tr, yR, xB; } kCombos[8] = { {1,0,0},{1,1,0},{1,0,1},{1,1,1},{0,0,0},{0,1,0},{0,0,1},{0,1,1} };
    // "axisMode" property: 0 = global (Preferences ▸ Globals, which defaults to
    // combo 4 "left & top"); 1..8 select a combo (kCombos index = axisMode - 1), same order as the choices.
    const int combo = std::clamp(choiceOf(el, "axisMode") - 1, 0, 7);
    // A 1-axis table (curve) transposes too — it flips the single strip between vertical (n0×1) and horizontal
    // (1×n0). Only the SECOND axis is conditional on the table actually having one; the single axis follows the
    // transpose onto whichever screen axis it lands.
    g.transpose = kCombos[combo].tr;
    g.yRight = kCombos[combo].yR; g.xBottom = kCombos[combo].xB;
    const int naxes = static_cast<int>(g.t.axes.size());
    const int vAxis = g.transpose ? 1 : 0, hAxis = g.transpose ? 0 : 1;   // storage axis on each SCREEN axis
    g.n0 = std::max(1, c.tiLiveN(g.t, 0));
    const int n1 = has2 ? std::max(1, c.tiLiveN(g.t, 1)) : 1;
    g.rows = g.transpose ? n1 : g.n0;                          // vertical-axis count
    g.cols = g.transpose ? g.n0 : n1;                          // horizontal-axis count
    g.zn = std::max(1, c.liveDepth(bind));                     // live Z-plane count (1 for a plain 2D table)
    { int& cz = tableCursors()[el.uid].z; cz = std::clamp(cz, 0, g.zn - 1); g.z = cz; }   // keep the active plane valid
    // Breakpoints, but only for an axis that is IN PLAY. An optional axis switched off collapses to one
    // bin (tiLiveN says 1) and still HAS a stored breakpoint, so asking for its bins gave a strip
    // labelling an axis the table does not use: etb[0].ff_table with its clt axis off drew a row header
    // reading "60.0" — a coolant bin nothing indexes by — and, because a header column exists only to be
    // labelled, a permanently blank corner cell above it. A disabled axis gets no strip and no corner.
    auto axisBins = [&](int k) {
        return (k < naxes && c.tiEnabled(g.t, k)) ? c.tiBins(g.t, k) : std::vector<double>{};
    };
    g.xb = axisBins(hAxis);   // horizontal breakpoints (if that axis exists and is on)
    g.yb = axisBins(vAxis);   // vertical breakpoints
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    // Cell precision, most specific first: this widget's own choice, else what the DATA says it is (a
    // sensor calibration's cells are readings in the sensor's type — lambda to hundredths, temperature
    // to tenths), else the global preference, which is a blanket default and outranks neither.
    if (const int own = ownDecimals(el, "decimals"); own > 0) g.dec = own - 1;
    else if (g.t.cellDigits >= 0)                             g.dec = g.t.cellDigits;
    else                                                      g.dec = choiceOf(el, "decimals") - 1;
    // Axis breakpoint precision, separate from the cell precision above. The props are indexed by STORAGE
    // axis (decimalsX = axes[0]) to match scaleX/scaleY, then mapped onto the screen axes here so both
    // follow transpose identically. "global" asks the globals, whose own answer may be 0 = "follow cell"
    // (the default) — borrowing g.dec, so an untouched table renders exactly as before. The borrow is a
    // VALUE you can see and change in Preferences, not a second meaning of the word "global".
    auto axisDec = [&](int storageAxis) { return TableWidget::axisDecimals(el, c, g.t, storageAxis, g.dec); };
    g.decH = axisDec(hAxis); g.decV = axisDec(vAxis);
    // …and HOW each screen axis reads: the unit it is shown in, and the precision that unit needs. The
    // strips, the inline editor and the width measurement all take the number from here, so a bin drawn
    // in volts and a bin typed in volts cannot mean different things.
    g.viewH = axisView(el, c, g.t, hAxis);
    g.viewV = axisView(el, c, g.t, vAxis);
    g.decH = g.viewH.digits; g.decV = g.viewV.digits;
    g.heat = choiceOf(el, "heatmap") != 1;   // global defaults to "On", as before
    // The "Font" property drives the grid's text AND therefore its metrics: row height, natural cell
    // width and the overflow/scrollbar decision all key off lh, so a bigger font makes bigger cells
    // rather than text spilling out of cells measured at the app font. Resolved once, into the struct
    // that render and the hit-test share.
    const float lh0 = jf::JTextHelper::lineHeight();
    const std::string fspec = el.prop("fontName");
    // …TIMES THE VIEW SCALE. The grid's whole geometry is text metrics, so this is the one place the
    // canvas scale has to enter: fold it in here and the cells, their text, the header strips and the
    // overflow decision all scale together, exactly as a bigger Font already made them bigger.
    // …TIMES THE VIEW SCALE, AND TIMES THIS GRID'S OWN CELL SIZE. A table is read as a field of numbers
    // rather than as a label, so it gets a density of its own on top of the interface scale — "denser
    // than the rest of the UI", not a second fight with it. 0/unset = 100 %, so an untouched table is
    // exactly what it was.
    // 1-based, as every table choice is: index 0 is "global" and choiceOf resolves it to the default
    // above (3 = 100 %) before it ever gets here, so slot 0 is simply 100 % as well.
    static const float kCellScales[] = { 1.f, 0.70f, 0.85f, 1.f, 1.25f, 1.5f, 2.f };
    const int csIdx = std::clamp(choiceOf(el, "cellScale"), 0, int(std::size(kCellScales)) - 1);
    const float cellK = kCellScales[csIdx];
    const float lh = fontSpecPx(fspec, lh0) * (zoom > 0.f ? zoom : 1.f) * cellK;
    g.lh = lh;
    g.fscale = (lh0 > 0.f) ? lh / lh0 : 1.f;
    g.face = fontSpecFace(fspec);
    const double pdRaw = numD("padding", -1.0);                       // common "Padding" prop (px); -1/unset = default
    const float pad = (pdRaw < 0.0) ? 3.f : static_cast<float>(pdRaw);
    // Axis-name bar (grid view): the live signal each SCREEN axis reads — the src selector's current channel,
    // falling back to the schema default / axis label. Vertical
    // axis = transpose ? storage-1 : storage-0, horizontal = the other, matching g.rows/g.cols above.
    auto axisChannel = [&](int k) -> std::string {
        if (k < 0 || k >= static_cast<int>(g.t.axes.size())) return {};
        if (!c.tiEnabled(g.t, k)) return {};   // an axis that is switched off names nothing
        const int sid = c.tiSrc(g.t, k);
        std::string nm = (sid >= 0 && c.meta()) ? c.meta()->signalName(sid) : std::string();
        if (nm.empty()) nm = g.t.axes[k].defaultSig;
        if (nm.empty()) nm = g.t.axes[k].label;
        return nm;
    };
    g.vName = axisChannel(vAxis);   // axisChannel() returns "" past the axis count, so a 1-axis table lands its one name correctly
    g.hName = axisChannel(hAxis);
    g.nameH = (g.xb.empty() && g.yb.empty() && g.vName.empty() && g.hName.empty()) ? 0.f : lh + 2.f;
    // Y-bin strip width. This was a flat 16% of the widget capped at 48px, with NO floor — so narrowing a
    // table shrank the strip in lockstep and the axis values lost digits off their right end (pushTextScaled
    // truncates at maxWidth). Measure the widest formatted bin instead and give it that, keeping the old 16%
    // as the minimum so nothing gets narrower than before. The cap is a third of the widget: a big font or
    // long bins may not eat the grid they label.
    if (g.yb.empty()) g.hdrW = 0.f;
    else {
        float ybW = 0.f;
        for (const double v : g.yb)
            ybW = std::max(ybW, jf::JTextHelper::measureWidthScaled(g.viewV.text(v), g.fscale));
        g.hdrW = std::clamp(ybW + 4.f, std::min(48.f, r.width * 0.16f), r.width * 0.33f);
    }
    g.hdrH = g.xb.empty() ? 0.f : lh + 3.f;
    g.gx = r.x + pad + (g.yRight ? 0.f : g.hdrW);              // Y strip on the left (default) or right
    g.gy = r.y + pad + g.nameH + (g.xBottom ? 0.f : g.hdrH);  // name bar on top, then the X strip (top) or grid
    const float gw = r.width - 2 * pad - g.hdrW, gh = r.height - 2 * pad - g.nameH - g.hdrH;
    if (gw <= 2.f || gh <= 2.f) return g;
    // Section sizing — the property's index: 0 global, 1 Interactive, 2 Stretch,
    // 3 Fixed, 4 Resize to contents. "global" resolves in the globals (default Fixed), so 0 no longer
    // reaches here; cells STRETCH to fill for Stretch (2), the other modes use a fixed "natural" cell size
    // (fits the formatted value + padding).
    const int hMode = choiceOf(el, "hSectionMode");
    const int vMode = choiceOf(el, "vSectionMode");
    std::string sample = "-8888"; if (g.dec > 0) { sample += '.'; sample.append(static_cast<size_t>(g.dec), '8'); }
    const float zpad = (zoom > 0.f ? zoom : 1.f);        // the paddings are 1:1 px, so they scale too
    const float natW = jf::JTextHelper::measureWidthScaled(sample, g.fscale) + 10.f * zpad;   // column width
    const float natH = lh + 6.f * zpad;                                                       // row height
    // Cell sizing + scrolling. Stretch (2) divides the space between the cells, but NEVER below natW/natH —
    // the size the formatted value actually needs. Without that floor, narrowing the widget shrank every
    // column until the cell values and the X bins above them lost digits off the end (the text truncates at
    // the cell width), which reads as the numbers being squashed. Flooring means a too-narrow Stretch table
    // overflows and scrolls — same as Fixed (3) / Resize-to-contents (4), which pin to the natural size so a
    // big table (the 21x22 ignition map) runs past the widget — instead of degrading into unreadable text.
    // Size once to learn whether it overflows, then, if it does, re-size against the reduced space so a
    // bar's gutter never sits on top of a cell.
    float availW = gw, availH = gh;
    g.natW = natW; g.natH = natH;
    auto sizeCells = [&] {
        g.cw = (hMode == 2) ? std::max(natW, availW / g.cols) : natW;
        g.ch = (vMode == 2) ? std::max(natH, availH / g.rows) : natH;
        g.contentW = g.cols * g.cw;  g.contentH = g.rows * g.ch;
        g.ovH = g.contentW > availW + 0.5f;  g.ovV = g.contentH > availH + 0.5f;
    };
    sizeCells();
    if (g.ovH || g.ovV) { availW = gw - (g.ovV ? kTSB : 0.f); availH = gh - (g.ovH ? kTSB : 0.f); sizeCells(); }
    g.vx = g.gx; g.vy = g.gy; g.vw = availW; g.vh = availH;
    // Clamp the stored offset to the live overflow (a resize / axis edit can shrink it out from under us),
    // then slide the grid origin. Everything downstream — cells, header strips, cell-trace, hit-tests —
    // positions off gx/gy, so this one shift scrolls the render and the input in lockstep.
    TableScroll& sc = tableScrolls()[el.uid];
    sc.x = std::clamp(sc.x, std::min(0.f, availW - g.contentW), 0.f);
    sc.y = std::clamp(sc.y, std::min(0.f, availH - g.contentH), 0.f);
    g.sx = g.ovH ? sc.x : 0.f;  g.sy = g.ovV ? sc.y : 0.f;
    g.gx += g.sx;  g.gy += g.sy;
    g.ok = true;
    return g;
}

// Expose the current rubber-band block for element id (the Surface's table context menu reads it).
int TableWidget::tableHoverAxis(const std::string& uid) {
    const auto it = tableCursors().find(uid);
    return it == tableCursors().end() ? -1 : it->second.hoverAxis;
}

TableWidget::TableCellSelection TableWidget::tableCellSelection(const std::string& uid) {
    TableCellSelection s;
    const auto it = tableCursors().find(uid);
    if (it == tableCursors().end()) return s;
    const TableCursor& c = it->second;
    s.r0 = std::min(c.row, c.aRow); s.r1 = std::max(c.row, c.aRow);
    s.c0 = std::min(c.col, c.aCol); s.c1 = std::max(c.col, c.aCol);
    s.activeRow = c.row; s.activeCol = c.col; s.has = true;
    return s;
}

void TableWidget::tableBeginOp(const std::string& uid, int op) {
    TableCursor& c = tableCursors()[uid];
    c.pendingOp = op; c.editing = true; c.buf.clear();          // next inline number applies across the block
}

int  TableWidget::tableCurrentPlane(const std::string& uid) { const auto it = tableCursors().find(uid); return it == tableCursors().end() ? 0 : it->second.z; }
void TableWidget::tableSetPlane(const std::string& uid, int z) { tableCursors()[uid].z = z; }   // Surface clamps to the plane count

TableWidget::TableDims TableWidget::tableDims(const Cache& c, const TableImage& t, bool transpose) {
    // Original semantics (AxisLayout): default (transpose=false) shows STORAGE AXIS 0 VERTICALLY (rows) and
    // axis 1 horizontally (cols) — vAxis = transpose?1:0, hAxis = transpose?0:1.
    TableDims d;
    d.transpose = transpose;   // 1-axis tables transpose too (flip the strip) — matches tableGeom
    d.n0 = std::max(1, c.tiLiveN(t, 0));                        // storage axis 0 live count (fast/inner dimension)
    const int n1 = (t.axes.size() >= 2) ? std::max(1, c.tiLiveN(t, 1)) : 1;
    d.rows = d.transpose ? n1 : d.n0;                          // rows = vertical-axis count
    d.cols = d.transpose ? d.n0 : n1;                          // cols = horizontal-axis count
    return d;
}

// THE PLANE IS PART OF THE ADDRESS. This took no z at all, so every op built on it — copy, paste,
// interpolate, smooth, "copy table as text" — read and wrote PLANE 0 whatever plane was on screen. On a
// 3D table (the VE map has six ethanol planes) that means the ops quietly worked on a different set of
// cells than the ones under the cursor: a paste onto plane 3 landed on plane 0, and the plane you were
// looking at did not change, so nothing about it looked wrong. z is now required, not defaulted — a
// caller that has a table has an element, and an element knows which plane it is showing.
int TableWidget::tableCellOffset(const TableImage& t, const TableDims& d, int screenRow, int col, int z) {
    const int vIdx = d.rows - 1 - screenRow, hIdx = col;       // vertical (top = highest) + horizontal indices
    // vAxis = transpose?1:0. transpose=false: i0=vIdx (axis0 vertical), i1=hIdx. transpose=true: i1=vIdx, i0=hIdx.
    const int i0 = d.transpose ? hIdx : vIdx, i1 = d.transpose ? vIdx : hIdx;
    return Cache::instance().tiCellOffset(t, i0, i1, z);       // ONE address rule (allocation stride)
}

const std::vector<TableWidget::GlobalDef>& TableWidget::globalDefs() {
    static const std::vector<GlobalDef> k = {
        { "axisMode",      5 },   // "left & top" (combo 4)
        // FIXED, not Stretch: a cell is the size its number needs, so a table reads the same in any panel
        // — a stretched grid in a wide panel spread eight cells into bars, which reads as a chart, not a
        // table, and changed shape every time the panel was resized.
        { "hSectionMode",  3 },   // Fixed
        { "vSectionMode",  3 },   // Fixed
        { "decimals",      2 },   // "1" decimal place
        { "cellScale",     3 },   // 100 % — a grid's own density, on top of the interface scale
        { "decimalsX",     0 },   // follow the cell's decimals
        { "decimalsY",     0 },
        { "decimalsZ",     0 },
        { "heatmap",       2 },   // On
        { "heatmapScheme", 1 },   // the first scheme
        { "cellTrace",     1 },   // Off
        { "depthControl",  3 },   // Stepper — the old else-branch, NOT Slider
        { "depthPosition", 1 },   // Top
    };
    return k;
}

int TableWidget::globalDefault(const char* key) {
    for (const GlobalDef& g : globalDefs()) if (std::strcmp(g.key, key) == 0) return g.def;
    return 1;
}

int TableWidget::choiceOf(const PanelElement& el, const char* key) {
    const std::string s = el.prop(key);
    if (!s.empty()) { try { const int v = std::stoi(s); if (v >= 1) return v; } catch (...) {} }
    return EditorSettings::instance().propGlobal(std::string("table.") + key, globalDefault(key));
}

bool TableWidget::tableTransposed(const PanelElement& el) {
    static const bool kTr[8] = { true, true, true, true, false, false, false, false };   // AxisLayout combos 0..7
    // Resolve the SAME "axisMode" through the SAME helper the render uses, so the cell ops transpose in
    // lock-step with what's drawn (was reading a separate "axisCombo" the menu wrote — they could disagree).
    return kTr[std::clamp(choiceOf(el, "axisMode") - 1, 0, 7)];
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("table", TableWidget, 190);

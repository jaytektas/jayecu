#pragma once
//
// DialogChrome / ListScroll — the window behaviour the studio's hand-built report dialogs share.
//
// The tune comparison and the firmware offer are not jf::JDialogWindow (they host whole dashboard pages
// and their own focus), so each grew its own loop — and neither grew a way to move, resize, minimise,
// maximise or close it, and both stopped their lists at "...more". Written here once so the two behave
// the same, and so the next such dialog does not start from nothing again.
//
//   DialogChrome  title bar: drag, double-click to maximise, and minimise / maximise / close buttons
//                 drawn exactly as the main window draws its own; the window resizes from its edges.
//   ListScroll    a list that scrolls: wheel over it, drag its bar, click the bar to page.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace dlgchrome {

inline bool hit(const jf::JRect& r, float x, float y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

struct DialogChrome {
    static constexpr float kBtnW = 28.f;          // the main window's own control-button width

    // Call once, after the window exists.
    template <class Win> void setup(Win& w, float hdr, uint32_t minW, uint32_t minH) {
        w.setResizable(true);
        w.setResizeTopInset(hdr);                  // the title bar drags; it does not resize
        w.setMinSize(minW, minH);
    }

    // Per frame, before anything else sees the mouse. Returns true when the close button was pressed —
    // the dialog decides what closing means (its "not now" / "no changes" answer).
    template <class Win> bool input(Win& w, float W, float hdr, float mx, float my, bool pressed, bool held) {
        const jf::JRect cr = closeRect(W, hdr), xr = maxRect(W, hdr), nr = minRect(W, hdr);
        if (pressed && hit(cr, mx, my)) return true;
        if (pressed && hit(xr, mx, my)) { w.setMaximized(!w.isMaximized()); return false; }
        if (pressed && hit(nr, mx, my)) { w.minimize(); return false; }
        const bool inTitle = my >= 0.f && my < hdr && mx < nr.x;
        if (pressed && inTitle) {
            const auto now = nowMs();
            if (now - m_lastTitleMs < 400) { w.setMaximized(!w.isMaximized()); m_lastTitleMs = 0; return false; }
            m_lastTitleMs = now;
            if (!w.isMaximized()) { m_drag = true; m_ax = mx; m_ay = my; }
        }
        if (m_drag && held) { auto [gx, gy] = w.globalCursorPos(); w.setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;
        return false;
    }

    template <class Win> void draw(jf::JPrimitiveBuffer& buf, Win& w, float W, float hdr,
                                   const std::string& title, float mx, float my) const {
        using namespace jf;
        JTitleBar::draw(buf, 0.f, 0.f, W, hdr, title, JStyle::current().cornerRadius, 0, 14.f, kBtnW * 3.f + 20.f);
        const JRect nr = minRect(W, hdr), xr = maxRect(W, hdr), cr = closeRect(W, hdr);
        static const uint8_t hov[4] = { 60, 60, 70, 200 }, ic[4] = { 190, 190, 200, 220 },
                             none[4] = { 0, 0, 0, 0 }, back[4] = { 22, 22, 28, 255 };
        if (hit(nr, mx, my)) buf.pushRectangle(nr.x, nr.y, nr.width, nr.height, hov, 0.f);
        buf.pushRectangle(nr.x + kBtnW * 0.5f - 4.f, hdr * 0.5f - 1.f, 9.f, 2.f, ic, 0.f);          // −
        if (hit(xr, mx, my)) buf.pushRectangle(xr.x, xr.y, xr.width, xr.height, hov, 0.f);
        const float cx = xr.x + kBtnW * 0.5f - 4.f, cy = hdr * 0.5f - 4.f;
        if (!w.isMaximized()) buf.pushRectangle(cx, cy, 9.f, 9.f, none, 1.f, 1.5f, ic);             // □
        else { buf.pushRectangle(cx + 2.f, cy, 7.f, 7.f, none, 0.f, 1.f, ic);                       // ⊡
               buf.pushRectangle(cx, cy + 2.f, 7.f, 7.f, back, 0.f, 1.f, ic); }
        JCloseButton::draw(buf, cr, hit(cr, mx, my));
    }

private:
    static jf::JRect closeRect(float W, float hdr) { return jf::JCloseButton::rectFor({ 0.f, 0.f, W, hdr }); }
    static jf::JRect maxRect(float W, float hdr)   { const auto c = closeRect(W, hdr); return { c.x - kBtnW, 0.f, kBtnW, hdr }; }
    static jf::JRect minRect(float W, float hdr)   { const auto c = closeRect(W, hdr); return { c.x - 2.f * kBtnW, 0.f, kBtnW, hdr }; }
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    bool m_drag = false;
    float m_ax = 0.f, m_ay = 0.f;
    int64_t m_lastTitleMs = 0;
};

// A scrolling list of fixed-height rows. layout() each frame with the box and the row count, then draw
// rows [first, last()) and drawBar(); input() takes the wheel and the bar.
struct ListScroll {
    int first = 0, visible = 0, total = 0;
    jf::JRect box{}, bar{}, thumb{};

    void reset() { first = 0; }
    void clear() { box = bar = thumb = {}; total = 0; }

    // Returns the width left for the rows (the bar takes its share only when it is needed).
    float layout(const jf::JRect& b, int rows, float rowH) {
        box = b; total = rows;
        visible = std::max(1, int((b.height - 8.f) / rowH));
        first = std::clamp(first, 0, std::max(0, total - visible));
        if (total <= visible) { bar = thumb = {}; return b.width; }
        constexpr float bw = 10.f;
        bar = { b.x + b.width - bw - 3.f, b.y + 4.f, bw, b.height - 8.f };
        const float th = std::max(24.f, bar.height * float(visible) / float(total));
        const float ty = bar.y + (bar.height - th) * float(first) / float(total - visible);
        thumb = { bar.x, ty, bw, th };
        return b.width - bw - 6.f;
    }
    int last() const { return std::min(total, first + visible); }

    void input(float mx, float my, bool pressed, bool held, float wheel) {
        const int maxFirst = std::max(0, total - visible);
        if (wheel != 0.f && hit(box, mx, my)) first -= int(wheel * 3.f);
        if (pressed && total > visible && hit(bar, mx, my)) {
            if (hit(thumb, mx, my)) { m_drag = true; m_grabY = my; m_grabFirst = first; }
            else first += (my < thumb.y ? -visible : visible);
        }
        if (m_drag && held && bar.height > thumb.height)
            first = m_grabFirst + int((my - m_grabY) / (bar.height - thumb.height) * float(maxFirst) + 0.5f);
        if (!held) m_drag = false;
        first = std::clamp(first, 0, maxFirst);
    }

    void drawBar(jf::JPrimitiveBuffer& buf) const {
        if (total <= visible) return;
        buf.pushRectangle(bar.x, bar.y, bar.width, bar.height, jf::Colors::Surface1, 4.f);
        buf.pushRectangle(thumb.x, thumb.y, thumb.width, thumb.height,
                          m_drag ? jf::Colors::TextPrimary : jf::Colors::TextSecondary, 4.f);
    }

private:
    bool m_drag = false;
    float m_grabY = 0.f;
    int m_grabFirst = 0;
};

}  // namespace dlgchrome

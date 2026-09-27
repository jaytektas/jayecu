// PADDING NARROWS A WIDGET; IT NEVER ERASES ONE.
//
// contentRect insets a widget's rect by its authored padding, and every widget lays its content out in
// what comes back. Inset both edges of a 40px-tall cell by 27 and what comes back is -14 tall — clamped to
// zero, so the widget draws its card and nothing else. That is not hypothetical: a per-type Widget Default
// of padding=27, sane for the 160x110 cell it was saved from, silently blanked every 40px Value cell that
// inherited it. The number was formatted, the colours resolved, the card painted — and there was nowhere
// to draw, which is the worst way for a layout to be wrong, because it looks like a live widget.
//
// A padding that does not fit is now capped at what leaves kMinContent, per axis, so a wide short cell
// keeps the horizontal padding it CAN afford and gives up only the vertical it cannot.
//
//   cmake --build build --target content_rect_test && ./build/content_rect_test
#include "../src/surface/widgets/ValueWidget.h"
#include "../src/surface/PanelModel.h"

#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[content-rect] %-56s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string sz(const jf::JRect& r) {
    char b[64]; std::snprintf(b, sizeof b, "%gx%g at %g,%g", r.width, r.height, r.x, r.y); return b;
}

// contentRect is a widget method and reads the widget's resolved skin, so drive it through a real one.
struct Probe : ValueWidget {
    using ValueWidget::ValueWidget;
    jf::JRect inner(int pad, float w, float h) {
        PanelElement el; el.id = 1; el.type = "value";
        el.props["padding"] = std::to_string(pad);
        setSource(el);
        return contentRect(jf::JRect{ 10.f, 20.f, w, h });
    }
};

int main() {
    jf::JSceneGraph graph;
    Probe p(graph);

    // The ordinary case: padding fits, and is applied exactly.
    {
        const jf::JRect r = p.inner(10, 200.f, 100.f);
        check(r.width == 180.f && r.height == 80.f, "a padding that fits is applied in full", sz(r));
        check(r.x == 20.f && r.y == 30.f, "…and moves the origin by it", sz(r));
    }
    {
        const jf::JRect r = p.inner(0, 200.f, 100.f);
        check(r.width == 200.f && r.height == 100.f && r.x == 10.f, "no padding, no inset", sz(r));
    }

    // THE REPORT: a 27px default meeting a 40px cell. It has to keep something to draw in.
    {
        const jf::JRect r = p.inner(27, 240.f, 40.f);
        check(r.height >= 8.f, "a padding taller than the box leaves content behind", sz(r));
        check(r.width == 240.f - 54.f, "…while the width, which had room, is padded in full", sz(r));
        check(r.y > 20.f && r.y + r.height <= 60.f, "…and the content still sits inside the widget", sz(r));
    }

    // Per axis: a cell short in BOTH directions gives up both, and one short in neither gives up neither.
    {
        const jf::JRect r = p.inner(27, 30.f, 30.f);
        check(r.width >= 8.f && r.height >= 8.f, "a small square keeps content in both axes", sz(r));
        const jf::JRect big = p.inner(27, 400.f, 300.f);
        check(big.width == 346.f && big.height == 246.f, "a big cell is unaffected by the cap", sz(big));
    }

    // THE ACTUAL BUG: two insets in a row. The base pads the widget, then the widget pads what it was
    // given — 8px of content less 6 a side is -4, and the Value cell's "is there room" guard declined,
    // so capping only the first inset fixed nothing you could see.
    {
        const jf::JRect content = p.inner(27, 240.f, 40.f);
        const jf::JRect inner   = Probe::insetRect(content, 6.f);
        check(inner.height >= Probe::kMinContent, "a second inset cannot push the content negative",
              sz(content) + " -> " + sz(inner));
        check(inner.width > 100.f, "…and the width it could afford is still there", sz(inner));
    }

    // Degenerate rects must not produce negative extents — a widget mid-drag can be a sliver.
    {
        const jf::JRect r = p.inner(27, 4.f, 0.f);
        check(r.width >= 0.f && r.height >= 0.f, "a sliver stays non-negative", sz(r));
    }

    std::printf("[content-rect] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

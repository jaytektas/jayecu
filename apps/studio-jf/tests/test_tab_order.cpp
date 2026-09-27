// What order does Tab walk a page in?
//
// Every tab ring in the studio used to be built by iterating the model's element vector, which is
// CREATION order. That is not an order anything is in on the screen: it is the sequence the page happened
// to be authored in, it survives every later edit, and Arrange > Front/Back rewrites it outright — so
// changing one control's z-order silently reshuffles the keyboard.
//
// The geometry below is the real "Configuration/Trigger/streams/Crank Primary" page, copied out of a live
// dashboard.gui, because that is the page the breakage was reported on. In element order its controls sit
// at y = 28, 112, 140, 84, 168, 196, 336, 224, 252, 280, 308, 392, 364 — Tab went down, back UP to
// Primitive, leapt from Gap ratio to Cell length, back up to Window, then from Repeats up to Nominal
// angle. The page itself reads straight down.
//
//   cmake --build build --target tab_order_test && ./build/tab_order_test
#include "../src/surface/CanvasWidget.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

int main() {
    using TabSlot = CanvasWidget::TabSlot;
    std::puts("=== tab ring: reading order, not creation order ===");

    // The page's interactive controls, in the ELEMENT order the ring used to be built in. The ids are the
    // element indices, so a failure prints something findable in the .gui.
    const std::vector<TabSlot> page = {
        {  0,  25.f,  28.f,  20.f },   // Enabled       (checkbox)
        {  2, 262.f, 112.f,  25.f },   // Capture index
        {  4, 262.f, 140.f,  25.f },   // Edge
        {  6, 262.f,  84.f,  25.f },   // Primitive
        {  8, 262.f, 168.f,  25.f },   // Slots
        { 10, 262.f, 196.f,  25.f },   // Gap ratio
        { 12, 262.f, 336.f,  25.f },   // Cell length
        { 14, 262.f, 224.f,  25.f },   // Window %
        { 16, 262.f, 252.f,  25.f },   // Width min
        { 18, 262.f, 280.f,  25.f },   // Width max
        { 20, 262.f, 308.f,  25.f },   // Width target
        { 22, 262.f, 392.f,  25.f },   // Nominal angle
        { 24, 262.f, 364.f,  25.f },   // Repeats
        { 26, 447.f,  84.f, 336.f },   // the Cells card — a tall panel beside the left column
    };

    const std::vector<int> ring = CanvasWidget::readingOrder(page);

    // Nothing may be dropped or invented: a ring that loses a control makes a field unreachable by keyboard.
    ck(ring.size() == page.size(), "every control is still in the ring",
       std::to_string(ring.size()) + " of " + std::to_string(page.size()));

    // THE property, stated directly: Tab never travels back up the page.
    {
        bool descends = true;
        float prevY = -1e9f;
        std::string worst;
        for (int id : ring) {
            for (const TabSlot& s : page)
                if (s.id == id) {
                    if (s.y < prevY) { descends = false; worst = "id " + std::to_string(id) + " jumps back up to y=" + std::to_string((int)s.y); }
                    prevY = s.y;
                }
        }
        ck(descends, "the ring never travels back UP the page", worst);
    }

    // The exact order, so a future change to the banding has to say so out loud.
    const std::vector<int> want = { 0, 6, 26, 2, 4, 8, 10, 14, 16, 18, 20, 12, 24, 22 };
    ck(ring == want, "…and it is the order the page reads", "got " + join(ring));

    // Controls sharing a row are walked ACROSS it, left to right — Primitive (x=262) before the Cells card
    // (x=447), both at y=84.
    {
        int iPrim = -1, iCard = -1;
        for (size_t i = 0; i < ring.size(); ++i) {
            if (ring[i] == 6)  iPrim = (int)i;
            if (ring[i] == 26) iCard = (int)i;
        }
        ck(iPrim >= 0 && iCard == iPrim + 1, "a shared row is walked left to right");
    }

    // Rows are BANDED, not compared exactly: a caption at y=282 beside a box at y=280 is one row to the
    // eye. Without the band, two controls either side of it would be ordered by a two-pixel accident.
    {
        const std::vector<TabSlot> row = {
            { 1, 300.f, 280.f, 25.f },
            { 2, 100.f, 282.f, 25.f },   // 2px lower, but the same row
            { 3, 200.f, 279.f, 25.f },
        };
        ck(CanvasWidget::readingOrder(row) == std::vector<int>({ 2, 3, 1 }),
           "a 2px stagger is still ONE row, ordered by x",
           join(CanvasWidget::readingOrder(row)));
    }

    // …but the band must not swallow a genuinely separate row, or a column of stacked fields collapses into
    // one row and comes out ordered by x — which is the original bug with extra steps.
    {
        const std::vector<TabSlot> stacked = {
            { 1, 300.f,   0.f, 25.f },
            { 2, 100.f,  28.f, 25.f },   // the next row down, 28px pitch
            { 3, 200.f,  56.f, 25.f },
        };
        ck(CanvasWidget::readingOrder(stacked) == std::vector<int>({ 1, 2, 3 }),
           "a 28px pitch is three rows, NOT one row sorted by x",
           join(CanvasWidget::readingOrder(stacked)));
    }

    // Degenerate inputs a real page hits: nothing focusable, and a single control.
    {
        ck(CanvasWidget::readingOrder({}).empty(), "an empty ring stays empty");
        ck(CanvasWidget::readingOrder({ { 7, 5.f, 5.f, 10.f } }) == std::vector<int>({ 7 }),
           "one control is its own ring");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "Tab walks the page the way it reads",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

#pragma once

// LandingView — the "What would you like to do?" landing, rendered as the CENTRAL widget (a backdrop inside
// the main window, NOT a modal dialog) while no project is open. A card is
// centred in the central area with three choices — Connect to an ECU / Create a new tune / Open an existing
// ECU — each a REAL jf::JButton (adopted, so it joins the window's focus tree), drawn as a card with the
// title + description painted over the button's own chrome. Being widgets they focus on click, take Tab,
// activate on Space/Return, and fire on release-inside so a press can be cancelled by sliding off — none of
// which the previous hand-drawn rows could do (they fired straight off the mouse-down).
// Shown at startup (no project); the app can swap back to it on Close Project.

#include "../model/ImageCache.h"
#include "AboutLogo.h"
#include "WrapText.h"

#include <j/core/JWidget.h>
#include <j/core/JButton.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

class LandingView : public jf::JWidget {
public:
    std::array<std::function<void()>, 3> onChoice;   // Connect / New tune / Open ECU

    explicit LandingView(jf::JSceneGraph& g) : jf::JWidget(g, "LandingView") {
        for (int i = 0; i < 3; ++i) {
            // Empty label: the button paints the card chrome (fill/border/hover/pressed/focus ring) and
            // this view paints the dot + title + description over it.
            m_btn[i] = adopt(std::make_unique<jf::JButton>(g, "", kCardW, kRowH));
            m_btn[i]->onClicked.connect([this, i] { if (onChoice[static_cast<size_t>(i)]) onChoice[static_cast<size_t>(i)](); });
        }
    }

    // The centre is not a container, so forward the pointer to the buttons; they own hover, arm/cancel
    // and the click itself.
    void handleMouseMove(float mx, float my) override    { for (auto* b : m_btn) b->handleMouseMove(mx, my); }
    void handleMousePress(float mx, float my) override   { for (auto* b : m_btn) b->handleMousePress(mx, my); }
    void handleMouseRelease(float mx, float my) override { for (auto* b : m_btn) b->handleMouseRelease(mx, my); }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        using namespace jf;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface0);   // central backdrop
        const jf::JRect card = cardRect();
        // At its own size: the artwork is resampled to this by assets/make-logo.sh, and a texture drawn
        // any other size is minified by the rasteriser with one sample per pixel to do it with.
        if (const jf::JRect L = logoRect(); L.width > 0.f)
            buf.pushImage(L.x, L.y, L.width, L.height, logo().tex);
        const float lh = JTextHelper::lineHeight();
        if (!JTextHelper::hasAtlas()) return;
        JTextHelper::pushText(buf, card.x + 4.f, card.y - lh - 6.f, "What would you like to do?",
                              Colors::TextPrimary, card.width);
        static const char* kTitle[3] = { "Connect to an ECU", "Create a new tune", "Open an existing ECU" };
        for (int i = 0; i < 3; ++i) {
            const jf::JRect r = rowRect(i);
            // The BUTTON paints the card: theme fill, border, hover, pressed and the focus ring — so the
            // card follows the theme and shows keyboard focus without this view tracking any of it.
            m_btn[i]->setBounds(r);
            m_btn[i]->populateRenderPrimitives(buf);
            uint8_t dot[4] = { Colors::Success[0], Colors::Success[1], Colors::Success[2], 255 };
            buf.pushRectangle(r.x + 14.f, r.y + r.height * 0.5f - 5.f, 10.f, 10.f, dot, 5.f);
            JTextHelper::pushText(buf, r.x + 36.f, r.y + 12.f, kTitle[i], Colors::TextPrimary, r.width - 48.f);
            wraptext::draw(buf, r.x + 36.f, r.y + 12.f + lh + 4.f, kDesc[i], Colors::TextSecondary, r.width - 48.f);
        }
    }

private:
    static constexpr float kCardW = 300.f, kRowH = 74.f, kGap = 12.f;
    // MEASURED TO FIT THE CARD. The old wording ran 327, 368 and 296 px into a text box 252 px wide,
    // so every one of the three was cut mid-word — "…and open the tu", "…to start", "…and pick a t".
    // A card is a third of the mark now, which buys 285 px; these are written to that, checked with
    // the app's own font through tools/layout/ruler.py. And they WRAP, so a narrow card or a bigger
    // font takes another line rather than losing the end of the sentence.
    static constexpr const char* kDesc[3] = { "Scan the ports and open its tune library.",
                                              "Start one offline, against a schema.",
                                              "Pick a tune from the ECUs already known." };
    // A card is as tall as its wrapped description needs, measured at the narrowest a card can be
    // (a column card) so no layout draws more lines than this made room for.
    float rowH() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float tw = std::min(kCardW, std::max(120.f, b.width - 2.f * kPad)) - 48.f;
        float d = 0.f;
        for (const char* t : kDesc) d = std::max(d, wraptext::height(t, tw));
        return std::max(kRowH, 12.f + jf::JTextHelper::lineHeight() + 4.f + d + 12.f);
    }
    static constexpr float kLogoGap = 28.f;   // between the mark and the choices
    static constexpr float kPad     = 24.f;   // what the mark leaves at the edges of the centre
    // THE MARK AND THE CHOICES ARE ONE BLOCK, centred together. Centring the card alone and hanging the
    // logo off its top pushes the logo toward the ceiling on a tall window and off it on a short one;
    // measuring both and centring the pair keeps the arrangement whatever the window is.
    //
    // A missing logo file costs nothing here: the height is zero and the card centres on its own, which
    // is exactly where it used to be.
    const ImageCache::Entry& logo() const {
        return ImageCache::instance().get(aboutlogo::path("jaytek-landing.png"));
    }

    // THE MARK AT ITS OWN SIZE, until the window is too small to hold it. Full size is the point of it
    // here — this is the whole centre of an empty studio — but a landing that overflows its own area is
    // worse than a smaller mark, so a window that cannot fit it gets it scaled to what there is.
    jf::JRect logoRect() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const ImageCache::Entry& e = logo();
        if (e.tex == jf::kNullTexture || e.w <= 0 || e.h <= 0) return { b.x, b.y, 0.f, 0.f };
        const float availW = b.width - 2.f * kPad;
        // THE CHOICES KEEP THEIR OWN HEIGHT, which is not always one row. Too narrow a window stacks
        // them into a column (see rowLayout), and that column is 246px where a row is 74 — so reserving
        // kRowH scaled the mark as though the other 172px were free, the block came out taller than the
        // area, and centring it hung the logo off the top and the third choice off the bottom. Asking
        // choicesH() is the same question the block height below already asks.
        const float availH = b.height - choicesH() - kLogoGap - 2.f * kPad;
        const float k = std::min({ 1.f, availW / float(e.w), availH / float(e.h) });
        const float w = float(e.w) * k, h = float(e.h) * k;
        // NEVER ABOVE THE TOP, however little room there is. Centring assumes the block fits; when it
        // does not, centring puts HALF the overflow off the top — which is how the choices came to be
        // drawn over the toolbar and the link banner on a 511px window. The choices cannot shrink
        // below one card each, so on a window that small something has to be cut, and cutting the
        // BOTTOM leaves a usable list with the rest reachable by growing the window. Cutting the top
        // leaves it painted over the chrome.
        const float block = h + kLogoGap + choicesH();
        return { std::round(b.x + (b.width - w) * 0.5f),
                 std::round(std::max(b.y, b.y + (b.height - block) * 0.5f)), w, h };
    }

    // THE CHOICES IN A ROW UNDER IT, when there is width for one. Three cards stacked under a 1024px
    // mark is a column of buttons adrift in the middle of a very wide space; side by side they sit on
    // the same base the logo does and the whole thing reads as one arrangement. A window too narrow for
    // the row gets the column back rather than three clipped cards.
    // THE ROW IS AS WIDE AS THE MARK. Three fixed cards under a 1024px logo leave a gap down each side
    // and read as a separate object; sharing its width makes the two one arrangement. The card is
    // therefore a THIRD of the mark, not a constant — and never narrower than the constant, so a small
    // window falls back to the column rather than to three slivers.
    float rowW() const {
        const jf::JRect L = logoRect();
        return (L.width > 0.f) ? L.width : 3.f * kCardW + 2.f * kGap;
    }
    float cardW() const {
        if (rowLayout()) return std::max(kCardW, (rowW() - 2.f * kGap) / 3.f);
        // A COLUMN CARD IS kCardW OR WHAT THERE IS, whichever is smaller. It was the constant
        // unconditionally, so a window narrower than 348 drew 300px cards into less than that and the
        // text ran off the right — the same overflow as the height, on the other axis.
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return std::min(kCardW, std::max(120.f, b.width - 2.f * kPad));
    }
    bool rowLayout() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return b.width - 2.f * kPad >= 3.f * kCardW + 2.f * kGap;
    }
    float choicesH() const { return rowLayout() ? rowH() : 3.f * rowH() + 2.f * kGap; }

    jf::JRect cardRect() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const jf::JRect L = logoRect();
        const float w  = rowLayout() ? 3.f * cardW() + 2.f * kGap : cardW();
        const float y  = (L.height > 0.f) ? L.y + L.height + kLogoGap
                                          : std::max(b.y, b.y + (b.height - choicesH()) * 0.5f);
        return { std::round(b.x + (b.width - w) * 0.5f), std::round(y), w, choicesH() };
    }
    jf::JRect rowRect(int i) const {
        const jf::JRect c = cardRect();
        if (rowLayout()) return { c.x + i * (cardW() + kGap), c.y, cardW(), rowH() };
        return { c.x, c.y + i * (rowH() + kGap), cardW(), rowH() };
    }
    std::array<jf::JButton*, 3> m_btn{};   // adopted -> owned here AND part of the focus tree
};

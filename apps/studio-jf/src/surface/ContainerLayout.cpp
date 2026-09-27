// The ONE managed-layout algorithm (moved verbatim from WidgetSupport, itself lifted from
// Surface::screenRectOf) — used by the canvas for its top-level elements AND by every PanelWidget for its
// owned children. See ContainerLayout.h for the contract.

#include "ContainerLayout.h"

#include <algorithm>

jf::JRect layoutChildRect(int mode, const jf::JRect& container, const std::vector<LayoutChild>& children,
                          int index, int gridColumns, int focusIndex, float scaleX, float scaleY,
                          const jf::JRect& freeRect, float gap) {
    const int n = static_cast<int>(children.size());
    if (mode == 0 || n == 0 || index < 0 || index >= n) return freeRect;   // Free — the child's own scaled rect
    const jf::JRect& page = container;
    switch (mode) {
        case 1: {   // Y Axis — vertical stack, full width, heights scaled to fit (scale by the SUM of child heights)
            float sum = 0.f; for (const auto& e : children) sum += e.rect.height; const float sy = sum > 0.f ? page.height / sum : 1.f;
            float y = page.y; for (int i = 0; i < index; ++i) y += children[i].rect.height * sy;
            return { page.x, y, page.width, children[index].rect.height * sy };
        }
        case 8: {   // Column — vertical stack, full width, children keep their NATURAL height
            // Y Axis (1) divides the container's height between its children, so the same page looks
            // different depending on how many rows it has: two rows inflate until a banner fills a third
            // of the screen, thirty squash until their labels are unreadable. A form row has a height —
            // it is a control — and a column of them is as tall as it is. When that is taller than the
            // container the surface scrolls, which is the honest answer and the one Y Axis cannot give.
            float y = page.y;
            for (int i = 0; i < index; ++i) y += children[i].rect.height * scaleY;
            return { page.x, y, page.width, children[index].rect.height * scaleY };
        }
        case 2: {   // X Axis — horizontal row, full height, widths scaled to fit (scale by the SUM of child widths)
            float sum = 0.f; for (const auto& e : children) sum += e.rect.width; const float sx = sum > 0.f ? page.width / sum : 1.f;
            float x = page.x; for (int i = 0; i < index; ++i) x += children[i].rect.width * sx;
            return { x, page.y, children[index].rect.width * sx, page.height };
        }
        case 6: {   // Grid — configurable column count
            const int cols = std::max(1, std::min(gridColumns, n)), rows = (n + cols - 1) / cols;
            const float cw = page.width / static_cast<float>(cols), ch = rows ? page.height / static_cast<float>(rows) : page.height;
            return { page.x + (index % cols) * cw, page.y + (index / cols) * ch, cw, ch };
        }
        case 7: {   // Wrap — a FLOW: children keep their authored size, run left to right, and wrap to a new
                    // row when the next one will not fit. The row count is therefore a result of the width
                    // and of how many children there are, not something the document has to state — which is
                    // what a strip of status lamps needs, since a different definition brings a different
                    // number of them.
            // A FLOW DOES NOT SCALE ITS CHILDREN — it wraps them. The scale here is the container's own fit
            // ratio (its box over the size it was authored at), and applying it to a flow defeats the
            // flow: the status lamps are authored 1280 wide and shown in a 240 px dock, so every cell came
            // out at a fifth of its size, eleven slivers on one row that no longer said anything, instead
            // of two lamps a row wrapped over six rows. Narrower means MORE ROWS, not smaller children.
            // The gap goes BETWEEN children, never before the first of a row or after the last: added to
            // the advance rather than to the child's box, so a row still ends flush with the container and
            // the wrap test asks whether the child itself fits, not the child plus a gutter nothing will
            // occupy. Getting that backwards costs a column at exactly the widths where one is worth most.
            const float w = page.width;
            float x = page.x, y = page.y, rowH = 0.f;
            for (int i = 0; i < n; ++i) {
                const float cw = children[i].rect.width, ch = children[i].rect.height;
                if (x > page.x && x + cw > page.x + w + 0.5f) { x = page.x; y += rowH + gap; rowH = 0.f; }
                if (i == index) return { x, y, cw, ch };
                x += cw + gap;
                rowH = std::max(rowH, ch);
            }
            return freeRect;
        }
        case 4: return page;   // Card — children fill the page; the run-mode visibility gate shows the matching one
        case 5:                // Index Card — the focused child fills the page; the others park offscreen (zero rect)
            return index == std::min(focusIndex, n - 1) ? page : jf::JRect{ page.x, page.y, 0.f, 0.f };
        case 3: {   // Border — N/S full width (element height), W/E between them (element width), Center the rest.
            auto regionOf = [](const std::string& r) -> int {
                return r == "North" ? 1 : r == "South" ? 2 : r == "West" ? 3 : r == "East" ? 4 : 0;
            };
            float top = page.y, bottom = page.y + page.height, left = page.x, right = page.x + page.width;
            // Two passes, because Center is "whatever the strips leave" and that is not known until EVERY
            // strip has been carved. Carving in declaration order and handing Center the remainder at the
            // moment it was reached gave it everything the strips before it had not taken — including the
            // area a later strip was about to claim, so a Center declared before an East child was drawn
            // straight through it (an imported dialog's `West, Center, East`, which is the usual order).
            jf::JRect strip{};
            bool isStrip = false;
            for (int i = 0; i < n; ++i) {
                const int reg = regionOf(children[i].region);
                if (reg == 0) continue;                              // Center: below, once the frame is final
                jf::JRect r{};
                switch (reg) {
                    case 1: r = { left, top, right - left, children[i].rect.height * scaleY };                 top    += r.height; break;
                    case 2: r = { left, bottom - children[i].rect.height * scaleY, right - left, children[i].rect.height * scaleY }; bottom -= r.height; break;
                    case 3: r = { left, top, children[i].rect.width * scaleX, bottom - top };                 left   += r.width;  break;
                    default: r = { right - children[i].rect.width * scaleX, top, children[i].rect.width * scaleX, bottom - top }; right -= r.width; break;
                }
                if (i == index) { strip = r; isStrip = true; }
            }
            if (isStrip) return strip;
            return { left, top, right - left, bottom - top };        // Center = what the strips left

        }
        default: return freeRect;
    }
}

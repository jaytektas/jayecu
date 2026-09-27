#pragma once

// Managed container layout — the 9 layout modes (0 Free .. 8 Column) that arrange a container's children,
// shared by the CANVAS (its top-level elements) and every PANEL (its owned children) so both place
// children through ONE algorithm instead of two copies of the math. Formerly in WidgetSupport.

#include <j/core/SceneGraph.h>   // jf::JRect

#include <string>
#include <vector>

struct LayoutChild {
    jf::JRect   rect;      // the child's RAW (unscaled) authored rect — its w/h drive Y/X/Border sizing
    std::string region;    // Border placement: "North" / "South" / "West" / "East" / "" (= Centre)
};

// One child's on-screen rect within `container` for managed layout `mode`. `children` = every sibling in
// order (raw rects + region); `index` selects the one being placed. `gridColumns` / `focusIndex` parameterize
// Grid / Index Card. Border strips are sized by the child's raw extent × (scaleX for W/E widths, scaleY for
// N/S heights) — pass the container's zoom (canvas: t.scale for both; panel: its content sx / sy). `freeRect`
// is this child's already-scaled screen rect, returned verbatim for Free (mode 0) and any unknown mode.
//
// `gap` is the space a WRAP (mode 7) leaves between its children, in container pixels — 0, so every other
// mode and every existing flow is unchanged. A flow packs edge to edge otherwise, which is right for a
// strip of status lamps and wrong for a row of titled panels: their borders meet and read as one box with
// a line through it. Stating it on the container beats padding each child with an invisible margin, which
// is a second size to keep in step with the first and makes every child a lie about its own width.
jf::JRect layoutChildRect(int mode, const jf::JRect& container, const std::vector<LayoutChild>& children,
                          int index, int gridColumns, int focusIndex, float scaleX, float scaleY,
                          const jf::JRect& freeRect, float gap = 0.f);

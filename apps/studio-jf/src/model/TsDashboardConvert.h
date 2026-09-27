#pragma once

// TsDashboardConvert — turn a parsed TunerStudio dashboard (tsdash::Dashboard) into a native project
// document: the navigation tree from [Menu], and one page of real studio widgets per menu leaf.
//
// This is the EAGER half of the import. Every dialog is converted once, here, into ordinary elements that
// behave like anything else you drew: they can be moved, rebound, restyled and deleted, and the ini is
// never consulted again. That is the whole point — the ini is an input format, not a runtime dependency.
//
// Two decisions worth stating, because they are what make the output ordinary rather than a viewer:
//
//   * CONTROL CHOICE goes through Cache::controlFor(path), the same function a dictionary drag uses. An
//     imported field therefore gets the control its binding deserves — a toggle for a flag, a combo for an
//     enum, a table editor for a map — instead of a hand-maintained mapping that would drift from it.
//   * NO SCALE IS STAMPED. Controls carry only their binding; the scale is read from the meta at display
//     time (CanvasWidget::scaleOf), exactly as for a control dropped from the dictionary. One source of
//     truth, so correcting a scale later fixes every control bound to it.
//
// Layout is a plain vertical flow: TS describes intent (xAxis/yAxis/border placements) rather than
// geometry, and a predictable stack of rows is easier to fix up by hand than a clever guess.

#include "EditorSettings.h"
#include <j/core/JStyle.h>

#include "MetaModel.h"
#include "TsDashboard.h"

#include <j/config/Json.h>

#include <map>
#include <string>
#include <vector>

namespace tsconvert {

// Page geometry — a dialog's fields stack down a page of this width.
struct Layout {
    // The page a converted dialog is laid out on: the Preferences canvas size, like every other page. A
    // dialog taller or wider than that stores its own size (see buildPage) — otherwise the page carries no
    // size at all and follows the preference, so raising it in Preferences gives every imported page the
    // extra room too.
    // THE PAGE THE CONVERTER LAYS OUT TO — a stated design size, not a personal preference.
    //
    // These read EditorSettings, so the same .ini imported on two machines produced two different
    // documents: dialogs wrapped differently and table editors came out a different size, because one
    // person's canvas preference was 2560 and another's 1280. Same ECU, same file, different pages —
    // and nothing on screen to say why.
    //
    // A dialog's width comes from the INI (its natural width; see naturalWidth), and the preference is
    // only ever a CAP on that — see maxDialogW in buildDashboard. Everything else here is a fixed design
    // page so an import is reproducible: hand the same .ini to any machine and get the same document.
    float pageW = 1280.f;
    float pageH = 800.f;
    float marginX = 24.f, marginY = 24.f;
    // THE APP'S OWN CONTROL HEIGHT, not a number this converter invented. A converted settings row used
    // to be 26 px because that is what this file said, while every native control beside it was 30 —
    // JStyle::controlHeight — so two pages of the same application disagreed about how tall a field is.
    // The converter runs inside the app, so it can simply ask, and then the answer follows the interface
    // scale like everything else rather than being frozen at whatever was typed here.
    float rowH = jf::JStyle::current().controlHeight, rowGap = 2.f;
    float labelW = 240.f, controlW = 260.f, gap = 12.f;
    // A converted dialog is sized to its CONTENT, as TunerStudio sizes one: dialogW is the MINIMUM, and a
    // dialog whose captions or columns need more gets more (up to maxDialogW, past which the page scrolls).
    // Stretching every dialog across a 1280px page instead would strand each control in an empty row.
    // maxDialogW is the PAGE: a converted dialog never grows past the window it is shown in. A dialog whose
    // captions want more than that shares out what there is, exactly as TunerStudio's own dialog does when
    // it hits the edge of its window — pushing the far column off the right instead reads as content lost.
    // THE MINIMUM IS A MINIMUM, not a target. At 760 every page opened 800 px wide whatever was on it —
    // a four-field dialog got the same slab as a table editor, and all of that slack went into the caption
    // column, so a 200 px caption sat in a 493 px box with its control pushed to the far side of the page.
    // The window a page opens in is now sized from this number (Surface::preferredSize), which makes a
    // generous floor a page nobody asked for. Wide enough that a one-row dialog still reads as a dialog.
    float dialogW = 360.f;
    float maxDialogW = 0.f;      // 0 = the CAP from Preferences (New page width), set in buildDashboard
    float minPanelW = 260.f;                          // a nested column is as wide as its rows, not a page
    // How wide a line of PROSE asks to be — a help row, a banner, a note. It wraps, so its own length is
    // not a width the page has to honour; this is the width it would like before it starts wrapping.
    float prosePreferredW = 420.f;
    // An embedded table/curve editor. A definition may state its own ("size = w, h"); none in the wild do,
    // so this is the size the spec suggests as the default — a plot you can read and drag points on. 220px
    // tall (the old value) left the curve a strip once its gauge and cells took their column.
    float editorW = 480.f, editorH = 350.f;
    float gaugeW = 240.f;                             // a dial is square, and asks for its own size only
    // Caption widths have to be estimated at CONVERT time, where there is no font: the page is built before
    // anything is laid out, let alone rendered. ~7.2px per character matches the studio's 16px UI font
    // closely enough to size a dialog; a few px of slack is invisible, a wrong dialog width is not.
    float charW = 7.2f;
};

// THE LIBRARY KEY THE STATUS LAMPS LIVE UNDER. Every other key is a menu path built from labels, so a
// leading '%' cannot collide with one and no tree node points here — the page is reachable only by the
// dock that hosts it.
inline constexpr const char* kStatusPage = "%Status";

// What a conversion produced — reported so an import can say what it did instead of the user discovering
// it page by page.
struct Report {
    int pages = 0;          // menu leaves that became a page
    int widgets = 0;        // elements emitted
    int tables = 0;         // leaves that bound straight to a [TableEditor]/[CurveEditor]
    int emptyPages = 0;     // leaves whose dialog was missing or had nothing convertible
    int skippedItems = 0;   // items with no native counterpart
    // WHAT was empty or skipped, not just how many — a count alone cannot be acted on, and these are
    // exactly the cases where the import quietly differs from what TunerStudio would have shown.
    std::vector<std::string> emptyNames;                 // menu leaves that produced no widget
    std::map<std::string, int> skippedKinds;             // item kind -> how many were dropped
};

// The project document: { "tree": [...], "panelLibrary": { "<node path>": <page> } }. Surfaces are left
// out — the app creates a fresh Main, and the pages are reached through the tree.
jf::JJson buildDashboard(const tsdash::Dashboard& dash, const MetaModel& meta,
                         Report* report = nullptr, const Layout& lay = {});

}  // namespace tsconvert

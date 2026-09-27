#pragma once

// StyleMetrics — how tall a control is, said once.
//
// A document used to state a height for every widget on it, and three different generators each had
// their own idea of what that height should be: the native layout installers author.py (LBL_H 24,
// CTL_H 25, a checkbox 20 — or 16 when the row pitch was tight), the .ini converter (rowH 26, its
// footer buttons 24), and JStyle, which is what the application actually draws its own controls with
// (controlHeight 30, buttonHeight 32, labelHeight 20, checkHeight 22). None of them could see the
// others. A checkbox came out three different sizes in one document, and a converted settings row sat
// 26 px tall beside a native control of 30.
//
// Worse, a stated height is a number frozen at whatever the generator assumed. It cannot follow the
// interface scale, so raising that scale grew the text inside boxes that stayed the size a Python
// script chose. The generators cannot fix this themselves: they are offline, they emit JSON, and
// JStyle is a C++ object that exists only while the app is running.
//
// So a document stops stating the height and states the KIND. Height 0 means "ask the style", and this
// answers — from JStyle, at render time, already multiplied by the interface scale. One place, one
// answer, every generator agreeing by construction rather than by luck.
//
// A 0 for a kind with no natural height (a table, a graph, a panel, a viewport) means exactly that: it
// has no opinion, and the caller keeps whatever the document said.

#include <j/core/JStyle.h>

#include <algorithm>
#include <string>
#include <vector>

// The height a widget of this type is, or 0 if the kind has no natural height of its own.
inline float naturalHeightFor(const std::string& type) {
    const jf::JStyle& s = jf::JStyle::current();
    // Text that is read, not operated.
    if (type == "label" || type == "text") return s.labelHeight;
    // A tick box is square and its own size.
    if (type == "checkbox" || type == "radio" || type == "toggle" || type == "indicator") return s.checkHeight;
    // Anything you push.
    if (type == "command" || type == "action" || type == "tuneaction" ||
        type == "learnedaction" || type == "wizard") return s.buttonHeight;
    // Anything you type into or choose from — the interactive field family.
    if (type == "field" || type == "configedit" || type == "combobox" || type == "enum" ||
        type == "value" || type == "expression" || type == "settingselector" ||
        type == "channels" || type == "wiring" || type == "canfield") return s.controlHeight;
    if (type == "slider") return s.sliderHeight;
    return 0.f;   // panel, viewport, table, curve, dial, gauge, livegraph… — sized by what they show
}

// The height to lay a document element out at: what it says, or the style's answer for its kind when it
// says nothing. A caller that has neither keeps 0 and the layout treats it as it always did.
inline float layoutHeightOf(const std::string& type, float stated) {
    if (stated > 0.f) return stated;
    const float natural = naturalHeightFor(type);
    return natural > 0.f ? natural : stated;
}

// HOW TALL THE CONTENT ON A PAGE ACTUALLY IS, in the page's own units.
//
// A REFLOW page takes the area it is given, which makes its canvas exactly the view — and a canvas that
// is exactly the view can never overflow, so nothing ever scrolls and anything past the bottom is simply
// clipped away. That is not "it fits", it is "you cannot see the rest".
//
// So the height is asked of the CONTENT: a Column stacks its children, so it is as tall as their heights
// added up; any other arrangement is as tall as its lowest edge. The caller takes whichever is larger,
// the view or this, and the surface scrolls when the content wins.
template <class ElementList>
float contentHeightOf(const ElementList& els, int layoutMode) {
    float h = 0.f;
    if (layoutMode == 8) {                       // Column — children stack, so heights add
        for (const auto& e : els) h += layoutHeightOf(e.type, e.h);
    } else {                                     // anything else — as tall as the lowest thing on it
        for (const auto& e : els) h = std::max(h, e.y + layoutHeightOf(e.type, e.h));
    }
    return h;
}

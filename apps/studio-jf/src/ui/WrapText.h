#pragma once
//
// WrapText — prose drawn into a width WRAPS; it does not clip.
//
// JTextHelper::pushText clips at the width it is given, so every sentence the studio drew into a box
// stopped at the box's edge, mid-word, with nothing to say there was more. Doing it properly is two
// halves that must agree: wrap once to the width, then size whatever holds the text from THAT wrapped
// line count and move everything below it down. These are those two halves, used together, so the
// height and the drawing can never disagree.
//
// Not for single-line controls — a button caption, a grid cell, a tab — where clipping is the design.

#include <j/core/JTextHelper.h>
#include <j/core/JLabel.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace wraptext {

struct Wrapped {
    std::string text;      // the text with '\n' at every break (pushText draws those as new lines)
    int         lines = 0; // 0 for empty text: it takes no room
    float       height = 0.f;
};

// A window measured before its first frame can get 0 for every width (ChoiceDialog.h has the story),
// which would wrap nothing and size the window for one line. A zero for real text is not a measurement,
// so the wrap falls back to a deliberately wide per-character estimate: early breaks, never lost words.
inline std::string estimateWrap(const std::string& s, float w) {
    constexpr float kCharW = 7.6f;
    std::string out, line, word;
    auto flush = [&] {
        if (word.empty()) return;
        if (!line.empty() && float(line.size() + 1 + word.size()) * kCharW > w) { out += line + "\n"; line.clear(); }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ')       flush();
        else if (c == '\n') { flush(); out += line + "\n"; line.clear(); }
        else                word += c;
    }
    flush();
    return out + line;
}

inline Wrapped wrap(const std::string& s, float w) {
    Wrapped r;
    if (s.empty()) return r;
    r.text   = jf::JTextHelper::measureWidth("Mm") > 0.f ? jf::JTextHelper::wrapToWidth(s, w)
                                                          : estimateWrap(s, w);
    r.lines  = 1 + static_cast<int>(std::count(r.text.begin(), r.text.end(), '\n'));
    r.height = static_cast<float>(r.lines) * jf::JTextHelper::lineHeight();
    return r;
}
inline int   lines (const std::string& s, float w) { return wrap(s, w).lines; }
inline float height(const std::string& s, float w) { return wrap(s, w).height; }
// Extra height over one line: what the things below a one-line slot must move down by.
inline float extra (const std::string& s, float w) { return std::max(0.f, height(s, w) - jf::JTextHelper::lineHeight()); }

// Draw `s` wrapped to `w` at (x, y); returns the height it took, for the caller's next y.
inline float draw(jf::JPrimitiveBuffer& buf, float x, float y, const std::string& s,
                  const uint8_t color[4], float w) {
    const Wrapped r = wrap(s, w);
    if (r.lines) jf::JTextHelper::pushText(buf, x, y, r.text, color, w);
    return r.height;
}

// The same, each line centred across [x, x + w] — an empty-state message in the middle of a plot.
inline float drawCentred(jf::JPrimitiveBuffer& buf, float x, float y, float w, const std::string& s,
                         const uint8_t color[4]) {
    const Wrapped r = wrap(s, w);
    const float lh = jf::JTextHelper::lineHeight();
    size_t a = 0;
    for (int i = 0; i < r.lines; ++i) {
        size_t e = r.text.find('\n', a);
        if (e == std::string::npos) e = r.text.size();
        const std::string ln = r.text.substr(a, e - a);
        const float lw = jf::JTextHelper::measureWidth(ln);
        jf::JTextHelper::pushText(buf, x + std::max(0.f, (w - lw) * 0.5f), y + float(i) * lh, ln, color, w);
        a = e + 1;
    }
    return r.height;
}

// A word-wrapped JLabel is only as tall as it was made. This makes it as tall as its text at the width
// it actually has (after layout). Returns true when the height changed — the caller re-lays out.
inline bool fit(jf::JLabel* l) {
    if (!l || !jf::JTextHelper::hasAtlas()) return false;
    jf::JRect b = l->bounds();
    if (b.width <= 0.f) return false;
    const float h = l->heightFor(b.width);
    if (std::fabs(h - b.height) < 0.5f) return false;
    b.height = h;
    l->setBounds(b);
    return true;
}

} // namespace wraptext

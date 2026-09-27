#pragma once

// LuaHighlighter — a lightweight Lua syntax highlighter for a JTextArea's setHighlighter hook. Fills a
// per-character RGBA colour: comments, strings (quoted + long-bracket), keywords, and numbers get their own
// colour; everything else is default text. A single-pass char scanner — no dependencies, no Lua runtime.

#include <j/core/JStyle.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace LuaHighlighter {

inline const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> k = {
        "and","break","do","else","elseif","end","false","for","function","goto","if","in",
        "local","nil","not","or","repeat","return","then","true","until","while" };
    return k;
}

inline void highlight(const std::string& src, std::vector<uint8_t>& out) {
    const size_t n = src.size();
    out.assign(n * 4, 0);
    // THE THEME DECIDES, NOT THE FILE. These were one hardcoded set, and it was the DARK one: default
    // text at {220,220,228} is very nearly white, so on the light theme the script was white-on-white
    // and simply could not be read. Syntax colouring is a set of hues chosen against a background, so
    // it has to ask which background it is on.
    //
    // Which is which comes from the palette itself rather than a theme name — a scheme is free to be
    // light or dark whatever it is called, and the only thing that matters here is whether the paper
    // under the text is bright.
    const uint8_t* bg = jf::Colors::Surface0;
    const bool dark = (std::max({bg[0], bg[1], bg[2]}) + std::min({bg[0], bg[1], bg[2]})) / 2 < 128;
    // Dark set: the familiar editor palette. Light set: the same ROLES at a darkness that reads on
    // paper — the green comment and the blue keyword stay recognisable, they just stop glowing.
    static const uint8_t defD[3] = {220,220,228}, cmtD[3] = {106,153,85}, strD[3] = {206,145,120},
                         kwD[3]  = {86,156,214},  numD[3] = {181,206,168};
    static const uint8_t defL[3] = {32,32,38},    cmtL[3] = {0,110,60},    strL[3] = {150,50,20},
                         kwL[3]  = {20,80,180},   numL[3] = {90,80,0};
    const uint8_t* def = dark ? defD : defL;
    const uint8_t* cmt = dark ? cmtD : cmtL;
    const uint8_t* str = dark ? strD : strL;
    const uint8_t* kw  = dark ? kwD  : kwL;
    const uint8_t* num = dark ? numD : numL;
    auto fill = [&](size_t a, size_t b, const uint8_t c[3]) {
        for (size_t i = a; i < b && i < n; ++i) { out[i*4]=c[0]; out[i*4+1]=c[1]; out[i*4+2]=c[2]; out[i*4+3]=255; }
    };
    for (size_t i = 0; i < n; ) {
        const char ch = src[i];
        if (ch == '-' && i + 1 < n && src[i+1] == '-') {                      // comment
            if (i + 3 < n && src[i+2] == '[' && src[i+3] == '[') {            // --[[ block ]]
                const size_t e = src.find("]]", i + 4); const size_t end = (e == std::string::npos) ? n : e + 2;
                fill(i, end, cmt); i = end; continue;
            }
            const size_t e = src.find('\n', i + 2); const size_t end = (e == std::string::npos) ? n : e;
            fill(i, end, cmt); i = end; continue;
        }
        if (ch == '[' && i + 1 < n && src[i+1] == '[') {                      // long-bracket string [[ ... ]]
            const size_t e = src.find("]]", i + 2); const size_t end = (e == std::string::npos) ? n : e + 2;
            fill(i, end, str); i = end; continue;
        }
        if (ch == '"' || ch == '\'') {                                       // quoted string
            const char q = ch; size_t j = i + 1;
            while (j < n && src[j] != q) { if (src[j] == '\\' && j + 1 < n) ++j; ++j; }
            const size_t end = (j < n) ? j + 1 : n; fill(i, end, str); i = end; continue;
        }
        if (std::isdigit((unsigned char)ch) || (ch == '.' && i + 1 < n && std::isdigit((unsigned char)src[i+1]))) {
            size_t j = i; while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '.')) ++j;   // ints/floats/hex
            fill(i, j, num); i = j; continue;
        }
        if (std::isalpha((unsigned char)ch) || ch == '_') {                  // identifier / keyword
            size_t j = i; while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) ++j;
            fill(i, j, keywords().count(src.substr(i, j - i)) ? kw : def); i = j; continue;
        }
        fill(i, i + 1, def); ++i;                                            // punctuation / whitespace
    }
}

}  // namespace LuaHighlighter

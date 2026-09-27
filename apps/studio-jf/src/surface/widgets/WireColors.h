#pragma once

// WireColors — the shared palette for the wiring UI: automotive wire-colour codes (base/stripe) and
// connector shell colour names -> RGBA, used by WiringWidget (the row) and SelectConnectionDialog (the
// picker). One source so the row and the dialog draw a resource's wire identically.

#include <array>
#include <cctype>
#include <cstdint>
#include <string>

namespace wirecol {
using RGBA = std::array<uint8_t, 4>;

// Short wire codes -> RGBA (W R G Bu Br O Y V Gy Pk Lg Lb + B=black), matching the board yaml scheme.
inline RGBA wire(std::string code) {
    for (auto& ch : code) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (code == "w")  return {235, 235, 235, 255};
    if (code == "b")  return { 40,  40,  40, 255};
    if (code == "r")  return {200,  60,  55, 255};
    if (code == "g")  return { 45, 160,  75, 255};
    if (code == "bu") return { 60, 110, 200, 255};
    if (code == "br") return {135,  85,  45, 255};
    if (code == "o")  return {225, 145,  45, 255};
    if (code == "y")  return {225, 200,  55, 255};
    if (code == "v")  return {150,  85, 190, 255};
    if (code == "gy") return {150, 150, 150, 255};
    if (code == "pk") return {230, 125, 175, 255};
    if (code == "lg") return {135, 205, 125, 255};
    if (code == "lb") return {125, 195, 230, 255};
    return {150, 150, 150, 255};
}

// Connector SHELL colours are full names ("white" / "blue" / "black"), not the terminal wire codes.
inline RGBA shell(std::string name) {
    for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (name == "white")  return {235, 235, 235, 255};
    if (name == "black")  return { 40,  40,  40, 255};
    if (name == "blue")   return { 60, 110, 200, 255};
    if (name == "red")    return {200,  60,  55, 255};
    if (name == "green")  return { 45, 160,  75, 255};
    if (name == "yellow") return {225, 200,  55, 255};
    if (name == "grey" || name == "gray") return {150, 150, 150, 255};
    if (name == "brown")  return {135,  85,  45, 255};
    if (name == "orange") return {225, 145,  45, 255};
    return { 90,  90,  90, 255};   // unknown shell
}

// "Lg/Br" -> base "Lg", stripe "Br"; a bare code -> empty stripe.
inline void split(const std::string& s, std::string& base, std::string& stripe) {
    const auto slash = s.find('/');
    if (slash == std::string::npos) { base = s; stripe.clear(); }
    else { base = s.substr(0, slash); stripe = s.substr(slash + 1); }
}

// WHAT A TERMINAL IS CALLED, once you are standing at the loom: "CN3-25" -> "CN3 (BLUE) 25".
//
// "CN3" means nothing until you know which of the three connectors it is, and they are told apart by
// colour. The badge is already FILLED in that colour, so the word repeats it — deliberately, because
// the fill only works on a screen. A screenshot pasted into a message, a printed harness sheet and
// anyone who cannot pick blue from black all need it in words.
//
// Here rather than in any of the three places that draw it, which each built the string by hand: the
// wiring row, the connection picker and the output page's pin row. This header exists so a resource
// looks the same everywhere it appears.
inline std::string terminal(const std::string& pin, const std::string& shellName) {
    const auto dash = pin.find('-');
    if (dash == std::string::npos) return pin;
    std::string shown = pin.substr(0, dash);
    if (!shellName.empty()) {
        std::string up = shellName;
        for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        shown += " (" + up + ")";
    }
    const std::string term = pin.substr(dash + 1);
    if (!term.empty()) shown += " " + term;
    return shown;
}

// Contrasting ink for reverse-video text on a shell/wire colour (light backgrounds -> black).
inline const RGBA& ink(const RGBA& bg) {
    static const RGBA kBlack{ 0, 0, 0, 255 }, kWhite{ 255, 255, 255, 255 };
    return (299 * bg[0] + 587 * bg[1] + 114 * bg[2]) / 1000 > 140 ? kBlack : kWhite;
}

// Draw a resource's wire as a bar of insulation in the base colour with an optional stripe band — no
// terminal glyph (the bare-conductor stub out the end read as clutter in a list).
// (buf is a jf::JPrimitiveBuffer; templated to avoid pulling the graphics header into this palette.)
template <class Buf>
inline void drawWire(Buf& buf, float x, float y, float barW, const std::string& code) {
    std::string base, stripe;
    split(code, base, stripe);
    const RGBA baseC = wire(base);
    buf.pushRectangle(x, y - 4.f, barW, 9.f, baseC.data(), 2.f);
    if (!stripe.empty()) {
        const RGBA stripeC = wire(stripe);
        buf.pushRectangle(x + barW * 0.60f, y - 4.f, barW * 0.22f, 9.f, stripeC.data());
    }
}
}  // namespace wirecol

// EVERY WAY OF WRITING A WIDGET'S ADDRESS NAMES THE SAME WIDGET.
//
// The picker yields "[@<uid>.units]". A hand-typed one may be "@<uid>.units", "<uid>.units", or just
// the uid. All four name it, and a caption pointed at one of them must resolve.
//
// The first version stripped "@" BEFORE "[", so a bracketed address kept its sigil: "[@uid]" became
// "@uid", the uid lookup found nothing, and the caption fell back to its text with nothing on screen
// to say why. Silent, and indistinguishable from "the feature does not work".
//
//   cmake --build build --target widget_addr_test && ./build/widget_addr_test

#include "../src/surface/PanelLibrary.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(const std::string& in, const std::string& want) {
    const std::string got = widgetsigil::normalizeAddr(in);
    const bool ok = (got == want);
    std::printf("[addr] %-42s -> %-40s %s\n", ("\"" + in + "\"").c_str(), ("\"" + got + "\"").c_str(),
                ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    const std::string U = "76e7726c-f7bd-419c-884c-2e15a55b71a1";

    ck(U,                    U);   // bare
    ck("@" + U,              U);   // sigil
    ck(U + ".units",         U);   // suffix
    ck("@" + U + ".units",   U);   // sigil + suffix
    ck("[@" + U + "]",       U);   // bracketed sigil
    ck("[@" + U + ".units]", U);   // bracketed sigil + suffix — what the picker produces
    ck("[" + U + "]",        U);   // bracketed, no sigil
    ck("  [@" + U + ".units]  ", U);   // and surrounding space, wherever it came from

    // Not an address: left alone rather than mangled into something that might accidentally resolve.
    ck("",           "");
    ck("value > 10", "value > 10");

    std::printf("[addr] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

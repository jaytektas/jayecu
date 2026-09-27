// textwidth — the ruler check_all.py never had.
//
// A studio caption is hard-clipped to its own widget box (CanvasWidget::paint pushes a clip rect and
// LabelWidget draws its glyphs with no maxWidth of their own), so a caption wider than its box loses the
// end of the word with nothing on screen to say so, and a wrapped one taller than its box loses its first
// and last rows. check_all.py measures BOXES against boxes; nothing measured TEXT against its box, because
// the authoring side is Python and has no font.
//
// This hands it the font — the app's own. Same JFontEngine, same atlas size, same glyph advances that
// JTextHelper::measureWidth sums at paint time. It prints the atlas metrics and the whole advance table,
// so the caller can measure any string (and any substring, which is what wrapping needs) exactly as the
// widget does:
//
//     lineHeight <lh>
//     ascent <a>
//     fallback <w>          # what an absent glyph advances (atlas.ascent * 0.35)
//     g <codepoint> <advanceX>
//     ...
//
//   textwidth <font.ttf|-> <basePx>        ('-' = the framework's auto-detected system face)
//
// Built on demand by tools/layout/text_fit.py, which is the only caller.

#include <j/core/JTextHelper.h>
#include <j/graphics/FontEngine.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: textwidth <font.ttf|-> <basePx>\n"); return 2; }
    const std::string path = argv[1];
    const float px = std::strtof(argv[2], nullptr);

    jf::JFontEngine fe;
    // An explicit face, or the framework's own auto-detected one when the profile has no override — the
    // same two branches main.cpp takes when it sets the application font.
    if (!(path == "-" ? fe.loadSystemFont() : fe.loadFromFile(path))) {
        std::fprintf(stderr, "textwidth: cannot load font %s\n", path.c_str());
        return 1;
    }
    const jf::JFontAtlas atlas = fe.buildAtlas(px);
    if (!atlas.valid) { std::fprintf(stderr, "textwidth: atlas invalid\n"); return 1; }

    std::printf("lineHeight %.6f\n", atlas.lineHeight);
    std::printf("ascent %.6f\n", atlas.ascent);
    std::printf("fallback %.6f\n", atlas.ascent * 0.35f);   // JTextHelper::measureWidth's unknown-glyph step
    for (const auto& [cp, g] : atlas.glyphs)
        std::printf("g %u %.6f\n", cp, g.advanceX);
    return 0;
}

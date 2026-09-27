// A LABEL WRAPS TO ITS BOX.
//
// The caption used to break only where the author typed '\n', so text longer than its widget simply ran
// out of it — off the side of the panel it was laid on. Wrap Text breaks it to the box instead, at word
// boundaries, and inside a word when a single word is wider than the box (the alternative being the very
// overflow wrapping exists to stop).
//
// The lines carry their own byte OFFSETS because the caret and selection maths reads them, and what
// separates two lines is no longer always one byte: an authored '\n' and the space a wrap eats are one,
// a break inside a long word is none. Deriving it as start+len+1 was right only while '\n' was the only
// way to break.
//
// Measured through JTextHelper, which with no atlas loaded answers a flat 8 px per byte — so these cases
// are exact arithmetic rather than a guess about a font: at avail = 80 a line holds ten bytes.
//
//   cmake --build build --target label_wrap_test && ./build/label_wrap_test

#include "../src/surface/widgets/LabelWidget.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("[label-wrap] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string show(const std::vector<LabelWidget::Line>& ls) {
    std::string s;
    for (const auto& l : ls) s += "[" + std::to_string(l.start) + ":" + l.text + "]";
    return s;
}
// Every line must be findable at the offset it claims — that is what the caret maths depends on.
static bool offsetsAgree(const std::string& text, const std::vector<LabelWidget::Line>& ls) {
    for (const auto& l : ls)
        if (l.start > text.size() || text.compare(l.start, l.text.size(), l.text) != 0) return false;
    return true;
}
static bool allFit(const std::vector<LabelWidget::Line>& ls, float avail) {
    for (const auto& l : ls)
        if (jf::JTextHelper::measureWidthScaled(l.text, 1.f) > avail && l.text.size() > 1) return false;
    return true;
}

int main() {
    check(!jf::JTextHelper::hasAtlas(), "no atlas: measurement is the flat 8 px/byte fallback");

    // ---- wrapping off: exactly the old behaviour, one line per authored paragraph ----------------
    {
        const std::string t = "hello world foo";
        const auto ls = LabelWidget::layoutLines(t, /*wrap=*/false, 1.f, 80.f);
        check(ls.size() == 1 && ls[0].text == t && ls[0].start == 0,
              "wrap off leaves a long caption on one line", show(ls));
        const auto two = LabelWidget::layoutLines("ab\ncd", false, 1.f, 80.f);
        check(two.size() == 2 && two[0].start == 0 && two[1].start == 3 && two[1].text == "cd",
              "…and an authored newline still breaks, at the right offset", show(two));
    }

    // ---- wrapping on, but the text already fits --------------------------------------------------
    {
        const auto ls = LabelWidget::layoutLines("short", true, 1.f, 80.f);
        check(ls.size() == 1 && ls[0].text == "short", "a caption that fits is not broken up", show(ls));
    }

    // ---- wrapping on: break at the last word that fits -------------------------------------------
    {
        const std::string t = "hello world foo";     // 8px/byte, 80 avail -> ten bytes a line
        const auto ls = LabelWidget::layoutLines(t, true, 1.f, 80.f);
        check(ls.size() == 2 && ls[0].text == "hello" && ls[1].text == "world foo",
              "it breaks at a space, taking as many whole words as fit", show(ls));
        check(ls.size() == 2 && ls[0].start == 0 && ls[1].start == 6,
              "…and the space it broke on is spent on the break", show(ls));
        check(offsetsAgree(t, ls), "…leaving every offset pointing at its own line");
        check(allFit(ls, 80.f), "…with no line wider than the box");
    }

    // ---- a word too long for the box: broken inside rather than left to overflow ------------------
    {
        const std::string t = "abcdefghijklmnop";
        const auto ls = LabelWidget::layoutLines(t, true, 1.f, 80.f);
        check(ls.size() == 2 && ls[0].text == "abcdefghij" && ls[1].text == "klmnop",
              "a single over-long word breaks inside itself", show(ls));
        check(ls.size() == 2 && ls[1].start == 10, "…consuming no byte at the break", show(ls));
        check(offsetsAgree(t, ls), "…and its offsets still agree");
        check(allFit(ls, 80.f), "…with no line wider than the box");
    }

    // ---- a break inside a word never splits a UTF-8 sequence --------------------------------------
    {
        std::string t;                                 // 8 x "°" = 16 bytes, no spaces
        for (int i = 0; i < 8; ++i) t += "\xC2\xB0";
        const auto ls = LabelWidget::layoutLines(t, true, 1.f, 80.f);
        bool whole = true;
        for (const auto& l : ls) {
            if (l.text.empty()) { whole = false; break; }
            if ((static_cast<unsigned char>(l.text.front()) & 0xC0) == 0x80) whole = false;   // starts mid-glyph
            if (l.text.size() % 2 != 0) whole = false;                                        // ends mid-glyph
        }
        check(whole && ls.size() >= 2, "a multi-byte glyph is never cut in half", show(ls));
        check(offsetsAgree(t, ls), "…and the halves still address the original bytes");
    }

    // ---- authored newlines survive wrapping, blank paragraphs keep their row ----------------------
    {
        const std::string t = "one two three\n\nx";
        const auto ls = LabelWidget::layoutLines(t, true, 1.f, 80.f);
        bool blank = false;
        for (const auto& l : ls) if (l.text.empty()) blank = true;
        check(blank, "an empty paragraph still occupies a line", show(ls));
        check(ls.back().text == "x" && ls.back().start == t.size() - 1,
              "…and the paragraph after it starts where it really does", show(ls));
        check(offsetsAgree(t, ls), "…every offset agreeing across both kinds of break");
        check(allFit(ls, 80.f), "…and nothing wider than the box");
    }

    // ---- degenerate boxes wrap nothing rather than emitting empty lines forever -------------------
    {
        const auto zero = LabelWidget::layoutLines("hello world", true, 1.f, 0.f);
        check(zero.size() == 1, "an unmeasurable box wraps nothing", show(zero));
        const auto tiny = LabelWidget::layoutLines("hello", true, 1.f, 1.f);
        check(tiny.size() == 5, "a box narrower than one glyph still terminates, a byte a line", show(tiny));
    }

    // ---- the height a wrapped caption asks for, which is what the editor grows instead of the width ---
    // No atlas: lineHeight() is 16 and a byte is 8 wide, so a 106-wide box holds ten bytes a line
    // (106 - 2*kTextPad = 100) and each row is 16 tall, over naturalSize's 10 of slack.
    {
        const float one = LabelWidget::wrappedHeight("short", "", 106.f);
        const float two = LabelWidget::wrappedHeight("hello world foo", "", 106.f);
        check(two > one, "a caption that wraps to more rows asks for more height",
              std::to_string(one) + " -> " + std::to_string(two));
        check(std::fabs((two - one) - 16.f) < 0.51f, "…one line height per extra row",
              std::to_string(two - one));
        const float wide = LabelWidget::wrappedHeight("hello world foo", "", 400.f);
        check(std::fabs(wide - one) < 0.51f, "…and a box wide enough to hold it needs no extra row",
              std::to_string(wide));
        check(LabelWidget::wrappedHeight("", "", 106.f) > 0.f, "an empty caption still has a height");
    }

    std::printf("[label-wrap] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

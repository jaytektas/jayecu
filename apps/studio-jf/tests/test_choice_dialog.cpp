// The confirmation dialog's TEXT, which is the whole reason it exists.
//
// The studio asked its overwrite questions through a list picker: the answers as rows inside a
// scrolling list box, OK and Cancel underneath, and the thing being decided nowhere in the dialog —
// it went past as a status toast instead. Replacing that put the question in the body, and the body
// then has to survive being laid out.
//
// It did not, on screen: a window sized for three lines showed one, ending mid-sentence. The cause of
// THAT is not yet established — measuring without a font atlas was the obvious suspect and is not it,
// since this test runs without one and wraps correctly. What is established, and what this pins, is
// the invariant the fix rests on: the lines are computed ONCE, and the window is sized from the same
// list that is drawn. Height and content cannot disagree, whatever the measurement does.
//
//   cmake --build build --target choice_dialog_test && ./build/choice_dialog_test
#include "ui/ChoiceDialog.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    // The question the Reset button actually asks, verbatim.
    const std::string body =
        "This forgets 84 learned cell(s) of Long Term Fuel Trim, the largest of them +11.40%.\n\n"
        "The engine has to learn them again from scratch. If they are worth keeping, Apply to Base "
        "Table first - that folds them into the map and resets them in one step.";
    const float maxW = static_cast<float>(ChoiceDialog::kW) - 32.f;

    std::printf("  (hasAtlas=%d, measureWidth(\"Mmmm\")=%.1f)\n",
                int(jf::JTextHelper::hasAtlas()), jf::JTextHelper::measureWidth("Mmmm"));

    // A WIDTH IS NEVER ZERO FOR TEXT THAT EXISTS. This is the rule both the wrap and the button sizes
    // rest on. In the app, measureWidth returns 0 while the dialog is being built — and nothing
    // downstream can tell that from "the string is empty", which is how one clipped line and a button
    // stuck at its 96 px minimum came from the same moment.
    ck(ChoiceDialog::textW("Apply to Base Table") > 0.f, "text has a width even when nothing can measure it");
    ck(ChoiceDialog::textW("") == 0.f, "...and empty text does not");
    ck(ChoiceDialog::textW("Apply to Base Table") > ChoiceDialog::textW("Reset"),
       "...and a longer label is wider than a shorter one");

    const auto lines = ChoiceDialog::wrapBody(body, maxW);
    ck(lines.size() > 1, "the question wraps rather than becoming one clipped line",
       std::to_string(lines.size()) + " line(s)");

    // NOTHING IS LOST. Reassembling the lines has to give the words back, in order — a wrap that drops
    // a word is the same failure as a clip, only harder to see.
    std::string joined;
    for (const std::string& l : lines)
        if (!l.empty()) { if (!joined.empty()) joined += ' '; joined += l; }
    std::string flat;
    for (char c : body) flat += (c == '\n') ? ' ' : c;
    while (flat.find("  ") != std::string::npos) flat.erase(flat.find("  "), 1);
    ck(joined == flat, "every word survives the wrap",
       joined == flat ? "" : ("got: " + joined.substr(0, 80) + "..."));

    bool blank = false;
    for (const std::string& l : lines) if (l.empty()) blank = true;
    ck(blank, "the paragraph break is kept as a line, not joined away");

    bool tooWide = false;
    for (const std::string& l : lines)
        if (ChoiceDialog::textW(l) > maxW) tooWide = true;
    ck(!tooWide, "no line is wider than the box it is drawn in");

    // THE INVARIANT THAT BROKE: the height came from one estimate and the text from a different one.
    const uint32_t h = ChoiceDialog::heightFor(body);
    const uint32_t want = static_cast<uint32_t>(ChoiceDialog::hdrH() + 16.f
                        + static_cast<float>(lines.size()) * ChoiceDialog::lineH()
                        + 20.f + ChoiceDialog::kBtnH() + 16.f);
    ck(h == want, "the window is sized from those same lines",
       std::to_string(h) + " vs " + std::to_string(want));

    // A one-line question still gets a sane box rather than a sliver.
    const auto few = ChoiceDialog::wrapBody("Short.", maxW);
    ck(few.size() == 1, "a short question is one line");
    ck(ChoiceDialog::heightFor("Short.") < h, "...and a smaller dialog than a long one");

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}

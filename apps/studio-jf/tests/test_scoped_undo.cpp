// test_scoped_undo.cpp — the < > arrows on a dialog walk THAT dialog's changes.
//
// Every dialog carries undo/redo arrows, and what they step through is the work you did on that
// dialog. A single global history would have the arrows on the page in front of you revert an edit
// made somewhere else entirely — a cell on another table, a field two pages away — which is not an undo,
// it is a change you did not ask for and cannot see. So every edit is filed under the page it was made
// on (Cache::setEditScope, called when the navigation switches pages), and each page walks its own file.
//
// Also pinned here: a re-read that lands the same bytes is NOT a new document. The burn button re-reads,
// and clearing history on that emptied the arrows a moment after the user had used them.
//
//   cmake --build build --target scoped_undo_test && ./build/scoped_undo_test

#include "model/MetaModel.h"
#include "model/Cache.h"

#include <cstdio>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static const char* kPageA = "Setup/Page A";
static const char* kPageB = "Setup/Page B";

int main()
{
    MetaModel meta;
    if (!meta.loadFile(FIXTURE_META)) { std::printf("could not load fixture: %s\n", FIXTURE_META); return 2; }

    Cache& c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(std::vector<uint8_t>(static_cast<size_t>(meta.configSize()), 0));

    // Two edits, one per page. "gain" is page A's, "enabled" is page B's.
    c.setEditScope(kPageA);
    c.setConfigValue("fixture.gain", 5.0);
    c.setEditScope(kPageB);
    c.setConfigValue("fixture.enabled", 1.0);

    CHECK(c.value("fixture.gain") == 5.0);
    CHECK(c.value("fixture.enabled") == 1.0);
    CHECK(c.canUndoScope(kPageA));
    CHECK(c.canUndoScope(kPageB));
    CHECK(!c.canRedoScope(kPageA));

    // Page A's arrow reverts PAGE A's edit — and leaves page B's alone. This is the whole point.
    c.undoScope(kPageA);
    std::printf("  after A undo: gain=%g enabled=%g\n", c.value("fixture.gain"), c.value("fixture.enabled"));
    CHECK(c.value("fixture.gain") == 0.0);
    CHECK(c.value("fixture.enabled") == 1.0);      // the other page's edit is untouched
    CHECK(!c.canUndoScope(kPageA));
    CHECK(c.canRedoScope(kPageA));
    CHECK(c.canUndoScope(kPageB));                 // …and its history is still intact

    // Redo puts it back.
    c.redoScope(kPageA);
    CHECK(c.value("fixture.gain") == 5.0);
    CHECK(c.canUndoScope(kPageA));
    CHECK(!c.canRedoScope(kPageA));

    // A page with no history does nothing at all rather than reaching for someone else's step.
    const double before = c.value("fixture.gain");
    c.undoScope("Setup/Never Edited");
    CHECK(c.value("fixture.gain") == before);

    // Walk back, then make a NEW edit: the redo tail is discarded, as any redo tail is.
    c.undoScope(kPageA);
    CHECK(c.canRedoScope(kPageA));
    c.setEditScope(kPageA);
    c.setConfigValue("fixture.gain", 7.0);
    CHECK(!c.canRedoScope(kPageA));                // the branch we walked back from is gone
    CHECK(c.canUndoScope(kPageA));
    c.undoScope(kPageA);
    CHECK(c.value("fixture.gain") == 0.0);         // back to before that new edit
    std::printf("  new edit after an undo discards the redo tail\n");

    // A re-read that lands IDENTICAL bytes keeps the arrows alive (the burn path re-reads).
    c.setEditScope(kPageB);
    c.setConfigValue("fixture.enabled", 0.0);
    CHECK(c.canUndoScope(kPageB));
    c.setConfigImage(c.configImage());             // same bytes: a confirmation, not a new document
    CHECK(c.canUndoScope(kPageB));
    std::printf("  a no-op re-read keeps the page's history\n");

    // A re-read with DIFFERENT bytes is a new document, and history resets.
    std::vector<uint8_t> other = c.configImage();
    other[0] = static_cast<uint8_t>(other[0] + 1);
    c.setConfigImage(other);
    CHECK(!c.canUndoScope(kPageA));
    CHECK(!c.canUndoScope(kPageB));
    std::printf("  a genuinely different image resets it\n");

    std::printf(g_fails ? "scoped undo: FAILED (%d)\n" : "scoped undo: OK\n", g_fails);
    return g_fails ? 1 : 0;
}

#pragma once

// LearnedOps — the two things you ever do with a learned correction, by table PATH.
//
// A trim is a correction on a map, and it has exactly two ends: fold it into the map it corrects
// (Apply to Base), or throw it away (Reset). Both were table-menu items, which put them behind a
// right-click on a grid — the wrong place twice over: they are not operations on "a table" the way
// Smooth or Linearise are, they are the workflow the learned surface exists for, and a menu that
// offers them on every map teaches people the menu lies.
//
// So they live here, addressed by path, and the two callers that want them — the table context menu
// (which resolves the selection to a path) and a button on the page that names the table outright —
// call the same code. Neither is the owner.
//
// STUDIO-SIDE, ENTIRELY. There is no firmware command for either. The learned region is reachable
// through the ordinary config read/write block protocol, so folding a trim into its base map is a
// read, some arithmetic and a write — all of it here, in the same edit transaction as any other
// table edit, undoable the same way, and reaching the ECU's RAM the same way. Nothing is permanent
// until the tune is burned.

#include <functional>
#include <string>

namespace learned {

// WHAT AN OPERATION WOULD DO, before it does it. Both of these destroy something — one rewrites a
// fuel map, the other discards what the engine spent a drive learning — so a caller has to be able to
// say what is about to happen in numbers rather than ask "are you sure?" about a verb.
struct Plan {
    bool        possible = false;   // this path is a learned trim with a base map to fold into
    int         cells    = 0;       // non-neutral cells that would take part
    double      worst    = 0.0;     // the largest of them, in the trim's own units
    std::string trimName, baseName; // what to call them in the question
    std::string detail;             // the whole question, already phrased
};

Plan planApplyToBase(const std::string& tablePath);
Plan planReset(const std::string& tablePath);

// Do it. One edit transaction, so an undo takes back the whole operation rather than a cell of it.
// No confirmation here: asking is the caller's job, because only the caller has a window.
void applyToBase(const std::string& tablePath);
void resetToZero(const std::string& tablePath);

// A LEARNED TRIM, OR AN ORDINARY MAP. The relationship is data — the schema's `apply_to` — so this is
// the one question every caller asks before offering either operation.
bool isLearnedTrim(const std::string& tablePath);

}  // namespace learned

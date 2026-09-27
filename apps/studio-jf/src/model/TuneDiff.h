#pragma once

// TuneDiff — what is different between two tunes, said in the words the pages use.
//
// The question this answers is asked at exactly one moment and it is the worst moment to be vague:
// the studio has connected, the ECU's config does not match the saved tune, and somebody has to choose
// which one survives. Until now that choice was made blind — two buttons, 141 KB of config behind
// them, and no way to tell whether the ECU had one changed idle target or a completely different
// engine. The answer is to show the two side by side, page by page, differences highlighted, and
// then ask; this is the half of that which knows what differs.
//
// GROUPED BY PAGE, because that is the unit a tuner thinks in and the unit the answer is read in. A
// difference in `idle.target_rpm` means nothing until it is "Idle Control ▸ Target RPM"; the page it
// lives on is the context that makes the number a fact about the engine.
//
// COMPARED BY MEANING, NOT BY BYTES. A raw memcmp reports every padding byte and every unmapped
// region, so it says "differs" for tunes that are identical in every value a page can show. This walks
// the bindings the definition declares and compares the values they resolve to, which is the same set
// of things the two tunes could ever disagree about visibly.

#include "MetaModel.h"

#include <cstdint>
#include <string>
#include <vector>

class PanelLibrary;

namespace tunediff {

// One setting that differs, in display units.
struct Item {
    std::string path;      // "idle.target_rpm" — the binding
    std::string label;     // what the page calls it
    double      a = 0.0;   // the left-hand tune (the studio's)
    double      b = 0.0;   // the right-hand tune (the ECU's)
    bool        table = false;   // a whole map differs; `cells` says how much of it
    int         cells = 0;       // for a table: how many cells differ
    // HOW MANY DECIMALS THE FIELD IS READ AT, from its own scale. Without it a report prints 4200 rpm
    // as "4.2e+03", which is not a number anybody tuning an engine reads — and printing everything to
    // three decimals instead would turn a count of cylinders into "6.000".
    int         decimals = 0;
    // TEXT, when the field is text. A VIN or an engine name is not a number, and comparing it as one
    // reads four of its bytes and calls the rest equal — which is how two tunes could differ in a name
    // and the report find nothing to say about it.
    bool        isText = false;
    std::string textA, textB;
    // WHERE THE FIRMWARE FILES IT, from the meta's own navigation tree ("Configuration > Engine"). Set
    // for a setting that no page of the document shows — a new setting the dashboard has not placed yet
    // still has a place in the firmware, and that is where a tuner would go looking for it.
    std::string where;
};

// One page carrying differences, in the order the navigation tree lists it.
struct Page {
    std::string       node;      // "Configuration/Idle Control/Target RPM"
    std::string       title;     // the leaf, which is what the dialog's header shows
    std::vector<Item> items;
};

struct Report {
    std::vector<Page> pages;
    int  settings = 0;           // total differing bindings across every page
    // Differences the document shows on NO page. Not a rounding error to be hidden: a tune can hold a
    // value no page binds, and reporting "no differences" over one would be a lie of exactly the kind
    // this exists to prevent.
    std::vector<Item> unpaged;
    bool empty() const { return pages.empty() && unpaged.empty(); }
};

// Compare two config images through the definition that describes them, grouping by the page each
// binding appears on. `lib` may be null, in which case everything lands in `unpaged`.
// THE REPORT MUST BE ABLE TO EXPLAIN THE VERDICT. Whether two tunes differ is decided by comparing what
// TuneFile::serialise writes — five sections, every binding it knows. This walked a DIFFERENT set (config
// scalars and tables), so the two could disagree: the studio would announce that the ECU no longer
// matches and then have nothing to show for it, falling back to a bare "keep which one?" prompt. Anything
// serialise can see and this walk cannot is now picked up as well, by path, so an empty report means
// there is genuinely nothing to report.
Report compare(const MetaModel& meta, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               const PanelLibrary* lib);

// ---- WHAT A FIRMWARE UPDATE TAKES AWAY AND BRINGS ---------------------------------------------------
//
// The same report, asked of two DIFFERENT firmwares instead of two tunes. Before an update, a tuner should
// see what they have set that the new firmware no longer has, and what the new firmware adds that is
// waiting to be configured — on the pages they would find it on, highlighted, as the tune comparison
// does it. Each side is its own Report because each side has its own pages: the retired settings live on
// the document the tuner has now, the new ones on the new firmware's shipped dashboard.
struct FirmwareChanges {
    // In the old firmware and not the new, AND set by this tune (not at the old default) — a setting nobody
    // touched is not a loss worth showing. `a` is the tune's value. References the migration could not
    // resolve (a signal or option the new firmware dropped) are listed here too, unpaged.
    Report retired;
    // In the new firmware and not the old. `b` is the value it starts at (the new firmware's default).
    Report added;
    bool empty() const { return retired.empty() && added.empty(); }
};

// `tune` is the tune as the old firmware holds it; `migrated` the same tune moved onto the new layout.
// Either library may be null (everything then lands unpaged). `unresolved` is MigrationReport::unresolved.
FirmwareChanges compareFirmware(const MetaModel& oldMeta, const std::vector<uint8_t>& tune, const PanelLibrary* oldLib,
                                const MetaModel& newMeta, const std::vector<uint8_t>& migrated, const PanelLibrary* newLib,
                                const std::vector<std::string>& unresolved);

}  // namespace tunediff

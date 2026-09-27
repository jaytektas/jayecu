#pragma once

// FirmwareKits — the ECU firmware the studio has on hand, and which of it is newest for a board.
//
// A KIT is everything one firmware build needs, kept together because it only works together: the
// firmware image, the meta that describes it, and the dashboard drawn for it. Each kit is a folder
// with a kit.json saying what it is:
//
//     {"board": "jaytek_v1", "version": "0.2.0", "build": "de68118", "layout_hash": "6dc604ca",
//      "min_studio": "0.1.0",
//      "firmware": "jaytek_v1-0.2.0-firmware.bin", "meta": "jaytek_v1-0.2.0.meta",
//      "dashboard": "jaytek_v1-0.2.0.gui"}
//
// Kits come from two places, and the studio treats them the same:
//   SHIPPED    — <studio>/firmware/<kit>/, put there when the studio was built. Read-only.
//   DOWNLOADED — <data>/firmware/<kit>/, fetched from a newer release than the one this studio came
//                from, so a studio can be offered firmware newer than the kit it shipped with.
//
// A kit that needs a newer studio than this one (min_studio) is still listed, but is not the one
// offered — the studio could not read its meta.

#include <string>
#include <vector>

namespace fwkits {

struct Kit {
    std::string board, version, build, layoutHash, minStudio;
    std::string dir;                                  // the kit's folder
    std::string firmware, meta, dashboard;            // full paths to its files
    bool shipped = false;
};

// Every readable kit under `root` (one folder per kit). A folder without a valid kit.json, or whose
// kit.json names a file that is not there, is skipped: a half-copied kit is not a kit.
std::vector<Kit> scan(const std::string& root, bool shipped);

// The newest kit for `board` that this studio can use (min_studio <= studioVersion). On a version
// tie a downloaded kit wins over a shipped one — it was fetched later, so it is the more recent build.
// Returns false when there is none.
bool newestFor(const std::vector<Kit>& kits, const std::string& board, const std::string& studioVersion,
               Kit& out);

// Is `kitVersion` newer than the version the ECU reports?
bool isNewerThan(const std::string& kitVersion, const std::string& ecuVersion);

}  // namespace fwkits

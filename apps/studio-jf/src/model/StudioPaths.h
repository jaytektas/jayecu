#pragma once

// StudioPaths — where the studio's own files live, answered in ONE place.
//
// Eleven call sites used to build the same string by hand: getenv("HOME"), then the literal
// ".local/share/jayecu/jayecu Studio", then a subfolder. That is fine until one of them is wrong,
// and one of them always is: the studio writes ECU folders, the meta library, imported .ini files,
// sensor calibrations, datalogs and the trigger-wheel library into that root, and they only find
// each other because eleven separate literals happen to agree.
//
// It also cannot be right on both platforms at once. $HOME does not exist on Windows — there,
// getenv returns null and the fallback "." puts the whole installation wherever the process happened
// to start, so a studio launched from two different folders has two different libraries and neither
// can see the other's ECUs. Windows keeps per-user application data under %APPDATA%, so that is what
// dataRoot() answers there.
//
// The literal space in "jayecu Studio" is intentional on both: it is the established on-disk path,
// and changing it would orphan every ECU folder already written.

#include <filesystem>
#include <string>

namespace StudioPaths {

// The installation's data folder — the one everything else hangs off. Not created; ask for a
// subfolder instead, which is.
std::filesystem::path dataRoot();

// dataRoot()/sub, with the directories created. The return is a string because that is what nearly
// every caller wants to hand to a file API or a status message.
std::string dataDir(const std::string& sub);

// dataRoot()/name, with dataRoot() created — for the few things that are a file in the root itself
// rather than a folder (the trigger-wheel library).
std::string dataFile(const std::string& name);

// The persisted-settings file JSettings reads and writes. On POSIX this is the XDG config location,
// which is deliberately NOT the data root: geometry and unit preferences are not the user's tunes.
// Windows has one per-user location for both, so there it sits in the data root.
std::filesystem::path settingsFile();

}  // namespace StudioPaths

#pragma once

// Resources — the files the studio SHIPS WITH, found wherever it was started from.
//
// These are not the user's data (that is StudioPaths) and not anything the studio writes. They are
// the artwork and the stylesheet that are part of the build: they sit beside the executable, put
// there by the build, and they have to be found when the studio is launched by its full path, from
// a shortcut, or by a shell whose working directory is somewhere else entirely.
//
// Every one of them used to be opened by a bare relative name, which is a bet that the working
// directory is the source tree. Launched any other way the bet loses silently — no missing-file
// message, just a blank About box and, in the stylesheet's case, an application wearing the
// framework's default theme instead of its own.

#include <string>

namespace resources {

// The shipped file's path: beside the executable, then in an assets/ folder beside it, then the
// bare name as a last resort so a developer's working directory still works. The name is returned
// even when nothing exists — the caller's own open() reports that better than a guess here can.
std::string path(const std::string& file);

// The folder the executable is in, with a trailing separator; "" if it cannot be found. For shipped
// FOLDERS (the firmware kits), which path() cannot answer because it looks for a file.
std::string exeDir();

}  // namespace resources

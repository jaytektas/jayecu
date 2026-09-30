#pragma once

// THE STUDIO'S OWN VERSION. Set in CMakeLists.txt — project(... VERSION x.y.z), or STUDIO_VERSION_OVERRIDE for
// a beta ("0.3.8-beta.1") — never here. The update check compares it against the tag of the newest release,
// so a release tagged v0.2.0 must be built from a tree that says 0.2.0, or every user of it is told that
// the version they already have is an update.
//
// DEFINED IN ONE GENERATED LINE (StudioVersion.cpp.in), NOT HERE. It used to be a #define in a generated
// header, and main.cpp includes it: every beta changes the version, so every beta recompiled main.cpp from
// scratch — about 80 s, twice (Linux and Windows) — for one string. Now a version change recompiles that one
// line and relinks.
extern const char* const kStudioVersion;
#define STUDIO_VERSION kStudioVersion

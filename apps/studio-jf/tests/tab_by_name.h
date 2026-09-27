#pragma once

// ADDRESSING A TAB BY NAME, FOR TESTS ONLY.
//
// SurfaceTabs used to have reopenByName()/closedNames() and lost them on purpose: View ▸ New Surface
// Tab makes every new surface "Surface", so a name is not an identity — the by-name loop took the first
// match, and two rows in the picker opened the same surface while the other was unreachable. The app
// addresses a surface by its POOL INDEX now (surfaces() → SurfaceRef{index, name, open} → reopenAt).
//
// A test is the one caller for which a name IS an identity, because the test chose the names and knows
// they are distinct. So the lookup lives here, in the tests, rather than back in the class where the
// app could reach it again.

#include "surface/SurfaceTabs.h"

#include <string>
#include <vector>

// Bring the named surface to the front. Returns false if no surface has that name — a test that
// mistypes one would otherwise assert against whichever tab happened to be in front.
inline bool reopenNamed(SurfaceTabs& tabs, const std::string& name) {
    for (const auto& s : tabs.surfaces())
        if (s.name == name) { tabs.reopenAt(s.index); return true; }
    return false;
}

// The surfaces that exist but are not tabs right now.
inline std::vector<std::string> closedSurfaceNames(SurfaceTabs& tabs) {
    std::vector<std::string> out;
    for (const auto& s : tabs.surfaces())
        if (!s.open) out.push_back(s.name);
    return out;
}

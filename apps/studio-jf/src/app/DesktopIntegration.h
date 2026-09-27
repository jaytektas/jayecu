#pragma once

// DesktopIntegration — an AppImage puts itself in the desktop's application menu, with its icon.
//
// An AppImage is one file the user downloads and runs. Nothing installs it: the desktop entry and
// icon inside it are for tools that unpack it, and a plain launch registers nothing. So the studio ran
// with no menu entry, no icon in the launcher or the dock, and a window the shell could not match to
// any application — it showed a generic cog.
//
// THE USER DECIDES. On startup (see main.cpp) the studio looks for its own menu entry. Present, it says
// nothing — at most it quietly re-points the entry at this AppImage if the file has been moved. Absent,
// it asks once; declining sets "desktop.skipMenuCheck", which Preferences ▸ Updates shows as a tick box,
// so a user who said no is never asked again unless they untick it. Per-user (~/.local/share), no root,
// and nothing at all outside an AppImage: a build directory or a Windows install has its own
// arrangements.

#include <string>

namespace desktop {

enum class MenuState {
    NotAppImage,    // not running from an AppImage: nothing to offer
    Installed,      // our menu entry exists (it may point at an old path; refresh() fixes that silently)
    NotInstalled,   // no menu entry: ask, unless the user has said not to
};

MenuState menuState();

// Write (or refresh) the menu entry and icon for THIS AppImage. Writes only what differs; true if the
// entry is in place afterwards.
bool install();

// The setting that stops the question: true once the user has declined.
constexpr const char* kSkipSetting = "desktop.skipMenuCheck";

}  // namespace desktop

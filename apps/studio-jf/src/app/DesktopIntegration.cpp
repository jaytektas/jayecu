// DesktopIntegration — see the header.

#include "DesktopIntegration.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream s; s << f.rdbuf();
    return s.str();
}

// Write only when the content differs, so an ordinary launch touches nothing. True if it wrote.
bool writeIfChanged(const fs::path& p, const std::string& content) {
    if (fs::exists(p) && readAll(p) == content) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

// The Desktop Entry spec's quoting for an Exec argument: wrap in double quotes, backslash-escape
// " ` $ \ inside, and double any % (field codes). A path with a space in it is common enough —
// "~/Downloads/jayecu studio.AppImage" — that an unquoted Exec would simply not launch.
std::string execQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
        if (c == '%') out += '%';
        out += c;
    }
    return out + "\"";
}

// $XDG_DATA_HOME, else ~/.local/share.
fs::path dataHome() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return x;
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".local" / "share";
    return {};
}

// Run a cache refresher if it is installed; its absence or failure changes nothing that matters.
void quiet(const std::string& cmd) {
    const int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
    (void)rc;
}

}  // namespace

static fs::path entryPath() {
    const fs::path home = dataHome();
    return home.empty() ? fs::path() : home / "applications" / "jayecu-studio.desktop";
}

desktop::MenuState desktop::menuState() {
#if defined(_WIN32)
    return MenuState::NotAppImage;
#else
    const char* appimage = std::getenv("APPIMAGE");
    if (!appimage || !*appimage) return MenuState::NotAppImage;
    const fs::path e = entryPath();
    std::error_code ec;
    return (!e.empty() && fs::exists(e, ec)) ? MenuState::Installed : MenuState::NotInstalled;
#endif
}

bool desktop::install() {
#if defined(_WIN32)
    return false;
#else
    const char* appimage = std::getenv("APPIMAGE");
    if (!appimage || !*appimage) return false;
    const fs::path home = dataHome();
    if (home.empty()) return false;
    // THE ICON, from inside the mounted image: the AppImage runtime sets $APPDIR to its mount point,
    // and make_appimage.sh puts the 256 px icon at its root.
    const char* appdir = std::getenv("APPDIR");
    bool iconChanged = false;
    if (appdir && *appdir) {
        const std::string icon = readAll(fs::path(appdir) / "jayecu-studio.png");
        if (!icon.empty())
            iconChanged = writeIfChanged(home / "icons" / "hicolor" / "256x256" / "apps" / "jayecu-studio.png", icon);
    }

    // THE MENU ENTRY. Exec is the AppImage itself, not the executable inside it: the mount point
    // changes on every launch, and the AppImage path is what the self-updater replaces in place.
    // StartupWMClass is the window's class, which the framework sets to the executable's name.
    const std::string entry =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=jayECU Studio\n"
        "Comment=Tune and configure jayECU engine controllers\n"
        "Exec=" + execQuote(appimage) + "\n"
        "Icon=jayecu-studio\n"
        "Terminal=false\n"
        "Categories=Development;\n"
        "Keywords=ECU;tuning;engine;\n"
        "StartupWMClass=studio\n"
        "X-AppImage-Path=" + std::string(appimage) + "\n";
    const fs::path apps = home / "applications";
    const bool entryChanged = writeIfChanged(apps / "jayecu-studio.desktop", entry);

    // Tell the desktop, when something actually changed. Most shells watch these folders anyway; the
    // explicit refresh covers the ones that only read their caches.
    if (entryChanged) quiet("update-desktop-database -q \"" + apps.string() + "\"");
    if (iconChanged) {
        const fs::path theme = home / "icons" / "hicolor";
        std::error_code ec;
        fs::last_write_time(theme, fs::file_time_type::clock::now(), ec);   // what icon caches check
        quiet("gtk-update-icon-cache -q -t -f \"" + theme.string() + "\"");
    }
    std::error_code ec;
    return fs::exists(apps / "jayecu-studio.desktop", ec);
#endif
}

// StudioPaths — see the header for why eleven hand-built copies of one path became one function.

#include "StudioPaths.h"

#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// The user's home, by whatever the platform calls it. Returns empty when there is nothing to go on,
// which the caller turns into the current directory — a bad location, but a working one, and the
// alternative is refusing to start.
fs::path userHome() {
    if (const char* s = std::getenv(
#if defined(_WIN32)
            "APPDATA"       // ...\AppData\Roaming — already the per-user application-data folder
#else
            "HOME"
#endif
        ))
        if (*s) return fs::path(s);

#if defined(_WIN32)
    // APPDATA is set for every interactive logon, but a service or a stripped environment may not
    // have it. USERPROFILE is the next best thing, and Roaming hangs off it by definition.
    if (const char* up = std::getenv("USERPROFILE"))
        if (*up) return fs::path(up) / "AppData" / "Roaming";
#endif
    return {};
}

}  // namespace

fs::path StudioPaths::dataRoot() {
    const fs::path home = userHome();
    const fs::path base = home.empty() ? fs::path(".") : home;
#if defined(_WIN32)
    // %APPDATA% IS the application-data root, so the POSIX ".local/share" segment would only be
    // repeating what the variable already says.
    return base / "jayecu" / "jayecu Studio";
#else
    return base / ".local" / "share" / "jayecu" / "jayecu Studio";
#endif
}

std::string StudioPaths::dataDir(const std::string& sub) {
    const fs::path d = sub.empty() ? dataRoot() : dataRoot() / sub;
    std::error_code ec;
    fs::create_directories(d, ec);   // already there is not an error, and neither is a read-only
    return d.string();               // volume here — the open that follows reports that properly
}

std::string StudioPaths::dataFile(const std::string& name) {
    std::error_code ec;
    fs::create_directories(dataRoot(), ec);
    return (dataRoot() / name).string();
}

fs::path StudioPaths::settingsFile() {
#if defined(_WIN32)
    return dataRoot() / "settings.json";
#else
    const fs::path home = userHome();
    return (home.empty() ? fs::path(".") : home) / ".config" / "jayecu-studio" / "settings.json";
#endif
}

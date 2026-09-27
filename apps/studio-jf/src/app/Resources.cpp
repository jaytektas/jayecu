// Resources — see the header. Finding one's own executable is the platform question here.

#include "Resources.h"

#include <cstdio>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace {

// The directory the running executable sits in, with its trailing separator; empty when the platform
// will not say.
std::string exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};                  // truncated is not a path, it is a guess
    const std::string exe(buf, n);
    const size_t slash = exe.find_last_of("\\/");            // the API gives backslashes; accept either
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    const std::string exe(buf, static_cast<size_t>(n));
    const size_t slash = exe.rfind('/');
#endif
    if (slash == std::string::npos) return {};
    return exe.substr(0, slash + 1);
}

// Readable, asked the way every platform answers the same: by opening it.
bool readable(const std::string& p) {
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

}  // namespace

std::string resources::exeDir() { return ::exeDir(); }

std::string resources::path(const std::string& file) {
    const std::string dir = exeDir();
    if (!dir.empty()) {
        const std::string beside = dir + file;
        if (readable(beside)) return beside;
        // A source tree keeps the artwork in assets/ rather than loose beside the binary.
        const std::string inAssets = dir + "assets/" + file;
        if (readable(inAssets)) return inAssets;
    }
    // Then the working directory, both ways — this is how the studio run from its own source tree
    // has always found these, and a developer doing that should keep working.
    if (readable(file)) return file;
    const std::string cwdAssets = "assets/" + file;
    if (readable(cwdAssets)) return cwdAssets;
    return file;
}

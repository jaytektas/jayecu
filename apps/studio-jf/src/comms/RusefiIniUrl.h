#pragma once

// Where rusEFI publishes the .ini for a given firmware. An ECU answers a signature query with
//
//     rusEFI <branch>.<year>.<month>.<day>.<target>.<hash>
//
// and that is all six fields of the URL its definition lives at. This builds that URL in the scheme
// rusEFI publishes its definitions under: exactly six dot-separated fields after the prefix, or the
// signature is not a rusEFI one and there is nothing to fetch.

#include <string>
#include <vector>

inline std::string rusefiIniUrl(const std::string& signature) {
    static constexpr const char* kPrefix = "rusEFI ";
    if (signature.rfind(kPrefix, 0) != 0) return {};

    std::string body = signature.substr(std::string(kPrefix).size());
    while (!body.empty() && (body.back() == ' ' || body.back() == '\0')) body.pop_back();
    while (!body.empty() && body.front() == ' ') body.erase(0, 1);

    std::vector<std::string> f;
    for (size_t p = 0; ; ) {
        const size_t dot = body.find('.', p);
        f.push_back(body.substr(p, dot == std::string::npos ? std::string::npos : dot - p));
        if (dot == std::string::npos) break;
        p = dot + 1;
    }
    if (f.size() != 6) return {};
    for (const auto& x : f) if (x.empty()) return {};

    return "https://rusefi.com/online/ini/rusefi/" + f[0] + "/" + f[1] + "/" + f[2] + "/" + f[3] + "/"
           + f[4] + "/" + f[5] + ".ini";
}

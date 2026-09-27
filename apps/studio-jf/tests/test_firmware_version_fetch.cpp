// A released version's descriptor and dashboard, fetched for an ECU that runs it (FirmwareFetch::
// fetchVersion) — against a release served from this machine, so no network and no GitHub.
//
// What must hold: the right release is found in the list, every file is proved against SHA256SUMS, a
// kit whose layout is not the ECU's gives nothing, a version no release carries gives nothing, and a
// file that does not match its checksum is not handed back.

#include "app/FirmwareFetch.h"

#include <j/core/MainThreadDispatcher.h>
#include <j/io/JLocalWebServer.h>
#include <j/update/JSha256.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[version-fetch] %s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}
static void put(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

static fwfetch::VersionFiles fetch(const std::string& ver, const std::string& hash) {
    bool done = false;
    fwfetch::VersionFiles got;
    fwfetch::fetchVersion("jaytek_v1", ver, hash, true, true,
                          [&](const fwfetch::VersionFiles& r) { got = r; done = true; });
    for (int i = 0; i < 300 && !done; ++i) jf::JMainThreadDispatcher::instance().drainFor(100);
    if (!done) got.error = "timed out";
    return got;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "version_fetch_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    jf::JLocalWebServer web;
    web.mount("r", dir.string());
    std::string err;
    if (!web.start(err)) { std::printf("[version-fetch] cannot serve: %s\n", err.c_str()); return 1; }
    const std::string base = "http://127.0.0.1:" + std::to_string(web.port()) + "/r/";

    const std::string meta = "{\"meta\":{\"board\":\"jaytek_v1\",\"layout_hash\":\"abcd1234\"}}";
    const std::string gui  = "{\"tree\":[],\"panelLibrary\":{}}";
    const std::string kit  = "{\"board\":\"jaytek_v1\",\"version\":\"0.4.2\",\"layout_hash\":\"abcd1234\","
                             "\"firmware\":\"jaytek_v1-0.4.2-firmware.bin\",\"meta\":\"jaytek_v1-0.4.2.meta\","
                             "\"dashboard\":\"jaytek_v1-0.4.2.gui\"}";
    put(dir / "jaytek_v1-0.4.2-kit.json", kit);
    put(dir / "jaytek_v1-0.4.2.meta", meta);
    put(dir / "jaytek_v1-0.4.2.gui", gui);
    const std::string sums = jf::JSha256::hex(kit) + "  jaytek_v1-0.4.2-kit.json\n" +
                             jf::JSha256::hex(meta) + "  jaytek_v1-0.4.2.meta\n" +
                             jf::JSha256::hex(gui) + "  jaytek_v1-0.4.2.gui\n";
    put(dir / "SHA256SUMS", sums);
    auto asset = [&](const char* n) { return "{\"name\":\"" + std::string(n) + "\",\"browser_download_url\":\"" + base + n + "\"}"; };
    // Two releases: a newer one without this version, then the one that carries it.
    put(dir / "list.json", "[{\"tag_name\":\"0.5.0\",\"assets\":[]},{\"tag_name\":\"0.4.2\",\"assets\":[" +
        asset("jaytek_v1-0.4.2-kit.json") + "," + asset("jaytek_v1-0.4.2.meta") + "," +
        asset("jaytek_v1-0.4.2.gui") + "," + asset("SHA256SUMS") + "]}]");
#ifdef _WIN32
    _putenv_s("JAYECU_FIRMWARE_URL", (base + "list.json").c_str());
#else
    setenv("JAYECU_FIRMWARE_URL", (base + "list.json").c_str(), 1);
#endif

    const auto ok = fetch("0.4.2", "abcd1234");
    check(std::string(ok.meta.begin(), ok.meta.end()) == meta && std::string(ok.dashboard.begin(), ok.dashboard.end()) == gui,
          "the version's descriptor and dashboard come back, from the older of two releases");

    const auto other = fetch("0.4.2", "ffff0000");
    check(other.meta.empty() && other.dashboard.empty() && other.error.find("layout") != std::string::npos,
          "a release whose layout is not the ECU's gives nothing, and says why");

    const auto missing = fetch("0.3.9", "abcd1234");
    check(missing.meta.empty() && missing.error.find("no release carries") != std::string::npos,
          "a version no release carries gives nothing");

    put(dir / "jaytek_v1-0.4.2.meta", meta + " ");           // changed after the checksums were made
    const auto bad = fetch("0.4.2", "abcd1234");
    check(bad.meta.empty() && bad.error.find("checksum") != std::string::npos,
          "a file that does not match SHA256SUMS is not handed back");

    // THE BETA CHANNEL (fetchLatest). A full release 0.5.0 and a newer pre-release 0.6.0-beta.1: off, the
    // release is fetched; on, the beta. Each kit is whole: label, image, descriptor.
    auto kitFiles = [&](const std::string& ver, std::string& list) {
        const std::string k = "{\"board\":\"jaytek_v1\",\"version\":\"" + ver + "\",\"layout_hash\":\"abcd1234\","
                              "\"firmware\":\"fw-" + ver + ".bin\",\"meta\":\"m-" + ver + ".meta\"}";
        put(dir / ("jaytek_v1-" + ver + "-kit.json"), k);
        put(dir / ("fw-" + ver + ".bin"), "image " + ver);
        put(dir / ("m-" + ver + ".meta"), meta);
        const std::string sumsName = "SHA256SUMS-" + ver;
        put(dir / sumsName, jf::JSha256::hex(k) + "  jaytek_v1-" + ver + "-kit.json\n" +
                            jf::JSha256::hex("image " + ver) + "  fw-" + ver + ".bin\n" +
                            jf::JSha256::hex(meta) + "  m-" + ver + ".meta\n");
        auto as = [&](const std::string& n, const std::string& file) {
            return "{\"name\":\"" + n + "\",\"browser_download_url\":\"" + base + file + "\"}"; };
        list = "[" + as("jaytek_v1-" + ver + "-kit.json", "jaytek_v1-" + ver + "-kit.json") + "," +
               as("fw-" + ver + ".bin", "fw-" + ver + ".bin") + "," + as("m-" + ver + ".meta", "m-" + ver + ".meta") +
               "," + as("SHA256SUMS", sumsName) + "]";
    };
    std::string rel, beta;
    kitFiles("0.5.0", rel);
    kitFiles("0.6.0-beta.1", beta);
    put(dir / "list.json", "[{\"tag_name\":\"0.6.0-beta.1\",\"prerelease\":true,\"assets\":" + beta +
        "},{\"tag_name\":\"0.5.0\",\"prerelease\":false,\"assets\":" + rel + "}]");
    auto latest = [&](bool includeBeta, const fs::path& dest) {
        bool done = false;
        fwfetch::Result got;
        fwfetch::fetchLatest({ "jaytek_v1" }, {}, "0.1.0", dest.string(), includeBeta,
                             [&](const fwfetch::Result& r) { got = r; done = true; });
        for (int i = 0; i < 300 && !done; ++i) jf::JMainThreadDispatcher::instance().drainFor(100);
        return got;
    };
    const auto off = latest(false, dir / "kits-off");
    check(off.installed.size() == 1 && off.installed[0] == "jaytek_v1 0.5.0" &&
          fs::exists(dir / "kits-off" / "jaytek_v1-0.5.0" / "fw-0.5.0.bin"),
          "with the beta channel off, the full release is fetched and the beta is not");
    const auto on = latest(true, dir / "kits-on");
    check(on.installed.size() == 1 && on.installed[0] == "jaytek_v1 0.6.0-beta.1",
          "with it on, the newer beta is fetched — one kit, not every release");

    fs::remove_all(dir);
    std::printf("\n[version-fetch] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}

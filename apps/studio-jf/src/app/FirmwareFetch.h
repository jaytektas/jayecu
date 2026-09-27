#pragma once

// FirmwareFetch — download newer ECU firmware kits from the project's releases.
//
// A RELEASE (github.com/jaytektas/jayecu) carries the studio packages and, per board:
//     <board>-<version>-kit.json        the kit's label (FirmwareKits.h), naming the files below
//     <board>-<version>-firmware.bin    the image the studio flashes over DFU
//     <board>-<version>.meta            its descriptor
//     <board>-<version>.gui             its dashboard (optional)
// and once for the whole release:
//     SHA256SUMS                        `sha256sum` output for every file above
//
// The release's tag is the STUDIO's version; a kit's version is its own, read from its file names and
// kit.json. Only boards the studio has actually met are fetched, and only when the kit is newer than the
// newest kit already on hand. Every file — the kit.json included — is checked against SHA256SUMS before
// anything is written, and a kit is written to a temporary folder and renamed into place, so a
// half-finished download is never mistaken for a kit.
//
// FOR TESTING, set JAYECU_FIRMWARE_URL to a local answer in the same shape as GitHub's releases API
// (tools/fake_release.py --firmware serves one).

#include "../model/FirmwareKits.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fwfetch {

inline constexpr const char* kReleasesApi =
    "https://api.github.com/repos/jaytektas/jayecu/releases/latest";

std::string releasesUrl();

struct Result {
    bool        noReleases = false;           // the repository has none yet (404)
    std::string error;                        // set when the check itself failed
    std::string latest;                       // the release's tag
    std::vector<std::string> installed;       // "<board> <version>" for each kit fetched this time
    std::vector<std::string> needsStudio;     // "<board> <version> needs studio <x>" — skipped
};

// Check the latest firmware release and fetch what is newer, OFF the main thread. `done` runs on the
// main thread. `boards` are the boards worth fetching; `have` the kits already on disk; kits are
// written under `destRoot`.
// `includeBeta`: the beta channel (Preferences ▸ Updates) — every release, pre-releases included, and the
// newest kit per board wins. Off: the latest full release only.
void fetchLatest(std::vector<std::string> boards, std::vector<fwkits::Kit> have,
                 std::string studioVersion, std::string destRoot, bool includeBeta,
                 std::function<void(const Result&)> done);

// ONE RELEASED VERSION'S DESCRIPTOR AND DASHBOARD, for an ECU that runs it. A studio that has never met
// that firmware — a new laptop, an ECU with no SD card — gets them from the release that shipped it: every
// release is listed, the one carrying "<board>-<version>-kit.json" is used, and each file is checked
// against that release's SHA256SUMS. The kit's layout_hash must be the ECU's, or nothing is returned: a
// descriptor for another layout would put every value in the wrong place. The firmware image is not
// fetched — this is for reading an ECU, not for flashing it.
struct VersionFiles {
    std::vector<uint8_t> meta, dashboard;     // empty when not wanted, absent, or not proven
    std::string error;                        // why nothing (or not everything) came back
};
void fetchVersion(std::string board, std::string version, std::string layoutHash,
                  bool wantMeta, bool wantDashboard, std::function<void(const VersionFiles&)> done);

}  // namespace fwfetch

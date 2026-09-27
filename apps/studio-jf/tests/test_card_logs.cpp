// Collecting the ECU's logs off the card, which is a folder and not a protocol.
//
// The card at key-off is an ordinary mounted drive, so this drives the real code against a real
// directory holding real MLG bytes. What it checks hardest is the two things that would be wrong
// silently: that "already imported" is read from the disk rather than remembered, and that the
// capture date lands in the four header bytes MegaLogViewer reads it from — a log that opens
// perfectly with no date on it looks fine and is worse than one that fails.
//
//   cmake --build build --target card_logs_test && ./build/card_logs_test
#include "../src/model/CardLogs.h"
#include "../src/model/DatalogRecorder.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// An MLG v2 file as the ECU writes one: the magic, version 2, and a ZERO epoch — which is the whole
// reason the importer has anything to do.
static void writeFakeLog(const fs::path& p, size_t payload) {
    std::ofstream f(p.string(), std::ios::binary);
    char head[24] = {};
    head[0]='M'; head[1]='L'; head[2]='V'; head[3]='L'; head[4]='G'; head[5]=0;
    head[6]=0; head[7]=2;                       // format version 2, big-endian
    // bytes 8..11 (epoch) deliberately left zero
    f.write(head, sizeof head);
    std::vector<char> body(payload, '\x5A');
    f.write(body.data(), (std::streamsize)body.size());
}

static uint32_t headerEpoch(const fs::path& p) {
    std::ifstream f(p.string(), std::ios::binary);
    unsigned char h[24] = {};
    f.read(reinterpret_cast<char*>(h), sizeof h);
    return (uint32_t(h[8]) << 24) | (uint32_t(h[9]) << 16) | (uint32_t(h[10]) << 8) | uint32_t(h[11]);
}

int main() {
    std::error_code ec;
    const fs::path card = fs::temp_directory_path() / "jayecu_card_test";
    const fs::path logs = fs::temp_directory_path() / "jayecu_card_logs_test";
    fs::remove_all(card, ec); fs::remove_all(logs, ec);
    fs::create_directories(card, ec); fs::create_directories(logs, ec);
    DatalogRecorder::setDirectory(logs.string());

    writeFakeLog(card / "LOG0001.MLG", 4096);
    writeFakeLog(card / "LOG0002.MLG", 8192);
    { std::ofstream junk((card / "TUNE.BIN").string(), std::ios::binary); junk << "not a log"; }

    // --- recognising the card ----------------------------------------------------------------
    ck(CardLogs::looksLikeCard(card.string()), "a folder holding .MLG logs is a card");
    ck(!CardLogs::looksLikeCard(logs.string()), "an empty folder is not");
    ck(!CardLogs::looksLikeCard((card / "nope").string()), "a folder that does not exist is not");

    // A pointed-at card is remembered and found again without any volume enumeration — which is the
    // path a machine with no automounter lives on.
    CardLogs::rememberPath(card.string());
    ck(CardLogs::findCard() == card.string(), "the remembered card is found", CardLogs::findCard());

    // --- listing -----------------------------------------------------------------------------
    auto list = CardLogs::logsOn(card.string());
    ck(list.size() == 2, "only the .MLG files are listed", std::to_string(list.size()));
    ck(!list.empty() && !list[0].imported, "nothing is imported yet");
    ck(!list.empty() && list[0].target.rfind("jayecu_", 0) == 0 &&
       list[0].target.find("LOG000") != std::string::npos,
       "the import name is date-first and keeps the ECU's own name",
       list.empty() ? "" : list[0].target);

    // --- importing ---------------------------------------------------------------------------
    std::string why;
    ck(CardLogs::import(list[0], &why), "a log imports", why);
    const fs::path landed = logs / fs::path(list[0].target);
    ck(fs::exists(landed, ec), "…and lands in the studio's log folder");
    ck(fs::file_size(landed, ec) == list[0].bytes, "…the whole file, byte for byte",
       std::to_string(fs::file_size(landed, ec)) + " vs " + std::to_string(list[0].bytes));

    // THE STAMP. The ECU wrote zero; the import must write the file's own time, or MegaLogViewer
    // shows a log with no capture date and nothing says why.
    ck(headerEpoch(card / list[0].name) == 0, "the card's copy still has no date in its header");
    const uint32_t stamped = headerEpoch(landed);
    ck(stamped != 0, "the imported copy carries a capture date", std::to_string(stamped));
    ck(stamped > 1700000000u, "…and it is a plausible one, not junk", std::to_string(stamped));

    // --- imported-ness is read from the disk, not remembered ---------------------------------
    list = CardLogs::logsOn(card.string());
    size_t importedNow = 0;
    for (const auto& e : list) if (e.imported) ++importedNow;
    ck(importedNow == 1, "the imported one now says so", std::to_string(importedNow));

    fs::remove(landed, ec);                     // move it out of the folder…
    list = CardLogs::logsOn(card.string());
    importedNow = 0;
    for (const auto& e : list) if (e.imported) ++importedNow;
    ck(importedNow == 0, "…and it is honestly un-imported again",
       "a manifest would still be claiming it was there");

    // --- deleting refuses to destroy the only copy -------------------------------------------
    why.clear();
    ck(!CardLogs::erase(list[0], false, &why), "deleting an un-imported log is refused", why);
    ck(!why.empty(), "…with a reason");
    ck(fs::exists(card / list[0].name, ec), "…and the file is still there");

    ck(CardLogs::import(list[0], &why), "import it first", why);
    auto after = CardLogs::logsOn(card.string());
    const CardLogs::Entry* imported = nullptr;
    for (const auto& e : after) if (e.imported) imported = &e;
    ck(imported != nullptr, "it is imported");
    if (imported) {
        ck(CardLogs::erase(*imported, false, &why), "…and now it deletes", why);
        ck(!fs::exists(imported->path, ec), "…gone from the card");
    }

    CardLogs::rememberPath("");
    fs::remove_all(card, ec); fs::remove_all(logs, ec);
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All card log tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

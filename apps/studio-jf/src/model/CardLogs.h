#pragma once
//
// CardLogs — the ECU's own logs, collected from the card once the PC has it.
//
// NOT OVER THE COMMS LINK. The card already becomes a USB drive on its own when the key goes off,
// which is exactly when a tuner is finished driving and wants the logs. So there is no directory
// protocol, no chunked fetch, no ownership choreography and no window in which the ECU and the PC
// both think they have the card: the studio waits for the drive to turn up and reads it like any
// other folder, at filesystem speed. A protocol was designed for this and thrown away when the
// simpler answer turned out to already be there.
//
// FINDING IT is the only part that is not free. A removable volume whose root holds LOGnnnn.MLG
// files is the card — that is a stronger signal than a volume label, which nothing sets, and it
// costs one directory listing. Anything the search cannot see, the tuner can point at once, and
// that choice is remembered.
//
// IMPORTING STAMPS THE DATE. The ECU writes its MLG header with epoch 0 (`write_file_header`'s
// default) because the log is opened before anything asks the clock, so MegaLogViewer opens a
// perfectly good file with no capture date on it. The filesystem knows when the file was written —
// the ECU's RTC stamped it through FatFS — so the import writes that into the header's four epoch
// bytes as it copies. It is the one thing about the file the studio knows and the ECU did not.
//
#include <cstdint>
#include <string>
#include <vector>

class CardLogs {
public:
    struct Entry {
        std::string path;        // absolute, on the card
        std::string name;        // "LOG0007.MLG"
        std::string target;      // what it is called once imported (date-first, so it sorts)
        uint64_t    bytes = 0;
        int64_t     epoch = 0;   // seconds; from the file's own timestamp, 0 if unknown
        bool        imported = false;
    };

    // The card's mount point, or empty. Tries the remembered folder first (a card the tuner pointed
    // at once), then every removable volume. Cheap enough to call on a timer.
    static std::string findCard();
    static std::string rememberedPath();
    static void        rememberPath(const std::string& dir);   // "" forgets it

    // Does this folder look like the card? One directory listing; true when it holds any .MLG.
    static bool looksLikeCard(const std::string& dir);

    // The logs on it, newest first. `imported` is true when a file of the same target name and size
    // is already in the studio's log directory — no manifest to go stale, and moving a log out of
    // that folder honestly makes it un-imported again.
    static std::vector<Entry> logsOn(const std::string& dir);

    // Copy one across, stamping the capture date into the MLG header on the way. False with a
    // reason; an entry already imported succeeds without copying again.
    static bool import(const Entry& e, std::string* whyNot = nullptr);

    // Delete one from the card. Refuses a log that has not been imported, unless `force` — the
    // whole point of a card is that it is the only copy until it is not.
    static bool erase(const Entry& e, bool force, std::string* whyNot = nullptr);

    // Where imports land: the same directory the studio's own recordings go to, so one folder is
    // the answer to "where are my logs".
    static std::string importDirectory();
};

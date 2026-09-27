#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <j/config/Json.h>

// Ecu — an ECU known to the studio, keyed by its factory UID (96-bit hex string).
//
//   <ecuRoot>/<uid>/
//     ecu.json          board + last_port + last_seen + current_tune + last_active_tune
//     tunes/
//       <name>.tune     named dict-format tune files (TuneFile::serialise)
//     dashboard.gui     the studio layout for this ECU
//     restore/
//       <tuneName> <timestamp>.tune   connect-time snapshots
//
// Meta is NOT stored here — resolved from the pool by board + layout_hash at connect time.
// Multiple tunes can coexist; current_tune is a pointer to whichever was last pushed to hardware.
class Ecu {
public:
    struct Summary {
        std::string uid;
        std::string board;
        std::string currentTune;
        std::string lastSeen;
    };

    // Root of the on-disk ECU store (created on demand).
    static std::string ecuRoot();

    // List all known ECUs, most-recently-seen first.
    static std::vector<Summary> list();

    // Open an existing ECU entry. Returns nullptr if the UID is unknown.
    static Ecu *open(const std::string &uid);

    // Open if known, create a new entry (with the given board) if not. Never returns nullptr for a
    // non-empty uid (unless the filesystem is broken).
    static Ecu *openOrCreate(const std::string &uid, const std::string &board);

    std::string uid()            const { return uid_; }
    std::string board()          const { return j_["board"].str(); }

    // WHAT TO CALL THIS ECU IN A LIST. `board` names the thing for a native ECU ("jaytek_v1"), and for
    // an imported TunerStudio definition it is only ever "ts" — the protocol, not the controller, which
    // tells a reader nothing about which of their ECUs they are looking at. An import records the
    // definition's own signature here instead ("rusEFI jaytek.2026.08.12.jaytek_f7.…"), and everything
    // that shows a name to a person asks for this.
    std::string label()          const { const std::string l = j_["label"].str(); return l.empty() ? board() : l; }
    void setLabel(const std::string& l);
    std::string currentTune()    const { return j_["current_tune"].str(); }
    std::string lastActiveTune() const { return j_["last_active_tune"].str(); }
    std::string lastSeen()       const { return j_["last_seen"].str(); }

    std::string dir()            const { return dir_; }
    std::string dashboardPath()  const;
    std::string restoreDir()     const;

    // Tune library access.
    std::vector<std::string> tuneNames()  const;
    std::vector<uint8_t>     loadTune(const std::string &name) const;
    void                     saveTune(const std::string &name, const std::vector<uint8_t> &data);

    // Metadata updates — each call persists ecu.json.
    void setCurrentTune(const std::string &name);
    void setLastPort(const std::string &port);
    void setLastActiveTune(const std::string &name);
    void save() const;

private:
    Ecu() = default;
    std::string uid_;
    std::string dir_;
    jf::JJson   j_ = jf::JJson::object();
};

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
    // WHAT A PERSON TELLS THEIR ECUS APART BY. The uid and the board are the same on every ECU of a kind;
    // the car is not. The tune's Vehicle Identity (Name, Make, Model, …) is read from the ECU's current
    // tune, and is empty wherever nobody filled it in.
    struct Vehicle {
        std::string name, make, model, engine, vin, notes;
        bool empty() const { return name.empty() && make.empty() && model.empty() && engine.empty()
                                    && vin.empty() && notes.empty(); }
    };
    struct Summary {
        std::string uid;
        std::string board;         // the board id ("jaytek_v1"; "ts" for an imported definition)
        std::string label;         // label(): the board, or an import's own signature
        std::string currentTune;   // the tune the vehicle details came from
        std::string lastSeen;
        std::string lastPort;
        std::string dfuSerial;     // the USB serial its STM32 bootloader answers with ("3974356C3138")
        Vehicle     vehicle;
    };

    // Root of the on-disk ECU store (created on demand).
    static std::string ecuRoot();

    // List all known ECUs, most-recently-seen first.
    static std::vector<Summary> list();
    // One entry's summary, for an ECU found on a port (empty uid when it is not in the library).
    static Summary summary(const std::string &uid);

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
    // THE BOOTLOADER'S NAME FOR THIS ECU. A board in its ROM bootloader cannot say which board it is —
    // ST's bootloader knows nothing about the PCB — but it does answer with a USB serial derived from the
    // chip, and that does not change. Recorded whenever the studio itself sends this ECU into its
    // bootloader, so a recovery later can recognise the board instead of asking someone to guess.
    std::string dfuSerial() const { return j_["dfu_serial"].str(); }
    void setDfuSerial(const std::string &serial);
    // The library ECU whose bootloader answers with `serial`; empty uid when none has been seen.
    static Summary byDfuSerial(const std::string &serial);
    void save() const;

private:
    Ecu() = default;
    std::string uid_;
    std::string dir_;
    jf::JJson   j_ = jf::JJson::object();
};

#pragma once

// EngineOutputLayout — the studio lays out the coil and injector output rows. The firmware never does.
//
// outputs.output[i] IS physical output i (IGN1-12, LS1-22, HS1-8). A coil or injector is a row whose
// function is Ignition / Injector and whose `cylinder` names ONE value: Cylinder 1-12 (the rotor on a
// rotary), All, Bank 1 or Bank 2. The firmware fires exactly what the rows say.
//
// WHEN: only when the user edits a setting that decides them — cylinder count, cycle, ignition mode (the
// coils); cylinder count, cycle, number of stages, a stage's mode or injector count (the injectors). The
// firing order is NOT one of them: it decides when outputs fire, never which output a cylinder has, and
// under wasted spark the firmware finds each coil's companion from it at run time. After a layout the user
// may edit any pin by hand, and it stays as edited until one of those settings changes again.
//
// THE STANDARD WIRING is by cylinder number:
//   coils      coil-on-plug IGN(n) = cylinder n; wasted spark one coil per companion pair, named by the
//              pair's lower cylinder, in cylinder order (1-3-4-2 -> IGN1 = 1, IGN2 = 2; a Chevy V8
//              1-8-4-3-6-5-7-2 -> 1, 2, 4, 5); distributor IGN1 = All. Rotary: leading IGN(r) / trailing
//              IGN(4+r) = rotor r, or IGN1 / IGN5 = All for a distributor.
//   injectors  one contiguous LS block per stage from LS1; a per-cylinder mode LS(base+n) = cylinder n;
//              Bank splits the stage's injector count between the banks present; Multi-Point = All.
//
// A PIN THAT IS ALREADY A GENERIC OUTPUT IS NEVER TAKEN. A fan on LS5 is somebody's configuration; the
// injector that would have landed there moves to the next free pin of its class instead.
//
// layout() is pure; readEngine(), apply() and install() are the Cache edges.

#include <array>
#include <string>
#include <vector>

class Cache;

namespace engine_outputs {

inline constexpr int kIgnRows = 12;   // IGN1-12 are rows 0-11
inline constexpr int kLsRows  = 22;   // LS1-22 are rows 12-33
inline constexpr int kLsBase  = kIgnRows;
inline constexpr int kMaxStages = 4;
inline constexpr int kFacesPerRotor = 3, kMaxRotors = 4;

enum Function : int { None = 0, Ignition = 1, Injector = 2, Generic = 3 };
enum CylValue : int { CylNone = 0, CylAll = 13, CylBank1 = 14, CylBank2 = 15 };
enum class Coils : int { Distributor = 0, Wasted = 1, CoilOnPlug = 2 };
enum Mode : int { Sequential = 0, SemiSequential = 1, MultiPoint = 2, Bank = 3, SequentialAnySync = 4 };

struct Engine {
    int cylinders = 0;                     // cylinder slots; on a rotary, rotor faces (3 per rotor)
    bool rotary = false;
    bool oddFire = false;
    int cycleDeg10 = 7200;
    Coils coils = Coils::Wasted;
    std::vector<int> firingOrder;          // cylinder (1-based) at each firing position
    std::vector<int> tdcDeg10;             // per cylinder (read for odd-fire wasted-spark pairs)
    std::vector<int> banks;                // per cylinder
    int stages = 1;
    std::array<int, kMaxStages> modes{};
    std::array<int, kMaxStages> injectors{};   // a Bank / Multi-Point stage's injector count
};

struct Row {
    int row = 0;                           // index into outputs.output[]
    int function = None;
    int cylinder = CylNone;
    int stage = 0;
    int plug = 0;                          // 0 leading, 1 trailing
};

[[nodiscard]] inline bool perCylinder(int mode) {
    return mode == Sequential || mode == SemiSequential || mode == SequentialAnySync;
}

// The rows for this engine. `generic[i]` is true for a row that is already a Generic output — such a pin
// is skipped. Only the class asked for is laid out: coils (IGN rows) and/or injectors (LS rows).
std::vector<Row> layout(const Engine& e, bool coils, bool injectors, const std::vector<bool>& generic);

// Which classes an edit to `path` re-lays: none, or coils and/or injectors.
void triggers(const std::string& path, bool& coils, bool& injectors);

// The Cylinder values an output row may take RIGHT NOW, as option indices; empty = no opinion (not a
// cylinder field, or a row that is neither a coil nor an injector). A coil: the cylinders (rotors) this
// engine has, or All for a distributor. An injector: per-cylinder stages the cylinders, a Bank stage the
// banks the cylinders are on, a Multi-Point stage All.
std::vector<int> cylinderOptions(const Cache& c, const std::string& bind);

// The Cache edges. readEngine() reads the tune; apply() writes the laid-out rows for the chosen classes
// and clears every other coil (or injector) row; install() hooks Cache::configEdited so an edit to a
// deciding setting lays the rows out in the same undo step as the edit.
Engine readEngine(const Cache& c);
void apply(Cache& c, bool coils, bool injectors);
void install(Cache& c);

} // namespace engine_outputs

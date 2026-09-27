#pragma once
#include "SchedulerTypes.h"
#include "../../generated/modules/engine_config.h"
#include "../../generated/modules/outputs_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// The FIRING half of the output rows.
//
// outputs.output[i] IS physical output i, in the board's own order: the ignition pins, then the low-side
// pins, then the high-side pins. A row whose function is Ignition or Injector names the ONE cylinder it
// serves (or All / a bank); this file reads those rows into what the scheduler binds. The STUDIO writes
// the rows — nothing here allocates, and a cylinder no row names gets nothing.
//
// The scheduler takes a VALUE COPY of this at reconfigure (engine stopped), exactly as it takes the engine
// shadow, so a row edited while the engine runs changes nothing until the next stop.
// ---------------------------------------------------------------------------

// Schema order of outputs.output[].function.
enum class OutputFunction : uint8_t { NONE = 0, IGNITION = 1, INJECTOR = 2, GENERIC = 3 };

// Schema order of outputs.output[].cylinder: 0 none, 1..12 a cylinder (a rotor on a rotary), then these.
inline constexpr uint8_t OUT_CYL_NONE   = 0;
inline constexpr uint8_t OUT_CYL_ALL    = 13;
inline constexpr uint8_t OUT_CYL_BANK1  = 14;
inline constexpr uint8_t OUT_CYL_BANK2  = 15;

// Where each pin class starts among the rows. The firing channels ARE the first rows, so an ignition
// channel index and its row index are the same number, and low-side channel k is row LS_BASE + k.
inline constexpr uint8_t OUT_ROW_IGN_BASE = 0;
inline constexpr uint8_t OUT_ROW_LS_BASE  = MAX_IGN_CHANNELS;
inline constexpr uint8_t OUT_ROW_HS_BASE  = MAX_IGN_CHANNELS + MAX_INJ_CHANNELS;

// One firing output: the cylinder value it names, and for a coil which plug (0 leading, 1 trailing —
// rotary only), for an injector which stage.
struct FiringOutput {
    bool     used        = false;   // the row's function claims this pin for firing
    uint8_t  cylinder    = OUT_CYL_NONE;
    uint8_t  stage       = 0;       // injector: 0..MAX_INJ_STAGES-1
    uint8_t  plug        = 0;       // coil: 0 leading, 1 trailing
    bool     active_high = true;

    bool operator==(const FiringOutput& o) const noexcept {
        return used == o.used && cylinder == o.cylinder && stage == o.stage && plug == o.plug
            && active_high == o.active_high;
    }
    bool operator!=(const FiringOutput& o) const noexcept { return !(*this == o); }
};

struct OutputMap {
    FiringOutput ign[MAX_IGN_CHANNELS];
    FiringOutput inj[MAX_INJ_CHANNELS];

    bool operator==(const OutputMap& o) const noexcept {
        for (int k = 0; k < MAX_IGN_CHANNELS; ++k) if (ign[k] != o.ign[k]) return false;
        for (int k = 0; k < MAX_INJ_CHANNELS; ++k) if (inj[k] != o.inj[k]) return false;
        return true;
    }
    bool operator!=(const OutputMap& o) const noexcept { return !(*this == o); }
};

// Read the firing rows out of the output array. `n` is how many rows the array has. A coil row off an IGN
// pin, or an injector row off an LS pin, has no compare channel under it and is not bound (the studio
// never offers it).
[[nodiscard]] inline OutputMap output_map_from(const OutputConfig* rows, uint8_t n) noexcept {
    OutputMap m{};
    for (uint8_t r = 0; r < n; ++r) {
        const OutputConfig& o = rows[r];
        const auto fn = static_cast<OutputFunction>(o.function);
        if (fn == OutputFunction::IGNITION) {
            if (r >= OUT_ROW_IGN_BASE + MAX_IGN_CHANNELS) continue;
            FiringOutput& f = m.ign[r - OUT_ROW_IGN_BASE];
            f.used = true; f.cylinder = o.cylinder; f.plug = o.ign_plug ? 1 : 0;
            f.active_high = o.active_high != 0;
        } else if (fn == OutputFunction::INJECTOR) {
            if (r < OUT_ROW_LS_BASE || r >= OUT_ROW_LS_BASE + MAX_INJ_CHANNELS) continue;
            FiringOutput& f = m.inj[r - OUT_ROW_LS_BASE];
            f.used = true; f.cylinder = o.cylinder; f.stage = o.inj_stage;
            f.active_high = o.active_high != 0;
        }
    }
    return m;
}

// Does a row's cylinder value name cylinder slot c (0-based)? On a rotary the slots are rotor FACES and
// the number is the rotor, so a rotor's value names all three of its faces.
[[nodiscard]] inline bool output_serves(const EngineConfig& e, uint8_t value, uint8_t c) noexcept {
    const bool rotary = static_cast<EngineCycleType>(e.cycle_type) == EngineCycleType::ROTARY;
    if (value >= 1 && value <= MAX_CYLINDERS)
        return rotary ? (c / FACES_PER_ROTOR) == (value - 1) : c == (value - 1);
    if (value == OUT_CYL_ALL)   return true;
    if (value == OUT_CYL_BANK1) return e.cyl[c].bank == 1;
    if (value == OUT_CYL_BANK2) return e.cyl[c].bank == 2;
    return false;
}

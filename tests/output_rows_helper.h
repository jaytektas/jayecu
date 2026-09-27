#pragma once
// Lay out the OUTPUT ROWS the way the studio does when an engine setting changes — the standard wiring
// every scheduler test was written against:
//   coils       coil-on-plug IGN(c) → cylinder c; wasted spark one coil per companion pair, naming the
//               pair's lower cylinder, in cylinder order; distributor IGN1 → All; rotary leading IGN(r)
//               / trailing IGN(MAX_ROTORS+r) → rotor r (distributor: IGN1 / IGN(MAX_ROTORS+1) → All).
//   injectors   each stage a contiguous LS block from LS1; a per-cylinder mode LS(base+c) → cylinder c
//               (rotary: rotor); Bank splits `outputs` between the banks present; Multi-Point → All.
// The rows are data — this is one layout of them, not a rule the firmware knows.
#include "Scheduler/OutputMap.h"
#include <cstring>

enum class TestCoils : uint8_t { DISTRIBUTOR = 0, WASTED = 1, COP = 2 };

// Write coil + injector rows into `rows` (the whole output array, cleared first). The engine's firing
// order, cylinder count, banks and cycle must already be set, and its TDCs written
// (update_cylinder_angles) for a wasted-spark layout; the stage MODES are read from eng.inj_stage.
inline void layout_output_rows(EngineConfig& eng, TestCoils coils, OutputConfig* rows,
                               uint8_t nrows, const uint8_t* grouped_outputs = nullptr) {
    std::memset(rows, 0, sizeof(OutputConfig) * nrows);
    for (uint8_t r = 0; r < nrows; ++r) rows[r].active_high = 1;
    eng.ign_mode = static_cast<uint8_t>(coils);
    const uint8_t n = eng.cylinder_count;
    const bool rotary = static_cast<EngineCycleType>(eng.cycle_type) == EngineCycleType::ROTARY;
    const uint8_t units = rotary ? static_cast<uint8_t>(n / FACES_PER_ROTOR) : n;   // cylinders or rotors
    auto ign = [&](uint8_t k, uint8_t value, uint8_t plug) {
        OutputConfig& o = rows[OUT_ROW_IGN_BASE + k];
        o.function = static_cast<uint8_t>(OutputFunction::IGNITION);
        o.cylinder = value;
        o.ign_plug = plug;
    };
    if (rotary) {
        for (uint8_t r = 0; r < units; ++r) {
            if (coils == TestCoils::DISTRIBUTOR && r > 0) break;
            const uint8_t v = (coils == TestCoils::DISTRIBUTOR) ? OUT_CYL_ALL : static_cast<uint8_t>(r + 1);
            ign(r, v, 0);
            ign(static_cast<uint8_t>(MAX_ROTORS + r), v, 1);
        }
    } else if (coils == TestCoils::COP) {
        for (uint8_t c = 0; c < n; ++c) ign(c, static_cast<uint8_t>(c + 1), 0);
    } else if (coils == TestCoils::WASTED) {
        const AngleDeg10 cyc = engine_cycle_angle(eng.cycle_type);
        uint16_t done = 0;
        uint8_t k = 0;
        for (uint8_t c = 0; c < n; ++c) {
            if (done & (1u << c)) continue;
            done = static_cast<uint16_t>(done | (1u << c));
            const AngleDeg10 comp = wasted_companion_tdc(angle_wrap(eng.cyl[c].tdc_angle, cyc), cyc);
            for (uint8_t j = c + 1; j < n; ++j)
                if (angle_wrap(eng.cyl[j].tdc_angle, cyc) == comp) done = static_cast<uint16_t>(done | (1u << j));
            ign(k++, static_cast<uint8_t>(c + 1), 0);
        }
    } else {
        ign(0, OUT_CYL_ALL, 0);
    }

    uint8_t banks[2] = {0, 0}, nbanks = 0;
    for (uint8_t c = 0; c < n; ++c) {
        const uint8_t b = eng.cyl[c].bank;
        if (!(nbanks > 0 && banks[0] == b) && !(nbanks > 1 && banks[1] == b) && nbanks < 2) banks[nbanks++] = b;
    }
    if (nbanks == 0) { banks[0] = 1; nbanks = 1; }
    if (nbanks == 2 && banks[0] > banks[1]) { const uint8_t t = banks[0]; banks[0] = banks[1]; banks[1] = t; }

    uint8_t base = 0;
    for (uint8_t s = 0; s < eng.num_inj_stages && s < MAX_INJ_STAGES; ++s) {
        const auto mode = static_cast<InjectionMode>(eng.inj_stage[s].mode);
        const bool per_cyl = mode == InjectionMode::SEQUENTIAL || mode == InjectionMode::SEMI_SEQUENTIAL ||
                             mode == InjectionMode::SEQUENTIAL_ANY_SYNC;
        const uint8_t outs = per_cyl ? units : (grouped_outputs ? grouped_outputs[s] : units);
        for (uint8_t i = 0; i < outs; ++i) {
            const uint8_t k = static_cast<uint8_t>(base + i);
            if (k >= MAX_INJ_CHANNELS) break;
            uint8_t v;
            if (per_cyl)                           v = static_cast<uint8_t>(i + 1);
            else if (mode == InjectionMode::BANK)  v = static_cast<uint8_t>(OUT_CYL_BANK1 - 1 +
                                                       banks[(i * nbanks) / (outs ? outs : 1)]);
            else                                   v = OUT_CYL_ALL;
            OutputConfig& o = rows[OUT_ROW_LS_BASE + k];
            o.function  = static_cast<uint8_t>(OutputFunction::INJECTOR);
            o.inj_stage = s;
            o.cylinder  = v;
        }
        base = static_cast<uint8_t>(base + outs);
    }
}

// The same layout, straight to the scheduler's map. Sets eng.ign_mode to match.
inline OutputMap layout_output_map(EngineConfig& eng, TestCoils coils,
                                   const uint8_t* grouped_outputs = nullptr) {
    OutputConfig rows[OUTPUTS_OUTPUT_COUNT];
    layout_output_rows(eng, coils, rows, OUTPUTS_OUTPUT_COUNT, grouped_outputs);
    return output_map_from(rows, OUTPUTS_OUTPUT_COUNT);
}

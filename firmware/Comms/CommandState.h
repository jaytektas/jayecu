#pragma once
#include <cstdint>

// command_state — one telemetry word the studio watches to know when a bench command finished and whether
// it succeeded, so it can re-read exactly the cal the routine wrote. value = (op << 2) | phase.
//
// It is LEVEL-valued, not an edge: it reads RUNNING for the whole routine, then flips to OK/FAIL. So the
// studio waits exactly as long as it says RUNNING and never guesses a completion window (the old gen-watch's
// 120 s arm window is gone). The `op` tag says which routine ran, so the studio pulls only that region.
//
// Packed into EcuTelemetry.command_state by CommsManager — no bus producer, mirroring g_config_generation.
// See apps/studio-jf Cache::ingestTelemetry for the decode + scoped re-read.
namespace cmdstate {

enum Phase : uint16_t { IDLE = 0, RUNNING = 1, OK = 2, FAIL = 3 };

// Concrete bench operations. ETB routines are (base + etb index); pedalcal stands alone. NONE = nothing ran.
enum Op : uint16_t {
    NONE = 0,
    ETB0_FINDLIMITS = 1, ETB1_FINDLIMITS,   // findlimits base = 1
    ETB0_FILLFF,         ETB1_FILLFF,        // fillff     base = 3
    ETB0_AUTOTUNE,       ETB1_AUTOTUNE,      // autotune   base = 5
    PEDALCAL,                                //            = 7
    ENGINE_RECONFIG,                         //            = 8  — engine stopped, cyl[].tdc_angle recomputed
};

inline uint16_t pack(Op op, Phase ph) { return static_cast<uint16_t>((static_cast<uint16_t>(op) << 2) | static_cast<uint16_t>(ph)); }
inline uint16_t op_of(uint16_t v)     { return static_cast<uint16_t>(v >> 2); }
inline uint16_t phase_of(uint16_t v)  { return static_cast<uint16_t>(v & 0x3); }

// ETB op for (routine base, etb index) — base is ETB0_FINDLIMITS / ETB0_FILLFF / ETB0_AUTOTUNE.
inline Op etb_op(Op base, unsigned etb) { return static_cast<Op>(static_cast<uint16_t>(base) + etb); }

}  // namespace cmdstate

extern volatile uint16_t g_command_state;   // defined in CommsManager.cpp (like g_config_generation)
inline void set_command_state(cmdstate::Op op, cmdstate::Phase ph) { g_command_state = cmdstate::pack(op, ph); }

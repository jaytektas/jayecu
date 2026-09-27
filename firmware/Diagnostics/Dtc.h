#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// One unified Diagnostic Trouble Code system. Every error source (sensors,
// engine protection, pin/input conflicts, config) raises the same DtcRecord into
// DtcManager — the single source of truth (current "open cases" + history). Each
// record carries its own FREEZE-FRAME (the conditions at the latest activation),
// so there is ONE record per distinct code, not a stream of duplicate events.
// One P-code namespace: real OBD codes (e.g. P0217) for everything, with the
// manufacturer P1xxx range for ECU-internal conditions without a standard code.
// ---------------------------------------------------------------------------

// Freeze-frame channels stored per record (conditions captured at the activation
// edge). Fixed set for now; a TS logFieldSelector-driven set is the later widening.
static constexpr uint8_t DTC_FF_CHANNELS = 4;   // [0]=rpm [1]=map [2]=clt_c [3]=batt_v

// One row per DISTINCT code that has occurred (deduped; re-occurrence bumps `count`
// + refreshes the freeze-frame, it does not consume a new slot). 36 bytes.
#pragma pack(push, 1)
struct DtcRecord {
    uint16_t code;        // OBD/mfr P-code (e.g. 0x0117 = P0117); 0 = empty slot
    uint8_t  source;      // who raised it (see DtcSource): sensor 0..90, subsystem 91+
    uint8_t  severity;    // 1..3 = level 1..3
    uint8_t  status;      // DTC_* flags below
    uint8_t  heal_cycles; // clean cycles since last active — OBD-style aging / auto-clear
    uint16_t count;       // number of times it has gone active (edge count, not per-frame)
    uint16_t first_boot;  // boot_id at first activation  } universal timestamp
    uint32_t first_ms;    // uptime ms at first activation } (RTC epoch lives in the log header)
    uint16_t last_boot;   // boot_id at most recent activation
    uint32_t last_ms;     // uptime ms at most recent activation
    float    ff[DTC_FF_CHANNELS];  // freeze-frame at the latest activation edge
};
#pragma pack(pop)
static_assert(sizeof(DtcRecord) == 36, "DtcRecord must be 36 bytes");

// HOW LONG A RAISED CODE STAYS CURRENT WITHOUT BEING RE-RAISED (see DtcManager::raise).
//
// The default is generous against the slowest judge on the ECU: a 2 Hz sensor evaluates every 500 ms,
// so three seconds is six of its periods — the bus's own "a writer updating at rate R passes a few
// periods". It is still short enough that unticking a check and watching the code go feels immediate.
constexpr uint32_t DTC_TTL_DEFAULT = 3000;
// An EVENT rather than a condition: nothing will re-assert it, and it stands until healed or cleared.
constexpr uint32_t DTC_TTL_LATCH   = 0;

// status flags
constexpr uint8_t DTC_ACTIVE    = 0x01;  // the condition is true right NOW (drives LED/cut)
constexpr uint8_t DTC_STORED    = 0x02;  // recorded — persists to SD, survives power-down
constexpr uint8_t DTC_CONFIRMED = 0x04;  // confirmed: active in two key cycles, held DTC_CONFIRM_MS, or an event
constexpr uint8_t DTC_CYCLE_ACT = 0x08;  // went active during the CURRENT key cycle (persisted: the next
                                         // key-on reads it to decide whether the last cycle was clean)

// OBD-style aging, counted in KEY CYCLES (key-on edges), not boots — a bench power-up on USB is not a
// drive. A key cycle in which a stored code never went active is a clean cycle (heal_cycles++).
constexpr uint32_t DTC_CONFIRM_MS       = 5000;  // continuously active this long -> CONFIRMED
constexpr uint8_t  DTC_UNCONFIRM_CYCLES = 3;     // clean cycles -> CONFIRMED drops (OBD: MIL off)
constexpr uint8_t  DTC_ERASE_CYCLES     = 40;    // clean cycles -> the record is erased (OBD: 40)

// Severity levels — intrinsic to each code; the reactor selects protection_levels[severity - 1]
// (Level 1 / Level 2 / Level 3), and what that level does is the tune's.
constexpr uint8_t DTC_SEV_LEVEL1 = 1;
constexpr uint8_t DTC_SEV_LEVEL2 = 2;
constexpr uint8_t DTC_SEV_LEVEL3 = 3;

// `source` id space — sensors take their catalog index (0..SENSOR_COUNT-1); the
// non-sensor subsystems take fixed ids above the sensor range.
//
// ABOVE IT, NOT INSIDE IT. These started at 91 when the catalogue had 91 rows; it now has 128, so sensors
// 91-98 raised under the SAME ids as the subsystems — sensor 93/94/97 codes counted as config sources
// (accepted and kept with the key off, dtc_is_config_source) and the studio named them "Pin arbiter",
// "Config", "Throttle". Sensors.cpp static_asserts SENSOR_COUNT <= SUBSYS_BASE so it cannot happen again.
// The studio's DtcDock::sourceName() copies these numbers and must move with them.
namespace DtcSource {
constexpr uint8_t SENSOR_BASE = 0;     // + sensor catalog index
constexpr uint8_t SUBSYS_BASE = 200;   // first non-sensor id (> max sensors)
constexpr uint8_t TRIGGER     = 200;   // crank/cam sync, tooth/gap
constexpr uint8_t PROTECTION  = 201;   // overtemp/overboost/etc.
constexpr uint8_t PIN_ARBITER = 202;   // output/input pin conflicts
constexpr uint8_t CONFIG      = 203;   // config-version / tune validity
constexpr uint8_t CAN_BUS     = 204;   // CAN device presence/health
constexpr uint8_t LUA         = 205;   // script-raised via setDtc()/clearDtc()
constexpr uint8_t THROTTLE    = 206;   // ETB / drive-by-wire L2 supervisor (feedback / stuck)
constexpr uint8_t MODULE      = 207;   // control module: required bus signal invalid/missing
constexpr uint8_t SUPPLY      = 208;   // the ECU's 5 V sensor references (power-good)
}

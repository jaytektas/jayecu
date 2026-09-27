#pragma once
#include "../II2cBus.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Lps2xBaro — the on-board barometric/ambient sensor, for BOTH ST parts in use:
// LPS22HB (jaytek_v1, I2C1) and LPS25HB (proteus, I2C2). One driver, because the
// two differ in four small ways and nothing else:
//
//   register            LPS22HB     LPS25HB
//   WHO_AM_I value      0xB1        0xBD
//   CTRL_REG1 address   0x10        0x20
//   CTRL_REG1 layout    BDU = bit1  PD = bit7 (must be SET to leave power-down,
//                                   and must be CLEAR on an LPS22), BDU = bit2
//   multi-byte read     auto-inc    needs bit7 of the sub-address set
//                       by default
//
// Those differences are why this is variant-aware rather than one register map: an
// LPS25 configured as an LPS22 stays powered down and reads nothing, and an LPS22
// read as an LPS25 returns the same byte three times. Pressure scaling is identical
// (4096 counts/hPa = 40960 counts/kPa).
//
// Variant detection is a WHO_AM_I PROBE, not board configuration, so a board that
// gets its part wrong is told rather than silently wrong. The probe is retried from
// service() until it succeeds — a sensor that is slow to come up, or a bus that is
// briefly busy at boot, must not leave the baro dead until the next reset.
//
// THE ENGINE FRAME NEVER BLOCKS ON THIS. service() is the ONLY member that touches
// the bus and must be called from a background task; the readers are pure cache
// reads that cannot reach I2C at all. Not "usually fast": incapable of blocking, by
// construction.
// ---------------------------------------------------------------------------
class Lps2xBaro {
public:
    struct Cfg {
        II2cBus* bus;
        uint8_t  dev_addr;      // 8-bit (7-bit << 1)
        uint32_t period_ms;     // rate-limit on ATTEMPTS, so a dead bus is not hammered
        uint32_t stale_ms;      // no successful read for this long -> stop trusting the cache
    };

    void init(const Cfg& cfg) noexcept { cfg_ = cfg; }

    // Sample the part. The only function here that touches the bus. Self-rate-limiting,
    // so calling it every pass of a background task is correct. `now_ms` is a plain tick.
    void service(uint32_t now_ms) noexcept;

    // Cache reads. No bus call, no timeout, no branch that can reach I2C.
    [[nodiscard]] float kpa()    const noexcept { return kpa_; }
    [[nodiscard]] float temp_c() const noexcept { return temp_c_; }

    // Has the sampler produced a good reading recently? A cached number that silently
    // stopped updating looks perfectly healthy, which is how a dead sensor becomes an
    // undetected wrong reading instead of a DTC — so consumers publish THIS as validity
    // rather than trusting the value's age to speak for itself.
    [[nodiscard]] bool valid(uint32_t now_ms) const noexcept;

    enum class Variant : uint8_t { Unknown, Lps22hb, Lps25hb };
    [[nodiscard]] Variant variant() const noexcept { return variant_; }

private:
    bool identify_and_configure_() noexcept;

    Cfg      cfg_{};
    Variant  variant_      = Variant::Unknown;
    float    kpa_          = 101.3f;   // sea level, until the part says otherwise
    float    temp_c_       = 25.0f;
    uint32_t attempt_ms_   = 0;
    uint32_t good_ms_      = 0;
    bool     attempted_    = false;
    bool     good_         = false;
};

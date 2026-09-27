#include "Lps2xBaro.h"

namespace {
constexpr uint8_t REG_WHO_AM_I   = 0x0Fu;
constexpr uint8_t REG_CR1_LPS22  = 0x10u;
constexpr uint8_t REG_CR1_LPS25  = 0x20u;
constexpr uint8_t REG_PRESS_OUT  = 0x28u;   // XL/L/H, 24-bit little-endian
constexpr uint8_t REG_TEMP_OUT   = 0x2Bu;   // L/H, int16

constexpr uint8_t WHOAMI_LPS22   = 0xB1u;
constexpr uint8_t WHOAMI_LPS25   = 0xBDu;

// CTRL_REG1, per part. Both select a continuous output data rate so a read is a quick
// register fetch rather than a one-shot wait, and both set BDU so a multi-byte read cannot
// tear across an update — but BDU is a DIFFERENT BIT on each part, and the LPS25 additionally
// needs PD set to leave power-down (that bit must stay clear on an LPS22).
constexpr uint8_t CR1_LPS22 = 0x22u;   // ODR 010 = 10 Hz (bits 6:4) | BDU (bit 1)
constexpr uint8_t CR1_LPS25 = 0xC4u;   // PD (bit 7) | ODR 100 = 25 Hz (bits 6:4) | BDU (bit 2)

// An LPS25 auto-increments across a multi-byte read only when bit7 of the sub-address is set.
// An LPS22 has IF_ADD_INC set by default and is read at the bare address — which is the path
// bench-proven on jaytek, so it is left exactly as it was.
constexpr uint8_t AUTO_INC = 0x80u;
}  // namespace

bool Lps2xBaro::identify_and_configure_() noexcept {
    uint8_t who = 0;
    if (!cfg_.bus->read_reg(cfg_.dev_addr, REG_WHO_AM_I, &who, 1)) return false;

    uint8_t cr1_reg, cr1_val;
    switch (who) {
        case WHOAMI_LPS22: variant_ = Variant::Lps22hb; cr1_reg = REG_CR1_LPS22; cr1_val = CR1_LPS22; break;
        case WHOAMI_LPS25: variant_ = Variant::Lps25hb; cr1_reg = REG_CR1_LPS25; cr1_val = CR1_LPS25; break;
        default:           variant_ = Variant::Unknown; return false;   // not a part we know
    }
    if (!cfg_.bus->write_reg(cfg_.dev_addr, cr1_reg, cr1_val)) { variant_ = Variant::Unknown; return false; }
    return true;
}

void Lps2xBaro::service(uint32_t now_ms) noexcept {
    if (!cfg_.bus) return;
    if (attempted_ && static_cast<uint32_t>(now_ms - attempt_ms_) < cfg_.period_ms) return;
    attempt_ms_ = now_ms;
    attempted_  = true;

    // Identify lazily and keep retrying: a part that is slow to come up, or a bus busy at boot,
    // must not leave the baro dead until the next reset.
    if (variant_ == Variant::Unknown && !identify_and_configure_()) return;

    const uint8_t inc = (variant_ == Variant::Lps25hb) ? AUTO_INC : 0u;
    bool ok = false;

    // Pressure: PRESS_OUT_XL/L/H, raw 24-bit. 4096 counts/hPa -> /10 = kPa = raw/40960.
    uint8_t p[3] = {0};
    if (cfg_.bus->read_reg(cfg_.dev_addr, static_cast<uint8_t>(REG_PRESS_OUT | inc), p, 3)) {
        const int32_t raw = static_cast<int32_t>(
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0]);
        kpa_ = static_cast<float>(raw) / 40960.0f;
        ok = true;
    }

    // Ambient temperature from the same part: TEMP_OUT_L/H, int16. The SCALING DIFFERS —
    // an LPS22HB reports hundredths of a degree, an LPS25HB reports an offset from 42.5 C in
    // 1/480ths. Sharing one formula would put an LPS25 out by tens of degrees while looking
    // like a plausible reading.
    uint8_t t[2] = {0};
    if (cfg_.bus->read_reg(cfg_.dev_addr, static_cast<uint8_t>(REG_TEMP_OUT | inc), t, 2)) {
        const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(t[1]) << 8) | t[0]);
        temp_c_ = (variant_ == Variant::Lps25hb) ? (42.5f + static_cast<float>(raw) / 480.0f)
                                                 : (static_cast<float>(raw) / 100.0f);
        ok = true;
    }

    // Only a SUCCESSFUL read refreshes the staleness clock. Stamping on every attempt would make
    // a NAKing sensor look permanently healthy while serving a frozen last-good value.
    if (ok) { good_ms_ = now_ms; good_ = true; }
}

bool Lps2xBaro::valid(uint32_t now_ms) const noexcept {
    return good_ && static_cast<uint32_t>(now_ms - good_ms_) < cfg_.stale_ms;
}

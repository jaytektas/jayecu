// Lps2xBaro — the on-board environmental sensor driver, across BOTH ST parts.
//
// This is worth testing on the host because the two parts differ in four small ways, and
// every one of them fails SILENTLY: get the CTRL_REG1 address wrong and an LPS25 stays in
// power-down; forget the auto-increment bit and a multi-byte read returns the same byte
// three times; share the temperature formula and an LPS25 is out by tens of degrees while
// still looking like a plausible reading. None of that raises an error anywhere.
#include "Platform/devices/Lps2xBaro.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

namespace {
constexpr uint8_t ADDR = 0x5Cu << 1;

struct FakeBus final : II2cBus {
    uint8_t who = 0xB1;
    bool    reads_fail = false;
    bool    writes_fail = false;
    int     read_calls = 0;
    std::vector<std::pair<uint8_t, uint8_t>> writes;   // (reg, value)
    std::vector<uint8_t> read_regs;                    // every sub-address presented
    int32_t pressure_raw = 0;
    int16_t temp_raw = 0;

    bool read_reg(uint8_t dev, uint8_t reg, uint8_t* buf, uint8_t len) noexcept override {
        read_calls++;
        read_regs.push_back(reg);
        if (dev != ADDR || reads_fail) return false;
        const uint8_t bare = reg & 0x7Fu;
        if (bare == 0x0Fu && len == 1) { buf[0] = who; return true; }
        if (bare == 0x28u && len == 3) {
            buf[0] = uint8_t(pressure_raw); buf[1] = uint8_t(pressure_raw >> 8);
            buf[2] = uint8_t(pressure_raw >> 16); return true;
        }
        if (bare == 0x2Bu && len == 2) {
            buf[0] = uint8_t(temp_raw); buf[1] = uint8_t(uint16_t(temp_raw) >> 8); return true;
        }
        return false;
    }
    bool write_reg(uint8_t dev, uint8_t reg, uint8_t value) noexcept override {
        if (dev != ADDR || writes_fail) return false;
        writes.push_back({reg, value});
        return true;
    }
    bool saw_read(uint8_t reg) const {
        for (uint8_t r : read_regs) if (r == reg) return true;
        return false;
    }
};

Lps2xBaro make(FakeBus& bus) {
    Lps2xBaro b;
    b.init({&bus, ADDR, 100u, 500u});
    return b;
}
// 101.3 kPa at 40960 counts/kPa
constexpr int32_t P_101_3 = int32_t(101.3f * 40960.0f);
}  // namespace

int main() {
    // --- LPS22HB: CTRL_REG1 at 0x10, BDU is bit1, NO auto-increment bit -----------------
    {
        FakeBus bus; bus.who = 0xB1; bus.pressure_raw = P_101_3; bus.temp_raw = 2500;  // 25.00 C
        auto b = make(bus);
        b.service(1000);
        CHECK(b.variant() == Lps2xBaro::Variant::Lps22hb, "variant not Lps22hb");
        CHECK(bus.writes.size() == 1, "expected one CTRL_REG1 write, got %zu", bus.writes.size());
        if (!bus.writes.empty()) {
            CHECK(bus.writes[0].first == 0x10u, "CR1 reg = 0x%02X, want 0x10", bus.writes[0].first);
            CHECK(bus.writes[0].second == 0x22u, "CR1 val = 0x%02X, want 0x22 (ODR 10Hz | BDU bit1)",
                  bus.writes[0].second);
        }
        // Read at the BARE address — this is the jaytek bench-proven path.
        CHECK(bus.saw_read(0x28u), "LPS22 pressure not read at bare 0x28");
        CHECK(!bus.saw_read(0xA8u), "LPS22 must NOT set the auto-increment bit");
        CHECK(std::fabs(b.kpa() - 101.3f) < 0.01f, "kpa = %.3f, want 101.3", b.kpa());
        CHECK(std::fabs(b.temp_c() - 25.0f) < 0.01f, "temp = %.3f, want 25.0 (raw/100)", b.temp_c());
        CHECK(b.valid(1000), "should be valid right after a good read");
    }

    // --- LPS25HB: CTRL_REG1 at 0x20, PD must be set, auto-increment bit required ---------
    {
        FakeBus bus; bus.who = 0xBD; bus.pressure_raw = P_101_3; bus.temp_raw = 0;
        auto b = make(bus);
        b.service(1000);
        CHECK(b.variant() == Lps2xBaro::Variant::Lps25hb, "variant not Lps25hb");
        if (!bus.writes.empty()) {
            CHECK(bus.writes[0].first == 0x20u, "CR1 reg = 0x%02X, want 0x20", bus.writes[0].first);
            CHECK(bus.writes[0].second & 0x80u, "CR1 val = 0x%02X, PD (bit7) not set — the part "
                  "would stay in power-down", bus.writes[0].second);
            CHECK(bus.writes[0].second & 0x04u, "CR1 val = 0x%02X, BDU (bit2) not set", bus.writes[0].second);
        }
        // Auto-increment REQUIRED, or the 3-byte read returns one byte three times.
        CHECK(bus.saw_read(0xA8u), "LPS25 pressure must be read at 0x28|0x80");
        CHECK(std::fabs(b.kpa() - 101.3f) < 0.01f, "kpa = %.3f, want 101.3", b.kpa());
        // raw 0 -> 42.5 C on an LPS25; the LPS22 formula would give 0.0.
        CHECK(std::fabs(b.temp_c() - 42.5f) < 0.01f,
              "temp = %.3f, want 42.5 (42.5 + raw/480); the LPS22 formula gives 0.0", b.temp_c());
    }

    // --- an unknown part is refused, not configured -------------------------------------
    {
        FakeBus bus; bus.who = 0x00; bus.pressure_raw = P_101_3;
        auto b = make(bus);
        b.service(1000);
        CHECK(b.variant() == Lps2xBaro::Variant::Unknown, "unknown WHO_AM_I was accepted");
        CHECK(bus.writes.empty(), "configured a part it could not identify");
        CHECK(!b.valid(1000), "an unidentified part must never read valid");
        CHECK(std::fabs(b.kpa() - 101.3f) < 0.01f, "should hold the sea-level default");
    }

    // --- the probe is RETRIED, not one-shot ---------------------------------------------
    // A sensor slow to come up, or a bus busy at boot, must not leave the baro dead until reset.
    {
        FakeBus bus; bus.who = 0xB1; bus.pressure_raw = P_101_3; bus.reads_fail = true;
        auto b = make(bus);
        b.service(1000);
        CHECK(b.variant() == Lps2xBaro::Variant::Unknown, "identified over a failing bus");
        bus.reads_fail = false;
        b.service(1200);                         // past the 100 ms attempt period
        CHECK(b.variant() == Lps2xBaro::Variant::Lps22hb, "probe was not retried after the bus recovered");
        CHECK(b.valid(1200), "should be valid once it identified and read");
    }

    // --- staleness: a frozen cache stops being trusted ----------------------------------
    {
        FakeBus bus; bus.who = 0xB1; bus.pressure_raw = P_101_3;
        auto b = make(bus);
        b.service(1000);
        CHECK(b.valid(1000), "valid after a good read");
        bus.reads_fail = true;
        for (uint32_t t = 1100; t <= 1400; t += 100) b.service(t);
        CHECK(b.valid(1400), "499 ms since the last good read is still inside the 500 ms window");
        b.service(1600);
        CHECK(!b.valid(1600), "600 ms of failures must read INVALID, not a plausible frozen number");
        CHECK(std::fabs(b.kpa() - 101.3f) < 0.01f, "the cached value is still held, just not trusted");
    }

    // --- attempts are rate-limited, so a dead bus is not hammered ------------------------
    {
        FakeBus bus; bus.who = 0xB1; bus.pressure_raw = P_101_3;
        auto b = make(bus);
        b.service(1000);
        const int after_first = bus.read_calls;
        for (uint32_t t = 1001; t < 1100; ++t) b.service(t);   // all inside the 100 ms period
        CHECK(bus.read_calls == after_first, "bus touched %d extra times inside the rate limit",
              bus.read_calls - after_first);
        b.service(1101);
        CHECK(bus.read_calls > after_first, "rate limit never released");
    }

    printf(g_fail ? "baro_lps2x: %d FAILURES\n" : "baro_lps2x: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

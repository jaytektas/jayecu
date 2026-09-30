// The bench output test obeys the key: nothing is driven with the key off.
//
// Every other output lets go of its pin with the key off (OutputManager releases generic outputs, the
// scheduler releases coils and injectors). The output test is the one thing that drives a pin on purpose,
// so it has to keep the same rule: refused while the key is off, and a test already running ends the
// moment the key goes off.
#include "test_helpers.h"
#include "Engine/Modules/OutputTest.h"
#include "Scheduler/PinArbiter.h"
#include "outputs_config.h"

#include <cstdint>
#include <vector>

bool g_system_active = true;

namespace {
struct Pin final : ITimerChannel {
    std::vector<OutputAction> drives;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return 0; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return 1000000; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction a) noexcept override { drives.push_back(a); }
};
}  // namespace

int main() {
    fprintf(stdout, "=== Output test ===\n");
    static Pin pins[OUTPUTS_OUTPUT_COUNT];
    static ITimerChannel* pool[OUTPUTS_OUTPUT_COUNT];
    for (unsigned i = 0; i < OUTPUTS_OUTPUT_COUNT; ++i) pool[i] = &pins[i];
    static PinArbiter arb;
    arb.bind(pool, OUTPUTS_OUTPUT_COUNT);
    static OutputsConfig cfg{};
    for (auto& o : cfg.output) o.active_high = 1;
    static OutputTest t;
    t.bind(&arb, &cfg);
    const uint8_t ROW = 11;                                        // IGN12

    SECTION("key off: a test is refused and the pin is never driven");
    {
        g_system_active = false;
        const bool ok = t.start(ROW, 5, 100, 100, 1000);
        CHECK(!ok);
        CHECK(!t.active());
        CHECK(pins[ROW].drives.empty());
    }

    SECTION("key on: it runs; the key going off ends it at once and hands the pin back");
    {
        g_system_active = true;
        CHECK(t.start(ROW, 5, 100, 100, 2000));
        CHECK(t.active());
        CHECK(!pins[ROW].drives.empty());
        g_system_active = false;
        t.step(true, 2050);
        CHECK(!t.active());
        CHECK(!pins[ROW].drives.empty() && pins[ROW].drives.back() == OutputAction::DRIVE_LOW);   // off first
        CHECK(arb.owner_of(ROW) == PinOwner::FREE);                                              // then released
    }

    return test_summary();
}

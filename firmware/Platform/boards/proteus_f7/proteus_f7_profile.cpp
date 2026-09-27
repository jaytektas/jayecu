#include "Platform/board_profile.h"
#include "board_config.h"
#include "Stm32Capture.h"
#include "Stm32OutputCompare.h"
#include "Scheduler/OutputMap.h"    // OUT_ROW_* — the row layout the output pool must match
#include "Scheduler/IHBridge.h"
#include "Stm32DmaPwm.h"
#include "../../board_hal.h"        // board_hbridge_set_direction / _enable
// The ACTIVE board's constants, through the neutral shim rather than by naming our own
// header: generated/ holds ONE board at a time, so two spellings could disagree.
#include "../../../../generated/boards/board.h"
#ifndef JAYECU_BOARD_IS_PROTEUS_F7
#error "generated/ was built for a different board. Re-run:  make codegen BOARD=proteus_f7"
#endif

// The capture/compare channel objects and their resource tables are GENERATED from
// definition/boards/proteus_f7.board.yaml.
#define BOARD_PINS_EMIT_CHANNELS
#include "proteus_f7_pins.h"

// ---------------------------------------------------------------------------
// proteus_f7 capture pool: VR1/VR2 on EXTI 7/8 (PE7/PE8) and DIG1-6 on EXTI 6 and
// 11-15 (PC6, PE11-PE15). Every capture pin is on a distinct EXTI line, which the
// capture layer requires — note this pool is NOT contiguous and spans two ports, so
// nothing may derive a line from an index.
//
// TIM5 is the shared 32-bit free-running timebase at 1 MHz, as on jaytek.
// ---------------------------------------------------------------------------
static constexpr uint32_t TICKS_PER_SECOND = 1000000u;

static Stm32Timebase s_timebase(TICKS_PER_SECOND);

// The VirtualTrigger DCO — a pin-less alarm on TIM5 CCR1.
static Stm32Alarm s_dco(1);
IAlarmTimer& board_dco_channel() noexcept { return s_dco; }

static const BoardProfile s_profile = {
    /* capture_resources */ s_board_capture_resources,
    /* capture_count     */ static_cast<uint8_t>(sizeof(s_board_capture_resources) /
                                                  sizeof(s_board_capture_resources[0])),
    /* compare_resources */ s_board_compare_resources,
    /* compare_count     */ static_cast<uint8_t>(sizeof(s_board_compare_resources) /
                                                  sizeof(s_board_compare_resources[0])),
    /* ign_compare_count */ BOARD_IGN_COMPARE_COUNT,
    /* timebase          */ &s_timebase,
    /* ticks_per_second  */ TICKS_PER_SECOND,
};

const BoardProfile& board_profile() noexcept { return s_profile; }

// ---------------------------------------------------------------------------
// The output pin pool — ONE ENTRY PER OUTPUT ROW, in row order: IGN(12), LS(16),
// then HS(4) = 32 rows, against jaytek's 42. The static_asserts below hold the pool,
// the generated counts and OutputMap's row bases together, so a mismatch is a build
// error rather than a coil firing an injector.
// ---------------------------------------------------------------------------
static Stm32CompareChannel s_board_hs_cmps[BOARD_HIGH_SIDE_COUNT] = {
    { BOARD_HS_PINS[0].port, BOARD_HS_PINS[0].bit },   // HS1
    { BOARD_HS_PINS[1].port, BOARD_HS_PINS[1].bit },   // HS2
    { BOARD_HS_PINS[2].port, BOARD_HS_PINS[2].bit },   // HS3
    { BOARD_HS_PINS[3].port, BOARD_HS_PINS[3].bit },   // HS4
};

static constexpr uint8_t OUT_POOL_N =
    sizeof(s_board_cmps)    / sizeof(s_board_cmps[0]) +
    sizeof(s_board_hs_cmps) / sizeof(s_board_hs_cmps[0]);
static ITimerChannel* s_output_pool[OUT_POOL_N] = {};
static_assert(OUT_POOL_N == OUTPUTS_OUTPUT_COUNT, "one output row per pool pin");
static_assert(BOARD_IGNITION_COUNT == MAX_IGN_CHANNELS && OUT_ROW_LS_BASE == BOARD_IGNITION_COUNT,
              "the ignition rows are the first MAX_IGN_CHANNELS rows");
static_assert(BOARD_LOW_SIDE_COUNT == MAX_INJ_CHANNELS &&
              OUT_ROW_HS_BASE == BOARD_IGNITION_COUNT + BOARD_LOW_SIDE_COUNT,
              "the low-side rows follow the ignition rows");

void board_build_output_pool() noexcept {
    uint8_t n = 0;
    for (auto& c : s_board_cmps)    s_output_pool[n++] = &c;   // 0..27  IGN+LS (firing sinks)
    for (auto& c : s_board_hs_cmps) s_output_pool[n++] = &c;   // 28..31 HS
}

ITimerChannel** board_output_pool(uint8_t& n) noexcept { n = OUT_POOL_N; return s_output_pool; }

// ---------------------------------------------------------------------------
// H-bridge adapters — TLE9201 on PD12/PD13 (vs jaytek's IFX9201 on PD6/PD3). Both
// parts take the same DIS/DIR/PWM triple and the same DIS-LOW-is-enabled sense, so
// only the PINS differ, and Stm32DmaPwm now takes those as arguments.
//
// PD12/PD13 DO have a timer AF here (TIM4 CH1/CH2), but TIM4 is the SoftPwm tick, so
// the magnitude is driven the same way jaytek drives its AF-less pins: TIM1 -> DMA ->
// GPIOD BSRR. One mechanism for both boards, and it is the one that runs the 10-25 kHz
// that kills ETB whine.
// ---------------------------------------------------------------------------
class ProteusHBridge final : public IHBridge {
public:
    void bind(Stm32DmaPwm& eng, int ch, uint8_t idx) noexcept {
        eng_ = &eng; ch_ = ch; idx_ = idx;
        board_hbridge_set_enable(idx_, false);            // DIS high — start disabled
    }
    void set_freq(uint32_t hz) noexcept override { if (eng_) eng_->set_freq(hz); }
    void drive(float s) noexcept override {
        if (s > 100.0f) s = 100.0f; else if (s < -100.0f) s = -100.0f;
        board_hbridge_set_direction(idx_, s >= 0.0f);
        if (eng_) eng_->set_duty(ch_, s < 0.0f ? -s : s);
    }
    void enable(bool on) noexcept override {
        if (on  && eng_) eng_->start();
        if (!on && eng_) eng_->set_duty(ch_, 0.0f);
        board_hbridge_set_enable(idx_, on);
    }
private:
    Stm32DmaPwm* eng_ = nullptr;
    int          ch_  = 0;
    uint8_t      idx_ = 0;
};

static Stm32DmaPwm     s_hbridge_dma;
static ProteusHBridge  s_hbridge_a;
static ProteusHBridge  s_hbridge_b;

void board_hbridges(IHBridge*& a, IHBridge*& b) noexcept {
    // The bit numbers come from the generated pin map, so the board yaml is the only
    // place PD12/PD13 is written down.
    s_hbridge_dma.init(20000, BOARD_HBRIDGE_PWM_PINS[0].bit, BOARD_HBRIDGE_PWM_PINS[1].bit);
    s_hbridge_a.bind(s_hbridge_dma, Stm32DmaPwm::CH_A, 0);
    s_hbridge_b.bind(s_hbridge_dma, Stm32DmaPwm::CH_B, 1);
    a = &s_hbridge_a;
    b = &s_hbridge_b;
}

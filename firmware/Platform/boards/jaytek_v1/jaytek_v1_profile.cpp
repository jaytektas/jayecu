#include "Platform/board_profile.h"
#include "board_config.h"
#include "Stm32Capture.h"
#include "Stm32OutputCompare.h"
#include "Scheduler/OutputMap.h"   // OUT_ROW_* — the row layout the output pool must match
#include "Scheduler/IHBridge.h"     // IHBridge (the H-bridge actuator seam)
#include "Stm32DmaPwm.h"            // Stm32DmaPwm (DMA-PWM engine for the bridge magnitude)
#include "../../board_hal.h"        // board_hbridge_set_direction / _enable (DIR/DIS HAL)

// The capture/compare channel objects (s_board_caps/s_board_cmps) and their
// resource tables (s_board_capture_resources/s_board_compare_resources) are
// GENERATED from schema/boards/jaytek_v1.board.yaml. Defining the macro pulls
// those definitions in (the types above must already be in scope).
#define BOARD_PINS_EMIT_CHANNELS
#include "jaytek_v1_pins.h"

// ---------------------------------------------------------------------------
// jaytek_v1 capture pool.
//
// Every edge-capable input this board provides, one Stm32CaptureChannel each:
//   VR1/VR2  — MAX9924 (mode B) conditioned, on EXTI0/EXTI1 (PE0/PE1)
//   DIG1-8   — 74HC2G17 Schmitt buffered, on EXTI8..15 (PD8..PD15)
// All ~30 V tolerant; all on distinct EXTI lines by design. No pin is bound to
// a function here — the resolver assigns roles from the tune at runtime.
//
// TIM5 is the shared 32-bit free-running timebase (reserved for this layer);
// 1 MHz / 1 µs ticks — ample for trigger decode, bump later if output-compare
// timing wants finer resolution.
// ---------------------------------------------------------------------------

static constexpr uint32_t TICKS_PER_SECOND = 1000000u;

static Stm32Timebase s_timebase(TICKS_PER_SECOND);

// The VirtualTrigger DCO = a pin-less alarm on TIM5 CCR1 ("call me at the next
// virtual-tooth tick"). Self-registers into the TIM5 ISR demux on construction.
static Stm32Alarm s_dco(1);
IAlarmTimer& board_dco_channel() noexcept { return s_dco; }

// The output-compare pool (IGN1-12, LS1-22) and capture pool (VR1/VR2, DIG1-8),
// with their channel objects and resource tables, are generated from the board
// schema — see s_board_{caps,cmps,capture_resources,compare_resources} pulled in
// from jaytek_v1_pins.h above. IX4427 gate drivers (IGN) and VNLD5090 low-sides
// (LS) are non-inverting: DRIVE_HIGH = dwell / injector-open. The resolver splits
// the compare pool at ign_compare_count = BOARD_IGN_COMPARE_COUNT (12).

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
// The output pin pool — ONE ENTRY PER OUTPUT ROW (outputs.output[i] IS pin i).
//
// IGN1-12 + LS1-22 already have GPIO compare-channel sinks (s_board_cmps[0..33], the firing pool); the 8
// HS pins have no channel object, so wrap them here. The pool is the board's pins_with("TACH_OUTPUT")
// declaration order — IGN(12), LS(22), HS(8) — which is the row order codegen sizes the output array by
// and the order OutputMap.h's OUT_ROW_* bases name. The static_asserts below hold the three together.
static Stm32CompareChannel s_board_hs_cmps[8] = {
    { BOARD_HS_PINS[0].port, BOARD_HS_PINS[0].bit },   // HS1
    { BOARD_HS_PINS[1].port, BOARD_HS_PINS[1].bit },   // HS2
    { BOARD_HS_PINS[2].port, BOARD_HS_PINS[2].bit },   // HS3
    { BOARD_HS_PINS[3].port, BOARD_HS_PINS[3].bit },   // HS4
    { BOARD_HS_PINS[4].port, BOARD_HS_PINS[4].bit },   // HS5
    { BOARD_HS_PINS[5].port, BOARD_HS_PINS[5].bit },   // HS6
    { BOARD_HS_PINS[6].port, BOARD_HS_PINS[6].bit },   // HS7
    { BOARD_HS_PINS[7].port, BOARD_HS_PINS[7].bit },   // HS8
};

// NOTE: PD6/PD3 (HBRIDGE1/2_PWM) are NOT in the user output pool — they are
// board-LOCKED hbridge hardware, driven DIRECTLY by the DMA-PWM engine (Stm32DmaPwm), same as the
// DIR/DIS pins. None of the six hbridge pins is a user-assignable output, so none can be stolen — the
// board "claims" them by simply not offering them. See board_hbridges() + _board_output_pin_count.


// The output pool for the PinArbiter, in ROW order: IGN+LS (the firing sinks), then HS. The hbridge PWM
// pins are board-locked and NOT in this pool.
static constexpr uint8_t OUT_POOL_N =
    sizeof(s_board_cmps)              / sizeof(s_board_cmps[0]) +
    sizeof(s_board_hs_cmps)           / sizeof(s_board_hs_cmps[0]);
static ITimerChannel* s_output_pool[OUT_POOL_N] = {};
static_assert(OUT_POOL_N == OUTPUTS_OUTPUT_COUNT, "one output row per pool pin");
static_assert(BOARD_IGNITION_COUNT == MAX_IGN_CHANNELS && OUT_ROW_LS_BASE == BOARD_IGNITION_COUNT,
              "the ignition rows are the first MAX_IGN_CHANNELS rows");
static_assert(BOARD_LOW_SIDE_COUNT == MAX_INJ_CHANNELS && OUT_ROW_HS_BASE == BOARD_IGNITION_COUNT + BOARD_LOW_SIDE_COUNT,
              "the low-side rows follow the ignition rows");

void board_build_output_pool() noexcept {
    uint8_t n = 0;
    for (auto& c : s_board_cmps)    s_output_pool[n++] = &c;   // 0..33  IGN+LS (firing sinks)
    for (auto& c : s_board_hs_cmps) s_output_pool[n++] = &c;   // 34..41 HS
}

// ---------------------------------------------------------------------------
// H-bridge IHBridge adapters — the board-specific bridge: magnitude via the DMA-PWM engine (TIM1 ->
// DMA2 -> GPIOD BSRR on PD6/PD3, hardware-timed at the configured carrier — runs the 10-25 kHz that
// kills ETB whine, which SoftPwm could not), DIR/DIS via the locked board hbridge HAL. ONE shared
// Stm32DmaPwm engine drives both bridges: one carrier, independent duty per channel. Per-bridge enable
// is the DIS pin, so the carrier keeps running for both while either is enabled (start() is idempotent;
// we never stop it per-bridge — DIS is the gate). HBridge drives these; it never names a pin.
// The engine prescales per frequency, so the full 1 Hz..50 kHz range works (slow valve AND ETB whine).
// ---------------------------------------------------------------------------
class Jaytek1HBridge final : public IHBridge {
public:
    void bind(Stm32DmaPwm& eng, int ch, uint8_t idx) noexcept {
        eng_ = &eng; ch_ = ch; idx_ = idx;
        board_hbridge_set_enable(idx_, false);            // DIS high — start disabled
    }
    void set_freq(uint32_t hz) noexcept override { if (eng_) eng_->set_freq(hz); }   // shared carrier
    void drive(float s) noexcept override {
        if (s > 100.0f) s = 100.0f; else if (s < -100.0f) s = -100.0f;
        board_hbridge_set_direction(idx_, s >= 0.0f);     // sign -> DIR
        if (eng_) eng_->set_duty(ch_, s < 0.0f ? -s : s); // magnitude -> DMA duty
    }
    void enable(bool on) noexcept override {
        if (on  && eng_) eng_->start();                   // idempotent; carrier serves both bridges
        if (!on && eng_) eng_->set_duty(ch_, 0.0f);       // leave the engine running; DIS does the gating
        board_hbridge_set_enable(idx_, on);               // DIS low = enabled (per-bridge gate)
    }
private:
    Stm32DmaPwm* eng_ = nullptr;
    int          ch_  = 0;
    uint8_t      idx_ = 0;
};

static Stm32DmaPwm    s_hbridge_dma;
static Jaytek1HBridge s_hbridge_a;
static Jaytek1HBridge s_hbridge_b;

void board_hbridges(IHBridge*& a, IHBridge*& b) noexcept {
    // Configure TIM1+DMA2+GPIOD now (no DMA running yet — start() is lazy on first enable, so a
    // default-disabled actuator never spins the DMA). The carrier is overridden from cfg via set_freq.
    // PD6/PD3 are board-LOCKED — they are NOT in the output pool/resolver, so no runtime output can
    // ever target them (the board owns all six hbridge pins, like battery owns its sense pin).
    s_hbridge_dma.init(20000, 6 /*PD6*/, 3 /*PD3*/);
    s_hbridge_a.bind(s_hbridge_dma, Stm32DmaPwm::CH_A, 0);   // bridge A -> PD6
    s_hbridge_b.bind(s_hbridge_dma, Stm32DmaPwm::CH_B, 1);   // bridge B -> PD3
    a = &s_hbridge_a;
    b = &s_hbridge_b;
}
ITimerChannel** board_output_pool(uint8_t& n) noexcept { n = OUT_POOL_N; return s_output_pool; }

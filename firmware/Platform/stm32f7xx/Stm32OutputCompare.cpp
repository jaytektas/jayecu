#include "Stm32OutputCompare.h"

// ---------------------------------------------------------------------------
// Output pins are dumb GPIO sinks (firing timing lives in the Stm32Alarms). They
// are NOT configured at boot: a pin is switched from its reset Hi-Z state to a
// driven push-pull output only when the scheduler binds it (enable_output), so an
// unassigned pool channel never actively drives a line.
// ---------------------------------------------------------------------------
static uint32_t             s_tps                 = 1000000u;

// Alarms on TIM5 CCR1..CCR4, indexed by CCR channel ([0] unused). Registered by
// the Stm32Alarm ctor, demuxed in the ISR. CCR1 = DCO, CCR2 = angle, CCR3 = time.
static Stm32Alarm*          s_alarm[5]            = { nullptr };

// ---------------------------------------------------------------------------
// TIM5 compare ISR — pure alarm demux (no scan). Service a channel when its CCnIF
// is set OR the alarm is armed-and-due: the past-deadline arm pends the IRQ
// WITHOUT CCnIF (the HW compare won't recur until a 32-bit wrap), so the flag
// alone is insufficient. One-shot; each on_match's callback re-arms for the next
// head. Hard-real-time, OS-decoupled.
// ---------------------------------------------------------------------------
// FRESH FLAGS PER CHANNEL. SR used to be read once on entry. CC1 (the virtual tooth) is serviced first
// and re-arms CC2 for the new tooth's first event — but the CC2IF captured on entry belonged to the OLD
// event, so on_match() then fired the NEW one at once, up to a tooth early. Reading SR and CNT again for
// each channel sees the re-arm (arm() clears the flag and moves the deadline).
//
// ARMED ONLY. The compare hardware sets CCnIF on a match whether or not the interrupt is enabled, so a
// disarmed channel whose old CCR comes round again raises a flag for an event that no longer exists.
// Such a flag is cleared and ignored.
static inline void service_alarm(uint8_t ch) {
    Stm32Alarm* a = s_alarm[ch];
    if (!a) return;
    const uint32_t flag = 1u << ch;                    // TIM_SR_CCnIF == (1 << n)
    const uint32_t sr   = TIM5->SR;
    if (!a->armed()) { if (sr & flag) TIM5->SR = ~flag; return; }
    if ((sr & flag) || a->is_due(TIM5->CNT)) { TIM5->SR = ~flag; a->on_match(); }
}

extern "C" void TIM5_IRQHandler(void) {
    service_alarm(1);
    service_alarm(2);
    service_alarm(3);
    service_alarm(4);
}

// ---------------------------------------------------------------------------
// Stm32Alarm — one-shot "call me at tick X" on TIM5 CCRn (n = 2,3,4). CCR1..CCR4
// are contiguous uint32 fields; DIER/SR channel bits are (1<<n). One pending
// deadline; the callback re-arms for the next list head (no soonest-scan).
// ---------------------------------------------------------------------------
Stm32Alarm::Stm32Alarm(uint8_t ccr_channel) noexcept
    : ch_(ccr_channel), cb_(nullptr), cb_data_(nullptr), deadline_(0), armed_(false)
{
    if (ch_ >= 1 && ch_ <= 4) s_alarm[ch_] = this;
}

uint32_t Stm32Alarm::now() const noexcept { return TIM5->CNT; }

void Stm32Alarm::arm(uint32_t abs_ticks) noexcept {
    if (ch_ < 1 || ch_ > 4) return;
    volatile uint32_t* ccr = &TIM5->CCR1 + (ch_ - 1);   // CCR1..CCR4 contiguous
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    *ccr        = abs_ticks;
    deadline_   = abs_ticks;
    TIM5->SR    = ~(1u << ch_);          // clear stale CCnIF (rc_w0)
    TIM5->DIER |= (1u << ch_);           // enable CCnIE
    armed_      = true;
    // Already due/past: the hardware match won't recur until the 32-bit counter
    // wraps, so pend the IRQ to service it immediately.
    if ((int32_t)(abs_ticks - TIM5->CNT) <= (int32_t)MIN_TICKS_AHEAD) {
        NVIC_SetPendingIRQ(TIM5_IRQn);
    }
    __set_PRIMASK(pm);
}

void Stm32Alarm::disarm() noexcept {
    if (ch_ < 1 || ch_ > 4) return;
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    TIM5->DIER &= ~(1u << ch_);
    TIM5->SR    = ~(1u << ch_);          // and drop a match already flagged: it is for no event now
    armed_ = false;
    __set_PRIMASK(pm);
}

void Stm32Alarm::on_match() noexcept {
    TIM5->DIER &= ~(1u << ch_);          // one-shot: disable until re-armed
    armed_ = false;
    if (cb_) cb_(cb_data_);              // scheduler callback re-arms for the next head
}

// ---------------------------------------------------------------------------
// Stm32CompareChannel
// ---------------------------------------------------------------------------
Stm32CompareChannel::Stm32CompareChannel(GPIO_TypeDef* port, uint8_t pin) noexcept
    : port_(port), pin_mask_(1u << pin)
{
    // No self-registration / no boot pin-init: the pin stays Hi-Z until the
    // scheduler binds this channel and calls enable_output().
}

uint32_t Stm32CompareChannel::get_ticks_per_second() const noexcept { return s_tps; }

void Stm32CompareChannel::force_output_now(OutputAction action) noexcept {
    if (port_ == nullptr) return;   // pinless channel — no GPIO
    switch (action) {
        case OutputAction::DRIVE_HIGH: port_->BSRR = pin_mask_;        break;
        case OutputAction::DRIVE_LOW:  port_->BSRR = pin_mask_ << 16;  break;  // BR bits
        case OutputAction::TOGGLE:     port_->ODR ^= pin_mask_;        break;
        case OutputAction::NO_CHANGE:
        default:                                                       break;
    }
}

void Stm32CompareChannel::enable_output(OutputAction idle_level) noexcept {
    if (port_ == nullptr) return;           // pinless channel — nothing to init
    // Preset ODR to the idle level BEFORE switching the pin to output, so it never
    // glitches through the wrong level. Active-high board: idle = LOW; active-low: HIGH.
    if (idle_level == OutputAction::DRIVE_HIGH) port_->BSRR = pin_mask_;
    else                                         port_->BSRR = pin_mask_ << 16;
    GPIO_InitTypeDef g = {};
    g.Pin   = pin_mask_;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port_, &g);
}

void Stm32CompareChannel::disable_output() noexcept {
    if (port_ == nullptr) return;
    GPIO_InitTypeDef g = {};        // back to Hi-Z: input, no pull (board pull holds safe)
    g.Pin  = pin_mask_;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(port_, &g);
}

// ---------------------------------------------------------------------------
extern "C" void Stm32OutputCompare_Init(uint32_t ticks_per_second) {
    s_tps = ticks_per_second ? ticks_per_second : 1000000u;

    // NOTE: output pins are intentionally NOT configured here. They stay in their
    // reset Hi-Z (input) state and are switched to driven push-pull outputs only
    // when the scheduler binds them (enable_output) — an unassigned pool pin never
    // actively drives a line.

    // TIM5 counter is already free-running (Stm32Capture_Init). Arm the CCR1
    // compare interrupt path; it stays disabled until the first event is armed.
    TIM5->DIER &= ~TIM_DIER_CC1IE;
    TIM5->SR    = ~TIM_SR_CC1IF;

    // Priority 2: above configMAX_SYSCALL_INTERRUPT_PRIORITY (5) so no critical
    // section can delay a coil/injector edge; just below the capture ISR (1).
    HAL_NVIC_SetPriority(TIM5_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM5_IRQn);
}

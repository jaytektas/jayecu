#include "SdArbitrator.h"

namespace {
// ECU writers must finish + close within this once cleanup begins; the bus is
// handed to USB afterwards regardless, as a backstop against a stuck writer.
constexpr uint32_t RELEASE_TIMEOUT_MS  = 3000;
// Host must be block-write idle this long before we withdraw the card.
constexpr uint32_t USB_QUIET_MS        = 300;
// Last-resort forced withdraw if the host never goes quiet (raises a flag so
// the caller can log a fault). Never fires while the host holds a removal lock.
constexpr uint32_t WITHDRAW_TIMEOUT_MS = 8000;
}  // namespace

// Apply comms SD_MCU / SD_MSC / SD_RELEASE command. SD_MCU bypasses USB_QUIET_MS because
// the studio unmounts the filesystem via the protocol before sending this command — the ECU
// can take the card immediately without waiting for block-write quiescence.
void SdArbitrator::set_comms_override(SdCommsOverride o, uint32_t now_ms)
{
    comms_override_ = o;
    if (o == SdCommsOverride::MCU) {
        // Immediately start transition to ECU_OWNED from whatever state we're in.
        switch (phase_) {
        case Phase::USB_OWNED:
        case Phase::USB_WITHDRAWING:
            take_back_from_usb();          // host has already unmounted — take it now
            break;
        default: break;                    // already ECU — nothing to do
        }
    } else if (o == SdCommsOverride::MSC) {
        // Hand the card to USB regardless of key (begin the ECU→USB release cycle).
        if (phase_ == Phase::ECU_OWNED) {
            phase_ = Phase::ECU_RELEASING;
            phase_since_ms_ = now_ms;
        }
    }
    // NONE: clear override; next service() tick resumes normal key-driven logic.
}

void SdArbitrator::service(bool key_on, uint32_t now_ms) {
    // Comms overrides take priority over the key state. SD_MCU/SD_MSC were handled
    // in set_comms_override(); here we just suppress normal key-driven transitions while
    // an override is active (so a key-off on bench doesn't undo an SD_MCU take).
    if (comms_override_ == SdCommsOverride::MCU) {
        // Hold ECU ownership — resist key-driven release until override is cleared.
        if (phase_ != Phase::ECU_OWNED && phase_ != Phase::ECU_RELEASING)
            take_back_from_usb();
        return;
    }
    if (comms_override_ == SdCommsOverride::MSC) {
        // Hold USB ownership — ECU_RELEASING continues normally (writers must drain).
        // Once USB_OWNED, stay there until override is cleared.
        switch (phase_) {
        case Phase::ECU_OWNED:
            phase_ = Phase::ECU_RELEASING; phase_since_ms_ = now_ms; break;
        case Phase::ECU_RELEASING:
            if (busy_mask_ == 0u ||
                (uint32_t)(now_ms - phase_since_ms_) >= RELEASE_TIMEOUT_MS)
                phase_ = Phase::USB_OWNED;
            break;
        default: break;   // USB_OWNED / USB_WITHDRAWING → stay
        }
        return;
    }

    // Normal key-driven path (comms_override_ == NONE).
    switch (phase_) {

    case Phase::ECU_OWNED:
        if (!key_on) { phase_ = Phase::ECU_RELEASING; phase_since_ms_ = now_ms; }
        break;

    case Phase::ECU_RELEASING:
        if (key_on) { phase_ = Phase::ECU_OWNED; break; }
        if (busy_mask_ == 0u ||
            (uint32_t)(now_ms - phase_since_ms_) >= RELEASE_TIMEOUT_MS)
            phase_ = Phase::USB_OWNED;
        break;

    case Phase::USB_OWNED:
        if (key_on) { phase_ = Phase::USB_WITHDRAWING; phase_since_ms_ = now_ms; }
        break;

    case Phase::USB_WITHDRAWING: {
        if (!key_on) { phase_ = Phase::USB_OWNED; break; }
        const bool quiet = (uint32_t)(now_ms - last_usb_write_ms_) >= USB_QUIET_MS;
        const bool flushed = (last_usb_sync_ms_ == 0u)
            ? true
            : (int32_t)(last_usb_sync_ms_ - last_usb_write_ms_) >= 0;
        if (quiet && flushed && !usb_locked_) {
            take_back_from_usb();
        } else if (!usb_locked_ &&
                   (uint32_t)(now_ms - phase_since_ms_) >= WITHDRAW_TIMEOUT_MS) {
            unsafe_withdraw_ = true;
            take_back_from_usb();
        }
        break;
    }
    }
}

#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// SdArbitrator — decides who may drive the SD card over SPI3, and enforces
// safe, corruption-free handoffs between the ECU and the USB MSC host.
//
// THE RULE (key-driven, with comms override):
//   • Key OFF → card is a USB MSC drive; ECU leaves SPI3 alone.
//   • Key ON  → card belongs to ECU; MSC withdrawn (host sees "medium not present").
//   • Comms SD_MCU override → ECU takes the card immediately (host must have already
//     unmounted via the protocol command; bypasses USB_QUIET wait).
//   • Comms SD_MSC override → card given to USB regardless of key state.
//   • Comms SD_RELEASE → clears overrides; key-state-driven again.
//
// Exactly one side ever touches SPI3 — the SD SPI layer has no cross-context
// mutex, so concurrent access corrupts transfers. Ownership is the SOLE gate;
// USB enumeration, bus resets and engine RPM do NOT change it.
//
// SAFE HANDOFF — neither side is ever yanked mid-write:
//   ECU → USB (key off):  enter ECU_RELEASING. writes_allowed() goes false;
//     every ECU writer (logger, config, fault log, comms, learned) finishes its current
//     write, f_sync + f_close, and clears its busy bit. The ECU keeps the bus
//     the whole time. Only when the busy mask is empty is the card handed to
//     USB. (A logger that holds its file open closes it *in response* to the
//     flag — we never passively wait for it to stop on its own.)
//   USB → ECU (key on):   enter USB_WITHDRAWING. The host keeps the bus so it
//     can finish whatever it is doing; we never force it. We withdraw only once
//     the host has gone block-write quiet (and, on a sync-aware MSC layer, has
//     flushed and unlocked). A long timeout is the only backstop.
//
// Boot: owner defaults to ECU so pre-kernel config sync can read the card.
// service() is called every engine frame once the kernel is running.
// ---------------------------------------------------------------------------

enum class SdOwner { ECU, USB };

// ECU-side writers that may hold the card. Each owns one bit in the busy mask;
// the arbitrator will not release the card to USB until every bit is clear.
enum SdWriter : uint8_t {
    SD_WRITER_LOGGER   = 1u << 0,
    SD_WRITER_CONFIG   = 1u << 1,
    SD_WRITER_FAULTLOG = 1u << 2,
    SD_WRITER_COMMS    = 1u << 3,
    SD_WRITER_LEARNED  = 1u << 4,   // learned-region totem flush (LTFT/LTT)
};

// Comms-layer SD ownership override. Takes priority over key state when set.
// SD_MCU: ECU takes the card now (bypasses USB_QUIET wait — host has unmounted via protocol).
// SD_MSC: USB keeps/gets the card regardless of key.
// NONE:   key-state-driven (normal).
enum class SdCommsOverride : uint8_t { NONE, MCU, MSC };

class SdArbitrator {
public:
    // ---- key-driven ownership (the SOLE decision once the kernel runs) -----
    // key_on = debounced ignition (battery > threshold, see EngineTask).
    // now_ms = monotonic millisecond tick (platform_get_tick_ms()).
    void service(bool key_on, uint32_t now_ms);

    // Comms SD_MCU / SD_MSC / SD_RELEASE protocol commands.
    // SD_MCU: immediately start withdrawing from USB (bypasses USB_QUIET_MS — the host
    //         already unmounted via the protocol choreography before sending this command).
    // SD_MSC: force ECU → USB handoff (tells ECU to release the card).
    // NONE:   clear any override; return to key-driven.
    void set_comms_override(SdCommsOverride o, uint32_t now_ms);
    SdCommsOverride comms_override() const { return comms_override_; }

    // ---- bus-access queries (diskio.c / MSC callbacks) ---------------------
    bool ecu_has_card() const {        // ECU may drive SPI (owns, or finishing)
        return phase_ == Phase::ECU_OWNED || phase_ == Phase::ECU_RELEASING;
    }
    bool usb_has_card() const {        // MSC may drive SPI (owns, or host finishing)
        return phase_ == Phase::USB_OWNED || phase_ == Phase::USB_WITHDRAWING;
    }

    // ---- ECU writer cleanup protocol ---------------------------------------
    // A writer MUST set its busy bit BEFORE checking writes_allowed()/ecu_has_card(),
    // and clear it when done. If the check fails after the bit is set, the writer
    // closes up and clears the bit without touching the card. This ordering is
    // what makes the ECU→USB handoff race-free (see service()).
    bool writes_allowed() const { return phase_ == Phase::ECU_OWNED; }
    void writer_busy(SdWriter w) { busy_mask_ = uint8_t(busy_mask_ |  uint8_t(w)); }
    void writer_idle(SdWriter w) { busy_mask_ = uint8_t(busy_mask_ & ~uint8_t(w)); }

    // ---- USB MSC flush signalling (called from MSC SCSI callbacks) ----------
    // Beacon every host block write so service() can wait for quiescence.
    void usb_note_write(uint32_t now_ms) { last_usb_write_ms_ = now_ms; }
    // Reserved for a sync-aware (vendored) MSC layer surfacing SCSI 0x35 /
    // 0x1E. Unused by the stock middleware; harmless to leave wired.
    void usb_note_sync(uint32_t now_ms)  { last_usb_sync_ms_ = now_ms; }
    void usb_set_locked(bool locked)     { usb_locked_ = locked; }

    // If a withdrawal had to be forced by timeout before the host confirmed it
    // was idle, this returns true once (so the caller can log a fault).
    bool consume_unsafe_withdraw() { bool f = unsafe_withdraw_; unsafe_withdraw_ = false; return f; }

    SdOwner owner() const { return ecu_has_card() ? SdOwner::ECU : SdOwner::USB; }

    // Bumped every time the card comes BACK from USB. The PC may have changed anything on it, so a
    // FatFs mount made before then describes a card that no longer exists. SdVolume remounts on change.
    uint32_t ecu_epoch() const { return ecu_epoch_; }

    // SD status query — compact descriptor for the OMNI_CMD_SD_STATUS response.
    struct Status {
        uint8_t phase;           // 0=ECU_OWNED 1=ECU_RELEASING 2=USB_OWNED 3=USB_WITHDRAWING
        uint8_t comms_override;  // SdCommsOverride: 0=NONE 1=MCU 2=MSC
        uint8_t card_present;    // SdCard_IsAvailable()
        uint8_t ecu_has;         // ecu_has_card()
        uint8_t usb_has;         // usb_has_card()
        uint8_t busy_mask;       // ECU writer busy bits
        uint8_t unsafe_withdraw; // was last withdraw forced?
        uint8_t pad;
    };
    Status status_snapshot() const {
        return Status{
            static_cast<uint8_t>(phase_),
            static_cast<uint8_t>(comms_override_),
            0, 0, 0,                            // card_present/ecu_has/usb_has filled by caller
            busy_mask_,
            unsafe_withdraw_,
            0
        };
    }

private:
    enum class Phase : uint8_t { ECU_OWNED, ECU_RELEASING, USB_OWNED, USB_WITHDRAWING };

    // Boot: ECU owns the card so pre-kernel config sync can read it.
    volatile Phase           phase_             = Phase::ECU_OWNED;
    volatile uint8_t         busy_mask_         = 0;
    volatile uint32_t        last_usb_write_ms_ = 0;
    volatile uint32_t        last_usb_sync_ms_  = 0;
    volatile bool            usb_locked_        = false;
    volatile bool            unsafe_withdraw_   = false;
    SdCommsOverride          comms_override_    = SdCommsOverride::NONE;
    uint32_t                 phase_since_ms_    = 0;
    volatile uint32_t        ecu_epoch_         = 0;

    void take_back_from_usb() { phase_ = Phase::ECU_OWNED; ecu_epoch_ = ecu_epoch_ + 1u; }
};

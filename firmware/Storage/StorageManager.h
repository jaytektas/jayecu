#pragma once
#include "ConfigBank.h"
#include "SdArbitrator.h"
#include "IConfigStore.h"

enum class BootResult {
    OK_FLASH_A,    // Flash bank A was the winner
    OK_FLASH_B,    // Flash bank B was the winner
    OK_SD,         // SD won, written to inactive bank, then loaded from flash
    OK_DEFAULT,    // No valid config anywhere — caller should apply defaults
    ERR_VERIFY,    // SD won but re-verify after programming flash failed
};

enum class BurnResult {
    OK_SD,         // Written to SD card (glitch-free; flash syncs at next boot)
    OK_FLASH,      // Written to the inactive flash bank now (and active swapped)
    ERR,           // Write failed
};

// Orchestrates boot arbitration and config persistence across two flash banks
// and an optional SD card store.
//
// Boot arbitration (runs in main() BEFORE the scheduler/engine start):
//   Compares sequence numbers from bank A, bank B, and (if available) SD.
//   Highest valid sequence wins. If SD is newer, it is programmed into the
//   inactive flash bank and swapped active — flash catches up to SD here,
//   glitch-free because nothing is running yet.
//
// Burn routing:
//   Engine stopped → write flash NOW. Nothing is spinning, so the flash-bus
//                    stall is harmless, and it's immediately persistent. This
//                    is the USB / bench-tuning path — no SD detour needed.
//   Engine running, SD present → write SD only. SD is over SPI, so it never
//                    stalls the flash bus / instruction fetch — glitch-free
//                    while running. Flash reconciles from SD at the next boot.
//   Engine running, no SD → write flash NOW anyway. There is no safe "later":
//                    the engine stopping is almost always key-off, which cuts
//                    ECU power mid-write (corruption). A live flash write stalls
//                    the CPU (an engine glitch) but at least completes.
class StorageManager {
public:
    StorageManager(ConfigBank& bank_a, ConfigBank& bank_b,
                   IConfigStore* sd,    // nullable — pass nullptr if no SD
                   SdArbitrator& arb);

    // Run once at startup. Fills config_out with the winning config data.
    // Returns OK_DEFAULT if no valid config exists anywhere.
    // Choose the newest valid config across bank A / bank B / SD and hand back a POINTER to its
    // payload where it already lies in memory-mapped flash, plus its length. No staging buffer: the
    // CRC is computed in place (ConfigBank::is_valid) and the caller does ONE copy into its own
    // config, after checking whatever else it needs to (size, layout_hash). Returns null on
    // OK_DEFAULT — no valid stored tune, so the caller keeps its compiled defaults untouched.
    //
    // The pointer stays valid until the next burn to that bank.
    // A candidate must satisfy BOTH conditions to be eligible, and only then does sequence decide:
    //
    //   1. it is FOR THIS FIRMWARE — CRC valid, data_len == expect_len, and its layout_hash (the
    //      payload's first 4 bytes, field 0 of EcuConfig) == expect_hash
    //   2. it has the highest sequence among those that qualify
    //
    // Layout is a gate, not a tie-break. Arbitrating on sequence alone lets a stale-layout tune with
    // a higher sequence WIN and then be thrown out by the caller — so a perfectly good older-layout-
    // matching bank never gets a look in, and the ECU silently runs compiled defaults. Bank A's
    // seeded default is always sequence 1, so any burn outranks it forever; that made the failure
    // permanent rather than transient.
    BootResult boot_arbitrate(uint32_t expect_hash, uint32_t expect_len,
                              const uint8_t** payload_out, uint32_t* len_out,
                              uint8_t* scratch = nullptr, uint32_t scratch_len = 0);

    // CALLED IMMEDIATELY BEFORE A BURN THAT WILL STALL THE CPU, and only then.
    //
    // Programming internal flash on a single-bank part blocks ALL instruction fetch for ~1-2 s, so no
    // ISR runs — and this ECU delivers every coil and injector edge FROM an ISR (Stm32Alarm::on_match
    // -> the scheduler's callback -> GPIO BSRR). A pin is therefore frozen at whatever level it held
    // when the erase began: start mid-dwell and the coil stays energised for a thousand times its
    // design dwell, start with an injector open and it stays open.
    //
    // Deferring until the engine stops is the obvious alternative and does not work here, because on
    // this ECU the engine stopping IS the power going off — the burn would simply be lost, which is
    // the failure this whole area has already produced twice. So the engine is DISABLED first and the
    // outputs parked, and only then does the flash go. The hook is what does that; the decision of
    // WHEN lives here, beside the branch that knows a stall is coming, so the two cannot drift.
    void set_prestall_hook(void (*fn)(void*), void* ctx) noexcept { prestall_ = fn; prestall_ctx_ = ctx; }

    // Persist a new config. Routing depends on engine_running (see class
    // comment): stopped → flash now; running → SD if present, else flash now.
    BurnResult burn(const uint8_t* data, uint32_t len,
                    bool engine_running, uint32_t tick_ms);

    // Next sequence number that will be used on the next burn.
    uint32_t next_sequence() const { return next_seq_; }

private:
    ConfigBank&   bank_a_;
    ConfigBank&   bank_b_;
    IConfigStore* sd_;
    SdArbitrator& arb_;

    bool     active_is_a_ = true;
    uint32_t next_seq_ = 1;

    void (*prestall_)(void*) = nullptr;   // see set_prestall_hook
    void*  prestall_ctx_     = nullptr;

    ConfigBank& inactive_bank();

    BurnResult program_inactive_and_swap(const uint8_t* data, uint32_t len,
                                         ConfigSource src, uint32_t tick_ms);
};

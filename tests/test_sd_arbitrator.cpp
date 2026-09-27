#include "test_helpers.h"
#include "../firmware/Storage/SdArbitrator.h"

// Mirrors the timeouts in SdArbitrator.cpp (kept in sync by these tests).
static constexpr uint32_t RELEASE_TIMEOUT_MS  = 3000;
static constexpr uint32_t USB_QUIET_MS        = 300;
static constexpr uint32_t WITHDRAW_TIMEOUT_MS = 8000;

int main() {
    fprintf(stdout, "=== SdArbitrator ===\n");

    SECTION("boot default — ECU owns the card so config sync can read it");
    {
        SdArbitrator arb;
        CHECK(arb.ecu_has_card());
        CHECK(!arb.usb_has_card());
        CHECK(arb.owner() == SdOwner::ECU);
        CHECK(arb.writes_allowed());
    }

    SECTION("key off → clean release → USB owns (no writers busy)");
    {
        SdArbitrator arb;
        arb.service(false, 1000);          // key off: enter ECU_RELEASING
        CHECK(arb.ecu_has_card());         // ECU keeps the bus while releasing
        CHECK(!arb.writes_allowed());      // writers told to finish + close
        arb.service(false, 1001);          // nothing busy → hand off to USB
        CHECK(arb.usb_has_card());
        CHECK(!arb.ecu_has_card());
    }

    SECTION("key off is blocked by a busy writer until it clears (cleanup)");
    {
        SdArbitrator arb;
        arb.writer_busy(SD_WRITER_CONFIG);     // a write is in flight
        arb.service(false, 1000);              // ECU_RELEASING
        arb.service(false, 1100);              // writer still busy → stay ECU
        CHECK(arb.ecu_has_card());
        CHECK(!arb.usb_has_card());
        arb.writer_idle(SD_WRITER_CONFIG);     // writer finished + closed
        arb.service(false, 1200);
        CHECK(arb.usb_has_card());             // now safe to hand off
        CHECK(!arb.ecu_has_card());
    }

    SECTION("a stuck writer is force-released after RELEASE_TIMEOUT_MS");
    {
        SdArbitrator arb;
        arb.writer_busy(SD_WRITER_LOGGER);
        arb.service(false, 0);                 // ECU_RELEASING at t=0
        arb.service(false, 100);
        CHECK(arb.ecu_has_card());             // still waiting
        arb.service(false, RELEASE_TIMEOUT_MS);// backstop fires
        CHECK(arb.usb_has_card());
    }

    SECTION("key on → withdraw waits for host block-write quiescence");
    {
        SdArbitrator arb;
        arb.service(false, 0); arb.service(false, 1);   // → USB_OWNED
        CHECK(arb.usb_has_card());
        arb.usb_note_write(1000);              // host is writing
        arb.service(true, 1100);               // key on: enter USB_WITHDRAWING
        CHECK(arb.usb_has_card());             // host still owns (not quiet)
        CHECK(!arb.ecu_has_card());
        arb.service(true, 1000 + USB_QUIET_MS - 50);   // not quiet long enough
        CHECK(arb.usb_has_card());
        arb.service(true, 1000 + USB_QUIET_MS + 50);   // quiet ≥ threshold
        CHECK(arb.ecu_has_card());             // clean handoff
        CHECK(!arb.usb_has_card());
        CHECK(!arb.consume_unsafe_withdraw()); // it was a clean, safe withdraw
    }

    SECTION("withdraw is forced (and flagged) if the host never goes quiet");
    {
        SdArbitrator arb;
        arb.service(false, 0); arb.service(false, 1);   // USB_OWNED
        arb.service(true, 100);                          // USB_WITHDRAWING, since=100
        for (uint32_t t = 200; t < WITHDRAW_TIMEOUT_MS; t += 200) {
            arb.usb_note_write(t);             // host keeps writing
            arb.service(true, t);
        }
        CHECK(arb.usb_has_card());             // still withdrawing (host busy)
        arb.usb_note_write(100 + WITHDRAW_TIMEOUT_MS - 10);
        arb.service(true, 100 + WITHDRAW_TIMEOUT_MS + 1);
        CHECK(arb.ecu_has_card());             // forced over
        CHECK(arb.consume_unsafe_withdraw());  // flagged for fault logging
        CHECK(!arb.consume_unsafe_withdraw()); // one-shot
    }

    SECTION("a sync-aware host (note_sync) must flush before clean withdraw");
    {
        SdArbitrator arb;
        arb.service(false, 0); arb.service(false, 1);   // USB_OWNED
        arb.usb_note_write(1000);
        arb.service(true, 1100);               // USB_WITHDRAWING
        // Quiet long enough, but last sync is BEFORE last write → not flushed.
        arb.usb_note_sync(900);
        arb.service(true, 2000);
        CHECK(arb.usb_has_card());             // held: host hasn't flushed
        arb.usb_note_sync(2001);               // host flushed past its last write
        arb.service(true, 2002);
        CHECK(arb.ecu_has_card());             // now clean
    }

    SECTION("key returns during release → stays ECU, writes resume");
    {
        SdArbitrator arb;
        arb.writer_busy(SD_WRITER_CONFIG);
        arb.service(false, 0);                 // ECU_RELEASING
        arb.service(true, 100);                // key back → ECU_OWNED
        CHECK(arb.ecu_has_card());
        CHECK(arb.writes_allowed());
    }

    SECTION("key drops during withdraw → stays USB");
    {
        SdArbitrator arb;
        arb.service(false, 0); arb.service(false, 1);   // USB_OWNED
        arb.service(true, 100);                // USB_WITHDRAWING
        arb.service(false, 150);               // key gone again → USB_OWNED
        CHECK(arb.usb_has_card());
        CHECK(!arb.ecu_has_card());
    }

    SECTION("invariant — ECU and USB never own the card simultaneously");
    {
        SdArbitrator arb;
        for (uint32_t t = 0; t < 30000; t += 137) {
            const bool key = (t / 2000) % 2;   // toggle key every ~2 s
            if ((t % 411) == 0) arb.usb_note_write(t);
            arb.service(key, t);
            CHECK(!(arb.ecu_has_card() && arb.usb_has_card()));
        }
    }

    SECTION("ecu_epoch moves only when the card comes back from USB (SdVolume remounts on it)");
    {
        SdArbitrator arb;
        const uint32_t e0 = arb.ecu_epoch();
        arb.service(true, 10);                       // key on, ECU keeps it
        arb.service(false, 20);                      // releasing...
        arb.service(true, 21);                       // ...key back on before handover: never left the ECU
        CHECK(arb.ecu_epoch() == e0);

        arb.service(false, 100);
        arb.service(false, 101);                     // USB owns
        CHECK(arb.usb_has_card());
        arb.service(true, 200);                      // withdraw, host quiet
        arb.service(true, 200 + USB_QUIET_MS + 1);
        CHECK(arb.ecu_has_card());
        CHECK(arb.ecu_epoch() == e0 + 1);            // the PC had it: remount

        arb.set_comms_override(SdCommsOverride::MSC, 1000);
        arb.service(true, 1001);
        CHECK(arb.usb_has_card());
        arb.set_comms_override(SdCommsOverride::MCU, 1100);
        CHECK(arb.ecu_has_card());
        CHECK(arb.ecu_epoch() == e0 + 2);
    }

    return test_summary();
}

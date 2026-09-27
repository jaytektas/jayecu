#pragma once

// FirmwareUpgrade — put a firmware kit on the connected ECU and bring the tune across, by name.
//
// It runs between the ECU saying who it is and the studio loading anything for it, so none of the
// studio's usual connect happens against firmware that is about to be replaced. In order:
//
//   1. READ         the tune off the ECU (a read changes nothing), with the ECU's CURRENT meta — which
//                   the studio must have; the tune cannot be read by name without it.
//   2. OFFER        migrate it into the new meta as a dry run and show what changes: what this tune has
//                   set that the new firmware drops, and what the new firmware adds, each on its own
//                   pages, highlighted (FirmwareChangesDialog). That report IS the question — Update
//                   firmware / Not now. "Not now" writes nothing and the connect carries on.
//   3. SAFE?        ignition off and engine stopped, from live data arriving AFTER the answer.
//   PERMISSION      the USB rule (Linux) or the WinUSB driver (Windows), through the system's own
//                   administrator prompt if missing.
//                   Before DFU, never after: a bootloader nobody may open is a board stuck in DFU.
//   BACKUP          the tune, by name, beside the ECU's others.
//   4. FLASH        the `dfu` command, then erase / write / verify / leave over USB (comms/Dfu.h). A
//                   failure leaves the ECU waiting in its bootloader, and the studio offers to retry.
//   5. CHECK        reconnect; the ECU must now report the kit's version and layout.
//   6. PUSH         write the migrated tune, read it back and compare, burn, reset (the firmware's
//                   `reset` waits for the burn to land).
//   7. SD CARD      the new meta and dashboard are copied onto the ECU's SD card under the names a studio
//                   looks for ("<board> <layout_hash>.meta/.gui"), so ANOTHER studio that has never seen
//                   this firmware can fetch them from the ECU itself. No card: skipped, and said so.
//   8. HAND BACK    the migrated tune is saved as the ECU's active tune, and the normal connect runs —
//                   which then finds the studio and the ECU in sync.
//
// RECOVERY (recover()) is the same sequence for an ECU found waiting in its bootloader — a new board, or
// one whose firmware stopped running. Steps 1-3 and the backup need firmware that is running, so they
// cannot happen; from 4 on it is an update. At 6 the tune is read off the NEW firmware: a tune it booted
// with (one that suits it) stays, and an ECU with none gets the firmware's default tune, as above.
//
// The studio's side (dialogs, progress, the link's signals) is passed in, so this class is only the
// sequence.

#include "../comms/EcuLink.h"
#include "../model/FirmwareKits.h"
#include "../model/MetaModel.h"
#include "../model/TuneDiff.h"
#include "../model/TuneFile.h"

class PanelLibrary;

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class FirmwareUpgrade {
public:
    struct Ui {
        std::function<void(const std::string&)> status;                                // status-line text
        std::function<void(const std::string& title, const std::string& what)> openProgress;
        std::function<void(int percent, const std::string& note)> progress;
        std::function<void()> closeProgress;
        // A yes/no question. Exactly one of the two runs.
        std::function<void(const std::string& title, const std::string& body,
                           std::function<void()> yes, std::function<void()> no)> ask;
        // A message with OK. `then` runs when it is dismissed — the upgrade hands the ECU back from there,
        // so the studio's connect (and whatever it asks) waits for the person to have read this.
        std::function<void(const std::string& title, const std::string& body, std::function<void()> then)> tell;
        // What the update takes away and brings (FirmwareChangesDialog), before anything is erased.
        // `proceed` gets the answer: true = update, false = cancel.
        struct Changes {
            std::string fromVersion, toVersion;
            tunediff::FirmwareChanges changes;
            const MetaModel* oldMeta = nullptr;       // owned by the upgrade, which outlives the dialog
            const MetaModel* newMeta = nullptr;
            std::vector<uint8_t> tune, migrated;      // the tune in each firmware's layout
            std::shared_ptr<PanelLibrary> oldPages, newPages;
        };
        std::function<void(Changes, std::function<void(bool proceed)>)> showChanges;
        // The upgrade is over — finished, refused or failed. `identity` is what the ECU last said it
        // is; the studio carries on its normal connect with it. Empty = the link is not up.
        std::function<void(const std::string& identity)> handBack;
        // A question with two named answers and a "Remember my choice" box: answer(yes, remember).
        std::function<void(const std::string& title, const std::string& body, const std::string& yesLabel,
                           const std::string& noLabel, std::function<void(bool yes, bool remember)> answer)> askRemember;
    };

    FirmwareUpgrade(EcuLink& link, Ui ui);

    bool active() const { return step_ != Step::Idle; }
    // The ECU is being CHANGED: from the flash on, until the tune is back and the ECU restarted. Only
    // this makes quitting unsafe. Reading the tune, the offer and the safety check change nothing on
    // the ECU — quitting there simply means "not now".
    bool writing() const { return step_ >= Step::Flashing; }

    // Begin, straight after the ECU identified itself on `port`.
    void start(const fwkits::Kit& kit, const std::string& identity, const std::string& port);

    // RECOVERY: an ECU is already waiting in its bootloader — an update that did not finish (the studio
    // closed, the cable moved). Flash `kit` and let it start; there is no tune to carry, because the
    // tune banks were never erased and the ECU boots with whatever it had.
    void recover(const fwkits::Kit& kit);

    // The link's signals, routed here while active(). Each returns true when it consumed the event.
    bool onIdentity(const std::string& sig);
    bool onConfigImage(const std::vector<uint8_t>& image);
    bool onConfigProgress(int done, int total);
    void onTelemetry(const std::vector<uint8_t>& frame);
    void onWriteFailed();
    bool onSdStatus(uint8_t state);
    bool onFileWritten(const std::string& name);
    bool onFileError(const std::string& name, const std::string& message);
    bool onFileProgress(const std::string& name, int64_t bytes, int64_t total);

private:
    enum class Step { Idle, Pulling, Offered, Safety, Flashing, Rebooting, Pushing, Verifying, Resetting, SdCopy };

    void safetyCheck();
    void checkPermission();
    void ensureUsbAccess(std::function<void()> then);   // udev rule (Linux) / WinUSB driver (Windows)
    void pull();
    void pulled(const std::vector<uint8_t>& image);
    void recoveredRead(const std::vector<uint8_t>& image);   // recovery: the tune the new firmware booted with
    std::string title() const;        // "Installing" for a recovery, "Updating" for an update
    std::string backupNote() const;   // " It is backed up in …" — or nothing, when there is no backup
    void declined();
    void backupThenFlash();
    void enterDfu();
    void runFlash();
    void flashed(bool ok, const std::string& error);
    void waitForEcu(Step next);
    void pushTune();
    void verify(const std::vector<uint8_t>& image);
    void resetEcu();
    void copyToSd();
    void sdNext();
    void finish();
    void fail(const std::string& why, bool ecuStillHasFirmware);
    void abandon(const std::string& why);   // stop before anything was changed; the connect carries on

    EcuLink& link_;
    Ui ui_;
    Step step_ = Step::Idle;
    bool recovering_ = false;
    fwkits::Kit kit_;
    std::string identity_, port_, board_, fromVersion_, fromHash_, uid_;
    MetaModel oldMeta_, newMeta_;
    std::vector<uint8_t> tuneDict_;       // the tune pulled off the ECU, by name (jayecu-tune/2)
    bool noTune_ = false;                 // the ECU had no tune: the new firmware's default tune goes on
    std::vector<uint8_t> pushed_;         // the image written to the new firmware
    MigrationReport report_;
    std::string backupPath_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> sdFiles_;   // still to write to the card
    std::string sdNote_;
    bool sdAnswered_ = false, sdYes_ = false;   // the SD question, answered this run without "remember"                                                  // what happened on the card
    std::vector<uint8_t> lastTelemetry_;
    int waitTicks_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

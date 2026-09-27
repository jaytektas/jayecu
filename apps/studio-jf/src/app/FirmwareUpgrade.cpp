#include "FirmwareUpgrade.h"

#include "UdevRules.h"   // the rule in tools/udev, carried so an AppImage can install it
#include "../comms/Crc32.h"
#include "../comms/Dfu.h"
#include "../model/Ecu.h"
#include "../model/StudioPaths.h"
#include "../surface/PanelLibrary.h"

#include <j/config/Settings.h>
#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/core/Timer.h>
#include <j/io/SerialPort.h>
#include <j/platform/JUdevRule.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string stamp() {
    std::time_t now = std::time(nullptr);
    char b[24]; std::strftime(b, sizeof b, "%Y-%m-%d %H.%M", std::localtime(&now));
    return b;
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeAll(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return static_cast<bool>(f);
}

// The ECU's serial port, if it is on the bus: the jayecu CDC (0483:5740). `prefer` wins when present.
std::string findEcuPort(const std::string& prefer) {
    std::string any;
    for (const auto& p : jf::JSerialPort::availablePorts()) {
        if (!p.hasVidPid || p.vendorId != 0x0483 || p.productId != 0x5740) continue;
        if (p.port == prefer) return p.port;
        if (any.empty()) any = p.port;
    }
    return any;
}

// The pages of a dashboard document (a .gui), or null when it has none.
std::shared_ptr<PanelLibrary> pagesOf(const std::string& guiPath) {
    if (guiPath.empty()) return nullptr;
    auto j = jf::JJson::tryParseFile(guiPath);
    if (!j || !j->isObject() || !(*j)["panelLibrary"].isObject()) return nullptr;
    auto lib = std::make_shared<PanelLibrary>();
    lib->load((*j)["panelLibrary"]);
    return lib->pages().empty() ? nullptr : lib;
}

}  // namespace

FirmwareUpgrade::FirmwareUpgrade(EcuLink& link, Ui ui) : link_(link), ui_(std::move(ui)) {}

void FirmwareUpgrade::recover(const fwkits::Kit& kit) {
    kit_ = kit; identity_.clear(); port_.clear(); board_ = kit.board;
    recovering_ = true;
    tuneDict_.clear(); noTune_ = false; pushed_.clear(); report_ = {}; backupPath_.clear(); sdNote_.clear();
    fromVersion_ = "none"; fromHash_.clear(); uid_.clear();
    JLOGC("firmware", jf::JLogLevel::Info) << "recovery: flashing " << kit_.board << " " << kit_.version
                                           << " onto an ECU found waiting in its bootloader";
    // RECOVERY IS AN UPDATE whose first half cannot happen: the board is in its bootloader, so there is no
    // old firmware to read a tune from, check the ignition with, or back up. From the flash on it is the
    // same sequence — check, then the tune (read off the new firmware: kept if it is valid for it, the
    // firmware's default tune if not), SD card, and the same summary.
    if (!newMeta_.loadFile(kit_.meta)) {
        abandon(newMeta_.needsStudio().empty()
                    ? "The firmware's meta could not be read (" + kit_.meta + "). Nothing was written."
                    : "Firmware " + kit_.version + " needs jayecu Studio " + newMeta_.needsStudio() +
                      ". Update the studio first. Nothing was written.");
        return;
    }
    // THE SAME ACCESS AN UPDATE NEEDS. A bootloader the studio may not open is found, offered, and
    // then fails at the first transfer — so the rule (Linux) or the driver (Windows) comes first here too.
    ensureUsbAccess([this] {
        step_ = Step::Flashing;
        ui_.openProgress(title(), "Writing firmware " + kit_.version + " \xE2\x80\x94 do not unplug the ECU");
        ui_.progress(0, "");
        runFlash();
    });
}

void FirmwareUpgrade::start(const fwkits::Kit& kit, const std::string& identity, const std::string& port) {
    kit_ = kit; identity_ = identity; port_ = port;
    recovering_ = false;
    tuneDict_.clear(); noTune_ = false; pushed_.clear(); report_ = {}; backupPath_.clear(); lastTelemetry_.clear();
    // identity = "jayecu <board> <version> <build> <layout_hash> <device_uid> t<telemetry_size>"
    const auto w = words(identity);
    board_       = w.size() > 1 ? w[1] : "";
    fromVersion_ = w.size() > 2 ? w[2] : "";
    fromHash_    = w.size() > 4 ? w[4] : "";
    uid_         = w.size() > 5 ? w[5] : "";
    JLOGC("firmware", jf::JLogLevel::Info) << "upgrade " << board_ << " " << fromVersion_ << " (" << fromHash_
        << ") -> " << kit_.version << " (" << kit_.layoutHash << ") from " << kit_.dir;

    // THE ECU'S CURRENT META. Without it the tune cannot be read by name, and a tune that cannot be read
    // cannot be carried across — so no meta, no upgrade.
    std::string oldPath;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(StudioPaths::dataDir("meta"), ec))
        if (e.path().extension() == ".meta" && !fromHash_.empty() &&
            e.path().stem().string().find(fromHash_) != std::string::npos) { oldPath = e.path().string(); break; }
    step_ = Step::Safety;
    if (oldPath.empty() || !oldMeta_.loadFile(oldPath)) {
        abandon("The studio does not have the description (meta) of the firmware this ECU is running now "
                "(layout " + fromHash_ + "), so it cannot read the tune to carry it across.\n\n"
                "Connect once without updating \xE2\x80\x94 the studio fetches it from the ECU's SD card \xE2\x80\x94 "
                "then update.");
        return;
    }
    if (!newMeta_.loadFile(kit_.meta)) {
        abandon(newMeta_.needsStudio().empty()
                    ? "The new firmware's meta could not be read (" + kit_.meta + ")."
                    : "Firmware " + kit_.version + " needs jayecu Studio " + newMeta_.needsStudio() +
                      (newMeta_.needsStudio().rfind("newer than", 0) == 0 ? "" : " or newer") +
                      ". Update the studio first. Nothing was changed.");
        return;
    }
    // Read the tune first — a read changes nothing — so the offer can SHOW what the update changes.
    pull();
}

// ---- 1. ignition off, engine stopped ---------------------------------------------------------------

void FirmwareUpgrade::onTelemetry(const std::vector<uint8_t>& frame) {
    if (step_ == Step::Safety) lastTelemetry_ = frame;   // only frames arriving AFTER the decision count
}

void FirmwareUpgrade::safetyCheck() {
    if (step_ != Step::Safety) return;
    if (lastTelemetry_.empty()) {                      // telemetry polls from the moment the link opens
        if (++waitTicks_ > 30) { abandon("The ECU is not sending live data, so the studio cannot check that "
                                         "the ignition is off. Nothing was changed."); return; }
        std::weak_ptr<std::atomic<bool>> alive = alive_;
        jf::JTimer::singleShot(std::chrono::milliseconds(100), [this, alive] { if (alive.lock()) safetyCheck(); });
        return;
    }
    auto value = [&](const char* name, bool& found) -> double {
        const auto& t = oldMeta_.telemetry();
        const auto it = t.find(name);
        found = it != t.end() && it->second.offset + it->second.size <= int(lastTelemetry_.size());
        if (!found) return 0.0;
        return MetaModel::decodeRaw(it->second.datatype, lastTelemetry_.data() + it->second.offset) * it->second.scale;
    };
    bool haveKey = false, haveRpm = false;
    const double key = value("key_on", haveKey), rpm = value("rpm", haveRpm);
    if (!haveKey || !haveRpm) { abandon("The ECU's live data has no ignition or RPM reading, so it is not safe to "
                                        "update from here. Nothing was changed."); return; }
    if (key != 0.0 || rpm != 0.0) {
        abandon(std::string(rpm != 0.0 ? "The engine is running." : "The ignition is on.") +
                "\n\nFirmware is only updated with the ignition OFF and the engine stopped, the ECU powered "
                "from USB alone. Turn the ignition off and connect again.\n\nNothing was changed.");
        return;
    }
    checkPermission();
}


// ---- 2. USB permission (Linux) ---------------------------------------------------------------------

// The ECU's serial port (5740) AND its DFU bootloader (df11) must both be covered. THE RULE MUST BE IN
// PLACE BEFORE THE ECU IS PUT INTO DFU: without it the bootloader appears, cannot be opened, and the only
// ways out are a successful write or a power cycle. Replaces the old 99- copy, which sat after
// 73-seat-late and so granted nothing.
static const jf::JUdevRule& ecuRule() {
    static const jf::JUdevRule rule("70-jayecu.rules", kJayecuUdevRules, { "df11", "5740" }, { "99-jayecu.rules" });
    return rule;
}

void FirmwareUpgrade::checkPermission() {
    ensureUsbAccess([this] { backupThenFlash(); });
}

// Linux: the udev rule. Windows: WinUSB bound to the bootloader. Either way asked once, through the
// system's own administrator prompt, and BEFORE the ECU is put into its bootloader.
void FirmwareUpgrade::ensureUsbAccess(std::function<void()> then) {
    std::weak_ptr<std::atomic<bool>> alive = alive_;
#if defined(_WIN32)
    if (dfu::driverInstalled()) { then(); return; }
    ui_.ask("USB driver needed",
        "To write firmware the studio needs a USB driver (WinUSB) for the ECU's bootloader. It is "
        "installed once, and Windows will ask for administrator permission.\n\nInstall it now?",
        [this, alive, then] {
            ui_.openProgress("Installing the USB driver", "Waiting for the administrator prompt");
            std::thread([this, alive, then] {
                std::string error;
                const bool ok = dfu::installDriver(error);
                jf::JMainThreadDispatcher::instance().post([this, alive, then, ok, error] {
                    if (!alive.lock()) return;
                    ui_.closeProgress();
                    if (ok) then();
                    else abandon("The USB driver was not installed (" + error + "). Nothing was changed.");
                });
            }).detach();
        },
        [this] { abandon("Cancelled. Nothing was changed."); });
#else
    if (ecuRule().installed()) { then(); return; }
    ui_.ask("USB permission needed",
        "To write firmware the studio needs permission to reach the ECU's bootloader over USB. "
        "This is set up once, and the system will ask for your password.\n\nSet it up now?",
        [this, alive, then] {
            ecuRule().install([this, alive, then](bool ok, const std::string& error) {
                if (!alive.lock()) return;
                if (ok) then();
                else abandon("USB permission was not set up (" + error + "). Nothing was changed.");
            });
        },
        [this] { abandon("Cancelled. Nothing was changed."); });
#endif
}

// ---- 3. pull the tune, back it up, try the migration -----------------------------------------------

void FirmwareUpgrade::pull() {
    step_ = Step::Pulling;
    ui_.openProgress("Firmware " + kit_.version + " is available", "Reading the tune to see what the update changes");
    link_.setConfigSize(oldMeta_.configSize());
    link_.setBlockSize(oldMeta_.blockSize());
    link_.readConfigImage();
}

bool FirmwareUpgrade::onConfigProgress(int done, int total) {
    if (step_ != Step::Pulling && step_ != Step::Verifying) return false;
    ui_.progress(total > 0 ? int(100LL * done / total) : 0,
                 std::to_string(done / 1024) + " / " + std::to_string(total / 1024) + " KB");
    return true;
}

bool FirmwareUpgrade::onConfigImage(const std::vector<uint8_t>& image) {
    if (step_ == Step::Pulling && recovering_) { recoveredRead(image); return true; }
    if (step_ == Step::Pulling)   { pulled(image); return true; }
    if (step_ == Step::Verifying) { verify(image); return true; }
    return false;
}

void FirmwareUpgrade::pulled(const std::vector<uint8_t>& image) {
    if (int(image.size()) != oldMeta_.configSize()) { abandon("The tune could not be read from the ECU. Nothing was changed."); return; }
    // AN ECU WITH NO TUNE HAS NOTHING TO CARRY OVER. Its config is all zero (it rejected its tune at boot),
    // and migrating that would back up zeros and put zeros on the new firmware as if they were a tune. A
    // new board is in exactly this state and has no tune anywhere to put on, so the new firmware's own
    // default tune goes on instead — the tune a fresh ECU starts from — and there is nothing to back up.
    const uint32_t stored = image.size() >= 4 ? (uint32_t(image[0]) | uint32_t(image[1]) << 8 |
                                                 uint32_t(image[2]) << 16 | uint32_t(image[3]) << 24) : 0u;
    noTune_ = stored != uint32_t(std::strtoul(oldMeta_.layoutHash().c_str(), nullptr, 16));
    if (noTune_) JLOGC("firmware", jf::JLogLevel::Info) << "the ECU has no tune: the new firmware's default tune goes on";
    const std::vector<uint8_t>& from = noTune_ ? oldMeta_.defaultImage() : image;
    tuneDict_ = noTune_ ? TuneFile::serialise(newMeta_.defaultImage(), newMeta_) : TuneFile::serialise(image, oldMeta_);
    Ecu* e = Ecu::openOrCreate(uid_, board_);

    // THE DRY RUN, and from it THE OFFER: what this tune has set that the new firmware drops, and what
    // the new firmware brings, each on its own pages. That report IS the question — deciding to update
    // before seeing it would be deciding blind.
    const std::vector<uint8_t> trial = TuneFile::deserialise(tuneDict_, newMeta_, report_);
    if (trial.empty()) { abandon("The tune could not be read. Nothing was changed."); return; }
    Ui::Changes c;
    c.fromVersion = fromVersion_;
    c.toVersion   = kit_.version;
    c.oldPages    = pagesOf(e ? e->dashboardPath() : std::string());   // the tuner's own pages
    c.newPages    = pagesOf(kit_.dashboard);                           // the new firmware's pages
    if (!c.newPages) c.newPages = c.oldPages;                          // no shipped dashboard: theirs, then
    c.changes     = tunediff::compareFirmware(oldMeta_, from, c.oldPages.get(),
                                              newMeta_, trial, c.newPages.get(), report_.unresolved);
    JLOGC("firmware", jf::JLogLevel::Info) << "offer: " << c.changes.retired.settings << " set setting(s) retired, "
                                           << c.changes.added.settings << " new";
    c.oldMeta = &oldMeta_; c.newMeta = &newMeta_;
    c.tune = from; c.migrated = trial;
    ui_.closeProgress();
    step_ = Step::Offered;
    ui_.showChanges(std::move(c), [this](bool proceed) {
        if (!proceed) { declined(); return; }
        // Only now: is it safe? Checked on live data from AFTER the answer — the key may have moved
        // while the report was being read.
        step_ = Step::Safety;
        lastTelemetry_.clear();
        waitTicks_ = 0;
        ui_.status("Checking the ECU is safe to update \xE2\x80\xA6");
        safetyCheck();
    });
}

// RECOVERY'S TUNE. The firmware rejects at boot any stored tune that is not for its layout, and then runs
// with every setting zero — so the layout hash at the head of the image says whether it started on a real
// one. A real one stays: it survived whatever broke the firmware, and it is this ECU's. Otherwise the
// firmware's own default tune goes on, exactly as an update does for an ECU that had none.
void FirmwareUpgrade::recoveredRead(const std::vector<uint8_t>& image) {
    if (int(image.size()) != newMeta_.configSize()) { fail("The tune could not be read from the ECU.", true); return; }
    const uint32_t stored = uint32_t(image[0]) | uint32_t(image[1]) << 8 | uint32_t(image[2]) << 16 |
                            uint32_t(image[3]) << 24;
    noTune_ = stored != uint32_t(std::strtoul(newMeta_.layoutHash().c_str(), nullptr, 16));
    if (!noTune_) {
        JLOGC("firmware", jf::JLogLevel::Info) << "recovery: the ECU kept a tune that suits this firmware";
        pushed_ = image;
        copyToSd();
        return;
    }
    JLOGC("firmware", jf::JLogLevel::Info) << "recovery: the ECU has no tune: the firmware's default tune goes on";
    tuneDict_ = TuneFile::serialise(newMeta_.defaultImage(), newMeta_);
    pushTune();
}

void FirmwareUpgrade::declined() {
    // "Not now": nothing was written anywhere, not even a backup. The connect carries on.
    JLOGC("firmware", jf::JLogLevel::Info) << "firmware " << kit_.version << " offered, not taken";
    step_ = Step::Idle;
    ui_.handBack(identity_);
}

void FirmwareUpgrade::backupThenFlash() {
    // The backup, by name, beside the ECU's other tunes — readable by any studio, on any firmware.
    if (noTune_) { enterDfu(); return; }   // no tune on the ECU: nothing to back up
    Ecu* e = Ecu::openOrCreate(uid_, board_);
    const fs::path dir = fs::path(e ? e->dir() : StudioPaths::dataDir("firmware")) / "backups";
    std::error_code ec;
    fs::create_directories(dir, ec);
    backupPath_ = (dir / ("before firmware " + kit_.version + " " + stamp() + ".tune")).string();
    if (!writeAll(backupPath_, tuneDict_)) { abandon("The tune could not be saved to " + backupPath_ + ". Nothing was changed."); return; }
    JLOGC("firmware", jf::JLogLevel::Info) << "tune backed up to " << backupPath_;
    enterDfu();
}

// ---- 4. flash --------------------------------------------------------------------------------------

void FirmwareUpgrade::enterDfu() {
    step_ = Step::Flashing;
    ui_.openProgress(title(), "Restarting the ECU into its bootloader \xE2\x80\x94 do not unplug it");
    ui_.progress(0, "");
    link_.sendCli("dfu");
    // The command never answers — the ECU resets. Give it a moment to go out, then let the port go.
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    jf::JTimer::singleShot(std::chrono::milliseconds(400), [this, alive] {
        if (!alive.lock()) return;
        link_.close();
        runFlash();
    });
}

void FirmwareUpgrade::runFlash() {
    const std::vector<uint8_t> image = readAll(kit_.firmware);
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    std::thread([this, alive, image] {
        auto post = [alive](std::function<void()> f) {
            jf::JMainThreadDispatcher::instance().post([alive, f] { if (alive.lock()) f(); });
        };
        std::string error;
        // The bootloader takes a moment to appear after the reset.
        for (int i = 0; i < 100 && !dfu::devicePresent(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto dev = dfu::devicePresent() ? dfu::openDevice(error) : nullptr;
        if (!dev && error.empty()) error = "the ECU's bootloader did not appear";
        if (dev) {
            const std::string layout = dev->layoutString();
            const unsigned xfer = dev->transferSize();
            post([layout, xfer, n = image.size()] {
                JLOGC("firmware", jf::JLogLevel::Info) << "DFU open: " << layout << ", " << xfer
                                                       << "-byte blocks; writing " << n << " bytes";
            });
        }
        bool ok = false;
        if (dev) {
            std::atomic<bool> cancel{ false };
            std::string lastStage;
            ok = dfu::flash(*dev, image, dfu::kFlashBase,
                [post, this, &lastStage](const dfu::Progress& p) {
                    const std::string note = p.stage;
                    const int pct = p.percent;
                    const bool newStage = note != lastStage;
                    lastStage = note;
                    post([this, pct, note, newStage] {
                        if (newStage) JLOGC("firmware", jf::JLogLevel::Info) << "DFU: " << note;
                        ui_.progress(pct, note + " \xE2\x80\x94 do not unplug the ECU");
                    });
                }, cancel, error);
        }
        post([this, ok, error] { flashed(ok, error); });
    }).detach();
}

void FirmwareUpgrade::flashed(bool ok, const std::string& error) {
    if (!ok) {
        JLOGC("firmware", jf::JLogLevel::Error) << "flash failed: " << error;
        ui_.closeProgress();
        const bool inDfu = dfu::devicePresent();
        if (inDfu) {
            ui_.ask("Firmware update failed",
                "Writing the firmware failed: " + error + ".\n\nThe ECU is waiting in its bootloader. Keep it "
                "plugged in.\n\nTry again?",
                [this] {
                    ui_.openProgress(title(), "Trying again \xE2\x80\x94 do not unplug the ECU");
                    runFlash();
                },
                // WHERE THE WAY BACK ACTUALLY IS. This named a Tools menu item that does not exist and a
                // developer's make target; the path a user has is Connect, which finds the bootloader and
                // offers the firmware again (s_offerRecovery).
                [this, error] { fail("The ECU is still in its bootloader with no working firmware. Keep it "
                                     "plugged in and press Connect: the studio finds it waiting and offers "
                                     "to install the firmware again.", false); });
        } else {
            fail("The ECU's bootloader could not be used: " + error + ".", true);
        }
        return;
    }
    JLOGC("firmware", jf::JLogLevel::Info) << "flashed " << kit_.firmware;
    ui_.progress(100, "Firmware written \xE2\x80\x94 waiting for the ECU to start");
    waitForEcu(Step::Rebooting);
}

// ---- 5. back on the new firmware? ------------------------------------------------------------------

void FirmwareUpgrade::waitForEcu(Step next) {
    step_ = next;
    waitTicks_ = 0;
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [this, alive, tick, next] {
        if (!alive.lock()) return;
        const std::string port = findEcuPort(port_);
        if (!port.empty()) {
            // A just-appeared port can take a moment to be given to the user (the udev ACL).
            std::weak_ptr<std::atomic<bool>> a2 = alive_;
            jf::JTimer::singleShot(std::chrono::milliseconds(800), [this, a2, port, tick, next] {
                if (!a2.lock()) return;
                port_ = port;
                link_.open(port);        // the identity handshake comes back through onIdentity
                // AN OPEN PORT IS NOT A RETURNED ECU. After `reset` the firmware first lets the burn land
                // (up to ~2 s), so the port found here can still be the OLD connection: it opens, the ECU
                // then resets under it, the link drops, and nothing ever answered. This loop used to stop
                // at the open, so that was a progress window waiting forever. Now the ECU has to IDENTIFY
                // itself (onIdentity moves step_ on) within 3 s, or the port is let go and the search
                // carries on — inside the same overall limit.
                jf::JTimer::singleShot(std::chrono::milliseconds(3000), [this, a2, tick, next] {
                    if (!a2.lock() || step_ != next) return;   // it answered: the sequence has moved on
                    JLOGC("firmware", jf::JLogLevel::Info) << "no identity on " << port_ << " yet; looking again";
                    link_.close();
                    waitTicks_ += 15;                          // the 3.8 s just spent counts against the limit
                    (*tick)();
                });
            });
            return;
        }
        if (++waitTicks_ > 80) {        // 20 s
            fail(step_ == Step::Rebooting ? "The ECU did not come back after the firmware was written."
                                          : "The ECU did not come back after it was reset.", true);
            return;
        }
        jf::JTimer::singleShot(std::chrono::milliseconds(250), *tick);
    };
    jf::JTimer::singleShot(std::chrono::milliseconds(1000), *tick);
}

bool FirmwareUpgrade::onIdentity(const std::string& sig) {
    if (step_ != Step::Rebooting && step_ != Step::Resetting) return false;
    identity_ = sig;
    const auto w = words(sig);
    const std::string ver = w.size() > 2 ? w[2] : "", build = w.size() > 3 ? w[3] : "",
                      hash = w.size() > 4 ? w[4] : "";
    // The build and layout name the image that was written; the version is only its label (a test kit
    // can be labelled newer than the image inside it).
    if (build != kit_.build || hash != kit_.layoutHash) {
        std::string why = "The ECU came back running " + ver + " " + build + " (layout " + hash + "), not " +
                          kit_.version + " " + kit_.build + " (layout " + kit_.layoutHash + ").";
        if (step_ == Step::Rebooting && !backupPath_.empty())
            why += " Your tune was not put back on it." + backupNote();
        fail(why, true);
        return true;
    }
    if (step_ == Step::Rebooting && recovering_) {
        // There was no old firmware to read the tune from, so read the one this firmware booted with.
        JLOGC("firmware", jf::JLogLevel::Info) << "recovery: firmware is running: " << sig;
        uid_ = w.size() > 5 ? w[5] : "";
        step_ = Step::Pulling;
        ui_.openProgress(title(), "Reading the tune the ECU started with");
        link_.setConfigSize(newMeta_.configSize());
        link_.setBlockSize(newMeta_.blockSize());
        link_.readConfigImage();
        return true;
    }
    if (step_ == Step::Rebooting) pushTune(); else copyToSd();
    return true;
}

// ---- 6. the tune onto the new firmware --------------------------------------------------------------

void FirmwareUpgrade::pushTune() {
    step_ = Step::Pushing;
    ui_.openProgress(title(), noTune_ ? "Putting the default tune on the new firmware"
                                      : "Putting your tune on the new firmware");
    ui_.progress(0, "");
    report_ = {};
    pushed_ = TuneFile::deserialise(tuneDict_, newMeta_, report_);
    if (pushed_.empty()) { fail("The tune could not be converted for the new firmware.", true); return; }
    link_.setConfigSize(newMeta_.configSize());
    link_.setBlockSize(newMeta_.blockSize());
    link_.writeConfigImage(pushed_);
    // Read it all back: the writes are acked one by one, but only a read proves the whole tune is there.
    // NOT until every write has been answered — the read claims the port for itself as it starts.
    waitTicks_ = 0;
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    auto wait = std::make_shared<std::function<void()>>();
    *wait = [this, alive, wait] {
        if (!alive.lock() || step_ != Step::Pushing) return;
        if (link_.busy()) {
            if (++waitTicks_ > 600) { fail("Writing the tune to the ECU did not finish." + backupNote(), true); return; }
            jf::JTimer::singleShot(std::chrono::milliseconds(50), *wait);
            return;
        }
        step_ = Step::Verifying;
        link_.readConfigImage();
    };
    (*wait)();
}

void FirmwareUpgrade::onWriteFailed() {
    if (step_ == Step::Pushing || step_ == Step::Verifying)
        fail("Writing the tune to the ECU failed." + backupNote(), true);
}

void FirmwareUpgrade::verify(const std::vector<uint8_t>& image) {
    // Compared by NAME, not by byte: padding and firmware-owned bytes need not round-trip.
    if (TuneFile::serialise(image, newMeta_) != TuneFile::serialise(pushed_, newMeta_)) {
        fail("The tune read back from the ECU is not the tune that was written." + backupNote(), true);
        return;
    }
    ui_.progress(100, "Saving the tune on the ECU");
    link_.burn();
    // The burn's acknowledgement first; `reset` itself then waits for the save to reach flash.
    waitTicks_ = 0;
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    auto wait = std::make_shared<std::function<void()>>();
    *wait = [this, alive, wait] {
        if (!alive.lock() || step_ != Step::Verifying) return;
        if (link_.busy() && ++waitTicks_ <= 200) { jf::JTimer::singleShot(std::chrono::milliseconds(50), *wait); return; }
        resetEcu();
    };
    (*wait)();
}

void FirmwareUpgrade::resetEcu() {
    // `reset` waits for the burn to reach flash before it restarts (CliCommands.cpp), so it can follow
    // the burn straight away. The ECU starts again on its new firmware with the tune in place.
    step_ = Step::Resetting;
    link_.sendCli("reset");
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    jf::JTimer::singleShot(std::chrono::milliseconds(400), [this, alive] {
        if (!alive.lock()) return;
        link_.close();
        waitForEcu(Step::Resetting);
    });
}

// ---- 7. meta + dashboard onto the ECU's SD card -----------------------------------------------------

// ASKED, UNLESS REMEMBERED. The copy is what lets ANOTHER studio, with no internet, read this ECU from its own
// card — and it costs about a minute over USB serial, wasted on an ECU only ever connected to this computer.
// Only the person knows which this is, so the studio asks; "Remember my choice" keeps the answer, and
// Preferences ▸ Updates shows and changes it.
static constexpr const char* kSdSetting = "updates.firmwareCopyToSd";

void FirmwareUpgrade::copyToSd() {
    step_ = Step::SdCopy;
    sdFiles_.clear();
    sdNote_.clear();
    auto& settings = jf::JSettings::instance();
    if (!settings.has(kSdSetting) && !sdAnswered_) {
        ui_.closeProgress();
        std::weak_ptr<std::atomic<bool>> alive = alive_;
        ui_.askRemember("Copy to the ECU's SD card?",
            "Copy the firmware's meta and dashboard to the ECU's SD card? Another studio can then read this "
            "ECU from its own card, with no internet. It takes about a minute.",
            "Copy to SD card", "Skip",
            [this, alive](bool yes, bool remember) {
                if (!alive.lock()) return;
                if (remember) jf::JSettings::instance().set(kSdSetting, yes);
                sdAnswered_ = true; sdYes_ = yes;
                copyToSd();
            });
        return;
    }
    const bool copy = settings.has(kSdSetting) ? settings.get<bool>(kSdSetting, true) : sdYes_;
    sdAnswered_ = false;                                // this run's answer is used once
    if (!copy) {                                        // No: the card is not touched at all
        JLOGC("firmware", jf::JLogLevel::Info) << "SD copy skipped";
        finish();
        return;
    }
    const std::string stem = board_ + " " + kit_.layoutHash;
    // The meta as the kit carries it — JSON plus its CRC32 footer, the form a studio fetching it checks.
    sdFiles_.push_back({ stem + ".meta", readAll(kit_.meta) });
    // The dashboard gets the same footer, so a studio can prove the transfer (tools/push_meta.py).
    if (!kit_.dashboard.empty()) {
        std::vector<uint8_t> gui = readAll(kit_.dashboard);
        const uint32_t crc = crc32_ieee::compute(gui);
        for (int i = 0; i < 4; ++i) gui.push_back(uint8_t(crc >> (8 * i)));
        sdFiles_.push_back({ stem + ".gui", std::move(gui) });
    }
    ui_.openProgress(title(), "Copying the meta and dashboard to the ECU's SD card");
    ui_.progress(0, "");
    link_.sdMcu();                       // take the card; the reply says whether there is one
}

bool FirmwareUpgrade::onSdStatus(uint8_t state) {
    if (step_ != Step::SdCopy) return false;
    if (state == 0x00) { sdNext(); return true; }
    sdNote_ = state == 0x01 ? "There is no SD card in the ECU, so the meta and dashboard were not copied to it."
                            : "The ECU's SD card was busy, so the meta and dashboard were not copied to it.";
    link_.sdRelease();
    finish();
    return true;
}

void FirmwareUpgrade::sdNext() {
    if (sdFiles_.empty()) {
        link_.sdRelease();              // this is what flushes and closes the last file on the card
        sdNote_ = "The meta and dashboard are on the ECU's SD card, for any other studio.";
        // Let the release be answered before handing back: the normal connect's tune read claims the
        // port for itself as it starts.
        waitTicks_ = 0;
        std::weak_ptr<std::atomic<bool>> alive = alive_;
        auto wait = std::make_shared<std::function<void()>>();
        *wait = [this, alive, wait] {
            if (!alive.lock() || step_ != Step::SdCopy) return;
            if (link_.busy() && ++waitTicks_ <= 100) { jf::JTimer::singleShot(std::chrono::milliseconds(50), *wait); return; }
            finish();
        };
        (*wait)();
        return;
    }
    JLOGC("firmware", jf::JLogLevel::Info) << "writing " << sdFiles_.front().first << " to the SD card ("
                                           << sdFiles_.front().second.size() << " bytes)";
    link_.writeFile(sdFiles_.front().first, sdFiles_.front().second);
}

bool FirmwareUpgrade::onFileProgress(const std::string& name, int64_t bytes, int64_t total) {
    if (step_ != Step::SdCopy) return false;
    ui_.progress(total > 0 ? int(100 * bytes / total) : 0,
                 name + ": " + std::to_string(bytes / 1024) + " / " + std::to_string(total / 1024) + " KB");
    return true;
}

bool FirmwareUpgrade::onFileWritten(const std::string& name) {
    if (step_ != Step::SdCopy || sdFiles_.empty() || name != sdFiles_.front().first) return false;
    sdFiles_.erase(sdFiles_.begin());
    sdNext();
    return true;
}

bool FirmwareUpgrade::onFileError(const std::string& name, const std::string& message) {
    if (step_ != Step::SdCopy || sdFiles_.empty() || name != sdFiles_.front().first) return false;
    JLOGC("firmware", jf::JLogLevel::Warn) << "SD copy of " << name << " failed: " << message;
    link_.sdRelease();
    sdNote_ = "Copying " + name + " to the ECU's SD card failed (" + message + ").";
    finish();
    return true;
}

// ---- 8. done ---------------------------------------------------------------------------------------

void FirmwareUpgrade::finish() {
    ui_.closeProgress();
    // The migrated tune becomes the ECU's active tune, so the normal connect finds studio and ECU in sync.
    if (Ecu* e = Ecu::openOrCreate(uid_, board_)) {
        const std::string name = e->lastActiveTune().empty() ? "current" : e->lastActiveTune();
        e->saveTune(name, TuneFile::serialise(pushed_, newMeta_));
    }
    // A RECOVERY THAT KEPT THE ECU'S OWN TUNE has nothing to report: no tune carried over, no backup, nothing
    // for the person to act on — so no dialog, which would only be a click in front of the connect. The
    // status line says what happened. (A recovery that put the DEFAULT tune on does stop, below: that one
    // carries a warning — set it up for your engine before starting it.)
    if (recovering_ && !noTune_) {
        recovering_ = false;
        step_ = Step::Idle;
        JLOGC("firmware", jf::JLogLevel::Info) << "recovery complete (tune kept). " << sdNote_;
        ui_.status("ECU firmware " + kit_.version + " installed, with the tune it already had. " + sdNote_);
        ui_.handBack(identity_);
        return;
    }
    // The report, in full, beside the backup.
    std::ostringstream r;
    r << "Firmware " << fromVersion_ << " (" << fromHash_ << ") -> " << kit_.version << " (" << kit_.layoutHash << ")\n"
      << "Backup: " << backupPath_ << "\n\n"
      << "Settings carried over: " << report_.migrated << "\n"
      << "New settings (firmware defaults): " << report_.defaulted << "\n";
    if (!report_.unmapped.empty())   { r << "\nNo longer exist:\n";      for (auto& s : report_.unmapped)   r << "  " << s << "\n"; }
    if (!report_.unresolved.empty()) { r << "\nCould not be resolved:\n"; for (auto& s : report_.unresolved) r << "  " << s << "\n"; }
    const std::string reportPath = noTune_ ? std::string()
                                           : fs::path(backupPath_).replace_extension(".report.txt").string();
    if (!reportPath.empty()) { std::ofstream f(reportPath); f << r.str(); }
    JLOGC("firmware", jf::JLogLevel::Info) << "upgrade complete\n" << r.str();
    if (noTune_) {
        step_ = Step::Idle;
        const bool recovered = recovering_;
        recovering_ = false;
        std::string body = "The ECU is now running firmware " + kit_.version + ". It had no tune, so it now has "
                           "the firmware's default tune \xE2\x80\x94 set it up for your engine before starting it.";
        if (!sdNote_.empty()) body += "\n\n" + sdNote_;
        ui_.tell(recovered ? "ECU firmware installed" : "ECU firmware updated", body,
                 [this, id = identity_] { ui_.handBack(id); });
        return;
    }

    std::string body = "The ECU is now running firmware " + kit_.version + ", with your tune.\n\n" +
        std::to_string(report_.migrated) + " settings carried over.\n" +
        std::to_string(report_.defaulted) + " new settings start at their defaults.";
    if (!report_.unmapped.empty())   body += "\n" + std::to_string(report_.unmapped.size()) + " no longer exist.";
    if (!report_.unresolved.empty()) body += "\n" + std::to_string(report_.unresolved.size()) + " could not be resolved.";
    if (!sdNote_.empty()) body += "\n\n" + sdNote_;
    body += "\n\nFull report: " + reportPath;
    step_ = Step::Idle;
    ui_.tell("ECU firmware updated", body, [this, id = identity_] { ui_.handBack(id); });
}

void FirmwareUpgrade::abandon(const std::string& why) {
    // Nothing was changed on the ECU: carry on with the normal connect on the link that is still up.
    JLOGC("firmware", jf::JLogLevel::Warn) << "upgrade not started: " << why;
    step_ = Step::Idle;
    ui_.closeProgress();
    ui_.tell("ECU firmware not updated", why, [this, id = identity_] { ui_.handBack(id); });
}

void FirmwareUpgrade::fail(const std::string& why, bool ecuStillHasFirmware) {
    JLOGC("firmware", jf::JLogLevel::Error) << "upgrade failed: " << why;
    step_ = Step::Idle;
    ui_.closeProgress();
    ui_.tell("ECU firmware update failed", why,
             [this, id = ecuStillHasFirmware && link_.isOpen() ? identity_ : std::string()] { ui_.handBack(id); });
}

std::string FirmwareUpgrade::title() const {
    return recovering_ ? "Installing ECU firmware" : "Updating ECU firmware";
}

std::string FirmwareUpgrade::backupNote() const {
    return backupPath_.empty() ? std::string() : " It is backed up in " + backupPath_ + ".";
}

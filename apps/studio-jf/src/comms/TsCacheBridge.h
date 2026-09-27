#pragma once

// TsCacheBridge — comms ADAPTER between the native Cache and a TS-protocol ECU. Once a converted ini's
// meta is loaded and its config image is in the Cache, the native widgets read/write through the Cache
// exactly as they do for a jayecu; this routes those signals to the TS link: Cache::writeRequested →
// 'C' page-0 write + a DEBOUNCED 'B' burn, and polls 'O' → Cache::ingestTelemetry for live gauges.
// The TS equivalent of EcuLink's Cache wiring — no widget or Cache change, all at arm's length.
// Page 0 only.

#include "TsLink.h"
#include "../model/Cache.h"
#include "../model/MetaModel.h"

#include <j/core/Signal.h>
#include <j/core/Timer.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

class TsCacheBridge {
public:
    explicit TsCacheBridge(TsLink& link) : link_(link) {
        telemTimer_.onTick.connect([this] { pollTelemetry(); });
    }

    // BURN IS DELIBERATE. Edits go to the ECU's RAM as they are made; committing them to flash is the
    // dialog's Burn button and nothing else. This used to happen on a 1.5 s timer after edits settled,
    // which quietly removed the whole point of the split: with every experiment committed a moment after
    // you made it, resetting the ECU could no longer take you back to the tune you trusted.
    bool burnNow() {
        if (!link_.isOpen()) return false;
        // A burn is OUR action, and on rusEFI it moves the published tune CRC (the commit recomputes it).
        // Without this stamp the watcher below reads that move as the ECU editing its own tune and spends a
        // full 67 KB re-read telling us what we just burned — and the re-read replaced the image, which
        // threw away the page's undo history a moment after the user had used it.
        ourWriteAt_ = std::chrono::steady_clock::now();
        const bool ok = link_.burnAll();
        if (ok) pending_ = false;
        return ok;
    }
    bool burnPending() const { return pending_ && link_.isOpen(); }

    void attach() {                              // hook Cache::writeRequested (idempotent)
        if (attached_) return;
        attached_ = true;
        // A native config edit (Cache::setConfigValue → debounced writeRequested) pushes to the ECU's
        // RAM at once; the burn commits to flash once edits settle. No-op while the TS link is closed,
        // so this connection coexists with EcuLink's (each guards on its own port).
        Cache::instance().writeRequested.connect([this](int offset, const std::vector<uint8_t>& bytes) {
            if (!link_.isOpen()) return;
            link_.writeFlat(offset, bytes);      // flat Cache offset → (page, offset) per the ini's geometry
            ourWriteAt_ = std::chrono::steady_clock::now();   // so the CRC move this causes isn't read as the ECU's
            pending_ = true;                     // RAM has it, flash does not — the Burn button lights up
        });
    }

    void startTelemetry(int intervalMs, int blockBytes) {
        telemBytes_ = blockBytes;
        misses_ = 0;
        pollTelemetry();
        telemTimer_.start(std::chrono::milliseconds(std::max(50, intervalMs)), jf::JTimer::JMode::Repeating);
    }
    void stopTelemetry() { telemTimer_.stop(); }

    // WATCH THE ECU'S OWN TUNE CRC. A rusEFI ECU edits its own configuration: "Grab Idle/Up" writes the
    // pedal's ADC reading into the calibration, an ETB autotune stores what it learned, a Lua script
    // sets a value. Nothing in the protocol announces it — the studio simply shows stale bytes, and a
    // burn would then write the stale ones back over the ECU's. What the firmware does publish is a CRC
    // over the whole tune, recomputed whenever the ECU changes it, so a change in it means "your copy is
    // out of date". Re-reading on it is the only way to see an ECU-side edit at all.
    void watchTuneCrc(const std::string& channel, int chunkBytes) {
        crcChannel_ = channel;
        chunk_      = std::max(16, chunkBytes);
        haveCrc_    = false;
    }

    // Config re-read decided by the CRC. Split out so the rules are testable without a serial port.
    enum class JCrcAction { Ignore, Adopt, Reload };
    static JCrcAction crcAction(bool haveLast, double last, double now,
                                bool pendingWrites, long long msSinceOurWrite) {
        if (!haveLast)          return JCrcAction::Adopt;    // first frame: nothing to compare against
        if (now == last)        return JCrcAction::Ignore;
        // Our own edits move the CRC too. Re-reading then would be pointless at best, and at worst would
        // overwrite bytes the user has typed but we have not flushed yet, so a change while writes are
        // outstanding is left alone entirely — the next frame reconsiders it.
        if (pendingWrites)      return JCrcAction::Ignore;
        if (msSinceOurWrite < kOurWriteWindowMs) return JCrcAction::Adopt;   // we caused it: just move on
        return JCrcAction::Reload;                                           // the ECU changed its own tune
    }

    // The whole configuration was re-read because the ECU changed it. Carries the byte count.
    jf::JSignal<int> configReloadedFromEcu;

    // Wait until the ECU has actually COMMITTED the burn. The 'B' ack means "write scheduled", not
    // "written" — rusEFI raises a flag and a background task does the flash — and it publishes exactly
    // that as a channel, so there is no need to guess a delay: poll `needBurn` until it clears. Measured
    // on the bench, a reboot sent right after the ack (or even 500 ms after, the definition's stated
    // pageActivationDelay) left the ECU running the value flash still held.
    //
    // Only what can ABANDON the write needs this — a controller command ("Reboot ECU", "Reset to DFU") and
    // disconnecting. Ordinary reads, writes and telemetry are unaffected by the pending write.
    void awaitBurnComplete(int timeoutMs = 8000) {
        if (!link_.isOpen()) return;
        const std::string kFlag = "needBurn";
        if (!Cache::instance().meta() || !Cache::instance().meta()->telemetry().count(kFlag)) {
            link_.awaitBurnSettled();                        // no such channel — fall back to the ini's delay
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const std::vector<uint8_t> blk = link_.readOutputChannels(0, telemBytes_);
            if (static_cast<int>(blk.size()) != telemBytes_) break;   // link gone: nothing left to wait for
            Cache::instance().ingestTelemetry(blk);
            if (Cache::instance().value(kFlag) == 0.0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // The ECU stopped answering (unplugged, rebooted, port taken). Somebody has to SAY so: the values on
    // screen are frozen at whatever arrived last, and a gauge holding its final reading looks exactly like
    // a gauge reading a steady engine.
    jf::JSignal<> linkLost;

private:
    void pollTelemetry() {
        if (!link_.isOpen()) return;
        const std::vector<uint8_t> blk = link_.readOutputChannels(0, telemBytes_);
        if (static_cast<int>(blk.size()) == telemBytes_) {
            misses_ = 0;
            Cache::instance().ingestTelemetry(blk);   // native decode via the converted telemetry descriptors
            _checkTuneCrc();
        } else if (++misses_ >= 5) {
            telemTimer_.stop();                        // dead link — stop stalling the UI thread on timeouts
            linkLost.emit();
        }
    }

    void _checkTuneCrc() {
        if (crcChannel_.empty() || !Cache::instance().has(crcChannel_)) return;
        const double now = Cache::instance().value(crcChannel_);
        const long long since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - ourWriteAt_).count();
        switch (crcAction(haveCrc_, lastCrc_, now, Cache::instance().hasPendingWrites(), since)) {
            case JCrcAction::Ignore: return;
            case JCrcAction::Adopt:  lastCrc_ = now; haveCrc_ = true; return;
            case JCrcAction::Reload: break;
        }
        int flat = 0;
        for (const auto& pg : link_.pages()) flat = std::max(flat, pg.base + pg.size);
        if (flat == 0) flat = static_cast<int>(Cache::instance().configImage().size());
        const std::vector<uint8_t> img = link_.readImage(chunk_, flat);
        if (static_cast<int>(img.size()) != flat) return;    // a failed read changes nothing; try again next frame
        Cache::instance().setConfigImage(img);
        Cache::instance().setBaseline(img);                  // the ECU's copy IS the truth now
        lastCrc_ = now; haveCrc_ = true;
        configReloadedFromEcu.emit(flat);
    }

    static constexpr long long kOurWriteWindowMs = 4000;   // covers a write + its debounced burn

    TsLink&   link_;
    jf::JTimer telemTimer_;
    bool       pending_ = false;   // unburned changes in the ECU's RAM
    std::string crcChannel_;
    double      lastCrc_{0.0};
    bool        haveCrc_{false};
    int         chunk_{256};
    std::chrono::steady_clock::time_point ourWriteAt_{};
    int       telemBytes_ = 256;
    int       misses_ = 0;
    bool      attached_ = false;
};

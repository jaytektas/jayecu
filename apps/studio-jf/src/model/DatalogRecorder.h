#pragma once
//
// DatalogRecorder — what the ECU is STREAMING, written to disk as it arrives.
//
// DATALOGGING, NOT APPLICATION LOGGING. The studio has both and they share only a word: JLOGC
// categories and genesis.log are this program describing its own behaviour to whoever is debugging
// it, and they belong to the developer. A datalog is the ENGINE's behaviour, recorded for the person
// tuning it, and it is a product feature with a button. Nothing here writes the former and nothing
// there writes the latter — the names are kept apart (model.datalog as the diagnostic category about
// this recorder, datalog.* as its settings) so a search for one never turns up the other.
//
// A LOG IS ONE START TO ONE STOP. That interval is the session — not the connection, not a clock,
// not a size. Nothing rolls and nothing splits; a file ends because something ended it, and the
// boundaries are therefore the ones somebody meant. Two things can press those buttons and they act
// on the same timeline: the app (start on connect / stop on disconnect, when autologging is on) and
// the user (the toolbar button, at any moment). Stopping halfway through an auto-started recording
// ends THAT log; starting again begins another. So one connection can produce several files, which
// is the point rather than a wrinkle.
//
// MSL, not a format of our own: tab-separated text that MegaLogViewer reads natively (its FieldMaps
// ship with a rusEFI profile, so an ECU in this family is a case the tool already expects). Text
// appends without seeking, a session killed mid-write is still a valid file, and it opens in an
// editor when something looks wrong — none of which is true of a binary log, and none of which the
// card's own MLG gives up either, because that one is optimising a different problem.
//
//   "<firmware signature>"
//   "Capture Date: Sat Aug 30 19:42:11 BST 2026"
//   Time<TAB>rpm<TAB>map<TAB>…
//   s<TAB>RPM<TAB>kPa<TAB>…
//   0.000<TAB>0.0<TAB>101.3<TAB>…
//
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class MetaModel;

class DatalogRecorder {
public:
    static DatalogRecorder& instance();

    // Begin a log. Chooses the filename from the wall clock, writes the header from the meta's
    // telemetry descriptor, and starts appending on every frame. False if a log is already open or
    // the file cannot be created — the caller shows why rather than failing silently.
    bool start(std::string* whyNot = nullptr);

    // End it. Flushes, closes, applies the retention rule, and returns the path just written. Safe
    // to call when not recording (returns empty). Pruning runs HERE rather than on a timer, so a
    // file is never removed out from under a viewer while nothing is happening.
    std::string stop();

    [[nodiscard]] bool        recording() const { return file_ != nullptr; }
    [[nodiscard]] const std::string& path()  const { return path_; }
    [[nodiscard]] uint32_t    rows()         const { return rows_; }

    // Append one row from the Cache's current values. Wired to Cache::frameUpdated — one row per
    // frame, at whatever rate the link is delivering, because a recorder that resamples is a
    // recorder that invents data points.
    void onFrame();

    // WHICH CHANNELS a recording carries. Every channel is a COLUMN, so this is the setting that
    // decides whether an hour costs 100 MB or 50 — and on the ECU side the same choice costs 89 bytes
    // of MLG header per channel in every file. Standard is what the DEFINITION marked worth logging
    // (`datalog:` in the signal catalog, 236 channels today); All is everything the frame carries.
    // A TEMPLATE, or a set the user built. The templates ship in the meta (schema
    // `datalog_templates`), so the studio offers the sets the CONNECTED firmware declares rather than
    // a list compiled into the app — the same reason the output wizard works that way. A template is
    // a starting selection, never a restriction: picking one fills the set, and editing the set from
    // there is the point rather than a departure from it.
    struct Template {
        std::string id, name, blurb, detail;
        bool        all = false;          // every channel, lists ignored
        bool        useFlag = false;      // whatever the definition marks `datalog:`
        // THE RATE IS PART OF THE SET. A template is a complete suggestion, not a channel list —
        // "Trigger and Sync (1 kHz)" that leaves the logger at 10 Hz has named a thing it does not
        // do. 0 = the definition offered none, so keep whatever is configured.
        int         rateHz  = 0;
        std::vector<std::string> categories, signals;
    };
    static const std::vector<Template>& templates();     // in meta order; empty on an older firmware

    // A PROFILE IS THE WHOLE ONBOARD-LOGGING SETUP, not a list of channels. It was the latter, which
    // made "the misfire profile" a half-truth: the channels came back and the CONDITION that decided
    // when to record them did not, so a saved setup still had to be finished by hand every time.
    // Everything the Datalog module holds travels together — the gate, its timing, the rate, and the
    // mask — because those are the parts of one decision.
    //
    // The gate is stored as SOURCE, never bytecode. Source survives a definition changing under it
    // and compiles against whatever firmware is connected; a stored program would be pinned to the
    // signal ids of the day it was saved.
    struct Profile {
        std::string name;
        std::vector<std::string> channels;
        std::string logWhen, logUntil;       // expression source; empty = the built-in rule
        int enabled   = 1;                   // SD logging on at all
        int rateHz    = 10;
        int minOnMs   = 0, minOffMs = 0, maxOnMs = 0, rearmMs = 0;
        int onInvalid = 1;                   // 0 Off, 1 On, 2 Hold — a logger fails ON
    };

    // USER PROFILES — the setups a tuner makes, beside the templates the firmware ships. The
    // templates are the definition's opinion and are read-only; this is where "what I set up for
    // this engine's misfire" lives, and it is the studio's own store because it is the tuner's, not
    // the ECU's. Kept and edited WITHOUT the ECU: writing one to the car is a separate act.
    static std::vector<std::string> profileNames();                   // sorted
    static std::vector<std::string> profileChannels(const std::string& name);
    static Profile profile(const std::string& name);                  // empty name = not found
    static void    saveProfile(const Profile& p);                     // by its own name; replaces
    static void saveProfile(const std::string& name);                 // from the current selection
    // …or from a set the caller is holding but has not committed — a dialog composing a profile
    // must not have to make its working set the live one first.
    static void saveProfile(const std::string& name, const std::vector<std::string>& channels);
    static void deleteProfile(const std::string& name);
    static bool applyProfile(const std::string& name);
    static const Template*              templateById(const std::string& id);
    // The channels a template names, resolved against the connected meta and WITHOUT committing
    // anything — no settings written, no mask pushed. applyTemplate is this plus the commit.
    static std::vector<std::string> templateChannels(const std::string& id);
    // A template as a profile, so the editor shows one the same way it shows everything else. A
    // template names channels and nothing more, so the rest comes back at the tune's own values.
    static Profile templateProfile(const std::string& id);

    // WHAT A SETUP COSTS THE CARD. The rule is exactly linear and both terms multiply, which is why
    // neither a channel count nor a rate means anything on its own: a record is 4 prefix bytes, the
    // selected fields, and a checksum, and the card sees that many bytes `rate` times a second.
    //
    // The budget is MEASURED, not assumed — bench_datalog.py on the rig puts this hardware's SD
    // write path at ~94 KB/s sustained, which is 5.8% of the 13.5 MHz SPI wire and therefore the
    // CARD's per-sector programming time rather than anything the firmware can hurry. A different
    // card moves this number; nothing else here does.
    struct Cost {
        int    channels    = 0;
        int    recordBytes = 0;    // 4 + fields + 1, what one record occupies on the card
        double bytesPerSec = 0.0;  // what the card is ASKED for
        double mbPerHour   = 0.0;  // …and what an hour of it costs in capacity
        bool   pastCard    = false;   // beyond what the reference card was measured to take
        // THE SLOWEST THING IN THE SET, and what it is. A rate above this records the same reading
        // repeatedly — which is an accurate account of when the value changed, and is only useful
        // if the tuner knows they asked for it. It is a FLOOR: a channel a running module claims
        // faster (transient enrichment holds throttle at 1 kHz) resolves better than this says.
        int         slowestHz = 0;
        std::string slowestChannel;
    };
    // WHAT A CARD WAS MEASURED TO TAKE — information, not a limit. The rate is never capped by the
    // channel count: asking for more than the card can write is not an error, because the logger
    // drops the surplus and counts it rather than holding anything up. A slow card means a slower
    // log, which is the card's problem to fix and the tuner's to notice.
    //
    // ~340 KB/s on the reference rig (bench_datalog.py, 8 GB class 6 — an old card; class 10 is the
    // modern floor). Was 95 KB/s before the drain learned to keep the file sector-aligned.
    static constexpr double kCardMeasuredBytesPerSec = 340.0 * 1024.0;
    static Cost costOf(const std::vector<std::string>& channels, int rateHz);

    // CAN THE ECU LOG AT ALL — read off the telemetry frame (sd_present / sd_msc_active), which is
    // where the firmware reports it. Asked by both the dialog's Activate and activate() itself:
    // editing a profile needs none of this, but writing one to a car that has no card to write to
    // is a setting with nowhere to land.
    enum class Card { Unknown,   // no live telemetry — there is no ECU to ask
                      None,      // connected, no card fitted
                      OnUsb,     // the card is mounted on the PC (the ordinary key-off state)
                      Ready };   // fitted, and the ECU has it
    static Card ecuCard();
    static const char* cardReason(Card c);   // "" when Ready; why not, otherwise

    // WHAT THE ECU IS SET TO, read out of the config image the studio holds — not out of the
    // studio's own settings. It is the answer to "what is this car actually doing", which is the
    // only reason to look at it beside a profile, and it comes from the cache because that is where
    // the ECU's config lives once it has been read.
    static Profile currentSettings();
    // Write a profile to the ECU: the mask, the gate, its timing and the rate. This is the ONLY
    // thing here that touches the car, and it lands in RAM like every other edit — the burn is
    // still deliberate. False (with a reason) when there is no link or the firmware has no such
    // fields; a gate that does not compile is reported rather than silently dropped.
    static bool activate(const Profile& p, std::string* whyNot = nullptr);

    // The mask a given set of channels would produce, and the set a mask decodes back to. An
    // all-zero mask means "as shipped" — the definition's own `datalog:` opinion — which is what
    // the firmware's own resolve_selection() does with it.
    static std::vector<uint8_t>     maskBytesFor(const std::vector<std::string>& channels);
    static std::vector<std::string> channelsFromMask(const std::vector<uint8_t>& bits);

    // The template the selection came from, or "" once it has been edited by hand. Kept so the page
    // can say what a set IS rather than only how many channels it has.
    static std::string templateId();
    // Fill the selection from a template. Unknown id selects nothing and returns false.
    static bool applyTemplate(const std::string& id);

    // THE ECU'S OWN COPY. The card's channels are a tune field (Datalog.mask), and picking a set
    // should mean the same thing whether the recording lands on the laptop or the card — so a change
    // here writes there too, whenever a link is up.
    //
    // The bit index is the channel's position in the firmware's descriptor CATALOG, which is the
    // telemetry frame's own field order — so it is recovered by sorting the meta's channels by their
    // frame OFFSET. That equivalence is the whole mapping, and a test asserts it against the
    // generated table rather than trusting it (a mismatch would log the wrong channels, silently, in
    // a file that opens perfectly).
    static std::vector<std::string> catalogOrder();
    // The mask the ECU would be given for the current selection: one bit per catalog channel, LSB
    // first, sized to the tune's array.
    static std::vector<uint8_t> maskBytes();
    // Push it to the ECU, one byte per array element. No-op with no link or no such field.
    static void pushMaskToEcu();
    static void pushMaskToEcu(const std::vector<uint8_t>& bits);

    // THE ONBOARD LOGGER'S channels, by name -- what the ECU writes to its card, as the Onboard
    // Logging dialog last activated it. Empty means "the definition's own `datalog:` set". Setting it
    // pushes the ECU's mask.
    static std::vector<std::string> customSelection();
    static void setCustomSelection(const std::vector<std::string>& names);
    // The onboard channels resolved against the loaded definition (the mask is built from these).
    static std::vector<std::string> selectedChannels();

    // THE STUDIO'S OWN SET -- what a recording on this PC carries, chosen on Logging ▸ Recording
    // Channels. Deliberately NOT the onboard logger's: the card and the laptop are two recorders with
    // two jobs. The card records a few channels for a long time with little room; the laptop records
    // while somebody is watching, and a log that turns out to lack the one channel that mattered cannot
    // be re-recorded. So an unconfigured studio records EVERY channel, and choosing channels for the
    // card never quietly narrows what the laptop keeps. Stored per definition family (the meta's board),
    // because a jayecu channel list means nothing to a rusEFI ECU.
    static std::vector<std::string> recordingSelection();                       // as chosen; empty = every channel
    static void setRecordingSelection(const std::vector<std::string>& names);   // empty = every channel
    // What start() writes: the chosen set, in the order chosen, less anything this definition does not
    // have -- or, with nothing chosen (or nothing chosen that still exists), every channel.
    static std::vector<std::string> recordingChannels();

    // --- settings (persisted by the caller; see PreferencesDialog) ---------------------------
    // Start on connect and stop on disconnect. Off by default: a recorder that runs unasked fills a
    // directory with sessions nobody meant to keep.
    static bool autoLog();
    static void setAutoLog(bool on);
    // Keep at most this many files in the log directory; 0 = keep everything. Applied after a
    // recording ENDS, and the file just written is never a candidate.
    static int  keepFiles();
    static void setKeepFiles(int n);
    // Where logs go. Empty = the default under the studio's data directory.
    static std::string directory();
    static void        setDirectory(const std::string& dir);

    // The resolved directory, created if it does not exist.
    static std::string resolvedDirectory();

    // Files in the log directory, newest first. The browser and the pruner ask the same question.
    static std::vector<std::string> existingLogs();

private:
    DatalogRecorder() = default;
    void prune();

    std::FILE*  file_ = nullptr;
    std::string path_;
    // The channels this log carries, resolved ONCE at start: a log's columns cannot change
    // half-way through without lying about every row above the change.
    std::vector<std::string> channels_;
    uint32_t    rows_  = 0;
    double      t0_ms_ = 0.0;    // the clock the Time column counts from
};

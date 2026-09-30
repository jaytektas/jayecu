#pragma once

#include <cstdint>
#include <string>
#include <deque>
#include <unordered_map>
#include <vector>

#include <j/core/Signal.h>
#include <j/core/Timer.h>
#include <j/io/SerialPort.h>

#include "ChunkReader.h"   // a full config read runs on its own thread, at the link's pace

#include "../model/TriggerLog.h"   // Record — the link hands out parsed records, not raw bytes

// EcuLink — the comms client: speaks the jaytek/omnidyno wire protocol over a serial port.
//   frame = [AA 55][typeId][rsv][len:u16 LE][ts:u64 LE][seq:u16 LE][ payload ][crc16:2 LE], len = total.
// The firmware echoes a request's `seq` in its reply, so EcuLink drives the link as a CORRELATION-ID
// RESOLVE POOL: every request is stamped with a unique sequence id, sent immediately (pipelined — no
// waiting for the previous reply), and parked in pending_ keyed by that id. An inbound frame is matched
// to its request by the echoed seq, resolved out of the pool and dispatched by the request's kind; a
// frame whose seq matches nothing is unsolicited/late and ignored. A ~50 ms retry service resends (same
// seq) or fails+drops pool entries past their deadline, so writes are acked + retried, not fired-and-forgotten.
// One classified knock measurement, decoded from 0x28.
//
// The buckets alone are unreadable: what they mean depends on the floor they were measured against,
// the spark they were placed relative to, and the threshold they had to beat — all resolved on the
// ECU at the moment of judgement and all moved on by the time this arrives. So the firmware records
// the verdict WITH its evidence and this carries the whole thing.
struct KnockShot {
    enum Verdict : uint8_t { Clean = 0, Bootstrapped = 1, Knocking = 2, PreIgnition = 3 };
    bool     valid   = false;   // false = the ECU has no classifier (no tune) or nothing classified
    uint32_t seq     = 0;       // increments per classified measurement; 0 = nothing yet
    uint8_t  cyl     = 0;
    uint8_t  verdict = Clean;
    float    db = 0, floorDb = 0, over = 0, threshold = 0, sparkDeg = 0;
    float    preFrac = -1.0f;   // -1 = the measurement carried no phase information
    float    startDeg = 0, stepDeg = 0;   // stepDeg 0 = unstamped, so the angles are not meaningful
    std::vector<float> buckets;           // band level per bucket, dB
    [[nodiscard]] bool hasPhase() const { return stepDeg != 0.0f && !buckets.empty(); }
    [[nodiscard]] float angleOf(size_t i) const { return startDeg + stepDeg * (float(i) + 0.5f); }
};

class EcuLink {
public:
    EcuLink();
    ~EcuLink();

    // Command codes (defaults match the protocol; overridden from the loaded meta if desired).
    void setConfigSize(int n) { configSize_ = n; }
    // Config read/write chunk = the firmware block size (from the meta), so a chunk frame exactly fits the
    // firmware's parser buffer. Ignored if <= 0 (keeps the conservative default).
    void setBlockSize(int n) { if (n > 0) blockSize_ = n; }

    void open(const std::string &portName);
    void close(bool emit = true);   // emit=false on teardown: don't fan openedChanged out to (freed) UI
    bool isOpen() const;
    std::string portName() const;

    void readConfigImage();                              // chunked full-config read -> configImageReady
    // Targeted re-read of a byte range (the gen-watch scoped refresh). Split into block-sized requests,
    // each emitting configRangeReady(offset, bytes) — independent of any full-image read in flight.
    void readConfigRange(int offset, int size);
    void writeConfig(int offset, const std::vector<uint8_t> &data);// queued, block-chunked, acked (0x00) + retried
    void writeConfigImage(const std::vector<uint8_t> &image);      // push the whole local tune (== writeConfig(0,…)), RAM only
    void burn();                                         // persist g_config to flash — deliberate action only
    // Anything other than the telemetry poll still queued or awaiting its reply — writes included. A
    // full config read claims the port for itself the moment it starts, so a read-back that must see
    // every write waits for this to go false first.
    [[nodiscard]] bool busy() const { return bulkInFlight(); }
    void requestDebugLog();                              // drain the ECU text console ('D')
    void readDtcImage();                                 // read the full DTC table image ('G') -> dtcImageReady
    // KNOCK SCOPE (0x28): the classifier's latest shot — the phase-resolved profile WITH the floor,
    // spark and threshold it was judged against. One request, one reply, no arming and no paging: a
    // profile fits a frame and the classifier is always running. Poll it; `seq` says whether what came
    // back is new.
    // `lastEvent` true asks for the last non-clean verdict rather than the newest measurement. That
    // is almost always what a person wants: at 50 windows/sec the newest is a quiet window, and the
    // event that fired is long gone before any poll reaches it.
    void readKnockScope(bool lastEvent = true);
    void clearDtcs();                                    // wipe the DTC table (CLI 'dtc clear' via 'E')
    // Capture ONE engine cycle of delivered coil/injector/trigger edges (0x26). Arms the ECU, waits
    // for the capture to close on the next cycle boundary, then pages the edges out — emitting
    // engineCycleReady once, with the assembled image. Nothing streams: this is request/response, so
    // the ECU is only ever recording during the single cycle we asked for.
    void captureEngineCycle();
    // RAW TRIGGER LOG (0x27), as a STREAM. start() arms and begins draining; stop() ends it.
    //
    // Continuous rather than one-shot because a fixed buffer is not a unit of capture: 4096 records
    // is twenty minutes of a one-trigger-per-revolution engine at cranking speed and 3.4 seconds of
    // a 180-slot wheel armed Both. The user cranks for as long as they want to and stops when they
    // have seen enough, and the display follows a poll behind.
    //
    // ARMING DISABLES THE ECU'S DECODER, so this is refused on a running engine and stopping is not
    // optional — the ECU tears it down on link loss for the same reason.
    // The capture is a SNAPSHOT of one ring-full: nothing wraps, so a host that cannot drain as fast
    // as the ring fills simply trails and catches up, instead of losing records it has not read yet.
    // It is drawn AS IT FILLS, so the boundedness costs no immediacy — only the endlessness goes.
    // 100 ms: a page is 168 records, so the tick alone drains 1680 records/s — over the 1180 a 60-2
    // plus a cam makes at 1200 rpm. The chaining in onTrigLogPage covers what a fixed tick cannot: a
    // Nissan 180-slit armed on both edges is 7200 records/s and no interval short enough to keep up
    // with that is one worth polling at when idle.
    void startTriggerLog(int pollMs = 100);
    void stopTriggerLog();
    [[nodiscard]] bool triggerLogRunning() const { return trigLogRunning_; }
    void sendCli(const std::string &command);            // run an arbitrary CLI command on the ECU ('E')

    // SD file transfer: SD_MCU must already be in effect before calling these (or call sdMcu() first).
    // fetchFile / writeFile emit fileTransferProgress(bytes, total) as chunks complete,
    // then fileReceived(name, data) / fileWritten(name) on success, or fileError(name, msg) on failure.
    void sdMcu();                         // take card: ECU_OWNED immediately
    // Ask what the SD state is without changing it — replies on sdStatusReceived. This is the one
    // to poll; sdMcu() answers the same question by seizing the card, which is not a question.
    void sdStatus();
    void sdRelease();                     // release override, return to key-driven
    // Push the HOST's wall clock into the ECU's RTC. Called automatically when the identity
    // handshake completes, because the ECU's clock is otherwise never set by anything: on its first
    // power-up (and on every cold boot of a board with no backup battery) it holds its compiled
    // 2025-01-01 default, and get_fattime() stamps that onto every SD log file and learned-store totem. Exposed publicly
    // so it can also be re-issued by hand after the host's own clock is corrected.
    //
    // Reads the clock BEFORE setting it, so the log can say what the ECU thought the time was —
    // which is the diagnostic: "it was 2025-01-01" means the board cold-booted, and the file
    // timestamps written before this moment are meaningless.
    void syncRtc();

private:
    // Build and enqueue the SET, sampling the host clock at THIS moment. Called when the read
    // replies, so the value written is current however long the link was busy.
    void sendRtcSet();
public:

    void fetchFile(const std::string &name);  // chunked SD read
    void writeFile(const std::string &name, const std::vector<uint8_t> &data); // chunked SD write

    // Signals (plain data members; connect() a callback, .emit(...) to fire).
    jf::JSignal<std::string>                     identityReceived;   // (signature)
    jf::JSignal<std::vector<uint8_t>>            telemetryFrame;     // payload after the flag byte
    jf::JSignal<std::string>                     debugTextReceived;  // raw console text drained from the ECU
    jf::JSignal<std::vector<uint8_t>>            configImageReady;
    // How far a full config read has got, in bytes. Emitted a bounded number of times over the
    // whole transfer, NOT once per chunk: every emission wakes the main thread, and waking it 140
    // times is the cost the transfer thread exists to avoid.
    jf::JSignal<int, int>                        configReadProgress;   // (bytesDone, bytesTotal)
    jf::JSignal<int, std::vector<uint8_t>>       configRangeReady;   // a targeted readConfigRange chunk arrived (offset, bytes)
    jf::JSignal<std::vector<uint8_t>>            dtcImageReady;       // the serialized DtcManager image ('G' reply)
    jf::JSignal<KnockShot>                      knockShotReady;      // one decoded 0x28 reply
    // The ECU's real-time clock, after the studio corrected it on connect: (hadClock, message).
    // hadClock false means the board has no RTC at all — proteus_f7 spends the only OSC32 pins on
    // its power-good inputs — which is a fact to state once, not a failure to retry.
    jf::JSignal<bool, std::string>              rtcSynced;
    jf::JSignal<std::vector<uint8_t>>            engineCycleReady;    // assembled CycleHeader + CycleEdge[] (0x26)
    // Everything captured so far, re-emitted on every poll. Whole-vector rather than incremental
    // because the consumer redraws from it anyway, and a partial update it had to accumulate itself
    // would put the same bookkeeping in two places.
    jf::JSignal<std::vector<triggerlog::Record>> triggerLogUpdated;
    jf::JSignal<std::string>                     triggerLogFailed;
    // The snapshot is full and fully read: the capture is over, and the view should stop
    // saying 'live' about a buffer that is not filling any more.
    jf::JSignal<>                                triggerLogComplete;
    jf::JSignal<std::string>                     engineCycleFailed;   // (why) — never armed, never completed, link down
    jf::JSignal<bool>                            openedChanged;       // (open)
    jf::JSignal<std::string>                     errorOccurred;       // (message)
    jf::JSignal<int, int>                        writeFailed;         // a queued write exhausted its retries / was rejected (offset, size)
    // SD state (reply to sdMcu()): 0x00=ready, 0x01=no card, 0x02=busy/denied
    jf::JSignal<uint8_t>                         sdStatusReceived;    // (state)
    // SD file transfer
    jf::JSignal<std::string, int64_t, int64_t>   fileTransferProgress; // emitted per chunk (name, bytes, total)
    jf::JSignal<std::string, std::vector<uint8_t>> fileReceived;      // (name, data)
    jf::JSignal<std::string>                     fileWritten;         // (name)
    jf::JSignal<std::string, std::string>        fileError;           // (name, message)
    // Human-readable progress for the meta-resolve / SD dialog: retries, missed replies, give-ups.
    // isError=false → transient status (e.g. "no reply, retrying"); isError=true → terminal failure.
    jf::JSignal<std::string, bool>               linkActivity;        // (message, isError)

private:
    // One outstanding request, parked in pending_ keyed by its `seq`. The firmware echoes that seq in the
    // reply, so the inbound frame is matched to its Cmd by id (not by guessing from the flag byte) — the
    // typeId/flag is then only a sanity check on the resolved kind.
    enum class Kind { Identity, Telemetry, ConfigRead, ConfigRangeRead, ConfigWrite, Burn, DebugLog, DtcRead, Cli,
                      CycleArm, CycleRead,  // engine-cycle capture (0x26): arm, then poll + page
                      TrigLogArm, TrigLogRead, TrigLogStop,   // raw trigger log (0x27), streamed
                      SdMcu, SdRelease,     // SD ownership commands (no extra state)
                      SdStatus,             // 0x23 — a POLLED query, so its timeout is silent
                      FetchFile,            // chunked SD read
                      WriteFile,
                      KnockScope,           // 0x28 — latest classified knock shot
                      RtcRead, RtcSet };    // 0x29 — read the clock, then correct it
    struct Cmd {
        Kind                 kind;
        char                 code;
        std::vector<uint8_t> body;
        int                  offset = 0, size = 0;   // config read/write bookkeeping
        int                  retriesLeft = 0;        // resend-on-timeout budget (telemetry: 0, just drop)
        int                  timeoutMs = 1000;
        uint16_t             seq = 0;                 // correlation id: echoed by the reply we're matching
        int64_t              deadlineMs = 0;          // nowMs()+timeoutMs; retry/fail once this passes
    };

    void enqueue(const Cmd &c);             // queue a request; pump() releases it when the link is clear
    void pump();                            // release queued requests up to kMaxInFlight (see kMaxInFlight)
    bool bulkInFlight() const;              // anything queued or outstanding that is not the telemetry poll
    void onResponse(uint16_t seq, uint8_t flag, const std::vector<uint8_t> &data);
    void serviceRetries();                  // retry-timer tick: resend or fail every past-deadline entry
    void failCmd(const Cmd &cmd, const std::string &why);   // kind-specific give-up after retries are exhausted
    void requestTelemetry();                // poll tick: send an 'A' unless one is already pending
    void sendFrame(char cmd, const std::vector<uint8_t> &body, Kind kind, uint16_t seq);   // build + write one frame
    void parseBuffer();
    // Per-packet frame logging: hex+ASCII dump filed under a comms.* sub-category by frame kind
    // (telem/cfg/cli/dtc/sd/ctl) at Trace — enable one via JF_LOG or Preferences▸Logging.
    void logFrame(const char *dir, Kind kind, const std::vector<uint8_t> &frame) const;
    static const char *kindName(Kind k);   // human-readable command name for the comms.cmd log
    void resetQueue();                      // drop the resolve pool + in-flight state (on close)
    static int64_t nowMs();                 // steady-clock milliseconds, for deadlines

    jf::JSerialPort port_;
    std::string     portName_;               // name of the currently/last opened port
    uint16_t txSeq_ = 0;                      // omnidyno frame sequence counter (next correlation id)
    // A full config read runs here instead of through the queue above. It claims the port's read
    // stream for its duration, which is why the telemetry poll is stopped around it.
    ChunkReader chunkReader_;
    jf::JTimer pollTimer_;                   // telemetry cadence: each tick sends an 'A'
    jf::JTimer retryTimer_;                  // ~50 ms sweep: retry/expire pending_ entries
    std::vector<uint8_t> rx_;

    std::unordered_map<uint16_t, Cmd> pending_;   // outstanding (sent, awaiting reply) requests, keyed by seq
    std::deque<Cmd> txQueue_;                     // enqueued but not yet sent — released one at a time by pump()
    // The ECU's USB-CDC RX buffer holds exactly ONE frame (usbd_cdc_if APP_RX_DATA_SIZE = OMNI_MAX_FRAME) and
    // silently drops the overflow on a burst, so the host contract is strictly one request in flight. Pipelining
    // (e.g. readConfigRange firing every chunk at once) overran it and corrupted frames -> CRC drops. NEVER raise
    // this above 1 without enlarging that firmware RX buffer to hold the whole burst.
    static constexpr size_t kMaxInFlight = 1;
    bool telemPending_ = false;             // an 'A' is outstanding (don't pile up more)
    int  consecFailures_ = 0;              // consecutive non-telemetry failures → auto-disconnect
    static constexpr int MAX_CONSEC_FAILURES = 5;

    char cmdIdentity_  = 'Q';
    char cmdTelemetry_ = 'A';
    char cmdConfigRead_  = 'r';
    char cmdConfigWrite_ = 'w';
    char cmdBurn_ = 'b';
    char cmdDebugLog_ = 'D';
    char cmdDtcRead_ = 'G';
    // Engine-cycle capture. Not a printable letter like the rest — 0x26 is in the binary command
    // range the SD/file commands use, and the payload is binary too.
    static constexpr char    cmdCycle_        = '\x26';
    static constexpr uint8_t kCycleActionArm  = 0x00;
    static constexpr uint8_t kCycleActionRead = 0x01;
    // Raw trigger log — same binary command range, same paging shape, different question.
    static constexpr char    cmdTrigLog_        = '\x27';
    static constexpr char    cmdKnockScope_     = '\x28';
    static constexpr char    cmdRtc_            = '\x29';
    static constexpr uint8_t kRtcActionRead     = 0x00;
    static constexpr uint8_t kRtcActionSet      = 0x01;
    // What the ECU's clock said BEFORE we corrected it, captured by the RtcRead reply and reported
    // alongside the new time when the RtcSet reply lands. Empty if the board has no clock.
    std::string rtcBefore_;
    static constexpr uint8_t kTrigLogActionArm  = 0x00;
    static constexpr uint8_t kTrigLogActionRead = 0x01;
    static constexpr uint8_t kTrigLogActionStop = 0x02;
    // Paged DTC read. The firmware caps a 'G' reply at one frame's payload, so a table larger than a
    // page comes back in pieces; readDtcImage() walks them and emits ONE assembled image so DtcDock
    // parses exactly as before. dtcAccum_ holds [10-byte header][records so far]; dtcRecords_ is the
    // running total, patched into the header's n field when the last page lands.
    std::vector<uint8_t> dtcAccum_;
    uint16_t             dtcRecords_ = 0;
    static constexpr uint16_t kDtcMaxRecords = 64;   // DtcManager::SLOTS — loop guard
    void requestDtcPage(uint16_t first);
    void onDtcPage(const std::vector<uint8_t> &page);

    // ---- Engine-cycle capture (0x26) ---------------------------------------
    // Arm, then poll the same READ command until the ECU reports Complete, then page the edges out.
    // The poll is self-clocking off each reply rather than run from a timer: one round trip is a few
    // milliseconds, so this converges within a cycle without adding another clock to reason about.
    //
    // The poll IS bounded. A cycle takes 6 ms at 20k rpm but 1.2 s at 100 rpm, and on an engine that
    // is not turning at all it never closes — so an unbounded poll would sit in a tight request loop
    // against a stopped ECU forever. kCycleMaxPolls gives up and says why.
    static constexpr int      kCycleMaxPolls  = 200;   // ~2-4 s of round trips; then report "not turning"
    static constexpr uint16_t kCycleMaxEdges  = 4096;  // loop guard on paging (ECU rings total 416)
    void requestCyclePage(uint16_t first);
    void onCyclePage(const std::vector<uint8_t> &page);
    void failCycle(const std::string &why);
    // ---- Raw trigger log (0x27) --------------------------------------------
    // No poll loop. 0x26 waits for a cycle boundary to close; a trigger log has no boundary to wait
    // for, which is the whole reason it exists — so it fills for a stated time and we take what it
    // got. The ECU stops it when the ring is full, so a fast wheel simply finishes early.
    void requestTrigLogPage();
    void onTrigLogPage(const std::vector<uint8_t> &page);
    // The whole capture so far, and the ECU-side cursor it was drained with. `trigLogCursor_` is an
    // ABSOLUTE record index, not an offset into the vector — when the ECU's ring wraps past it, the
    // two stop agreeing and that difference is the gap we mark.
    // BOUNDED. A trigger log streams for as long as someone cranks, and this used to grow without
    // limit while every poll copied the whole thing through a by-value signal and re-decoded all of
    // it — O(n^2) work against a vector nobody had capped. At ~150 edges/s this is about 13 minutes
    // of cranking, far past any real session, and the oldest are the ones to lose: the display
    // follows the live end.
    static constexpr size_t kTrigLogRetain = 120000;
    std::vector<triggerlog::Record> trigLogRecs_;
    uint32_t             trigLogCursor_  = 0;
    // The ECU's monotonic record count as of the last page: the live edge, and where a clear
    // restarts from. Monotonic since the arm, so it is a position and not a size.
    uint32_t             trigLogTotal_   = 0;
    bool                 trigLogRunning_ = false;
    bool                 trigLogComplete_ = false;   // ECU reported Full AND we have paged it all out
    bool                 trigLogInFlight_ = false;   // one outstanding READ; polls do not stack
    jf::JTimer           trigLogPoll_;

    std::vector<uint8_t> cycleAccum_;      // [20-byte header][edges so far]
    uint16_t             cycleEdges_ = 0;  // edges accumulated across pages
    int                  cyclePolls_ = 0;  // polls spent waiting for the capture to close
    // Armed once per connection: recording on the ECU is continuous, so re-arming before every
    // frame only costs round trips (64.3% of cycles covered at 20833 rpm, against 99.5% for a plain
    // read). Cleared when the port closes — a newly connected ECU has not been armed.
    bool                 cycleArmed_ = false;
    bool                 cycleBusy_  = false;
    char cmdCli_ = 'E';
    int pollHz_ = 30;

    // active full-image read session, assembled from ConfigRead replies
    int configSize_ = 0;
    int blockSize_ = 2000;          // config chunk; raised to the firmware block size via setBlockSize()
    std::vector<uint8_t> configBuf_;
    int readCursor_ = 0, readEnd_ = 0;      // [readCursor_, readEnd_) still to read
    bool reading_ = false;
    // A clock correction waiting for the tune-image read to let go of the port. See syncRtc().
    bool rtcSyncPending_ = false;

    // SD file transfer state (shared for fetch and write)
    std::string          xferName_;     // current transfer filename
    std::vector<uint8_t> xferBuf_;      // accumulated data (fetch) or data to push (write)
    int64_t              xferOffset_ = 0; // byte offset of next chunk
    int64_t              xferTotal_  = 0; // total file size (fetch: from first reply; write: from input)
    // SD transfer chunk sizing derived from the firmware block size (blockSize_), never hardcoded:
    // a read requests up to a whole block (the firmware clamps to its block and reports actual_len);
    // a write leaves room for the frame's offset+name header so the frame still fits the firmware's
    // inbound buffer (OMNI_MAX_FRAME = block + slack). Keeps SD transfers correct as the block shrinks.
    int fileReadChunk()  const { return blockSize_; }
    int fileWriteChunk() const { return blockSize_ - static_cast<int>(xferName_.size()) - 8; }
    void enqueueNextFetchChunk();
    void enqueueNextWriteChunk();
};

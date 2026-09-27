#include "EcuLink.h"
#include <j/core/FrameTimer.h>   // the trigger log fills on a wall clock, not on a cycle boundary

#include "Crc32.h"
#include "OmniFrame.h"    // the wire format — shared with the transfer worker, so there is one parser

#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>   // declare a transfer so the run loop services it at the link's pace

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>   // std::time / localtime_r — the host clock pushed into the ECU's RTC
#include <cstring>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Little-endian codec + small byte helpers.
// ---------------------------------------------------------------------------
static uint32_t leU32(const unsigned char *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void appendLE32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static std::vector<uint8_t> toBytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}
static bool startsWith(const std::vector<uint8_t> &d, const char *s)
{
    const size_t n = std::strlen(s);
    return d.size() >= n && std::memcmp(d.data(), s, n) == 0;
}
static std::string hex8(uint32_t v)
{
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return std::string(buf);
}

// The wire format itself lives in OmniFrame.h — ONE parser, shared with the transfer worker.
using omni::leU16;
using omni::appendLE16;
using omni::crc16ccitt;
using omni::RSP_ACK;
using omni::RSP_TELEMETRY;
using omni::RSP_CONFIG;
using omni::RSP_BURN_ACK;
using omni::PACKET_IDENTITY;
using omni::RSP_FILE_DATA;

// Omnidyno frame typeIds beyond the ones OmniFrame names. See firmware/Comms/OmniProtocol.h.
enum : uint8_t {
    RSP_SD_STATUS = 0x09,   // SD command result: [state:u8] — SD_MCU + SD_STATUS queries
    RSP_DTC = 0x0A,         // DTC table image (BINARY) — its own code so it's never confused with text
    RSP_CYCLE = 0x0B,       // engine-cycle capture page (BINARY): CycleHeader + CycleEdge[]
    RSP_TRIGGER_LOG = 0x0C, // raw trigger log page (BINARY): TriggerLogHeader + records
    RSP_KNOCK_SCOPE = 0x0D, // latest classified knock shot (BINARY): KnockScopeReply + int16 buckets
    RSP_RTC = 0x0E,         // RtcReply (8 bytes): [present][sec][min][hour][wday][date][month][year-2000]
    RSP_ERROR = 0x84,
    CMD_SD_MCU = 0x20, CMD_SD_RELEASE = 0x22, CMD_SD_STATUS = 0x23,
    CMD_FETCH_FILE = 0x24, CMD_WRITE_FILE = 0x25,
};

EcuLink::EcuLink()
{
    // The port pushes received bytes on the main thread: append them and try to frame.
    port_.onData.connect([this](const std::vector<uint8_t> &d) {
        rx_.insert(rx_.end(), d.begin(), d.end());
        parseBuffer();
    });
    port_.onError.connect([this](const std::string &message) {
        errorOccurred.emit(message);
    });
    port_.onDisconnect.connect([this] {
        close();
    });
    pollTimer_.onTick.connect([this] { requestTelemetry(); });
    trigLogPoll_.onTick.connect([this] { requestTrigLogPage(); });
    retryTimer_.onTick.connect([this] { serviceRetries(); });
}

// close(false): `link` is a static-lifetime object, so it is destroyed at program exit AFTER main's stack
// locals (the ConnectButton + its scene graph, the diagnostics dock) that openedChanged fans out to. Emitting
// here would call setConnState/invalidate on freed widgets — the shutdown OOB. Release the port quietly.
EcuLink::~EcuLink() { close(false); }

// Steady-clock milliseconds — the timebase for pending-request deadlines.
int64_t EcuLink::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}



void EcuLink::open(const std::string &portName)
{
    close();
    portName_ = portName;
    JLOGC("comms", jf::JLogLevel::Info) << "opening " << portName << " @115200";
    if (!port_.open(portName, jf::JSerialPort::JBaudRate::B115200_)) {
        // THE SYSTEM'S REASON, not just "failed". Asked synchronously, because the port also reports
        // it through onError — and that is posted to the main thread, so it arrives long after this
        // line has already been written. "Access is denied" (something else holds the port) and "the
        // system cannot find the file specified" (it is gone) are the two answers worth acting on,
        // and they are indistinguishable without this.
        const std::string why = port_.lastError();
        JLOGC("comms", jf::JLogLevel::Error) << "open " << portName << " failed"
                                             << (why.empty() ? "" : ": ") << why;
        errorOccurred.emit("open " + portName + (why.empty() ? ": failed" : ": " + why));
        return;
    }
    rx_.clear();
    consecFailures_ = 0;
    // The OS receive buffer is flushed by the port on open, so stale bytes (e.g. an idle identity
    // broadcast mid-frame from a prior session) can't corrupt the handshake parse.
    resetQueue();
    JLOGC("comms", jf::JLogLevel::Info) << "port open — sending identity handshake, polling at " << pollHz_ << " Hz";
    openedChanged.emit(true);
    enqueue({Kind::Identity, cmdIdentity_, {}, 0, 0, 1, 2000});   // handshake
    pollTimer_.start(std::chrono::milliseconds(1000 / pollHz_),   // then poll telemetry
                     jf::JTimer::JMode::Repeating);
    retryTimer_.start(std::chrono::milliseconds(50),             // and sweep the resolve pool for retries
                      jf::JTimer::JMode::Repeating);
}

void EcuLink::close(bool emit)
{
    pollTimer_.stop();
    retryTimer_.stop();
    // The transfer thread holds the port's read stream and is blocked waiting on it. Stop it BEFORE
    // the port closes underneath it: cancel() returns only once the worker is joined and the claim
    // released, so nothing is left reading a handle we are about to destroy.
    chunkReader_.cancel();
    reading_ = false;
    resetQueue();
    // The trigger log dies with the link, and the FLAG has to die with it. Left set, it outlived the
    // connection twice over: startTriggerLog() early-returns on it, so the log could never be re-armed
    // after a reconnect, and every consumer that asks triggerLogRunning() (the cycle view's notice)
    // went on believing a decoder was disabled on an ECU we are no longer talking to. The ECU tears
    // its own side down on link loss, so there is nothing to send — this is purely our state.
    trigLogPoll_.stop();
    trigLogRunning_  = false;
    trigLogInFlight_ = false;
    if (port_.isOpen()) {
        port_.close();
        // The next ECU has not been armed, whoever it turns out to be.
        cycleArmed_ = false;
        cycleBusy_  = false;
        JLOGC("comms", jf::JLogLevel::Info) << "link closed";
        if (emit) openedChanged.emit(false);
    }
}

std::string EcuLink::portName() const { return portName_; }

bool EcuLink::isOpen() const { return port_.isOpen(); }

void EcuLink::resetQueue()
{
    pending_.clear();          // drop every outstanding request
    txQueue_.clear();          // and everything queued behind them
    telemPending_ = false;
    reading_ = false;
    // WITHDRAWN HERE TOO, because this is the one path that empties the queues without going
    // through pump(): closing the link. Left set, the run loop would go on giving comms a slice
    // on every frame for a link that no longer exists.
    jf::JMainThreadDispatcher::instance().setBurstSource(this, false);
}

// Build a config command body: [filler 0][selector 0][offset:u32 LE][size:u16 LE](+data for write).
// Config request payload (the typeId carries the command): [offset:u32 LE][size:u16 LE]([data] for write).
static std::vector<uint8_t> cfgBody(int offset, int size, const std::vector<uint8_t> &data = {})
{
    std::vector<uint8_t> b;
    appendLE32(b, static_cast<uint32_t>(offset));
    appendLE16(b, static_cast<uint16_t>(size));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

// --- the correlation-id resolve pool ---------------------------------------------------------------

// Queue a request. It is NOT sent here — pump() releases it once the link is clear, so we never put more
// than kMaxInFlight frames on the wire at once (the ECU's RX buffer holds exactly one). seq/deadline are
// stamped at send time in pump() so correlation ids stay in send order.
void EcuLink::enqueue(const Cmd &c)
{
    if (!port_.isOpen())
        return;
    txQueue_.push_back(c);
    pump();
}

// Release queued requests while fewer than kMaxInFlight are outstanding. This is the flow control that stops
// a chunk loop (readConfigRange) — or any burst — from overrunning the ECU's one-frame RX buffer. Called
// after every enqueue, every resolved reply, and every retry give-up, so the queue always drains.
// Is a BULK transfer in flight — something whose pace should not be the paint rate?
//
// Everything except the telemetry poll. Telemetry is a 30 Hz heartbeat feeding gauges that are
// only ever read off the screen, so servicing it faster than the screen redraws buys nothing and
// would keep the run loop in a comms slice permanently. Every other command is either part of a
// transfer (the 140 chunks of a tune image, an SD file) or a one-off the user is waiting on (a
// burn acknowledgement), and both finish sooner for being serviced at the link's pace.
bool EcuLink::bulkInFlight() const
{
    for (const auto& c : txQueue_)
        if (c.kind != Kind::Telemetry) return true;
    for (const auto& [seq, c] : pending_)
        if (c.kind != Kind::Telemetry) return true;
    return false;
}

void EcuLink::pump()
{
    // DERIVED, not counted. pump() runs after every enqueue, every resolved reply and every retry
    // give-up — that is every point where the answer can change — so recomputing it here cannot
    // drift, and an idempotent set means there is no begin/end pair to leak if a transfer aborts.
    struct BurstMark {
        const EcuLink* self;
        ~BurstMark() {
            jf::JMainThreadDispatcher::instance().setBurstSource(self, self->bulkInFlight());
        }
    } mark{this};

    while (port_.isOpen() && pending_.size() < kMaxInFlight && !txQueue_.empty()) {
        Cmd cmd = std::move(txQueue_.front());
        txQueue_.pop_front();
        cmd.seq        = txSeq_++;
        cmd.deadlineMs = nowMs() + cmd.timeoutMs;
        JLOGC("comms.cmd", jf::JLogLevel::Debug)
            << "\xE2\x86\x92 " << kindName(cmd.kind) << " code='" << cmd.code << "' seq=" << cmd.seq
            << " off=0x" << hex8(static_cast<uint32_t>(cmd.offset)) << " size=" << cmd.size
            << " body=" << cmd.body.size() << "B timeout=" << cmd.timeoutMs << "ms"
            << " [inflight " << (pending_.size() + 1) << "/" << kMaxInFlight
            << " queued " << txQueue_.size() << "]";
        sendFrame(cmd.code, cmd.body, cmd.kind, cmd.seq);
        pending_[cmd.seq] = std::move(cmd);
    }
}

void EcuLink::requestTelemetry()
{
    if (telemPending_ || !port_.isOpen())
        return;                            // don't pile up polls behind a read/write burst
    telemPending_ = true;
    enqueue({Kind::Telemetry, cmdTelemetry_, {}, 0, 0, 0, 800});
}

void EcuLink::readConfigImage()
{
    if (!port_.isOpen() || configSize_ <= 0 || reading_)
        return;
    reading_ = true;
    configBuf_.clear();
    readCursor_ = 0;
    readEnd_ = configSize_;
    JLOGC("comms.cfg", jf::JLogLevel::Info) << "full config read: " << configSize_ << " B in "
        << ((configSize_ + blockSize_ - 1) / std::max(1, blockSize_)) << " chunks of " << blockSize_
        << " \xE2\x80\x94 on the transfer thread";

    // THE POLL STOPS FIRST. The reader's claim on the port is exclusive, so a telemetry request
    // released while it runs would have its reply swallowed by the worker and time out — and worse,
    // the ECU holds one frame at a time, so interleaving a poll into the chunk stream is not
    // something the protocol can express in the first place. A tune read takes about a second; the
    // gauges hold their last value for it, which is what they already do between polls.
    pollTimer_.stop();

    const uint16_t firstSeq = txSeq_;
    txSeq_ = static_cast<uint16_t>(txSeq_ + (configSize_ / std::max(1, blockSize_)) + 8);

    const bool started = chunkReader_.start(
        port_, cmdConfigRead_, configSize_, blockSize_, firstSeq,
        [this](int done, int total) { configReadProgress.emit(done, total); },
        [this](std::vector<uint8_t> data, std::string error) {
            reading_ = false;
            if (port_.isOpen()) pollTimer_.start(std::chrono::milliseconds(1000 / pollHz_));
            // The chunk reader has let the port go, so a clock correction that deferred can run now.
            if (rtcSyncPending_) { rtcSyncPending_ = false; syncRtc(); }
            if (!error.empty()) {
                JLOGC("comms.cfg", jf::JLogLevel::Warn) << "full config read failed: " << error;
                errorOccurred.emit("config read failed: " + error
                                   + " — reflash the ECU if the layout changed.");
                return;
            }
            configBuf_ = std::move(data);
            readCursor_ = static_cast<int>(configBuf_.size());
            JLOGC("comms.cfg", jf::JLogLevel::Info) << "full config read complete: "
                << configBuf_.size() << " B \xE2\x86\x92 configImageReady";
            configImageReady.emit(configBuf_);
        });

    if (!started) {          // could not claim the stream — say so rather than hang on a dead read
        reading_ = false;
        if (port_.isOpen()) pollTimer_.start(std::chrono::milliseconds(1000 / pollHz_));
        // The chunk reader has let the port go, so a clock correction that deferred can run now.
        if (rtcSyncPending_) { rtcSyncPending_ = false; syncRtc(); }
        errorOccurred.emit("config read could not start: the link is busy");
    }
}

void EcuLink::readConfigRange(int offset, int size)
{
    if (!port_.isOpen() || size <= 0)
        return;
    const int end = offset + size;
    JLOGC("comms.cfg", jf::JLogLevel::Info) << "range read: off=0x" << hex8(static_cast<uint32_t>(offset))
        << " size=" << size << " in " << ((size + blockSize_ - 1) / std::max(1, blockSize_)) << " chunk(s)";
    for (int o = offset; o < end; o += blockSize_) {       // one request per block; each replies on its own
        const int n = std::min(blockSize_, end - o);
        enqueue({Kind::ConfigRangeRead, cmdConfigRead_, cfgBody(o, n), o, n, 2, 2000});
    }
}

void EcuLink::writeConfig(int offset, const std::vector<uint8_t> &data)
{
    if (!port_.isOpen() || data.empty())
        return;
    // Chunk at the firmware block size — ALWAYS, regardless of how small the caller thinks `data` is. A
    // single ConfigWrite frame must fit the firmware's parser buffer (OMNI_MAX_FRAME = header + one block
    // + CRC); a payload larger than one block — a whole-image push, or a coalesced dirty range that spans
    // a big table — would otherwise build an oversized frame that the firmware's PacketParser rejects
    // wholesale (frame_len > OMNI_MAX_FRAME -> reset()), silently dropping the ENTIRE write. Chunking here
    // (not just in writeConfigImage) makes every config-write path correct by construction, so no caller
    // can re-introduce the "pushed but nothing landed -> perpetually out of sync" bug.
    JLOGC("comms.cfg", jf::JLogLevel::Info) << "write: off=0x" << hex8(static_cast<uint32_t>(offset))
        << " size=" << data.size() << " in " << ((data.size() + blockSize_ - 1) / std::max(1, blockSize_))
        << " chunk(s)";
    for (int o = 0; o < static_cast<int>(data.size()); o += blockSize_) {
        const int len = std::min(blockSize_, static_cast<int>(data.size()) - o);
        const std::vector<uint8_t> part(data.begin() + o, data.begin() + o + len);
        enqueue({Kind::ConfigWrite, cmdConfigWrite_, cfgBody(offset + o, static_cast<int>(part.size()), part),
                 offset + o, static_cast<int>(part.size()), 3, 1000});
    }
}

void EcuLink::writeConfigImage(const std::vector<uint8_t> &image)
{
    // A whole-tune push is just a write from offset 0 — same block-chunking path. RAM only: NO burn here;
    // persisting to flash is a deliberate, separate burn() action (the studio never burns implicitly).
    writeConfig(0, image);
}

void EcuLink::burn()
{
    // Persist g_config to SD/flash. The firmware burn handler needs payload length >= 3, so the body
    // can't be empty. Only ever sent from an explicit user "Burn to flash" action.
    enqueue({Kind::Burn, cmdBurn_, std::vector<uint8_t>(3, 0), 0, 0, 1, 5000});
}

void EcuLink::requestDebugLog()
{
    if (!port_.isOpen())
        return;
    // 'D' drains the ECU text console — no arguments. The reply is whatever text accumulated since
    // the last poll (a double-buffer swap on the firmware side).
    enqueue({Kind::DebugLog, cmdDebugLog_, {}, 0, 0, 1, 1000});
}

void EcuLink::readDtcImage()
{
    if (!port_.isOpen())
        return;
    // 'G' is PAGED: the firmware clamps each reply to one frame's payload (10 + 36*28 = 1018 of 1030),
    // so a table over 28 records arrives in several pages. Walk them from 0 and reassemble. Sending the
    // page index is backward compatible — a firmware that ignores the body just answers page 0.
    dtcAccum_.clear();
    dtcRecords_ = 0;
    requestDtcPage(0);
}

namespace {
// RtcReply -> "2026-09-21 01:15:42". Calendar values, not BCD; year is 2000-based.
std::string formatRtc(const std::vector<uint8_t> &d)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  2000 + d[7], d[6], d[5], d[3], d[2], d[1]);
    return buf;
}
}  // namespace

void EcuLink::syncRtc()
{
    rtcBefore_.clear();

    // WAIT FOR THE PORT. A full config read does not go through this queue — ChunkReader claims the
    // read stream outright for its duration, which is why the telemetry poll is stopped around it.
    // A request issued into that window is transmitted and then its REPLY is invisible until the
    // chunk reader lets go, so it burns its timeout and gets resent for nothing. Observed exactly
    // that: "resend RtcRead (deadline missed)" on connect, because syncRtc() fires on the identity
    // handshake and the tune pull starts on the same event. Correcting a clock can wait a second.
    if (reading_) { rtcSyncPending_ = true; return; }

    // ONLY the read is enqueued here. The set is built when the read REPLIES, so the host clock is
    // sampled microseconds before the frame goes out rather than whenever this happened to be called.
    // That matters because syncRtc() fires on the identity handshake, and the connect burst — the full
    // config image pull — then owns the link for seconds: sampling here set the ECU a second BEHIND
    // the host on the very first run of this code, and a busier link would make it worse.
    //
    // The timeout is generous for the same reason. 1000 ms missed its deadline behind the tune pull
    // and had to be resent; correcting a clock is not urgent, so it waits rather than thrashing.
    enqueue({Kind::RtcRead, cmdRtc_, { kRtcActionRead }, 0, 0, 2, 5000});
}

void EcuLink::sendRtcSet()
{
    // LOCAL time, not UTC. These timestamps are read back by a person looking at a log file next to
    // a wall clock, and the ECU has no timezone to convert with.
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    // tm_wday is 0=Sunday; the RTC wants 1..7 with Monday=1 and Sunday=7.
    const uint8_t weekday = static_cast<uint8_t>(lt.tm_wday == 0 ? 7 : lt.tm_wday);
    std::vector<uint8_t> body{
        kRtcActionSet,
        static_cast<uint8_t>(lt.tm_sec > 59 ? 59 : lt.tm_sec),   // a leap second would be out of range
        static_cast<uint8_t>(lt.tm_min),
        static_cast<uint8_t>(lt.tm_hour),
        weekday,
        static_cast<uint8_t>(lt.tm_mday),
        static_cast<uint8_t>(lt.tm_mon + 1),                     // tm_mon is 0-based
        static_cast<uint8_t>(lt.tm_year + 1900 - 2000),          // tm_year is years since 1900
    };
    enqueue({Kind::RtcSet, cmdRtc_, std::move(body), 0, 0, 2, 5000});
}

void EcuLink::readKnockScope(bool lastEvent)
{
    if (!port_.isOpen()) return;
    enqueue({Kind::KnockScope, cmdKnockScope_, { static_cast<uint8_t>(lastEvent ? 1 : 0) }, 0, 0, 1, 1000});
}

void EcuLink::requestDtcPage(uint16_t first)
{
    const std::vector<uint8_t> body{ static_cast<uint8_t>(first & 0xFF), static_cast<uint8_t>(first >> 8) };
    enqueue({Kind::DtcRead, cmdDtcRead_, body, 0, 0, 1, 2000});
}

// One 'G' page landed: append its records, then either ask for the next page or emit the assembled
// image. A page reporting zero records is the end of the table.
void EcuLink::onDtcPage(const std::vector<uint8_t> &page)
{
    constexpr size_t kHdr = 10, kRec = 36;
    if (page.size() < kHdr) {                 // malformed/empty — hand it straight to the dock to report
        dtcImageReady.emit(page);
        dtcAccum_.clear(); dtcRecords_ = 0;
        return;
    }
    const uint16_t n = static_cast<uint16_t>(page[8] | (page[9] << 8));
    if (dtcAccum_.empty())                    // keep the first page's header (magic/version/boot_id)
        dtcAccum_.assign(page.begin(), page.begin() + kHdr);
    const size_t avail = page.size() - kHdr;
    const size_t use   = std::min(avail, static_cast<size_t>(n) * kRec);
    dtcAccum_.insert(dtcAccum_.end(), page.begin() + kHdr, page.begin() + kHdr + use);
    dtcRecords_ = static_cast<uint16_t>(dtcRecords_ + n);

    if (n > 0 && dtcRecords_ < kDtcMaxRecords) {   // more may follow; the empty page terminates us
        requestDtcPage(dtcRecords_);
        return;
    }
    dtcAccum_[8] = static_cast<uint8_t>(dtcRecords_ & 0xFF);   // header n = TOTAL across pages
    dtcAccum_[9] = static_cast<uint8_t>(dtcRecords_ >> 8);
    dtcImageReady.emit(dtcAccum_);
    dtcAccum_.clear();
    dtcRecords_ = 0;
}

void EcuLink::clearDtcs()
{
    if (!port_.isOpen())
        return;
    enqueue({Kind::Cli, cmdCli_, toBytes("dtc clear"), 0, 0, 1, 1500});   // CLI escape: wipe the table
}

// ---------------------------------------------------------------------------
// Engine-cycle capture (0x26) — arm ONCE, then just read.
//
// Recording on the ECU is continuous and arming is idempotent, so re-arming and then polling until
// Complete before every frame spends three round trips to fetch one cycle. Above about 12000 rpm the
// engine finishes cycles faster than that, and the frames simply get skipped: measured at 20833 rpm,
// arm+poll+read covered 64.3% of engine cycles while a single read covered 99.5%. The ring was never
// the constraint — the round trips were.
//
// So the arm happens once per connection and each capture is one read. A read that comes back not
// yet Complete still re-polls, which is what carries the very first capture after arming (the ECU
// needs a boundary to close a cycle) and any later loss of sync.
// ---------------------------------------------------------------------------
void EcuLink::captureEngineCycle()
{
    if (!port_.isOpen()) {
        engineCycleFailed.emit("not connected");
        return;
    }
    // One capture at a time. A second request while the first is polling would interleave two page
    // walks over the same accumulator and assemble a cycle out of both.
    if (cycleBusy_)
        return;
    cycleBusy_  = true;
    cycleAccum_.clear();
    cycleEdges_ = 0;
    cyclePolls_ = 0;
    if (!cycleArmed_) {
        cycleArmed_ = true;
        enqueue({Kind::CycleArm, cmdCycle_, {kCycleActionArm}, 0, 0, 1, 1500});
    } else {
        requestCyclePage(0);
    }
}

// ---- Raw trigger log (0x27), streamed ---------------------------------------------------------
//
// Arm, then drain on a timer until the user stops. Not one-shot: a fixed ring is not a unit of
// capture (4096 records is twenty minutes of a one-per-revolution trigger at cranking speed, and 3.4
// seconds of a 180-slot wheel armed Both), so the ECU wraps and we outrun it.
//
// The cursor is an ABSOLUTE record index the ECU assigns, not an offset into what we hold. If the
// ring wraps past it the ECU serves from where the data now starts and says so, and the difference
// is marked as a gap rather than letting two disconnected stretches butt together and read as one
// continuous capture.
void EcuLink::startTriggerLog(int pollMs)
{
    if (!port_.isOpen()) { triggerLogFailed.emit("not connected"); return; }
    // START ALWAYS MEANS A FRESH CAPTURE. This used to return silently when one was already running,
    // so a Start that could not be honoured looked exactly like one that had been — which is how a
    // stale running flag turned into "the second Start does nothing". Tear the old one down instead.
    if (trigLogRunning_) stopTriggerLog();
    trigLogRunning_  = true;
    trigLogInFlight_ = false;
    trigLogCursor_   = 0;
    trigLogTotal_    = 0;
    trigLogRecs_.clear();
    trigLogComplete_ = false;
    enqueue({Kind::TrigLogArm, cmdTrigLog_, {kTrigLogActionArm, 0, 0, 0, 0}, 0, 0, 1, 1500});
    trigLogPoll_.start(std::chrono::milliseconds(pollMs), jf::JTimer::JMode::Repeating);
}

void EcuLink::stopTriggerLog()
{
    if (!trigLogRunning_) return;
    trigLogRunning_ = false;
    trigLogPoll_.stop();
    // Tell the ECU. This is not housekeeping — arming DISABLED ITS DECODER, and the engine cannot
    // start until it comes back. (The ECU also tears this down on link loss, so a crashed studio
    // does not strand it, but that is the backstop and not the path.)
    if (port_.isOpen())
        enqueue({Kind::TrigLogStop, cmdTrigLog_, {kTrigLogActionStop, 0, 0, 0, 0}, 0, 0, 1, 1500});
}



void EcuLink::requestTrigLogPage()
{
    // One outstanding read at a time. A poll that fires while the previous is unanswered would
    // interleave two cursors over one accumulator and duplicate whatever arrived in between.
    if (!trigLogRunning_ || trigLogInFlight_ || !port_.isOpen()) return;
    trigLogInFlight_ = true;
    const uint32_t f = trigLogCursor_;
    enqueue({Kind::TrigLogRead, cmdTrigLog_,
             {kTrigLogActionRead,
              static_cast<uint8_t>(f & 0xFF), static_cast<uint8_t>((f >> 8) & 0xFF),
              static_cast<uint8_t>((f >> 16) & 0xFF), static_cast<uint8_t>((f >> 24) & 0xFF)},
             0, 0, 1, 1500});
}

// TriggerLogger::State::Full — the capture reached its record limit and stopped.
static constexpr uint8_t kTrigLogStateFull = 3;

void EcuLink::onTrigLogPage(const std::vector<uint8_t> &page)
{
    trigLogInFlight_ = false;
    std::vector<triggerlog::Record> fresh;
    const triggerlog::Header h = triggerlog::parsePage(page.data(), page.size(), fresh);
    if (h.magic != triggerlog::kMagic) {
        stopTriggerLog();
        triggerLogFailed.emit("trigger log: not a TLOG reply");
        return;
    }
    if (h.version != triggerlog::kVersion) {
        // The version byte exists so a reader REFUSES an unknown layout rather than inferring it
        // from the record size. Naming both is the whole point of the byte.
        stopTriggerLog();
        triggerLogFailed.emit("trigger log version " + std::to_string(int(h.version)) +
                              " — this studio reads version " +
                              std::to_string(int(triggerlog::kVersion)));
        return;
    }
    // Remembered on EVERY page, empty or not: a clear that happens while nothing new has arrived
    // still needs to know where the live edge is.
    trigLogTotal_ = h.total;
    if (!fresh.empty()) {
        // The ECU served from further on than we asked: its ring wrapped past our cursor and that
        // many records are gone for good. Mark the first survivor so the view draws a break there.
        if (h.first > trigLogCursor_) fresh.front().gapBefore = true;
        trigLogRecs_.insert(trigLogRecs_.end(), fresh.begin(), fresh.end());
        trigLogCursor_ = h.first + h.count;
        // Drop the oldest past the cap, and mark the new front as discontinuous — it is, and a
        // capture that silently forgets its beginning while still drawing as one run is the same
        // lie the ECU-side wrap guard exists to prevent.
        if (trigLogRecs_.size() > kTrigLogRetain) {
            trigLogRecs_.erase(trigLogRecs_.begin(),
                               trigLogRecs_.begin() + (trigLogRecs_.size() - kTrigLogRetain));
            trigLogRecs_.front().gapBefore = true;
        }
        triggerLogUpdated.emit(trigLogRecs_);
    }
    // THE SNAPSHOT IS DONE when the ECU says Full and we have read to its end. Keep polling until
    // then even though nothing more is being captured: a host that fell behind while the buffer
    // filled still has records to collect, and with no wrap they are all still there waiting.
    if (h.state == kTrigLogStateFull && trigLogCursor_ >= h.total && !trigLogComplete_) {
        // THE CAPTURE IS OVER, so it is not running any more. Leaving this set made the next Start
        // hit the guard below and return silently: the panel cleared, said it was capturing, and
        // nothing had been armed. It took a second click (which the toggle read as Stop) and a third
        // to actually run again.
        trigLogComplete_ = true;
        trigLogRunning_  = false;
        trigLogPoll_.stop();
        triggerLogComplete.emit();
        return;
    }
    // KEEP PULLING WHILE BEHIND, instead of asking once and sleeping until the next tick. The header
    // just told us exactly how far behind we are — `total` is what the ECU has captured, the cursor
    // is what we hold — so there is no need to guess or to poll for a count.
    //
    // One page is 168 records and the tick is 200 ms, so a page-per-tick drains 840 records/s. A 60-2
    // plus a cam at 1200 rpm produces 1180. That deficit no longer LOSES anything now the buffer is
    // bounded, but it still means the picture trails the engine by up to the whole capture, and the
    // point of drawing it live is that it does not. Chaining costs nothing when already caught up:
    // the condition is false and the tick takes over.
    if (trigLogCursor_ < h.total) requestTrigLogPage();
}

void EcuLink::requestCyclePage(uint16_t first)
{
    // [action:u8][back:u8][first:u16]. `back` is 0 — the newest complete cycle. It must be sent:
    // without it the ECU reads the low byte of `first` as the back index, so paging past edge 0 asks
    // for a cycle further and further into the past.
    const std::vector<uint8_t> body{ kCycleActionRead,
                                     0,
                                     static_cast<uint8_t>(first & 0xFF),
                                     static_cast<uint8_t>(first >> 8) };
    enqueue({Kind::CycleRead, cmdCycle_, body, 0, 0, 1, 1500});
}

void EcuLink::failCycle(const std::string &why)
{
    cycleBusy_ = false;
    cycleAccum_.clear();
    cycleEdges_ = 0;
    engineCycleFailed.emit(why);
}

// One 0x26 reply landed. Until the ECU says Complete we are still waiting for the cycle to close, so
// re-poll; after that, walk the pages and emit the assembled image.
void EcuLink::onCyclePage(const std::vector<uint8_t> &page)
{
    constexpr size_t kHdr = 20, kEdge = 4;
    constexpr uint8_t kStateComplete = 3;
    // 4 = Stale: a whole cycle, but the ECU lost the trigger after capturing it. It is finished data
    // and must be assembled, or a stalled engine would poll to the limit and report a failure when
    // what it actually has is the last cycle it turned.
    constexpr uint8_t kStateStale = 4;
    if (page.size() < kHdr) {
        failCycle("capture reply too short");
        return;
    }
    const uint8_t  state = page[3];
    const uint16_t total = static_cast<uint16_t>(page[6] | (page[7] << 8));
    const uint16_t count = static_cast<uint16_t>(page[10] | (page[11] << 8));

    if (state != kStateComplete && state != kStateStale) {
        // Idle / Armed / Recording — the cycle has not closed yet. An Idle state here means the arm
        // did not take (no recorder on the ECU: no tune, or firmware without the feature), and
        // re-polling that would spin to the limit for no reason, so say so immediately.
        if (state == 0) {
            failCycle("the ECU has no cycle recorder (no tune loaded, or older firmware)");
            return;
        }
        if (++cyclePolls_ > kCycleMaxPolls) {
            failCycle("capture never completed — is the engine turning?");
            return;
        }
        requestCyclePage(0);
        return;
    }

    if (cycleAccum_.empty())                  // keep the first page's header
        cycleAccum_.assign(page.begin(), page.begin() + kHdr);
    const size_t avail = page.size() - kHdr;
    const size_t use   = std::min(avail, static_cast<size_t>(count) * kEdge);
    cycleAccum_.insert(cycleAccum_.end(), page.begin() + kHdr, page.begin() + kHdr + use);
    cycleEdges_ = static_cast<uint16_t>(cycleEdges_ + count);

    if (count > 0 && cycleEdges_ < total && cycleEdges_ < kCycleMaxEdges) {
        requestCyclePage(cycleEdges_);
        return;
    }
    // The header's `count` describes a page; rewrite it to what we actually assembled so the decoder
    // reads one coherent image rather than a header describing only its first page.
    cycleAccum_[10] = static_cast<uint8_t>(cycleEdges_ & 0xFF);
    cycleAccum_[11] = static_cast<uint8_t>(cycleEdges_ >> 8);
    cycleBusy_ = false;
    engineCycleReady.emit(cycleAccum_);
    cycleAccum_.clear();
    cycleEdges_ = 0;
}

void EcuLink::sendCli(const std::string &command)
{
    if (!port_.isOpen() || command.empty())
        return;
    enqueue({Kind::Cli, cmdCli_, toBytes(command), 0, 0, 1, 1500});          // run a command button's CLI string
}

void EcuLink::sdMcu()
{
    if (!port_.isOpen()) return;
    enqueue({Kind::SdMcu, char(CMD_SD_MCU), {}, 0, 0, 2, 2000});
}

// ASKING, NOT TAKING. sdMcu() also answers with the SD state, but it seizes the card to do it —
// which is not a thing to do four times a minute just to find out whether one is fitted. 0x23 is
// the query on its own: no ownership change, no side effect, same reply.
void EcuLink::sdStatus()
{
    if (!port_.isOpen()) return;
    enqueue({Kind::SdStatus, char(CMD_SD_STATUS), {}, 0, 0, 0, 1500});
}

void EcuLink::sdRelease()
{
    if (!port_.isOpen()) return;
    enqueue({Kind::SdRelease, char(CMD_SD_RELEASE), {}, 0, 0, 1, 1000});
}

void EcuLink::fetchFile(const std::string &name)
{
    if (!port_.isOpen()) return;
    xferName_   = name;
    xferBuf_.clear();
    xferOffset_ = 0;
    xferTotal_  = -1;   // unknown until first reply
    enqueueNextFetchChunk();
}

void EcuLink::enqueueNextFetchChunk()
{
    std::vector<uint8_t> body;
    appendLE32(body, static_cast<uint32_t>(xferOffset_));
    appendLE16(body, static_cast<uint16_t>(fileReadChunk()));
    body.insert(body.end(), xferName_.begin(), xferName_.end());
    body.push_back('\0');
    enqueue({Kind::FetchFile, char(CMD_FETCH_FILE), body, 0, 0, 2, 10000});
}


// Declared, and its chunking, ack and progress handling all present — but the entry point itself was
// never written, so nothing could call it. Offset 0 makes the firmware create (or truncate) the file;
// the chunks then go one at a time, each sent when the last is acked. The file is flushed and closed
// on the card when the host sends SD_RELEASE (or starts another file), so a caller ends with sdRelease().
void EcuLink::writeFile(const std::string &name, const std::vector<uint8_t> &data)
{
    if (!port_.isOpen()) return;
    xferName_   = name;
    xferBuf_    = data;
    xferOffset_ = 0;
    xferTotal_  = static_cast<int64_t>(data.size());
    enqueueNextWriteChunk();
}

void EcuLink::enqueueNextWriteChunk()
{
    const int remaining = static_cast<int>(xferTotal_ - xferOffset_);
    if (remaining <= 0) {
        fileWritten.emit(xferName_);
        return;
    }
    const int chunkLen = std::min(fileWriteChunk(), remaining);
    std::vector<uint8_t> body;
    appendLE32(body, static_cast<uint32_t>(xferOffset_));
    body.insert(body.end(), xferName_.begin(), xferName_.end());
    body.push_back('\0');
    body.insert(body.end(), xferBuf_.begin() + xferOffset_, xferBuf_.begin() + xferOffset_ + chunkLen);
    enqueue({Kind::WriteFile, char(CMD_WRITE_FILE), body, 0, 0, 2, 8000});
}

// Retry-timer tick (~50 ms while open): resend or fail every pending request past its deadline. Collect
// the expired seqs up front so we never mutate pending_ while iterating it (a resend leaves the entry in
// place; a fail erases it, and failCmd may even close() and clear the whole pool).
void EcuLink::serviceRetries()
{
    if (!port_.isOpen() || pending_.empty())
        return;
    const int64_t now = nowMs();
    std::vector<uint16_t> expired;
    for (const auto &kv : pending_)
        if (kv.second.deadlineMs <= now)
            expired.push_back(kv.first);

    for (uint16_t seq : expired) {
        auto it = pending_.find(seq);
        if (it == pending_.end())
            continue;                                  // already gone (e.g. a prior close())
        Cmd &cmd = it->second;
        if (cmd.retriesLeft > 0) {                     // transient stall: resend the SAME frame (same seq)
            --cmd.retriesLeft;
            cmd.deadlineMs = now + cmd.timeoutMs;
            // Tell the SD/meta dialog something is wrong so it isn't sitting silently (the SD steps can
            // wait 10 s/attempt). Only the SD-resolve kinds drive that dialog.
            if (cmd.kind == Kind::SdMcu || cmd.kind == Kind::FetchFile || cmd.kind == Kind::WriteFile)
                linkActivity.emit("No reply from ECU — retrying (" + std::to_string(cmd.retriesLeft)
                                      + " attempt" + (cmd.retriesLeft == 1 ? "" : "s") + " left)…",
                                  false);
            JLOGC("comms.cmd", jf::JLogLevel::Warn) << "resend " << kindName(cmd.kind) << " seq=" << cmd.seq
                << " (deadline missed; " << cmd.retriesLeft << " retr" << (cmd.retriesLeft == 1 ? "y" : "ies")
                << " left)";
            sendFrame(cmd.code, cmd.body, cmd.kind, cmd.seq);
        } else {                                       // budget exhausted: drop it and run the failure path
            Cmd dead = std::move(it->second);
            pending_.erase(it);
            failCmd(dead, "no reply");
        }
    }
    pump();   // a give-up may have freed the in-flight slot — release the next queued request
}

void EcuLink::failCmd(const Cmd &cmd, const std::string &why)
{
    if (cmd.kind == Kind::Telemetry)
        JLOGC("comms.telem", jf::JLogLevel::Debug) << "telemetry poll dropped: " << why;
    else
        JLOGC("comms", jf::JLogLevel::Warn) << "command failed: kind=" << static_cast<int>(cmd.kind)
                                            << " (" << why << ")";

    // Non-telemetry failures count toward the auto-disconnect threshold (telemetry is best-effort).
    if (cmd.kind != Kind::Telemetry) {
        if (++consecFailures_ >= MAX_CONSEC_FAILURES) {
            JLOGC("comms", jf::JLogLevel::Error) << "auto-disconnect: " << consecFailures_
                << " consecutive non-telemetry failures (last: " << why << ")";
            close();
            return;
        }
    }

    switch (cmd.kind) {
    case Kind::Telemetry:                              // just drop it; the next poll re-requests
        telemPending_ = false;
        break;
    case Kind::ConfigWrite:
        writeFailed.emit(cmd.offset, cmd.size);
        break;
    case Kind::Burn:
        errorOccurred.emit("burn " + why);
        break;
    case Kind::Identity:
        errorOccurred.emit("ECU did not identify (" + why + ")");
        break;
    case Kind::DebugLog:
        break;
    case Kind::DtcRead:
        break;
    case Kind::Cli:
        break;
    case Kind::FetchFile:
    case Kind::WriteFile:
        fileError.emit(xferName_, why);
        break;
    case Kind::SdMcu:
    case Kind::SdRelease:
        // Was previously swallowed (default case) → the resolve dialog hung on "Checking SD card…".
        linkActivity.emit("ECU did not respond to the SD-card request (" + why + ").", true);
        break;
    default:
        break;
    }
}

// Wrapped frame: [len:2 BE][payload][crc32:4 BE], payload = cmd + body.
// Omnidyno frame: [AA 55][typeId][rsv][len:u16 LE][ts:u64 LE][seq:u16 LE][payload][crc16:2 LE], len = total.
// Human-readable command name for the decoded comms.cmd log (complements the raw comms.* hex dump).
const char *EcuLink::kindName(EcuLink::Kind k)
{
    switch (k) {
        case EcuLink::Kind::RtcRead:         return "RtcRead";
        case EcuLink::Kind::RtcSet:          return "RtcSet";
        case EcuLink::Kind::Identity:        return "Identity";
        case EcuLink::Kind::Telemetry:       return "Telemetry";
        case EcuLink::Kind::ConfigRead:      return "ConfigRead";
        case EcuLink::Kind::ConfigRangeRead: return "ConfigRangeRead";
        case EcuLink::Kind::ConfigWrite:     return "ConfigWrite";
        case EcuLink::Kind::Burn:            return "Burn";
        case EcuLink::Kind::DebugLog:        return "DebugLog";
        case EcuLink::Kind::DtcRead:         return "DtcRead";
        case EcuLink::Kind::CycleArm:        return "CycleArm";
        case EcuLink::Kind::CycleRead:       return "CycleRead";
        case EcuLink::Kind::Cli:             return "Cli";
        case EcuLink::Kind::SdMcu:           return "SdMcu";
        case EcuLink::Kind::SdRelease:       return "SdRelease";
        case EcuLink::Kind::FetchFile:       return "FetchFile";
        case EcuLink::Kind::WriteFile:       return "WriteFile";
    }
    return "?";
}

void EcuLink::logFrame(const char *dir, Kind kind, const std::vector<uint8_t> &frame) const
{
    // Per-packet hex+ASCII dump, filed under a comms.* sub-category by what the frame is — enable one
    // at Trace to capture just that traffic (e.g. only telemetry, or only config) without the fire-hose.
    const char *cat = "comms.ctl";
    switch (kind) {
        case Kind::Telemetry:                                                cat = "comms.telem"; break;
        case Kind::ConfigRead: case Kind::ConfigRangeRead:
        case Kind::ConfigWrite: case Kind::Burn:                             cat = "comms.cfg";   break;
        case Kind::Cli:        case Kind::DebugLog:                          cat = "comms.cli";   break;
        case Kind::DtcRead:                                                  cat = "comms.dtc";   break;
        case Kind::CycleArm: case Kind::CycleRead:                           cat = "comms.cycle"; break;
        case Kind::TrigLogArm: case Kind::TrigLogRead: case Kind::TrigLogStop: cat = "comms.trigLog"; break;
        case Kind::SdMcu:      case Kind::SdRelease:  case Kind::SdStatus:
        case Kind::FetchFile:  case Kind::WriteFile:                         cat = "comms.sd";    break;
        case Kind::Identity:   default:                                      cat = "comms.ctl";   break;
    }
    jf::JLog::instance().hexDump(jf::JLogLevel::Trace, cat, dir, frame.data(), frame.size());
}

void EcuLink::sendFrame(char cmd, const std::vector<uint8_t> &body, Kind kind, uint16_t seq)
{
    if (!port_.isOpen())
        return;
    const std::vector<uint8_t> frame = omni::buildFrame(cmd, body, seq);
    logFrame("[tx]", kind, frame);
    port_.write(frame);
}

void EcuLink::parseBuffer()
{
    // The framing itself is omni::takeFrame — shared with the transfer worker, so there is exactly
    // one implementation of resync, length bounds and CRC. What is left here is what only EcuLink
    // can do: match the frame to the request that is waiting for it.
    while (auto f = omni::takeFrame(rx_)) {
        auto pit = pending_.find(f->seq);
        logFrame("[rx]", pit != pending_.end() ? pit->second.kind : Kind::Identity, f->raw);
        onResponse(f->seq, f->typeId, f->payload);
    }
}

void EcuLink::onResponse(uint16_t seq, uint8_t flag, const std::vector<uint8_t> &data)
{
    // Match the reply to its request by the echoed correlation id. A seq we don't hold is an
    // unsolicited/late/duplicate frame (e.g. a reply that arrived after we already gave up on it) — ignore it.
    auto it = pending_.find(seq);
    if (it == pending_.end()) {
        JLOGC("comms", jf::JLogLevel::Debug) << "unmatched seq " << seq;
        return;
    }
    // Resolve: take the Cmd out of the pool before dispatching (so any follow-on send — the next config
    // or file chunk — can't collide with a stale entry). The SEQ is the authority; `flag` is only a
    // sanity check on the resolved kind below.
    const Cmd cmd = std::move(it->second);
    pending_.erase(it);
    consecFailures_ = 0;                          // any matched reply resets the drop counter
    if (cmd.kind == Kind::Telemetry)
        telemPending_ = false;                    // this poll resolved; the next tick may request again

    switch (cmd.kind) {
    case Kind::Identity:
        if (flag == PACKET_IDENTITY && startsWith(data, "jayecu")) {   // solicited OR unsolicited broadcast
            JLOGC("comms", jf::JLogLevel::Info) << "identity: " << std::string(data.begin(), data.end());
            identityReceived.emit(std::string(data.begin(), data.end()));
            // No control claim: the ECU accepts writes/burns from any valid frame, so the studio just writes.
            // Correct the ECU's clock now, while we know we are talking to one. Nothing else ever sets
            // it, and every SD log file and learned totem written from here on is stamped from it.
            syncRtc();
        }
        break;
    case Kind::Telemetry:
        if (flag == 0x01) {
            JLOGC("comms.telem", jf::JLogLevel::Trace) << "\xE2\x86\x90 telemetry " << data.size() << "B seq=" << seq;
            telemetryFrame.emit(data);
        }
        break;
    case Kind::ConfigRangeRead:
        if (flag == 0x02) {                          // a targeted gen-watch / segment range chunk
            JLOGC("comms.cfg", jf::JLogLevel::Debug) << "\xE2\x86\x90 range chunk off=0x"
                << hex8(static_cast<uint32_t>(cmd.offset)) << " " << data.size() << "B";
            configRangeReady.emit(cmd.offset, data);
        }
        break;
    case Kind::ConfigWrite:
        if (flag == 0x84) {                          // rejected (out of range / short): permanent, no retry
            JLOGC("comms.cfg", jf::JLogLevel::Warn) << "\xE2\x86\x90 write REJECTED off=0x"
                << hex8(static_cast<uint32_t>(cmd.offset)) << " size=" << cmd.size;
            writeFailed.emit(cmd.offset, cmd.size);
        } else {                                     // flag == 0x00 → ack: write landed
            JLOGC("comms.cfg", jf::JLogLevel::Debug) << "\xE2\x86\x90 write ACK off=0x"
                << hex8(static_cast<uint32_t>(cmd.offset)) << " size=" << cmd.size;
        }
        break;
    case Kind::Burn:
        JLOGC("comms.cfg", jf::JLogLevel::Info) << "\xE2\x86\x90 burn " << (flag == 0x04 ? "acked" : "reply flag=0x")
            << (flag == 0x04 ? "" : std::to_string(flag));
        break;
    case Kind::DebugLog:
        if (flag == RSP_ACK) {
            // The reply is the raw drained console text (may be empty when nothing was logged). The DTC
            // image now has its own code, so a stale binary reply can no longer land here.
            if (!data.empty())
                debugTextReceived.emit(std::string(data.begin(), data.end()));
        }
        break;
    case Kind::DtcRead:
        if (flag == RSP_DTC)                             // distinct code: can't collide with the text replies
            onDtcPage(data);                             // accumulate; emits dtcImageReady on the last page
        break;
    // 0x28 — one classified knock shot. An ECU with no tune answers with a well-formed reply carrying
    // seq 0 rather than an error, so "nothing to show" and "no link" stay distinguishable.
    case Kind::RtcRead:
        // What the ECU thought the time was before we corrected it — the diagnostic half of the
        // message. Then send the set, sampling the host clock NOW so the value we write is current.
        if (flag == RSP_RTC && data.size() >= 8)
            rtcBefore_ = data[0] ? formatRtc(data) : std::string();
        sendRtcSet();
        break;
    case Kind::RtcSet: {
        if (flag != RSP_RTC || data.size() < 8) break;
        const bool present = data[0] != 0;
        if (!present) {
            // Not a failure. This board has no clock — proteus_f7 spends PC14/PC15, the only OSC32
            // pins, on its power-good inputs — so say it once and never retry.
            JLOGC("comms", jf::JLogLevel::Info) << "ECU has no real-time clock; SD timestamps will be unset";
            rtcSynced.emit(false, "ECU has no real-time clock");
            break;
        }
        const std::string nowStr = formatRtc(data);
        const std::string msg = rtcBefore_.empty()
            ? ("ECU clock set to " + nowStr)
            : ("ECU clock was " + rtcBefore_ + ", set to " + nowStr);
        JLOGC("comms", jf::JLogLevel::Info) << msg;
        rtcSynced.emit(true, msg);
        break;
    }
    case Kind::KnockScope: {
        constexpr size_t kHdr = 32;   // KnockScopeReply, packed
        KnockShot sh;
        if (flag == RSP_KNOCK_SCOPE && data.size() >= kHdr && leU32(data.data()) == 0x4B534350u) {
            const uint8_t  n   = data[7];
            sh.valid     = true;
            sh.cyl       = data[5];
            sh.verdict   = data[6];
            sh.seq       = leU32(data.data() + 8);
            auto s16 = [&](size_t off) { return static_cast<int16_t>(leU16(data.data() + off)); };
            sh.db        = s16(12) * 0.1f;
            sh.floorDb   = s16(14) * 0.1f;
            sh.over      = s16(16) * 0.1f;
            sh.threshold = s16(18) * 0.1f;
            sh.sparkDeg  = s16(20) * 0.1f;
            sh.preFrac   = s16(22) * 0.01f;
            sh.startDeg  = s16(24) * 0.1f;
            sh.stepDeg   = s16(26) * 0.1f;
            const size_t have = (data.size() - kHdr) / 2;
            const size_t take = (n < have) ? n : have;   // trust the SHORTER of the two, never the header alone
            sh.buckets.reserve(take);
            for (size_t i = 0; i < take; ++i)
                sh.buckets.push_back(static_cast<int16_t>(leU16(data.data() + kHdr + i * 2)) * 0.1f);
        }
        knockShotReady.emit(sh);
        break;
    }
    // Both cycle actions answer with the same header, so the ARM reply is already a status: feed it
    // through the same path and the poll starts from whatever state the arm left the ECU in.
    case Kind::CycleArm:
    case Kind::CycleRead:
        if (flag == RSP_CYCLE) onCyclePage(data);
        else if (flag == RSP_ERROR) failCycle("the ECU rejected the capture request");
        break;
    // The ARM reply is a page header too, but there is nothing to read from it yet — the draining is
    // driven by the poll timer. An error is still handled, but it no longer has a known cause to
    // name: the ECU used to refuse an arm while the engine was RUNNING because arming disabled its
    // decoder, and it does neither now. A capture can be taken at any time, on a running engine.
    case Kind::TrigLogArm:
        if (flag == RSP_ERROR) {
            trigLogRunning_ = false;
            trigLogPoll_.stop();
            triggerLogFailed.emit("the ECU rejected the trigger log request");
        }
        break;
    case Kind::TrigLogRead:
        if (flag == RSP_TRIGGER_LOG) onTrigLogPage(data);
        else if (flag == RSP_ERROR) {
            trigLogInFlight_ = false;
            stopTriggerLog();
            triggerLogFailed.emit("the ECU rejected the trigger log read");
        }
        break;
    case Kind::TrigLogStop:
        break;   // fire and forget; the ECU also tears down on link loss
    case Kind::Cli:
        if (flag == RSP_ACK) {
            // Surface the command's text reply in the console (e.g. "findlimits 0 started …", or a
            // "?usage:" hint) — same channel as the debug drain, so command buttons report what happened
            // instead of silently swallowing the firmware's output.
            if (!data.empty())
                debugTextReceived.emit(std::string(data.begin(), data.end()));
        }
        break;
    case Kind::SdStatus:
    case Kind::SdMcu:
        if (flag == RSP_SD_STATUS && !data.empty())
            sdStatusReceived.emit(static_cast<uint8_t>(data[0]));
        else if (flag == RSP_ERROR)
            sdStatusReceived.emit(0x02);   // SD_STATE_BUSY — ECU rejected the request
        break;
    case Kind::SdRelease:
        // flag == RSP_ACK or RSP_ERROR → override released; nothing to emit.
        break;
    case Kind::FetchFile:
        if (flag == RSP_FILE_DATA && data.size() >= 10) {
            const uint32_t fileSize  = leU32(data.data());
            const uint32_t offset    = leU32(data.data() + 4);
            const uint16_t actual    = leU16(data.data() + 8);
            if (fileSize == 0xFFFFFFFF) {
                fileError.emit(xferName_, "file not found or card unavailable (err=0x" + hex8(offset) + ")");
            } else {
                if (xferTotal_ < 0) xferTotal_ = static_cast<int64_t>(fileSize);
                const int take = std::min(static_cast<int>(actual), static_cast<int>(data.size()) - 10);
                xferBuf_.insert(xferBuf_.end(), data.begin() + 10, data.begin() + 10 + take);
                xferOffset_ += actual;
                fileTransferProgress.emit(xferName_, xferOffset_, xferTotal_);
                if (actual == 0 || xferOffset_ >= xferTotal_)
                    fileReceived.emit(xferName_, xferBuf_);
                else
                    enqueueNextFetchChunk();     // sequential: request the next SD chunk
            }
        } else if (flag == RSP_ERROR) {
            fileError.emit(xferName_, "ECU returned error for FETCH_FILE");
        }
        break;
    case Kind::WriteFile:
        if (flag == RSP_ACK && data.size() >= 6) {
            const uint16_t written = leU16(data.data() + 4);
            xferOffset_ += written;
            fileTransferProgress.emit(xferName_, xferOffset_, xferTotal_);
            if (xferOffset_ >= xferTotal_)
                fileWritten.emit(xferName_);
            else
                enqueueNextWriteChunk();         // sequential: push the next SD chunk
        } else if (flag == RSP_ERROR) {
            fileError.emit(xferName_, "ECU returned error writing file at offset " + std::to_string(xferOffset_));
        }
        break;
    }

    pump();   // a slot just cleared — release the next queued request (any follow-on the dispatch enqueued
              // already went out; this covers the pre-queued chunks of a readConfigRange burst)
}

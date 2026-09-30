#pragma once

#include "../../generated/schema_meta.h"   // JAYECU_BLOCK_SIZE — the tuning-data block (single source)
#include <cstdint>
#include <cstddef>

// Omnidyno wire protocol (the jayecu comms framing). Sync-byte framed (0xAA 0x55) so a receiver can re-lock
// on frame boundaries over ANY transport (USB / CAN / serial). Identity is POLLED: the host sends
// OMNI_CMD_IDENTITY ('Q') and the ECU replies with a PACKET_IDENTITY frame — the ECU never broadcasts
// unsolicited. Mirrors omnidyno/shared/Protocol.h: a 16-byte PacketHeader + payload + CRC16-CCITT trailer.
//
//   [0xAA 0x55][typeId:1][reserved:1][length:u16 LE][timestamp:u64 LE][sequence:u16 LE][payload…][crc16:2 LE]
//
// length = TOTAL bytes (header + payload + 2-byte CRC). CRC16-CCITT (poly 0x1021, init 0xFFFF) covers
// every byte from the first sync up to (but not including) the CRC. typeId carries the command (request)
// or response code; jayecu keeps its own command set (below), unlike omnidyno's dyno-specific one.
namespace Comms {

inline constexpr uint8_t OMNI_SYNC1 = 0xAA;
inline constexpr uint8_t OMNI_SYNC2 = 0x55;

// Request typeIds (host -> ECU) = jayecu command bytes (kept from the old protocol, now the frame typeId).
enum : uint8_t {
    OMNI_CMD_IDENTITY     = 'Q',
    OMNI_CMD_TELEMETRY    = 'A',
    OMNI_CMD_CONFIG_READ  = 'r',
    OMNI_CMD_CONFIG_WRITE = 'w',
    OMNI_CMD_BURN         = 'b',
    OMNI_CMD_CLI          = 'E',
    OMNI_CMD_DEBUG        = 'D',
    OMNI_CMD_DTC_READ     = 'G',
    // (0x10–0x12 were the write-control claim/release/keep-alive — removed; writes are open to any frame.)
    // SD ownership protocol (overrides the key state; higher priority than key-driven).
    OMNI_CMD_SD_MCU          = 0x20,   // ECU takes card immediately (host must have unmounted first)
    OMNI_CMD_SD_MSC          = 0x21,   // give card to USB MSC regardless of key
    OMNI_CMD_SD_RELEASE      = 0x22,   // clear override; SD returns to key-driven state
    OMNI_CMD_SD_STATUS       = 0x23,   // query SD state; ACK payload = SdArbitrator::Status (8 bytes)
    OMNI_CMD_FETCH_FILE      = 0x24,   // read a named file from SD; payload = [offset:u32][chunk:u16][name...]
    OMNI_CMD_WRITE_FILE      = 0x25,   // write chunk to a named file; payload = [offset:u32][name NUL][data...]
                                       //   offset==0 → create/truncate; ACK payload = [offset:u32][written:u16]
    // Engine-cycle capture: one cycle of DELIVERED coil/injector/trigger edges in engine degrees.
    // payload = [action:u8] (0 = arm, 1 = read) [back:u8][first:u16] (read only).
    //
    // Recording is CONTINUOUS once armed; a read returns a whole cycle out of the ECU's ring. `back`
    // picks which: 0 is the newest complete cycle, 1 the one before it, up to CycleRecorder::BACK_MAX.
    // That exists because above ~12000 rpm the engine completes cycles faster than a round trip can
    // fetch one (measured: 61% coverage at 20833 rpm, 386 cycles skipped), while the ring is holding
    // them the whole time — so a host reads backwards to collect a contiguous run instead of only
    // ever the latest. `first` pages the edges of the selected cycle. The ECU never sends this
    // unsolicited.
    OMNI_CMD_CYCLE           = 0x26,
    // Raw trigger log — time-domain edges for a wheel the decoder does not understand. Distinct from
    // 0x26 because it answers a different question: 0x26 says where events landed on a wheel that
    // decodes, this says what the pins did when nothing decodes. It needs no sync and reports none.
    OMNI_CMD_TRIGGER_LOG     = 0x27,
    // The knock SCOPE: the latest classified measurement, whole. One shot per request, no paging —
    // a profile is ~150 bytes and fits a frame, so the host polls and gets whatever the classifier
    // last decided. Distinct from telemetry because it is EVIDENCE, not a value: the buckets only
    // mean something alongside the floor, spark and threshold they were judged against, and those
    // move every tick.
    OMNI_CMD_KNOCK_SCOPE     = 0x28,
    // Real-time clock. payload = [action:u8] and, for a set, seven more bytes:
    //   action 0 = read
    //   action 1 = set, followed by [sec][min][hour][weekday 1-7][date][month][year-2000]
    // The reply is always the SAME shape (RtcReply below) — the clock as it stands AFTER the
    // command — so a set is self-verifying and a host needs no second round trip to see what
    // took. `present` is how a host learns the ECU has no clock at all: proteus_f7 uses
    // PC14/PC15 for its power-good inputs, which are the only OSC32 pins, so it has no LSE
    // and no RTC. A host must not treat that as a failure to correct.
    //
    // THIS EXISTS BECAUSE NOTHING EVER SET THE CLOCK. platform_rtc_set() has been there from
    // the start with no caller, so the RTC could only ever hold its cold-boot default of
    // 2025-01-01 — and get_fattime() feeds that to FatFS, stamping every SD log and every
    // learned-store totem with a date that was never true. jaytek_v1 keeps time on its backup
    // battery once set; a board without one takes that default on EVERY cold boot, and correcting
    // it on connect is the only way its timestamps mean anything.
    OMNI_CMD_RTC             = 0x29,
};

// Reply to OMNI_CMD_RTC. Calendar values, not BCD — the same encoding platform_rtc_get uses.
struct __attribute__((packed)) RtcReply {
    uint8_t present;    // 1 = the ECU has a running RTC; 0 = no clock, the fields below are meaningless
    uint8_t second;     // 0-59
    uint8_t minute;     // 0-59
    uint8_t hour;       // 0-23
    uint8_t weekday;    // 1-7
    uint8_t date;       // 1-31
    uint8_t month;      // 1-12
    uint8_t year;       // 2000-based, so 26 == 2026
};
static_assert(sizeof(RtcReply) == 8, "RtcReply is a fixed 8 bytes");

// Knock scope reply (ECU -> host), little-endian. `seq` increments per classified measurement, so a
// host can tell a fresh shot from a re-read of the same one; 0 means nothing has been classified.
struct __attribute__((packed)) KnockScopeReply {
    uint32_t magic;         // KSCOPE_MAGIC
    uint8_t  version;       // stated outright, never inferred from length
    uint8_t  cyl;
    uint8_t  verdict;       // Knock::Verdict — clean / bootstrap / knock / pre-ignition
    uint8_t  buckets;       // profile buckets that follow as int16 dB x10
    uint32_t seq;
    int16_t  db_x10;        // measured level (gain-trimmed)
    int16_t  floor_x10;     // what it was measured against
    int16_t  over_x10;      // db - floor: the number actually thresholded
    int16_t  thr_x10;       // what it had to beat at that cell
    int16_t  spark_x10;     // advance BTDC when it was judged
    int16_t  pre_frac_pct;  // -1 = no phase information
    int16_t  start_deg_x10; // profile bucket 0 leading edge, signed (BTDC negative)
    int16_t  step_deg_x10;  // angular width of one bucket; 0 = unstamped
    uint16_t rpm;
    int16_t  load_x10;
    // int16_t db_x10[buckets] follows
};
static constexpr uint32_t KSCOPE_MAGIC   = 0x4B534350u;   // 'KSCP'
static constexpr uint8_t  KSCOPE_VERSION = 1;

// Sub-actions for OMNI_CMD_TRIGGER_LOG.
enum : uint8_t {
    TLOG_ACTION_ARM  = 0x00,   // start capturing. Never refused: the log copies edges and touches
                               // nothing, so it cannot stop an engine running or starting. A SNAPSHOT
                               // of one bufferful that never wraps, so nothing the host has yet to
                               // read is overwritten and `base` stays 0 for its whole life. Ends in
                               // state Full; the host arms again to recapture.
    TLOG_ACTION_READ = 0x01,   // [action:u8][first:u32] — everything from absolute index `first`
    TLOG_ACTION_STOP = 0x02,   // stop and put the decoder back
};

// Trigger-log page header (ECU -> host), little-endian, followed by `count` 6-byte records.
struct __attribute__((packed)) TriggerLogHeader {
    uint32_t magic;        // TLOG_MAGIC
    uint8_t  version;      // TLOG_VERSION — stated outright, never inferred from record size
    uint8_t  state;        // TriggerLogger::State
    uint32_t total;        // records written since the arm. MONOTONIC — the host's cursor.
    uint32_t base;         // oldest absolute index still held. ALWAYS 0 since v5 — nothing is
                           // overwritten — and kept precisely so a host can prove that, and notice
                           // immediately if it ever stops being true.
    uint32_t first;        // absolute index of the first record in THIS page
    uint16_t count;        // records in this page
};
// `first` > what the host asked for would mean records were dropped before it: that cannot happen
// in v5, and the check stays because it is what would catch a regression. It meant, and would mean,
// the host draws the span as missing rather than splicing two disconnected stretches into one
// continuous-looking picture. There is no `dropped` counter because there is nothing to count that
// (base, first) does not already say exactly.
//
// Deliberately NO sync level and NO rpm. What the decoder makes of these edges is a separate
// question with its own telemetry, and a log that answered it would be answering about a wheel it
// cannot read — which is the case it exists for. And rpm does not exist in a
// trigger log at all: edges/second is (features per revolution) x (revolutions per second), one
// equation with two unknowns, so no header field can honestly carry it.
static constexpr uint32_t TLOG_MAGIC   = 0x474F4C54u;   // 'TLOG'
// 5: the capture is a bounded SNAPSHOT, not a wrapping ring — one bufferful, then state Full. It
//    wrapped before and the host could not keep up: a page holds 168 records and the host asked
//    every 200 ms, so it drained 840/s against 1180/s
//    from a 60-2 plus a cam at 1200 rpm. Twelve seconds of slack, then permanent loss — drawn as
//    enormous intervals that looked like the wheel and were the link. No wire format changed: the
//    host still draws the buffer as it fills, so it loses no immediacy, and it now gets an END.
// 4: `streams` dropped from the header. It reported the configured stream COUNT, which over-stated
//    what a log contains — a slot can be enabled and name no pin — and nothing needed it: lanes are
//    created by the edges that actually arrive, each naming its own stream.
// 3: continuous capture. The ring WRAPS and records carry a monotonic absolute index, so the host
//    streams it while cranking instead of waiting for a fixed buffer to fill — 4096 records is
//    twenty minutes of a one-per-revolution trigger and 3.4 seconds of a 180-slot wheel armed Both.
//    Per-record sync bits are gone with the decoder that set them.
// 2: flags bits 5-7 name the stream that fired. v1 logs from a single-edge-armed stream were
//    undecodable (constant levels byte).
static constexpr uint8_t  TLOG_VERSION = 5u;

// Sub-actions for OMNI_CMD_CYCLE.
enum : uint8_t {
    CYCLE_ACTION_ARM  = 0x00,   // enable continuous recording (idempotent)
    CYCLE_ACTION_READ = 0x01,
};

// Response typeIds (ECU -> host).
enum : uint8_t {
    OMNI_RSP_ACK       = 0x00,   // receipt: command understood, no work-result to report
    OMNI_RSP_TELEMETRY = 0x01,
    OMNI_RSP_CONFIG    = 0x02,
    OMNI_RSP_BURN_ACK  = 0x04,
    OMNI_PACKET_IDENTITY = 0x07, // identity response (reply to a polled OMNI_CMD_IDENTITY 'Q')
    OMNI_RSP_FILE_DATA   = 0x08, // FETCH_FILE chunk: [file_size:u32][offset:u32][actual_len:u16][data...]
                                 //   file_size = 0xFFFFFFFF → no card / no file / busy
    OMNI_RSP_SD_STATUS   = 0x09, // SD command result: [state:u8] — see SdState enum
                                 //   SD_MCU: card probed synchronously; SD_STATUS: snapshot query
    OMNI_RSP_DTC       = 0x0A,   // DTC table image: [magic][ver][boot][n] + n DtcRecord (BINARY). Its own
                                 //   code so it can't be mistaken for the text debug-drain / CLI replies (ACK).
    OMNI_RSP_CYCLE     = 0x0B,   // engine-cycle capture page: CycleHeader + CycleEdge[] (BINARY)
    OMNI_RSP_TRIGGER_LOG = 0x0C, // raw trigger log page: TriggerLogHeader + TriggerLogRecord[] (BINARY)
    OMNI_RSP_KNOCK_SCOPE = 0x0D, // latest knock shot: KnockScopeReply + int16 bucket dB x10 (BINARY)
    OMNI_RSP_RTC       = 0x0E,   // RtcReply (8 bytes) — the clock AFTER the command, or present=0
    OMNI_RSP_ERROR     = 0x84,   // out of range / malformed
};

// Payload state byte for OMNI_RSP_SD_STATUS.
enum : uint8_t {
    SD_STATE_READY       = 0x00,  // ECU holds the card, SPI init OK
    SD_STATE_NO_CARD     = 0x01,  // ECU holds arbitrator but no physical card detected
    SD_STATE_BUSY        = 0x02,  // arbitrator denied (engine running / another override active)
};

#pragma pack(push, 1)
struct OmniPacketHeader {
    uint8_t  sync[2];    // OMNI_SYNC1, OMNI_SYNC2
    uint8_t  type_id;    // OMNI_CMD_* (request) or OMNI_RSP_*/OMNI_PACKET_* (response)
    uint8_t  reserved;   // 0
    uint16_t length;     // TOTAL frame bytes = header(16) + payload + crc(2)
    uint64_t timestamp;  // ms tick at emit (informational)
    uint16_t sequence;   // incrementing per emit (informational)
};
#pragma pack(pop)

inline constexpr size_t  OMNI_HEADER_SIZE = sizeof(OmniPacketHeader);   // 16
inline constexpr size_t  OMNI_CRC_SIZE    = 2;
inline constexpr size_t  OMNI_MIN_FRAME   = OMNI_HEADER_SIZE + OMNI_CRC_SIZE;

// Frame/buffer sizing DERIVED from JAYECU_BLOCK_SIZE (never hardcode 2048 — that would silently force tiny
// chunks, or let an over-chunking host build frames the parser rejects). The largest inbound frame is a
// config WRITE: header + [offset:u32][size:u16] (the 6-byte CFG sub-header) + a full block + CRC. The
// parser buffer (buf_[OMNI_MAX_FRAME]) must hold exactly that — so a host that chunks at JAYECU_BLOCK_SIZE
// fits and never stomps memory.
inline constexpr uint16_t OMNI_CFG_SUBHDR  = 6;                          // [offset:u32][size:u16]
inline constexpr uint16_t OMNI_MAX_PAYLOAD = JAYECU_BLOCK_SIZE + OMNI_CFG_SUBHDR;
inline constexpr uint16_t OMNI_MAX_FRAME   = static_cast<uint16_t>(OMNI_HEADER_SIZE + OMNI_MAX_PAYLOAD +
                                                                   OMNI_CRC_SIZE);

// CRC16-CCITT (poly 0x1021, init 0xFFFF) — matches omnidyno/shared/CRC16.h. The incremental form lets
// send_packet CRC the header then the payload straight from their source buffers (no staging copy).
inline uint16_t omni_crc16_step(uint16_t crc, const uint8_t* data, uint16_t length) {
    for (uint16_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}
inline uint16_t omni_crc16(const uint8_t* data, uint16_t length) {
    return omni_crc16_step(0xFFFF, data, length);
}

} // namespace Comms

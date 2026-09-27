#include "CommsManager.h"
#include "../Signal/Expr.h"            // the log gate's two conditions
#include "../Integration/OutputGate.h" // …and the latch/timers that decide what they mean over time
#include "OmniProtocol.h"       // sync-framed identity broadcast header
#include "well_known_telem.h"   // wkt:: rename-safe telemetry accessors
#include "Crc32.h"
#include "SdProtocol.h"                        // §8 SD serial protocol backend
#include "../Storage/SdArbitrator.h"           // SD ownership + SD_MCU/MSC/RELEASE/STATUS commands
#include "../Platform/stm32f7xx/SdCardSpi.h"  // SdCard_IsAvailable()
#include "../../generated/ecu_config.h"
#include "../../generated/learned_layout.h"  // LEARNED_PAGE_BASE (comms device base for the learned region)
#include "../Platform/platform_hal.h"
#include "../Platform/board_hal.h"               // board_device_uid()
#include "../Cli/CliRegistry.h"                // 'E' text command execute
#include "../Cli/CliCommands.h"                // register_default_commands()
#include "../Diagnostics/TextLog.h"            // 'D' — g_text_log text console drain
#include "../Diagnostics/DtcManager.h"         // 'G' — DTC table image read
#include "../Scheduler/CycleRecorder.h"        // 0x26 — engine-cycle capture arm/read
#include "../Engine/Modules/Knock.h"           // 0x28 — knock scope (last classified shot)
#include "../Scheduler/TriggerLogger.h"        // 0x27 — raw trigger log arm/read
#include "../Scheduler/EnginePositionHal.h"    // 0x27 arms by DISABLING the decoder
#include "../../generated/well_known_signals.h" // wk::engine_state — the arm-time run gate
#include "../../generated/shadow_meta.h"     // per-module reconfig watch regions ('w' classifier)
#include "../../generated/protocol.h"        // JAYECU_CMD_* command codes (single source of truth)
#include "CommandState.h"                    // set_command_state — arm the studio's re-read on a reconfigure
#include "../Engine/TelemScale.h"            // telem_round<> for the generated pack fragment
#include "../version.h"
#include <cstring>
#include <cstdio>

// SD arbitrator (defined in main.cpp) — used directly for SD ownership commands.
extern SdArbitrator g_sd_arb;

// The DTC table the 'G' command serializes — set by SystemComposer::compose() to &diag_.table().
DtcManager* g_dtc_table = nullptr;

// The engine-cycle capture buffer the 0x26 command arms and pages out. Set by main once the
// position HAL is wired; null on a board with no tune (the disabled state), where there is no
// scheduler running to record anything and the command answers with a header saying so.
CycleRecorder* g_cycle_recorder = nullptr;
// The knock classifier the scope reads. Set by main once the modules exist; null with no tune.
Knock* g_knock_scope = nullptr;
// The raw trigger log the 0x27 command arms and pages out. Set by main once the HAL exists.
TriggerLogger* g_trigger_logger = nullptr;
// The position HAL, so arming the log can DISABLE THE DECODER for the duration and stopping can put
// it back. Same wiring contract as the two above: main sets it once, null means the feature is not
// available rather than that the link is broken.
EnginePositionHal* g_position_hal = nullptr;

// Build the identity payload into `buf` (cap 96):
//   "<signature> <layout_hash> <uid-24-hex> t<telemetry_size>"
// Shared by the JAYECU_CMD_IDENTITY response and the idle identity broadcast.
//
// THE TELEMETRY SIZE IS PART OF THE IDENTITY. layout_hash covers the CONFIG byte layout and nothing
// else, so adding a telemetry channel leaves it unchanged while every channel after the new one moves
// in the frame. A client holding the older meta then decodes the whole frame one field out of step and
// reports plausible-looking rubbish, with nothing in the protocol to notice: exactly what happened when
// app_state took the frame from 786 to 787 bytes under an unchanged 5431b57e. Stating the size lets a
// client refuse a frame it cannot decode instead of misreading it.
//
// Appended AFTER the uid so the existing positional fields keep their indices.
static uint16_t fill_identity(char* buf) {
    static const char* const prefix = JAYECU_SIGNATURE " " JAYECU_LAYOUT_HASH_STR " ";
    const int n = static_cast<int>(strlen(prefix));
    memcpy(buf, prefix, n);
    uint8_t uid[12];
    board_device_uid(uid);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 12; ++i) {
        buf[n + i * 2]     = hexd[uid[i] >> 4];
        buf[n + i * 2 + 1] = hexd[uid[i] & 0x0F];
    }
    int k = n + 24;
    buf[k++] = ' ';
    buf[k++] = 't';
    unsigned ts = JAYECU_TELEMETRY_SIZE;                  // decimal, no printf on this path
    char digits[8]; int nd = 0;
    do { digits[nd++] = char('0' + ts % 10); ts /= 10; } while (ts && nd < 8);
    while (nd) buf[k++] = digits[--nd];
    return static_cast<uint16_t>(k);
}

// Monotonic counter bumped on every live config 'w' write. Consumers that derive
// state from g_config at init (Sensors: pin arbitration + enabled-channel cache)
// watch it and re-derive when it changes — `on_config_change()` is not wired, and
// re-init'ing would glitch filter state, so this is the lightweight live signal.
volatile uint32_t g_config_generation = 0;
// WHICH BYTES MOVED, not merely that something did. The writer knows the offset and the length; every
// consumer downstream was left to work it out for itself, and the one that could not — Sensors — swept
// all 134 elements with a CRC on EVERY write to anything, which measured 1.45 ms inside the 1 kHz
// engine frame whether a sensor had been touched or not.
//
// Accumulated (min/max) because several writes can land between two frames, and reset by the consumer
// when it takes them. lo > hi means "nothing recorded" — a writer that does not go through here (a burn
// restore, say) still bumps the generation, and the consumer falls back to the full sweep rather than
// trusting a range nobody set.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;
// The generation the save task last PERSISTED. `config_dirty` is (g_config_generation != this), so the
// indicator is derived from state that already exists rather than a flag every writer must remember to
// set -- there is no path that mutates config without bumping the generation, so none can slip through
// leaving the tune looking clean. Zero at boot equals the generation at boot, i.e. not dirty.
volatile uint32_t g_config_generation_saved = 0;
extern bool g_no_tune;   // main.cpp: the stored tune was rejected at boot; packed as no_tune

// Bench-command status word (op<<2 | phase) the studio watches to know when a routine finished and to
// re-read the cal it wrote. Set by the ETB / App modules; packed into telemetry below. See CommandState.h.
volatile uint16_t g_command_state = 0;

namespace Comms {

namespace {
// ---- Config-access wire layout (DIRECT 32-bit offsets, NO PAGES) -----------------------------
// We own the protocol, so config is one flat address space keyed
// by a 32-bit absolute g_config offset — no pages, no 64 KB limit. A wrapped config command is:
//   data[0]=cmd, data[1]=filler, data[2]=selector (0 = config), data[3..6]=offset (32 LE),
//   data[7..8]=size (16 LE), data[9..]=write payload.
inline uint16_t rd_u16(const uint8_t* p) noexcept {   // little-endian
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rd_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void wr_u16(uint8_t* p, uint16_t v) noexcept {   // little-endian
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
}
inline void wr_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);       p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}

// Bounds-check a direct config [offset, offset+size) range against g_config. Returns the offset, or
// -1 if it escapes the config. No page table — the whole config is one flat span.
inline long config_abs(uint32_t offset, uint16_t size) noexcept {
    if (static_cast<uint64_t>(offset) + size > sizeof(EcuConfig)) return -1;
    return static_cast<long>(offset);
}

// Learned "page": offsets ≥ LEARNED_PAGE_BASE (generated/learned_layout.h) route the SAME r/w block
// protocol to the RAM learned region (Lambda/Idle/VVT learned tables; persisted to SD by LearnedStore)
// instead of g_config — so the studio reads the learned tables (as ordinary meta config paths in that
// segment) to view/apply-to-base
// and writes zeros to reset them, with no new command. The base is far above any g_config offset, so
// ordinary config r/w is untouched. Writes here do NOT bump the config generation or flag a reconfig.
inline uint8_t* learned_ptr(uint32_t offset, uint16_t size) noexcept {
    if (offset < LEARNED_PAGE_BASE) return nullptr;
    const uint32_t reg_off = offset - LEARNED_PAGE_BASE;
    uint32_t cap = 0;
    auto* base = static_cast<uint8_t*>(platform_learned_base(&cap));
    if (!base || static_cast<uint64_t>(reg_off) + size > cap) return nullptr;
    return base + reg_off;
}

// Wrapped config command (selector 0): 32-bit offset at [3..6], size at [7..8], payload at [9..].
// Config request payload (omnidyno frame; the typeId is the command, so the payload is just):
//   [offset:u32 LE][size:u16 LE]([data…] for write)
constexpr unsigned CFG_OFF_AT  = 0;
constexpr unsigned CFG_CNT_AT  = 4;
constexpr unsigned CFG_DATA_AT = 6;
constexpr unsigned CFG_HDR_LEN = 6;
}  // namespace

CommsManager::CommsManager() = default;

// ---------------------------------------------------------------------------
// Telemetry frame — packed from the SignalBus on demand (see packed_frame()).
// ---------------------------------------------------------------------------

const EcuTelemetry& CommsManager::packed_frame() {
    pack_into(telem_frame_);
    return telem_frame_;
}

// INTO A CALLER'S BUFFER, because there is now a second reader on a different task. The datalog
// sampler runs at 1 kHz on its own task and needs a frame of its own: packing into telem_frame_
// from there would rewrite the comms snapshot mid-transmit, which is precisely the TOCTOU this
// frame was made private to end. Two buffers, no lock, no shared state.
void CommsManager::pack_into(EcuTelemetry& out) {
    // Pack the wire frame from the bus on demand. The result is private to the caller, so a
    // subsequent CRC + transmit read identical, stable bytes. Channels are read value-by-value
    // (each atomic); at most sub-ms cross-field skew, never a torn value.
    if (signal_bus_ != nullptr) {
        SignalBus& bus = *signal_bus_;
        EcuTelemetry& t = out;                // flat frame — pack writes t.<channel> directly
        #include "../../generated/sensor_telem_pack.inc"   // one read per channel, scaled per type
        // config_gen has no bus producer — it's this layer's firmware-write counter. Override the (zero)
        // bus read with the live value so the studio sees firmware-authored config writes move the level.
        t.config_gen = static_cast<uint16_t>(::g_config_generation);
        t.config_dirty = (::g_config_generation != ::g_config_generation_saved) ? 1u : 0u;
        // No valid tune at boot: same override pattern, nothing on the bus produces it (see no_tune).
        t.no_tune = ::g_no_tune ? 1u : 0u;
        // SD state, same override pattern and for the same reason: nothing on the bus produces it.
        // Three reads of a flag — the studio gates its logging UI on these rather than on a poll.
        t.sd_present    = SdCard_IsAvailable()     ? 1u : 0u;
        t.sd_msc_active = g_sd_arb.usb_has_card()  ? 1u : 0u;   // the PC has it: the key-off state
        t.sd_logging    = sd::is_logging()         ? 1u : 0u;
        // command_state — same story: no bus producer, set by the bench modules. Studio watches it to
        // re-read the cal a routine wrote (replaces the config_gen-armed scoped-refresh guesswork).
        t.command_state = ::g_command_state;
    }
}

// ---------------------------------------------------------------------------
// Transport management
// ---------------------------------------------------------------------------

bool CommsManager::add_transport(ITransport* transport) {
    if (stream_count_ >= MAX_TRANSPORTS) return false;
    streams_[stream_count_].transport = transport;
    streams_[stream_count_].parser.init(stream_count_, packet_callback_bridge, this);
    stream_count_++;
    return true;
}

// WHEN THE CARD LOGS. Two expressions and the same latch an output slot uses (OutputGate) — the
// deadband, the minimum on and off times, the maximum-on with its re-arm, and what an unanswerable
// condition means. Reused rather than re-derived: it is already the state a "should this be on right
// now" question needs, and it is already tested.
//
// The deadband matters MORE here than on a fan. An output that chatters at a threshold wastes relay
// life; a logger that chatters at one opens a NEW FILE every transition, header and all.
//
// BOTH CONDITIONS EMPTY = the built-in rule this replaced: log while the engine is turning. So a tune
// written before the gate existed behaves exactly as it did.
bool CommsManager::gate_says_log(uint32_t now_ms) {
    const auto& cfg = g_config.datalog;
    const bool has_on  = !expr::is_empty(cfg.log_when,  sizeof(cfg.log_when));
    const bool has_off = !expr::is_empty(cfg.log_until, sizeof(cfg.log_until));
    if (!has_on && !has_off)
        return wkt::rpm(packed_frame()) > 100.0f;      // the rule this used to be, unchanged

    expr::Ctx ectx;
    ectx.bus      = signal_bus_;
    ectx.cfg      = reinterpret_cast<const uint8_t*>(&g_config);
    ectx.cfg_size = sizeof(g_config);
    ectx.now_ms   = now_ms;

    // A LOGGER FAILS ON. An output's unanswerable-condition policy usually wants OFF — a starter must
    // not crank on a dead channel — but a channel going dark is exactly the moment a recording is
    // worth having. Same field, same three options, opposite default, and the slot says which.
    bool ask_on = false;
    if (has_on) {
        const expr::Result r = expr::exec(cfg.log_when, sizeof(cfg.log_when), ectx);
        ask_on = gate_answer(r.ran && r.v.ok, r.v.value, cfg.on_invalid, log_gate_.on != 0);
    }
    bool ask_off;
    if (has_off) {
        const expr::Result r = expr::exec(cfg.log_until, sizeof(cfg.log_until), ectx);
        ask_off = gate_answer(r.ran && r.v.ok, r.v.value,
                              cfg.on_invalid == GATE_INVALID_ON  ? GATE_INVALID_OFF :
                              cfg.on_invalid == GATE_INVALID_OFF ? GATE_INVALID_ON  : GATE_INVALID_HOLD,
                              log_gate_.on == 0);
    } else {
        ask_off = !ask_on;
    }

    const OutputGateTimings t{ cfg.min_on_ms, cfg.min_off_ms, cfg.max_on_ms, cfg.rearm_ms };
    log_gate_.step(ask_on, ask_off, t, now_ms);
    return log_gate_.on != 0;
}

void CommsManager::update(uint32_t delta_ms) {
    for (size_t i = 0; i < stream_count_; i++) {
        auto& s = streams_[i];
        s.parser.update(delta_ms);
        // Bulk, event-driven read: block briefly for the first bytes (the RX ISR wakes us the instant
        // a packet lands), then drain whatever else is queued without waiting. Replaces the per-byte
        // poll — far less overhead and near-zero RX latency to the parser.
        uint8_t rxbuf[128];
        size_t got = s.transport->receive_blocking(rxbuf, sizeof(rxbuf), 5);
        while (got > 0) {
            for (size_t k = 0; k < got; ++k) s.parser.parse_byte(rxbuf[k]);
            got = s.transport->receive(rxbuf, sizeof(rxbuf));   // drain the rest, non-blocking
        }

        // Surface frames the parser silently dropped. parse_byte() runs inline-in-ISR and only counts;
        // we're in task context here, so edge-log the new drops to the 'D' console. An oversized-frame
        // drop is almost always a host that failed to chunk a config write at the block size — exactly
        // the "pushed but nothing landed -> perpetually out of sync" failure, now visible instead of mute.
        const PacketParser::RejectStats rej = s.parser.reject_stats();
        if (rej.oversize != s.logged_oversize) {
            g_text_log.printf("WARN comms[%u]: dropped oversized frame len=%u (max=%u) "
                              "- chunk config writes at block size %u\n",
                              (unsigned)i, (unsigned)rej.last_len, (unsigned)OMNI_MAX_FRAME,
                              (unsigned)JAYECU_BLOCK_SIZE);
            s.logged_oversize = rej.oversize;
        }
        if (rej.crc != s.logged_crc) {
            g_text_log.printf("WARN comms[%u]: dropped frame on CRC mismatch (len=%u, count=%u)\n",
                              (unsigned)i, (unsigned)rej.last_len, (unsigned)rej.crc);
            s.logged_crc = rej.crc;
        }
    }

    // Status LED — blink when disconnected, solid when connected.
    // Using a simple heuristic: if any transport is available the link is up.
    bool connected = false;
    for (size_t i = 0; i < stream_count_; i++) {
        if (streams_[i].transport->is_available()) { connected = true; break; }
    }
    platform_led_connected(connected);

    // LINK LOSS TEARS DOWN THE SHORT-TERM DIAGNOSTIC LOGGERS. Both of them exist only to answer a
    // question a host is actively asking, and neither should outlive the host asking it — the
    // trigger log especially, because it holds the DECODER DISABLED, and a pulled cable would
    // otherwise leave an engine that cannot start with nothing left to re-enable it.
    //
    // This is physical-layer state, not a timeout: `connected` is USB CDC enumeration, the same
    // value that drives the blue COMMS LED one line above. No polling, no keepalive, no guessing at
    // how long silence means gone.
    if (!connected) {
        // A capture left running costs nothing now that it holds nothing hostage — it fills once and
        // stops — but the host is gone, so nobody is going to read it. Stop it so the next connection
        // starts from a clean logger rather than inheriting a stranger's half-finished capture.
        if (g_trigger_logger && g_trigger_logger->recording()) g_trigger_logger->stop();
        if (g_cycle_recorder && g_cycle_recorder->recording()) g_cycle_recorder->reset();
    }

    // Identity is POLLED, not broadcast: the host sends JAYECU_CMD_IDENTITY and we reply (echoing its
    // sequence). No unsolicited announce — the link only ever carries one clean reply per request, so
    // the host's correlation-id resolve pool never has to sift stray frames.
    (void)delta_ms;

    // (Write-control lock removed — nothing to time out / release here.)

    // SD datalogging (off unless g_config.datalog.enabled). Auto-starts when the engine is running and
    // the ECU owns the card; the logger appends a raw telemetry record at the configured rate and
    // self-closes on a key-off release (SdArbitrator writes_allowed / SD_WRITER_LOGGER). Inert by
    // default — enable + bench-validate before relying on it.
    if (sd::is_forced()) {
        // A bench pass owns the logger — don't let the gate close the file underneath it. This is why
        // it exists: a rig with no crank signal can never satisfy any sensible condition.
        //
        // IT STILL HAS TO BE DRAINED. Forcing skips the gate, not the writing: when sampling and
        // writing were one call this branch could do nothing and the record still landed, but the
        // sampler now fills a ring that only this side empties. Doing nothing here meant a forced
        // run filled 8 KB and dropped everything after it — which is exactly what the rig showed.
        sd::service_logging(platform_get_tick_ms());
    } else if (g_config.datalog.enabled) {
        const uint32_t now = platform_get_tick_ms();
        // The GATE is still evaluated here — it reads the bus and the config, which is this layer's
        // job — but the record no longer is. This side opens and closes the file and drains what the
        // sampler queued; the sampler owns the clock (see LogRing.h).
        if (gate_says_log(now) && g_sd_arb.writes_allowed() && SdCard_IsAvailable()) {
            if (!sd::is_logging()) sd::start_logging(g_config.datalog.rate_hz);
            sd::service_logging(now);
        } else if (sd::is_logging()) {
            sd::stop_logging_public();                    // condition released, or card handed off
        }
    } else if (sd::is_logging()) {
        sd::stop_logging_public();                        // feature switched off at runtime
    }
}

// ---------------------------------------------------------------------------
// Packet routing
// ---------------------------------------------------------------------------

void CommsManager::packet_callback_bridge(void* ctx, size_t stream_idx, uint8_t type_id, uint16_t sequence,
                                           const uint8_t* data, uint16_t length) {
    static_cast<CommsManager*>(ctx)->handle_packet(stream_idx, type_id, sequence, data, length);
}

void CommsManager::handle_packet(size_t stream_idx, uint8_t type_id, uint16_t sequence,
                                  const uint8_t* data, uint16_t length) {
    const uint8_t command = type_id;     // omnidyno frame: the typeId IS the command
    rx_packet_count_++;
    last_command_ = command;

    // Helper: send a response frame back on the originating stream.
    auto reply = [&](uint8_t rsp, const uint8_t* payload, uint16_t len) {
        send_packet(stream_idx, sequence,rsp, payload, len);
    };

    switch (command) {

        // Identity / handshake —
        //   "jayecu <board> <version> <build> <layout_hash> <device_uid>"
        // Tokens 0-2 are the GUI family tag, token 4 is the meta-match key (== JAYECU_LAYOUT_HASH),
        // token 5 is the 96-bit factory device UID (24 hex chars) used by the host to route the ECU to
        // its project. The static prefix is a compile-time literal; the UID is appended at runtime.
        case JAYECU_CMD_IDENTITY: {
            char buf[96];
            const uint16_t total = fill_identity(buf);
            reply(OMNI_PACKET_IDENTITY, reinterpret_cast<const uint8_t*>(buf), total);
            break;
        }

        // Realtime telemetry snapshot (the whole packed frame).
        case JAYECU_CMD_TELEMETRY: {
            const EcuTelemetry& f = packed_frame();
            reply(OMNI_RSP_TELEMETRY, reinterpret_cast<const uint8_t*>(&f), sizeof(EcuTelemetry));
            break;
        }

        // 'E' — execute a text console command. Wrapped-only.
        // Payload after the command byte is the command line; the reply is the
        // command's text output, returned with the OK flag. Rides the same
        // Omni framing — no new wire protocol.
        case JAYECU_CMD_CLI: {
            Cli::register_default_commands();          // idempotent (guarded)
            char cmdline[Cli::LINE_MAX];
            uint16_t n = length;                        // payload IS the command line (typeId is the cmd)
            if (n >= sizeof(cmdline)) n = sizeof(cmdline) - 1;
            if (n > 0) memcpy(cmdline, data, n);
            cmdline[n] = '\0';
            static char respbuf[Cli::REPLY_MAX];   // static: 2 KB must not sit on the comms-task stack
            Cli::Out out;
            out.buf = respbuf;
            out.cap = static_cast<uint16_t>(sizeof(respbuf));
            out.len = 0;
            Cli::execute(cmdline, out);
            // Cap at the frame payload: REPLY_MAX (2 KB) can exceed what one Omni
            // frame carries. Sending out.len uncapped announced a length the framing couldn't deliver,
            // so the payload never followed and the host blocked — the same trap 'G'/'D' guard against.
            uint16_t reply_len = (out.len > OMNI_MAX_PAYLOAD) ? OMNI_MAX_PAYLOAD : out.len;
            send_packet(stream_idx, sequence, 0x00, reinterpret_cast<uint8_t*>(out.buf), reply_len);
            break;
        }

        // 'D' — drain the text console (Lua errors, ecu_print, …). Wrapped-only. Reply is the raw
        // text accumulated since the last 'D' poll (a double-buffer swap): the host appends it to a
        // flat scrolling console. No records, no seq — see TextLog. Empty reply when nothing logged.
        case JAYECU_CMD_DEBUG: {
            size_t len = 0;
            const char* txt = g_text_log.swap_drain(&len);
            if (len > JAYECU_BLOCK_SIZE) len = JAYECU_BLOCK_SIZE;   // BUF_SIZE == block, so a drain (<=block-1) always fits — defensive
            reply(0x00, reinterpret_cast<const uint8_t*>(txt), static_cast<uint16_t>(len));
            break;
        }

        // 'G' — read the full DTC table (active + stored). Reply is the DtcManager serialized image
        // ([magic][ver][boot][n] + n DtcRecord), the same format SD persistence uses; the studio
        // parses DtcRecord[] to list every code. Fits one 4 KB block (≤64 records). Wrapped-only.
        // DTC table read, PAGED. Body is optional: [first:u16] = index into the stored set to start at
        // (absent/short body = 0, so an existing host that sends no body still works).
        //
        // The reply MUST be capped at OMNI_MAX_PAYLOAD. A full table serialises to
        // 10 + sizeof(DtcRecord)*64 = 2314 bytes, well over the 1030 a frame can carry: handing that to
        // reply() emitted a header announcing a length the framing could never deliver, so the payload
        // never followed and every client blocked waiting for it. It only bit once 29 DTCs were stored
        // (10 + 36*28 = 1018 fits, 10 + 36*29 = 1054 does not) -- which a batch of newly-enabled,
        // unwired sensors reaches in one go. The header's `n` is the count in THIS page, so a host pages
        // by adding it to `first` until a page comes back with fewer records than fit.
        case JAYECU_CMD_DTC_READ: {
            static_assert(10u + sizeof(DtcRecord) <= OMNI_MAX_PAYLOAD,
                          "one DTC record + header must fit a single frame, or paging can never progress");
            static uint8_t dtc_img[DtcManager::image_max_bytes()];
            const uint16_t first = (length >= 2) ? static_cast<uint16_t>(data[0] | (data[1] << 8)) : 0u;
            const uint32_t cap   = (sizeof(dtc_img) < OMNI_MAX_PAYLOAD) ? sizeof(dtc_img) : OMNI_MAX_PAYLOAD;
            const uint32_t n     = g_dtc_table ? g_dtc_table->serialize(dtc_img, cap, first) : 0;
            reply(OMNI_RSP_DTC, dtc_img, static_cast<uint16_t>(n));   // own code: never confused with text

            break;
        }

        // 0x28 — KNOCK SCOPE. One request, one shot: the latest classified measurement with the
        // evidence it was judged against. No arming and no paging — a profile fits a frame, and the
        // classifier is always running, so there is nothing to start and nothing to page.
        //
        // seq is what makes polling honest: it increments per classified measurement, so a host can
        // tell a fresh shot from the same one read twice, rather than inferring change from values
        // that may legitimately repeat.
        case OMNI_CMD_KNOCK_SCOPE: {
            static uint8_t buf[sizeof(KnockScopeReply) + KnockProfile::MAX_BUCKETS * sizeof(int16_t)];
            static_assert(sizeof(buf) <= OMNI_MAX_PAYLOAD, "a knock shot must fit one frame");
            KnockScopeReply h{};
            h.magic   = KSCOPE_MAGIC;
            h.version = KSCOPE_VERSION;
            if (!g_knock_scope) {
                // Not wired (no tune, so no modules): an empty shot with seq 0, which the studio reads
                // as "nothing to show" — not a protocol error and not a dead link.
                std::memcpy(buf, &h, sizeof(h));
                reply(OMNI_RSP_KNOCK_SCOPE, buf, sizeof(h));
                break;
            }
            // [action:u8] — 0 (or absent) = the newest measurement, 1 = the last non-clean EVENT.
            // Two questions, and the second is the one a person actually asks: "show me the knock",
            // not "show me whichever window happened to be classified when I asked".
            const uint8_t act = (length >= 1) ? data[0] : 0u;
            const Knock::Shot& sh = act ? g_knock_scope->last_event() : g_knock_scope->last_shot();
            const uint8_t n = (sh.profile.count < KnockProfile::MAX_BUCKETS)
                            ? sh.profile.count : KnockProfile::MAX_BUCKETS;
            h.cyl           = sh.cyl;
            h.verdict       = sh.verdict;
            h.buckets       = n;
            h.seq           = sh.seq;
            h.db_x10        = static_cast<int16_t>(sh.db * 10.0f);
            h.floor_x10     = static_cast<int16_t>(sh.floor_db * 10.0f);
            h.over_x10      = static_cast<int16_t>(sh.intensity * 10.0f);
            h.thr_x10       = static_cast<int16_t>(sh.threshold * 10.0f);
            h.spark_x10     = static_cast<int16_t>(sh.spark_deg * 10.0f);
            h.pre_frac_pct  = static_cast<int16_t>(sh.pre_frac * 100.0f);
            h.start_deg_x10 = static_cast<int16_t>(sh.profile.start_deg * 10.0f);
            h.step_deg_x10  = static_cast<int16_t>(sh.profile.step_deg * 10.0f);
            std::memcpy(buf, &h, sizeof(h));
            int16_t* db = reinterpret_cast<int16_t*>(buf + sizeof(h));
            for (uint8_t i = 0; i < n; ++i)
                db[i] = static_cast<int16_t>(sh.profile.db[i] * 10.0f);
            reply(OMNI_RSP_KNOCK_SCOPE,
                  buf, static_cast<uint16_t>(sizeof(h) + n * sizeof(int16_t)));
            break;
        }

        // 0x26 — engine-cycle capture. ARM requests one cycle; READ pages the result out.
        //
        // Both actions answer with a CycleHeader, so a READ against an unarmed, still-running or
        // absent capture is a normal reply carrying that state rather than an error the host has
        // to special-case: the host arms, then polls READ until the header says Complete. That
        // also makes the disabled-state ECU (no recorder) answer honestly instead of timing out.
        //
        // The reply is capped at OMNI_MAX_PAYLOAD by serialize() itself, which is what makes
        // paging terminate — the same trap the DTC read documents: announcing a length the
        // framing cannot deliver blocks the client forever.
        case OMNI_CMD_CYCLE: {
            static uint8_t cyc_img[sizeof(CycleHeader) +
                                   CycleRecorder::PAGE_MAX * sizeof(CycleEdge)];
            static_assert(sizeof(cyc_img) <= OMNI_MAX_PAYLOAD,
                          "a cycle page must fit one frame, or paging can never progress");
            const uint8_t  action = (length >= 1) ? data[0] : static_cast<uint8_t>(CYCLE_ACTION_READ);
            // [action:u8][back:u8][first:u16]. `back` counts cycles backwards from the newest
            // complete one, so a host can page a CONTIGUOUS run out of the ring instead of only ever
            // the latest — which is how it keeps up above ~12000 rpm, where the engine finishes
            // cycles faster than a round trip fetches them.
            const uint8_t  back   = (length >= 2) ? data[1] : 0u;
            const uint16_t first  = (length >= 4) ? static_cast<uint16_t>(data[2] | (data[3] << 8)) : 0u;

            if (!g_cycle_recorder) {
                // No recorder wired: report an empty Idle capture rather than a protocol error,
                // so the studio shows "not available" instead of a dead link.
                CycleHeader h{};
                h.magic   = CYCLE_MAGIC;
                h.version = CYCLE_VERSION;
                h.state   = static_cast<uint8_t>(CycleRecorder::State::Idle);
                memcpy(cyc_img, &h, sizeof(h));
                reply(OMNI_RSP_CYCLE, cyc_img, sizeof(h));
                break;
            }
            // ARM now means ENABLE. The recorder runs continuously once on and the reader picks the
            // last COMPLETE cycle out of the rings, so there is no one-shot to re-arm and no window
            // in which a boundary decision could be raced.
            //
            // THE TWO TOOLS CAN NOW RUN TOGETHER. This used to refuse a cycle arm while the trigger
            // log was recording, because that mode switched the decoder off and a cycle capture
            // cannot close a boundary without one — the host would re-request forever, measured at
            // ~55 requests/second, saturating the link with a question that had no answer. The log
            // leaves the decoder alone now, so the exclusion went with the reason for it.
            if (action == CYCLE_ACTION_ARM) {
                g_cycle_recorder->enable();
            }
            const uint16_t n = g_cycle_recorder->serialize(cyc_img, sizeof(cyc_img), first, back);
            reply(OMNI_RSP_CYCLE, cyc_img, n);
            break;
        }

        case OMNI_CMD_TRIGGER_LOG: {
            // Paged exactly like 0x26, and for the same reason: one capture is far larger than a
            // frame. PAGE_MAX is derived from the payload ceiling so the page always fits and paging
            // can always progress.
            static constexpr uint16_t TLOG_PAGE_MAX =
                static_cast<uint16_t>((OMNI_MAX_PAYLOAD - sizeof(TriggerLogHeader)) / sizeof(TriggerLogRecord));
            static uint8_t tl_img[sizeof(TriggerLogHeader) + TLOG_PAGE_MAX * sizeof(TriggerLogRecord)];
            static_assert(sizeof(tl_img) <= OMNI_MAX_PAYLOAD,
                          "a trigger-log page must fit one frame, or paging can never progress");
            const uint8_t  action = (length >= 1) ? data[0] : static_cast<uint8_t>(TLOG_ACTION_READ);
            const uint32_t first  = (length >= 5)
                ? static_cast<uint32_t>(data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24)) : 0u;

            TriggerLogHeader h{};
            h.magic   = TLOG_MAGIC;
            h.version = TLOG_VERSION;
            if (!g_trigger_logger) {
                // Not wired: an empty Idle page, not a protocol error — the host shows "unavailable"
                // rather than a dead link, the same contract 0x26 has.
                memcpy(tl_img, &h, sizeof(h));
                reply(OMNI_RSP_TRIGGER_LOG, tl_img, sizeof(h));
                break;
            }
            if (action == TLOG_ACTION_ARM) {
                // NO GATE. A capture takes a copy of what the pin did and changes nothing, so there
                // is no state it can be unsafe to take it in: a running engine can be captured, and
                // arming can no longer prevent one from starting. The refusal that used to live here
                // — engine RUNNING, plus a settle window for a decoder that had not resolved yet —
                // existed solely because arming disabled the decoder. It does not any more.
                g_trigger_logger->arm_now();
            } else if (action == TLOG_ACTION_STOP) {
                g_trigger_logger->stop();
            }
            h.state   = static_cast<uint8_t>(g_trigger_logger->state());
            h.total   = g_trigger_logger->total();
            h.base    = g_trigger_logger->base();
            // `served` is a local because h.first is a PACKED field and cannot bind to a reference.
            uint32_t served = first;
            const uint32_t n = g_trigger_logger->read(
                first, TLOG_PAGE_MAX,
                reinterpret_cast<TriggerLogRecord*>(tl_img + sizeof(TriggerLogHeader)),
                served);
            h.first = served;
            h.count = static_cast<uint16_t>(n);
            memcpy(tl_img, &h, sizeof(h));
            reply(OMNI_RSP_TRIGGER_LOG, tl_img,
                  static_cast<uint16_t>(sizeof(h) + n * sizeof(TriggerLogRecord)));
            break;
        }

        // Control arbitration. A writer (config/burn) must hold control; reads are open to all.

        // SD ownership protocol. Comms overrides take priority over the key state.
        // SD_MCU: ECU takes the card now (host has already unmounted via the protocol choreography;
        //         bypasses USB_QUIET wait). SD_MSC: give the card to USB regardless of key.
        // SD_RELEASE: clear override; SD returns to key-driven. SD_STATUS: query current state.
        case OMNI_CMD_SD_MCU: {
            g_sd_arb.set_comms_override(SdCommsOverride::MCU, platform_get_tick_ms());
            SdCard_Invalidate();
            SdCard_Init();   // synchronous probe; caller gets the real card state, not just a receipt
            const uint8_t sd_state = SdCard_IsAvailable() ? SD_STATE_READY : SD_STATE_NO_CARD;
            reply(OMNI_RSP_SD_STATUS, &sd_state, 1);
            break;
        }
        case OMNI_CMD_SD_MSC: {
            Comms::sd::close_write();   // card going to USB — flush + close any half-done WRITE_FILE handle
            Comms::sd::close_fetch();   // card going to USB — drop any half-done FETCH_FILE handle
            g_sd_arb.set_comms_override(SdCommsOverride::MSC, platform_get_tick_ms());
            reply(OMNI_RSP_ACK, nullptr, 0);
            break;
        }
        case OMNI_CMD_RTC: {
            // A set is applied first, then the clock is read back, so the reply describes what
            // the ECU actually holds rather than what the host asked for. An ECU with no clock
            // accepts the command and answers present=0: refusing it would make "this board has
            // no RTC" indistinguishable from "the link is broken".
            if (length >= 8u && data[0] == 1u) {
                (void)platform_rtc_set(data + 1);   // false = no clock; the read below says so
            }
            RtcReply r{};
            uint8_t now[8] = {0};
            r.present = platform_rtc_get(now) ? 1u : 0u;
            if (r.present) {
                r.second = now[0]; r.minute = now[1]; r.hour  = now[2];
                r.weekday= now[3]; r.date   = now[4]; r.month = now[5]; r.year = now[6];
            }
            reply(OMNI_RSP_RTC, reinterpret_cast<const uint8_t*>(&r), sizeof(r));
            break;
        }
        case OMNI_CMD_SD_RELEASE: {
            Comms::sd::close_write();   // host finished the push — flush + close so the file is durable
            Comms::sd::close_fetch();   // host cancelled / finished — release any streamed file handle
            g_sd_arb.set_comms_override(SdCommsOverride::NONE, platform_get_tick_ms());
            reply(OMNI_RSP_ACK, nullptr, 0);
            break;
        }
        case OMNI_CMD_SD_STATUS: {
            auto st = g_sd_arb.status_snapshot();
            st.card_present = SdCard_IsAvailable() ? 1 : 0;
            st.ecu_has      = g_sd_arb.ecu_has_card() ? 1 : 0;
            st.usb_has      = g_sd_arb.usb_has_card() ? 1 : 0;
            const uint8_t sd_state = !st.ecu_has        ? SD_STATE_BUSY
                                   : !st.card_present   ? SD_STATE_NO_CARD
                                                        : SD_STATE_READY;
            uint8_t buf[1 + sizeof(st)];
            buf[0] = sd_state;
            memcpy(buf + 1, &st, sizeof(st));
            reply(OMNI_RSP_SD_STATUS, buf, sizeof(buf));
            break;
        }
        case OMNI_CMD_FETCH_FILE: {
            // Payload: [offset:u32 LE][chunk:u16 LE][name: null-terminated string]
            if (length < 7) { send_packet(stream_idx, sequence,OMNI_RSP_ERROR, nullptr, 0); break; }
            const uint32_t offset = rd_u32(data);
            const uint16_t chunk  = rd_u16(data + 4);
            char name[128] = {};
            const uint16_t name_len = length - 6;
            std::memcpy(name, data + 6, name_len < 127 ? name_len : 127);
            static uint8_t file_buf[OMNI_MAX_PAYLOAD];
            const uint16_t out_len = Comms::sd::read_file(name, offset, chunk,
                                                           file_buf, sizeof(file_buf));
            reply(OMNI_RSP_FILE_DATA, file_buf, out_len);
            break;
        }

        case OMNI_CMD_WRITE_FILE: {
            // Payload: [offset:u32 LE][name: null-terminated string][data...]
            // offset==0 → create/truncate; offset>0 → seek into existing file.
            // ACK payload: [offset:u32 LE][written:u16 LE]
            if (length < 6) { send_packet(stream_idx, sequence,OMNI_RSP_ERROR, nullptr, 0); break; }
            const uint32_t offset = rd_u32(data);
            // find the NUL terminator of the name
            char name[128] = {};
            uint16_t ni = 0;
            while (ni < length - 4 && ni < 127 && data[4 + ni] != 0) { name[ni] = char(data[4 + ni]); ni++; }
            const uint16_t data_start = 4 + ni + 1;   // past NUL
            const uint16_t data_len   = (data_start < length) ? uint16_t(length - data_start) : 0;
            const uint16_t written    = Comms::sd::write_file(name, offset,
                                                               data + data_start, data_len);
            if (written == 0xFFFF) { send_packet(stream_idx, sequence,OMNI_RSP_ERROR, nullptr, 0); break; }
            uint8_t ack_buf[6];
            std::memcpy(ack_buf,     &offset,  4);
            std::memcpy(ack_buf + 4, &written, 2);
            reply(OMNI_RSP_ACK, ack_buf, sizeof(ack_buf));
            break;
        }

        // Read config — flat 32-bit absolute offset (selector 0).
        case JAYECU_CMD_CONFIG_READ: {
            // Flat 32-bit absolute offset, no pages. Payload = [offset:u32][size:u16].
            {
                if (length < CFG_HDR_LEN) break;
                const uint32_t offset = rd_u32(data + CFG_OFF_AT);
                const uint16_t size   = rd_u16(data + CFG_CNT_AT);
                // ONE FRAME'S WORTH, like every other reply. A larger request used to be honoured whole:
                // the frame's 16-bit length wrapped and the host could not parse what came back. Memory
                // access was always in bounds; the REPLY was not. Refused rather than truncated, so a
                // host asking for too much learns that it did instead of receiving fewer bytes than it
                // believes it asked for.
                if (size > JAYECU_BLOCK_SIZE) { send_packet(stream_idx, sequence, OMNI_RSP_ERROR, nullptr, 0); break; }
                if (uint8_t* lp = learned_ptr(offset, size)) {
                    reply(OMNI_RSP_CONFIG, lp, size);                            // learned page → BKPSRAM
                } else {
                    const long cfg_abs = config_abs(offset, size);
                    if (cfg_abs >= 0)
                        reply(OMNI_RSP_CONFIG, reinterpret_cast<const uint8_t*>(&g_config) + cfg_abs, size);
                    else
                        send_packet(stream_idx, sequence,OMNI_RSP_ERROR, nullptr, 0);     // out of range
                }
            }
            break;
        }

        // Write config — flat 32-bit absolute offset. Payload = [offset:u32][size:u16][data].
        case JAYECU_CMD_CONFIG_WRITE: {
            {                                          // no write-control lock: any valid frame may write
                if (length < CFG_HDR_LEN) break;
                const uint32_t offset = rd_u32(data + CFG_OFF_AT);
                const uint16_t size   = rd_u16(data + CFG_CNT_AT);
                const uint8_t* payload = data + CFG_DATA_AT;
                const bool have_data = length >= static_cast<uint32_t>(CFG_DATA_AT + size);
                if (uint8_t* lp = (have_data ? learned_ptr(offset, size) : nullptr)) {
                    memcpy(lp, payload, size);                    // learned page → BKPSRAM (no gen bump/reconfig)
                    send_packet(stream_idx, sequence, OMNI_RSP_ACK, nullptr, 0);
                    break;
                }
                const long cfg_abs = config_abs(offset, size);
                if (cfg_abs >= 0 && have_data) {
                    memcpy(reinterpret_cast<uint8_t*>(&g_config) + cfg_abs, payload, size);
                    // Signal live-config consumers (Sensors pin arbitration, enabled-channel
                    // cache) that g_config changed, so they re-derive on the next frame.
                    ++::g_config_generation;   // file-global, defined above (outside namespace Comms)
                    { const uint32_t lo = static_cast<uint32_t>(cfg_abs), hi = lo + size;
                      if (lo < ::g_config_dirty_lo) ::g_config_dirty_lo = lo;
                      if (hi > ::g_config_dirty_hi) ::g_config_dirty_hi = hi; }
                    // Per-module reconfig classification: if the write touches a module's structural
                    // watch region, flag THAT module pending — its bytes are in g_config RAM now, but
                    // the owning subsystem won't act on them until it reconfigures at its safe
                    // boundary (trigger/scheduler: engine-stopped). One module's change never flags
                    // another. Live writes (tables, trims) hit no region → instant.
                    const unsigned wlo = static_cast<unsigned>(cfg_abs), whi = wlo + size;
                    for (unsigned m = 0; m < JAYECU_SHADOW_MODULE_COUNT; ++m) {
                        const JayecuShadowRegion& r = JAYECU_SHADOW_REGIONS[m];
                        if (wlo < r.hi && whi > r.lo) {
                            shadow_pending_mask_ |= (1u << m);
                            // An even-fire ENGINE structural write arms a scheduler reconfigure that will
                            // recompute cyl[].tdc_angle when the engine stops. Flag it RUNNING so the studio
                            // waits, then re-reads engine.cyl on the OK edge EnginePositionHal emits. Odd-fire
                            // edits the angles directly (nothing to recompute), so it does not arm.
                            if (m == JAYECU_SHADOW_ENGINE && g_config.engine.odd_fire == 0)
                                set_command_state(cmdstate::ENGINE_RECONFIG, cmdstate::RUNNING);
                        }
                    }
                    send_packet(stream_idx, sequence,OMNI_RSP_ACK, nullptr, 0);
                } else {
                    send_packet(stream_idx, sequence,OMNI_RSP_ERROR, nullptr, 0);     // out of range / short
                }
            }
            break;
        }

        // §5.2 — Burn constants to flash
        case JAYECU_CMD_BURN: {
            // No write-control lock: any valid frame may request a burn.
            // Just flag the request + ACK. The config's self-integrity CRC is stamped by the save task
            // at store time (it owns persistence and writes the final g_config bytes).
            request_save();
            send_packet(stream_idx, sequence,OMNI_RSP_BURN_ACK, nullptr, 0);
            break;
        }

        default:
            send_packet(stream_idx, sequence,0x83, nullptr, 0);   // unknown command
            break;
    }
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

void CommsManager::send_packet(size_t stream_idx, uint16_t sequence, uint8_t flag,
                                const uint8_t* payload, uint16_t length) {
    if (stream_idx >= stream_count_) return;

    // Omnidyno frame: [AA 55][typeId][rsv][len:u16][ts:u64][seq:u16][payload…][crc16:2 LE], len = total.
    // Header built on the stack; payload sent straight from its source (g_config, telem snapshot, …) with
    // no staging copy — the CRC16 is accumulated incrementally over header then payload.
    OmniPacketHeader h{};
    h.sync[0]   = OMNI_SYNC1;
    h.sync[1]   = OMNI_SYNC2;
    h.type_id   = flag;          // request typeId echoed / response typeId
    h.reserved  = 0;
    h.length    = static_cast<uint16_t>(OMNI_HEADER_SIZE + length + OMNI_CRC_SIZE);
    h.timestamp = platform_get_tick_ms();
    h.sequence  = sequence;      // echo the request's correlation id (reply), or a fresh id (unsolicited)

    uint16_t crc = omni_crc16_step(0xFFFF, reinterpret_cast<const uint8_t*>(&h), OMNI_HEADER_SIZE);
    if (payload && length) crc = omni_crc16_step(crc, payload, length);
    const uint8_t trailer[2] = { static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>(crc >> 8) };  // LE

    ITransport* t = streams_[stream_idx].transport;
    t->send(reinterpret_cast<const uint8_t*>(&h), OMNI_HEADER_SIZE);
    if (payload && length) t->send(payload, length);
    t->send(trailer, OMNI_CRC_SIZE);
}

} // namespace Comms

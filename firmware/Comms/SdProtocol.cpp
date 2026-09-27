#include "SdProtocol.h"
#include "MlgLog.h"
#include "LogRing.h"
#include "CommsManager.h"      // pack_into() — the sampler packs a frame of its own
#include "../../generated/ecu_config.h"   // g_config.datalog.mask — the tune picks the columns

// The live tune. Declared at GLOBAL scope: everything below is inside Comms::sd, and an extern in
// there would name Comms::sd::g_config, which is not the object.
extern EcuConfig g_config;      // the log FORMAT — MLG v2, which MegaLogViewer reads directly
#include "../Platform/stm32f7xx/SdCardSpi.h"
#include "../Platform/stm32f7xx/SdVolume.h"   // SdCard_Init() / SdCard_IsAvailable()
#include "../Platform/platform_hal.h"
#include "../Storage/SdArbitrator.h"
#include "ff.h"
#include <cstring>
#include <cstdio>

// ECU/USB ownership shim (defined in SdArbitratorShim.cpp).
extern "C" bool SdArbitrator_EcuHasCard(void);
// SD card arbitrator (defined in main.cpp) — used to bracket comms card access.
extern SdArbitrator g_sd_arb;

namespace Comms {
namespace sd {

namespace {

// RAII guard: marks the comms writer busy for the duration of any card access
// (read OR write). The busy bit holds the ECU's ownership through a key-off
// release until the access completes, so the host's MSC and this code can never
// drive SPI3 at the same instant. Set the bit BEFORE checking card_ready().
struct CommsCardGuard {
    CommsCardGuard()  { g_sd_arb.writer_busy(SD_WRITER_COMMS); }
    ~CommsCardGuard() { g_sd_arb.writer_idle(SD_WRITER_COMMS); }
};

inline bool card_ready() { return SdCard_IsAvailable() && SdArbitrator_EcuHasCard(); }

// Datalogging state. The logger holds an open file on the shared mount (SdVolume.h) across records
// and clears it (+ the LOGGER busy bit) on stop. Off unless start_logging() succeeds; guarded by the
// SdArbitrator (LOGGER writer bit) so it never drives SPI at the same instant as USB MSC.
bool     s_logging      = false;
FIL      s_log_file;
uint32_t s_log_last_ms  = 0;
uint32_t s_log_records  = 0;
uint32_t s_log_period_ms = 100;   // 10 Hz default; set from the configured rate at start
uint8_t  s_log_counter  = 0;      // MLG's rolling record counter — wraps, and is meant to
// WHERE THE NEXT FILE NUMBER STARTS. The free slot used to be found by f_stat'ing from LOG0000
// upward on every engine start, so a card holding three hundred logs paid three hundred filesystem
// stats before the first record landed. Remembering where the last search finished makes that one
// stat in the normal case; it is only a hint, so a wrong one costs a scan and nothing else.
uint16_t s_next_log_idx = 0;
// THE COLUMNS OF THE OPEN FILE, resolved from the tune when it was opened and held for its life. A
// log whose columns changed part-way through would have lied about every record above the change, so
// re-reading the mask per record is exactly the thing not to do.
mlg::Selection s_log_sel;

// THE RING, and the two ends of it. See LogRing.h for why sampling and writing are separated at
// all. In DTCM: the 384K region has 108 bytes free, DTCM has room, and DMA never touches this.
//
// 8 KB is chosen against the WORST case rather than the average — a card that disappears for
// 200 ms to do an internal erase, while a 20-channel log runs at 1 kHz (42 B/record), needs
// 8.4 KB to ride it out without dropping a sample. At the 10 Hz default it is four hours of slack
// nothing will ever use, which is the right way round: the buffer exists for the bad moment.
__attribute__((section(".dtcm"))) uint8_t s_ring_store[8192];
mlg::LogRing s_ring;

// The sampler's OWN telemetry frame. It cannot share the comms one — that gets transmitted while
// this task would be rewriting it.
__attribute__((section(".dtcm"))) EcuTelemetry s_sample_frame;

// WHICH FIELDS, RESOLVED ONCE. write_record walks all MLG_FIELD_COUNT descriptors and tests a bit
// per record, so a 20-channel log cost almost exactly what a 367-channel one did (measured: 0.50 us
// against 1.54 us for eighteen times the data). At 10 Hz nobody notices; at 1 kHz it is the
// difference between the sampler being free and being a tax. So the selected indices are listed at
// start_logging and the hot path walks the list.
__attribute__((section(".dtcm"))) uint16_t s_sel_idx[MLG_FIELD_COUNT];
uint16_t s_sel_n = 0;

uint32_t s_log_sync_ms  = 0;      // last flush; sync is now on a CLOCK, not a record count
uint32_t s_log_dropped  = 0;      // records the ring refused — reported, never hidden

// Move whatever the sampler has queued onto the card. `passes` bounds how long this may hold the
// caller: the comms task wants a slice, the close path wants all of it.
// KEEP THE FILE POSITION SECTOR-ALIGNED.
//
// FatFS only takes its direct, multi-sector disk_write path when fptr % 512 == 0; otherwise every
// sector goes through the single-sector window, which is a READ-MODIFY-WRITE — two card operations
// per sector instead of one, and no chance for the card to stream.
//
// An MLG header is 24 + channels x 89 bytes, which is a multiple of 512 essentially never (4474 for
// fifty channels). Writing 1024-byte blocks from there preserves the misalignment forever, so the
// logger spent its whole life on the slow path: measured 890 writes for a five-second run, every
// one of them a single sector, longest run 1.
//
// So the first write after the header is trimmed to land exactly on the next boundary. After that
// every block is two whole sectors and stays aligned.
bool drain_to_file(int passes) {
    // 1 KB, AND BIGGER IS WORSE — measured, against the expectation. The reasoning for a larger
    // block is sound on paper: FatFS passes full sectors through to disk_write and the driver issues
    // a multi-block CMD25 for more than one, so the card should pipeline instead of paying a
    // busy-wait per sector. On the rig 8 KB took 50 channels at 1 kHz from 93.6 KB/s to 58.5, and
    // made 50 at 500 Hz drop for the first time. Whatever the cause, the number is the number.
    __attribute__((section(".dtcm"))) static uint8_t block[4096];
    for (int i = 0; i < passes; ++i) {
        const uint32_t mis = static_cast<uint32_t>(f_tell(&s_log_file)) % 512u;
        // Off a boundary: take exactly enough to get back on one. On a boundary: whole sectors
        // only, so the write stays aligned and FatFS keeps its direct multi-sector path — popping a
        // ragged tail here would put the file back off a boundary and cost a read-modify-write per
        // sector until the next realignment.
        uint16_t want;
        if (mis) {
            want = static_cast<uint16_t>(512u - mis);
        } else {
            const uint32_t avail = s_ring.used();
            want = static_cast<uint16_t>((avail / 512u) * 512u);
            if (want == 0) want = static_cast<uint16_t>(avail);   // never stall on a short tail
            if (want > sizeof block) want = static_cast<uint16_t>(sizeof block);
        }
        const uint16_t n = s_ring.pop(block, want);
        if (n == 0) return true;
        UINT bw = 0;
        if (f_write(&s_log_file, block, n, &bw) != FR_OK || bw != n) return false;
        s_log_records += n;
    }
    return true;
}

void stop_logging() {
    if (s_logging) {
        // DRAIN BEFORE CLOSING, or the last records never reach the card — and because a drain
        // writes blocks rather than whole records, what the file then ends with is HALF of one.
        // A log that stops with a torn record at the end is a log whose last moment is the one you
        // cannot read, which is usually the moment you stopped it to look at.
        s_logging = false;              // the sampler stops pushing, so this drain is the last word
        drain_to_file(64);              // 64 KB of backlog: far more than the ring can hold
        f_sync(&s_log_file);
        f_close(&s_log_file);
    }
    s_logging = false;
    g_sd_arb.writer_idle(SD_WRITER_LOGGER);
}

}  // namespace

static bool s_forced = false;
void set_forced(bool on) { s_forced = on; }
bool is_forced() { return s_forced; }

bool is_logging() { return s_logging; }

// --- FETCH_FILE streaming state -------------------------------------------------------------------
// A large file (the ~530 KB meta) is pulled in ~4 KB chunks. Re-mounting the volume, re-opening the
// file, and f_lseek-ing from byte 0 on EVERY chunk made deep chunks progressively slower (FatFS seek
// walks the cluster chain from the start) and occasionally exceeded the host's per-chunk timeout —
// the "stalls a few times during download" symptom. So keep the file open across sequential chunks.
static FIL      s_fetch_fil;
static bool     s_fetch_open = false;
static char     s_fetch_name[128] = {};
static uint32_t s_fetch_pos  = 0;       // current read position of s_fetch_fil (bytes consumed)

void close_fetch() {
    if (s_fetch_open) {
        f_close(&s_fetch_fil);
        s_fetch_open = false;
    }
    s_fetch_name[0] = '\0';
    s_fetch_pos = 0;
}

// --- WRITE_FILE streaming state (mirror of the FETCH fast-path) ------------------------------------
// A large file (the ~580 KB meta) is pushed in ~1 KB chunks. Re-mounting the volume, re-opening the
// file, f_lseek-ing to `offset`, f_sync-ing and f_close-ing on EVERY chunk cost ~200 ms/chunk — the
// f_sync forces a full FAT + directory flush (several SD sector writes) per chunk, and the seek walks
// the cluster chain from byte 0. So keep the file open across sequential chunks and sync once, on
// close. close_write() flushes + closes; it MUST run before any reader/other SD user touches the
// volume (read_file calls it; SD_MSC/SD_RELEASE call it) so the pushed bytes are durable and visible.
static FIL      s_write_fil;
static bool     s_write_open = false;
static char     s_write_name[128] = {};
static uint32_t s_write_pos  = 0;       // current write position of s_write_fil (bytes written)

void close_write() {
    if (s_write_open) {
        f_sync(&s_write_fil);           // flush FAT + dir entry now, once — the file is durable/visible
        f_close(&s_write_fil);
        s_write_open = false;
    }
    s_write_name[0] = '\0';
    s_write_pos = 0;
}

uint16_t read_file(const char* name, uint32_t offset, uint16_t chunk,
                   uint8_t* out, uint16_t out_max)
{
    // Minimum output: 10-byte sub-header [file_size:u32][offset:u32][actual_len:u16].
    if (out_max < 10) return 0;

    // Sentinel: file_size = 0xFFFFFFFF signals "unavailable / not found"; the offset field carries an
    // error detail for the studio log.
    auto put_sentinel = [&](uint32_t detail) -> uint16_t {
        const uint32_t fs = 0xFFFFFFFFu;
        std::memcpy(out,     &fs,     4);
        std::memcpy(out + 4, &detail, 4);
        out[8] = out[9] = 0;
        return 10;
    };

    CommsCardGuard guard;
    if (!SdArbitrator_EcuHasCard()) { close_fetch(); return put_sentinel(0); }

    // Reuse the already-open file for a sequential continuation (same file, not a fresh start). Only the
    // first chunk (offset 0) / a different file pays the mount+open cost; a host retry that re-asks an
    // earlier offset falls through to the re-seek below.
    const bool reuse = s_fetch_open && offset != 0 && std::strcmp(name, s_fetch_name) == 0;
    if (!reuse) {
        close_write();   // flush + close any file left open by a preceding push, so this read sees it
        close_fetch();
        // The shared mount (SdVolume.h). Unmounting and remounting here used to pull the volume from
        // under an open log file or a totem flush in another task.
        if (!sd_volume_ensure(g_sd_arb)) return put_sentinel(0);
        char path[128];
        std::snprintf(path, sizeof(path), "0:/%s", name);
        const FRESULT ores = f_open(&s_fetch_fil, path, FA_READ);
        if (ores != FR_OK) {
            return put_sentinel(0x1000u | static_cast<uint32_t>(ores));   // 0x1xxx = f_open err
        }
        std::strncpy(s_fetch_name, name, sizeof(s_fetch_name) - 1);
        s_fetch_name[sizeof(s_fetch_name) - 1] = '\0';
        s_fetch_open = true;
        s_fetch_pos  = 0;
    }

    const uint32_t file_size = static_cast<uint32_t>(f_size(&s_fetch_fil));

    // Seek only on a discontinuity (a host retry/resend of an earlier chunk); sequential reads just
    // continue from where the last read left off — no cluster-chain walk.
    if (offset != s_fetch_pos) {
        if (f_lseek(&s_fetch_fil, offset) != FR_OK) { close_fetch(); return put_sentinel(0x2000u); }
        s_fetch_pos = offset;
    }

    uint16_t actual = 0;
    if (offset < file_size) {
        uint16_t want = chunk;
        const uint32_t remaining = file_size - offset;
        if (want > remaining)  want = static_cast<uint16_t>(remaining);
        const uint16_t data_space = static_cast<uint16_t>(out_max - 10);
        if (want > data_space) want = data_space;
        UINT br = 0;
        if (f_read(&s_fetch_fil, out + 10, want, &br) != FR_OK) { close_fetch(); return put_sentinel(0x3000u); }
        actual       = static_cast<uint16_t>(br);
        s_fetch_pos += br;
    }

    // Reached EOF (or nothing left) → close the file.
    if (offset + actual >= file_size || actual == 0)
        close_fetch();

    std::memcpy(out,     &file_size, 4);
    std::memcpy(out + 4, &offset,    4);
    std::memcpy(out + 8, &actual,    2);
    return static_cast<uint16_t>(10u + actual);
}

uint16_t write_file(const char* name, uint32_t offset, const uint8_t* data, uint16_t data_len)
{
    CommsCardGuard guard;
    if (!SdArbitrator_EcuHasCard()) { close_write(); return 0xFFFF; }

    // Reuse the already-open file for a sequential continuation (same file, not a fresh start). Only
    // the first chunk (offset 0) / a different file pays the mount+open cost; the durable f_sync is
    // deferred to close_write(). A host that re-pushes from 0 gets a fresh CREATE_ALWAYS truncate.
    const bool reuse = s_write_open && offset != 0 && std::strcmp(name, s_write_name) == 0;
    if (!reuse) {
        close_write();
        if (!sd_volume_ensure(g_sd_arb)) return 0xFFFF;   // the shared mount (SdVolume.h)
        char path[128];
        std::snprintf(path, sizeof(path), "0:/%s", name);
        // offset == 0: create or truncate; offset > 0: open existing and seek.
        const BYTE mode = (offset == 0) ? (FA_WRITE | FA_CREATE_ALWAYS)
                                        : (FA_WRITE | FA_OPEN_EXISTING);
        if (f_open(&s_write_fil, path, mode) != FR_OK) return 0xFFFF;
        std::strncpy(s_write_name, name, sizeof(s_write_name) - 1);
        s_write_name[sizeof(s_write_name) - 1] = '\0';
        s_write_open = true;
        s_write_pos  = 0;
    }

    // Seek only on a discontinuity (a host retry/resend of an earlier chunk); sequential writes just
    // continue from where the last write left off — no cluster-chain walk, no per-chunk seek.
    if (offset != s_write_pos) {
        if (f_lseek(&s_write_fil, offset) != FR_OK) { close_write(); return 0xFFFF; }
        s_write_pos = offset;
    }

    UINT bw = 0;
    if (data_len > 0) {
        if (f_write(&s_write_fil, data, data_len, &bw) != FR_OK) { close_write(); return 0xFFFF; }
    }
    s_write_pos += bw;
    return static_cast<uint16_t>(bw);
}

bool start_logging(uint16_t rate_hz) {
    if (s_logging) return true;                 // already running
    // Must not collide with an in-flight file transfer on the shared single volume.
    if (s_fetch_open || s_write_open) return false;

    // Claim the LOGGER writer bit BEFORE checking ownership (the release protocol requires the bit
    // set first), then verify the ECU actually owns the card and writes are permitted.
    g_sd_arb.writer_busy(SD_WRITER_LOGGER);
    if (!g_sd_arb.writes_allowed() || !card_ready()) { g_sd_arb.writer_idle(SD_WRITER_LOGGER); return false; }

    if (!sd_volume_ensure(g_sd_arb)) { g_sd_arb.writer_idle(SD_WRITER_LOGGER); return false; }

    // First free LOGnnnn.MLG, from where the last search finished (see s_next_log_idx). The extension
    // is the format's own: a file named .BIN says nothing, and MegaLogViewer opens .mlg on sight.
    char path[32]; FILINFO fno;
    int idx = s_next_log_idx;
    for (; idx < 10000; idx++) {
        std::snprintf(path, sizeof(path), "0:/LOG%04d.MLG", idx);
        if (f_stat(path, &fno) != FR_OK) break;   // FR_NO_FILE -> this slot is free
    }
    if (idx >= 10000 && s_next_log_idx != 0) {    // the hint was past the end — search properly once
        for (idx = 0; idx < 10000; idx++) {
            std::snprintf(path, sizeof(path), "0:/LOG%04d.MLG", idx);
            if (f_stat(path, &fno) != FR_OK) break;
        }
    }
    if (idx >= 10000) { g_sd_arb.writer_idle(SD_WRITER_LOGGER); return false; }
    s_next_log_idx = static_cast<uint16_t>(idx + 1);

    if (f_open(&s_log_file, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) {
        g_sd_arb.writer_idle(SD_WRITER_LOGGER); return false;
    }
    // THE HEADER IS THE FILE'S MEANING. 24 bytes, then one 89-byte descriptor per channel carrying
    // its name, units, scale and digits — which is what lets this log be read years from now with no
    // reference to the meta it was written against. Written a descriptor at a time rather than from a
    // 21 KB buffer: the ECU has no such buffer to spare, and FatFS is happy to be fed in pieces.
    {
        // The tune decides the columns. All-zero falls back to the definition's own `datalog:` set,
        // so a tune that predates the mask logs the sensible thing rather than nothing.
        s_log_sel = mlg::resolve_selection(reinterpret_cast<const uint8_t*>(&g_config.datalog.mask[0]),
                                           DATALOG_MASK_COUNT);
        uint8_t buf[mlg::kDescriptorSize];
        UINT bw = 0;
        mlg::write_file_header(buf, s_log_sel);            // 24 bytes, and the descriptor buffer holds it
        bool ok = (f_write(&s_log_file, buf, mlg::kHeaderSize, &bw) == FR_OK && bw == mlg::kHeaderSize);
        // Descriptors for the SELECTED channels, in catalog order — the same order the record's
        // values are gathered in, which is the whole contract between the header and the data.
        for (uint16_t i = 0; ok && i < MLG_FIELD_COUNT; ++i) {
            if (!s_log_sel.has(i)) continue;
            mlg::write_field_descriptor(i, buf);
            ok = (f_write(&s_log_file, buf, mlg::kDescriptorSize, &bw) == FR_OK
                  && bw == mlg::kDescriptorSize);
        }
        // A header that did not land completely is a file no reader can use, so it does not become a
        // logging session — better no log than one that opens to nonsense.
        if (!ok) {
            f_close(&s_log_file);
            f_unlink(path);
            g_sd_arb.writer_idle(SD_WRITER_LOGGER);
            return false;
        }
        f_sync(&s_log_file);
    }

    s_log_records   = 0;
    s_log_last_ms   = 0;
    s_log_counter   = 0;
    s_log_sync_ms   = 0;
    s_log_dropped   = 0;
    // The sample period in whole milliseconds, which is the resolution the sampler task has. A rate
    // that does not divide 1000 lands on the nearest tick (300 Hz -> 3 ms -> 333 Hz), and 0 means
    // the old 10 Hz default rather than a divide by zero.
    s_log_period_ms = rate_hz ? (1000u / rate_hz) : 100u;
    if (s_log_period_ms == 0) s_log_period_ms = 1;      // above 1 kHz there is no faster tick

    // Flatten the selection into the list the hot path walks.
    s_sel_n = 0;
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i)
        if (s_log_sel.has(i)) s_sel_idx[s_sel_n++] = i;

    s_ring.attach(s_ring_store, sizeof s_ring_store);
    s_logging       = true;
    return true;
}

// ---------------------------------------------------------------------------
// THE SAMPLER SIDE. Runs on its own task at a fixed period, and touches neither the card nor the
// filesystem — it packs a frame, gathers a record and pushes it into the ring. Nothing here can
// block on SPI, which is the whole reason the sample interval is a timebase rather than a
// description of what the SD card and the USB link were doing.
// ---------------------------------------------------------------------------
uint32_t sample_period_ms() { return s_logging ? s_log_period_ms : 0; }

void sample_now(Comms::CommsManager& comms, uint32_t tick_ms) {
    if (!s_logging) return;
    comms.pack_into(s_sample_frame);

    // DTCM static, not a local: kMaxRecordBytes is 912 and the sampler's stack is 1536, which is
    // not a margin to spend on a buffer only one task ever writes.
    __attribute__((section(".dtcm"))) static uint8_t rec[mlg::kMaxRecordBytes];
    const uint16_t n = mlg::write_record_indexed(
        rec, s_sel_idx, s_sel_n, s_log_sel.record_len,
        reinterpret_cast<const uint8_t*>(&s_sample_frame), sizeof s_sample_frame,
        s_log_counter++, tick_ms);
    if (!s_ring.push(rec, n)) ++s_log_dropped;
}

uint32_t log_dropped()    { return s_log_dropped; }
uint32_t log_high_water() { return s_ring.high_water(); }

void stop_logging_public() { stop_logging(); }

void service_logging(uint32_t tick_ms) {
    if (!s_logging) return;
    // Ownership is being handed to USB (key-off release) or the card is gone: close cleanly while we
    // still own SPI. writes_allowed() goes false at the START of the release, before USB drives SPI,
    // so this stop() (which f_sync/f_closes) is safe and clears the LOGGER busy bit — letting
    // the release complete.
    if (!g_sd_arb.writes_allowed() || !card_ready()) { stop_logging(); return; }

    // DRAIN, don't sample. The records were gathered on the sampler's clock; this side only moves
    // bytes to the card, whenever it happens to run and for as long as there is something there.
    // A card stall now lands in the ring rather than in the sample interval, which is the entire
    // point of the split.
    //
    // Written in blocks rather than a record at a time: at 1 kHz a 42-byte record would be a
    // thousand f_write calls a second to fill two sectors, and FatFS charges for the call.
    // FOUR PASSES OF 4 KB. The bound is about the COMMS TASK, not the card: this runs on the task
    // that also answers the studio, and one visit that writes 64 KB holds it long enough to time a
    // command out — which is exactly what 8 KB blocks did on the rig. 16 KB is ~45 ms at the
    // measured 347 KB/s, and the ring covers the gap until the next visit.
    if (!drain_to_file(4)) { stop_logging(); return; }

    // SYNC ON A CLOCK, not a record count. Every 16 records is once every 1.6 s at 10 Hz and
    // sixty-two times a second at 1 kHz — a FAT and directory flush each, which at that rate is the
    // dominant cost of the whole feature. A second's worth of unflushed data is the exposure, and
    // that is the same exposure the old rule gave at the default rate.
    if (tick_ms - s_log_sync_ms >= 1000u) {
        s_log_sync_ms = tick_ms;
        f_sync(&s_log_file);
    }
}

}  // namespace sd
}  // namespace Comms

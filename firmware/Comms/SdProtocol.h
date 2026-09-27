#pragma once
// ---------------------------------------------------------------------------
// SD-card file transfer over the Omni protocol.
//
// Backs the FETCH_FILE / WRITE_FILE commands (the ~530-580 KB meta descriptor is
// streamed in chunks) with the SPI SD driver (SdCardSpi) and FatFS. All card
// access is gated on SdArbitrator_EcuHasCard(): if the USB MSC backend owns the
// card the operations report unavailable, which is the spec-correct response.
//
// Firmware-target only (pulls in FatFS). Not part of the host unit-test build.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>

namespace Comms {
class CommsManager;   // the sampler packs its own frame through it (see sample_now)
namespace sd {

// Read a chunk of a named file from SD (FETCH_FILE backing). Fills `out` as:
//   [file_size:u32 LE][offset:u32 LE][actual_len:u16 LE][data…]
// file_size = 0xFFFFFFFF if the card is unavailable or the file does not exist.
// Returns bytes written, or 0 on buffer underflow (out_max too small for the 10-byte header).
uint16_t read_file(const char* name, uint32_t offset, uint16_t chunk,
                   uint8_t* out, uint16_t out_max);

// Close any file left open by read_file's streaming fast-path. Call when a
// FETCH_FILE transfer ends abnormally — the host cancelled (SD_RELEASE) or the card goes to USB
// (SD_MSC). The normal end-of-file path closes itself; this prevents a leaked handle / a held card.
void close_fetch();

// Write a chunk of data to a named file on SD (WRITE_FILE backing). The file is held open across
// sequential chunks (mirror of read_file's fast-path) and synced once on close_write() — so a push
// MUST be finalised with close_write() before the file is read back or the card changes hands.
//   offset == 0: create/truncate; offset > 0: seek and write (file must exist).
// Returns bytes actually written, or 0xFFFF on card/filesystem error.
uint16_t write_file(const char* name, uint32_t offset, const uint8_t* data, uint16_t data_len);

// Flush + close any file left open by write_file's streaming fast-path. Call
// when a push ends (before a readback, on SD_RELEASE, or when the card goes to USB via SD_MSC). This
// is what makes the pushed bytes durable and visible — read_file calls it before opening its handle.
void close_write();

// Open a new LOGnnnn.MLG and begin datalogging at rate_hz. MLG v2, header and per-channel
// descriptors and all — the raw .BIN of telemetry structs this used to write is gone, and with it
// the need for anything to convert one. Claims the SdArbitrator LOGGER writer
// bit; fails (returns false) if the ECU doesn't own the card, a file transfer is in flight, or the
// filesystem can't be opened. Idempotent while already logging.
bool start_logging(uint16_t rate_hz);

// Flush, close, and release the datalog file + volume (+ the LOGGER busy bit). Safe to call anytime.
void stop_logging_public();

// SAMPLING AND WRITING ARE TWO JOBS ON TWO TASKS. See LogRing.h for why: the comms task is not a
// clock (it blocks on the RX read and wakes on USB traffic), and an SD card is not prompt (a sector
// write can stall for tens of milliseconds). Doing both in one place caps the honest sample rate at
// a jittery ~200 Hz; separating them makes 1 kHz a matter of buffer size.
//
// sample_now(): gather one record and push it into the ring. Touches NO filesystem and NO SPI, so
// it cannot block — call it from a fixed-period task. Its own telemetry frame, so it never races
// the comms snapshot.
void sample_now(Comms::CommsManager& comms, uint32_t tick_ms);
// The period the sampler should run at, in ms; 0 when not logging.
uint32_t sample_period_ms();
// Records the ring had no room for, and how full it ever got. A log that could not keep up says so.
uint32_t log_dropped();
uint32_t log_high_water();

// Drain whatever the sampler has queued onto the card (no-op unless logging is active and the ECU
// still owns the card / writes are allowed). Call periodically from comms context; self-closes on a
// key-off release.
void service_logging(uint32_t tick_ms);

bool is_logging();

// Bench override: while forced, the comms loop's rpm auto-gate leaves the logger alone. Without this a
// forced session dies on the very next comms update — the gate sees rpm 0 and closes the file — so the
// FatFS write path stays untestable on a bench with no crank signal, which is exactly what needed
// proving. Set by the 'datalog' CLI command only.
void set_forced(bool on);
bool is_forced();

}  // namespace sd
}  // namespace Comms

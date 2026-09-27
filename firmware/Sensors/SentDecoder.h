#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// SentDecoder — SAE J2716 SENT fast-channel decoder.
//
// SENT encodes data as the time between FALLING edges: each "pulse" is a nibble
// whose value is (pulse_ticks / tick_time) - 12, i.e. 12..27 ticks for value
// 0..15. A fast-channel frame is:
//
//   [ sync/calibration : 56 ticks ][ status : 1 nibble ][ data : 6 nibbles ]
//   [ CRC : 1 nibble ]  (then an optional pause)
//
// The 56-tick sync pulse calibrates the tick time for THAT frame (the sensor's
// clock drifts), so every frame is self-clocking. The fast-channel value is the
// first three data nibbles, most-significant nibble first → a 12-bit number that
// the sensor pipeline then runs through its calibration curve (like frequency).
//
// Minimal-ISR design: the capture interrupt only calls on_edge() to push the
// falling-edge timestamp into a ring (one store). ALL decoding — sync search,
// tick calibration, nibble extraction, CRC — runs in value(), called from the
// frame/task context. Header-only + hardware-free so the decode is host-testable
// with synthetic edge streams (tests/test_sent.cpp); the platform owns one
// decoder per SENT-capable pin and feeds it from platform_sent_on_edge().
//
// Single-writer (ISR on_edge) / single-reader (task value()). value() snapshots
// the ring; a frame torn by a concurrent edge simply fails to validate and the
// last good value is held — never a wrong reading.
// ---------------------------------------------------------------------------

class SentDecoder {
public:
    static constexpr uint8_t  DATA_NIBBLES = 6;                      // J2716 fast-channel data
    static constexpr uint8_t  FRAME_NIBBLES = 1 + DATA_NIBBLES + 1;  // status + data + CRC = 8
    static constexpr uint8_t  SYNC_TICKS   = 56;                     // calibration pulse length
    static constexpr uint16_t RING         = 32;                     // ≥ a few frames of edges (pow2)

    // Tick-time bounds (microseconds per tick). J2716 nominal is 3 µs; allow a wide
    // window so any in-spec sensor calibrates, while rejecting noise. A sync pulse is
    // 56 ticks, so its width in µs is 56 * tick_us.
    static constexpr uint32_t TICK_US_MIN = 1;
    static constexpr uint32_t TICK_US_MAX = 90;

    // Record a FALLING edge captured at now_us (free-running µs). ISR context — the
    // ONLY work done per edge: store the timestamp and advance the head.
    void on_edge(uint32_t now_us) {
        ring_[head_ & (RING - 1)] = now_us;
        head_   = head_ + 1u;          // wraps; single writer
        last_us_ = now_us;
    }

    // Decode the most recent complete fast-channel frame. On success sets `out` to
    // the 12-bit value and returns true; on no/partial/failed frame returns false
    // and leaves the caller to hold its last value. `enforce_crc` drops frames whose
    // CRC4 nibble doesn't match the computed checksum. `now_us` is for staleness:
    // if no edge has arrived for > stale_us, the signal is treated as stopped.
    bool value(uint32_t now_us, bool enforce_crc, uint16_t& out) const {
        const uint32_t h = head_;
        if (h < FRAME_NIBBLES + 2u) return false;       // not enough edges yet

        // Staleness: a steady SENT stream sends a frame every few ms; if nothing has
        // arrived for a while the sensor is gone. 50 ms covers slow framing + margin.
        if ((now_us - last_us_) > 50000u) return false;

        // Snapshot the most recent edges (chronological). Copy at most RING of them.
        const uint16_t n = (h < RING) ? static_cast<uint16_t>(h) : RING;
        uint32_t ts[RING];
        for (uint16_t i = 0; i < n; ++i)
            ts[i] = ring_[(h - n + i) & (RING - 1)];

        // Pulses between consecutive edges: pulse[k] = ts[k+1] - ts[k].
        const uint16_t pulses = static_cast<uint16_t>(n - 1);
        if (pulses < FRAME_NIBBLES + 1u) return false;  // need sync + 8 nibbles

        // Find the LATEST sync pulse that yields a fully-valid frame after it.
        // A frame is pulses[s] (sync) + pulses[s+1 .. s+8] (status,data,crc).
        bool crc_counted = false;   // count at most one CRC error per read (the scan tries many candidates)
        for (int s = static_cast<int>(pulses) - (FRAME_NIBBLES + 1); s >= 0; --s) {
            const uint32_t sync = ts[s + 1] - ts[s];
            // Candidate tick time from the 56-tick sync; reject if out of range.
            if (sync < SYNC_TICKS * TICK_US_MIN || sync > SYNC_TICKS * TICK_US_MAX)
                continue;
            const uint32_t tick_x16 = (sync * 16u + SYNC_TICKS / 2u) / SYNC_TICKS;  // tick µs * 16
            if (tick_x16 == 0u) continue;

            uint8_t nib[FRAME_NIBBLES];
            bool ok = true;
            for (uint8_t k = 0; k < FRAME_NIBBLES; ++k) {
                const uint32_t w = ts[s + 2 + k] - ts[s + 1 + k];   // pulse after sync
                // ticks = w / tick; value = ticks - 12. Round to nearest tick.
                const uint32_t ticks = (w * 16u + tick_x16 / 2u) / tick_x16;
                if (ticks < 12u || ticks > 27u) { ok = false; break; }
                nib[k] = static_cast<uint8_t>(ticks - 12u);
            }
            if (!ok) continue;

            // nib[0]=status, nib[1..6]=data, nib[7]=CRC. Sensors in the field do not agree on which
            // J2716 checksum they send -- the bit-wise augmented form, or the spec's table form with or
            // without the 2010 augmentation -- so a frame is accepted when its CRC nibble matches any
            // of them. That validates every sensor without a per-sensor checksum setting.
            if (enforce_crc) {
                const uint8_t rx = nib[FRAME_NIBBLES - 1];
                const uint8_t* d = &nib[1];
                if (rx != crc4(d, DATA_NIBBLES) && rx != crc4_table(d, DATA_NIBBLES, false)
                                                && rx != crc4_table(d, DATA_NIBBLES, true)) {
                    if (!crc_counted && crc_errors_ != 0xFFFFu) { crc_errors_++; crc_counted = true; }
                    continue;                              // bad frame — keep scanning older
                }
            }

            // Fast-channel value: first 3 data nibbles, MSN first → 12 bits.
            out = static_cast<uint16_t>((nib[1] << 8) | (nib[2] << 4) | nib[3]);
            return true;
        }
        return false;
    }

    void reset() { head_ = 0; last_us_ = 0; }

    // J2716 fast-channel CRC4 over the data nibbles. Bit-wise (poly x^4+x+1 = 0x13),
    // seed 5, with the 2010-spec augmentation round (an extra 0 nibble). Exposed so
    // the host test can build valid frames.
    static uint8_t crc4(const uint8_t* data, uint8_t count) {
        uint8_t crc = 5;                       // J2716 seed
        for (uint8_t i = 0; i < count; ++i) crc = crc_step(crc, data[i]);
        crc = crc_step(crc, 0);                // augmentation (2010+)
        return crc & 0x0Fu;
    }

    // J2716's own table-driven CRC4, as the spec presents it: seed 5, and for each data nibble the
    // running value is looked up in the table and the nibble XORed in. The 2010 revision
    // (`augmented`) adds a final lookup, which amounts to one more round with a zero nibble; sensors
    // built to the earlier text leave it out. `data` is the 6 data nibbles (status excluded).
    static uint8_t crc4_table(const uint8_t* data, uint8_t count, bool augmented) {
        uint8_t crc = 5;                                   // J2716 seed
        for (uint8_t i = 0; i < count; ++i)
            crc = static_cast<uint8_t>(kJ2716Crc4[crc & 0x0Fu] ^ (data[i] & 0x0Fu));
        if (augmented) crc = kJ2716Crc4[crc & 0x0Fu];
        return crc & 0x0Fu;
    }

    // Cumulative count of well-formed frames rejected because the CRC matched no known variant — a
    // running diagnostic (a rising count => wiring/EMI trouble on this SENT channel). Saturates at 0xFFFF.
    [[nodiscard]] uint16_t crc_errors() const { return crc_errors_; }

private:
    // The CRC4 table printed in SAE J2716, for crc4_table.
    static constexpr uint8_t kJ2716Crc4[16] = {0, 13, 7, 10, 14, 3, 9, 4, 1, 12, 6, 11, 15, 2, 8, 5};

    static uint8_t crc_step(uint8_t crc, uint8_t nib) {
        crc ^= (nib & 0x0Fu);
        for (uint8_t b = 0; b < 4; ++b) {
            const bool top = (crc & 0x08u) != 0u;
            crc = static_cast<uint8_t>((crc << 1) & 0x0Fu);
            if (top) crc ^= 0x03u;             // x^4+x+1 → low nibble 0x3 after wrap
        }
        return crc & 0x0Fu;
    }

    volatile uint32_t ring_[RING] = {};
    volatile uint32_t head_    = 0;
    volatile uint32_t last_us_ = 0;
    mutable  uint16_t crc_errors_ = 0;   // cumulative CRC-reject count (diagnostic); see crc_errors()
};

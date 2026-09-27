#pragma once
//
// LogRing — the buffer between a logging CLOCK and a logging DEVICE.
//
// The SD datalogger used to do both jobs in one place: the comms task called service_logging(),
// which gathered a record and wrote it to the card. That works at 10 Hz and cannot work at 1 kHz,
// for two independent reasons.
//
//   THE CLOCK. The comms task is not one. It blocks up to ~5 ms in the RX read when the line is
//   idle and returns the instant a packet arrives, so its cadence is USB traffic — around 200 Hz,
//   jittery, and faster while the studio is streaming. A log sampled on that is a log whose sample
//   interval is a property of what the laptop was doing.
//
//   THE DEVICE. SD over polled SPI is blocky: sectors, FAT updates, and a card that vanishes for
//   tens of milliseconds to do an internal erase. Sampling and writing in the same breath means the
//   sample rate inherits every one of those stalls.
//
// So they are separated by this. A fixed-rate sampler pushes finished records in; the comms task
// drains whatever has accumulated whenever it next runs. Sampling stays regular across a card stall
// because the stall lands in the buffer instead of in the timebase.
//
// SINGLE PRODUCER, SINGLE CONSUMER, NO LOCK. The producer runs at task priority 2 and the consumer
// at 1, so a mutex here would be a priority inversion waiting for a card. head_ is written only by
// the producer and tail_ only by the consumer; each reads the other's with acquire/release, which is
// all the ordering an SPSC ring needs and is why it is safe for the sampler to never block.
//
// A FULL RING DROPS THE NEW RECORD AND COUNTS IT. The alternatives are worse: blocking would put a
// card stall into the sample clock, which is the thing this exists to prevent, and overwriting the
// oldest would silently punch a hole in the middle of a file that still looks complete. Dropped
// records are reported (dropped()), so a log that could not keep up says so instead of lying.
//
#include <atomic>
#include <cstdint>
#include <cstring>

namespace mlg {

class LogRing {
public:
    void attach(uint8_t* storage, uint32_t capacity) {
        buf_ = storage; cap_ = capacity;
        reset();
    }
    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        dropped_ = 0;
        high_water_ = 0;
    }

    uint32_t capacity() const { return cap_; }

    // PRODUCER SIDE. All of `n` or none of it — a half-written record is not a record, and a reader
    // that found one would resynchronise by luck.
    bool push(const uint8_t* p, uint16_t n) {
        if (!buf_ || n == 0) return false;
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        // One byte is left unused so head==tail means EMPTY and never FULL — the alternative is a
        // separate count that both sides have to agree about.
        const uint32_t free_bytes = (tail + cap_ - head - 1u) % cap_;
        if (free_bytes < n) { ++dropped_; return false; }

        const uint32_t first = (head + n <= cap_) ? n : (cap_ - head);
        std::memcpy(buf_ + head, p, first);
        if (first < n) std::memcpy(buf_, p + first, n - first);
        head_.store((head + n) % cap_, std::memory_order_release);

        const uint32_t used_now = (head + n + cap_ - tail) % cap_;
        if (used_now > high_water_) high_water_ = used_now;
        return true;
    }

    // CONSUMER SIDE. Takes up to `max` bytes, and deliberately does NOT stop at a record boundary:
    // the consumer is writing to a file where records are already back to back, so a block is a
    // block. Returns 0 when empty.
    uint16_t pop(uint8_t* out, uint16_t max) {
        if (!buf_ || max == 0) return 0;
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t avail = (head + cap_ - tail) % cap_;
        if (avail == 0) return 0;
        if (avail > max) avail = max;

        const uint32_t first = (tail + avail <= cap_) ? avail : (cap_ - tail);
        std::memcpy(out, buf_ + tail, first);
        if (first < avail) std::memcpy(out + first, buf_, avail - first);
        tail_.store((tail + avail) % cap_, std::memory_order_release);
        return static_cast<uint16_t>(avail);
    }

    uint32_t used() const {
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        return (head + cap_ - tail) % cap_;
    }
    // How full it ever got. The number that says whether the buffer is the right size — a log that
    // never exceeded a quarter of it was never close, and one that touched the top was lucky.
    uint32_t high_water() const { return high_water_; }
    uint32_t dropped()    const { return dropped_; }

private:
    uint8_t*  buf_ = nullptr;
    uint32_t  cap_ = 0;
    std::atomic<uint32_t> head_{0};    // producer writes, consumer reads
    std::atomic<uint32_t> tail_{0};    // consumer writes, producer reads
    uint32_t  dropped_ = 0;            // producer-only
    uint32_t  high_water_ = 0;         // producer-only
};

}  // namespace mlg

// The buffer between the logging clock and the card.
//
// A lock-free SPSC ring is exactly the thing to prove off-target: on the ECU the producer is a
// 1 kHz task and the consumer is the comms task behind an SD card, so the interleavings that
// matter are precisely the ones you cannot arrange on purpose. Here they can be.
//
//   cmake --build build --target test_log_ring && ./build/test_log_ring
#include "Comms/LogRing.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    // --- all of a record or none of it -------------------------------------------------------
    {
        uint8_t storage[64];
        mlg::LogRing r;
        r.attach(storage, sizeof storage);
        ck(r.used() == 0, "a fresh ring is empty");

        uint8_t rec[10];
        for (int i = 0; i < 10; ++i) rec[i] = uint8_t(i);
        ck(r.push(rec, 10), "a record fits");
        ck(r.used() == 10, "…and is accounted for", std::to_string(r.used()));

        uint8_t out[32] = {};
        ck(r.pop(out, sizeof out) == 10, "…and comes back whole");
        ck(std::memcmp(out, rec, 10) == 0, "…byte for byte");
        ck(r.used() == 0 && r.pop(out, sizeof out) == 0, "…leaving it empty");
    }

    // --- the wrap, which is where a ring is either right or subtly wrong ----------------------
    {
        uint8_t storage[16];
        mlg::LogRing r;
        r.attach(storage, sizeof storage);
        uint8_t a[6] = {1,2,3,4,5,6}, out[16] = {};
        // Walk the head most of the way round, then push a record that straddles the end.
        for (int i = 0; i < 3; ++i) { r.push(a, 6); r.pop(out, 6); }
        ck(r.push(a, 6), "a record that straddles the end is accepted");
        std::memset(out, 0, sizeof out);
        ck(r.pop(out, sizeof out) == 6, "…and reads back at full length");
        ck(std::memcmp(out, a, 6) == 0, "…in the right order across the seam");
    }

    // --- full drops the NEW record, never a written one ---------------------------------------
    {
        uint8_t storage[16];
        mlg::LogRing r;
        r.attach(storage, sizeof storage);
        uint8_t a[10] = {9,9,9,9,9,9,9,9,9,9};
        ck(r.push(a, 10), "the first record fits");
        ck(!r.push(a, 10), "the second does not fit and is refused");
        ck(r.dropped() == 1, "…and is counted as dropped", std::to_string(r.dropped()));
        uint8_t out[16] = {};
        ck(r.pop(out, sizeof out) == 10, "the record already in it is untouched");
        ck(std::memcmp(out, a, 10) == 0, "…and intact — a drop never corrupts what is stored");
    }

    // --- capacity is honest: one byte is reserved so empty != full ----------------------------
    {
        uint8_t storage[8];
        mlg::LogRing r;
        r.attach(storage, sizeof storage);
        uint8_t a[7] = {};
        ck(r.push(a, 7), "capacity-1 bytes fit");
        ck(r.used() == 7, "…and read as used", std::to_string(r.used()));
        ck(!r.push(a, 1), "…with nothing left over");
    }

    // --- the real shape: a fast producer, a consumer that stalls like a card ------------------
    // 100k records through a ring smaller than the traffic, with the consumer sleeping the way an
    // SD card does. Every byte that made it in must come out in order, and nothing may tear.
    {
        constexpr uint32_t kCap = 512;
        static uint8_t storage[kCap];
        mlg::LogRing r;
        r.attach(storage, kCap);

        constexpr int  kRecords = 100000;
        constexpr int  kLen     = 20;
        std::atomic<bool> done{false};
        std::atomic<int>  pushed{0};

        std::thread producer([&] {
            uint8_t rec[kLen];
            for (int i = 0; i < kRecords; ++i) {
                // Each record carries its own sequence number, so the consumer can prove ORDER and
                // not merely that bytes arrived.
                std::memcpy(rec, &i, sizeof i);
                for (int b = int(sizeof i); b < kLen; ++b) rec[b] = uint8_t(i + b);
                if (r.push(rec, kLen)) pushed.fetch_add(1, std::memory_order_relaxed);
            }
            done.store(true, std::memory_order_release);
        });

        int      got = 0, lastSeq = -1;
        bool     ordered = true, intact = true;
        std::vector<uint8_t> carry;
        uint8_t  block[128];
        while (!done.load(std::memory_order_acquire) || r.used() > 0) {
            const uint16_t n = r.pop(block, sizeof block);
            if (n == 0) { std::this_thread::yield(); continue; }
            carry.insert(carry.end(), block, block + n);
            while (carry.size() >= kLen) {
                int seq = 0;
                std::memcpy(&seq, carry.data(), sizeof seq);
                if (seq <= lastSeq) ordered = false;      // records must arrive in the order written
                lastSeq = seq;
                for (int b = int(sizeof seq); b < kLen; ++b)
                    if (carry[b] != uint8_t(seq + b)) intact = false;   // …and never torn
                carry.erase(carry.begin(), carry.begin() + kLen);
                ++got;
            }
        }
        producer.join();

        ck(got == pushed.load(), "every accepted record comes out",
           std::to_string(got) + " of " + std::to_string(pushed.load()));
        ck(ordered, "…in the order it went in");
        ck(intact,  "…with no torn record");
        ck(carry.empty(), "…and no partial left over", std::to_string(carry.size()));
        // The point of the exercise: a ring smaller than the traffic DROPS rather than corrupts.
        ck(pushed.load() + int(r.dropped()) == kRecords, "accepted + dropped accounts for all of them",
           std::to_string(pushed.load()) + " + " + std::to_string(r.dropped()));
        std::printf("        (ring %u B: %d accepted, %u dropped, high water %u)\n",
                    kCap, pushed.load(), r.dropped(), r.high_water());
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All log ring tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

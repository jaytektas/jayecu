// ChunkReader — see the header for why a transfer does not belong in the paint loop.

#include "ChunkReader.h"
#include "OmniFrame.h"

#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>

#include <chrono>

namespace {

// How long to wait for one chunk's reply before resending it, and how many times. Matches what
// EcuLink allows the same request on the main-thread path — this is the same ECU and the same
// failure (a dropped frame), so it gets the same patience rather than a second opinion.
constexpr int kReplyTimeoutMs = 2000;
constexpr int kRetries        = 2;

// The ECU's config request body: [offset:u32 LE][size:u16 LE].
std::vector<uint8_t> cfgBody(int offset, int size) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(offset) >> (8 * i)));
    omni::appendLE16(b, static_cast<uint16_t>(size));
    return b;
}

}  // namespace

ChunkReader::~ChunkReader() {
    // Whatever owns this is going away, so a completion still queued for the main thread must not
    // call back into it. Mark dead BEFORE joining: the posted lambda checks this flag.
    alive_->store(false);
    cancel();
}

bool ChunkReader::start(jf::JSerialPort& port, char code, int total, int blockSize,
                        uint16_t firstSeq, Progress onProgress, Done onDone) {
    if (running_.load() || total <= 0 || blockSize <= 0) return false;
    if (!port.claim()) {
        JLOGC("comms.cfg", jf::JLogLevel::Warn)
            << "chunk read not started: the port's read stream is already claimed";
        return false;
    }
    if (thread_.joinable()) thread_.join();   // reap the previous run before starting another
    progress_ = std::move(onProgress);
    done_     = std::move(onDone);
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this, &port, code, total, blockSize, firstSeq] {
        run(&port, code, total, blockSize, firstSeq);
    });
    return true;
}

void ChunkReader::cancel() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void ChunkReader::run(jf::JSerialPort* port, char code, int total, int blockSize, uint16_t firstSeq) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(total));
    std::vector<uint8_t> rx;
    std::string error;
    uint16_t seq = firstSeq;

    // Progress is posted, and every post wakes the main thread — so a post per chunk would hand the
    // UI back the 140 wake-ups this class exists to avoid. Twenty is enough to move a progress bar.
    const int progressEvery = total / 20 + 1;
    int lastReported = 0;

    while (out.size() < static_cast<size_t>(total) && !stop_.load()) {
        const int offset    = static_cast<int>(out.size());
        const int chunk     = std::min(blockSize, total - offset);
        bool     got        = false;

        for (int attempt = 0; attempt <= kRetries && !got && !stop_.load(); ++attempt) {
            const uint16_t thisSeq = seq++;
            if (!port->write(omni::buildFrame(code, cfgBody(offset, chunk), thisSeq))) {
                error = "the link went away mid-transfer";
                break;
            }
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(kReplyTimeoutMs);
            while (!got && !stop_.load()) {
                // Parse first: the previous read may already have delivered a whole frame.
                if (auto f = omni::takeFrame(rx)) {
                    // A reply to a request we have already given up on is not this chunk's answer.
                    // Correlating rather than taking the next frame is what keeps a late duplicate
                    // from being spliced into the image at the wrong offset.
                    if (f->seq != thisSeq) continue;
                    if (f->typeId != omni::RSP_CONFIG) {
                        error = "the ECU refused a config read at offset " + std::to_string(offset);
                        break;
                    }
                    out.insert(out.end(), f->payload.begin(), f->payload.end());
                    got = true;
                    break;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;                    // resend this chunk
                const int waitMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
                auto more = port->readClaimed(waitMs);
                if (more.empty() && !port->isClaimed()) {       // the port closed under us
                    error = "the link closed mid-transfer";
                    break;
                }
                rx.insert(rx.end(), more.begin(), more.end());
            }
            if (!error.empty()) break;
        }
        if (!error.empty()) break;
        if (!got) {
            if (stop_.load()) break;
            error = "the ECU stopped answering at offset " + std::to_string(offset)
                  + " of " + std::to_string(total);
            break;
        }
        if (static_cast<int>(out.size()) - lastReported >= progressEvery) {
            lastReported = static_cast<int>(out.size());
            if (progress_) {
                auto alive = alive_;
                const int doneBytes = lastReported;
                jf::JMainThreadDispatcher::instance().post([this, alive, doneBytes, total] {
                    if (alive->load() && progress_) progress_(doneBytes, total);
                });
            }
        }
    }

    port->release();
    running_.store(false);

    if (stop_.load()) return;          // cancelled: the caller asked, so it is not told again
    if (error.empty() && out.size() != static_cast<size_t>(total))
        error = "short read: " + std::to_string(out.size()) + " of " + std::to_string(total) + " B";

    auto alive = alive_;
    jf::JMainThreadDispatcher::instance().post(
        [this, alive, data = std::move(out), error = std::move(error)]() mutable {
            if (alive->load() && done_) done_(std::move(data), std::move(error));
        });
}

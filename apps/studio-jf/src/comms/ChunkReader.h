#pragma once

// ChunkReader — a whole chunked read, driven on ITS OWN thread at the link's pace.
//
// WHY THIS EXISTS. The ECU answers one frame at a time, so reading its 140 KB tune image is 140
// sequential request/reply exchanges. Run through EcuLink they are stepped by the main loop: the
// reply cannot be SEEN until the main thread comes round again, so the transfer advances one chunk
// per iteration and its throughput becomes the frame rate. On a GPU that is a fifth of a second and
// invisible. On a software rasteriser — a VM with no Vulkan — the same read takes tens of seconds,
// and every one of those frames is redundant because nothing on screen changes between chunks.
//
// So the exchange runs where it belongs: on a thread that does nothing but write a request and wait
// for its reply. The UI is not in the loop at all. It is told when the transfer finishes, and
// occasionally how far along it is, and that is all.
//
// WHAT IT OWNS AND WHAT IT DOES NOT. For the duration it claims the port's read stream
// (JSerialPort::claim), which is exclusive — so the caller must make sure nothing else is mid
// exchange before starting one, which for a one-frame-in-flight protocol means stopping the poll.
// It touches no EcuLink state, no cache and no widget: everything it produces arrives on the main
// thread through JMainThreadDispatcher. The wire format is omni::, the same parser EcuLink uses.

#include <j/io/SerialPort.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class ChunkReader {
public:
    // Both are delivered on the MAIN thread. `error` empty means the read completed.
    using Progress = std::function<void(int bytesDone, int bytesTotal)>;
    using Done     = std::function<void(std::vector<uint8_t> data, std::string error)>;

    ChunkReader() : alive_(std::make_shared<std::atomic<bool>>(true)) {}
    ~ChunkReader();

    ChunkReader(const ChunkReader&)            = delete;
    ChunkReader& operator=(const ChunkReader&) = delete;

    // Begin reading `total` bytes from offset 0 in `blockSize` chunks, using command `code`.
    // False if a read is already running or the port cannot be claimed. `firstSeq` is where this
    // transfer's correlation ids start; the caller keeps its own counter clear of them.
    bool start(jf::JSerialPort& port, char code, int total, int blockSize, uint16_t firstSeq,
               Progress onProgress, Done onDone);

    // Ask the worker to stop. Returns once it has: the port is released and no further callback
    // will fire. Safe to call when nothing is running.
    void cancel();

    bool running() const { return running_.load(); }

private:
    void run(jf::JSerialPort* port, char code, int total, int blockSize, uint16_t firstSeq);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    Progress          progress_;
    Done              done_;
    // Lets a completion already posted to the main thread find out that this reader (and whatever
    // owns it) has gone away, instead of calling into freed memory.
    std::shared_ptr<std::atomic<bool>> alive_;
};

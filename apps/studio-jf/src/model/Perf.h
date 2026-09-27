#pragma once

// Perf — scope timing for "why is this sluggish", reported as a periodic summary rather than a per-frame
// firehose. OFF unless JF_LOG names the "perf" category, and when off a scope costs one atomic-free bool
// test and nothing else: no clock read, no map lookup.
//
// Two things are measured, because "slow" is usually one of them:
//   * TIME, per named scope, summed over the reporting window (PERF_SCOPE).
//   * COUNT, for work whose cost is in how OFTEN it happens rather than how long one of them takes —
//     expression evaluations, cache reads, widget rebuilds (PERF_COUNT).
//
// The summary prints per FRAME (total / frames), which is the number that matters when the question is
// whether the UI can hold 60Hz, plus the worst single frame seen in the window.

#include <j/core/Log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class Perf {
public:
    static Perf& instance() { static Perf p; return p; }

    // Enabled once, from the environment the logger already reads, so turning it on is
    //     JF_LOG=perf=info ./studio
    static bool enabled() {
        static const bool on = [] {
            const char* e = std::getenv("JF_LOG");
            return e && std::strstr(e, "perf") != nullptr;
        }();
        return on;
    }

    struct Entry { double totalMs = 0.0, worstMs = 0.0; long long calls = 0; };

    void add(const char* name, double ms) {
        Entry& e = scopes_[name];
        e.totalMs += ms;
        e.worstMs = std::max(e.worstMs, ms);
        ++e.calls;
    }
    void count(const char* name, long long n = 1) { counts_[name] += n; }

    // Called once per presented frame. Reports every `windowFrames` frames OR every couple of seconds,
    // whichever comes first: the app is event-driven and only paints when something happens, so a
    // frame-count window alone can sit there for a minute without ever printing.
    void frame(double frameMs) {
        if (frames_ == 0) windowStart_ = Clock::now();
        ++frames_;
        frameTotalMs_ += frameMs;
        frameWorstMs_ = std::max(frameWorstMs_, frameMs);
        const double elapsed = std::chrono::duration<double>(Clock::now() - windowStart_).count();
        if (frames_ < windowFrames_ && elapsed < 2.0) return;
        report();
        reset();
    }

    // Whatever has been gathered, printed now — so a short session still says something.
    ~Perf() { if (frames_ > 0) report(); }

    void reset() {
        scopes_.clear(); counts_.clear();
        frames_ = 0; frameTotalMs_ = 0.0; frameWorstMs_ = 0.0;
    }

    void report() const {
        if (frames_ == 0) return;
        const double f = double(frames_);
        JLOGC("perf", jf::JLogLevel::Info)
            << "---- " << frames_ << " frames: " << (frameTotalMs_ / f) << " ms/frame avg, worst "
            << frameWorstMs_ << " ms (" << (frameTotalMs_ > 0.0 ? 1000.0 * f / frameTotalMs_ : 0.0) << " fps)";
        // Slowest first: the top line is where the time went.
        std::vector<std::pair<std::string, Entry>> rows(scopes_.begin(), scopes_.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });
        for (const auto& [name, e] : rows)
            JLOGC("perf", jf::JLogLevel::Info)
                << "  " << name << "  " << (e.totalMs / f) << " ms/frame   worst " << e.worstMs
                << " ms   " << (double(e.calls) / f) << " calls/frame";
        std::vector<std::pair<std::string, long long>> crows(counts_.begin(), counts_.end());
        std::sort(crows.begin(), crows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [name, n] : crows)
            JLOGC("perf", jf::JLogLevel::Info) << "  # " << name << "  " << (double(n) / f) << " /frame";
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point windowStart_{};
    std::map<std::string, Entry>     scopes_;
    std::map<std::string, long long> counts_;
    long long frames_ = 0;
    double    frameTotalMs_ = 0.0, frameWorstMs_ = 0.0;
    static constexpr long long windowFrames_ = 120;
};

// One scope's wall time, added on destruction. Constructing it when profiling is off does nothing.
class PerfScope {
public:
    explicit PerfScope(const char* name)
        : name_(name), on_(Perf::enabled()), t0_(on_ ? Clock::now() : Clock::time_point{}) {}
    ~PerfScope() {
        if (!on_) return;
        Perf::instance().add(name_, std::chrono::duration<double, std::milli>(Clock::now() - t0_).count());
    }
private:
    using Clock = std::chrono::steady_clock;
    const char* name_;
    bool        on_;
    Clock::time_point t0_;
};

#define PERF_SCOPE(name) PerfScope _perf_scope_##__LINE__(name)
#define PERF_COUNT(name) do { if (Perf::enabled()) Perf::instance().count(name); } while (0)
#define PERF_COUNT_N(name, n) do { if (Perf::enabled()) Perf::instance().count(name, (n)); } while (0)

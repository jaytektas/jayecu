#pragma once

// EngineCyclePanel — the Engine Cycle tab: a rolling RECORDER of engine cycles with transport.
//
// The capture itself is one cycle (see firmware/Scheduler/CycleRecorder), and a single frame answers
// "what did it just do". It cannot answer the question a tuner actually has, which is "what CHANGED"
// — a spark that wanders a degree per cycle, an injector that occasionally misses, a tooth that
// arrives late once every few revolutions. Those only exist as a difference between frames, so the
// panel keeps a ring of them and lets you stop and walk back through it.
//
// Live vs paused is the whole model:
//
//   RECORDING — every completed capture appends and the view follows the newest frame. When the ring
//     is full the oldest is dropped, so you always hold the last N cycles rather than the first N.
//     Requesting the next capture is driven by the ARRIVAL of the previous one, not a timer: the ECU
//     records exactly one cycle per request, so this self-paces to whatever the engine and link can
//     actually deliver instead of queueing requests the ECU has not answered.
//
//   PAUSED — nothing is requested, the ring stops changing, and the cursor is yours. This is the
//     state you are in while reading a frame, and it must be genuinely still: a view that kept
//     updating underneath you would make comparing two cycles impossible.
//
// Stepping or scrubbing implies pause. Nothing is more annoying than dragging to frame 3 and being
// yanked back to the newest, so any deliberate navigation stops the recording rather than fighting it.

#include "EngineCycleView.h"
#include "../model/CycleDiagnostics.h"

#include <j/core/JButton.h>
#include <j/core/JContainer.h>
#include <j/core/JLabel.h>
#include <j/core/JScrollBar.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>

class EngineCyclePanel : public jf::JContainer {
public:
    explicit EngineCyclePanel(jf::JSceneGraph& g);
    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

    // Asked for the next capture whenever one is wanted. The panel never talks to a link itself —
    // the same panel serves a native jayecu capture and a converted rusEFI one, and neither belongs
    // in a widget.
    std::function<void()> onCaptureRequest;


    // A completed cycle. Appends, drops the oldest if the ring is full, and follows it when live.
    void addFrame(const enginecycle::Cycle& c);

    // Begin/continue recording (fires onCaptureRequest), or stop.
    void startRecording();
    void pause();
    [[nodiscard]] bool recording() const { return recording_; }

    // Ring size, from preferences. Shrinking drops the OLDEST frames, keeping the newest, because
    // the recent ones are the ones being looked at.
    void setCapacity(size_t n);
    [[nodiscard]] size_t capacity() const { return capacity_; }

    // Status text for an empty view ("Capturing…", "Not connected", a failure reason).
    void setStatus(const std::string& s) { view_->setStatus(s); }
    // Shown alongside the frames — for saying the panel has stopped while its data stays on screen.
    void setNotice(const std::string& s) { view_->setNotice(s); }
    void clearFrames();

    // Navigation, exposed because the transport buttons are not the only caller worth having: a test
    // can drive these deterministically, which clicking cannot.
    void stepBy(int delta) { step(delta); }        // relative; implies pause
    void stepTo(size_t idx) { pause(); showFrame(idx); }

    // The diagnosis of the frame currently on screen — exposed so a test can assert what the user is
    // being told, which reading a rendered label cannot.
    [[nodiscard]] const enginecycle::Diagnosis& diagnosis() const { return lastDiagnosis_; }

    [[nodiscard]] size_t frameCount() const { return frames_.size(); }
    [[nodiscard]] size_t cursor() const { return cursor_; }
    // The rpm of the frame currently on screen, or 0 when there is none — a cheap, unambiguous way
    // to ask "which frame am I looking at" without reaching into the view.
    [[nodiscard]] double displayedRpm() const {
        return frames_.empty() ? 0.0 : frames_[cursor_].rpm();
    }

private:
    void showFrame(size_t idx);      // clamp, display, refresh the transport
    void refreshTransport();         // labels, counter, scrollbar position + thumb size
    void step(int delta);            // relative move; implies pause

    std::deque<enginecycle::Cycle> frames_;
    size_t cursor_    = 0;
    size_t capacity_  = 60;
    bool   recording_ = false;
    enginecycle::Diagnosis lastDiagnosis_;   // of the frame on screen; drives the readout

    jf::JContainer*  row_      = nullptr;
    jf::JButton*     playBtn_  = nullptr;
    jf::JButton*     firstBtn_ = nullptr;
    jf::JButton*     prevBtn_  = nullptr;
    jf::JButton*     nextBtn_  = nullptr;
    jf::JButton*     lastBtn_  = nullptr;
    jf::JButton*     clearBtn_ = nullptr;
    jf::JScrollBar*  scrub_    = nullptr;
    jf::JLabel*      counter_  = nullptr;
    jf::JLabel*      diag_     = nullptr;
    EngineCycleView* view_     = nullptr;

    bool suppressScrub_ = false;     // guard: moving the bar programmatically must not re-enter step()
};

#pragma once

// TriggerLogPanel — the trigger log view plus its transport.
//
// The transport is a Start/Stop, not a capture button: the log STREAMS while the engine is cranked
// and the user stops when they have seen enough. A fixed-size capture is meaningless here — 4096
// records is twenty minutes of a one-trigger-per-revolution engine at cranking speed and 3.4 seconds
// of a 180-slot wheel armed Both.
//
// Like EngineCyclePanel it never talks to a link. Start/stop are requests the owner fulfils. A capture
// changes nothing on the ECU — it copies the edges before the decoder sees them — so it is safe on a
// running engine.

#include <j/core/JContainer.h>
#include <j/core/JButton.h>
#include <j/core/JLabel.h>
#include <j/core/JScrollBar.h>
#include <j/core/JSpinBox.h>

#include "TriggerLogView.h"

#include <functional>
#include <memory>
#include <string>

class TriggerLogPanel : public jf::JContainer {
public:
    explicit TriggerLogPanel(jf::JSceneGraph& g);

    std::function<void()> onStart;   // arm and begin draining (the decoder is unaffected)
    std::function<void()> onStop;    // stop the capture

    // A fresh view of everything captured so far. Called on every poll while running.
    // Raised when the user clears, so the owner can drop the records it is holding — the panel
    // cannot, they are not its.
    // Called when the ECU reports the snapshot full and everything has been read out.
    void setComplete();

    void setRecords(const std::vector<triggerlog::Record>& recs,
                    const std::vector<std::string>& streamNames);

    void setStatus(const std::string& s) { view_->setStatus(s); }

    // Running state, driven by the owner — the panel does not know whether the ECU accepted.
    void setRunning(bool on);
    [[nodiscard]] bool running() const { return running_; }

    // Exposed so a test can drive them deterministically, which clicking cannot.
    void scrollTo(size_t firstBar);
    void setZoom(int barsOnScreen);
    [[nodiscard]] size_t barCount() const { return view_->barCount(); }

private:
    void refreshTransport();

    jf::JContainer*  row_    = nullptr;
    TriggerLogView*  view_   = nullptr;
    jf::JButton*     runBtn_ = nullptr;
    jf::JScrollBar*  scrub_  = nullptr;
    jf::JLabel*      counter_ = nullptr;
    jf::JLabel*      zoomLbl_ = nullptr;
    jf::JSpinBox*    zoom_    = nullptr;

    bool   running_    = false;
    bool   complete_   = false;     // the snapshot filled and was read out — not 'live' any more
    bool   follow_     = true;    // stay at the live end until the user scrolls back
    bool   suppress_   = false;
    size_t first_      = 0;
};

#include "EngineCyclePanel.h"
#include "WrapText.h"

#include "../model/CycleDiagnostics.h"

#include <algorithm>

using namespace jf;

void EngineCyclePanel::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    if (wraptext::fit(diag_)) invalidate();   // the findings line is as tall as its wrapped text
    JContainer::populateRenderPrimitives(buf);
}

EngineCyclePanel::EngineCyclePanel(JSceneGraph& g) : JContainer(g) {
    setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::Column)
        ->setGap(4.f)->setPadding(JEdges{ 4.f, 4.f, 4.f, 4.f })
        ->setAlignItems(JAlignItems::Stretch);

    // The PLOT is added first and the transport row second, so the column puts the controls at the
    // BOTTOM. That is where a transport belongs: the lanes are what you are reading, and a control
    // strip above them pushes the data down and sits between the tab and its own content.
    view_ = add(std::make_unique<EngineCycleView>(g));
    view_->setBounds({ 0.f, 0.f, 600.f, 400.f });
    // The PLOT claims the leftover height so the transport is pinned to the true bottom of the tab.
    // Left at the default (Preferred) both children sit at their size hint and the row ends up
    // floating in the middle with dead space beneath it.
    view_->setVSizePolicy(jf::JSizePolicyMode::Expanding, 1);

    // ---- transport row ----
    row_ = add(std::make_unique<JContainer>(g));   // OWNING add: the container destroys it with itself
    row_->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(4.f);
    row_->setBounds({ 0.f, 0.f, 600.f, 24.f });
    // Fixed: a transport strip has one correct height and should never absorb slack.
    row_->setVSizePolicy(jf::JSizePolicyMode::Fixed, 0);

    // add(unique_ptr) is the OWNING overload and returns the raw pointer; add(JWidget*) is the legacy
    // non-owning one and would leak every control here.
    auto mkBtn = [&](const char* label, float w) {
        return row_->add(std::make_unique<JButton>(g, label, w, 22.f));
    };
    // Glyphs a font atlas is certain to have. A transport bar rendered as "?" boxes is worse than a
    // plain one — the same trap the cycle header's open-circle fell into.
    playBtn_  = mkBtn("Start", 64.f);      // label reflects what the button DOES next
    firstBtn_ = mkBtn("|<", 32.f);
    prevBtn_  = mkBtn("<", 32.f);
    nextBtn_  = mkBtn(">", 32.f);
    lastBtn_  = mkBtn(">|", 32.f);
    // DISCARD THE SCROLLBACK. frames_ holds up to capacity_ cycles and nothing emptied it: after
    // watching a fault go past there was no way to throw the history away and start fresh, and Start
    // does not do it — Start resumes appending to what is already there. clearFrames() existed,
    // tested, and reachable from nothing.
    //
    // Placed at the far end, away from Start: the two are the buttons whose misfire costs most, one
    // losing the history and the other leaving the view following when it should be held.
    clearBtn_ = mkBtn("Clear", 56.f);

    scrub_   = row_->add(std::make_unique<JScrollBar>(g, 320.f, 18.f, 0.12f));
    counter_ = row_->add(std::make_unique<JLabel>(g, "0 / 0", 190.f, 20.f));

    // The frame's own account of itself: what it contains, and anything that disagrees with the
    // frame before it. A view that only draws bars cannot show a MISSING one, and a cycle served
    // with a third of its teeth draws perfectly happily — so the counts are stated in words. On a line
    // of its own under the transport, WRAPPED and as tall as the findings make it: beside the buttons it
    // was cut off at whatever was left of the row, which is where the findings are.
    diag_    = add(std::make_unique<JLabel>(g, "", 600.f, 20.f));
    diag_->setWordWrap(true);
    diag_->setVSizePolicy(jf::JSizePolicyMode::Fixed, 0);


    playBtn_->onClicked.connect([this] { if (recording_) pause(); else startRecording(); });
    firstBtn_->onClicked.connect([this] { pause(); showFrame(0); });
    prevBtn_->onClicked.connect([this] { step(-1); });
    nextBtn_->onClicked.connect([this] { step(+1); });
    lastBtn_->onClicked.connect([this] { pause(); showFrame(frames_.empty() ? 0 : frames_.size() - 1); });
    clearBtn_->onClicked.connect([this] { clearFrames(); });

    scrub_->onScrolled.connect([this](float pos) {
        if (suppressScrub_ || frames_.empty()) return;
        // Dragging is deliberate navigation, so it stops the recording rather than fighting it.
        pause();
        const size_t idx = static_cast<size_t>(pos * static_cast<float>(frames_.size() - 1) + 0.5f);
        showFrame(idx);
    });

    refreshTransport();
}

void EngineCyclePanel::setCapacity(size_t n) {
    capacity_ = std::max<size_t>(1, n);
    // Drop from the FRONT: the newest frames are the ones being looked at, so a smaller ring should
    // keep the recent history rather than the beginning of the session.
    while (frames_.size() > capacity_) frames_.pop_front();
    if (cursor_ >= frames_.size()) cursor_ = frames_.empty() ? 0 : frames_.size() - 1;
    if (!frames_.empty()) view_->setCycle(frames_[cursor_]);
    refreshTransport();
}

void EngineCyclePanel::clearFrames() {
    frames_.clear();
    cursor_ = 0;
    view_->clear();
    refreshTransport();
}

void EngineCyclePanel::addFrame(const enginecycle::Cycle& c) {
    // The SAME engine cycle, fetched again. The link outruns the engine — a capture is ~7.6 ms while
    // a cycle at 3000 rpm is 40 ms — and the recorder correctly returns the last COMPLETE cycle
    // until the crank has finished a new one, so about five reads in a row describe one cycle.
    //
    // Storing the repeats spends the ring on them: 60 slots held roughly 13 distinct cycles at 3000
    // rpm and 7 at 1500. That defeats the point of a recorder whose job is comparing one cycle
    // against the next, and it makes the frame diagnosis useless too — a duplicate pair always
    // reports "nothing changed", which is true and worthless.
    //
    // Compared against the NEWEST frame only, not the whole ring: cycle numbers advance, so a match
    // anywhere else would mean the counter wrapped, and dropping a genuinely new cycle is worse than
    // keeping a stale one. Sequence 0 means the producer does not number its frames (a bridge that
    // cannot say), and then every frame would look like a duplicate — so it disables the check
    // rather than collapsing the whole recording into one slot.
    if (!frames_.empty() && c.sequence() != 0 && c.sequence() == frames_.back().sequence()) {
        // Still ask for the next one. The capture loop is arrival-driven, so returning without
        // requesting would stop the recording dead on the first duplicate — which, at any normal
        // rpm, is immediately.
        if (recording_ && onCaptureRequest) onCaptureRequest();
        return;
    }
    frames_.push_back(c);
    if (frames_.size() > capacity_) {
        frames_.pop_front();
        if (cursor_ > 0) --cursor_;      // keep pointing at the same CYCLE, not the same index
    }
    if (recording_) {
        showFrame(frames_.size() - 1);   // follow the newest
        if (onCaptureRequest) onCaptureRequest();   // ask for the next; arrival-driven, not timed
    } else {
        refreshTransport();              // ring grew under a paused cursor: only the counter moves
    }
}

void EngineCyclePanel::startRecording() {
    if (recording_) return;
    recording_ = true;
    if (!frames_.empty()) showFrame(frames_.size() - 1);
    refreshTransport();
    if (onCaptureRequest) onCaptureRequest();
}

void EngineCyclePanel::pause() {
    if (!recording_) return;
    recording_ = false;
    refreshTransport();
}

void EngineCyclePanel::step(int delta) {
    pause();
    if (frames_.empty()) return;
    const long want = static_cast<long>(cursor_) + delta;
    showFrame(static_cast<size_t>(std::clamp<long>(want, 0, static_cast<long>(frames_.size()) - 1)));
}

void EngineCyclePanel::showFrame(size_t idx) {
    if (frames_.empty()) { view_->clear(); refreshTransport(); return; }
    cursor_ = std::min(idx, frames_.size() - 1);
    view_->setCycle(frames_[cursor_]);
    // Compare against the PREVIOUS frame in the ring, which is the neighbouring engine cycle. That
    // is the only reference the studio can honestly use: it does not know the wheel or the firing
    // order, but it does know a steadily turning engine repeats its counts.
    const enginecycle::Cycle* ref = (cursor_ > 0) ? &frames_[cursor_ - 1] : nullptr;
    lastDiagnosis_ = enginecycle::diagnose(frames_[cursor_], ref);
    refreshTransport();
}

void EngineCyclePanel::refreshTransport() {
    playBtn_->setLabel(recording_ ? "Stop" : "Start");

    // Two different numbers, and conflating them would mislead: the ring position says where you are
    // in the recording, the CYCLE number says which engine cycle you are looking at. Captures are not
    // consecutive cycles, so the jump between two frames' cycle numbers is how many the engine turned
    // while the previous frame was being read out.
    char buf[96];
    if (frames_.empty()) {
        std::snprintf(buf, sizeof(buf), "0 / 0");
    } else {
        const uint32_t seq = frames_[cursor_].sequence();
        if (seq) std::snprintf(buf, sizeof(buf), "%zu / %zu  \xC2\xB7  cycle %u",
                               cursor_ + 1, frames_.size(), seq);
        else     std::snprintf(buf, sizeof(buf), "%zu / %zu  of %zu",
                               cursor_ + 1, frames_.size(), capacity_);
    }
    counter_->setText(buf);

    if (frames_.empty()) {
        diag_->setText("");
    } else {
        // Summary always; the findings appended so a suspect frame reads as suspect at a glance
        // rather than requiring the user to count bars.
        std::string line = lastDiagnosis_.summary();
        for (const auto& f : lastDiagnosis_.findings) {
            line += (f.severity == enginecycle::Severity::Error)   ? "   [!] "
                  : (f.severity == enginecycle::Severity::Warning) ? "   [~] "
                                                                   : "   \xC2\xB7 ";
            line += f.text;
        }
        diag_->setText(line);
    }

    // The thumb size is fixed at construction (JScrollBar exposes no setter), so the COUNTER carries
    // "which of how many" instead — which it has to anyway, since a thumb cannot show 47 of 60.
    if (!frames_.empty()) {
        const float pos = (frames_.size() > 1)
            ? static_cast<float>(cursor_) / static_cast<float>(frames_.size() - 1) : 0.f;
        // Guarded: setScrollPosition emits onScrolled, which would call back into step() and pause
        // the recording every time a live frame arrived.
        suppressScrub_ = true;
        scrub_->setScrollPosition(pos);
        suppressScrub_ = false;
    }
}

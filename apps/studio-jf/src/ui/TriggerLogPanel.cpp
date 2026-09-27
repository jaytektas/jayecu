#include "TriggerLogPanel.h"

#include <algorithm>
#include <cstdio>

using namespace jf;

TriggerLogPanel::TriggerLogPanel(JSceneGraph& g) : JContainer(g) {
    setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::Column)
        ->setGap(4.f)->setPadding(JEdges{ 4.f, 4.f, 4.f, 4.f })
        ->setAlignItems(JAlignItems::Stretch);

    // Plot first, transport second, so the column pins the controls to the bottom — same reasoning
    // as the cycle panel: the bars are what you are reading.
    view_ = add(std::make_unique<TriggerLogView>(g));
    view_->setBounds({ 0.f, 0.f, 600.f, 400.f });
    view_->setVSizePolicy(jf::JSizePolicyMode::Expanding, 1);

    row_ = add(std::make_unique<JContainer>(g));
    row_->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(4.f);
    row_->setBounds({ 0.f, 0.f, 600.f, 24.f });
    row_->setVSizePolicy(jf::JSizePolicyMode::Fixed, 0);

    auto mkBtn = [&](const char* label, float w) {
        return row_->add(std::make_unique<JButton>(g, label, w, 22.f));
    };
    runBtn_ = mkBtn("Start", 72.f);
    scrub_  = row_->add(std::make_unique<JScrollBar>(g, 360.f, 18.f, 0.12f));
    counter_ = row_->add(std::make_unique<JLabel>(g, "0 edges", 220.f, 20.f));
    // How many bars are on screen. A cranking session is thousands and the screen is hundreds, so
    // what is shown is a decision — and at the wrong zoom a 36-1's gap is either invisible or the
    // only thing visible.
    zoomLbl_ = row_->add(std::make_unique<JLabel>(g, "bars", 34.f, 20.f));
    zoom_    = row_->add(std::make_unique<JSpinBox>(g, 20, 2000, 80.f, 22.f));

    runBtn_->onClicked.connect([this] {
        if (running_) { if (onStop) onStop(); }
        else          { if (onStart) onStart(); }
    });
    scrub_->onScrolled.connect([this](float pos) {
        if (suppress_) return;
        const size_t n = view_->barCount();
        const size_t win = view_->windowCount();
        if (n <= win) return;
        const auto want = static_cast<size_t>(pos * static_cast<float>(n - win) + 0.5f);
        // Scrolling back is deliberate: stop chasing the live end, or the next poll would yank the
        // view away from whatever the user just went looking at.
        follow_ = (want + win >= n);
        scrollTo(want);
    });
    zoom_->onValueChanged.connect([this](int v) { if (!suppress_) setZoom(v); });

    suppress_ = true;
    zoom_->setValue(200);
    suppress_ = false;
    view_->setWindow(0, 200);
    refreshTransport();
}

void TriggerLogPanel::setRecords(const std::vector<triggerlog::Record>& recs,
                                 const std::vector<std::string>& streamNames) {
    std::vector<triggerlog::Lane> lanes;
    const auto r = triggerlog::decodeLanes(recs, streamNames, lanes);
    if (!r.ok) { view_->setStatus(r.message); refreshTransport(); return; }
    view_->setStatus("");
    view_->setLanes(std::move(lanes));
    if (follow_) {
        const size_t n = view_->barCount();
        const size_t win = view_->windowCount();
        first_ = (n > win) ? (n - win) : 0;
        view_->setWindow(first_, win);
    }
    refreshTransport();
}

void TriggerLogPanel::setComplete() {
    complete_ = true;
    running_  = false;
    runBtn_->setLabel("Start");
    // SAY SO. A buffer that has stopped filling looks exactly like a signal that went away, and the
    // difference matters: one means you have a complete capture to read, the other means you have a
    // problem. It also has to say what to do next, because there is nothing left to wait for.
    view_->setStatus("");
    view_->setNotice("buffer full \xE2\x80\x94 the capture is complete; "
                     "press Start to clear it and run again");
    refreshTransport();
}

void TriggerLogPanel::setRunning(bool on) {
    running_ = on;
    runBtn_->setLabel(on ? "Stop" : "Start");
    if (on) {
        // START IS THE CLEAR. Arming resets the ECU's buffer (TriggerLogger::arm zeroes total_) and
        // EcuLink drops its accumulation, so the only thing left holding the previous capture was
        // this view — it kept drawing it until the first page of the new one arrived. There is no
        // separate Clear button: one existed, cleared the view alone, and the next poll pushed the
        // records straight back, so it never cleared anything. Start does the whole job or nothing
        // does.
        complete_ = false;
        view_->setNotice("");
        view_->clear();
        first_  = 0;
        follow_ = true;
        view_->setWindow(0, view_->windowCount());
        // NOT "crank the engine". The log no longer touches the decoder and has no entry gate, so a
        // capture can be taken at any time — on a running engine, or while starting one. Telling the
        // user to crank was describing the old restriction, not what the tool needs.
        view_->setStatus("Capturing\xE2\x80\xA6 waiting for the first edges");
    }
    refreshTransport();
}

void TriggerLogPanel::scrollTo(size_t firstBar) {
    first_ = firstBar;
    view_->setWindow(first_, view_->windowCount());
    refreshTransport();
}

void TriggerLogPanel::setZoom(int barsOnScreen) {
    const auto win = static_cast<size_t>(std::max(1, barsOnScreen));
    const size_t n = view_->barCount();
    if (follow_ && n > win) first_ = n - win;
    view_->setWindow(first_, win);
    refreshTransport();
}

void TriggerLogPanel::refreshTransport() {
    const size_t n = view_->barCount();
    const size_t win = view_->windowCount();
    char b[128];
    if (n == 0) std::snprintf(b, sizeof(b), "0 edges");
    else        std::snprintf(b, sizeof(b), "bars %zu\xE2\x80\x93%zu of %zu%s",
                              first_ + 1, std::min(first_ + win, n), n,
                              complete_ ? "  \xC2\xB7  full" : (follow_ ? "  \xC2\xB7  live" : ""));
    counter_->setText(b);

    if (n > win) {
        suppress_ = true;
        scrub_->setScrollPosition(static_cast<float>(first_) / static_cast<float>(n - win));
        suppress_ = false;
    }
}

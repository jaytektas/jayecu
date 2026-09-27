#include "AutotunePanel.h"
#include "WrapText.h"

#include "../model/Cache.h"
#include "../model/MetaModel.h"

#include <j/config/Settings.h>
#include <j/core/JStyle.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace jf;

namespace {

uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A channel's value, or a stated default when the definition names a channel this ECU does not
// publish. Distinguishing "reads zero" from "there is no such channel" matters most for the ego
// multipliers: a missing one must be 1.000 (no correction) and never 0, which would propose that
// every cell be zeroed.
double chanOr(const Cache& c, const std::string& name, double dflt) {
    return (!name.empty() && c.has(name)) ? c.value(name) : dflt;
}

}  // namespace

AutotunePanel::AutotunePanel(JSceneGraph& g) : JContainer(g) {
    setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::Column)
        ->setGap(4.f)->setPadding(JEdges{ 4.f, 4.f, 4.f, 4.f })
        ->setAlignItems(JAlignItems::Stretch);

    buildControls(g);

    // THE THRESHOLDS ARE THE TUNER'S. A definition states sensible ones, but the figure that makes a
    // rig learn nothing is engine-specific — a bench with no coolant sensor never clears "Minimum CLT",
    // and a car on a dyno wants a different minimum throttle than one on the road. The reference
    // implementation marks each filter "user adjustable" for exactly this reason.
    filterRow_ = add(std::make_unique<JContainer>(g));
    filterRow_->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(6.f);
    filterRow_->setBounds({ 0.f, 0.f, 640.f, 26.f });
    filterRow_->setVSizePolicy(JSizePolicyMode::Fixed, 0);

    view_ = add(std::make_unique<AutotuneView>(g));
    view_->setBounds({ 0.f, 0.f, 640.f, 380.f });
    view_->setVSizePolicy(JSizePolicyMode::Expanding, 1);
    view_->setEngine(&eng_);

    // The accountability line, last so it sits under the grid it is accounting for.
    // WRAPPED, and as tall as its lines (populateRenderPrimitives fits it to the width it is given).
    stats_ = add(std::make_unique<JLabel>(g, "", 640.f, 20.f));
    stats_->setWordWrap(true);
    stats_->setVSizePolicy(JSizePolicyMode::Fixed, 0);

    refresh();
}

void AutotunePanel::buildControls(JSceneGraph& g) {
    auto* row = add(std::make_unique<JContainer>(g));
    row->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(6.f);
    row->setBounds({ 0.f, 0.f, 640.f, 26.f });
    row->setVSizePolicy(JSizePolicyMode::Fixed, 0);

    startBtn_ = row->add(std::make_unique<JButton>(g, "Start Auto Tune", 128.f, 22.f));
    applyBtn_ = row->add(std::make_unique<JButton>(g, "Apply", 70.f, 22.f));
    resetBtn_ = row->add(std::make_unique<JButton>(g, "Reset", 70.f, 22.f));
    // Offered only where the app knows how to do it (a native ECU with declared trims).
    openLoopBtn_ = row->add(std::make_unique<JButton>(g, "Open Loop", 110.f, 22.f));
    openLoopBtn_->setVisible(false);

    // CONTINUOUS APPLY is opt-in and separate from Start, because they are different decisions: one is
    // "watch and tell me", the other is "and change the tune as you go". The reference implementation
    // calls it Update Controller and defaults it OFF for the same reason.
    liveBox_ = row->add(std::make_unique<JCheckBox>(g, "Apply continuously", 150.f, 22.f));

    // THE GRID SHOWS ONE THING AT A TIME, so the control that decides which has to say so. Unlabelled
    // it reads as a setting rather than a view selector, and "where is the proposed VE table" is then
    // a question with no answer on screen — the proposal is right there, under a dropdown nothing
    // identified. (Asked within a minute of the panel first being looked at.)
    row->add(std::make_unique<JLabel>(g, "  Show", 44.f, 22.f));
    modeBox_ = row->add(std::make_unique<JComboBox>(
        g, std::vector<std::string>{ "Change %", "Proposed VE", "Base VE", "Records" }, 120.f, 22.f));
    modeBox_->setCurrentIndex(0);
    row->add(std::make_unique<JLabel>(g, "Resistance", 68.f, 22.f));
    resistBox_ = row->add(std::make_unique<JComboBox>(
        g, std::vector<std::string>{ "Easy", "Normal", "Hard" }, 90.f, 22.f));
    resistBox_->setCurrentIndex(1);

    // THE STATUS IS A SENTENCE ("No ego correction: this ECU does not publish ..."), so it has a line
    // of its own under the buttons and WRAPS there. Squeezed in beside them it was cut off mid-word.
    status_ = add(std::make_unique<JLabel>(g, "", 640.f, 20.f));
    status_->setWordWrap(true);
    status_->setVSizePolicy(JSizePolicyMode::Fixed, 0);

    startBtn_->onClicked.connect([this] { if (recording_) stop(); else start(); });
    applyBtn_->onClicked.connect([this] { applyProposal(); });
    resetBtn_->onClicked.connect([this] { eng_.clear(); refresh(); });
    // Open when the trims are on, restore when they are off — the button says which, and does that.
    openLoopBtn_->onClicked.connect([this] { if (onSetOpenLoop) onSetOpenLoop(trimActive_); });
    liveBox_->onStateChanged.connect([this](bool on) { live_ = on; refresh(); });
    modeBox_->onIndexChanged.connect([this](int i) {
        view_->setMode(i == 1 ? AutotuneView::Mode::Proposed
                     : i == 2 ? AutotuneView::Mode::Base
                     : i == 3 ? AutotuneView::Mode::Weight
                              : AutotuneView::Mode::Change);
    });
    resistBox_->onIndexChanged.connect([this](int) { applySettings(); refresh(); });
}

void AutotunePanel::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    // The two wrapped lines take the height their text needs at the width they were laid out to.
    if (wraptext::fit(status_) | wraptext::fit(stats_)) invalidate();
    JContainer::populateRenderPrimitives(buf);
}

std::string AutotunePanel::settingKey(const std::string& channel) {
    return "autotune.filter." + channel;
}

void AutotunePanel::buildFilters(JSceneGraph& g, const std::vector<autotune::Filter>& fs) {
    if (!filterRow_) return;
    filterRow_->clear();
    filterSpins_.clear();
    maxPct_ = maxAbs_ = nullptr;
    if (fs.empty()) return;

    auto& st = JSettings::instance();
    filterRow_->add(std::make_unique<JLabel>(g, "Reject when", 74.f, 22.f));
    for (const autotune::Filter& f : fs) {
        // THE COMPARISON IS ON SCREEN, not implied by the name. "Minimum RPM 500" and "dTPS 50" are
        // opposite tests, and a bare number beside a name gives no way to tell which way one runs.
        filterRow_->add(std::make_unique<JLabel>(
            g, f.name + (f.op == autotune::Filter::Op::Above ? " >" : " <"),
            static_cast<float>(f.name.size()) * 7.f + 16.f, 22.f));
        auto* sp = filterRow_->add(std::make_unique<JDoubleSpinBox>(g, -10000.0, 30000.0, 1.0, 1, 76.f, 22.f));
        sp->setValue(st.get<double>(settingKey(f.channel), f.value));
        sp->onValueChanged.connect([this](double) { applySettings(); refresh(); });
        filterSpins_.push_back(sp);
    }
    filterRow_->add(std::make_unique<JLabel>(g, "    Max change %", 100.f, 22.f));
    maxPct_ = filterRow_->add(std::make_unique<JDoubleSpinBox>(g, 0.0, 100.0, 1.0, 1, 70.f, 22.f));
    maxPct_->setValue(st.get<double>("autotune.maxCellPct", eng_.settings().maxCellPct));
    maxPct_->onValueChanged.connect([this](double) { applySettings(); refresh(); });
    filterRow_->add(std::make_unique<JLabel>(g, "Max change", 72.f, 22.f));
    maxAbs_ = filterRow_->add(std::make_unique<JDoubleSpinBox>(g, 0.0, 1000.0, 1.0, 1, 70.f, 22.f));
    maxAbs_->setValue(st.get<double>("autotune.maxCellAbs", eng_.settings().maxCellAbs));
    maxAbs_->onValueChanged.connect([this](double) { applySettings(); refresh(); });
}

void AutotunePanel::applySettings() {
    autotune::Settings s = eng_.settings();
    const int i = resistBox_ ? resistBox_->currentIndex() : 1;
    s.resistance = i == 0 ? autotune::Resistance::Easy
                 : i == 2 ? autotune::Resistance::Hard
                          : autotune::Resistance::Normal;
    if (maxPct_) s.maxCellPct = maxPct_->value();
    if (maxAbs_) s.maxCellAbs = maxAbs_->value();
    eng_.setSettings(s);

    // A threshold edited mid-run applies to what is ALREADY accumulated, because every proposal is
    // re-derived from the raw weights each time it is asked for — so tightening a limit takes the
    // change back rather than leaving cells that were let through under the old one.
    auto& st = JSettings::instance();
    std::vector<autotune::Filter> fs = eng_.filters();
    for (size_t k = 0; k < fs.size() && k < filterSpins_.size(); ++k) {
        fs[k].value = filterSpins_[k]->value();
        st.set(settingKey(fs[k].channel), JVariant(fs[k].value));
    }
    eng_.setFilters(std::move(fs));
    if (maxPct_) st.set("autotune.maxCellPct", JVariant(s.maxCellPct));
    if (maxAbs_) st.set("autotune.maxCellAbs", JVariant(s.maxCellAbs));
}

// ------------------------------------------------------------------------------------------------
// Setup: everything here comes from the loaded definition, so the panel names no channel of its own.
// ------------------------------------------------------------------------------------------------
void AutotunePanel::attach() {
    Cache& c = Cache::instance();
    const MetaModel* m = c.meta();
    tablePath_.clear(); targetTable_.clear(); targetChannel_.clear(); lambdaChannel_.clear();
    xChannel_.clear(); yChannel_.clear(); egoChannels_.clear();
    eng_.setFilters({});

    if (!m) { view_->setNotice("No definition loaded"); refresh(); return; }
    const MetaModel::Autotune& a = m->autotune();
    if (!a.valid()) {
        // Said plainly rather than shown as an empty grid: this ECU's definition does not describe an
        // autotune, and no amount of driving will change that.
        view_->setNotice("This definition does not declare an autotune contract");
        refresh();
        return;
    }

    tablePath_     = a.table;
    targetTable_   = a.targetTable;
    targetChannel_ = a.targetChannel;
    lambdaChannel_ = a.lambdaChannel;
    egoChannels_   = a.egoChannels;

    const TableImage t = c.resolveTable(tablePath_);
    if (!t.valid || t.axes.size() < 2) {
        view_->setNotice("The table this definition names does not resolve: " + tablePath_);
        refresh();
        return;
    }
    // The axes name their own channels — the same question the table widget's live cursor asks, asked
    // through the same function, so the grid the autotuner fills is indexed by what the map is indexed by.
    xChannel_ = c.axisChannel(t, 0);
    yChannel_ = c.axisChannel(t, 1);
    view_->setNotice({});
    view_->setAxisLabels(xChannel_.empty() ? t.axes[0].label : xChannel_,
                         yChannel_.empty() ? t.axes[1].label : yChannel_);

    seedGrid();

    std::vector<autotune::Filter> fs;
    for (const auto& f : a.filters)
        fs.push_back({ f.name, f.channel,
                       f.above ? autotune::Filter::Op::Above : autotune::Filter::Op::Below,
                       f.value, true });
    eng_.setFilters(fs);
    buildFilters(m_graph, fs);   // one spin per declared filter, seeded from what was last used

    // THE ECU'S OWN DELAY MAP when it has one. The firmware already carries a measured transport-delay
    // surface for its own learning, and the studio using a second, different one would credit readings
    // to different cells than the ECU does — two answers to one physical question.
    autotune::DelayTable d;
    if (!a.delayTable.empty()) {
        const TableImage dt = c.resolveTable(a.delayTable);
        if (dt.valid && dt.axes.size() >= 2) {
            d.xb = c.tiBins(dt, 0);
            d.yb = c.tiBins(dt, 1);
            const int nx = c.tiLiveN(dt, 0), ny = c.tiLiveN(dt, 1);
            d.cells.reserve(static_cast<size_t>(nx) * ny);
            for (int r = 0; r < ny; ++r)
                for (int cc = 0; cc < nx; ++cc) d.cells.push_back(c.tiCell(dt, cc, r, 0));
            d.xb.resize(static_cast<size_t>(nx));
            d.yb.resize(static_cast<size_t>(ny));
        }
    }
    eng_.setDelay(d.valid() ? d : autotune::defaultDelay());
    applySettings();
    refresh();
}

// THE GRID IS THE TABLE'S, READ BACK. Compared value for value rather than by bin COUNT alone: an
// axis rescaled in place (2000 rpm moved to 2500) keeps its count and changes every cell's meaning,
// which is the version of this that a count check would wave through.
bool AutotunePanel::gridMatchesTable() const {
    if (tablePath_.empty()) return false;
    const TableImage t = Cache::instance().resolveTable(tablePath_);
    if (!t.valid || t.axes.size() < 2) return false;
    return Cache::instance().tiBins(t, 0) == eng_.xBins()
        && Cache::instance().tiBins(t, 1) == eng_.yBins();
}

void AutotunePanel::seedGrid() {
    Cache& c = Cache::instance();
    const TableImage t = c.resolveTable(tablePath_);
    if (!t.valid || t.axes.size() < 2) return;
    eng_.setGrid(c.tiBins(t, 0), c.tiBins(t, 1));   // discards everything accumulated — see Engine::setGrid
    readBase();
}

// The plane the engine is on right now: nearest breakpoint of the third axis to its channel. A table
// with no live third axis is one plane, and that is plane 0 — not a special case, just n == 1.
int AutotunePanel::livePlane() const {
    if (tablePath_.empty()) return 0;
    const Cache& c = Cache::instance();
    const TableImage t = c.resolveTable(tablePath_);
    if (!t.valid || t.axes.size() < 3) return 0;
    const int n = c.tiLiveN(t, 2);
    if (n <= 1) return 0;
    // ASKED OF THE TABLE, NOT REMEMBERED. Caching this at attach meant a third axis switched ON later
    // was never noticed — enabling an axis reaches no signal — and the panel went on tuning plane 0 of
    // a map that had become a stack.
    const std::string z = c.axisChannel(t, 2);
    if (z.empty() || !c.has(z)) return 0;
    const std::vector<double> zb = c.tiBins(t, 2);
    const double v = c.value(z);
    int best = 0; double bd = 1e300;
    for (int i = 0; i < n && i < static_cast<int>(zb.size()); ++i) {
        const double d = std::fabs(zb[i] - v);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void AutotunePanel::readBase() {
    Cache& c = Cache::instance();
    const TableImage t = c.resolveTable(tablePath_);
    if (!t.valid) return;
    const int nx = c.tiLiveN(t, 0), ny = c.tiLiveN(t, 1);
    std::vector<double> cells;
    cells.reserve(static_cast<size_t>(nx) * ny);
    for (int r = 0; r < ny; ++r)
        for (int cc = 0; cc < nx; ++cc) cells.push_back(c.tiCell(t, cc, r, plane_));
    eng_.setBase(std::move(cells));
}

// ------------------------------------------------------------------------------------------------
void AutotunePanel::start() {
    if (tablePath_.empty()) return;
    gridNote_.clear();
    // THE BINS TOO, not just the cells: tiWriteBins announces nothing, so an axis edited through it
    // reaches no signal and the tab would start a run against a grid that has already moved.
    plane_ = livePlane();          // this run is about the plane the engine is on
    seedGrid();
    eng_.clear();
    recording_ = true;
    lastApplyMs_ = nowMs();
    startBtn_->setLabel("Stop");
    refresh();
}

void AutotunePanel::stop() {
    recording_ = false;
    startBtn_->setLabel("Start Auto Tune");
    refresh();
}

void AutotunePanel::onTableEdited() {
    if (tablePath_.empty()) return;
    if (gridMatchesTable() && livePlane() == plane_) {   // cells only — still the same map
        readBase();
        refresh();
        return;
    }
    // THE AXES MOVED. Every record was credited to a cell index, and those indices now mean somewhere
    // else, so the proposal goes with them. Stopping as well as clearing is deliberate: a run that
    // silently restarted from nothing would look like one that had been watching the whole time.
    const bool had = eng_.hasProposal() || recording_;
    if (recording_) stop();
    seedGrid();
    if (had) gridNote_ = "Table axes changed - proposal discarded";
    refresh();
}

void AutotunePanel::onFrame() {
    if (!recording_ || tablePath_.empty()) return;
    // THE ENGINE CHANGED FUEL. A stacked map holds one plane per ethanol content, and a proposal is
    // about the plane it was measured on — carrying it across would correct E85 cells with E10
    // evidence. Ending the run is the honest answer; the tuner starts another on the new plane.
    if (livePlane() != plane_) {
        stop();
        plane_ = livePlane();
        seedGrid();
        gridNote_ = "Fuel plane changed - proposal discarded";
        refresh();
        return;
    }
    const Cache& c = Cache::instance();

    autotune::Sample s;
    s.ms = nowMs();
    s.x  = chanOr(c, xChannel_, 0.0);
    s.y  = chanOr(c, yChannel_, 0.0);
    s.lambda = chanOr(c, lambdaChannel_, 0.0);
    s.ok     = !lambdaChannel_.empty() && c.has(lambdaChannel_) && s.lambda > 0.0;

    // The target, from whichever the definition names: a live channel is the ECU's own commanded
    // figure and needs nothing solved; a target MAP is solved at this instant's operating point,
    // which is the same answer the ECU would have looked up.
    s.target = !targetChannel_.empty() && c.has(targetChannel_) ? c.value(targetChannel_)
             : !targetTable_.empty()                            ? c.solveTable(targetTable_).v
                                                                : 1.0;

    // THE CORRECTION THE ECU WAS ALREADY APPLYING, as a multiplier — converted from whatever shape the
    // definition says the channel is published in. Reading rusEFI's 100-based Gego as though it were a
    // multiplier would not shade the proposal, it would scale it by a hundred.
    //
    // An absent channel contributes nothing (1.000) rather than zero, and is REPORTED: a definition
    // that names an ego channel this ECU does not publish means the fold silently stops happening, and
    // a wrong VE table held on target by closed loop then looks perfect. That is the one failure this
    // whole mechanism exists to prevent, so it must not be silent. See egoMissing_.
    s.ego = 1.0;
    egoMissing_.clear();
    for (const MetaModel::AutotuneEgo& e : egoChannels_) {
        if (e.channel.empty()) continue;
        if (!c.has(e.channel)) { egoMissing_.push_back(e.channel); continue; }
        s.ego *= e.asMultiplier(c.value(e.channel));
    }

    s.filt.reserve(eng_.filters().size());
    for (const autotune::Filter& f : eng_.filters()) s.filt.push_back(chanOr(c, f.channel, 0.0));

    eng_.add(s);

    // Continuous apply, paced rather than per-frame: writing every telemetry frame would be a config
    // write per frame, and the point is to converge, not to stream.
    if (live_ && eng_.hasProposal() && s.ms - lastApplyMs_ > 2000u) applyProposal();

    refresh();
}

void AutotunePanel::applyProposal() {
    if (tablePath_.empty() || !eng_.hasProposal()) return;
    // LAST GATE BEFORE THE MAP IS WRITTEN. onTableEdited should have caught this already; an edit that
    // reaches no signal (tiWriteBins emits nothing) would not have. Writing a proposal through a
    // mapping that no longer holds is the one failure here with no symptom and no undo trail worth
    // reading — the same reason Apply to Base Table checks the grid it is about to walk.
    if (!gridMatchesTable() || livePlane() != plane_) {
        if (recording_) stop();
        plane_ = livePlane();
        seedGrid();
        gridNote_ = "Table axes changed - proposal discarded, nothing written";
        refresh();
        return;
    }
    Cache& c = Cache::instance();
    const TableImage t = c.resolveTable(tablePath_);
    if (!t.valid) return;
    const int nx = std::min(c.tiLiveN(t, 0), eng_.cols());
    const int ny = std::min(c.tiLiveN(t, 1), eng_.rows());

    // ONE undo step. A proposal is a single decision even though it moves a hundred cells, and
    // undoing it a cell at a time would be unusable.
    c.beginEdit();
    for (int r = 0; r < ny; ++r)
        for (int cc = 0; cc < nx; ++cc) {
            const double pct = eng_.changePct(cc, r);
            if (pct == 0.0) continue;                       // untouched cells are left exactly alone
            c.tiSetCell(t, cc, r, plane_, c.tiCell(t, cc, r, plane_) * (1.0 + pct / 100.0));
        }
    c.endEdit("Auto Tune");

    // THE EVIDENCE WAS MEASURED AGAINST THE OLD CELLS. Keeping it would apply the same correction a
    // second time on the next Apply, and a third on the one after — so it is spent, and the run
    // continues from the map as it now stands.
    lastApplyMs_ = nowMs();
    readBase();
    eng_.clear();
    refresh();
}

// ------------------------------------------------------------------------------------------------
void AutotunePanel::setLinkNotice(const std::string& s) { linkNotice_ = s; refresh(); }
void AutotunePanel::setTrimState(bool anyTrimActive, bool canRestore) {
    trimActive_ = anyTrimActive;
    canRestore_ = canRestore;
    if (openLoopBtn_) {
        // Shown while there is something to say: trims on (offer to leave closed loop) or trims this
        // panel switched off (offer to put them back). Trims someone else switched off leave nothing
        // to offer — restoring a state the panel never saw would be inventing one.
        openLoopBtn_->setVisible(static_cast<bool>(onSetOpenLoop) && (anyTrimActive || canRestore));
        openLoopBtn_->setLabel(anyTrimActive ? "Open Loop" : "Restore Trims");
    }
    refresh();
}

std::string AutotunePanel::statusText() const {
    if (!gridNote_.empty())   return gridNote_;
    if (!linkNotice_.empty()) return linkNotice_;
    if (tablePath_.empty())   return "No autotune contract";
    // AHEAD OF THE COUNTERS, because this one invalidates them. A missing ego channel does not stop the
    // run — it makes the run agree with a table that is wrong — so it has to be said while there is
    // still time not to trust the result.
    if (!egoMissing_.empty()) {
        std::string s = "No ego correction: this ECU does not publish ";
        for (size_t i = 0; i < egoMissing_.size(); ++i) { if (i) s += ", "; s += egoMissing_[i]; }
        return s + " - a table held on target by closed loop will read as correct";
    }
    const autotune::Stats st = eng_.stats();
    if (!recording_) return st.used ? "Stopped - " + std::to_string(st.altered) + " cell(s) proposed"
                                    : "Ready - tuning " + tablePath_;
    // WHY NOTHING IS HAPPENING, in the words of whatever is stopping it. "Active Filter" is the first
    // thing a tuner looks for when the counters are not moving.
    if (!st.activeFilter.empty()) return "Filtered: " + st.activeFilter;
    if (trimActive_) return "Learning (ECU trims active - folded in)";
    return canRestore_ ? "Learning (open loop - trims held for Restore)" : "Learning";
}

std::string AutotunePanel::statsText() const {
    const autotune::Stats st = eng_.stats();
    char b[320];
    std::snprintf(b, sizeof b,
                  "Records %ld total / %ld filtered / %ld used     Cells %d, altered %d     "
                  "Change avg %.1f %%, max %.1f %%     Weight avg %.1f",
                  st.total, st.filtered, st.used, st.cells, st.altered,
                  st.avgChange, st.maxChange, st.avgWeight);
    return b;
}

void AutotunePanel::refresh() {
    if (status_) status_->setText(statusText());
    if (stats_)  stats_->setText(statsText());
    if (applyBtn_) applyBtn_->setEnabled(eng_.hasProposal());
    if (view_) view_->invalidate();
}

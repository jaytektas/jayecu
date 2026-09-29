#pragma once

// AutotunePanel — the VE autotuner: watch the engine, propose a better fuel table, apply it.
//
// WHAT IT IS FOR. Tuning a VE table by hand means driving to a cell, reading the wideband, working out
// the percentage, typing it in, and repeating that a few hundred times. The arithmetic is mechanical
// and the driving is not, so the machine should do the first while the person does the second. This
// watches the same channels the tuner would have read and keeps the answer for every cell at once.
//
// WHAT IT IS NOT. It does not touch the tune until somebody presses Apply. Everything up to that point
// is a PROPOSAL sitting beside the map, and the panel's job is to make it checkable: what changed, by
// how much, on what evidence, and what it threw away. An autotuner that quietly rewrites a fuel table
// is asking to be trusted about an engine it cannot hear.
//
// MORE THAN ONE TABLE. The VE table is learned from a wideband ratio; a table whose answer a channel
// MEASURES directly — Predicted MAP, the pressure a steady throttle and speed settle to — is learned from
// that channel (MetaModel::ValueAutotune). The "Tune" selector picks which; both are the same record,
// review and Apply flow, and the value kind proposes the cell's new value rather than a percentage.
//
// TWO SOURCES, ONE VIEW. Which channels to watch comes from the loaded definition (MetaModel::autotune)
// — our schema declares it, a TunerStudio ini declares the same facts in [VeAnalyze] — so a rusEFI ECU
// and a native one are the same code path with different names in it. See MetaModel::Autotune.
//
// THE ECU'S OWN TRIMS. A native ECU corrects fuelling itself, and a correction it is already applying
// is not visible in lambda: the mixture is on target and the table is still wrong. The declared `ego`
// channels are folded back in so the proposal is the table's own error either way — but the honest way
// to measure is with the trims OFF, which is what "Open Loop" does, and what the panel says when they
// are not. (Firmware guarantees that switching a trim off ZEROES it rather than freezing it, which is
// the only reason that button means anything.)

#include "AutotuneView.h"

#include "../model/Autotune.h"
#include "../model/MetaModel.h"

#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JContainer.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JLabel.h>

#include <functional>
#include <string>
#include <vector>

class AutotunePanel : public jf::JContainer {
public:
    explicit AutotunePanel(jf::JSceneGraph& g);
    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;

    // (Re)read the loaded definition and the table it names: grid, base cells, filters, delay. Called
    // when the tab opens and whenever the definition or the tune is reloaded underneath it.
    void attach();

    // One telemetry frame arrived. Builds a record from the declared channels and offers it to the
    // engine — but only while recording, because sampling an engine nobody asked to tune is how a
    // proposal appears that the tuner did not start.
    void onFrame();
    // The table changed underneath us — an edit on another page, an undo, a restore. The proposal is
    // expressed against the cells as they were READ, so a stale base draws a Base/Proposed grid that
    // disagrees with the map it is about. (Found by pressing Undo after an Apply and watching the grid
    // go on showing the applied values.)
    void onTableEdited();

    // Does the engine's grid still describe the table? Everything the panel holds is addressed by CELL
    // INDEX, so a breakpoint that moved or a bin that was inserted makes every accumulated record
    // describe a place that no longer exists — and an Apply would write the proposal into cells it was
    // never measured in, silently, across the whole map.
    [[nodiscard]] bool gridMatchesTable() const;

    // WHICH PLANE OF THE MAP THE ENGINE IS ON. A fuel table can be a stack — VE's third axis is
    // ethanol — and the autotuner was reading and writing plane 0 whatever the engine was burning. On
    // a flex tune above E0 that is measuring one fuel and correcting another. The plane is resolved
    // from the axis's own channel, the same way the rows and columns are.
    [[nodiscard]] int livePlane() const;

    void start();
    void stop();
    [[nodiscard]] bool recording() const { return recording_; }

    // Write the proposal into the tune. One undo step, and the accumulated evidence is discarded
    // afterwards: it was measured against the OLD cells, and keeping it would apply the same
    // correction a second time on the next Apply.
    void applyProposal();

    // Asked to take the ECU out of closed loop for tuning, or to put it back the way it was (native
    // only — the panel does not know how, and the app does). Left unset, the button is not offered.
    //
    // A TOGGLE, not a one-shot. The first version fired once and then hid itself, because with the
    // trims off there was "nothing to do" — so the one gesture the panel offered vanished the instant
    // it was used, with no confirmation that it had worked and no way back. Leaving closed loop and
    // returning to it are the same decision seen from two sides.
    std::function<void(bool open)> onSetOpenLoop;
    // What the panel reports about the link and the ECU's trims, since neither is its to inspect.
    // `canRestore` is whether the app remembers what the trims WERE — a Restore offered without that
    // would be inventing the state it puts back.
    void setLinkNotice(const std::string& s);
    void setTrimState(bool anyTrimActive, bool canRestore = false);

    [[nodiscard]] const autotune::Engine& engine() const { return eng_; }
    // The status line, exposed so a test can read what the user is being told rather than a rendered
    // pixel. Same string, one source.
    [[nodiscard]] std::string statusText() const;
    [[nodiscard]] std::string statsText()  const;
    [[nodiscard]] const std::string& tablePath() const { return tablePath_; }

    void refresh();          // repaint the stats/status lines from the engine

private:
    void buildControls(jf::JSceneGraph& g);
    // The filter row is built from the DEFINITION, so it cannot exist until one is loaded — rebuilt on
    // every attach, one spin box per declared filter, in the order the definition lists them.
    void buildFilters(jf::JSceneGraph& g, const std::vector<autotune::Filter>& fs);
    // Where a filter's threshold is remembered between sessions. Keyed on the CHANNEL, not the display
    // name: the name is prose a definition may reword, the channel is what the filter actually reads.
    std::string settingKey(const std::string& channel) const;
    std::string settingPrefix() const;   // "autotune." for VE, "autotune.<table>." for a value target
    void selectTarget(int i);            // the Tune selector: set the engine up for target i
    void readBase();         // pull the table's live cells into the engine (values only)
    void seedGrid();         // …and its BINS, which discards whatever was accumulated against the old ones
    void applySettings();    // combo/entry state -> engine settings

    autotune::Engine eng_;
    AutotuneView*    view_ = nullptr;

    jf::JButton*   startBtn_ = nullptr;
    jf::JButton*   applyBtn_ = nullptr;
    jf::JButton*   resetBtn_ = nullptr;
    jf::JButton*   openLoopBtn_ = nullptr;
    jf::JComboBox* modeBox_ = nullptr;
    jf::JComboBox* targetBox_ = nullptr;
    jf::JComboBox* resistBox_ = nullptr;
    jf::JCheckBox* liveBox_ = nullptr;
    jf::JLabel*    status_ = nullptr;
    jf::JLabel*    stats_  = nullptr;
    jf::JContainer*                  filterRow_ = nullptr;
    std::vector<jf::JDoubleSpinBox*> filterSpins_;      // parallel to the engine's filters
    jf::JDoubleSpinBox*              maxPct_ = nullptr;
    jf::JDoubleSpinBox*              maxAbs_ = nullptr;

    // The contract, resolved from the definition once per attach: the table, the channels each axis
    // traces, and the channels every record is built from.
    std::string tablePath_, targetTable_, targetChannel_, lambdaChannel_;
    std::string xChannel_, yChannel_;
    std::vector<MetaModel::AutotuneEgo> egoChannels_;
    // The targets this definition offers, in selector order: the VE contract first when it has one, then
    // each value target. `valueTarget_` is the selected one when it is a value target (isValue_).
    std::vector<std::string>                targetNames_;
    std::vector<MetaModel::ValueAutotune>   valueTargets_;
    bool                                    hasVe_   = false;
    int                                     target_  = 0;
    bool                                    isValue_ = false;
    MetaModel::ValueAutotune                valueTarget_;
    bool                                    selecting_ = false;   // selector repopulating: ignore its signal

    int         plane_ = 0;  // the plane this run is about; a change discards the proposal
    std::string linkNotice_;
    // Ego channels the definition names and this ECU does not publish. Not a diagnostic detail: with no
    // ego fold, closed loop holds the mixture on target and a wrong VE table reads as a right one, so
    // the run would quietly propose nothing and look like a clean tune. Said out loud in statusText().
    std::vector<std::string> egoMissing_;
    std::string gridNote_;   // "the axes moved and your proposal went with them", until the next Start
    bool recording_   = false;
    bool trimActive_  = false;
    bool canRestore_  = false;   // the app is holding the trim settings this panel switched off
    bool live_        = false;    // apply continuously while recording
    uint32_t lastApplyMs_ = 0;
};

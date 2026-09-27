#pragma once
//
// OnboardLoggingDialog — the ECU's own logger, edited as PROFILES rather than as one live setting.
//
// It began as "Choose Channels", which named the tree and nothing else, and then grew the gate — but
// it still had exactly one setup in it: the car's. So the only way to work out what a profile
// contained was to install it, and the only way to change one was to install it, edit the car, and
// save it back. A tuner sitting at a desk with no ECU in front of them could do none of it.
//
// So the dialog is an EDITOR over a list of setups, and the car is one entry in that list:
//
//   Current (on the ECU)   what the config image says the car is set to — the answer to "what is
//                          this thing actually doing", read from the cache, not from studio settings
//   <firmware templates>   the definition's own sets, read-only; they name channels and nothing else
//   <your profiles>        saved setups, edited and kept entirely without a link
//
// Choosing one SHOWS it. Nothing is written anywhere by looking, and nothing reaches the car except
// through Activate — which is the only button here that touches it, and even then it lands in RAM
// like every other edit, because the burn is still deliberate.
//
//   Activate   write what is on screen to the ECU (mask, gate, timing, rate)
//   Save       write it back to the profile it came from (refused for the car and for a template)
//   Duplicate… save it under a new name — which is also how a template becomes something editable
//   Delete     remove a profile
//
// A PROFILE IS THE WHOLE SETUP, not a channel list: the gate, its timing, the rate and the mask
// travel together because they are parts of one decision. "Log these channels, while this is true,
// no shorter than that" is one thought, and a saved half of it still has to be finished by hand.
//
// The window, the chrome, the drag, the focus ring, the key routing and the wheel are
// jf::JDialogWindow's — written once in the framework precisely so a dialog like this is only the
// dialog. `layout` places, `paint` draws behind the widgets, close() ends it.
//
#include <j/app/JDialogWindow.h>
#include <j/core/JStyle.h>
#include <j/core/JLineEdit.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JTreeView.h>
#include <j/core/JTextHelper.h>
#include <j/core/Dialog.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/Cache.h"
#include "../model/ExprCompiler.h"
#include "../model/MetaModel.h"
#include "../model/DatalogRecorder.h"
#include "../surface/Surface.h"          // Surface::onEditExpression — the full expression editor
#include "WrapText.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class OnboardLoggingDialog : public jf::JDialogWindow {
public:
    static constexpr uint32_t kW = 760, kH = 800;
    static constexpr float kPad = 14.f, kLabelW = 92.f, kFxW = 30.f;
    static constexpr float kSetBtnW = 78.f, kNumW = 78.f;

    OnboardLoggingDialog(jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : jf::JDialogWindow("Onboard Logging", kW, kH, hal, sx, sy, parent) {

        // ---- the list of setups, the car first --------------------------------------------
        m_sets = std::make_unique<jf::JComboBox>(graph(), std::vector<std::string>{}, 240.f);
        m_sets->onIndexChanged.connect([this](int i) { _selected(i); });
        m_activate = std::make_unique<jf::JButton>(graph(), "Activate", kSetBtnW + 8.f, rowH());
        m_save     = std::make_unique<jf::JButton>(graph(), "Save",     kSetBtnW, rowH());
        m_copy     = std::make_unique<jf::JButton>(graph(), "Duplicate\xE2\x80\xA6", kSetBtnW, rowH());
        m_delete   = std::make_unique<jf::JButton>(graph(), "Delete",   kSetBtnW, rowH());
        m_activate->onClicked.connect([this] { _activate(); });
        m_save->onClicked.connect    ([this] { _save();     });
        m_copy->onClicked.connect    ([this] { _copy();     });
        m_delete->onClicked.connect  ([this] { _delete();   });

        // ---- the gate ----------------------------------------------------------------------
        m_when  = _makeExpr(m_edit.logWhen,  m_whenErr,  "rpm > 4000");
        m_until = _makeExpr(m_edit.logUntil, m_untilErr, "rpm < 3000");
        m_whenFx  = std::make_unique<jf::JButton>(graph(), "fx", kFxW, rowH());
        m_untilFx = std::make_unique<jf::JButton>(graph(), "fx", kFxW, rowH());
        m_whenFx->onClicked.connect ([this] { _editExpr("datalog.log_when",  m_when.get()); });
        m_untilFx->onClicked.connect([this] { _editExpr("datalog.log_until", m_until.get()); });

        // ---- the gate's timing, and the rate ------------------------------------------------
        m_rate   = _makeNum(m_edit.rateHz,   1, 1000);
        m_minOn  = _makeNum(m_edit.minOnMs,  0, 65535);
        m_minOff = _makeNum(m_edit.minOffMs, 0, 65535);
        m_maxOn  = _makeNum(m_edit.maxOnMs,  0, 65535);
        m_rearm  = _makeNum(m_edit.rearmMs,  0, 65535);
        m_onInvalid = std::make_unique<jf::JComboBox>(
            graph(), std::vector<std::string>{ "Off", "On", "Hold" }, 90.f);
        m_onInvalid->onIndexChanged.connect([this](int i) {
            if (m_loading) return;
            m_edit.onInvalid = i; _touch();
        });
        m_enabled = std::make_unique<jf::JCheckBox>(graph(), "SD logging enabled", 190.f);
        m_enabled->onCheckStateChanged.connect([this](jf::JCheckBox::CheckState s) {
            if (m_loading) return;
            m_edit.enabled = (s == jf::JCheckBox::Checked) ? 1 : 0; _touch();
        });

        // ---- the channels -------------------------------------------------------------------
        m_filter = std::make_unique<jf::JLineEdit>(graph(), "Filter channels\xE2\x80\xA6");
        // The ✕ every other search box in the studio has. Opt-in on JLineEdit, and this is a query you
        // discard rather than a value you keep — which is exactly what that flag is for.
        m_filter->setClearButtonEnabled(true);
        m_filter->onTextChanged.connect([this](const std::string& t) { m_query = t; _rebuild(); });

        m_tree = std::make_unique<jf::JTreeView>(graph());
        // A click on a channel toggles it. On a CATEGORY it toggles the whole group, which is what
        // makes "everything about lambda, nothing else" two clicks instead of thirty.
        // The BOX toggles; the label still selects. Two gestures, so the tree stays browsable while
        // it is also a chooser.
        m_tree->onNodeChecked.connect([this](jf::JTreeViewNode* n) { _activateNode(n); });
        m_tree->onNodeActivated.connect([this](jf::JTreeViewNode* n) { _activateNode(n); });

        m_all  = std::make_unique<jf::JButton>(graph(), "All shown",  90.f, rowH());
        m_none = std::make_unique<jf::JButton>(graph(), "None shown", 90.f, rowH());
        // "Shown", not "all": with a filter in the box these act on what the filter found, which is
        // the only reading that lets a filter be useful for anything but scrolling.
        m_all->onClicked.connect ([this] { _setVisible(true);  });
        m_none->onClicked.connect([this] { _setVisible(false); });

        m_box = std::make_unique<jf::JDialogButtonBox>(graph());
        m_box->addButton("Close", jf::JDialogButtonBox::Role::Reject, 100.f);
        m_box->onReject.connect([this] { _closeGuarded(); });

        add(m_sets.get()); add(m_activate.get()); add(m_save.get());
        add(m_copy.get()); add(m_delete.get());
        add(m_when.get());  add(m_whenFx.get());
        add(m_until.get()); add(m_untilFx.get());
        add(m_rate.get()); add(m_minOn.get()); add(m_minOff.get());
        add(m_maxOn.get()); add(m_rearm.get());
        add(m_onInvalid.get()); add(m_enabled.get());
        add(m_filter.get()); add(m_all.get()); add(m_none.get());
        add(m_tree.get());   add(m_box.get());

        _reloadSets();
        _selected(0);                    // opens on the car, which is what a tuner came to look at
    }

protected:
    void layout(float w, float h) override {
        const float r = rowH();
        // EVERY LINE OF TEXT HERE WRAPS, and the layout makes room for the lines it wraps to: the
        // "what this is" line, the gate's line and the cost lines at the foot were each one line that
        // ran off the right edge when the sentence was longer than the dialog.
        const float tw = w - 2.f * kPad;
        m_ySets  = contentTop();
        m_yWhen  = m_ySets + r + 26.f + wraptext::extra(_topLine(), tw - kLabelW);   // room for the "what this is" line
        m_yUntil = m_yWhen + r + 6.f;
        // The gate's explanatory line sits between Stop When and the numbers, and it is a FULL line
        // of text. 26 px left it sharing pixels with the number captions, which draw UPWARDS from
        // m_yNums — so the room needed is that line plus the caption above the boxes, not one gap.
        m_yNums  = m_yUntil + r + 48.f + wraptext::extra(_gateLine(nullptr), tw);
        m_yFlags = m_yNums + r + 24.f;                 // room for the number labels above the boxes
        m_yFilt  = m_yFlags + r + 14.f;
        const float treeY = m_yFilt + r + 10.f;
        std::string cost, note;
        _costLines(cost, note);
        const float costX = wraptext::extra(cost, tw), noteX = wraptext::extra(note, tw);
        m_yCost = h - 2.f * r - kPad - costX;
        m_yNote = m_yCost - r + 4.f - noteX;
        const float treeH = h - treeY - (2.f * r + 3.f * kPad) - costX - noteX;

        const float bw = kSetBtnW + 6.f;
        const float bx = w - kPad - (3.f * bw + kSetBtnW + 8.f);
        m_sets->setBounds({ kPad + kLabelW, m_ySets, bx - kPad - kLabelW - 10.f, r });
        m_activate->setBounds({ bx,                          m_ySets, kSetBtnW + 8.f, r });
        m_save    ->setBounds({ bx + kSetBtnW + 14.f,        m_ySets, kSetBtnW, r });
        m_copy    ->setBounds({ bx + kSetBtnW + 14.f + bw,   m_ySets, kSetBtnW, r });
        m_delete  ->setBounds({ bx + kSetBtnW + 14.f + 2*bw, m_ySets, kSetBtnW, r });

        const float ew = w - kPad - kLabelW - kPad - kFxW - 6.f;
        m_when ->setBounds({ kPad + kLabelW, m_yWhen,  ew, r });
        m_until->setBounds({ kPad + kLabelW, m_yUntil, ew, r });
        m_whenFx ->setBounds({ w - kPad - kFxW, m_yWhen,  kFxW, r });
        m_untilFx->setBounds({ w - kPad - kFxW, m_yUntil, kFxW, r });

        // Five numbers on one row, each under its own caption — a form of six stacked labelled rows
        // would push the channel tree off the bottom, and these are read together anyway.
        float x = kPad;
        for (jf::JSpinBox* sb : { m_rate.get(), m_minOn.get(), m_minOff.get(),
                                  m_maxOn.get(), m_rearm.get() }) {
            sb->setBounds({ x, m_yNums, kNumW, r });
            x += kNumW + 16.f;
        }
        m_onInvalid->setBounds({ kPad + 132.f, m_yFlags, 90.f, r });
        m_enabled->setBounds({ kPad + 260.f, m_yFlags, 200.f, r });

        m_filter->setBounds({ kPad, m_yFilt, w - 2.f * kPad - 190.f, r });
        m_all ->setBounds({ w - kPad - 186.f, m_yFilt, 90.f, r });
        m_none->setBounds({ w - kPad - 92.f,  m_yFilt, 90.f, r });
        m_tree->setBounds({ kPad, treeY, w - 2.f * kPad, treeH });
        m_box->setBounds({ kPad, h - kPad - r, w - 2.f * kPad, r });

        // THE CARD GATES ACTIVATE, NOT THE DIALOG. Everything here except writing to the car is the
        // studio's own business — a tuner builds a profile at a desk with no ECU in the room, which
        // is most of what profiles are for. Only the one button that needs a card asks for one.
        // Re-checked per frame because a key turn changes the answer while this is open.
        m_activate->setEnabled(DatalogRecorder::ecuCard() == DatalogRecorder::Card::Ready);
    }

    void paint(jf::JPrimitiveBuffer& buf, float w, float h) override {
        if (!jf::JTextHelper::hasAtlas()) return;
        (void)h;
        const float r = rowH();
        auto label = [&buf](float x, float y, const std::string& s, const uint8_t* c) {
            jf::JTextHelper::pushText(buf, x, y, s, c);
        };

        label(kPad, m_ySets  + 6.f, "Profile",   jf::Colors::TextPrimary);
        label(kPad, m_yWhen  + 6.f, "Log While", jf::Colors::TextPrimary);
        label(kPad, m_yUntil + 6.f, "Stop When", jf::Colors::TextPrimary);

        // WHOSE SETUP THIS IS, and whether it has been touched since it was loaded. Save and Delete
        // refuse the car and the firmware's templates, and a button that refuses without having said
        // why first is a button that looks broken.
        const float tw = w - 2.f * kPad;
        wraptext::draw(buf, kPad + kLabelW, m_ySets + r + 5.f, _topLine(),
                       m_note.empty() ? jf::Colors::TextSecondary : jf::Colors::TextPrimary, tw - kLabelW);

        const uint8_t* gc = jf::Colors::TextSecondary;
        const std::string gate = _gateLine(&gc);
        wraptext::draw(buf, kPad, m_yUntil + r + 8.f, gate, gc, tw);

        // Captions ABOVE the numbers: five boxes in a row are unreadable without them, and beneath
        // them there is no room before the next control.
        const char* caps[] = { "Rate (Hz)", "Min log (ms)", "Min gap (ms)",
                               "Max log (ms)", "Re-arm (ms)" };
        float x = kPad;
        for (const char* c : caps) { label(x, m_yNums - 15.f, c, jf::Colors::TextSecondary);
                                     x += kNumW + 16.f; }
        label(kPad, m_yFlags + 6.f, "If unanswerable", jf::Colors::TextSecondary);

        std::string cost, note;
        _costLines(cost, note);
        wraptext::draw(buf, kPad, m_yCost, cost, jf::Colors::TextSecondary, tw);
        wraptext::draw(buf, kPad, m_yNote, note, jf::Colors::TextSecondary, tw);
    }

    // The line under the profile picker: what the last button did, else whose setup this is.
    std::string _topLine() const { return m_note.empty() ? _kindLine() : m_note; }

    // The gate's own line: the compile error if either condition does not parse, otherwise what
    // an empty pair actually does — which is not "nothing".
    std::string _gateLine(const uint8_t** colour) const {
        std::string gate;
        const uint8_t* gc = jf::Colors::TextSecondary;
        if (!m_whenErr.empty())       { gate = "Log While: " + m_whenErr;  gc = jf::Colors::Danger; }
        else if (!m_untilErr.empty()) { gate = "Stop When: " + m_untilErr; gc = jf::Colors::Danger; }
        else if (m_edit.logWhen.empty() && m_edit.logUntil.empty())
            gate = "Both empty \xE2\x80\x94 the built-in rule: log whenever the engine is turning.";
        else if (m_edit.logUntil.empty())
            gate = "Stops when Log While stops being true.";
        if (colour) *colour = gc;
        return gate;
    }

    void _costLines(std::string& out, std::string& note) const {
        // THE COUNT AND WHAT IT COSTS. A channel count on its own says nothing — the card sees
        // channels x bytes x rate, and both terms multiply. So this says the byte rate, the hourly
        // fill, and the fastest this particular selection will actually sustain, because "how fast
        // can it log" has no answer until you have chosen what to log.
        const DatalogRecorder::Cost cost =
            DatalogRecorder::costOf(std::vector<std::string>(m_on.begin(), m_on.end()), m_edit.rateHz);
        char line[240];
        // CAPACITY IS THE COST THAT ALWAYS APPLIES. A rate the card cannot keep up with costs less
        // per hour, not more — the ECU simply writes fewer records — so the hourly figure is the
        // honest budget for "will this fill my card", and it is stated for what was ASKED for.
        if (cost.mbPerHour >= 1000.0)
            std::snprintf(line, sizeof line,
                          "%zu of %zu channels  \xE2\x80\xA2  %d B/record  \xE2\x80\xA2  %.0f KB/s at %d Hz"
                          "  \xE2\x80\xA2  %.1f GB/hour",
                          m_on.size(), _total(), cost.recordBytes, cost.bytesPerSec / 1024.0,
                          m_edit.rateHz, cost.mbPerHour / 1000.0);
        else
            std::snprintf(line, sizeof line,
                          "%zu of %zu channels  \xE2\x80\xA2  %d B/record  \xE2\x80\xA2  %.0f KB/s at %d Hz"
                          "  \xE2\x80\xA2  %.0f MB/hour",
                          m_on.size(), _total(), cost.recordBytes, cost.bytesPerSec / 1024.0,
                          m_edit.rateHz, cost.mbPerHour);
        out = line;

        // PAST THE CARD IS NOT AN ERROR, and the wording matters: the logger drops the surplus and
        // counts it, holding nothing else up. A faster card is the fix, and that is the tuner's
        // call — so this informs and does not warn, and nothing here caps the rate.
        // TWO THINGS CAN BE TRUE AT ONCE, so this line says whichever is worth saying. Outrunning
        // the DATA is the more interesting one: sensors are decimated to their type's cadence and a
        // faster log does not speed them up — deliberately, because making acquisition follow the
        // logger would change the system being measured and could hide the fault being hunted. The
        // repeats are therefore an accurate record of when the value actually moved.
        note.clear();
        if (cost.slowestHz > 0 && m_edit.rateHz > cost.slowestHz) {
            char n2[200];
            std::snprintf(n2, sizeof n2,
                          "Above the data: %s updates at %d Hz, so the rest of each %d Hz record "
                          "repeats it. Sensors are not sped up to match \xE2\x80\x94 that is the real state.",
                          cost.slowestChannel.c_str(), cost.slowestHz, m_edit.rateHz);
            note = n2;
        } else if (cost.pastCard) {
            note = "More than the reference card took (~340 KB/s) \xE2\x80\x94 a slower card simply logs "
                   "fewer records and reports how many it skipped.";
        }
    }

private:
    // ---- the list -----------------------------------------------------------------------------
    // Entry 0 is always the car. Then the firmware's templates, then the tuner's own profiles —
    // most authoritative first, and the one a tuner opened the dialog to look at is the one showing.
    enum class Kind { Current, Template, Profile };

    void _reloadSets() {
        m_ids.clear(); m_kinds.clear();
        std::vector<std::string> items;
        items.push_back("Current (on the ECU)");
        m_ids.emplace_back(); m_kinds.push_back(Kind::Current);
        for (const auto& t : DatalogRecorder::templates()) {
            items.push_back(t.name + "  (firmware)");
            m_ids.push_back(t.id); m_kinds.push_back(Kind::Template);
        }
        for (const std::string& n : DatalogRecorder::profileNames()) {
            items.push_back(n);
            m_ids.push_back(n); m_kinds.push_back(Kind::Profile);
        }
        m_loading = true;
        m_sets->setItems(items);
        m_loading = false;
    }

    Kind _kind() const {
        const int i = m_sets->currentIndex();
        return (i >= 0 && size_t(i) < m_kinds.size()) ? m_kinds[size_t(i)] : Kind::Current;
    }
    std::string _id() const {
        const int i = m_sets->currentIndex();
        return (i >= 0 && size_t(i) < m_ids.size()) ? m_ids[size_t(i)] : std::string();
    }
    std::string _kindLine() const {
        const char* what = _kind() == Kind::Current
            ? "What the ECU is set to \xE2\x80\x94 Activate writes changes back to it."
            : _kind() == Kind::Template
                ? "The connected firmware's own set \xE2\x80\x94 read-only; Duplicate\xE2\x80\xA6 to edit it."
                : "Your profile \xE2\x80\x94 kept by the studio, edited with or without a link.";
        const DatalogRecorder::Card card = DatalogRecorder::ecuCard();
        if (card != DatalogRecorder::Card::Ready)
            return std::string(what) + "  (cannot Activate \xE2\x80\x94 " +
                   DatalogRecorder::cardReason(card) + ")";
        return m_dirty ? std::string("Edited \xE2\x80\x94 Save, Duplicate\xE2\x80\xA6 or Activate to keep it.")
                       : std::string(what);
    }

    // Selecting SHOWS a setup. Nothing is written by looking — not to the ECU, not to the profile.
    void _selected(int i) {
        if (m_loading) return;
        if (m_dirty && i != m_shownIndex) {
            // Unsaved work does not evaporate because a list changed. Ask, and put the list back
            // where it was if the answer is no — the combo has already moved by the time we hear.
            const int from = m_shownIndex;
            m_dirty = false;                     // the guard must not re-enter on our own setIndex
            jf::JDialog::confirm("Discard changes?",
                "The setup on screen has been edited and not saved.\n\nDiscard those changes?",
                [this, i] { _load(i); },
                [this, from] { m_loading = true; m_sets->setCurrentIndex(from);
                               m_loading = false; m_dirty = true; });
            return;
        }
        _load(i);
    }

    void _load(int i) {
        if (i < 0 || size_t(i) >= m_kinds.size()) return;
        m_shownIndex = i;
        switch (m_kinds[size_t(i)]) {
        case Kind::Current:  m_edit = DatalogRecorder::currentSettings(); break;
        case Kind::Template: m_edit = DatalogRecorder::templateProfile(m_ids[size_t(i)]); break;
        case Kind::Profile:  m_edit = DatalogRecorder::profile(m_ids[size_t(i)]); break;
        }
        m_on.clear();
        for (const std::string& c : m_edit.channels) m_on.insert(c);
        m_dirty = false;
        m_note.clear();
        _push();
        _rebuild();
    }

    // Model -> controls. Guarded, because every setter emits and every emission would otherwise be
    // read as the tuner having typed something.
    void _push() {
        m_loading = true;
        m_when->setText(m_edit.logWhen);
        m_until->setText(m_edit.logUntil);
        m_rate->setValue(m_edit.rateHz);
        m_minOn->setValue(m_edit.minOnMs);
        m_minOff->setValue(m_edit.minOffMs);
        m_maxOn->setValue(m_edit.maxOnMs);
        m_rearm->setValue(m_edit.rearmMs);
        m_onInvalid->setCurrentIndex(m_edit.onInvalid);
        m_enabled->setChecked(m_edit.enabled != 0);
        m_loading = false;
        _recompile();
    }

    void _touch() { m_dirty = true; m_note.clear(); }

    std::unique_ptr<jf::JLineEdit> _makeExpr(std::string& src, std::string& err, const char* eg) {
        auto e = std::make_unique<jf::JLineEdit>(graph(), std::string("e.g. ") + eg);
        e->setClearButtonEnabled(true);        // an empty gate is a meaning here too — see ExpressionWidget
        e->onTextChanged.connect([this, &src, &err](const std::string& t) {
            src = t;
            err = _compileErr(t);
            if (!m_loading) _touch();
        });
        return e;
    }

    std::unique_ptr<jf::JSpinBox> _makeNum(int& field, int lo, int hi) {
        auto sb = std::make_unique<jf::JSpinBox>(graph(), lo, hi, kNumW, rowH());
        sb->onValueChanged.connect([this, &field](int v) {
            field = v;
            if (!m_loading) _touch();
        });
        return sb;
    }

    // Compile for FEEDBACK only. Nothing is stored — a gate is bytecode only at the moment it is
    // written to the car, which is Activate.
    static std::string _compileErr(const std::string& src) {
        const MetaModel* meta = Cache::instance().meta();
        if (src.empty() || !meta) return {};       // empty is a valid setting, not a bad one
        const auto r = ExprCompiler::compile(src, *meta, (uint32_t)meta->configSize());
        return r.ok ? std::string() : r.error;
    }
    void _recompile() {
        m_whenErr  = _compileErr(m_edit.logWhen);
        m_untilErr = _compileErr(m_edit.logUntil);
    }

    // The full editor — field tree, live feedback, block usage — for the gate that is more than a
    // line you can type from memory. It opens as a CHILD modal above this one.
    void _editExpr(const std::string& path, jf::JLineEdit* box) {
        if (!Surface::onEditExpression) return;
        const MetaModel* meta = Cache::instance().meta();
        int off = 0, size = 0;
        if (!meta || !meta->resolveBlob(path, off, size)) return;
        Surface::onEditExpression(box->text(), (uint16_t)size, "Onboard logging gate",
                                  [box](std::string text) { box->setText(text); });
    }

    // ---- the four verbs -----------------------------------------------------------------------
    DatalogRecorder::Profile _current() const {
        DatalogRecorder::Profile p = m_edit;
        p.channels.assign(m_on.begin(), m_on.end());
        std::sort(p.channels.begin(), p.channels.end());
        return p;
    }

    void _activate() {
        if (m_on.empty()) { m_note = "Nothing selected \xE2\x80\x94 a log of no channels is a file of headers."; return; }
        std::string why;
        if (!DatalogRecorder::activate(_current(), &why)) { m_note = "Not written: " + why; return; }
        m_dirty = false;
        m_note = "Written to the ECU \xE2\x80\x94 " + std::to_string(m_on.size()) +
                 " channels. Burn to keep it across a reset.";
    }

    void _save() {
        if (_kind() == Kind::Current) {
            m_note = "This is the ECU's own setup \xE2\x80\x94 Duplicate\xE2\x80\xA6 to keep it as a profile.";
            return;
        }
        if (_kind() == Kind::Template) {
            m_note = "A firmware template is read-only \xE2\x80\x94 Duplicate\xE2\x80\xA6 to make an editable one.";
            return;
        }
        DatalogRecorder::Profile p = _current();
        p.name = _id();
        DatalogRecorder::saveProfile(p);
        m_dirty = false;
        m_note = "Saved \"" + p.name + "\".";
    }

    // DUPLICATE IS NAMING WHAT IS ALREADY ON SCREEN — which is also the only way a firmware template or
    // the car's own setup becomes something you can edit.
    void _copy() {
        DatalogRecorder::Profile p = _current();
        const std::string suggest = _kind() == Kind::Current ? std::string("From the ECU")
                                                             : m_sets->currentText();
        jf::JDialog::input("Duplicate Profile", "Name this onboard logging setup",
            [this, p](std::string name) mutable {
                if (name.empty()) return;
                p.name = name;
                DatalogRecorder::saveProfile(p);
                m_dirty = false;
                m_note = "Saved \"" + name + "\" (" + std::to_string(p.channels.size()) + " channels).";
                _reloadSets();
                for (size_t i = 0; i < m_ids.size(); ++i)
                    if (m_kinds[i] == Kind::Profile && m_ids[i] == name) {
                        m_loading = true; m_sets->setCurrentIndex(int(i)); m_loading = false;
                        m_shownIndex = int(i);
                        break;
                    }
            }, {}, suggest);
    }

    void _delete() {
        if (_kind() != Kind::Profile) {
            m_note = _kind() == Kind::Current
                ? std::string("The ECU's own setup is not a profile \xE2\x80\x94 there is nothing to delete.")
                : std::string("A firmware template belongs to the definition and cannot be deleted.");
            return;
        }
        const std::string name = _id();
        DatalogRecorder::deleteProfile(name);
        _reloadSets();
        m_loading = true; m_sets->setCurrentIndex(0); m_loading = false;
        _load(0);
        m_note = "Deleted \"" + name + "\".";
    }

    void _closeGuarded() {
        if (!m_dirty) { close(); return; }
        m_dirty = false;                       // whatever the answer, this dialog stops asking
        jf::JDialog::confirm("Discard changes?",
            "The setup on screen has been edited and not saved.\n\nClose and discard it?",
            [this] { close(); },
            [this] { m_dirty = true; });
    }

    // ---- channels -----------------------------------------------------------------------------
    size_t _total() const {
        const MetaModel* m = Cache::instance().meta();
        return m ? m->telemetry().size() : 0;
    }

    static std::string _lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Rebuild the visible tree from the meta, the filter and the current ticks. Cheap enough to do on
    // every keystroke: it is a walk of 452 entries and a sort, and the alternative — patching nodes
    // in place — is where a list and its model drift apart.
    void _rebuild() {
        const MetaModel* meta = Cache::instance().meta();
        jf::JTreeViewNode root;
        if (!meta) { m_tree->setRootNode(std::move(root)); return; }

        const std::string q = _lower(m_query);
        std::map<std::string, std::vector<std::string>> byCat;
        for (const auto& [name, f] : meta->telemetry()) {
            if (!q.empty() && _lower(name).find(q) == std::string::npos &&
                _lower(f.label).find(q) == std::string::npos &&
                _lower(f.module).find(q) == std::string::npos) continue;
            byCat[f.module.empty() ? std::string("Uncategorised") : f.module].push_back(name);
        }
        for (auto& [cat, names] : byCat) {
            std::sort(names.begin(), names.end());
            jf::JTreeViewNode group;
            size_t on = 0;
            for (const std::string& n : names) if (m_on.count(n)) ++on;
            group.label     = cat + "   (" + std::to_string(on) + "/" + std::to_string(names.size()) + ")";
            group.userData  = "@" + cat;            // '@' marks a group, so a click can tell them apart
            group.expanded  = !q.empty();           // a filtered tree opens: the point is to SEE the hits
            group.checkable = true;
            group.checked   = (on == names.size());
            group.partial   = (on > 0 && on < names.size());   // half a group is neither on nor off
            for (const std::string& n : names) {
                jf::JTreeViewNode leaf;
                leaf.label     = n;
                leaf.userData  = n;
                leaf.checkable = true;
                leaf.checked   = m_on.count(n) != 0;
                group.children.push_back(std::move(leaf));
            }
            root.children.push_back(std::move(group));
        }
        m_tree->setRootNode(std::move(root));
    }

    void _activateNode(jf::JTreeViewNode* n) {
        if (!n || n->userData.empty()) return;
        if (n->userData[0] == '@') {                 // a whole category, on or off together
            const std::string cat = n->userData.substr(1);
            const MetaModel* meta = Cache::instance().meta();
            if (!meta) return;
            size_t on = 0, total = 0;
            for (const auto& [name, f] : meta->telemetry())
                if (f.module == cat) { ++total; if (m_on.count(name)) ++on; }
            const bool turnOn = on < total;          // partly on -> fill it; fully on -> clear it
            for (const auto& [name, f] : meta->telemetry())
                if (f.module == cat) { if (turnOn) m_on.insert(name); else m_on.erase(name); }
        } else {
            if (!m_on.erase(n->userData)) m_on.insert(n->userData);
        }
        _touch();
        _rebuild();
    }

    // Everything the filter is currently showing — not everything there is. With an empty filter the
    // two are the same, which is the only case where "All" could mean either.
    void _setVisible(bool on) {
        const MetaModel* meta = Cache::instance().meta();
        if (!meta) return;
        const std::string q = _lower(m_query);
        for (const auto& [name, f] : meta->telemetry()) {
            if (!q.empty() && _lower(name).find(q) == std::string::npos &&
                _lower(f.label).find(q) == std::string::npos &&
                _lower(f.module).find(q) == std::string::npos) continue;
            if (on) m_on.insert(name); else m_on.erase(name);
        }
        _touch();
        _rebuild();
    }

    std::unique_ptr<jf::JComboBox>        m_sets, m_onInvalid;
    std::unique_ptr<jf::JButton>          m_activate, m_save, m_copy, m_delete;
    std::unique_ptr<jf::JLineEdit>        m_when, m_until;
    std::unique_ptr<jf::JButton>          m_whenFx, m_untilFx;
    std::unique_ptr<jf::JSpinBox>         m_rate, m_minOn, m_minOff, m_maxOn, m_rearm;
    std::unique_ptr<jf::JCheckBox>        m_enabled;
    std::unique_ptr<jf::JLineEdit>        m_filter;
    std::unique_ptr<jf::JTreeView>        m_tree;
    std::unique_ptr<jf::JButton>          m_all, m_none;
    std::unique_ptr<jf::JDialogButtonBox> m_box;

    std::vector<std::string> m_ids;      // parallel to the combo
    std::vector<Kind>        m_kinds;
    int  m_shownIndex = 0;               // what the editor is showing, for the discard guard
    bool m_loading = false;              // suppress the change signals a load necessarily emits
    bool m_dirty   = false;
    std::string m_note;                  // the last thing a button did, or why it refused

    DatalogRecorder::Profile m_edit;     // the setup on screen; the tree owns its channels
    std::set<std::string>    m_on;
    std::string m_whenErr, m_untilErr, m_query;

    float m_ySets = 0, m_yWhen = 0, m_yUntil = 0, m_yNums = 0, m_yFlags = 0, m_yFilt = 0;
    float m_yCost = 0, m_yNote = 0;          // the cost lines at the foot, placed by their wrapped height
};

#pragma once
//
// GenericCanPanel — the CAN bus as user-defined signal I/O, edited per bus.
//
// A CENTRE TOOL TAB, not a modal. Frames are tune data and editing them is not a question with an
// answer — it is work you do while watching the bus, with the page that reads those channels open
// beside it. It follows the trigger split: the PAGE draws what the bus carries, and the editing
// lives here, the way TriggerDiagramWidget draws a wheel the designer edits.
//
// THE SHAPE. Receive and transmit are two lists, not one with a direction column — they are opposite
// jobs, one arriving and becoming a signal, the other unsolicited traffic on somebody else's wire,
// and merging them buries that. The signal table edits IN PLACE, because a table you have to select
// a row of and then edit somewhere else is two places to look for one number. And the bit grid is
// the biggest thing on the page, because it is the only part that answers the question the numbers
// cannot: which bits does this field actually occupy, and does it collide with its neighbour.
//
// A JWidget, NOT a JContainer, and the difference is why this lays out at all. JContainer::add()
// adds a scene-graph LAYOUT edge as well as a tree edge, and the parent's paint then runs a flex
// pass over the subtree that overwrites every box set here. SurfaceCanvas documents the same
// discovery: write the children's boxes, never add the layout edge, forward input by hand.
//
#include <j/core/JWidget.h>
#include <j/core/JControl.h>
#include <j/core/KeyEvent.h>
#include <j/core/JStyle.h>
#include <j/core/JLineEdit.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JPickerField.h>
#include <j/core/JClearMark.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDataGrid.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/Cache.h"
#include "../model/MetaModel.h"
#include "CanTemplates.h"
#include "WrapText.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class GenericCanPanel : public jf::JWidget {
public:
    static constexpr float kPad = 12.f;
    // Signal rows the table can show. A frame is 64 bits, so 64 one-bit flags is the physical
    // ceiling and the schema allows it — this is what FITS on the page, and a frame holding more
    // says so rather than pretending the rest are not there.
    static constexpr int   kRows = 16;
    static constexpr int   kMaxFrames = 96, kMaxFields = 512;
    // THE HEIGHT EVERY CONTROL ON EVERY PAGE IS. Not JStyle::buttonHeight, which is 22: the pages are
    // authored at 30 (tools/layout/author.py, CTL_H) and this panel sits among them, so reading the
    // style's number made every control in here visibly shorter than the identical control one page
    // away. The style and the pages disagree about what a control is — until they are reconciled, the
    // panel follows the pages, because that is what it is drawn next to.
    static constexpr float kCtlH = 30.f;
    static float rowH() { return kCtlH; }

    // The row's remove control. A JButton so it hit-tests and takes keyboard focus like every other
    // control in the row, but the ✕ is DRAWN by the framework's one clear mark rather than set as a
    // label — a label put the character U+2715 through the font atlas, which does not pack it, so the
    // button rendered a literal '?' and read as a question instead of a remove.
    class ClearButton : public jf::JButton {
    public:
        ClearButton(jf::JSceneGraph& g, float w, float h) : jf::JButton(g, "", w, h) {}
    protected:
        void drawLabel(jf::JPrimitiveBuffer& buf, const jf::JRect& b) override {
            const auto o = jf::jstyle::option(m_state, isFocused());
            jf::JClearMark::draw(buf, b, jf::jstyle::role(jf::JColorRole::Text, o),
                                 m_state == jf::JWidgetState::Hovered ||
                                 m_state == jf::JWidgetState::Pressed);
        }
    };

    // Field flags and frame flags, as the firmware reads them (Can/CanMessageTypes.h).
    enum : uint8_t { F_SIGNED = 1u << 0, F_LITTLE = 1u << 1, F_SENTINEL = 1u << 2 };
    enum : uint8_t { M_USED = 1u << 0, M_TX = 1u << 1, M_EXT = 1u << 2, M_OFF = 1u << 3 };

    // NO CHANNEL. It cannot be 0: signal id 0 is abs_mode, a real channel, so a field left unset
    // published its decoded value onto it — and two unset fields counted as two producers of one
    // channel and raised the conflict code. 65535 is the firmware's SIG_NONE.
    static constexpr int kSigNone = 65535;

    struct Field {
        int      sig      = kSigNone;
        int      bitOff   = 7;
        int      width    = 8;
        int      flags    = 0;
        double   scale    = 1.0;
        double   offset   = 0.0;
        int      ttlMs    = 500;
        int      policy   = 0;      // transmit: what to send when the channel is absent
        int      priority = 10;     // receive: the authority it is written with (PRIO_CAN)
        uint32_t sentinel = 0;      // receive: the raw code meaning "nothing to report"
        int      index    = -1;     // its place in the pool — what a CAN sensor is pointed at
    };
    struct Frame {
        bool        used = false, tx = true, ext = false, off = false;
        int         bus = 0, id = 0, dlc = 8, periodMs = 50;
        std::string name;
        std::vector<Field> fields;
    };

    explicit GenericCanPanel(jf::JSceneGraph& g) : jf::JWidget(g, "GenericCanPanel"), g_(g) {
        const float r = rowH();

        m_loadTpl = mk<jf::JButton>("Load Template\xE2\x80\xA6", 132.f, r);
        m_loadTpl->onClicked.connect([this] { if (onLoadTemplate) onLoadTemplate(this); });
        // LOADING IS ADDITIVE — it appends a template's frames beside whatever is already here and
        // skips the ids this bus already carries — so the matching gesture is to take one back OUT
        // again, as a set. Removing a dozen Haltech frames one selection at a time is not a thing
        // anyone should have to do, and picking them out of a list of twenty-nine by eye is worse.
        m_dropTpl = mk<jf::JButton>("Remove Template\xE2\x80\xA6", 148.f, r);
        m_dropTpl->onClicked.connect([this] { if (onDropTemplate) onDropTemplate(this); });

        // ---- the frame list --------------------------------------------------------------------
        // ONE list, because the page is one bus in one direction. A Bus picker and a second list were
        // two controls answering questions the page had already answered by being the page it is.
        m_list = mk<jf::JDataGrid>();
        m_list->setSelectionMode(jf::JDataGrid::SelectionMode::Extended);
        m_list->onSelectionChanged.connect([this](int) { _syncFrame(); });

        m_add = mk<jf::JButton>("Add Frame", 94.f, r);
        m_del = mk<jf::JButton>("Remove",    82.f, r);
        m_add->onClicked.connect([this] { _addFrame();    });
        m_del->onClicked.connect([this] { _removeFrame(); });

        // ---- the frame's own settings, one row -------------------------------------------------
        m_on = mk<jf::JCheckBox>("Enabled", 88.f);
        m_on->onCheckStateChanged.connect([this](jf::JCheckBox::CheckState s) {
            if (Frame* f = _frame(); f && !m_loading) { f->off = (s != jf::JCheckBox::Checked); _commit(); }
        });
        m_name = mk<jf::JLineEdit>("what this frame is for");
        m_name->onTextChanged.connect([this](const std::string& t) {
            if (Frame* f = _frame(); f && !m_loading) { f->name = t.substr(0, 19); _commit(); }
        });
        m_idType = mk<jf::JComboBox>(
            std::vector<std::string>{ "Standard (11 bit)", "Extended (29 bit)" }, 156.f);
        m_idType->onIndexChanged.connect([this](int i) {
            if (Frame* f = _frame(); f && !m_loading) { f->ext = (i == 1); _clampId(*f); _commit(); }
        });
        m_id = mk<jf::JLineEdit>("0x360");
        m_id->onTextChanged.connect([this](const std::string& t) {
            if (Frame* f = _frame(); f && !m_loading) { f->id = _parseId(t); _clampId(*f); _commit(); }
        });
        m_dlc = mk<jf::JComboBox>(std::vector<std::string>{ "0 Bytes", "1 Byte", "2 Bytes", "3 Bytes",
                                   "4 Bytes", "5 Bytes", "6 Bytes", "7 Bytes", "8 Bytes" }, 96.f);
        m_dlc->onIndexChanged.connect([this](int i) {
            if (Frame* f = _frame(); f && !m_loading) { f->dlc = i; _commit(); }
        });
        // The rates a real sender uses, plus whatever the tune already holds — a list that cannot
        // express the frame in front of you is a list that silently changes it.
        m_rate = mk<jf::JComboBox>(_rateLabels(), 140.f);
        m_rate->onIndexChanged.connect([this](int i) {
            if (Frame* f = _frame(); f && !m_loading && i < int(kRateMs().size())) {
                f->periodMs = kRateMs()[size_t(i)]; _commit();
            }
        });

        m_addFld = mk<jf::JButton>("Add Signal", 98.f, r);

        // ---- the signal rows, edited IN PLACE --------------------------------------------------
        for (int k = 0; k < kRows; k++) {
            Row& w = m_row[k];
            w.sig = mk<jf::JPickerField>(210.f, r);
            w.sig->setClearable(true);
            // EMPTY IS THE UNSET STATE, and the placeholder is what says so. Putting "— none —" in
            // the field as a value would make an unset field look like a chosen one.
            w.sig->setPlaceholder(std::string(kNone));
            w.sig->onOpenRequested.connect([this, k] { _pickSignal(k); });
            w.sig->onCleared.connect([this, k] {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->sig = kSigNone; _commit(); _syncFrame(); }
            });
            w.fmt = mk<jf::JComboBox>(
                std::vector<std::string>{ "Big Endian (Motorola)", "Little Endian (Intel)" }, 176.f);
            w.fmt->onIndexChanged.connect([this, k](int i) {
                Field* d = _fieldAt(k);
                if (!d || m_loading) return;
                // The start bit means the field's MSB in one order and its LSB in the other, so
                // keeping the number would silently move the field off the bits that were drawn.
                const int lastOld = _lastBit(*d);
                d->flags = (i == 1) ? (d->flags | F_LITTLE) : (d->flags & ~F_LITTLE);
                d->bitOff = (i == 1) ? std::min(d->bitOff, lastOld) : std::max(d->bitOff, lastOld);
                _commit();
            });
            w.start = mk<jf::JSpinBox>(0, 63, 1);
            w.start->onValueChanged.connect([this, k](int v) {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->bitOff = v; _commit(); }
            });
            w.bits = mk<jf::JSpinBox>(1, 32, 1);
            w.bits->onValueChanged.connect([this, k](int v) {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->width = v; _commit(); }
            });
            w.sign = mk<jf::JComboBox>(std::vector<std::string>{ "Unsigned", "Signed" }, 108.f);
            w.sign->onIndexChanged.connect([this, k](int i) {
                if (Field* d = _fieldAt(k); d && !m_loading) {
                    d->flags = i ? (d->flags | F_SIGNED) : (d->flags & ~F_SIGNED); _commit();
                }
            });
            w.mult = mk<jf::JLineEdit>("1");
            w.mult->onTextChanged.connect([this, k](const std::string& t) {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->scale = _num(t, 1.0); _commit(); }
            });
            w.off = mk<jf::JLineEdit>("0");
            w.off->onTextChanged.connect([this, k](const std::string& t) {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->offset = _num(t, 0.0); _commit(); }
            });
            // THE THREE SETTINGS ONLY A TEMPLATE COULD SET. They were round-tripped and never shown,
            // so a frame built here got 500 ms / Send zero / no code whatever its sender did — a
            // 1 Hz sender's value expired between frames and nothing on the page could say otherwise.
            // One direction each, so a row only ever shows the ones its frame uses.
            w.absent = mk<jf::JComboBox>(
                std::vector<std::string>{ "Send zero", "Hold last", "Skip frame" }, 110.f);
            w.absent->onIndexChanged.connect([this, k](int i) {
                if (Field* d = _fieldAt(k); d && !m_loading) { d->policy = i; _commit(); }
            });
            w.ttl = mk<jf::JLineEdit>("500");
            w.ttl->onTextChanged.connect([this, k](const std::string& t) {
                if (Field* d = _fieldAt(k); d && !m_loading) {
                    d->ttlMs = int(std::clamp(_num(t, 500.0), 0.0, 65535.0)); _commit();
                }
            });
            // Empty is "this sender has no such code" — the flag, not a code of 0, which is a real
            // reading on most fields.
            w.noread = mk<jf::JLineEdit>("none");
            w.noread->onTextChanged.connect([this, k](const std::string& t) {
                Field* d = _fieldAt(k);
                if (!d || m_loading) return;
                char* end = nullptr;
                const unsigned long v = t.empty() ? 0ul : std::strtoul(t.c_str(), &end, 0);
                if (t.empty() || !end || end == t.c_str()) d->flags &= ~F_SENTINEL;
                else { d->flags |= F_SENTINEL; d->sentinel = uint32_t(v); }
                _commit();
            });
            w.del = mk<ClearButton>(26.f, r);
            w.del->onClicked.connect([this, k] { _removeField(k); });
        }

        m_addFld->onClicked.connect([this] { _addField(); });

        attach();
    }

    // Wired by the app: choosing a template is a question with named answers, and the panel does not
    // own a window. Left unset the button does nothing rather than silently loading the first one,
    // which is exactly the bug this replaced.
    static inline std::function<void(GenericCanPanel*)> onLoadTemplate;
    static inline std::function<void(GenericCanPanel*)> onDropTemplate;

    // Re-read the tune. The frames are config, and config changes without asking this panel first.
    void attach() { _load(); _rebuildLists(); }

    // Which bus this page is about, for anything that has to ask the TUNE about it — the template
    // picker checks the bus's bit rate before it offers a protocol that runs at another one.
    int  bus() const      { return m_busSel; }
    bool transmit() const { return m_tx; }

    // Which bus and which direction this page is about. Set by the widget hosting the panel, from
    // the page's own properties — never by a control, because the page already said it by existing.
    void configure(int bus, bool transmit) {
        if (bus < 0 || bus > 1) return;
        // THE FIRST CALL ALWAYS APPLIES, whatever it asks for. This used to return early when the
        // values already matched — and the defaults ARE bus 0 / transmit, so the CAN1 Transmit page
        // never set its headers or column widths at all: the grid kept its bare constructor state,
        // rows and no columns to draw them in, which renders as a tall box of empty lines.
        const bool changed = (m_busSel != bus) || (m_tx != transmit);
        if (!changed && m_configured) return;
        m_busSel    = bus;
        m_tx        = transmit;
        m_configured = true;
        m_list->setHeaders(m_tx ? std::vector<std::string>{ "ID", "Rate", "Name" }
                                : std::vector<std::string>{ "ID", "Name" });
        // 68, not 58: the Rate column held "1000ms" comfortably and now holds a rate, and an odd
        // period reads as "30.3 Hz" — seven characters where the widest canned rate is six.
        m_list->setColumnWidths(m_tx ? std::vector<float>{ 82.f, 68.f, 130.f }
                                     : std::vector<float>{ 82.f, 160.f });
        if (changed) m_list->clearSelection();
        _rebuildLists();
    }

    // Load one template's frames into this bus. Called by the app once the user has PICKED one.
    void loadTemplate(const CanTemplates::Template& t) {
        for (const CanTemplates::Frame& tf : t.frames) {
            if (int(m_all.size()) >= kMaxFrames) break;
            if (hasFrame(tf.id, tf.ext)) continue;      // already on this bus — do not stack a copy
            Frame f;
            f.used = true; f.tx = t.transmit; f.ext = tf.ext; f.bus = m_busSel;
            f.id = tf.id; f.dlc = tf.dlc;
            f.periodMs = tf.periodMs ? tf.periodMs : 50;
            f.name = tf.name.empty() ? t.id : tf.name;
            for (const CanTemplates::Field& s : tf.fields) {
                Field d;
                d.sig      = s.sig.empty() ? kSigNone : _sigIdFor(s.sig);   // empty = a sensor consumes it
                d.bitOff   = s.bitOff;  d.width  = s.width;  d.flags = s.flags;
                d.scale    = s.scale;   d.offset = s.offset; d.policy = s.policy;
                d.ttlMs    = s.ttlMs;   d.sentinel = s.sentinel;     d.priority = 10;
                f.fields.push_back(d);
            }
            m_all.push_back(f);
        }
        _commit();
    }

    // Take a template's frames back off this bus, as the set they arrived as.
    //
    // MATCHED BY ID, not by a provenance mark stored in the tune. A frame's identity on the wire IS
    // its id, so matching on it removes exactly what loading that template would have added — and it
    // keeps working after the frame has been renamed, re-rated or had a field added, which a stored
    // "this came from Haltech V2" would not. What it cannot tell apart is a frame the user built by
    // hand that happens to share an id with a template's, which is why the caller says how many are
    // about to go and the button asks before it acts.
    //
    // Returns how many were removed. The frames a removal orphans are NOT chased down: a sensor left
    // pointing at a frame that is gone raises its config DTC and shows as missing in its own picker,
    // which is the honest outcome and a better one than quietly clearing the sensor's setting.
    int dropTemplate(const CanTemplates::Template& t) {
        int n = 0;
        for (size_t i = m_all.size(); i-- > 0; ) {     // highest first: the earlier indices keep meaning
            const Frame& f = m_all[i];
            if (f.bus != m_busSel || f.tx != t.transmit) continue;
            for (const CanTemplates::Frame& tf : t.frames) {
                if (f.id != int(tf.id) || f.ext != tf.ext) continue;
                m_all.erase(m_all.begin() + long(i));
                n++;
                break;
            }
        }
        if (n) { _commit(); m_list->clearSelection(); _syncFrame(); }
        return n;
    }

    // How many of a template's frames this bus is carrying — what the Remove list labels each entry
    // with, and what decides whether it is offered at all.
    int templateFramesHere(const CanTemplates::Template& t) const {
        int n = 0;
        for (const Frame& f : m_all) {
            if (f.bus != m_busSel || f.tx != t.transmit) continue;
            for (const CanTemplates::Frame& tf : t.frames)
                if (f.id == int(tf.id) && f.ext == tf.ext) { n++; break; }
        }
        return n;
    }

    // How many CAN SENSORS read a field in the frames a removal would take, so the question the user
    // is answering says what it costs. Counted off the tune, because a sensor names its frame by the
    // same key the firmware matches on — nothing here has to know which sensor is which.
    int sensorsReading(const CanTemplates::Template& t) const {
        const Cache& c = Cache::instance();
        const MetaModel* m = c.meta();
        if (!m) return 0;
        const auto it = m->configArrays().find("sensors.sensor");
        if (it == m->configArrays().end()) return 0;
        int n = 0;
        for (int i = 0; i < it->second.count; i++) {
            const std::string sp = "sensors.sensor[" + std::to_string(i) + "].";
            if (c.configValue(sp + "enabled") == 0.0) continue;
            if (c.elementOption("sensors.sensor", i, "interface") != "can_device") continue;
            if (int(c.configValue(sp + "can_bit")) < 0) continue;   // names no field
            const uint32_t key = uint32_t(c.configValue(sp + "can_frame"));
            for (const CanTemplates::Frame& tf : t.frames)
                if (_frameKey(tf.id, tf.ext) == key) { n++; break; }
        }
        return n;
    }

    // A frame's identity on this bus as one number — the key the firmware matches an arriving frame
    // on (GenericCan::frame_key) and the one a CAN sensor stores to name the frame it reads.
    uint32_t _frameKey(uint32_t id, bool ext) const {
        return (uint32_t(m_busSel & 1) << 30) | (ext ? (1u << 29) : 0u) | (id & 0x1FFFFFFFu);
    }

    // Is this frame already here? The template button asks before loading, because twelve identical
    // widebands is what happens when it does not.
    bool hasFrame(uint32_t id, bool ext) const {
        for (const Frame& f : m_all)
            if (f.bus == m_busSel && f.id == int(id) && f.ext == ext) return true;
        return false;
    }

    // A HOST CAN PLACE THIS. On a page the panel is not a node the layout engine sizes — it is
    // whatever box the page widget hands it — so the box is set from outside when there is one.
    void setBox(const jf::JRect& b) { m_box = b; m_hasBox = true; }

    // Test access: pick a frame in the list, which is what makes a signal row exist at all — the
    // editor follows the selection, and arriving at the page with nothing selected shows no rows.
    bool selectFrameForTest(int row) {
        if (!m_list || row < 0 || row >= int(m_view.size())) return false;
        m_list->setSelectedIndex(row);
        _syncFrame();
        return true;
    }

    // Test access: a signal row's picker field. Handed over whole rather than as "what does it say"
    // and "clear it", because the claim being tested is that the ✕ is THERE AND REACHABLE — and a
    // signal emitted by hand would pass whether or not anything could ever press it.
    jf::JPickerField* rowSignalFieldForTest(int k) {
        return (k >= 0 && k < kRows) ? m_row[k].sig : nullptr;
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const jf::JRect b = m_hasBox ? m_box : g_.getLayoutConst(getNodeId()).boundingBox;
        _place(b);
        buf.pushClip(b.x, b.y, b.width, b.height);
        _paint(buf, b);
        for (jf::JWidget* w : children())
            if (w && w->isVisible()) w->populateRenderPrimitives(buf);
        buf.popClip();
    }

    // THE HOST DRIVES INPUT. On the canvas the Surface routes run-mode events through
    // handleControlInput to ONE widget — it never uses the framework's JWidget dispatch — so the
    // handleMouse* overrides that used to be here were never called by anything. That is why every
    // click on Add Frame went nowhere.
    //
    // A hosted control is also outside the window's focus chain (the canvas is its own focus
    // domain), so the panel tracks which control has the keyboard and blurs the last one itself:
    // nothing else will.
    bool press(float mx, float my) {
        jf::JControl* hit = _at(mx, my);
        if (hit != m_active) {
            if (m_active) { m_active->endEdit(); m_active->setFocused(false); }
            m_active = hit;
        }
        if (!hit) return false;
        hit->setFocused(true);
        hit->handleMousePress(mx, my);
        // …and re-read: a press may move the grid's selection without emitting (it only emits when
        // the index CHANGES, so re-clicking the highlighted row is silent).
        if (hit == static_cast<jf::JControl*>(m_list)) _syncFrame();
        return true;
    }

    static bool _inside(const jf::JWidget* w, float mx, float my) {
        if (!w || !w->isVisible()) return false;
        const jf::JRect b = w->bounds();
        return mx >= b.x && mx < b.x + b.width && my >= b.y && my < b.y + b.height;
    }
    void move(float mx, float my) {
        for (jf::JWidget* w : children()) if (w && w->isVisible()) w->handleMouseMove(mx, my);
    }
    void release(float mx, float my) {
        for (jf::JWidget* w : children()) if (w && w->isVisible()) w->handleMouseRelease(mx, my);
    }
    bool scroll(float mx, float my, float wheel) {
        jf::JControl* hit = _at(mx, my);
        return hit ? hit->handleScroll(mx, my, wheel) : false;
    }
    bool key(const jf::JKeyEvent& e) { return m_active ? m_active->handleKeyEvent(e) : false; }
    void blur() {
        if (!m_active) return;
        m_active->endEdit();
        m_active->setFocused(false);
        m_active = nullptr;
    }

private:
    // The topmost visible control under the cursor. Reverse order, so a control added later wins —
    // the same order they paint in.
    jf::JControl* _at(float mx, float my) const {
        const auto& cs = children();
        for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
            jf::JWidget* w = *it;
            if (!w || !w->isVisible()) continue;
            const jf::JRect b = w->bounds();
            if (mx >= b.x && mx < b.x + b.width && my >= b.y && my < b.y + b.height)
                return dynamic_cast<jf::JControl*>(w);
        }
        return nullptr;
    }

    // Raw pointers: mk() owns every control in m_own, so a row is a VIEW of eight of them.
    struct Row {
        // A PICKER FIELD, NOT A DROPDOWN AND NOT A BUTTON. There are ~440 channels, so a combo of them
        // in every one of twelve rows is 5,000 strings nobody can find anything in — this opens the
        // same searchable dialog every *_src selector in the studio opens (enumpick::signal).
        //
        // It was a bare JButton, and a button has ONE affordance: press it and the dialog opens. So
        // there was nowhere to UNSET a channel. A field with no channel is not an edge case here — it
        // is how a CAN SENSOR reads a frame — and the only way to get back to one was to find
        // "— none —" at the top of a searchable list of four hundred. JPickerField is the control the
        // framework already has for exactly this: the chosen name, an ellipsis well that says the list
        // is elsewhere, and an ✕ that says nothing chosen is an answer.
        jf::JPickerField *sig = nullptr;
        jf::JComboBox *fmt = nullptr, *sign = nullptr;
        jf::JSpinBox  *start = nullptr, *bits = nullptr;
        jf::JLineEdit *mult = nullptr, *off = nullptr;
        jf::JComboBox *absent = nullptr;                    // transmit only
        jf::JLineEdit *ttl = nullptr, *noread = nullptr;    // receive only
        jf::JButton   *del = nullptr;
    };

    template <class T, class... A> T* mk(A&&... a) {
        auto p = std::make_unique<T>(g_, std::forward<A>(a)...);
        T* raw = p.get();
        addChild(raw);                 // tree edge only — _place() decides the geometry
        // NoFocus, always: a canvas control cannot sit in the window's focus chain or the framework
        // clears its focus a frame after every click. The panel drives Focus/Blur itself.
        raw->setFocusPolicy(jf::JFocusPolicy::NoFocus);
        m_own.push_back(std::move(p));
        return raw;
    }

    static constexpr const char* kNone = "\xE2\x80\x94 none (a sensor reads it) \xE2\x80\x94";

    // A COLUMN HEADED "Rate" READS IN HERTZ. It printed the stored period — "50ms" under a heading
    // that says Rate — while the combo editing that same field one panel over said "20 Hz (50ms)",
    // so the list and its editor disagreed about which of the two numbers a frame's rate IS. Every
    // protocol document states these in Hz (Haltech 20, AEM 100, Link 50); the firmware schedules a
    // deadline in ms. Hz is the question and ms is the answer, so Hz is what the column shows.
    //
    // It cannot be a UNIT conversion: UnitManager converts linearly (base = (value - offset)/factor)
    // and Hz from ms is reciprocal, so there is no factor that expresses it. Hence a formatter.
    static std::string _rateText(int periodMs) {
        if (periodMs <= 0) return "—";                       // no period set: not a rate at all
        char b[24];
        // A period that does not divide 1000 is a real rate all the same — a tune can hold 33ms —
        // and rounding it to "30 Hz" claims a precision the frame does not have.
        if (1000 % periodMs == 0) std::snprintf(b, sizeof b, "%d Hz", 1000 / periodMs);
        else                      std::snprintf(b, sizeof b, "%.1f Hz", 1000.0 / double(periodMs));
        return b;
    }

    static const std::vector<int>& kRateMs() {
        static const std::vector<int> v{ 1000, 500, 200, 100, 50, 10, 5 };
        return v;
    }
    static std::vector<std::string> _rateLabels() {
        return { "1 Hz (1000ms)", "2 Hz (500ms)", "5 Hz (200ms)", "10 Hz (100ms)",
                 "20 Hz (50ms)", "100 Hz (10ms)", "200 Hz (5ms)" };
    }

    // ---------------------------------------------------------------------------------------
    // Placement
    // ---------------------------------------------------------------------------------------
    // ONE PASS COMPUTES BOTH. Labels used to be drawn from hand-tuned offsets in _paint while the
    // controls were positioned from other hand-tuned offsets here, and the two drifted the moment the
    // box changed size — captions over the top of the fields they name. Now a label is placed BY the
    // same call that places its control, into m_lab, and _paint only draws what this decided.
    struct Lab { float x, y; std::string t; };

    static float textW(const std::string& t) {
        if (t.empty()) return 0.f;
        const float w = jf::JTextHelper::measureWidth(t);
        // A zero width for non-empty text is a refusal, not a measurement (the atlas is not up yet).
        return w > 0.f ? w : float(t.size()) * 7.6f;
    }

    void _place(const jf::JRect& box) {
        const float r = rowH(), ox = box.x, oy = box.y, W = box.width, H = box.height;
        m_lab.clear();
        const Frame* f = _frame();

        // Everything about a frame is hidden when there is no frame, rather than sitting greyed under
        // a message telling you to add one.
        for (jf::JWidget* c : { (jf::JWidget*)m_on, (jf::JWidget*)m_name, (jf::JWidget*)m_idType,
                                (jf::JWidget*)m_id, (jf::JWidget*)m_dlc, (jf::JWidget*)m_rate,
                                (jf::JWidget*)m_addFld })
            c->setVisible(f != nullptr);

        // ---- header: the bus and the template button -------------------------------------------
        m_x0 = ox + kPad;
        const float yTop = oy + kPad;
        m_loadTpl->setBounds({ ox + W - kPad - 132.f, yTop, 132.f, r });
        m_dropTpl->setBounds({ ox + W - kPad - 132.f - 6.f - 148.f, yTop, 148.f, r });

        // ---- left column: this page's frames ---------------------------------------------------
        const float listW = 292.f;
        const float ly    = yTop + r + 18.f;
        const float listH = (oy + H) - ly - 18.f - (r + kPad);

        m_lab.push_back({ m_x0, ly, m_tx ? "Transmit Frames" : "Receive Frames" });
        m_list->setBounds({ m_x0, ly + 18.f, listW, listH });
        m_add->setBounds({ m_x0,         ly + 18.f + listH + 6.f, 94.f, r });
        m_del->setBounds({ m_x0 + 100.f, ly + 18.f + listH + 6.f, 82.f, r });
        // The button SAYS how many it will take, so a multi-row delete is never a surprise.
        const size_t nSel = m_list->selectedIndices().size();
        m_del->setEnabled(nSel > 0);
        m_del->setLabel(nSel > 1 ? "Remove " + std::to_string(nSel) : std::string("Remove"));

        // ---- right column ----------------------------------------------------------------------
        const float x = m_x0 + listW + 18.f;
        m_colX = x;
        m_gridW = (ox + W) - x - kPad;
        m_yFrm  = ly;

        if (!f) { for (int k = 0; k < kRows; k++) _showRow(k, false); return; }

        // The frame's settings FLOW: each control sits just right of its own caption, so the row is
        // as wide as it needs to be instead of spread across a thousand pixels of fixed offsets.
        float fx = x;
        m_on->setBounds({ fx, m_yFrm, 88.f, r });  fx += 88.f + 16.f;
        fx = _row(fx, m_yFrm, "Name",   m_name,   190.f);
        fx = _row(fx, m_yFrm, "ID",     m_id,     96.f);
        fx = _row(fx, m_yFrm, "",       m_idType, 156.f);
        fx = _row(fx, m_yFrm, "Bytes",  m_dlc,    96.f);
        if (f->tx) fx = _row(fx, m_yFrm, "Every", m_rate, 140.f);
        m_rate->setVisible(f->tx);

        m_yAdd = m_yFrm + r + 14.f;
        m_addFld->setBounds({ x, m_yAdd, 98.f, r });

        // ---- the signal table, edited in place --------------------------------------------------
        // The note beside Add Signal WRAPS in what is left of the row; the table starts below it.
        m_yTbl = m_yAdd + r + 12.f + (m_note.empty() ? 0.f : wraptext::extra(m_note, m_gridW - 120.f));
        const float rh = r + 4.f;
        const int   n  = _fieldCount();
        for (int k = 0; k < kRows; k++) {
            const bool show = k < n;
            _showRow(k, show);
            if (!show) continue;
            Row& w = m_row[k];
            const float ry = m_yTbl + 18.f + float(k) * rh;
            float cx = x;
            for (auto& c : m_col) c.x = 0.f;
            // Sized to fit the page at its minimum width WITH the direction's own columns on the end.
            cx = _cell(cx, ry, 0, w.sig,   180.f);
            cx = _cell(cx, ry, 1, w.fmt,   168.f);
            cx = _cell(cx, ry, 2, w.start, 58.f);
            cx = _cell(cx, ry, 3, w.bits,  54.f);
            cx = _cell(cx, ry, 4, w.sign,  90.f);
            cx = _cell(cx, ry, 5, w.mult,  70.f);
            cx = _cell(cx, ry, 6, w.off,   70.f);
            if (m_tx) {
                cx = _cell(cx, ry, 7, w.absent, 110.f);
            } else {
                cx = _cell(cx, ry, 8, w.ttl,    64.f);
                cx = _cell(cx, ry, 9, w.noread, 84.f);
            }
            w.del->setBounds({ cx, ry, 26.f, r });
        }
        m_yGrid = m_yTbl + 18.f + float(std::max(1, n)) * rh + 16.f;
    }

    // One "caption then control" step. Returns where the next one starts.
    float _row(float x, float y, const char* cap, jf::JWidget* c, float w) {
        float cx = x;
        if (cap && *cap) {
            m_lab.push_back({ x, y + 6.f, cap });
            cx = x + textW(cap) + 8.f;
        }
        c->setBounds({ cx, y, w, rowH() });
        return cx + w + 16.f;
    }
    // …and one table cell, which also records the column x so the header lands over it.
    float _cell(float x, float y, int col, jf::JWidget* c, float w) {
        if (col < int(std::size(m_col))) m_col[col].x = x;
        c->setBounds({ x, y, w, rowH() });
        return x + w + 6.f;
    }
    // WHICH CHANNEL THIS FIELD CARRIES. The full bus list, because a generic CAN field may carry
    // anything the ECU knows — unlike a *_src selector on a sensor, which is restricted to channels a
    // sensor produces. NO channel is a real answer too, and it is the ✕ on the field rather than a row
    // in here: it means a SENSOR reads this field, and the sensor publishes the channel instead.
    void _pickSignal(int k) {
        Field* d = _fieldAt(k);
        const MetaModel* m = Cache::instance().meta();
        if (!d || !m || !enumpick::signal()) return;
        // NO "none" ENTRY. The ✕ on the field is where unsetting lives now, which is both easier to
        // find than one row among four hundred and the same gesture as every other picker field.
        std::vector<std::string> names;
        std::string current;
        for (const auto& [nm, id] : m->signalMap()) {
            names.push_back(nm);
            if (id == d->sig) current = nm;
        }
        enumpick::signal()(std::move(names), std::move(current), [this, k](std::string picked) {
            Field* f = _fieldAt(k);
            const MetaModel* mm = Cache::instance().meta();
            if (!f || !mm || picked.empty()) return;          // cancelled
            const auto it = mm->signalMap().find(picked);
            if (it == mm->signalMap().end()) return;
            f->sig = it->second;
            _commit();
            _syncFrame();
        });
    }

    void _showRow(int k, bool on) {
        Row& w = m_row[k];
        for (jf::JWidget* c : { (jf::JWidget*)w.sig, (jf::JWidget*)w.fmt, (jf::JWidget*)w.start,
                                (jf::JWidget*)w.bits, (jf::JWidget*)w.sign, (jf::JWidget*)w.mult,
                                (jf::JWidget*)w.off, (jf::JWidget*)w.del })
            c->setVisible(on);
        w.absent->setVisible(on && m_tx);
        w.ttl->setVisible(on && !m_tx);
        w.noread->setVisible(on && !m_tx);
    }

    // ---------------------------------------------------------------------------------------
    // Painting
    // ---------------------------------------------------------------------------------------
    void _paint(jf::JPrimitiveBuffer& buf, const jf::JRect& box) {
        if (!jf::JTextHelper::hasAtlas()) return;
        auto text = [&buf](float x, float y, const std::string& s, const uint8_t* c) {
            jf::JTextHelper::pushText(buf, x, y, s, c);
        };
        // Every caption this panel draws was positioned by _place, beside the control it names.
        for (const Lab& l : m_lab) text(l.x, l.y, l.t, jf::Colors::TextPrimary);

        const Frame* f = _frame();
        if (!f) {
            text(m_colX, m_yFrm + 6.f,
                 std::string("No frame selected \xE2\x80\x94 Add Frame to begin."),
                 jf::Colors::TextSecondary);
            return;
        }

        // The table's headers, over the columns _place actually put the controls in.
        static const char* kHead[] = { "Signal", "Format", "Start", "Bits", "Sign",
                                       "Multiplier", "Offset", "If Absent", "Valid (ms)", "No Reading" };
        for (size_t i = 0; i < std::size(kHead); i++)
            if (m_col[i].x > 0.f)
                text(m_col[i].x, m_yTbl, kHead[i], jf::Colors::TextSecondary);
        if (_fieldCount() == 0)
            text(m_colX, m_yTbl + 20.f,
                 "No signals in this frame yet \xE2\x80\x94 Add Signal.", jf::Colors::MutedText);
        // A frame loaded from a template may carry more than the table can show; say so rather than
        // drawing the first sixteen and leaving the rest invisible but live on the wire.
        else if (_fieldCount() > kRows)
            text(m_colX, m_yGrid - 14.f,
                 "Showing " + std::to_string(kRows) + " of " + std::to_string(_fieldCount()) +
                 " signals in this frame.", jf::Colors::Warning);
        if (!m_note.empty()) wraptext::draw(buf, m_colX + 120.f, m_yAdd + 6.f, m_note, jf::Colors::Warning, m_gridW - 120.f);

        _paintGrid(buf, m_colX, m_yGrid, m_gridW, *f);
        (void)box;
    }

    // The bit grid — the biggest thing here, because it is the only part that answers which bits a
    // field actually occupies and whether it collides with its neighbour.
    void _paintGrid(jf::JPrimitiveBuffer& buf, float x, float y, float w, const Frame& f) {
        const float cw = std::min(96.f, (w - 52.f) / 8.f), chh = 21.f;
        auto rect = [&buf](float rx, float ry, float rw, float rh, const uint8_t* c) {
            buf.pushRectangle(rx, ry, rw, rh, c, 2.f, 1.f, jf::Colors::Border);
        };
        auto text = [&buf](float tx, float ty, const std::string& s, const uint8_t* c) {
            jf::JTextHelper::pushText(buf, tx, ty, s, c);
        };

        for (int c = 0; c < 8; c++)
            text(x + 50.f + float(c) * cw + 6.f, y, "Bit " + std::to_string(7 - c),
                 jf::Colors::TextSecondary);

        int owner[64];
        for (int& o : owner) o = -1;
        bool clash[64] = {};
        for (size_t i = 0; i < f.fields.size(); i++)
            for (int b = 0; b < f.fields[i].width; b++) {
                const int idx = _bitIndex(f.fields[i], b);
                if (idx < 0 || idx > 63) continue;
                if (owner[idx] >= 0) clash[idx] = true;
                owner[idx] = int(i);
            }

        // One colour per field, so two neighbours are told apart without reading a number.
        static const uint8_t kHue[6][4] = {
            {140, 52, 56, 255}, {56, 112, 64, 255}, {56, 62, 132, 255},
            {124, 116, 48, 255}, {112, 52, 116, 255}, {44, 108, 108, 255} };

        for (int by = 0; by < 8; by++) {
            const float ry = y + 16.f + float(by) * (chh + 2.f);
            text(x, ry + 5.f, "Byte " + std::to_string(by),
                 by < f.dlc ? jf::Colors::TextPrimary : jf::Colors::MutedText);
            for (int c = 0; c < 8; c++) {
                const int idx = by * 8 + (7 - c);
                const float rx = x + 50.f + float(c) * cw;
                const uint8_t* fill = jf::Colors::Surface2;
                if (by >= f.dlc)      fill = jf::Colors::Surface0;
                else if (clash[idx])  fill = jf::Colors::Danger;
                else if (owner[idx] >= 0) fill = kHue[owner[idx] % 6];
                rect(rx, ry, cw - 2.f, chh, fill);
                text(rx + 5.f, ry + 5.f, std::to_string(idx),
                     by < f.dlc ? jf::Colors::TextPrimary : jf::Colors::MutedText);
                if (owner[idx] >= 0 && idx == _bitIndex(f.fields[size_t(owner[idx])], 0))
                    text(rx + 28.f, ry + 5.f, _sigName(f.fields[size_t(owner[idx])].sig).substr(0, 12),
                         jf::Colors::TextPrimary);
            }
        }

        std::string warn;
        for (int i = 0; i < 64; i++) if (clash[i]) { warn = "Two signals share bit " + std::to_string(i); break; }
        if (warn.empty())
            for (const Field& d : f.fields)
                if (_lastBit(d) / 8 >= f.dlc) { warn = _sigName(d.sig) + " runs past the end of the frame"; break; }
        if (!warn.empty()) text(x, y + 16.f + 8.f * (chh + 2.f) + 6.f, warn, jf::Colors::Danger);
    }

    static int _bitIndex(const Field& f, int b) {
        if (f.flags & F_LITTLE) return f.bitOff + b;
        const int d = (7 - (f.bitOff & 7)) + b;
        return ((f.bitOff >> 3) + (d >> 3)) * 8 + (7 - (d & 7));
    }
    static int _lastBit(const Field& f) { return _bitIndex(f, f.width - 1); }

    // ---------------------------------------------------------------------------------------
    // The tune
    // ---------------------------------------------------------------------------------------
    static std::string _fr(int i, const char* f) { return "can.gc_frame[" + std::to_string(i) + "]." + f; }
    static std::string _fl(int i, const char* f) { return "can.gc_field[" + std::to_string(i) + "]." + f; }

    void _load() {
        Cache& c = Cache::instance();
        m_all.clear();
        for (int i = 0; i < kMaxFrames; i++) {
            Frame f;
            const int flags = int(c.configValue(_fr(i, "flags")));
            if (!(flags & M_USED)) continue;
            f.used = true;
            f.tx   = (flags & M_TX)  != 0;
            f.ext  = (flags & M_EXT) != 0;
            f.off  = (flags & M_OFF) != 0;
            f.bus      = int(c.configValue(_fr(i, "bus")));
            f.id       = int(c.configValue(_fr(i, "id")));
            f.dlc      = int(c.configValue(_fr(i, "dlc")));
            f.periodMs = int(c.configValue(_fr(i, "period_ms")));
            f.name     = c.configString(_fr(i, "name"));
            const int first = int(c.configValue(_fr(i, "first_field")));
            const int n     = int(c.configValue(_fr(i, "field_count")));
            for (int k = 0; k < n && first + k < kMaxFields; k++) {
                const int j = first + k;
                Field d;
                d.sig      = int(c.configValue(_fl(j, "sig")));
                d.bitOff   = int(c.configValue(_fl(j, "bit_off")));
                d.width    = int(c.configValue(_fl(j, "width")));
                d.flags    = int(c.configValue(_fl(j, "flags")));
                d.scale    = c.configValue(_fl(j, "scale"));
                d.offset   = c.configValue(_fl(j, "offset"));
                d.ttlMs    = int(c.configValue(_fl(j, "ttl_ms")));
                d.policy   = int(c.configValue(_fl(j, "policy")));
                d.priority = int(c.configValue(_fl(j, "priority")));
                d.sentinel = uint32_t(c.configValue(_fl(j, "sentinel")));
                d.index    = j;
                f.fields.push_back(d);
            }
            m_all.push_back(f);
        }
    }

    // THE WHOLE POOL IS REPACKED on any structural edit. Frames index a contiguous RUN of fields, so
    // inserting one in the middle moves every later run, and an incremental fixup of that is the
    // bookkeeping that ends with two frames sharing a field. Cache drops writes whose value has not
    // changed, so the ECU only sees what actually moved.
    void _commit() {
        if (m_loading) return;
        Cache& c = Cache::instance();
        int fi = 0;
        for (int i = 0; i < kMaxFrames; i++) {
            if (i < int(m_all.size())) {
                Frame& f = m_all[size_t(i)];
                const int flags = M_USED | (f.tx ? M_TX : 0) | (f.ext ? M_EXT : 0) | (f.off ? M_OFF : 0);
                c.setConfigValue(_fr(i, "flags"), flags);
                c.setConfigValue(_fr(i, "bus"), f.bus);
                c.setConfigValue(_fr(i, "id"), f.id);
                c.setConfigValue(_fr(i, "dlc"), f.dlc);
                c.setConfigValue(_fr(i, "period_ms"), f.periodMs);
                c.setConfigValue(_fr(i, "first_field"), fi);
                c.setConfigValue(_fr(i, "field_count"), int(f.fields.size()));
                c.setConfigString(_fr(i, "name"), f.name);
                for (Field& d : f.fields) {
                    if (fi >= kMaxFields) break;
                    c.setConfigValue(_fl(fi, "sig"), d.sig);
                    c.setConfigValue(_fl(fi, "bit_off"), d.bitOff);
                    c.setConfigValue(_fl(fi, "width"), d.width);
                    c.setConfigValue(_fl(fi, "flags"), d.flags);
                    c.setConfigValue(_fl(fi, "scale"), d.scale);
                    c.setConfigValue(_fl(fi, "offset"), d.offset);
                    c.setConfigValue(_fl(fi, "ttl_ms"), d.ttlMs);
                    c.setConfigValue(_fl(fi, "policy"), d.policy);
                    c.setConfigValue(_fl(fi, "priority"), d.priority);
                    c.setConfigValue(_fl(fi, "sentinel"), double(d.sentinel));
                    d.index = fi;
                    fi++;
                }
            } else {
                // A freed slot is CLEARED, not merely unflagged: a frame left behind with a live id
                // would be invisible here and still on the wire if the used bit were ever set again.
                c.setConfigValue(_fr(i, "flags"), 0);
                c.setConfigValue(_fr(i, "id"), 0);
                c.setConfigValue(_fr(i, "field_count"), 0);
            }
        }
        _rebuildLists();
    }

    // ---------------------------------------------------------------------------------------
    // Lists and selection
    // ---------------------------------------------------------------------------------------
    void _rebuildLists() {
        m_view.clear();
        std::vector<std::vector<std::string>> rows;
        for (size_t i = 0; i < m_all.size(); i++) {
            const Frame& f = m_all[i];
            if (f.bus != m_busSel || f.tx != m_tx) continue;   // not this page's
            char id[24];
            std::snprintf(id, sizeof id, f.ext ? "0x%08X" : "0x%03X", unsigned(f.id));
            const std::string nm = (f.off ? "(off) " : "") + f.name;
            if (m_tx) rows.push_back({ id, _rateText(f.periodMs), nm });
            else      rows.push_back({ id, nm });
            m_view.push_back(int(i));
        }
        m_list->setRows(rows);
        _syncFrame();
    }

    // THE GRID IS THE SELECTION. Keeping a second index beside it is what drifted: the grid clamps,
    // the copy did not, and the two disagreed the moment a frame was removed.
    Frame* _frame() {
        const int i = m_list ? m_list->selectedIndex() : -1;
        if (i < 0 || i >= int(m_view.size())) return nullptr;
        return &m_all[size_t(m_view[size_t(i)])];
    }
    const Frame* _frame() const { return const_cast<GenericCanPanel*>(this)->_frame(); }
    int _fieldCount() const { const Frame* f = _frame(); return f ? int(f->fields.size()) : 0; }
    Field* _fieldAt(int k) {
        Frame* f = _frame();
        return (f && k >= 0 && k < int(f->fields.size())) ? &f->fields[size_t(k)] : nullptr;
    }

    void _syncFrame() {
        m_note.clear();
        m_loading = true;
        if (const Frame* f = _frame()) {
            m_on->setCheckState(f->off ? jf::JCheckBox::Unchecked : jf::JCheckBox::Checked);
            m_name->setText(f->name);
            char id[24];
            std::snprintf(id, sizeof id, f->ext ? "0x%08X" : "0x%03X", unsigned(f->id));
            m_id->setText(id);
            m_idType->setCurrentIndex(f->ext ? 1 : 0);
            m_dlc->setCurrentIndex(f->dlc);
            int best = 4;
            for (size_t k = 0; k < kRateMs().size(); k++) if (kRateMs()[k] == f->periodMs) best = int(k);
            m_rate->setCurrentIndex(best);
            for (int k = 0; k < kRows && k < int(f->fields.size()); k++) {
                const Field& d = f->fields[size_t(k)];
                Row& w = m_row[k];
                w.sig->setText(d.sig == kSigNone ? std::string() : _sigName(d.sig));
                w.fmt->setCurrentIndex((d.flags & F_LITTLE) ? 1 : 0);
                w.start->setValue(d.bitOff);
                w.bits->setValue(d.width);
                w.sign->setCurrentIndex((d.flags & F_SIGNED) ? 1 : 0);
                char b[24];
                std::snprintf(b, sizeof b, "%g", d.scale);  w.mult->setText(b);
                std::snprintf(b, sizeof b, "%g", d.offset); w.off->setText(b);
                w.absent->setCurrentIndex(std::clamp(d.policy, 0, 2));
                std::snprintf(b, sizeof b, "%d", d.ttlMs);  w.ttl->setText(b);
                if (d.flags & F_SENTINEL) { std::snprintf(b, sizeof b, "0x%X", unsigned(d.sentinel)); w.noread->setText(b); }
                else                      w.noread->setText("");
            }
        }
        m_loading = false;
    }

    // ---------------------------------------------------------------------------------------
    // Edits
    // ---------------------------------------------------------------------------------------
    void _addFrame() {
        if (int(m_all.size()) >= kMaxFrames) return;
        Frame f;
        f.used = true; f.tx = m_tx; f.bus = m_busSel;
        f.id = 0x100; f.dlc = 8; f.periodMs = 50;
        f.name = m_tx ? "New transmit" : "New receive";
        m_all.push_back(f);
        _commit();
        // Select what was just made — a new row that does not become the selection is one the user
        // has to go and find.
        for (size_t i = 0; i < m_view.size(); i++)
            if (m_view[i] == int(m_all.size()) - 1) { m_list->setSelectedIndex(int(i)); break; }
        _syncFrame();
    }
    // Removes from the list the button belongs to, and only when that list is the selected one — so
    // pressing Remove under Receive can never delete a transmit frame you happened to click earlier.
    // Removes EVERY selected frame in this list — shift picks a range, ctrl picks individuals, and
    // deleting a dozen template frames one press at a time is not a thing anyone should have to do.
    // Erased highest-index first so the earlier indices are still the ones they were.
    void _removeFrame() {
        const std::vector<int> rows = m_list->selectedIndices();
        if (rows.empty()) return;
        // Where the selection should land afterwards: the row that MOVED UP into the first gap.
        // Clearing it instead leaves the editor on "no frame selected" and makes deleting several in
        // a row a click-select-click-select — every other toolkit picks the neighbour, and so does
        // this now. Clamped after the rebuild, so removing the last row selects the new last row.
        const int landing = *std::min_element(rows.begin(), rows.end());

        std::vector<int> pool;
        for (int r : rows)
            if (r >= 0 && r < int(m_view.size())) pool.push_back(m_view[size_t(r)]);
        // Highest index first, so the earlier ones are still the ones they were.
        std::sort(pool.begin(), pool.end(), std::greater<int>());
        for (int i : pool) m_all.erase(m_all.begin() + i);

        _commit();                                   // rebuilds the list
        if (m_view.empty()) m_list->clearSelection();
        else                m_list->setSelectedIndex(std::min(landing, int(m_view.size()) - 1));
        _syncFrame();
    }
    void _addField() {
        Frame* f = _frame();
        if (!f || _usedFields() >= kMaxFields) return;
        if (int(f->fields.size()) >= kRows) {            // the table cannot show another one
            m_note = "This frame already has " + std::to_string(kRows) +
                     " signals \xE2\x80\x94 as many as the table can show.";
            return;
        }
        m_note.clear();
        Field d;
        // Start where the last field ended, which is how a frame is usually filled.
        d.bitOff = f->fields.empty() ? 7
                 : std::min(63, ((_lastBit(f->fields.back()) / 8) + 1) * 8 + 7);
        f->fields.push_back(d);
        _commit();
        _syncFrame();
    }
    void _removeField(int k) {
        Frame* f = _frame();
        if (!f || k < 0 || k >= int(f->fields.size())) return;
        f->fields.erase(f->fields.begin() + k);
        _commit();
        _syncFrame();
    }
    int _usedFields() const {
        int n = 0;
        for (const Frame& f : m_all) n += int(f.fields.size());
        return n;
    }

    void _clampId(Frame& f) const {
        const int limit = f.ext ? 0x1FFFFFFF : 0x7FF;
        f.id = std::max(0, std::min(f.id, limit));
    }
    static int _parseId(const std::string& t) {
        if (t.empty()) return 0;
        const bool hex = t.find('x') != std::string::npos || t.find('X') != std::string::npos;
        return int(std::strtol(t.c_str(), nullptr, hex ? 16 : 10));
    }
    static double _num(const std::string& t, double fallback) {
        if (t.empty()) return fallback;
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        return (end && end != t.c_str()) ? v : fallback;
    }

    // ---------------------------------------------------------------------------------------
    // Signals
    // ---------------------------------------------------------------------------------------
    // A channel's NAME, for the button face and for the bit grid's labels.
    std::string _sigName(int id) const {
        if (id == kSigNone) return kNone;
        if (const MetaModel* m = Cache::instance().meta())
            for (const auto& [name, sid] : m->signalMap())
                if (sid == id) return name;
        return "(" + std::to_string(id) + "?)";
    }
    int _sigIdFor(const std::string& name) const {
        if (const MetaModel* m = Cache::instance().meta()) {
            const auto& sm = m->signalMap();
            const auto it = sm.find(name);
            if (it != sm.end()) return it->second;
        }
        return kSigNone;
    }

    jf::JSceneGraph& g_;
    jf::JRect        m_box{};
    bool             m_hasBox = false;
    std::vector<std::unique_ptr<jf::JWidget>> m_own;   // every control this panel made

    jf::JComboBox *m_idType = nullptr, *m_dlc = nullptr, *m_rate = nullptr;
    jf::JCheckBox *m_on = nullptr;
    jf::JButton   *m_loadTpl = nullptr, *m_dropTpl = nullptr,
                  *m_add = nullptr, *m_del = nullptr, *m_addFld = nullptr;
    jf::JDataGrid *m_list = nullptr;
    jf::JLineEdit *m_name = nullptr, *m_id = nullptr;
    Row            m_row[kRows];

    std::vector<Frame>       m_all;
    std::vector<int>         m_view;           // list row -> index into m_all
    struct Col { float x = 0.f; };
    Col   m_col[10];
    std::vector<Lab> m_lab;
    std::string      m_note;              // why the last action did nothing
    jf::JControl*    m_active = nullptr;   // which control has the keyboard
    int   m_busSel = 0;
    bool  m_tx = true;                    // which direction this page is
    bool  m_configured = false;           // has configure() ever run (see why it must)
    bool  m_loading = false;
    float m_x0 = 0, m_colX = 0, m_gridW = 0;
    float m_yFrm = 0, m_yAdd = 0, m_yTbl = 0, m_yGrid = 0;
};

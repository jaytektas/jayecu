#pragma once

// OutputWizardDialog — give an output slot a personality, and ask only what varies.
//
// An output slot is deliberately generic: a pin, a condition, a value and some timings. That is what
// makes four fuel pumps and eight fans twelve slots rather than a limit somebody chose — and on its own
// it is a blank expression editor, which is no use to anybody who just wants a thermo fan.
//
// So: pick what it IS on the left, answer two or three numbers on the right, press Apply. The template
// writes the conditions, the timings, the fail direction and the kind; the numbers land in the slot's own
// parameter fields, which the conditions REFERENCE — so a fuel pump compiles to the same bytecode
// whatever its prime time, and the wizard still recognises it next time.
//
// Not everything that varies between cars is a NUMBER. A neutral switch is a term or it is nothing, and
// a car without one had no way to say so: the starter's condition named it regardless, an unwired
// channel reads as false, and the output could never come on. So a template may also declare OPTIONS —
// clauses offered as tick boxes, assembled into the condition by templateOptionGroup(). Ticking none
// removes the clause and the switch it named along with it. How they COMBINE is offered too: each row
// past the first carries an and/or button, and assembly is left-to-right and bracketed, so the list
// reads as the meaning.
//
// AND THE POLARITY, which is not part of any of that. Whether a switch reads 1 closed or 1 open is a
// fact about the loom, and it already lives on the sensor as flags.invert. The input rows edit THAT —
// so correcting a brake switch here also corrects the dash, the datalog and every other condition
// reading it, instead of fixing the starter and leaving everything else backwards. It matters more
// than it sounds: this board's digital inputs are pulled UP, so a switch to ground reads 1 when
// nobody is touching it, and an interlock that has never been inverted is inverted.
//
// WHAT IT WROTE IS SHOWN, always. The two conditions are rendered under the parameters as the text they
// will compile from, because a wizard that hides the expression is a wizard you cannot take over from —
// and taking over is the point at which somebody outgrows it. Editing them afterwards is expected: the
// slot is then simply a slot whose conditions came from somewhere.
//
// The templates themselves are DATA in the ECU's meta (definition/ecu.schema.yaml `output_templates`),
// so the list offered is the one the connected firmware can run.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JLineEdit.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/Cache.h"
#include "../model/MetaModel.h"
#include "../model/ExprCompiler.h"
#include "../model/OutputTemplate.h"
#include <j/config/Json.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

class OutputWizardDialog {
public:
    // TALL ENOUGH FOR THE ROWS IT NOW HAS. Two parameters, three options, four input rows and two
    // wrapped conditions do not fit 600 — the preview went under the button box.
    static constexpr uint32_t kW = 880, kH = 720;
    static constexpr float kBtnW = 100.f, kPad = 14.f, kRowH = 46.f;
    static float kBtnH()   { return jf::JStyle::current().buttonHeight; }
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
    static jf::JRect _closeRect() { return jf::JCloseButton::rectFor({0.f, 0.f, float(kW), kHeader()}); }
    static bool _in(const jf::JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    OutputWizardDialog(std::string slotPath, jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_slot(std::move(slotPath))
        , m_window(std::make_unique<PlatformWinType>("Set Up Output", kW, kH, sx, sy,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        // The templates ride in the meta FILE — the same route the trigger-wheel library takes, and
        // for the same reason: what a studio offers should be what the connected firmware can run.
        const MetaModel* meta = Cache::instance().meta();
        if (meta && !meta->path().empty())
            m_templates = outputTemplatesFromMeta(jf::JJson::parseFile(meta->path()));
        m_name = std::make_unique<jf::JLineEdit>(m_graph, "Name");
        // OPEN ON WHAT THE SLOT ALREADY IS, not on whatever happens to be first in the list.
        //
        // This selected row 0 — Fuel Pump — for every slot, and _select only shows a slot's own numbers,
        // name and options when the row it lands on MATCHES the slot. So opening the wizard on a
        // configured starter showed a fuel pump's defaults, and the settings you were looking for
        // appeared only if you thought to click the right row first. Which reads, correctly, as the
        // wizard resetting your slot every time you open it — and pressing OK would have done exactly
        // that. The recognition to answer it already existed; nothing asked it before the first paint.
        int start = 0;
        for (size_t i = 0; i < m_templates.size(); ++i) {
            TemplateChoice c = templateDefaultChoice(m_templates[i]);
            if (_slotMatches(m_templates[i], c)) { start = static_cast<int>(i); break; }
        }
        _select(start);
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", jf::JDialogButtonBox::Role::Reject, kBtnW);
        m_box->addButton("OK",     jf::JDialogButtonBox::Role::Accept, kBtnW);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _apply(); m_done = true; });
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress();
        const bool held    = m_window->isLeftButtonDown();

        const JRect closeR = _closeRect();
        if (pressed && _in(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // Two columns: the personalities on the left, the chosen one's questions on the right.
        const float listY = kHeader() + kPad + 22.f;
        const float listH = float(kH) - listY - (kBtnH() + 2.f * kPad);
        m_listH = listH;
        m_listScroll = std::clamp(m_listScroll, 0.f, _listMaxScroll());
        const float colX  = kListW + 2.f * kPad;
        m_box->setBounds({ kPad, float(kH) - kPad - kBtnH(), float(kW) - 2.f * kPad, kBtnH() });
        m_name->setBounds({ colX + 110.f, listY + 4.f, 200.f, kBtnH() });
        for (size_t i = 0; i < m_spin.size(); ++i)
            m_spin[i]->setBounds({ colX + 190.f, _paramY(i), 130.f, kBtnH() });
        const float chkH = jf::JStyle::current().checkHeight;
        for (size_t i = 0; i < m_opt.size(); ++i) {
            m_opt[i]->setBounds({ colX + kJoinW + 12.f, _optionY(i),
                                  float(kW) - colX - kPad - kJoinW - 12.f, chkH });
            // The connective belongs to the row it attaches, so row 0 has none — and a row whose
            // option is not ticked has nothing to attach either.
            if (m_join[i]) m_join[i]->setBounds({ colX + 4.f, _optionY(i), kJoinW, chkH });
        }
        const std::vector<size_t> pol = _activePol();
        for (size_t r = 0; r < pol.size(); ++r)
            m_pol[pol[r]]->setBounds({ colX + 330.f, _inputY(r), 120.f, chkH });

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
            if (ke.key == JKeyEvent::JKey::Down) { _select(std::min(m_sel + 1, int(m_templates.size()) - 1)); continue; }
            if (ke.key == JKeyEvent::JKey::Up)   { _select(std::max(m_sel - 1, 0)); continue; }
            // Tab walks the fields, name first — the order they are asked for. Without it the only way
            // to reach the third parameter is the mouse, in a dialog whose whole content is typing.
            if (jf::jIsTabNav(ke)) { _focusNext(jf::jTabNavDir(ke)); continue; }
            // KEYS GO TO WHAT WAS LAST CLICKED, and this dialog has to remember that itself.
            //
            // It used to ask each spin box whether it isFocused() and fall back to the name field when
            // none said yes — and in a popup window with its own scene graph, none ever does: focus is
            // granted by the window's focus chain, which a bespoke dialog like this is not part of. So
            // the fallback was not a fallback, it was the only path, and every digit typed into a
            // parameter landed in the name box.
            //
            // The canvas solved this years ago by tracking its own active control; so does this now.
            if (m_kbd == m_name.get() || (m_kbd == nullptr)) m_name->handleKeyEvent(ke);
            else m_kbd->handleKeyEvent(ke);   // Space on a focused tick box toggles it (JControl::activate)
        }

        m_name->handleMouseMove(mx, my);
        for (auto& s : m_spin) s->handleMouseMove(mx, my);
        for (auto& o : m_opt)  o->handleMouseMove(mx, my);
        for (auto& j : m_join) if (j) j->handleMouseMove(mx, my);
        for (size_t i : _activePol()) m_pol[i]->handleMouseMove(mx, my);
        m_box->handleMouseMove(mx, my);
        const bool overList = mx >= kPad && mx < kListW + kPad && my >= listY && my < listY + listH;
        const auto thumb = _listThumb();
        const bool overBar = overList && thumb.second > 0.f && mx >= kPad + kListW - kBarW - 2.f;
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f && overList)
            m_listScroll = std::clamp(m_listScroll - wheel * kRowH, 0.f, _listMaxScroll());
        if (m_barDrag) {
            if (!held) m_barDrag = false;
            else if (m_listH - thumb.second > 0.f)
                m_listScroll = std::clamp((my - listY - m_barGrab) / (m_listH - thumb.second) * _listMaxScroll(),
                                          0.f, _listMaxScroll());
        }
        if (pressed) {
            if (overBar) {
                // On the thumb: grab it where it was taken. On the track: jump the thumb there, then drag.
                const float ty = listY + thumb.first;
                m_barGrab = (my >= ty && my < ty + thumb.second) ? my - ty : thumb.second * 0.5f;
                m_barDrag = true;
                if (!(my >= ty && my < ty + thumb.second) && m_listH - thumb.second > 0.f)
                    m_listScroll = std::clamp((my - listY - m_barGrab) / (m_listH - thumb.second) * _listMaxScroll(),
                                              0.f, _listMaxScroll());
            } else if (overList) {
                const int i = int((my - listY + m_listScroll) / kRowH);
                if (i >= 0 && i < int(m_templates.size())) _select(i);
            } else {
                // The press decides where the keyboard is. Hit-test first, then deliver: a control that
                // was not clicked must lose the caret, or two of them draw one.
                jf::JControl* hit = nullptr;
                if (m_name->hitTest(mx, my)) hit = m_name.get();
                for (auto& s : m_spin) if (s->hitTest(mx, my)) hit = s.get();
                for (auto& o : m_opt)  if (o->hitTest(mx, my)) hit = o.get();
                for (auto& j : m_join) if (j && j->hitTest(mx, my)) hit = j.get();
                for (size_t i : _activePol()) if (m_pol[i]->hitTest(mx, my)) hit = m_pol[i].get();
                if (hit) _focus(hit);
                m_name->handleMousePress(mx, my);
                for (auto& s : m_spin) s->handleMousePress(mx, my);
                for (auto& o : m_opt)  o->handleMousePress(mx, my);
                for (auto& j : m_join) if (j) j->handleMousePress(mx, my);
                for (size_t i : _activePol()) m_pol[i]->handleMousePress(mx, my);
                m_box->handleMousePress(mx, my);
            }
        }
        if (m_window->consumeRelease()) {
            m_name->handleMouseRelease(mx, my);
            for (auto& s : m_spin) s->handleMouseRelease(mx, my);
            for (auto& o : m_opt)  o->handleMouseRelease(mx, my);
            for (auto& j : m_join) if (j) j->handleMouseRelease(mx, my);
            for (size_t i : _activePol()) m_pol[i]->handleMouseRelease(mx, my);
            m_box->handleMouseRelease(mx, my);
        }
        if (m_done) return false;

        _render(buf, mx, my, listY, listH, colX);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    static constexpr float kListW = 250.f;

    // WHERE THE SETTINGS START: below the name row and below the DETAIL, measured rather than
    // guessed. It was a constant 96px, which is one particular paragraph's height — so a template
    // whose detail ran to four lines had its first parameter drawn straight through the text. The
    // bounds are set before the render pass, so the height is measured once when the template is
    // chosen (it depends on nothing else) and read from here.
    float _settingsTopY() const {
        return kHeader() + kPad + 22.f + 4.f + kBtnH() + 10.f + m_detailH + 10.f;
    }
    float _paramY(size_t i) const { return _settingsTopY() + float(i) * 40.f; }
    // The tick boxes sit under the numbers, on their own tighter pitch — they are one line each.
    float _optionY(size_t i) const { return _paramY(m_spin.size()) + 4.f + float(i) * kRowPitch; }
    // Where the options end, whichever of the two the template actually has.
    float _optionsEndY() const {
        return m_opt.empty() ? _paramY(m_spin.size()) : _optionY(m_opt.size());
    }
    // The wiring rows: one per input this choice actually names, under their own heading.
    float _inputsHeadY() const { return _optionsEndY() + 12.f; }
    float _inputY(size_t r) const {
        return _inputsHeadY() + jf::JTextHelper::lineHeight() + 4.f + float(r) * kRowPitch;
    }

    // WHAT IS CHOSEN NOW — the ticks and the connectives together, which is what every condition,
    // input list and write is assembled from.
    TemplateChoice _choice() const {
        TemplateChoice c;
        for (size_t i = 0; i < m_opt.size(); ++i) {
            c.on.push_back(m_opt[i]->isChecked());
            c.join.push_back(m_joinIsOr[i] ? "or" : "and");
        }
        return c;
    }

    // Which polarity rows are live: the inputs this choice names, as indices into m_polIds. The
    // controls are built once per template and SELECTED here rather than rebuilt on every tick, so a
    // box does not lose the value you just set it to when a neighbouring option is ticked.
    std::vector<size_t> _activePol() const {
        std::vector<size_t> out;
        if (m_templates.empty()) return out;
        for (const std::string& sid : templateInputs(m_templates[m_sel], _choice()))
            for (size_t i = 0; i < m_polIds.size(); ++i)
                if (m_polIds[i] == sid) { out.push_back(i); break; }
        return out;
    }

    static std::string _polPath(const std::string& sid) {
        return "sensors.sensor[" + sid + "].flags.invert";
    }

    // Choosing a personality builds its questions. Where the slot ALREADY holds this template's
    // parameters (it was set up with it before), those numbers are what the boxes come up with —
    // re-opening the wizard on a fan should offer its temperatures, not the shipped defaults.
    float _listContentH() const { return float(m_templates.size()) * kRowH; }
    float _listMaxScroll() const { return std::max(0.f, _listContentH() - m_listH); }
    // The scrollbar's thumb, when the list is longer than its box: {y, height} inside the list.
    std::pair<float, float> _listThumb() const {
        const float content = _listContentH();
        if (m_listH <= 0.f || content <= m_listH) return { 0.f, 0.f };
        const float th = std::max(24.f, m_listH * m_listH / content);
        return { (m_listH - th) * (m_listScroll / _listMaxScroll()), th };
    }
    static constexpr float kBarW = 8.f;

    void _select(int i) {
        if (m_templates.empty()) return;
        m_sel = std::clamp(i, 0, int(m_templates.size()) - 1);
        // Keep the chosen row in view (arrow keys walk past the bottom of the box).
        if (m_listH > 0.f) {
            const float top = m_sel * kRowH, bot = top + kRowH;
            if (top < m_listScroll)               m_listScroll = top;
            if (bot > m_listScroll + m_listH)     m_listScroll = bot - m_listH;
            m_listScroll = std::clamp(m_listScroll, 0.f, _listMaxScroll());
        }
        const OutputTemplate& t = m_templates[m_sel];
        m_spin.clear();
        m_opt.clear();
        static const char* kField[4] = { "param_a", "param_b", "param_c", "param_d" };
        // WHICH OPTIONS the slot is actually running, recovered from its own program, so reopening the
        // wizard on a starter set up without a clutch switch does not silently offer to add one back.
        m_detailH = _wrapHeight(t.detail, float(kW) - (kListW + 2.f * kPad) - kPad);
        TemplateChoice chosen = templateDefaultChoice(t);
        const bool same = _slotMatches(t, chosen);
        for (size_t k = 0; k < t.params.size(); ++k) {
            // The box is built from the parameter's own declaration — its range, its decimals, and a
            // step that matches them (a whole-number temperature steps by one, a prime time in tenths
            // by 0.1). The units are drawn beside it: the control has no suffix of its own.
            const double step = (t.params[k].digits <= 0) ? 1.0
                              : (t.params[k].digits == 1) ? 0.1 : 0.01;
            auto sb = std::make_unique<jf::JDoubleSpinBox>(m_graph, t.params[k].min, t.params[k].max,
                                                           step, t.params[k].digits, 130.f);
            // …and RAW OUT, ENGINEERING BACK IN on the way here. The 0.001 this used to multiply by was
            // param_a's scale written as a constant — right for the parameters and wrong the moment one
            // of them is a field with any other scale.
            const std::string pf = m_slot + "." + kField[k];
            const double sc = Cache::instance().configScale(pf);
            const double live = Cache::instance().configValue(pf) * (sc != 0.0 ? sc : 1.0);
            sb->setValue(same ? live : t.params[k].def);
            m_spin.push_back(std::move(sb));
        }
        m_join.clear();
        m_joinIsOr.clear();
        m_polIds.clear();
        m_pol.clear();
        for (size_t k = 0; k < t.options.size(); ++k) {
            auto cb = std::make_unique<jf::JCheckBox>(m_graph, t.options[k].label, 300.f);
            cb->setChecked(chosen.ticked(k));
            m_opt.push_back(std::move(cb));
            m_joinIsOr.push_back(k < chosen.join.size() && chosen.join[k] == "or");
            // A BUTTON, NOT A DROP-DOWN: two values, and this dialog's own scene graph is not in the
            // application's focus chain, so a control that needs a popup has nowhere to put it.
            if (k == 0) { m_join.push_back(nullptr); continue; }
            auto btn = std::make_unique<jf::JButton>(m_graph, m_joinIsOr[k] ? "or" : "and");
            jf::JButton* raw = btn.get();
            btn->onClicked.connect([this, k, raw] {
                m_joinIsOr[k] = !m_joinIsOr[k];
                raw->setLabel(m_joinIsOr[k] ? "or" : "and");
            });
            m_join.push_back(std::move(btn));
        }
        // ONE POLARITY BOX PER INPUT THE TEMPLATE COULD NAME, ticked from the sensor's own flag. Built
        // for all of them, shown for the ones in play — see _activePol().
        for (const std::string& sid : templateInputs(t, TemplateChoice{
                 std::vector<bool>(t.options.size(), true), {} })) {
            m_polIds.push_back(sid);
            auto cb = std::make_unique<jf::JCheckBox>(m_graph, "inverted", 120.f);
            cb->setChecked(Cache::instance().configValue(_polPath(sid)) != 0.0);
            m_pol.push_back(std::move(cb));
        }
        if (m_name) m_name->setText(same ? Cache::instance().configString(m_slot + ".name") : t.name);
    }

    // Does the slot already hold THIS template, and WITH WHICH OPTIONS? Compared on the compiled
    // conditions, which is the same test the page's picker uses — and the reason the parameters are
    // referenced rather than baked.
    //
    // The choice is part of the answer, not an input to it. Three tick boxes and their connectives
    // make fourteen possible programs from one template, so recognising the template means finding
    // which of them the slot holds; `chosen` comes back as that choice and is left alone when nothing
    // matches. templateChoiceCandidates() puts the defaults first, so a template whose options happen
    // to compile to the same bytecode reports the choice the wizard would itself have written rather
    // than whichever the loop reached first.
    bool _slotMatches(const OutputTemplate& t, TemplateChoice& chosen) const {
        const MetaModel* meta = Cache::instance().meta();
        if (!meta) return false;
        int off = 0, size = 0;
        if (!meta->resolveBlob(m_slot + ".on_expr", off, size)) return false;
        const std::vector<uint8_t> have = Cache::instance().configBlob(m_slot + ".on_expr");

        for (const TemplateChoice& c : templateChoiceCandidates(t)) {
            const std::string src = templateSource(t, t.onWhen, m_slot, c);
            ExprCompiler::Result r = ExprCompiler::compile(src, *meta,
                                                           uint32_t(Cache::instance().configImage().size()),
                                                           uint16_t(size));
            if (!r.ok) continue;
            // THE BLOCK IS FIXED AND THE PROGRAM IS NOT: everything past the END must be zero for the
            // stored program to BE this one, rather than this one with something else after it.
            if (have.size() < r.code.size()) continue;
            bool tail_clean = true;
            for (size_t k = r.code.size(); k < have.size(); ++k) if (have[k]) { tail_clean = false; break; }
            if (!tail_clean) continue;
            if (!std::equal(r.code.begin(), r.code.end(), have.begin())) continue;
            chosen = c;
            return true;
        }
        return false;
    }

    void _apply() {
        if (m_templates.empty()) return;
        const OutputTemplate& t = m_templates[m_sel];
        std::vector<double> vals;
        for (auto& s : m_spin) vals.push_back(s->value());
        const TemplateWrites w = templateWrites(t, m_slot, vals, _choice());

        Cache& c = Cache::instance();
        for (const auto& [path, v] : w.values) c.setConfigValue(path, v);   // already RAW: see templateWrites
        c.setConfigString(m_slot + ".name", m_name ? m_name->text() : w.name);
        // THE SENSORS' OWN POLARITY, for the inputs this choice actually uses. Only those: writing the
        // flag of an option the user declined would be editing a switch the dialog never showed them.
        for (size_t i : _activePol())
            c.setConfigValue(_polPath(m_polIds[i]), m_pol[i]->isChecked() ? 1.0 : 0.0);

        const MetaModel* meta = c.meta();
        if (!meta) return;
        auto writeExpr = [&](const char* field, const std::string& src) {
            int off = 0, size = 0;
            if (!meta->resolveBlob(m_slot + "." + field, off, size)) return;
            ExprCompiler::Result r = ExprCompiler::compile(src, *meta,
                                                           uint32_t(c.configImage().size()),
                                                           uint16_t(size));
            // A template that will not compile writes NOTHING for that condition rather than half a
            // program: an output holding one of the two conditions is worse than one holding neither.
            if (r.ok) c.setConfigBlob(m_slot + "." + field, r.code);
        };
        writeExpr("on_expr",  w.onSource);
        writeExpr("off_expr", w.offSource);
        if (!w.freqSource.empty()) writeExpr("freq_expr", w.freqSource);
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my, float listY, float listH, float colX) {
        using namespace jf;
        buf.clear();
        const float W = float(kW), H = float(kH), lh = JTextHelper::lineHeight();
        const float rc = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, rc, 1.f, Colors::Border);
        const std::string title = "Set Up " + _slotName();
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), title, rc, 1, 0.f, _closeRect().width + 14.f);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _in(cr, mx, my));

        if (JTextHelper::hasAtlas()) {
            JTextHelper::pushText(buf, kPad, kHeader() + kPad, "What is it?", Colors::TextSecondary);
            JTextHelper::pushText(buf, colX, kHeader() + kPad, "Its settings", Colors::TextSecondary);
        }

        // ---- the personalities ----
        buf.pushRectangle(kPad, listY, kListW, listH, Colors::Surface0, 4.f, 1.f, Colors::Border);
        buf.pushClip(kPad, listY, kListW, listH);
        const auto thumb = _listThumb();
        const float textW = kListW - 24.f - (thumb.second > 0.f ? kBarW + 4.f : 0.f);
        for (int i = 0; i < int(m_templates.size()); ++i) {
            const float ry = listY + i * kRowH - m_listScroll;
            if (ry + kRowH < listY) continue;
            if (ry > listY + listH) break;
            static const uint8_t kSelFg[4] = { 255, 255, 255, 255 };
            if (i == m_sel) buf.pushRectangle(kPad, ry, kListW, kRowH, Colors::Accent, 0.f);
            const uint8_t* fg = (i == m_sel) ? kSelFg : Colors::TextPrimary;
            if (JTextHelper::hasAtlas()) {
                JTextHelper::pushText(buf, kPad + 10.f, ry + 6.f, m_templates[i].name, fg);
                JTextHelper::pushText(buf, kPad + 10.f, ry + 6.f + lh,
                                      _clip(m_templates[i].blurb, textW),
                                      (i == m_sel) ? fg : Colors::TextSecondary);
            }
        }
        if (thumb.second > 0.f) {
            const float bx = kPad + kListW - kBarW - 2.f;
            buf.pushRectangle(bx, listY + 2.f, kBarW, listH - 4.f, Colors::Surface1, kBarW * 0.5f);
            buf.pushRectangle(bx, listY + thumb.first, kBarW, thumb.second,
                              m_barDrag ? Colors::Accent : Colors::Border, kBarW * 0.5f);
        }
        buf.popClip();

        if (m_templates.empty()) {
            if (JTextHelper::hasAtlas())
                JTextHelper::pushText(buf, colX, listY + 8.f,
                                      "This firmware ships no output templates.", Colors::TextSecondary);
            if (m_box) m_box->populateRenderPrimitives(buf);
            return;
        }

        // ---- the chosen one ----
        const OutputTemplate& t = m_templates[m_sel];
        float y = listY + 4.f;
        if (JTextHelper::hasAtlas()) JTextHelper::pushText(buf, colX, y + 6.f, "Name", Colors::TextPrimary);
        m_name->populateRenderPrimitives(buf);
        y += kBtnH() + 10.f;
        if (JTextHelper::hasAtlas())
            y = _wrapped(&buf, colX, y, W - colX - kPad, t.detail, Colors::TextSecondary) + 6.f;

        for (size_t k = 0; k < m_spin.size(); ++k) {
            const float py = _paramY(k);
            if (JTextHelper::hasAtlas()) {
                JTextHelper::pushText(buf, colX, py + 4.f, t.params[k].label, Colors::TextPrimary);
                if (!t.params[k].units.empty())
                    JTextHelper::pushText(buf, colX + 190.f + 138.f, py + 4.f, t.params[k].units,
                                          Colors::TextSecondary);
            }
            m_spin[k]->populateRenderPrimitives(buf);
        }

        // ---- the optional clauses ----
        // Drawn as what they are: a heading the template supplies, a tick box per term, and the
        // connective that attaches each row to the ones above. The condition below re-renders as they
        // change, which is the whole feedback loop — you watch the interlock enter and leave the
        // program, in the grouping it will actually have, before pressing OK.
        if (!m_opt.empty() && JTextHelper::hasAtlas() && !t.optionsLabel.empty())
            JTextHelper::pushText(buf, colX, _optionY(0) - lh - 2.f, t.optionsLabel, Colors::TextSecondary);
        for (size_t i = 0; i < m_opt.size(); ++i) {
            m_opt[i]->populateRenderPrimitives(buf);
            // The connective is only real once there is something before it to attach TO: shown greyed
            // by absence rather than drawn as a live button that changes nothing.
            if (!m_join[i]) continue;
            bool any_before = false;
            for (size_t k = 0; k < i; ++k) if (m_opt[k]->isChecked()) any_before = true;
            if (any_before && m_opt[i]->isChecked()) m_join[i]->populateRenderPrimitives(buf);
        }

        // ---- the switches it will read, and how they are wired ----
        // A condition that reads an unwired switch reads it as FALSE, which for an interlock is an
        // output that can never come on — so the wiring is stated here rather than discovered later.
        // Only the switches this CHOICE uses: an option nobody ticked names no channel, and warning
        // about it would be telling the user off for a decision they made.
        //
        // The polarity box beside each is the SENSOR's invert flag, not a variation on the condition.
        // Same switch, same answer, everywhere it is read.
        const std::vector<size_t> pol = _activePol();
        if (!pol.empty() && JTextHelper::hasAtlas())
            JTextHelper::pushText(buf, colX, _inputsHeadY(), "Switches it reads", Colors::TextSecondary);
        for (size_t r = 0; r < pol.size(); ++r) {
            const std::string& sid = m_polIds[pol[r]];
            const std::string sp = "sensors.sensor[" + sid + "]";
            const bool on     = Cache::instance().configValue(sp + ".enabled") != 0.0;
            const bool pinned = Cache::instance().configValue(sp + ".source") < 250.0;
            const bool ok     = on && pinned;
            if (JTextHelper::hasAtlas())
                JTextHelper::pushText(buf, colX + 4.f, _inputY(r) + 3.f,
                                      sid + (ok ? "  \xE2\x80\x94 wired" : "  \xE2\x80\x94 NOT WIRED"),
                                      ok ? Colors::TextSecondary : Colors::Warning);
            // The polarity of a switch that is not connected to anything says nothing, so it is not
            // offered — one fewer control claiming to mean something.
            if (ok) m_pol[pol[r]]->populateRenderPrimitives(buf);
        }

        // WHAT IT WILL RUN. Not decoration: this is the line somebody reads when the fan does something
        // they did not expect, and the thing they edit when they outgrow the template.
        float cy = (pol.empty() ? _optionsEndY() : _inputY(pol.size())) + 12.f;
        if (JTextHelper::hasAtlas()) {
            JTextHelper::pushText(buf, colX, cy, "It will run", Colors::TextSecondary);
            cy += lh + 4.f;
            const float tw = W - colX - kPad;
            cy = _wrapped(&buf, colX, cy, tw, "on:  " + _pretty(t, t.onWhen),  Colors::TextPrimary);
            cy = _wrapped(&buf, colX, cy, tw, "off: " + _pretty(t, t.offWhen), Colors::TextPrimary);
        }
        if (m_box) m_box->populateRenderPrimitives(buf);
    }

    // The template's source with its {tokens} shown as the numbers in the boxes and the option group
    // as the terms that are ticked — what the compiled program will actually test, rather than a path
    // nobody can read.
    std::string _pretty(const OutputTemplate& t, const std::string& src) const {
        std::string out;
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] != '{') { out += src[i]; continue; }
            const size_t close = src.find('}', i);
            if (close == std::string::npos) { out += src[i]; continue; }
            const std::string token = src.substr(i + 1, close - i - 1);
            if (token == "@options") {
                out += templateOptionGroup(t, _choice());
                i = close;
                continue;
            }
            const int idx = t.indexOf(token);
            if (idx >= 0 && idx < int(m_spin.size())) {
                char b[32];
                std::snprintf(b, sizeof(b), "%.*f", t.params[idx].digits, m_spin[idx]->value());
                out += b;
            } else {
                out += src.substr(i, close - i + 1);
            }
            i = close;
        }
        return out;
    }

    std::string _slotName() const {
        const auto lb = m_slot.find('['), rb = m_slot.find(']');
        if (lb == std::string::npos || rb == std::string::npos) return "Output";
        const int idx = std::atoi(m_slot.substr(lb + 1, rb - lb - 1).c_str());
        // THE ROW IS A PIN, so it is named as one ("LS7") when the definition names its elements.
        if (const MetaModel* m = Cache::instance().meta()) {
            const auto it = m->configArrays().find(m_slot.substr(0, lb));
            if (it != m->configArrays().end() && idx >= 0 && idx < int(it->second.elementLabels.size()))
                return it->second.elementLabels[idx];
        }
        return "Output " + std::to_string(idx + 1);
    }

    static std::string _clip(const std::string& s, float w) {
        if (!jf::JTextHelper::hasAtlas() || jf::JTextHelper::measureWidth(s) <= w) return s;
        std::string out = s;
        while (out.size() > 4 && jf::JTextHelper::measureWidth(out + "\xE2\x80\xA6") > w) out.pop_back();
        return out + "\xE2\x80\xA6";
    }

    // How tall that paragraph will be, by the SAME wrapping rule — one function short of two, so the
    // measurement and the drawing cannot disagree about where the text ends.
    static float _wrapHeight(const std::string& text, float w) {
        return _wrapped(nullptr, 0.f, 0.f, w, text, nullptr);
    }

    // Word-wrapped paragraph; returns the y below it.
    // A null buffer MEASURES instead of drawing — see _wrapHeight. The y it returns is the same
    // either way, which is the only reason one function can serve both.
    static float _wrapped(jf::JPrimitiveBuffer* buf, float x, float y, float w,
                          const std::string& text, const uint8_t* fg) {
        const float lh = jf::JTextHelper::lineHeight();
        std::string line;
        size_t i = 0;
        while (i <= text.size()) {
            const size_t sp = text.find(' ', i);
            const std::string word = text.substr(i, sp == std::string::npos ? sp : sp - i);
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && jf::JTextHelper::measureWidth(next) > w) {
                if (buf) jf::JTextHelper::pushText(*buf, x, y, line, fg);
                y += lh;
                line = word;
            } else line = next;
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        if (!line.empty()) {
            if (buf) jf::JTextHelper::pushText(*buf, x, y, line, fg);
            y += lh;
        }
        return y;
    }

    std::string m_slot;
    std::vector<OutputTemplate> m_templates;
    int   m_sel{0};
    bool  m_done{false}, m_drag{false};
    // THE LIST SCROLLS. It is as long as the templates the firmware ships, and past the height of the
    // dialog the rest were clipped with no way to reach them. Wheel over the list, drag its bar, or arrow
    // down: the selection is kept in view.
    float m_listScroll{0.f}, m_listH{0.f}, m_barGrab{0.f};
    bool  m_barDrag{false};
    float m_ax{0}, m_ay{0};
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    // The control the keyboard belongs to — this dialog's own focus, because a popup window's widgets
    // are not in the application's focus chain. Null until something is clicked, and the name field is
    // the sensible default because naming the output is the first thing a wizard asks for.
    jf::JControl* m_kbd = nullptr;
    // The fields in the order the dialog asks for them.
    std::vector<jf::JControl*> _fields() {
        std::vector<jf::JControl*> v{ m_name.get() };
        for (auto& s : m_spin) v.push_back(s.get());
        for (size_t i = 0; i < m_opt.size(); ++i) {
            if (m_join[i]) v.push_back(m_join[i].get());
            v.push_back(m_opt[i].get());
        }
        for (size_t i : _activePol()) v.push_back(m_pol[i].get());
        return v;
    }
    void _focusNext(int dir) {
        const auto v = _fields();
        if (v.empty()) return;
        int at = 0;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == m_kbd) { at = int(i); break; }
        const int n = int(v.size());
        _focus(v[((at + dir) % n + n) % n]);
    }
    void _focus(jf::JControl* c) {
        if (m_kbd == c) return;
        if (m_kbd) { m_kbd->endEdit(); m_kbd->setFocused(false); }
        m_kbd = c;
        if (m_kbd) { m_kbd->setFocused(true); m_kbd->beginEdit(); }
    }

    std::unique_ptr<jf::JLineEdit> m_name;
    std::vector<std::unique_ptr<jf::JDoubleSpinBox>> m_spin;
    static constexpr float kRowPitch = 26.f, kJoinW = 44.f;
    // One per optional clause, in template order — the tick boxes _choice() reads.
    std::vector<std::unique_ptr<jf::JCheckBox>> m_opt;
    // The and/or button per clause (null for the first, which attaches to nothing) and its state. The
    // state is held here rather than parsed back off the button's label, which is presentation.
    std::vector<std::unique_ptr<jf::JButton>> m_join;
    std::vector<bool> m_joinIsOr;
    // The invert flag of every input the template could name, and the sensor ids they belong to.
    std::vector<std::string> m_polIds;
    std::vector<std::unique_ptr<jf::JCheckBox>> m_pol;
    // The chosen template's detail paragraph, as tall as it wraps — what the settings sit below.
    float m_detailH{0.f};
    std::unique_ptr<jf::JDialogButtonBox> m_box;
};

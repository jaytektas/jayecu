#pragma once

// LuaScriptEditor — THE Lua editor. It is mounted by the `script` element on the Lua Scripting page
// (ScriptWidget). It was written as a dock and briefly was both; the dock is gone, but the split stays —
// this class owns the RULES (which field, how big, when it is sent) and knows nothing about where it is
// mounted, so a second mount costs nothing and a second set of rules never appears.
//
// A toolbar (Apply to ECU / Reload from ECU + a character budget) over a syntax-highlighted text area.
//
// IT DOES NOT PUSH PER KEYSTROKE. Every other bound control writes the tune as you type; a script must
// not, because the ECU live-reloads what it is given and half a line of Lua is a syntax error in a
// running engine. Typing marks the buffer dirty and nothing more — Apply sends it.
//
// IT ASKS THE ECU WHAT ITS SCRIPT FIELD IS CALLED. jayecu's is `lua.source`, rusEFI's is `ts.luaScript`
// and eight times the size; Cache::scriptPath() / scriptCapacity() answer from whichever definition is
// loaded, and both are re-asked on configLoaded — connecting to the other ECU mid-session re-points the
// editor rather than leaving it reading a path that does not resolve.
//
// It is a jf::JControl, not a plain JWidget, so the canvas can host it exactly like any other control
// (HostedControlWidget paints it and forwards the run-mode mouse and keyboard to it).

#include "LuaHighlighter.h"
#include "../model/Cache.h"

#include <j/core/JControl.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JButton.h>
#include <j/core/JTextArea.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

class LuaScriptEditor : public jf::JControl {
public:
    // What just happened, in the host's own voice: the dock's host puts it in the status bar. Optional —
    // the editor works without one, it just says nothing.
    std::function<void(const std::string&)> onStatus;

    explicit LuaScriptEditor(jf::JSceneGraph& g)
        : jf::JControl(g, "LuaScriptEditor"),
          apply_(g, "Apply to ECU", kApplyW, kBtnH),
          reload_(g, "Reload from ECU", kReloadW, kBtnH),
          edit_(g, "-- Lua script") {
        addChild(&apply_); addChild(&reload_); addChild(&edit_);
        apply_.onClicked.connect([this]  { applyToEcu(); });
        reload_.onClicked.connect([this] { reloadFromEcu(); });
        edit_.onTextChanged.connect([this](const std::string&) { if (!loading_) dirty_ = true; });
        // Syntax colours + the error line the ECU is reporting, which is why the highlighter is bound
        // here and not left to the host: both mounts need the same one.
        edit_.setHighlighter([this](const std::string& src, std::vector<uint8_t>& out) {
            LuaHighlighter::highlight(src, out);
            tintErrorLine_(src, out);
        });
        rebind();
        // A NEW DEFINITION MAY BE A DIFFERENT FIRMWARE. JWidget is a JSlotTracker, so this disconnects
        // itself when the editor dies — a page's editor is destroyed with its panel, and a live slot
        // into a dead one would fire on the next connect.
        addConnection(Cache::instance().configLoaded.connect([this] { rebind(); }));
    }

    jf::JTextArea& editor() { return edit_; }
    bool           dirty() const { return dirty_; }      // an unsent edit is in the buffer

    // Send the buffer to the ECU. Writes the resolved field and flushes immediately rather than waiting
    // on the write debounce, because the user just asked for it to be running.
    void applyToEcu() {
        const std::string path = Cache::instance().scriptPath();
        // NOT A MISSING FEATURE — A REFUSAL TO CLAIM ONE. Every firmware the studio talks to has a script
        // field, so this should never fire. What it prevents if it ever does is the failure it replaces:
        // an unresolvable path makes setConfigString return WITHOUT writing, and the lines below would
        // then clear the dirty flag and report success — the edit silently lost, the status bar saying it
        // landed.
        if (path.empty()) { say_("No Lua script field in this ECU's definition"); return; }
        Cache::instance().setConfigString(path, edit_.text());
        Cache::instance().flushWrites();
        dirty_ = false;
        say_("Lua script applied \xE2\x80\x94 the ECU live-reloads it");
    }

    // Throw the local edits away and show what the ECU actually holds.
    void reloadFromEcu() {
        dirty_ = false;
        show_(Cache::instance().configString(Cache::instance().scriptPath()));
        say_("Lua script reloaded from the ECU image");
    }

    // Re-ask the definition which field holds the script and how big it is, and re-read it. Called on
    // every configLoaded; safe to call at any time. An unsent edit is never overwritten.
    void rebind() {
        edit_.setMaxLength(Cache::instance().scriptCapacity());
        if (dirty_) return;
        show_(Cache::instance().configString(Cache::instance().scriptPath()));
    }

    // Pick up the line the ECU is failing on (lua_state 2/3 carry lua_error_line). Edge-triggered: the
    // re-highlight only runs when the line actually moves. Called from the host's housekeeping tick and
    // from the canvas widget's per-frame sync — both mounts, one rule.
    void pollEcuState() {
        const Cache& c = Cache::instance();
        const int st   = static_cast<int>(c.value("lua_state"));
        const int line = (st == 2 || st == 3) ? static_cast<int>(c.value("lua_error_line")) : 0;
        if (line == errLine_) return;
        errLine_ = line;
        edit_.refreshHighlight();   // the text has not changed, only what the colours mean
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, kBarH, jf::Colors::Surface1);          // toolbar strip
        apply_.setBounds({ b.x + 4.f, b.y + (kBarH - kBtnH) * 0.5f, kApplyW, kBtnH });
        reload_.setBounds({ b.x + 4.f + kApplyW + 6.f, b.y + (kBarH - kBtnH) * 0.5f, kReloadW, kBtnH });
        apply_.populateRenderPrimitives(buf);
        reload_.populateRenderPrimitives(buf);
        // Character budget, right-aligned in the toolbar: "used / available". The limit is the editor's
        // maxLength (the script field's capacity in THIS ECU's definition) — never hardcoded here. Turns
        // red once the cap is hit. When uncapped it just shows the count.
        if (jf::JTextHelper::hasAtlas()) {
            const size_t used = edit_.text().size(), cap = edit_.maxLength();
            char cnt[48];
            if (cap) std::snprintf(cnt, sizeof(cnt), "%zu / %zu", used, cap);
            else     std::snprintf(cnt, sizeof(cnt), "%zu", used);
            const float tw = jf::JTextHelper::measureWidth(cnt);
            const uint8_t* col = (cap && used >= cap) ? jf::Colors::Danger : jf::Colors::TextSecondary;
            jf::JTextHelper::pushText(buf, b.x + b.width - tw - 12.f,
                                      b.y + (kBarH - jf::JTextHelper::lineHeight()) * 0.5f, cnt, col, tw + 4.f);
        }
        edit_.setBounds({ b.x, b.y + kBarH, b.width, b.height - kBarH });
        edit_.populateRenderPrimitives(buf);
    }

    void handleMouseMove(float mx, float my) override    { apply_.handleMouseMove(mx, my); reload_.handleMouseMove(mx, my); edit_.handleMouseMove(mx, my); }
    void handleMousePress(float mx, float my) override   { if (inBar(my)) { apply_.handleMousePress(mx, my); reload_.handleMousePress(mx, my); } else edit_.handleMousePress(mx, my); }
    void handleMouseRelease(float mx, float my) override { apply_.handleMouseRelease(mx, my); reload_.handleMouseRelease(mx, my); edit_.handleMouseRelease(mx, my); }
    bool handleScroll(float mx, float my, float w) override { return inBar(my) ? false : edit_.handleScroll(mx, my, w); }
    // TYPING GOES TO THE TEXT AREA. In the dock the framework's focus chain delivers keys to it directly;
    // hosted on the canvas the keys arrive HERE, addressed to the control the surface is driving, and
    // without this they stop at a JControl that does not handle keys.
    bool handleKeyEvent(const jf::JKeyEvent& ke) override { return edit_.handleKeyEvent(ke); }
    void beginEdit() override { edit_.beginEdit(); }
    void endEdit()   override { edit_.endEdit(); }

protected:
    // Focus is the caret: the canvas owns its own notion of the focused control and mirrors it onto us,
    // so pass it on to the part that draws a caret.
    void onFocusEvent(bool focused) override { edit_.setFocused(focused); }

private:
    bool inBar(float my) const { const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox; return my < b.y + kBarH; }

    // Put text in the box WITHOUT it counting as an edit — the load guard is the only thing standing
    // between "the ECU sent us the script" and "the user has unsent changes".
    void show_(const std::string& t) { loading_ = true; edit_.setText(t); loading_ = false; }
    void say_(const std::string& m)  { if (onStatus) onStatus(m); }

    // Paint the failing line red over whatever the syntax colours made it.
    void tintErrorLine_(const std::string& src, std::vector<uint8_t>& out) const {
        if (errLine_ <= 0) return;
        int line = 1; size_t a = std::string::npos, b = src.size();
        for (size_t i = 0; i <= src.size(); ++i) {
            if (line == errLine_) { a = i; const size_t e = src.find('\n', i); b = (e == std::string::npos) ? src.size() : e; break; }
            if (i < src.size() && src[i] == '\n') ++line;
        }
        if (a == std::string::npos) return;
        for (size_t i = a; i < b && i * 4 + 3 < out.size(); ++i) { out[i*4] = 255; out[i*4+1] = 96; out[i*4+2] = 96; out[i*4+3] = 255; }
    }

    static constexpr float kBarH = 30.f, kBtnH = 22.f, kApplyW = 110.f, kReloadW = 130.f;
    jf::JButton   apply_, reload_;
    jf::JTextArea edit_;
    bool          loading_ = false;   // a programmatic setText is in flight — not a user edit
    bool          dirty_   = false;   // the buffer holds something the ECU has not been given
    int           errLine_ = 0;       // the line lua_state is complaining about (0 = none)
};

#pragma once

// DiagnosticsDock — the bottom "Diagnostics" dock: a tabbed log viewer with an ECU Console (raw device
// console text + link errors) and a Lua Errors pane.
// Fed from EcuLink signals; buffers are capped so the log can't grow unbounded. Each pane has a right-click
// menu to Copy (the selection, or the whole log when nothing is selected) and Clear.

#include <j/core/JTabWidget.h>
#include <j/core/JTextArea.h>
#include <j/core/MenuSystem.h>
#include <j/platform/Clipboard.h>

#include <string>

class DiagnosticsDock : public jf::JTabWidget {
public:
    explicit DiagnosticsDock(jf::JSceneGraph& g)
        : jf::JTabWidget(g), console_(g, "ECU console output…"), luaErrors_(g, "Lua errors…") {
        addTab("ECU Console", &console_);
        addTab("Lua Errors", &luaErrors_);
        buildMenu(g, consoleMenu_, console_, cbuf_);
        buildMenu(g, luaMenu_,     luaErrors_, lbuf_);
        console_.setContextMenu(&consoleMenu_);   // framework opens these on right-click
        luaErrors_.setContextMenu(&luaMenu_);
    }

    void appendConsole(const std::string& s) { append(cbuf_, console_, s); }
    void appendLua(const std::string& s)     { append(lbuf_, luaErrors_, s); }

    // Bring the console to the front. Something arriving here that the user must READ — the ECU's own
    // account of a configuration error — is not reported if it lands on a tab that is not showing.
    void showConsole() { setActiveTab(0); }

private:
    static void append(std::string& buf, jf::JTextArea& area, const std::string& s) {
        buf += s;
        constexpr size_t kCap = 8000;
        if (buf.size() > kCap) buf.erase(0, buf.size() - kCap);
        area.setText(buf);
    }
    static void clear(std::string& buf, jf::JTextArea& area) { buf.clear(); area.setText(""); }

    // Wire a pane's right-click menu. Captures member pointers (stable for the dock's lifetime), not the
    // reference params — Copy grabs the current selection, or the whole buffer when nothing is selected.
    static void buildMenu(jf::JSceneGraph& g, jf::JMenu& menu, jf::JTextArea& area, std::string& buf) {
        jf::JTextArea* a = &area; std::string* b = &buf;
        menu.add(g, "Copy")->onTriggered.connect([a, b] {
            const std::string sel = a->selectedText();
            jf::JClipboard::setText(sel.empty() ? *b : sel);
        });
        menu.add(g, "Clear")->onTriggered.connect([a, b] { b->clear(); a->setText(""); });
    }

    jf::JTextArea console_, luaErrors_;
    jf::JMenu     consoleMenu_{ "ECU Console" }, luaMenu_{ "Lua Errors" };
    std::string   cbuf_, lbuf_;
};

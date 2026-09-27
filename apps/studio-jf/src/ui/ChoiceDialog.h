#pragma once

// ChoiceDialog — a question, and buttons that say what pressing them does.
//
// The studio had one dialog for this and it was a LIST PICKER: a scrolling list box holding the
// options as rows, with OK and Cancel underneath. So every confirmation offered two ways to accept
// (double-click the row, or select it and press OK) and two ways to cancel (the Cancel row, and the
// Cancel button), and the reader had to look in two places to answer one question. Worse, the thing
// being decided — "forget 84 learned cells" — was not in the dialog at all; it went past as a status
// toast while the dialog asked "Reset Long Term Fuel Trim?" over an empty box.
//
// A question with named answers is a message and some buttons. Up to three, each with a ROLE, so the
// button box puts them where the platform expects: the destructive one apart on the left, Cancel and
// the accept pair on the right, Escape and Return bound to reject and accept respectively.
//
// THE ONE RULE THAT MATTERS: dismissal — Escape, the close button, the window manager — is always the
// REJECT answer, never a silent accept and never a destructive one. Some of these dialogs overwrite an
// ECU, and closing a window you did not understand must not be the way that happens.
//
// Result is the index into the buttons passed in, or -1 for dismissal.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>
#include <j/core/JButton.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/core/Log.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

class ChoiceDialog {
public:
    // One answer: what the button says, and what kind of answer it is.
    struct Choice {
        std::string label;
        jf::JDialogButtonBox::Role role = jf::JDialogButtonBox::Role::Accept;
    };

    static constexpr uint32_t kW = 560;
    // openModal centres on kW x kH before the dialog exists, so this is the PLACEMENT height —
    // a typical one. The window is built at heightFor(body), which is the real one.
    static constexpr uint32_t kH = 210;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }
    static float hdrH()  { return jf::JStyle::current().titleBarHeight; }
    // ONE WRAP, USED TWICE. The height and the drawing have to agree about how many lines the question
    // takes, and the first version worked that out twice — an estimate for the window size and a real
    // wrap for the text — so a body that wrapped to more lines than the guess was simply drawn off the
    // bottom of the dialog. The reader saw half a question and no sign there was more.
    // HOW WIDE IS THIS TEXT, when the thing that measures it is not ready?
    //
    // A dialog is built before its own window has drawn a frame, and in that moment measureWidth
    // returns 0 for every string. Nothing downstream can tell that from "this text is empty": the wrap
    // never found a line too long, so the whole question became one line and the draw clipped it; and
    // every button fell back to its 96 px minimum, so "Apply to Base Table" was rendered centred in a
    // box too small for it and clipped at BOTH ends. Same cause, two symptoms, and the button is what
    // gave it away.
    //
    // So: a zero width for non-empty text is not a measurement, it is a refusal, and the estimate takes
    // over. Deliberately pessimistic — a slightly narrow line wraps early, a slightly wide one is lost.
    static constexpr float kCharW = 7.6f;
    static float textW(const std::string& t) {
        if (t.empty()) return 0.f;
        const float w = jf::JTextHelper::measureWidth(t);
        return w > 0.f ? w : static_cast<float>(t.size()) * kCharW;
    }

    static std::vector<std::string> wrapBody(const std::string& body, float maxW) {
        std::vector<std::string> out;
        std::string para;
        auto wrapPara = [&](const std::string& t) {
            if (t.empty()) { out.emplace_back(); return; }     // a paragraph break, kept as a blank line
            std::string line, word;
            auto push = [&] { if (!line.empty()) out.push_back(line); line.clear(); };
            for (size_t i = 0; i <= t.size(); ++i) {
                if (i == t.size() || t[i] == ' ') {
                    const std::string cand = line.empty() ? word : line + " " + word;
                    const bool over = !line.empty() && textW(cand) > maxW;
                    if (over) { push(); line = word; } else line = cand;
                    word.clear();
                } else word += t[i];
            }
            push();
        };
        for (char c : body) { if (c == '\n') { wrapPara(para); para.clear(); } else para += c; }
        wrapPara(para);
        return out;
    }
    static float lineH() { return jf::JTextHelper::lineHeight() + 4.f; }
    static uint32_t heightFor(const std::string& body) {
        const auto lines = wrapBody(body, static_cast<float>(kW) - 32.f);
        return static_cast<uint32_t>(hdrH() + 16.f + lines.size() * lineH() + 20.f + kBtnH() + 16.f);
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = uint32_t;
#endif

    ChoiceDialog(std::string title, std::string body, std::vector<Choice> choices,
                 std::function<void(int)> onResult,
                 jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_title(std::move(title)), m_body(std::move(body)), m_onResult(std::move(onResult))
        , m_lines(wrapBody(m_body, static_cast<float>(kW) - 32.f))
        , m_h(heightFor(m_body))
        , m_window(std::make_unique<PlatformWinType>(m_title, kW, m_h, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, m_h)) {
        // WHAT THIS DIALOG DECIDED, AND WHEN. The body has twice been laid out as one clipped line in
        // a window sized for several, and both times the cause was a measurement that answered
        // differently at construction than at draw. Neither is visible from a screenshot, so the two
        // moments say what they saw: JF_LOG=dialog=debug.
        JLOGC("dialog", jf::JLogLevel::Debug) << "CTOR lines=" << m_lines.size()
            << " h=" << m_h << " lineH=" << lineH() << " atlas=" << int(jf::JTextHelper::hasAtlas())
            << " measure(Mmmm)=" << jf::JTextHelper::measureWidth("Mmmm");
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            const float w = std::max(96.f, textW(choices[i].label) + 28.f);
            jf::JButton* b = m_box->addButton(choices[i].label, choices[i].role, w);
            // Every button answers with its own INDEX. The box's accept/reject signals only distinguish
            // two of them, and this dialog routinely has three.
            b->onClicked.connect([this, i] { finish(i); });
        }
        // …and the keyboard's two answers map to the roles, not to positions.
        m_box->onReject.connect([this] { finish(-1); });
        m_window->grabKeyboardFocus();
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { finish(-1); return false; }   // closing the prompt = reject

        // A LATER, WIDER MEASUREMENT WINS. The wrap is taken at construction, but if the font the frames
        // are drawn with needs MORE lines than that guess (a scaled atlas, a different face), the lines
        // were clipped at the box edge. So the question is re-wrapped against the live atlas, and a
        // result with more lines replaces the old one and the window grows to hold it. Never fewer: an
        // atlas that is not up yet measures everything as short (see the constructor's comment).
        if (JTextHelper::hasAtlas()) {
            std::vector<std::string> now = wrapBody(m_body, static_cast<float>(kW) - 32.f);
            if (now.size() > m_lines.size()) {
                m_lines = std::move(now);
                m_h = static_cast<uint32_t>(hdrH() + 16.f + m_lines.size() * lineH() + 20.f + kBtnH() + 16.f);
                m_window->setSize(kW, m_h);
                hal.resizeSurface(m_surface, kW, m_h);
            }
        }
        const float W = static_cast<float>(kW), H = static_cast<float>(m_h);
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease();

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
        }

        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(),
                              static_cast<std::uintptr_t>(m_window->rawWindowId()));
        const float pad = 16.f, by = H - kBtnH() - 12.f;
        m_box->setBounds({ pad, by, W - 2 * pad, kBtnH() });

        _refreshFocusRoots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }
        m_box->handleMouseMove(mx, my);
        if (pressed)  m_box->handleMousePress(mx, my);
        if (released) m_box->handleMouseRelease(mx, my);
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), m_title, jf::JStyle::current().cornerRadius, 0, 14.f);
        if (JTextHelper::hasAtlas()) {
            // THE QUESTION, IN THE DIALOG. Wrapped, and paragraph breaks kept — the second paragraph is
            // usually the consequence ("the engine has to learn them again"), which is the half that
            // decides the answer.
            // WRAPPED ONCE, AT CONSTRUCTION. Re-wrapping here measured text against this dialog's OWN
            // window, whose font atlas is not up yet on the first frames — every width came back too
            // small, nothing ever exceeded the line width, and the whole question was laid out as one
            // line and then CLIPPED by the draw. The window was the right height for three lines and
            // showed one, ending mid-sentence.
            float y = hdrH() + 16.f;
            if (m_frames++ < 2)   // the first frames only: this is a diagnosis, not a stream
                JLOGC("dialog", jf::JLogLevel::Debug) << "RENDER lines=" << m_lines.size()
                    << " lineH=" << lineH() << " H=" << H << " atlas=" << int(JTextHelper::hasAtlas());
            for (const std::string& line : m_lines) {
                if (!line.empty()) JTextHelper::pushText(buf, 16.f, y, line, Colors::TextSecondary, W - 32.f);
                y += lineH();
            }
        }
        m_box->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void finish(int result) {
        if (m_done) return;
        m_done = true;
        if (m_onResult) m_onResult(result);
    }
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_box) roots.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    std::string m_title, m_body;
    std::vector<std::string> m_lines;   // the question, wrapped once where the atlas is known good
    std::function<void(int)> m_onResult;
    uint32_t m_h;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    jf::JFocusManager m_focus;
    bool m_focusSeeded{ false };
    int  m_frames{ 0 };
    bool m_done{ false };
};

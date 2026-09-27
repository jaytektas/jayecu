// ExpressionWidget — supplies a live jf::JLineEdit to HostedControlWidget and translates between
// the readable source the tuner types and the bytecode the field stores.

#include "ExpressionWidget.h"
#include "../../model/Cache.h"
#include "../../model/ExprCompiler.h"
#include "../../model/MetaModel.h"

#include "../Surface.h"            // Surface::onEditExpression — the app-wired modal hook
#include "../../model/MetaModel.h"

#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>
#include <j/core/Log.h>

jf::JControl* ExpressionWidget::control() {
    if (!m_edit) {
        // An empty program is not a gap, it is a meaning — and WHICH meaning is the field's own: launch arms
        // while standing still, the datalog logs while the engine turns, learning follows its built-in
        // gates. So the placeholder says only that the built-in rule applies; the field's help says what
        // that rule is. (It read "always armed" on every condition field, which was true of none.)
        m_edit = std::make_unique<jf::JLineEdit>(sceneGraph(), "empty: built-in rule (see help)");
        // THE ✕, for the same reason a signal selector has one: a condition has an UNSET state that
        // means something — always armed — and until now the only way back to it was to select the
        // whole line and delete it. An enum field offers "not assigned" as a press; a condition field
        // has to offer "no condition" the same way, or the empty program is a state you can only reach
        // by accident. It clears through onTextChanged, so it takes exactly the path typing does and
        // writes the all-zero block (OP_END) rather than leaving the last program behind.
        m_edit->setClearButtonEnabled(true);
        m_edit->onTextChanged.connect([this](const std::string& t) {
            if (syncing()) return;                // ignore our own setText echo
            setSource(t);
        });
    }
    return m_edit.get();
}

void ExpressionWidget::setSource(const std::string& text) {
    const std::string path = writableConfigPath(bindPath());
    if (path.empty()) return;

    Cache& C = Cache::instance();
    const MetaModel* meta = C.meta();
    if (!meta) return;

    const auto r = ExprCompiler::compile(text, *meta, (uint32_t)meta->configSize());
    if (!r.ok) {
        // Keep the last good program. Writing a program that does not compile is not an option, and
        // clearing the field on a typo would throw away work the tuner is in the middle of.
        m_error = r.error;
        JLOGC("surface.expr", jf::JLogLevel::Debug)
            << "expression not committed (" << r.error << "): " << text;
        return;
    }
    m_error.clear();
    m_shown = text;
    C.setConfigBlob(path, r.code);
}

void ExpressionWidget::syncControl() {
    const std::string path = writableConfigPath(bindPath());
    if (path.empty()) return;
    const MetaModel* meta = Cache::instance().meta();
    if (!meta) return;

    const std::vector<uint8_t> code = Cache::instance().configBlob(path);
    const std::string src = code.empty()
        ? std::string()
        : ExprCompiler::decompile(code.data(), (uint16_t)code.size(), *meta);

    // Do not fight the typist. While the box holds text that compiles to exactly what is stored,
    // leave it alone — decompiled source is normalised ("rpm>2500" comes back as "rpm > 2500"), so
    // re-showing it on every sync would rewrite the caret out from under someone mid-edit.
    if (!m_error.empty()) return;                       // invalid text: theirs to fix, not ours to replace

    // The control is created lazily on first paint, so a sync can legitimately arrive before it
    // exists — a refresh driven by a config read, a test, or a widget that is bound but not yet
    // realised. The stored source is still tracked; only the display is skipped.
    if (!m_edit) { m_shown = src; return; }

    if (m_edit->text() == m_shown && !m_shown.empty()) {
        const auto r = ExprCompiler::compile(m_shown, *meta, (uint32_t)meta->configSize());
        if (r.ok && r.code == code) return;             // same program, different spelling — keep theirs
    }
    if (m_edit->text() != src) { m_edit->setText(src); m_shown = src; }
}

// The `fx` strip: RESERVE it, paint the control in what is left, then the glyph in the strip.
//
// It used to paint the control across the whole content rect and drop the glyph on top, which is
// two bugs in one: a long expression ran underneath "fx" and was unreadable exactly when it most
// needed reading, and the strip was inside the line edit's own bounds — so the box hit-tested and
// placed a caret under a glyph that belongs to the button. Narrowing the rect before the base sees
// it fixes both, because setBounds() is what decides where the control paints AND where it is hit.
void ExpressionWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& content, const Cache& cache) {
    using namespace jf;
    // Below twice the strip width there is no room for a target, and the glyph is not drawn at all
    // (see below) — so the control keeps the full width rather than being squeezed for nothing.
    const bool strip = content.width >= kFxW * 2.f;
    JRect box = content;
    if (strip) box.width -= kFxW;
    HostedControlWidget::render(buf, box, cache);      // paint the control, clear of the strip
    if (!JTextHelper::hasAtlas()) return;
    const JRect& r = content;
    if (!strip) return;                               // too small to be worth a target
    const float bx = r.x + r.width - kFxW;
    buf.pushClip(bx, r.y, kFxW, r.height);
    const uint8_t* col = m_error.empty() ? Colors::TextSecondary : Colors::Danger;
    const float tw = JTextHelper::measureWidth("fx");
    JTextHelper::pushText(buf, bx + (kFxW - tw) * 0.5f,
                          r.y + (r.height - JTextHelper::lineHeight()) * 0.5f,
                          "fx", col, kFxW);
    buf.popClip();
}

bool ExpressionWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    // A press in the fx strip opens the full editor rather than putting a caret in the text box.
    // Hit-tested against the CONTENT rect and under the same width guard the paint uses, so the
    // target is exactly where the glyph is: the strip is drawn inside the padding, and on a widget
    // too narrow to draw one at all there is no invisible target sitting over the text.
    const jf::JRect c = contentRect(r);
    if (in.kind == ControlInput::Kind::Press && c.width >= kFxW * 2.f &&
        in.mx >= c.x + c.width - kFxW) {
        openEditor();
        return true;
    }
    return HostedControlWidget::handleControlInput(r, in);
}

// The ONE expression editor, told this expression runs on the ECU: it shows compile errors, how much
// of the program block is used, and what the firmware will actually run. Same dialog and same grammar
// the visibility-condition editor uses — see ExprAst.h.
void ExpressionWidget::openEditor() {
    if (!Surface::onEditExpression) return;
    const std::string path = writableConfigPath(bindPath());
    const MetaModel* meta = Cache::instance().meta();
    if (path.empty() || !meta) return;
    int off = 0, size = 0;
    if (!meta->resolveBlob(path, off, size)) return;

    Surface::onEditExpression(m_shown, (uint16_t)size, elementContext(), [this](std::string text) {
        setSource(text);                              // exactly the path typing takes
        // Re-read the field so the box shows what was actually stored (normalised), rather than
        // trusting the dialog's text — same route a config read takes.
        syncControl();
    });
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("expression", ExpressionWidget, 116);

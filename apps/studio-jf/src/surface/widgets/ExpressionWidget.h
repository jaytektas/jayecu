#pragma once

// ExpressionWidget — an editable EXPRESSION config field (a sensor's DTC precondition, a module's
// arm condition), hosting a real framework jf::JLineEdit exactly as TextFieldWidget hosts a string.
//
// The field holds compiled BYTECODE (firmware/Signal/ExprIsa.h), not text, so this widget is the
// two-way translation:
//
//   show   bytes -> ExprCompiler::decompile -> "rpm > 2500 and (map > 50 or tps > 80)"
//   commit text  -> ExprCompiler::compile   -> bytes -> Cache::setConfigBlob
//
// A field that does not compile is NOT written. Silently storing a half-typed expression would
// leave the ECU running a gate the tuner never finished writing; the control marks itself invalid
// and keeps the last good program until the text parses.
//
// This is why `expression` is its own schema type: without it the dictionary would hand you a spin
// box over byte 0 of a program, which is an opcode.

#include "HostedControlWidget.h"
#include <j/core/JLineEdit.h>
#include <memory>
#include <string>

class ExpressionWidget : public HostedControlWidget {
public:
    explicit ExpressionWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "expression") {}
    std::string elementType() const override { return "expression"; }
    std::string paletteTitle() const override { return "Expression"; }
    float       defaultW()     const override { return 320.f; }
    float       defaultH()     const override { return 30.f; }

    // The compile error for the text currently in the box, or empty when it compiles. The editor
    // dialog and the tooltip both read this rather than re-compiling.
    const std::string& compileError() const { return m_error; }

    // Compile `text` and store the program in the bound field. This IS what typing does — the line
    // edit's onTextChanged calls it — so a caller that wants to set an expression without a
    // keyboard (a test, a preset, a paste) goes through exactly the same path a tuner does.
    void setSource(const std::string& text);
    // The source currently displayed (decompiled from the field, or what was last committed).
    const std::string& source() const { return m_shown; }
    // Re-read the bound field and refresh the displayed source.
    void refresh() { syncControl(); }

    // Width of the `fx` strip at the right-hand end of the control. A press there opens the full
    // editor (field tree, live compile feedback) instead of going to the text box — the box is fine
    // for a gate you can type from memory, the dialog is for building one.
    static constexpr float kFxW = 26.f;

    // Public because the base declares them so — the Surface drives both, and narrowing the access
    // here would only make the widget harder to exercise than the thing it overrides.
    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& content, const Cache& cache) override;
    bool handleControlInput(const jf::JRect& r, const ControlInput& in) override;

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    void openEditor();

private:

    std::unique_ptr<jf::JLineEdit> m_edit;
    std::string m_error;        // last compile error ("" = the text is a valid program)
    std::string m_shown;        // the source we last put in the box, so sync does not fight typing
};

// ActionButtonWidget — see the header. Evaluates its expression and writes the result to its target,
// through the same path a typed edit takes, so it is undoable and burnable like any other change.

#include "ActionButtonWidget.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"
#include "../../model/MetaModel.h"
#include <j/core/Log.h>
#include <cmath>
#include <cstdio>

jf::JControl* ActionButtonWidget::control() {
    if (!m_btn) {
        m_btn = std::make_unique<jf::JButton>(sceneGraph(), std::string{});
        m_btn->onClicked.connect([this] { run(); });
    }
    return m_btn.get();
}

// CONFIG SIGILS, COOKED. `[#path]` resolves to the RAW stored count — 600 for a calibration speed of
// 60.0 kph — because the Cache is raw and every widget cooks with the field's own scale. That is fine
// where a condition tests an enable flag (scale 1, so raw and engineering are the same number) and quietly
// wrong the moment a sum mixes two fields of different scale, which is exactly what this button does.
//
// So the button works in ENGINEERING units throughout: every `[#…]` is replaced by its cooked value before
// the expression is evaluated, and the result is written back through the target's scale. A page author
// writes the arithmetic they mean — Hz x 3600 / kph — without knowing what any of it is stored as.
std::string ActionButtonWidget::cookExpr(const std::string& expr) {
    std::string out;
    size_t i = 0;
    while (i < expr.size()) {
        if (expr[i] != '[' || i + 1 >= expr.size() || expr[i + 1] != '#') { out += expr[i++]; continue; }
        // Find the matching ']' allowing one level of nesting, which is how an indexed path is written
        // ("[#sensors.sensor[clt].source]").
        size_t j = i + 2; int depth = 0;
        while (j < expr.size() && !(expr[j] == ']' && depth == 0)) {
            if (expr[j] == '[') ++depth;
            else if (expr[j] == ']') --depth;
            ++j;
        }
        if (j >= expr.size()) { out += expr[i++]; continue; }        // unterminated: leave it alone
        const std::string path = expr.substr(i + 2, j - i - 2);
        Cache& c = Cache::instance();
        if (c.isConfig(path)) {
            const double scale = c.configScale(path);
            char lit[32];
            std::snprintf(lit, sizeof lit, "%.17g", c.configValue(path) * (scale != 0.0 ? scale : 1.0));
            out += lit;
        } else {
            out += expr.substr(i, j - i + 1);                        // not a config path: the evaluator's problem
        }
        i = j + 1;
    }
    return out;
}

// THE TARGETS ONLY — no expression is evaluated here: this is asked every frame by enabledNow(), and a
// button whose target is locked must say so whatever its values would come to.
std::vector<std::string> ActionButtonWidget::writesTo() const {
    std::vector<std::string> out;
    const PanelElement* el = element();
    if (!el) return out;
    const std::string spec = el->prop("writes");
    size_t i = 0;
    while (i < spec.size()) {
        size_t end = spec.find_first_of(";\n", i);
        if (end == std::string::npos) end = spec.size();
        const std::string line = spec.substr(i, end - i);
        i = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string path = line.substr(0, eq);
        const size_t a = path.find_first_not_of(" \t\r"), b = path.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        path = MathEvaluator::instance().resolveIndexed(
            MathEvaluator::resolveTemplate(path.substr(a, b - a + 1), elementContext()));
        out.push_back(path);
    }
    return out;
}

std::vector<ActionButtonWidget::Write> ActionButtonWidget::pending() const {
    std::vector<Write> out;
    const PanelElement* el = element();
    if (!el) return out;
    const std::string spec = el->prop("writes");
    if (spec.empty()) return out;
    // Evaluated in THIS widget's element scope, so "[*]" and a sigil to another control on the page mean
    // what they mean everywhere else. Opened once for the whole list rather than per write.
    MathEvaluator::ElementScope scope(elementContext());
    Cache& c = Cache::instance();

    size_t i = 0;
    while (i <= spec.size()) {
        // One write per ';' or newline. A ';' inside an expression would be a syntax error anyway, so
        // there is nothing to escape and no quoting to get wrong.
        size_t end = spec.find_first_of(";\n", i);
        if (end == std::string::npos) end = spec.size();
        const std::string line = spec.substr(i, end - i);
        i = end + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;                 // not a write; nothing to guess at
        auto trim = [](std::string t) {
            const size_t a = t.find_first_not_of(" \t\r");
            if (a == std::string::npos) return std::string{};
            return t.substr(a, t.find_last_not_of(" \t\r") - a + 1);
        };
        const std::string path = trim(line.substr(0, eq));
        const std::string expr = trim(line.substr(eq + 1));
        if (path.empty() || expr.empty() || !c.isConfig(path)) continue;   // nothing writable to write to

        const double v = MathEvaluator::instance().evaluate(
            cookExpr(MathEvaluator::resolveTemplate(expr, elementContext())));
        // NOT FINITE OR ZERO: SKIPPED, NOT WRITTEN. See the header — this button computes measurements,
        // and neither an infinity nor a zero is one. It is what lets a single Capture calibrate whichever
        // pickups are turning without erasing the ones that are not.
        if (!std::isfinite(v) || (v == 0.0 && m_writeZeros != "1")) continue;   // a settings button writes 0
        out.push_back(Write{ path, v });
    }
    return out;
}

void ActionButtonWidget::syncControl() {
    const PanelElement* el = element();
    if (!el) return;
    const std::string label = el->prop("labelText");
    m_btn->setLabel(label.empty() ? "Calibrate" : label);
    // Greyed when there is nothing it would write. A button that computes a setting must not look ready
    // while the thing it divides by is still zero — the click would write an infinity, or nothing at all,
    // and both are worse than a button that plainly says "not yet".
    const size_t n = pending().size();
    const bool on = n > 0 && !Cache::instance().readOnly();
    if (on != m_lastEnabled) {
        m_lastEnabled = on;
        JLOGC("surface.action", jf::JLogLevel::Debug)
            << "action \"" << label << "\" -> " << (on ? "ENABLED" : "disabled")
            << " (" << n << " write(s) ready)";
    }
    m_btn->setEnabled(on);
}

void ActionButtonWidget::run() {
    const PanelElement* el = element();
    if (!el) return;
    Cache& c = Cache::instance();
    // Each expression gives its value in the target's ENGINEERING units — the same number the user could
    // have typed — so it comes back to raw through that field's own scale, and goes out through writeRaw
    // like any other edit: clamped to the field, undoable, and pending a burn.
    //
    // EVALUATED FIRST, WRITTEN AFTER. pending() reads every expression before a single write lands, so a
    // list cannot measure itself: a later write changing a setting an earlier expression read would make
    // the result depend on the order the lines happen to be in.
    for (const Write& w : pending()) {
        const MetaModel::Location L = c.meta() ? c.meta()->locate(w.path) : MetaModel::Location{};
        const double scale = c.configScale(w.path);
        const double raw   = (scale != 0.0) ? w.value / scale : w.value;
        JLOGC("surface.action", jf::JLogLevel::Info)
            << "action \"" << el->prop("labelText") << "\": " << w.path << " = " << w.value
            << " (raw " << raw << ")";
        c.writeRaw(L, Raw{raw}, w.path);
    }
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("action", ActionButtonWidget, 72);

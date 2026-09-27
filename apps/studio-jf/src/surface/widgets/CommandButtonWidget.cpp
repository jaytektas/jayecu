// CommandButtonWidget — supplies a live jf::JButton to HostedControlWidget; on click it fires the CLI command.
// The base does paint + input-forward; JButton::onClicked (fires on press) runs the command below.

#include "CommandButtonWidget.h"
#include "../../model/Cache.h"
#include <cmath>
#include <cstdio>

jf::JControl* CommandButtonWidget::control() {
    if (!m_btn) {
        m_btn = std::make_unique<jf::JButton>(sceneGraph(), std::string{});
        m_btn->onClicked.connect([this] { run(); });
    }
    return m_btn.get();
}

void CommandButtonWidget::syncControl() {
    const std::string t = element() ? element()->prop("labelText") : std::string{};
    m_btn->setLabel(t.empty() ? "Command" : t);
}

void CommandButtonWidget::run() {
    const PanelElement* el = element();
    if (!el) return;
    // An arg is a literal token by default; if it contains a sigil ("[…]") it is an expression evaluated live
    // (so [@dial.value] / [$clt] pass the current value). Whole numbers format as ints (usually indices/counts).
    auto resolveArg = [this](const std::string& a) -> std::string {
        if (a.empty() || a.find('[') == std::string::npos) return a;
        const double v = evalSource(a).v;
        char b[32];
        if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 1e15)
            std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(v)));
        else
            std::snprintf(b, sizeof b, "%g", v);
        return b;
    };
    // An IMPORTED button carries the controller's own instruction bytes; send those, through the app's link.
    if (const std::string hex = el->prop("commandHex"); !hex.empty()) {
        std::vector<std::vector<uint8_t>> payloads;
        std::vector<uint8_t> cur;
        for (size_t i = 0; i + 1 < hex.size() || (i < hex.size() && hex[i] == ','); ) {
            if (hex[i] == ',') { payloads.push_back(std::move(cur)); cur.clear(); ++i; continue; }
            cur.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            i += 2;
        }
        if (!cur.empty()) payloads.push_back(std::move(cur));
        const bool sent = tscmd::send() ? tscmd::send()(payloads) : false;
        if (el->prop("showMessageOnClick") == "1" && appmsg::show())
            appmsg::show()(el->prop("labelText").empty() ? "Command" : el->prop("labelText"),
                           sent ? el->prop("message") : "Not connected — the command was not sent.");
        return;
    }
    // FOUR ARGS, not two. The bench output test takes `test <row> <count> <on_ms> <off_ms>`, and three
    // of those are live host values the operator sets beside the button — so a button that can only
    // carry two would have to bake one of them in, which is the one thing a settable parameter must not
    // be. An absent arg is skipped, so a two-arg button is unchanged.
    std::string cli = el->prop("command");
    for (const char* k : {"arg0", "arg1", "arg2", "arg3"}) {
        const std::string a = resolveArg(el->prop(k));
        if (!a.empty()) cli += " " + a;
    }
    if (cli.empty()) return;
    Cache::instance().sendCli(cli);   // fire-and-forget; firmware reports completion in command_state telemetry
    if (el->prop("showMessageOnClick") == "1" && appmsg::show())
        appmsg::show()(el->prop("labelText").empty() ? "Command" : el->prop("labelText"), el->prop("message"));
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("command", CommandButtonWidget, 70);

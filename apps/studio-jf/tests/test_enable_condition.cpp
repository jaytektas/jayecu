// Does the `enableCondition` property actually DISABLE a control?
//
// It greys one: the Surface draws a disabled wash over any element whose enable condition is false
// (Surface.cpp), a label fades its own caption, and a panel passes the state to its children. But
// greyed is not disabled — the question is whether the control still ACTS.
//
// This drives the run-mode input path the Surface uses and asserts that a disabled control refuses
// input. A checkbox that looks disabled and still toggles the tune when clicked is worse than one
// that looks enabled, because the UI is actively lying about what it will do.
//
// It goes through the widget's OWN enabledNow(), which is what the Surface, the viewport and the panel
// all call. It used to re-implement that step in a lambda — read the prop, evaluate, compare to zero —
// and so agreed with itself while the viewport honoured none of it: the gate was evaluated there and
// the veil never drawn, and this test passed throughout.
//
//   cmake --build build --target enable_condition_test && ./build/enable_condition_test
#include "../src/surface/CanvasWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MathEvaluator.h"
#include "../src/model/ISigilResolver.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <map>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-60s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// A channel the enable condition can read, so the test can turn it on and off.
struct TestChannels : ISigilResolver {
    std::map<std::string, double> v;
    char sigil() const override { return '$'; }
    double resolveSigil(const std::string& n) const override {
        auto it = v.find(n);
        return it == v.end() ? 0.0 : it->second;
    }
    bool provides(const std::string& n) const override { return v.count(n) != 0; }
    std::vector<std::string> available() const override {
        std::vector<std::string> o;
        for (const auto& [k, x] : v) { (void)x; o.push_back(k); }
        return o;
    }
};
static TestChannels g_chans;

// A minimal interactive widget that records whether input reached it.
class ProbeWidget : public CanvasWidget {
public:
    explicit ProbeWidget(jf::JSceneGraph& g) : CanvasWidget(g) {}
    std::string elementType()  const override { return "probe"; }
    std::string paletteTitle() const override { return "Probe"; }
    float       defaultW()     const override { return 100.f; }
    float       defaultH()     const override { return 30.f; }
    bool interactive() const override { return true; }
    bool handleControlInput(const jf::JRect&, const ControlInput&) override {
        ++hits;
        return true;
    }
    int hits = 0;
};

int main() {
    jf::JSceneGraph graph;
    MathEvaluator::instance().registerResolver(&g_chans);
    g_chans.v["armed"] = 1.0;

    std::puts("=== enableCondition: greyed, or actually disabled? ===");

    ProbeWidget w(graph);
    PanelElement el;
    el.type = "probe";
    el.props["enableCondition"] = "armed";
    w.loadSkin(el);
    // The Surface binds the element before it paints or routes anything; enabledNow() reads its condition
    // from there. Skipping this left the widget with no element at all, which fails OPEN — so the test
    // would have reported "enabled" for a gate it never actually consulted.
    w.setData(el);

    // What the Surface does each frame before painting. It asks the WIDGET — enabledNow() — rather than
    // reading the prop and evaluating it here, and that difference is the whole point of this test now:
    // re-implementing the call site is exactly how the viewport came to honour none of this. A copy of
    // the rule in a test agrees with itself forever while the real path drifts.
    auto applyEnable = [&] { w.setRenderDisabled(!w.enabledNow()); };

    // What the Surface's routeRunInput_ does on a click. The gate under test is acceptsInput() —
    // the ONE rule every delivery site asks (Surface, viewport, panel), rather than three copies.
    auto send = [&](ControlInput::Kind k) {
        if (!w.interactive()) return false;
        if (!w.acceptsInput(k)) return false;
        ControlInput in;
        in.kind = k;
        return w.onControlInput(jf::JRect{0, 0, 100, 30}, in);
    };
    auto deliver = [&] { return send(ControlInput::Kind::Press); };

    g_chans.v["armed"] = 1.0;
    applyEnable();
    ck(!w.renderDisabled(), "condition true -> the widget is enabled");
    const int before = w.hits;
    deliver();
    ck(w.hits == before + 1, "…and a click reaches it");

    g_chans.v["armed"] = 0.0;
    applyEnable();
    ck(w.renderDisabled(), "condition false -> the widget is marked disabled");
    const int mid = w.hits;
    deliver();
    ck(w.hits == mid, "…and a click must NOT reach it",
       "input still delivered " + std::to_string(w.hits - mid) + " time(s)");

    // The same question for the keyboard: a disabled control must not take typing either.
    {
        g_chans.v["armed"] = 0.0;
        applyEnable();
        const int k0 = w.hits;
        send(ControlInput::Kind::Key);
        ck(w.hits == k0, "…and neither does a key event",
           "key still delivered " + std::to_string(w.hits - k0) + " time(s)");

        send(ControlInput::Kind::Scroll);
        ck(w.hits == k0, "…nor the scroll wheel");

        // Blur is the ONE kind that must still arrive: a control mid-edit when the condition went
        // false has to commit or drop its caret, or it strands one in a control now shown inert.
        const int b0 = w.hits;
        send(ControlInput::Kind::Blur);
        ck(w.hits == b0 + 1, "…but Blur IS still delivered, so a mid-edit control can close out");
    }

    // Re-enabling restores it — the flag is per-frame state, not a latch.
    {
        g_chans.v["armed"] = 1.0;
        applyEnable();
        const int r0 = w.hits;
        deliver();
        ck(w.hits == r0 + 1, "re-enabling makes it live again");
    }

    // ---- canframes(bus): the CAN Setup page's lock -------------------------------------------------
    // The Bitrate row on Configuration/CAN Bus/CANn carries enableCondition "canframes(n) == 0", so a
    // bus locks its rate the moment it carries a frame. An UNKNOWN function name in this evaluator
    // returns 0 rather than failing, and 0 == 0 is true — so a typo, or the hook never being
    // installed, leaves the row editable with a full bus and looks exactly like it working.
    {
        int frames0 = 0, frames1 = 0;
        MathEvaluator::setCanFrameCounter([&](int b) { return b == 0 ? frames0 : frames1; });

        PanelElement ce;
        ce.type = "probe";
        ce.props["enableCondition"] = "canframes(0) == 0";
        ProbeWidget cw(graph);
        cw.loadSkin(ce);
        cw.setData(ce);

        ck(cw.enabledNow(), "empty bus: the Bitrate row is editable");

        frames0 = 6;
        ck(!cw.enabledNow(), "six frames on the bus: the row is LOCKED");   // red if the name is unknown

        frames0 = 0;
        ck(cw.enabledNow(), "clearing the bus unlocks it again");

        // The other bus is its own question.
        frames1 = 3;
        ck(cw.enabledNow(), "CAN2 carrying frames does not lock CAN1");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "enableCondition disables as well as greys",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

// A DIGITAL OUTPUT SLOT READS AS A STATE, NOT A PERCENTAGE.
//
// out_<n> is one channel shared by all the slots, so it has to be a float in percent — a PWM slot's
// duty needs it. But the number a DIGITAL slot publishes is the command handed to the sink BEFORE the
// sink thresholded it, so 60 means the pin is on and 40 would mean it is off, identical on screen.
// This checks the readout says which, at the same threshold the firmware switches at.
//
//   cmake --build build --target output_readout_test && ./build/output_readout_test
#include "../src/surface/CanvasWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "../src/model/MathEvaluator.h"
#include "../src/model/SigilResolvers.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[out-readout] (no meta found — skipped)"); return 0; }

    // The "$" sigil is what a page binds through, so the test resolves it the way the app does —
    // without this the binding evaluates to 0 and every readout looks like an off output.
    static TelemetrySigilResolver telemSigils;
    MathEvaluator::instance().registerResolver(&telemSigils);

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    const std::string slot = "outputs.output[0]";
    if (!C.isConfig(slot + ".kind")) { std::puts("[out-readout] (no output slots — skipped)"); return 0; }

    PanelElement el;
    el.type = "value";
    el.props["signalName"] = "[$out_1]";     // the channel slot 1 publishes
    auto shown = [&] { return CanvasWidget::fmtVal(el, C); };

    // Drive the channel the way the ECU does — a telemetry frame — so this tests the whole path the
    // screen takes, not a decision in isolation. out_1 is one byte at the descriptor's own offset.
    const MetaModel::TelemField* tf = nullptr;
    const auto& telem = meta.telemetry();
    if (const auto it = telem.find("out_1"); it != telem.end()) tf = &it->second;
    if (!tf) { std::puts("[out-readout] (meta has no out_1 channel — skipped)"); return 0; }
    auto send = [&](double pct) {
        std::vector<uint8_t> frame((size_t)meta.telemetrySize(), 0);
        const double raw = pct / (tf->scale > 0.0 ? tf->scale : 1.0);
        frame[(size_t)tf->offset] = (uint8_t)(raw < 0 ? 0 : raw > 255 ? 255 : raw);
        C.ingestTelemetry(frame);
    };

    std::puts("=== Output readout: a digital slot is a state ===");

    C.setConfigValue(slot + ".function", 3.0);              // a Generic output…
    C.setConfigValue(slot + ".kind", 0.0);                  // …at PWM: the number IS the duty
    send(60.0);
    ck(shown().find("HI") == std::string::npos && shown().find("LOW") == std::string::npos,
       "a PWM slot still reads as a number", shown());

    // Digital. The firmware publishes the LEVEL it drove, so the channel is already 0 or 100 — and
    // that, not the old pre-threshold command, is what the readout names.
    //
    // MAKE IT A GENERIC OUTPUT FIRST. A row at its defaults has no function (None), and this used to
    // read it as LOW — the absence of a reading drawn identically to a reading of "off".
    C.setConfigValue(slot + ".kind", 1.0);
    C.setConfigValue(slot + ".function", 3.0);
    send(0.0);
    ck(shown() == "LOW", "a digital slot reads LOW when the pin is off", shown());
    send(100.0);
    ck(shown() == "HI", "…and HI when it is on", shown());

    // ...AND NOTHING TO REPORT IS NOT A REPORT OF NOTHING. A row that is not a Generic output — None, or
    // a coil or an injector — cannot be HI or LOW on this channel. The channel still carries a
    // number, so this has to be decided from the config, never from the value.
    send(100.0);
    C.setConfigValue(slot + ".function", 0.0);
    ck(shown() == "Unused", "a row with no function reads Unused, not LOW", shown());
    C.setConfigValue(slot + ".function", 3.0);
    ck(shown() == "HI", "…and comes back when it is a generic output again", shown());
    C.setConfigValue(slot + ".function", 1.0);
    ck(shown() == "Coil", "a coil row names itself on its output channel", shown());
    C.setConfigValue(slot + ".function", 2.0);
    ck(shown() == "Injector", "…and an injector row", shown());
    C.setConfigValue(slot + ".function", 3.0);
    ck(shown() == "HI", "…and comes back", shown());

    // KIND IS A GENERIC OUTPUT'S SETTING. A coil row keeps whatever Kind it had as a lamp; it must read
    // the same either way (it read NA left at Digital, and a duty of 0 % left at PWM).
    C.setConfigValue(slot + ".function", 1.0);
    C.setConfigValue(slot + ".kind", 0.0);
    ck(shown() == "Coil", "a coil row left at PWM reads Coil too, not a duty", shown());
    C.setConfigValue(slot + ".function", 0.0);
    ck(shown() == "Unused", "…and an unused one", shown());

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "All output readout tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

// What control a dictionary field is dropped as.
//
// A channel selector must drop as a PICKER. It regressed to a spin box — a field asking you to type a
// raw signal id — because the classifier matched one literal control kind ("signal") while the field
// carried another ("sensor"). Any kind that means "pick a channel" has to route to the picker; the test
// asserts the BEHAVIOUR, not which spelling of the kind a given field happens to use, so renaming a kind
// cannot quietly turn a selector back into a number box.
//
//   cmake --build build --target control_kind_test && ./build/control_kind_test

#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[control] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    // META_PATH is absolute, from the build. The relative guesses that used to be the only candidates
    // resolved against the CWD, so running the binary from anywhere but the build directory found
    // nothing — and the miss returned 0, so the suite reported a pass for a test that never ran.
    const char* cands[] = { argc > 1 ? argv[1] : nullptr, META_PATH };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::printf("[control] FAIL: no meta at %s\n", META_PATH); return 1; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    C.setConfigImage(std::vector<uint8_t>(size_t(meta.configSize()), 0));

    // Nested sub-struct fields (a sensor's inline preconditions). These resolve for OFFSET but the
    // descriptive lookups stopped at one array level, so every one of them classified as a plain number:
    // the operator list, the AND/OR list and the signal picker all dropped as Config Edit spin boxes.
    // Skipped on a meta without preconditions (the trimmed fixture), so this only asserts where it applies.
    {
        std::string pre;
        for (const char* id : {"clt", "iat", "map", "tps", "0"}) {
            const std::string p = std::string("sensors.sensor[") + id + "].precond[0].op";
            if (!meta.enumOptions(p).empty()) { pre = std::string("sensors.sensor[") + id + "].precond[0]"; break; }
        }
        if (pre.empty()) {
            std::printf("[control] (meta has no preconditions — nested-field cases skipped)\n");
        } else {
            check(C.widgetTypeFor(pre + ".op") == "combobox",
                  "precondition Operator drops as a list", C.widgetTypeFor(pre + ".op"));
            check(C.widgetTypeFor(pre + ".combine") == "combobox",
                  "precondition Combine drops as a list", C.widgetTypeFor(pre + ".combine"));
            check(meta.isSignalField(pre + ".signal"),
                  "precondition Signal is a channel selector", meta.subFieldKind(pre + ".signal"));
            check(C.widgetTypeFor(pre + ".signal") == "enum",
                  "…so it drops as a PICKER", C.widgetTypeFor(pre + ".signal"));
            check(C.widgetTypeFor(pre + ".value") == "configedit",
                  "precondition Value stays a number box", C.widgetTypeFor(pre + ".value"));
            check(!meta.labelFor(pre + ".op").empty(),
                  "…and each is captioned by its field, not its widget type", meta.labelFor(pre + ".op"));
        }
    }

    // A BOARD-PIN picker with no interface to gate on. Every other pin field in the schema offers
    // names through a `pickers` block; a trigger stream's capture input lost its declaration when the
    // crank/cam inputs were replaced by generic streams, so it dropped as a spin box and the user had
    // to know that "4" meant DIG3. The picker is UNGATED (`by` empty) — a sensor's list depends on its
    // interface, a capture input has no such sibling.
    {
        const std::string cap = "trigger.streams[0].capture_index";
        std::string by; std::vector<MetaModel::PickerSet> sets;
        check(meta.fieldPicker(cap, by, sets), "capture input has a board-pin picker");
        check(by.empty(), "…ungated: one list, always", "by='" + by + "'");
        check(sets.size() == 1 && !sets[0].options.empty(), "…with the board's TRIGGER_INPUT pins",
              sets.empty() ? "none" : std::to_string(sets[0].options.size()) + " options");
        if (!sets.empty() && sets[0].options.size() >= 5) {
            // Each option carries its own pool index as its VALUE — VR1=0, VR2=1, DIG1=2 … — so nothing
            // maps between the board yaml's order and s_board_capture_resources[]. Asserted by value
            // rather than by position, because a sentinel may lead the list (see below) and a picker
            // that reads position instead of value is the bug this pins.
            auto valueOf = [&](const char* label) {
                for (size_t i = 0; i < sets[0].options.size(); ++i)
                    if (sets[0].options[i] == label) return sets[0].values[i];
                return -1;
            };
            check(valueOf("VR1") == 0, "VR1 is pool index 0", std::to_string(valueOf("VR1")));
            check(valueOf("DIG3") == 4, "DIG3 is pool index 4", std::to_string(valueOf("DIG3")));
            // …and "None" — the 255 sentinel that means the stream names no pin — is offered, with
            // pin=0 so nothing treats it as a claimed resource. Without it a capture input could be
            // assigned and never unassigned.
            check(valueOf("None") == 255, "None (255) is in the list", std::to_string(valueOf("None")));
            const size_t none_i = 0;   // it leads the list: a sentinel is emitted ahead of the pool
            check(sets[0].options[none_i] == "None" && none_i < sets[0].pins.size()
                      && sets[0].pins[none_i] == 0,
                  "…and names no board pin");
        }
        check(C.widgetTypeFor(cap) == "enum", "…so it drops as a PICKER, not a spin box",
              C.widgetTypeFor(cap));
    }

    // The one that started this: a channel selector on an array element.
    const std::string tpsA = "electronic_throttle.etb[0].tps_a_src";
    check(meta.isSignalField(tpsA), "TPS A Signal is a channel selector", meta.fieldKind(tpsA));
    check(C.widgetTypeFor(tpsA) == "enum", "…so it drops as a PICKER, not a spin box",
          C.widgetTypeFor(tpsA));

    // An ordinary editable config scalar is still a spin box — the fix must not turn everything into a
    // picker.
    const std::string num = "electronic_throttle.etb[0].tps_match_ms";
    check(C.widgetTypeFor(num) == "configedit", "an ordinary config number is still a spin box",
          C.widgetTypeFor(num));

    // A YES/NO field is a TICK. The meta says 0..1 integer and nothing else, which is what every
    // "Enable …" in the schema is; a spin box over it offers the numbers 0 and 1 and shows "0" where
    // the answer is "no". The dictionary already dropped these correctly — it was the authored pages
    // that carried spin boxes — so this pins the behaviour they now derive from.
    check(C.widgetTypeFor("transient_throttle.enable_overall_corr") == "checkbox",
          "a 0..1 flag drops as a checkbox", C.widgetTypeFor("transient_throttle.enable_overall_corr"));
    check(C.widgetTypeFor("trigger.streams[0].enabled") == "checkbox",
          "…an array element's flag too", C.widgetTypeFor("trigger.streams[0].enabled"));

    // A telemetry channel is a readout.
    check(C.widgetTypeFor("tps") == "field", "a telemetry channel is still a readout",
          C.widgetTypeFor("tps"));

    // The selector index is what the studio offers pickers from — a sensor field missing from it means
    // no picker is offered anywhere, not just on drop.
    bool listed = false;
    for (const auto& s : meta.signalSelectorFields()) if (s.path == tpsA) { listed = true; break; }
    check(listed, "the sensor selector appears in the selector index");

    // An EXPRESSION field is a compiled program. Dropped as a spin box it would offer you byte 0 of
    // the bytecode — an opcode — as a number to type. That is the whole reason the schema has an
    // explicit `expression` type rather than a nameless blob.
    const std::string expr = "sensors.sensor[clt].precond_expr";
    check(meta.isExpressionField(expr), "a sensor precondition is an expression field",
          meta.fieldKind(expr));
    check(C.widgetTypeFor(expr) == "expression", "…so it drops as the EXPRESSION editor",
          C.widgetTypeFor(expr));
    check(C.isConfig(expr), "…and it is a writable config path");
    int eoff = 0, esize = 0;
    const bool blobOk = meta.resolveBlob(expr, eoff, esize);   // BEFORE the check call: argument
    check(blobOk && esize == 64,                               // evaluation order is unspecified
          "…addressed as a 64-byte blob, not a scalar", std::to_string(esize));
    // Its help text travels with it (every meta field carries one; a program needs it most).
    check(!meta.helpFor(expr).empty(), "…and it carries help text");

    // READ AS A NUMBER, an expression field reports whether it HOLDS a program. decodeRaw has no case
    // for EXPR, so this used to be 0 for every slot, written or not — and a page had no way to ask
    // "has this one been filled in?". It is the question the threshold monitors are laid out around:
    // the ACTION control is gated on "[#…condition] > 0", because deciding what to do about a question
    // nobody has asked is meaningless.
    check(C.configValue(expr) == 0.0, "an empty expression field reads as 0",
          std::to_string(C.configValue(expr)));
    C.setConfigBlob(expr, {0x04, 0x11, 0x00});     // any program at all
    check(C.configValue(expr) == 1.0, "…and 1 once it holds a program",
          std::to_string(C.configValue(expr)));
    C.setConfigBlob(expr, {});                     // cleared: back to "no question asked"
    check(C.configValue(expr) == 0.0, "…back to 0 when it is cleared",
          std::to_string(C.configValue(expr)));

    std::printf("\n[control] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}

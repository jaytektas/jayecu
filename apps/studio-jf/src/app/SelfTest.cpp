#include "SelfTest.h"

// The includes this test needs in its own right — it drives the real widget classes and the panel model.
#include "../surface/CanvasWidget.h"
#include "../surface/PanelModel.h"
#include "../surface/WidgetRegistry.h"
#include "../surface/Surface.h"        // the higher-integration case drives a REAL Surface + its undo stack
#include "../surface/widgets/TableWidget.h"
#include "../model/Cache.h"

#include <j/core/SceneGraph.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Headless self-test of the owned-state architecture — runs the REAL widget classes, resolveElement cascade,
// and commit/undo logic without a window (STUDIO_SELFTEST=1). It verifies the exact invariants the "widgets are
// the running state, serialise only at boundaries" goal depends on: setSource hydrates the owned override store,
// setOwnProp authors onto the widget, commit writes ONLY overrides (inheritance preserved), clearOwnProp
// re-inherits, and an undo-style re-source restores prior state. Returns the failure count (0 = all pass).
int runOwnedStateSelfTest() {
    jf::JSceneGraph tg;
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::fprintf(stderr, "[selftest] %-54s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    };

    PanelElement el; el.id = 1; el.type = "value"; el.uid = "u-1";
    el.props["bgColor"] = "#111111";                       // one explicit override
    auto w = makeWidgetInstance("value", tg);
    check(!!w, "makeWidgetInstance(value)");
    if (!w) { std::fprintf(stderr, "[selftest] 1 failure(s)\n"); return 1; }

    // setSource: owned override store + resolved render copy.
    w->setSource(el);
    check(w->editElement().props.count("bgColor") && w->editElement().props.at("bgColor") == "#111111",
          "setSource: m_edit holds the override");
    check(w->element() && w->element()->prop("bgColor") == "#111111", "setSource: render copy resolves override");
    check(w->editElement().props.count("fgColor") == 0, "setSource: unset key absent from override store");

    // setOwnProp: author an edit straight onto the widget.
    w->setOwnProp("fgColor", "#abcdef");
    check(w->element()->prop("fgColor") == "#abcdef", "setOwnProp: render copy shows the edit");
    check(w->editElement().props.count("fgColor") && w->editElement().props.at("fgColor") == "#abcdef",
          "setOwnProp: override recorded in m_edit");

    // commit: ONLY overrides written back — a never-overridden prototype default must NOT be materialized.
    PanelElement out; out.id = 1; out.type = "value";
    w->commit(out);
    check(out.props.count("bgColor") && out.props.at("bgColor") == "#111111", "commit: original override written");
    check(out.props.count("fgColor") && out.props.at("fgColor") == "#abcdef", "commit: authored override written");
    std::string defKey;
    for (const jf::JProperty& p : w->properties().all())
        if (!p.meta.def.empty() && p.name != "bgColor" && p.name != "fgColor") { defKey = p.name; break; }
    if (!defKey.empty())
        check(out.props.count(defKey) == 0, "commit: unoverridden default NOT materialized (inheritance kept)");

    // clearOwnProp: drop the override → re-inherit.
    w->clearOwnProp("fgColor");
    check(w->editElement().props.count("fgColor") == 0, "clearOwnProp: override removed from m_edit");

    // Undo-style re-source: an authored value is visible, then a restore re-sources prior state.
    w->setOwnProp("bgColor", "#999999");
    check(w->element()->prop("bgColor") == "#999999", "pre-undo: edited value visible");
    w->setSource(el);                                     // ElementsSwap.undo → setElements → rehydrate
    check(w->element()->prop("bgColor") == "#111111", "undo(rehydrate): value restored");
    check(w->editElement().props.count("fgColor") == 0, "undo(rehydrate): stray override cleared");

    // TableWidget view: a menu-authored setting must ride the override store + commit path (not saveContent).
    if (auto t = makeWidgetInstance("table", tg)) {
        PanelElement te; te.id = 2; te.type = "table"; te.uid = "t-2";
        t->setSource(te);
        auto* tv = dynamic_cast<TableWidget*>(t.get());
        check(tv && tv->view() == 0, "table: default view is 0 (grid)");
        tv->setView(1);                                  // menu action → owned override
        check(tv->view() == 1, "table setView(1): member updated");
        check(t->element()->prop("view") == "1", "table setView(1): render copy shows view");
        check(t->editElement().props.count("view") == 1, "table setView(1): recorded as override");
        PanelElement tout; tout.id = 2; tout.type = "table"; t->commit(tout);
        check(tout.props.count("view") && tout.props.at("view") == "1", "table commit: view persisted");
        tv->setView(0);                                  // back to default → clears the override (re-inherits)
        check(tv->view() == 0 && t->editElement().props.count("view") == 0, "table setView(0): override cleared");
        PanelElement tout0; tout0.id = 2; tout0.type = "table"; t->commit(tout0);
        check(tout0.props.count("view") == 0, "table commit: default view NOT materialized");
    } else check(false, "makeWidgetInstance(table)");

    // Multi-field widget (label): commit writes every authored override, materializes none of the defaults.
    if (auto l = makeWidgetInstance("label", tg)) {
        PanelElement le; le.id = 3; le.type = "label"; le.uid = "l-3";
        l->setSource(le);
        l->setOwnProp("labelText", "Boost");
        PanelElement lout; lout.id = 3; lout.type = "label"; l->commit(lout);
        check(lout.props.count("labelText") && lout.props.at("labelText") == "Boost", "label commit: authored text written");
        check(lout.props.count("align") == 0 && lout.props.count("font") == 0, "label commit: untouched fields NOT materialized");
    } else check(false, "makeWidgetInstance(label)");

    // ---- Integration: the four live scenarios, driven end-to-end through a REAL PanelModel + serialize/load +
    // setSource rehydrate (everything the GUI does except pixel paint + input dispatch). ----

    // Scenario 4 — save → reopen round-trip: an authored override survives serialization; an unoverridden default
    // is NOT written to the file (inheritance preserved across the file boundary), yet resolves live on reopen.
    {
        PanelModel m;
        const int id = m.add("value", 0, 0, 100, 40);
        auto wa = makeWidgetInstance("value", tg);
        wa->setSource(*m.get(id));
        wa->setOwnProp("bgColor", "#abcdef");      // author an edit
        wa->commit(*m.get(id));                    // save-time flush (commitInstances path)
        const jf::JJson doc = m.toJson();          // serialize (file write)
        PanelModel m2; m2.load(doc);               // reopen (file read)
        check(m2.get(id) != nullptr, "roundtrip: element restored from file");
        check(m2.get(id) && m2.get(id)->props.count("bgColor") && m2.get(id)->props.at("bgColor") == "#abcdef",
              "roundtrip: authored override persisted to file");
        check(m2.get(id) && m2.get(id)->props.count("format") == 0,
              "roundtrip: unoverridden default NOT written to file (inheritance preserved)");
        auto wb = makeWidgetInstance("value", tg);
        wb->setSource(*m2.get(id));                // rebuild widget from reopened model
        check(wb->element()->prop("bgColor") == "#abcdef", "roundtrip: reopened widget shows override");
        check(wb->element()->prop("format") == "%.1f",     "roundtrip: reopened widget resolves the default live");
    }

    // Scenario 1 — undo: an edit is captured (ElementsSwap before/after via setElements), and restoring `before`
    // then re-sourcing the widget reverts it. Scenario 2 — a menu-style model.setProp rehydrates into the widget.
    {
        PanelModel m;
        const int id = m.add("value", 0, 0, 100, 40);
        auto w = makeWidgetInstance("value", tg);
        w->setSource(*m.get(id));
        const std::vector<PanelElement> before = m.elements();   // undo snapshot
        m.setProp(id, "bgColor", "#222222");                     // menu-authored edit
        w->setSource(*m.get(id));                                // refreshInstancesIfChanged rehydrate
        check(w->element()->prop("bgColor") == "#222222", "menu edit: rehydrate reflects model-authored change");
        m.setElements(before);                                   // doUndoRedo → ElementsSwap.undo
        w->setSource(*m.get(id));                                // rehydrate on gen bump
        check(w->element()->prop("bgColor") != "#222222", "undo: model restored + rehydrate reverts widget");
    }

    // Scenario 3 — clear-override: clearing a local override re-inherits the default after rehydrate.
    {
        PanelModel m;
        const int id = m.add("value", 0, 0, 100, 40, {{"format", "%.3f"}});
        auto w = makeWidgetInstance("value", tg);
        w->setSource(*m.get(id));
        check(w->element()->prop("format") == "%.3f", "clear: starts at the local override");
        m.clearProp(id, "format");                               // ✕ clear-override (model-authored)
        w->setSource(*m.get(id));                                // rehydrate
        check(w->element()->prop("format") == "%.1f", "clear: re-inherits the default after rehydrate");
    }

    // ---- Higher-integration: drive a REAL Surface + its real undo stack (ElementsSwap) + rehydrate, exercising
    // the exact chain the dock and Edit▸Undo invoke (elementsSnapshot → author on widget → commit → applyExternalEdit
    // → menuUndo/menuRedo → refreshInstancesIfChanged). Everything the GUI does on an edit/undo except pixels+input. ----
    {
        PanelModel m;
        const int id = m.add("value", 0, 0, 100, 40);
        Surface surf(tg, Cache::instance(), &m);
        surf.setMode(Surface::Mode::Edit);
        CanvasWidget* w = surf.widgetById(id);       // syncInstances → builds + setSource
        check(!!w, "surface: instance built from model");

        if (w) {
            // The dock's exact edit chain.
            const auto before = surf.elementsSnapshot();
            w->setOwnProp("bgColor", "#abcdef");
            if (PanelElement* me = m.get(id)) w->commit(*me);
            surf.applyExternalEdit("Edit bgColor", before, 700000 + id);
            check(m.get(id)->prop("bgColor") == "#abcdef", "surface edit: authored value reached the model");
            check(surf.canUndo(), "surface edit: a real undo step was pushed (ElementsSwap)");

            // Undo through the real stack, then rehydrate as the render loop would.
            surf.menuUndo();
            surf.refreshInstancesIfChanged();
            check(m.get(id)->prop("bgColor") != "#abcdef", "surface undo: model reverted via ElementsSwap");
            check(surf.widgetById(id) && surf.widgetById(id)->element()->prop("bgColor") != "#abcdef",
                  "surface undo: widget rehydrated to the reverted state");

            // Redo.
            surf.menuRedo();
            surf.refreshInstancesIfChanged();
            check(surf.widgetById(id) && surf.widgetById(id)->element()->prop("bgColor") == "#abcdef",
                  "surface redo: widget shows the re-applied edit");

            // Highest fidelity available in-process: drive undo through the REAL keyboard event handler as a
            // synthesized Ctrl+Z keystroke (Surface::handleKeyEvent → doUndoRedo). This exercises the app's own
            // key-dispatch path — one layer below is only the OS delivering the event, which no tool here can do.
            jf::JKeyEvent kz; kz.key = jf::JKeyEvent::JKey::Z; kz.ctrl = true; kz.pressed = true;
            const bool handled = surf.handleKeyEvent(kz);            // real editor keybinding: Ctrl+Z
            surf.refreshInstancesIfChanged();
            check(handled, "surface Ctrl+Z: real keyboard handler consumed the event");
            check(surf.widgetById(id) && surf.widgetById(id)->element()->prop("bgColor") != "#abcdef",
                  "surface Ctrl+Z (real key dispatch): edit undone, widget reverted");

            // Save-time flush: commitInstances writes overrides only (no default materialized).
            surf.menuRedo(); surf.refreshInstancesIfChanged();      // back to the edited state before the save check
            surf.commitInstances();
            check(m.get(id)->props.count("format") == 0, "surface commitInstances: default not materialized");
            check(m.get(id)->props.count("bgColor") == 1, "surface commitInstances: override present");
        }
    }

    // Scenario 4 with ACTUAL disk I/O: write with dumpToFile and read back with JJson::parseFile — the exact
    // functions saveAll()/loadDoc() call. This covers the real file write→read path, not just in-memory JJson.
    {
        PanelModel m;
        const int id = m.add("value", 0, 0, 100, 40);
        auto w = makeWidgetInstance("value", tg);
        w->setSource(*m.get(id));
        w->setOwnProp("bgColor", "#0affee");
        w->commit(*m.get(id));
        const char* path = "/tmp/studio_selftest_doc.gui";
        m.toJson().dumpToFile(path, 2);                  // real serialize-to-disk (saveAll path)
        PanelModel m2;
        bool read = false;
        try { m2.load(jf::JJson::parseFile(path)); read = true; } catch (...) {}
        check(read, "disk roundtrip: file written and parsed back");
        check(m2.get(id) && m2.get(id)->prop("bgColor") == "#0affee", "disk roundtrip: override survives real file write+read");
        check(m2.get(id) && m2.get(id)->props.count("format") == 0, "disk roundtrip: default NOT written to disk (inheritance)");
        std::remove(path);
    }

    std::fprintf(stderr, "[selftest] %d failure(s)\n", fails);
    return fails;
}

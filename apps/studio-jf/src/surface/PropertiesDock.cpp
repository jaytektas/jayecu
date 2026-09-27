#include "PropertiesDock.h"
#include <cmath>
#include "CanvasWidget.h"            // the selected element's widget instance — its JPropertyModel drives the form
#include "WidgetRegistry.h"
#include "../model/Cache.h"          // live "Current Value" readout + a bound channel's source unit
#include "../model/UnitManager.h"    // Display Unit picker: the convertible units of a channel's quantity
#include "../model/MetaModel.h"      // arrays1d/signalRange — the dictionary-binding (read-only) view

#include <j/core/Log.h>              // JLOGC — deferred per-type prototype + form build (surface.lazy)

#include <algorithm>
#include <cstdio>

using namespace jf;
using std::make_unique;

static constexpr float kRowH = 22.f;   // uniform field height

static int parseInt(const std::string& s, int dflt) {
    if (s.empty()) return dflt;
    try { return static_cast<int>(std::stod(s)); } catch (...) { return dflt; }
}
// A gauge's min/max may be an EXPRESSION rather than a number; the fallback is what a seeded zone uses
// when it is, which is the same answer as an unset bound.
static double parseDouble(const std::string& s, double dflt) {
    if (s.empty()) return dflt;
    try { return std::stod(s); } catch (...) { return dflt; }
}

PropertiesDock::PropertiesDock(JSceneGraph& g)
    : JWidget(g, "PropertiesDock"), g_(g), rulesForm_(g), canvasForm_(g), commonForm_(g), bindingForm_(g) {
    // --- Binding view (a dictionary entry) — rows are built per-entry in showForBinding(); only the
    //     host container is configured here, matching the common form's metrics.
    bindingForm_.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 12.f, 10.f, 12.f, 10.f });
    // --- Surface canvas view -------------------------------------------------------------------
    buildCanvasForm();

    // --- Common view (multi-select) — rows are built per-selection in populateCommon(); here we only
    //     configure the stable host container and the "all vs one" focus chooser.
    commonForm_.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 12.f, 10.f, 12.f, 10.f });
    // Value rules. A Form like the others: caption on the left, the rule's controls on the right. It was
    // built with NO layout mode at all, so its children were never positioned -- the expression field and
    // its fx button existed and were simply nowhere, which reads as them not having been written.
    rulesForm_.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 12.f, 10.f, 12.f, 10.f });

    // Focus combo (multi-select): "All N (common)" or one of the selected elements.
    focusCombo_ = make_unique<JComboBox>(g_, std::vector<std::string>{}, 220.f, kRowH);
    focusCombo_->onIndexChanged.connect([this](int i) { if (!comboGuard_) focusSelection(i); });

    scroll_ = make_unique<JScrollArea>(g_);   // hosts whichever form is active so overflow scrolls
    // The dock's two direct children. Everything else hangs below them: the scroll area re-parents
    // whichever form is active (paint(), below), and each form owns its own rows — so the whole dock
    // is one tree the framework can walk, hide and traverse without being told about it piecemeal.
    addChild(scroll_.get());
    addChild(focusCombo_.get());
}

// Row count of the active form → drives the form's natural content height for the scroll area.
int PropertiesDock::activeRowCount() const {
    if (view_ == View::Canvas)                 return static_cast<int>(canvasCaps_.size());
    if (view_ == View::Element && activeTF_)   return static_cast<int>(activeTF_->caps.size());
    if (view_ == View::Common)                 return static_cast<int>(commonCaps_.size());
    if (view_ == View::Binding)                return static_cast<int>(bindingCaps_.size());
    return 0;
}

void PropertiesDock::addCanvasRow(const char* caption, std::unique_ptr<JWidget> ed, char kind, const std::string& key) {
    canvasCaps_.push_back(make_unique<JLabel>(g_, caption, 90.f, kRowH));
    canvasForm_.add(canvasCaps_.back().get());
    canvasForm_.add(ed.get());
    // Canvas fields write the CANVAS (writeCanvas), not an element — so they wire their own writeCanvas signal.
    JWidget* e = ed.get();
    switch (kind) {
        case 't':
        case 'X': static_cast<JLineEdit*>(e)->onTextChanged.connect([this](const std::string&) { writeCanvas(); }); break;
        case 'k': static_cast<JColorButton*>(e)->onColorChanged.connect([this](const std::string&) { writeCanvas(); }); break;
        case 'f': static_cast<JFontButton*>(e)->onFontChanged.connect([this](const std::string&) { writeCanvas(); }); break;
        case 'n': static_cast<JSpinBox*>(e)->onValueChanged.connect([this](int) { writeCanvas(); }); break;
        case 'c': static_cast<JComboBox*>(e)->onIndexChanged.connect([this](int) { writeCanvas(); }); break;
        default: break;
    }
    canvasFields_.push_back({ key, kind, ed.get() });
    canvasEditors_.push_back(std::move(ed));
}

void PropertiesDock::buildCanvasForm() {
    canvasForm_.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 12.f, 10.f, 12.f, 10.f });

    auto combo = [&](std::vector<std::string> items) { return make_unique<JComboBox>(g_, std::move(items), 170.f, kRowH); };
    auto spin  = [&](int lo, int hi) { return make_unique<JSpinBox>(g_, lo, hi, 120.f, kRowH); };
    auto color = [&]() { auto c = make_unique<JColorButton>(g_, 200.f, kRowH); c->setInheritable(true); return c; };

    // The canvas is a widget too — show its own UID first, read-only but selectable so it can be copied.
    { auto le = make_unique<JLineEdit>(g_, "", 200.f, kRowH); le->setReadOnly(true);
      addCanvasRow("Widget ID", std::move(le), 'R', "uid"); }
    addCanvasRow("Width",   spin(0, 7680), 'n', "w");
    addCanvasRow("Height",  spin(0, 4320), 'n', "h");
    // REFLOW IS ONE OF THE ANSWERS, and it was missing — so a reflowing page (every imported one, and
    // the tab surface itself) showed "Scale to fit" here, which is a different mode and the one that
    // shrinks text. An author reading that would have been told the wrong thing, and an author touching
    // any other row on this panel would have written it back and turned reflow into scaling.
    addCanvasRow("Scaling", combo({ "Inherit (global)", "Fixed size (1:1)", "Scale to fit",
                                    "Reflow (page is the view)" }), 'c', "static");
    // Where a canvas SMALLER than its viewport sits. Per canvas: a page canvas and a small canvas hosted
    // in a viewport want different answers, and the old single global preference could express neither.
    addCanvasRow("Anchor", combo({ "Inherit (global)",
                                   "Top-left", "Top", "Top-right",
                                   "Left", "Centre", "Right",
                                   "Bottom-left", "Bottom", "Bottom-right" }), 'c', "canvasAnchor");

    // Editing guide — a compound row: two spins (W×H, virtual units) + a clear button.
    guideRow_ = make_unique<JContainer>(g_);
    guideRow_->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(6.f);
    // Fit the editor column (~161px): 2 spins + gaps + the clear button. At 90px each the row measured
    // 220px, so the clear button was laid out past the dock's right edge -- clipped, unclickable, and (until
    // the focus work) an invisible Tab stop at x=1923 on a 1920px screen.
    guideW_ = make_unique<JSpinBox>(g_, 0, 7680, 58.f, kRowH);
    guideH_ = make_unique<JSpinBox>(g_, 0, 4320, 58.f, kRowH);
    guideClear_ = make_unique<JButton>(g_, "x", 28.f, kRowH);
    guideW_->onValueChanged.connect([this](int) { writeCanvas(); });
    guideH_->onValueChanged.connect([this](int) { writeCanvas(); });
    guideClear_->onClicked.connect([this] { if (suppress_ || !model_) return; guideW_->setValue(0); guideH_->setValue(0); model_->setGuideSize(0, 0); if (onInvalidate) onInvalidate(); });
    guideRow_->add(guideW_.get()); guideRow_->add(guideH_.get()); guideRow_->add(guideClear_.get());
    canvasCaps_.push_back(make_unique<JLabel>(g_, "Editing guide", 90.f, kRowH));
    canvasForm_.add(canvasCaps_.back().get());
    canvasForm_.add(guideRow_.get());

    auto title = make_unique<JLineEdit>(g_, "(empty = no border; \".\" = frame, no title)", 200.f, kRowH);
    addCanvasRow("Title", std::move(title), 't', "title");
    addCanvasRow("Layout", combo({ "Free", "Y Axis", "X Axis", "Border", "Card", "Index Card", "Grid" }), 'c', "layout");
    addCanvasRow("Grid Columns", spin(1, 12), 'n', "gridColumns");
    addCanvasRow("Focus Index",  spin(0, 63), 'n', "focusIndex");
    addCanvasRow("Border Width", spin(0, 50), 'n', "borderWidth");
    addCanvasRow("Border", color(), 'k', "borderColor");
    addCanvasRow("Line style", combo({ "Solid", "Dash", "Dot", "Dash-Dot", "Dash-Dot-Dot" }), 'c', "borderStyle");
    addCanvasRow("Border Radius", spin(0, 80), 'n', "borderRadius");
    { auto fb = make_unique<JFontButton>(g_, 200.f, kRowH); fb->setInheritable(true);
      addCanvasRow("Title Font", std::move(fb), 'f', "titleFont"); }
    addCanvasRow("Title Colour", color(), 'k', "titleColor");
    addCanvasRow("Title Padding", spin(0, 40), 'n', "titlePadding");
    addCanvasRow("Title Style", combo({ "Plain", "Underline", "Filled" }), 'c', "titleStyle");
    addCanvasRow("Title Place", combo({ "Inside", "Outside" }), 'c', "titlePlace");
    addCanvasRow("Title Edge",  combo({ "Top", "Bottom" }), 'c', "titleEdge");
    addCanvasRow("Title Align", combo({ "Left", "Centre", "Right" }), 'c', "titleAlign");
}

void PropertiesDock::wireChangeTo(JWidget* ed, char kind, bool multi) {
    // A change in either form runs the matching writer; both re-read every row and coalesce to one undo step.
    auto write = [this, multi] { if (multi) writeCommon(); else writeElement(); };
    switch (kind) {
        case 't':
        case 'X':            static_cast<JLineEdit*>(ed)->onTextChanged.connect([write](const std::string&) { write(); }); break;
        case 'k':            static_cast<JColorButton*>(ed)->onColorChanged.connect([write](const std::string&) { write(); }); break;
        case 'f':            static_cast<JFontButton*>(ed)->onFontChanged.connect([write](const std::string&) { write(); }); break;
        case 'b':            static_cast<JCheckBox*>(ed)->onStateChanged.connect([write](bool) { write(); }); break;
        case 'd':            static_cast<JDoubleSpinBox*>(ed)->onValueChanged.connect([write](double) { write(); }); break;
        case 'r': case 'R': case 'S': case 'N': break;   // read-only display / picker button — no edit signal
        case 'n': case 'g':  static_cast<JSpinBox*>(ed)->onValueChanged.connect([write](int) { write(); }); break;
        case 'c': case 'e': case 'u':  static_cast<JComboBox*>(ed)->onIndexChanged.connect([write](int) { write(); }); break;
        default: break;
    }
}

// A JProperty → the editor widget + kind char to represent it. Shared by the per-type form and the
// multi-select common form; the caller wires the change/✕ (single vs fan). kind: t(ext) n(um) g(eom)
// c(hoice) e(num) u(nit) k(color) f(ont) b(ool) d(ouble) S(ource) R(read-only id) r(read-only label).
std::pair<std::unique_ptr<JWidget>, char> PropertiesDock::makeEditor(const jf::JProperty& p, const std::string& key) {
    const bool geom = (key == "x" || key == "y" || key == "w" || key == "h");
    const int lo = p.meta.min.isNull() ? 0   : static_cast<int>(p.meta.min.toInt());
    const int hi = p.meta.max.isNull() ? 100 : static_cast<int>(p.meta.max.toInt());

    if (geom) return { make_unique<JSpinBox>(g_, lo, hi, 120.f, kRowH), 'g' };
    if (!p.writable()) {
        if (key == "uid") {   // Widget ID: read-only but SELECTABLE (Ctrl+C) so it can be pasted into equations
            auto le = make_unique<JLineEdit>(g_, "", 200.f, kRowH); le->setReadOnly(true);
            return { std::move(le), 'R' };
        }
        return { make_unique<JLabel>(g_, "", 200.f, kRowH), 'r' };
    }
    if (p.meta.editor == "color") {
        // Colour/font are the dual-function picker buttons: click to pick, click the inline "x" to clear the
        // override. That inline clear IS the reset for these rows (addRow adds no external button for k/f).
        auto cb = make_unique<JColorButton>(g_, 200.f, kRowH); cb->setInheritable(true);
        return { std::move(cb), 'k' };
    }
    if (p.meta.editor == "font") {
        auto fb = make_unique<JFontButton>(g_, 200.f, kRowH); fb->setInheritable(true);
        return { std::move(fb), 'f' };
    }
    if (p.meta.editor == "signal") {
        auto btn = make_unique<JButton>(g_, "(pick source…)", 200.f, kRowH);
        const std::string k = key;
        btn->onClicked.connect([this, k] { pickSource(k); });
        return { std::move(btn), 'S' };
    }
    if (p.meta.editor == "node") {
        auto btn = make_unique<JButton>(g_, "(pick node…)", 200.f, kRowH);
        const std::string k = key;
        btn->onClicked.connect([this, k] { pickNode(k); });
        return { std::move(btn), 'N' };
    }
    if (p.meta.editor == "unit")   // items depend on the element's signalName → (re)built per element in populate
        return { make_unique<JComboBox>(g_, std::vector<std::string>{}, 170.f, kRowH), 'u' };
    if (p.meta.editor == "expr") {  // sigil-capable: a plain line edit, PLUS an "fx" button (addRow) that
        auto le = make_unique<JLineEdit>(g_, "", 200.f, kRowH);         // opens the ExpressionEditor
        le->setClearButtonEnabled(true);   // no condition is a state, and it needs a press to reach
        return { std::move(le), 'X' };
    }
    if (p.meta.editor == "enum") {   // combo whose STORED value is the option INDEX (0-based), not the label
        std::vector<std::string> ch; ch.reserve(p.meta.choices.size());
        for (const jf::JVariant& c : p.meta.choices) ch.push_back(c.toString());
        return { make_unique<JComboBox>(g_, ch, 170.f, kRowH), 'e' };
    }
    if (!p.meta.choices.empty()) {
        std::vector<std::string> ch; ch.reserve(p.meta.choices.size());
        for (const jf::JVariant& c : p.meta.choices) ch.push_back(c.toString());
        return { make_unique<JComboBox>(g_, ch, 170.f, kRowH), 'c' };
    }
    const jf::JVariant sample = p.get();   // fresh instance's default value → stable type probe
    if (sample.isBool())
        return { make_unique<JCheckBox>(g_, "", 120.f, kRowH), 'b' };
    if (sample.isDouble() && p.meta.decimals > 0) {
        const double mn = p.meta.min.isNull() ? 0.0 : p.meta.min.toDouble();
        const double mx = p.meta.max.isNull() ? 100.0 : p.meta.max.toDouble();
        const double st = p.meta.step.isNull() ? 1.0 : p.meta.step.toDouble();
        auto sp = make_unique<JDoubleSpinBox>(g_, mn, mx, st, p.meta.decimals, 120.f, kRowH);
        if (!p.meta.suffix.empty()) sp->setSuffix(p.meta.suffix);
        return { std::move(sp), 'd' };
    }
    if (sample.isNumber())
        return { make_unique<JSpinBox>(g_, lo, hi, 120.f, kRowH), 'n' };
    return { make_unique<JLineEdit>(g_, "", 200.f, kRowH), 't' };
}

// Append one row (caption + editor, plus a "✕" reset-to-default beside inheritable editors) to `form`,
// recording it in the parallel caps/owned/fields vectors. The editor + ✕ + wrapper are all owned so they're
// destroyed with the form and toggled by applyVisibility. multi selects the fan-out target (single / all).
void PropertiesDock::addRow(JContainer& form, std::vector<std::unique_ptr<JLabel>>& caps,
                            std::vector<std::unique_ptr<JWidget>>& owned, std::vector<Field>& fields,
                            const char* caption, std::unique_ptr<JWidget> field, char kind,
                            const std::string& key, bool inheritable, bool multi) {
    caps.push_back(make_unique<JLabel>(g_, caption, 90.f, kRowH));
    form.add(caps.back().get());

    JWidget* editor = field.get();
    wireChangeTo(editor, kind, multi);
    fields.push_back({ key, kind, editor });

    // The "x" clears a local OVERRIDE back to the inherited default, so it only belongs on INHERITABLE rows.
    // Colour/font ('k'/'f') clear via their own inline "x" (JColorButton/JFontButton). Every other inheritable
    // row gets a small external "x". Non-inheritable rows (scale min/max, label format, geometry, …) have no
    // override to clear, so they get nothing.
    const bool inlineClear   = (kind == 'k' || kind == 'f');
    // A sigil-capable ('X') row carries an "fx" button that opens the ExpressionEditor seeded with the
    // field's current text; typing still works, the button is just the builder. It sits where the reset
    // "x" would, and takes precedence (an expr field is not inheritable, so the two never both apply).
    if (kind == 'X') {
        auto wrap = make_unique<JContainer>(g_);
        wrap->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(4.f);
        auto fx = make_unique<JButton>(g_, "fx", kRowH, kRowH);   // plain ASCII — the ƒ hook (U+0192) has no glyph and renders as "?x"
        fx->setTooltip("Build a sigil expression");
        const std::string k = key;
        fx->onClicked.connect([this, k] { openExpr(k); });
        editor->setHSizePolicy(JSizePolicyMode::Expanding, 1);
        wrap->add(field.get());
        wrap->add(fx.get());
        form.add(wrap.get());
        owned.push_back(std::move(field));
        owned.push_back(std::move(fx));
        owned.push_back(std::move(wrap));
        return;
    }
    // A picker row ('S' source, 'N' node) needs the SAME "x" as any other override: picking is one-way
    // otherwise — the popup offers no "none" entry, so once a viewport had a data source there was no way
    // to take it off again, at all. Clearing writes an empty value through the normal clearOverride path,
    // which is exactly what an unbound element carries.
    const bool pickerClear   = (kind == 'S' || kind == 'N');
    const bool externalReset = (inheritable || pickerClear) && (kind != 'g' && kind != 'r' && kind != 'R' && !inlineClear);
    if (externalReset) {
        auto wrap = make_unique<JContainer>(g_);
        wrap->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(4.f);
        auto x = make_unique<JButton>(g_, "x", kRowH, kRowH);
        const std::string k = key;
        x->onClicked.connect([this, k, multi] { clearOverride(k, multi); });
        // The editor FILLS the cell and the "x" stays fixed at the right — otherwise a wide (200px) editor plus
        // the button overflows the narrow dock column and the button is clipped. Expanding shrinks it to fit.
        editor->setHSizePolicy(JSizePolicyMode::Expanding, 1);
        wrap->add(field.get());
        wrap->add(x.get());
        form.add(wrap.get());
        owned.push_back(std::move(field));
        owned.push_back(std::move(x));
        owned.push_back(std::move(wrap));
    } else {
        form.add(field.get());
        owned.push_back(std::move(field));
    }
}

// A cached prototype instance per widget type — built once via the factory, it supplies the type's
// JPropertyModel for form-building. Replaces reaching into a live surface for the instance.
CanvasWidget* PropertiesDock::instanceFor(const std::string& type) {
    auto it = protos_.find(type);
    if (it == protos_.end()) {
        JLOGC("surface.lazy", jf::JLogLevel::Info) << "PropertiesDock: build prototype for type=" << type;
        it = protos_.emplace(type, makeWidgetInstance(type, g_)).first;
    }
    return it->second.get();
}

PropertiesDock::TypeForm* PropertiesDock::typeForm(const PanelElement& e) {
    if (auto it = typeForms_.find(e.type); it != typeForms_.end()) return it->second.get();

    auto tf = make_unique<TypeForm>();
    tf->form = make_unique<JContainer>(g_);
    tf->form->setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 12.f, 10.f, 12.f, 10.f });

    // Rows come from the WIDGET'S OWN JPropertyModel — its collectProperties() is the single source of
    // truth for the type (the JFramework-native Q_PROPERTY + propertyMetaData()). A type with no ported
    // class yet (nullptr instance) gets an empty form. Designable rows only, sorted by meta.order; the
    // meta's editor kind + the value type pick the editor widget. Values still round-trip through the
    // PanelElement prop store (keys == property names), so populate/write below are unchanged.
    CanvasWidget* inst = instanceFor(e.type);
    if (inst) {
        std::vector<const jf::JProperty*> rows;
        for (const jf::JProperty& p : inst->properties().all())
            if (p.meta.designable) rows.push_back(&p);
        std::stable_sort(rows.begin(), rows.end(),
                         [](const jf::JProperty* a, const jf::JProperty* b) { return a->meta.order < b->meta.order; });

        for (const jf::JProperty* pp : rows) {
            const jf::JProperty& p = *pp;
            const std::string& key = p.name;
            const std::string capStr = p.meta.label.empty() ? key : p.meta.label;
            auto [ed, kind] = makeEditor(p, key);
            addRow(*tf->form, tf->caps, tf->editors, tf->fields, capStr.c_str(), std::move(ed), kind, key,
                   p.meta.inheritable, /*multi=*/false);
        }
    }

    TypeForm* raw = tf.get();
    JLOGC("surface.lazy", jf::JLogLevel::Info)
        << "PropertiesDock: build property form for type=" << e.type << " rows=" << raw->fields.size();
    typeForms_[e.type] = std::move(tf);
    return raw;
}

JContainer* PropertiesDock::activeForm() {
    if (view_ == View::Canvas) return &canvasForm_;
    if (view_ == View::Common) return &commonForm_;
    if (view_ == View::Element && activeTF_) return activeTF_->form.get();
    if (view_ == View::Binding) return &bindingForm_;
    return nullptr;
}

// One form is shown at a time; the rest are hidden. Hiding the FORM is the whole job — every caption and
// editor in it is a child of it, so effective visibility takes the rows with it, and focus traversal
// prunes the subtree at the hidden form rather than tab-cycling through editors laid out underneath the
// one on screen. This used to walk each form's caps/editors/compound-row members by hand, in two
// directions, and any row built through a path that list did not know about stayed a phantom tab stop.
void PropertiesDock::applyVisibility() {
    canvasForm_.setVisible(view_ == View::Canvas);
    rulesForm_.setVisible(view_ == View::Element && elemId_ != 0);
    commonForm_.setVisible(view_ == View::Common);
    bindingForm_.setVisible(view_ == View::Binding);
    for (auto& [type, tf] : typeForms_) {
        (void)type;
        tf->form->setVisible(view_ == View::Element && tf.get() == activeTF_);
    }
    // The multi-select chooser is not in any form — it sits above them, and shows only when there is
    // more than one element to choose between.
    focusCombo_->setVisible(comboActive());
}

// Push the EFFECTIVE value of one field into its editor: `e` is the element (geometry off its rect), `re` the
// resolved element (own prop, else per-type / built-in default), so an unset property shows its default rather
// than blank. Shared by populateElement, populateCommon (seeded from the primary), and clearOverride's refresh.
void PropertiesDock::loadField(const Field& f, const PanelElement& e, const PanelElement& re) {
    if (f.kind == 'g') {
        const int v = f.key == "x" ? static_cast<int>(e.x) : f.key == "y" ? static_cast<int>(e.y)
                    : f.key == "w" ? static_cast<int>(e.w) : static_cast<int>(e.h);
        static_cast<JSpinBox*>(f.editor)->setValue(v);
    } else if (f.kind == 'n') {
        static_cast<JSpinBox*>(f.editor)->setValue(parseInt(re.prop(f.key), 0));
    } else if (f.kind == 'c') {
        auto* cb = static_cast<JComboBox*>(f.editor);
        const std::string cur = re.prop(f.key);
        int idx = 0;
        for (size_t i = 0; i < cb->items().size(); ++i) if (cb->items()[i] == cur) { idx = static_cast<int>(i); break; }
        cb->setCurrentIndex(idx);
    } else if (f.kind == 'e') {   // enum: stored value is the option index
        auto* cb = static_cast<JComboBox*>(f.editor);
        int idx = parseInt(re.prop(f.key), 0);
        cb->setCurrentIndex(idx >= 0 && idx < static_cast<int>(cb->items().size()) ? idx : 0);
    } else if (f.kind == 'u') {   // Display Unit: rebuild the item list from the bound channel's quantity
        auto* cb = static_cast<JComboBox*>(f.editor);
        std::vector<std::string> items = { "Auto", "Raw" }, ids = { "Auto", "Raw" };
        // Resolve a templated source ("[*]"/"$*") to a concrete channel first, using the element in scope —
        // otherwise "$*" never names a channel, its quantity is unknown, and the picker offers only Auto/Raw.
        const std::string bind = MathEvaluator::resolveTemplate(e.prop("signalName"), elementScope_);
        std::string src = CanvasWidget::bindingUnit(bind);   // path OR sigil -- "[$rpm]" is a binding too
        // An AXIS unit picker offers the AXIS's quantity, not the cells'. On a sensor calibration those
        // are different things entirely — the cells are kPa or °C, the axis is ADC counts — and offering
        // the cells' units for the axis would let you ask for a raw pin input in degrees.
        if (f.key.rfind("displayUnit", 0) == 0 && f.key.size() == 12) {
            const int axis = f.key[11] == 'X' ? 0 : f.key[11] == 'Y' ? 1 : f.key[11] == 'Z' ? 2 : -1;
            src = (axis < 0) ? std::string()
                             : Cache::instance().axisDomain(Cache::instance().resolveTable(bind), axis).units;
        }
        if (!src.empty()) {
            const std::string q = UnitManager::instance().findQuantityForUnit(src);
            if (!q.empty()) for (const auto& u : UnitManager::instance().getQuantity(q).units) {
                items.push_back(u.label); ids.push_back(u.id);
            }
        }
        unitIds_[f.key] = ids;              // parallel id list for writeback (combo shows labels)
        cb->setItems(items);
        const std::string cur = re.prop(f.key);   // "Auto" / "Raw" / a unit id
        int idx = 0;
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == cur) { idx = static_cast<int>(i); break; }
        cb->setCurrentIndex(idx);
    } else if (f.kind == 'k') {
        // Seed from the raw OVERRIDE, not the resolved value: the picker shows "(scheme)" when inheriting
        // and its inline ✕ (clear-override) appears iff there's a local override — identical for every
        // widget regardless of per-type defaults.
        static_cast<JColorButton*>(f.editor)->setColorHex(e.prop(f.key));
    } else if (f.kind == 'f') {
        static_cast<JFontButton*>(f.editor)->setFontSpec(e.prop(f.key));
    } else if (f.kind == 'b') {
        const std::string v = re.prop(f.key);
        static_cast<JCheckBox*>(f.editor)->setChecked(v == "1" || v == "true");
    } else if (f.kind == 'd') {
        double v = 0; try { v = std::stod(re.prop(f.key)); } catch (...) {}
        static_cast<JDoubleSpinBox*>(f.editor)->setValue(v);
    } else if (f.kind == 'R') {   // Widget ID — the selectable read-only field
        static_cast<JLineEdit*>(f.editor)->setText(e.uid);
    } else if (f.kind == 'r') {
        // "Current Value" tracks live telemetry (not a stored prop); other read-only rows show their prop.
        static_cast<JLabel*>(f.editor)->setText(
            f.key == "value" ? CanvasWidget::fmtVal(re, Cache::instance()) : re.prop(f.key));
    } else if (f.kind == 'S') {
        const std::string v = re.prop(f.key);
        static_cast<JButton*>(f.editor)->setLabel(v.empty() ? "(pick source…)" : v);
    } else if (f.kind == 'N') {
        const std::string v = re.prop(f.key);
        static_cast<JButton*>(f.editor)->setLabel(v.empty() ? "(pick node…)" : v);
    } else {
        static_cast<JLineEdit*>(f.editor)->setText(re.prop(f.key));
    }
}

// --- Binding (dictionary entry) view -----------------------------------------------------------------
// An EDITABLE value row: a spin box bound straight to the path. Used for host-side (pc.*) entries, whose
// value the studio owns — a write lands in the host cache block and travels with the tune, never the wire.
jf::JWidget* PropertiesDock::addBindingValueEditor(const std::string& path) {
    Cache& c = Cache::instance();
    // What the field may hold: its declared range, or its datatype's when it declares none. This used to
    // read min == max as "unbounded" and open a +/-1e9 box — the old meaning, from when undeclared meant
    // unenforced. It has not meant that since bounds fell back to the type, so the box was offering a
    // billion on a field the storage caps at 255.
    double lo = 0.0, hi = 0.0;
    Cache::rawBounds(c.meta() ? c.meta()->locate(path) : MetaModel::Location{}, lo, hi);
    const bool bounded = std::isfinite(lo) && std::isfinite(hi) && hi > lo;   // floats have no integer cap
    const int  digits  = c.digits(path);

    bindingCaps_.push_back(make_unique<JLabel>(g_, "Value", 90.f, kRowH));
    auto spin = make_unique<JDoubleSpinBox>(g_, bounded ? lo : -1e9, bounded ? hi : 1e9,
                                            std::pow(10.0, -digits), digits, 200.f, kRowH);
    spin->setValue(c.configValue(path) * c.configScale(path));
    const double scale = c.configScale(path);
    spin->onValueChanged.connect([path, scale](double disp) {
        Cache::instance().setConfigValue(path, scale != 0.0 ? disp / scale : disp);   // RAW in, as always
    });
    bindingForm_.add(bindingCaps_.back().get());
    bindingForm_.add(spin.get());
    jf::JWidget* raw = spin.get();
    bindingEditors_.push_back(std::move(spin));
    return raw;
}

void PropertiesDock::addBindingRow(const char* caption, const std::string& value) {
    bindingCaps_.push_back(make_unique<JLabel>(g_, caption, 90.f, kRowH));
    auto val = make_unique<JLabel>(g_, value, 200.f, kRowH);
    bindingForm_.add(bindingCaps_.back().get());
    bindingForm_.add(val.get());
    bindingEditors_.push_back(std::move(val));
}

// Read-only inspector for a dictionary entry. Everything shown comes from the meta via Cache — this view
// never writes, so there is no undo baseline and no Field list (nothing to read back out of an editor).
void PropertiesDock::showForBinding(const std::string& path) {
    bindingForm_.clear();
    bindingCaps_.clear();
    bindingEditors_.clear();
    if (path.empty()) { view_ = View::None; header_.clear(); applyVisibility(); invalidate(); return; }

    const Cache& c = Cache::instance();
    view_ = View::Binding; elemId_ = 0; activeTF_ = nullptr; selIds_.clear();
    header_ = c.label(path).empty() ? path : c.label(path);

    auto num = [](double v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };

    addBindingRow("Path", path);

    // A PcVariable is host-side: the studio owns its storage, so unlike every other row here its VALUE is
    // editable in place. Everything else in this view describes the ECU's dictionary and is read-only by
    // nature — you cannot edit a coolant channel's value, only the sensor can.
    if (path.rfind("pc.", 0) == 0) {
        addBindingRow("Storage", "host (no ECU storage)");
        auto* spin = addBindingValueEditor(path);
        (void)spin;
    }

    const std::string kind = c.controlFor(path);
    addBindingRow("Kind", kind.empty() ? "(unknown)" : kind);
    { const std::string u = c.unit(path); if (!u.empty()) addBindingRow("Units", u); }

    if (c.isTable(path)) {
        const TableImage t = c.resolveTable(path);
        if (!t.valid) { addBindingRow("Status", "unresolved"); applyVisibility(); invalidate(); return; }
        addBindingRow("Datatype", t.cellType);
        addBindingRow("Scale", num(t.cellScale));
        // The schema cell bounds — min == max means the schema declares none.
        addBindingRow("Cell Min", t.cellMaxV > t.cellMinV ? num(t.cellMinV) : "(unbounded)");
        addBindingRow("Cell Max", t.cellMaxV > t.cellMinV ? num(t.cellMaxV) : "(unbounded)");
        addBindingRow("Size", std::to_string(c.liveCols(path)) + " x " + std::to_string(c.liveRows(path))
                              + (c.liveDepth(path) > 1 ? " x " + std::to_string(c.liveDepth(path)) : ""));
        addBindingRow("Cell offset", std::to_string(t.cellBase));
        // One row per axis: its label, live bin count, and the signal currently driving it (srcBase < 0 =
        // the axis has no selector, so it is whatever the schema fixed it to).
        static const char* kAxisName[3] = { "X axis", "Y axis", "Z axis" };
        for (size_t a = 0; a < t.axes.size() && a < 3; ++a) {
            const TableImage::Axis& ax = t.axes[a];
            std::string v = ax.label.empty() ? std::string("(unnamed)") : ax.label;
            v += "  " + std::to_string(c.tiLiveN(t, static_cast<int>(a))) + " bins";
            if (!ax.units.empty()) v += "  " + ax.units;
            addBindingRow(kAxisName[a], v);
        }
    } else if (c.isConfig(path)) {
        // DECLARED bounds, and — when there are none — the ones the datatype imposes, said as such.
        // "(unbounded)" was true when undeclared meant unenforced; a field with no declared range is
        // still capped by what its bytes can hold, and saying otherwise misreads the tune's limits.
        const double dlo = c.configMin(path), dhi = c.configMax(path);
        double tlo = 0.0, thi = 0.0;
        Cache::rawBounds(c.meta() ? c.meta()->locate(path) : MetaModel::Location{}, tlo, thi);
        const bool declared = dhi > dlo;
        addBindingRow("Value", num(c.configValue(path)));
        addBindingRow("Min", declared ? num(dlo) : (std::isfinite(tlo) ? num(tlo) + " (datatype)" : "(unbounded)"));
        addBindingRow("Max", declared ? num(dhi) : (std::isfinite(thi) ? num(thi) + " (datatype)" : "(unbounded)"));
        addBindingRow("Decimals", std::to_string(c.digits(path)));
    } else if (const MetaModel* mm = c.meta()) {
        // 1D arrays and telemetry channels — neither is a table nor a config scalar.
        const auto it = mm->arrays1d().find(path);
        if (it != mm->arrays1d().end()) {
            const MetaModel::ConfigField& a = it->second;
            addBindingRow("Datatype", a.datatype);
            addBindingRow("Scale", num(a.scale));
            addBindingRow("Count", std::to_string(a.count));
            addBindingRow("Min", a.maxV > a.minV ? num(a.minV) : "(unbounded)");
            addBindingRow("Max", a.maxV > a.minV ? num(a.maxV) : "(unbounded)");
            addBindingRow("Offset", std::to_string(a.offset));
        } else {
            double lo = 0, hi = 0;
            addBindingRow("Range", mm->signalRange(path, lo, hi) ? num(lo) + " .. " + num(hi) : "(unbounded)");
        }
    }
    applyVisibility();
    invalidate();
}

// ---------------------------------------------------------------------------------------------------
// VALUE RULES — inline, growable, per element.
// ---------------------------------------------------------------------------------------------------
int PropertiesDock::rulesRowCount() const {
    if (view_ != View::Element || !elemId_) return 0;
    return 1 + 2 * static_cast<int>(ruleRows_.size()) + 1   // header + TWO rows per rule + Add
         + zonesRowCount();                                 // ... and the zones section stacked under it
}

int PropertiesDock::zonesRowCount() const {
    if (zoneRows_.empty() && !zonesOffered_) return 0;      // a widget that paints no zone shows no section
    return 1 + static_cast<int>(zoneRows_.size()) + 1;      // header + one row per zone + Add
}

// Serialise `rules_` into the element's "ranges" prop. ONE path: the instance owns the authored props
// when there is one (its m_edit is what commit() writes back), so a model-level setProp beside it is a
// write into a copy that the next commit silently overwrites. Add and Remove used to do exactly that,
// which is why the first rule stuck and the second vanished.
void PropertiesDock::commitRules() {
    if (!model_ || !elemId_) return;
    ColorRules rr; rr.rules = rules_;
    const std::string compact = rr.toCompact();
    if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(elemId_) : nullptr) {
        inst->setOwnProp("ranges", compact);
        if (PanelElement* me = model_->get(elemId_)) inst->commit(*me);
    } else {
        model_->setProp(elemId_, "ranges", compact);
    }
    if (onInvalidate) onInvalidate();
    if (onApplyEdit) onApplyEdit("Value rules", editBefore_, 720000 + elemId_);
}

// Pull every row's current state into rules_, then commit.
void PropertiesDock::writeRules() {
    if (suppress_ || !model_ || !elemId_) return;
    for (size_t i = 0; i < ruleRows_.size() && i < rules_.size(); ++i) {
        const RuleRow& r = ruleRows_[i];
        rules_[i].bg     = r.bg->colorHex();     rules_[i].fg     = r.fg->colorHex();
        rules_[i].accent = r.accent->colorHex(); rules_[i].border = r.border->colorHex();
        rules_[i].blink  = r.blink->isChecked();
        rules_[i].when   = r.when->text();
        if (r.text) rules_[i].text = r.text->text();
    }
    commitRules();
}

// Form layout pairs children: caption, control, caption, control. A rule (and a zone) needs more than one
// control, so each gets a sub-container -- the same shape addRow() uses for its expr+fx pair.
jf::JWidget* PropertiesDock::ruleOwn(std::unique_ptr<JWidget> w) {
    JWidget* p = w.get();
    rulesOwned_.push_back(std::move(w));
    return p;
}
void PropertiesDock::ruleCap(const char* text) {
    rulesForm_.add(ruleOwn(make_unique<JLabel>(g_, text, 78.f, kRowH)));
}
jf::JContainer* PropertiesDock::ruleRowBox() {
    auto* c = static_cast<JContainer*>(ruleOwn(make_unique<JContainer>(g_)));
    c->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(4.f);
    rulesForm_.add(c);
    return c;
}

void PropertiesDock::buildRulesForm(const PanelElement* e) {
    using namespace jf;
    rulesForm_.clear();
    ruleRows_.clear();
    rulesOwned_.clear();
    rules_.clear();
    zoneRows_.clear();
    zones_.clear();
    zonesOffered_ = false;
    if (!e) return;
    rules_ = ColorRules::fromCompact(e->prop("ranges")).rules;

    auto own    = [&](std::unique_ptr<JWidget> w) { return ruleOwn(std::move(w)); };
    auto cap    = [&](const char* text) { ruleCap(text); };
    auto rowBox = [&]() { return ruleRowBox(); };

    // WHAT EACH SWATCH IS FOR. Four coloured squares in a row say nothing about which channel each one
    // paints; named once here, above columns of the same widths, they say it for every rule below.
    cap("Value rules");
    {
        JContainer* h = rowBox();
        for (const char* t : { "Bg", "Text", "Accent", "Border" })
            h->add(own(make_unique<JLabel>(g_, t, kSwatchW, kRowH)));
        h->add(own(make_unique<JLabel>(g_, "Blink", 54.f, kRowH)));
    }

    for (size_t i = 0; i < rules_.size(); ++i) {
        const int idx = static_cast<int>(i);
        RuleRow row;
        cap(("Rule " + std::to_string(i + 1)).c_str());
        // TWO FORM ROWS per rule, not one row holding two lines. A Form pairs children caption/control and
        // sizes each pair to ONE row height, so a column container in the control slot had its second line
        // clipped away -- the expression field and its fx button were laid out, and then cut off below the
        // row. Emitting a second caption/control pair gives that line a row of its own, which the layout
        // already knows how to size. rulesRowCount() counts two per rule to match.
        JContainer* line1 = rowBox();

        auto swatch = [&](const std::string& hex) {
            auto* b = static_cast<JColorButton*>(own(make_unique<JColorButton>(g_, kSwatchW, kRowH)));
            b->setInheritable(true);                       // inline clear -> "" -> inherit the scheme
            b->setColorHex(hex);
            b->onColorChanged.connect([this](const std::string&) { writeRules(); });
            line1->add(b);
            return b;
        };
        row.bg = swatch(rules_[i].bg); row.fg = swatch(rules_[i].fg);
        row.accent = swatch(rules_[i].accent); row.border = swatch(rules_[i].border);
        row.blink = static_cast<JCheckBox*>(own(make_unique<JCheckBox>(g_, "", 54.f)));
        row.blink->setChecked(rules_[i].blink);
        row.blink->onStateChanged.connect([this](bool) { writeRules(); });
        line1->add(row.blink);

        cap("");                                   // second row of the same rule
        JContainer* line2 = rowBox();
        row.when = static_cast<JLineEdit*>(own(make_unique<JLineEdit>(g_, "when… e.g. value > 6500", 150.f, kRowH)));
        row.when->setClearButtonEnabled(true);   // a band with no condition is one that always applies
        row.when->setText(rules_[i].when);
        row.when->onTextChanged.connect([this](const std::string&) { writeRules(); });
        row.when->setHSizePolicy(JSizePolicyMode::Expanding, 1);
        line2->add(row.when);

        // THE CAPTION THIS RULE PUTS ON A LABEL. Blank on most controls — a gauge has no words to change —
        // but it is what turns a Label into a lamp, so it sits beside the condition that decides it rather
        // than in a section of its own.
        row.text = static_cast<JLineEdit*>(own(make_unique<JLineEdit>(g_, "says…", 72.f, kRowH)));
        row.text->setText(rules_[i].text);
        row.text->onTextChanged.connect([this](const std::string&) { writeRules(); });
        line2->add(row.text);

        // "fx" -> the expression editor, seeded with the cell. Its result lands in rules_ and asks for a
        // rebuild, so the row can never show one thing while the rule holds another.
        auto* fx = static_cast<JButton*>(own(make_unique<JButton>(g_, "fx", 26.f, kRowH)));
        fx->onClicked.connect([this, idx] {
            if (!onPickSource || idx >= static_cast<int>(rules_.size())) return;
            writeRules();                                  // capture every other cell before we leave
            onPickSource(rules_[static_cast<size_t>(idx)].when, [this, idx](std::string ex) {
                if (idx < static_cast<int>(rules_.size())) rules_[static_cast<size_t>(idx)].when = std::move(ex);
                commitRules();
                rulesRebuild_ = true;
            });
        });
        line2->add(fx);

        auto* rm = static_cast<JButton*>(own(make_unique<JButton>(g_, "x", kRowH, kRowH)));
        rm->onClicked.connect([this, idx] {
            writeRules();
            if (idx >= 0 && idx < static_cast<int>(rules_.size())) rules_.erase(rules_.begin() + idx);
            commitRules();
            rulesRebuild_ = true;
        });
        line2->add(rm);
        ruleRows_.push_back(row);
    }

    cap("");
    {
        JContainer* f = rowBox();
        auto* add = static_cast<JButton*>(own(make_unique<JButton>(g_, "+ Add rule", 92.f, kRowH)));
        add->onClicked.connect([this] {
            writeRules();                                  // keep what the existing rows hold
            rules_.push_back(ColorRule{});
            commitRules();
            rulesRebuild_ = true;
        });
        f->add(add);
    }

    buildZonesSection(e);      // the dial-face spans, into the same container, under the rules
}

// ---------------------------------------------------------------------------------------------------
// SCALE ZONES — the gauge face's own colour, authored the same way as a rule.
// ---------------------------------------------------------------------------------------------------
void PropertiesDock::commitZones() {
    if (!model_ || !elemId_) return;
    const std::string compact = ColorRules::zonesToCompact(zones_);
    if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(elemId_) : nullptr) {
        inst->setOwnProp("zones", compact);
        if (PanelElement* me = model_->get(elemId_)) inst->commit(*me);
    } else {
        model_->setProp(elemId_, "zones", compact);
    }
    if (onInvalidate) onInvalidate();
    if (onApplyEdit) onApplyEdit("Scale zones", editBefore_, 721000 + elemId_);
}

void PropertiesDock::writeZones() {
    if (suppress_ || !model_ || !elemId_) return;
    for (size_t i = 0; i < zoneRows_.size() && i < zones_.size(); ++i) {
        const ZoneRow& z = zoneRows_[i];
        // A HALF-TYPED NUMBER IS NOT A ZERO. Every keystroke commits, so "65" on the way to "6500" must
        // not wipe what the field held when the text is not yet a number at all ("", "-", "6e"); the last
        // good value stays until a real one replaces it.
        auto num = [](const std::string& t, double keep) {
            try { size_t n = 0; const double v = std::stod(t, &n); return n == t.size() ? v : keep; }
            catch (...) { return keep; }
        };
        zones_[i].start = num(z.from->text(), zones_[i].start);
        zones_[i].end   = num(z.to->text(),   zones_[i].end);
        zones_[i].color = z.color->colorHex();
    }
    commitZones();
}

void PropertiesDock::buildZonesSection(const PanelElement* e) {
    using namespace jf;
    if (!e) return;
    // Offered only where it means something: a widget with no scale to lay a span along has nothing to
    // paint, so it gets no section rather than a section that does nothing.
    CanvasWidget* proto = instanceFor(e->type);
    if (!proto || !proto->paintsZones()) return;
    zonesOffered_ = true;
    zones_ = ColorRules::zonesFromCompact(e->prop("zones"));

    ruleCap("Scale zones");
    {
        JContainer* h = ruleRowBox();
        h->add(ruleOwn(make_unique<JLabel>(g_, "From", 64.f, kRowH)));
        h->add(ruleOwn(make_unique<JLabel>(g_, "To", 64.f, kRowH)));
        h->add(ruleOwn(make_unique<JLabel>(g_, "Colour", kSwatchW, kRowH)));
    }

    for (size_t i = 0; i < zones_.size(); ++i) {
        const int idx = static_cast<int>(i);
        ZoneRow row;
        ruleCap(("Zone " + std::to_string(i + 1)).c_str());
        JContainer* line = ruleRowBox();
        char nb[32];
        auto edit = [&](double v, const char* hint) {
            auto* le = static_cast<JLineEdit*>(ruleOwn(make_unique<JLineEdit>(g_, hint, 64.f, kRowH)));
            std::snprintf(nb, sizeof nb, "%g", v);
            le->setText(nb);
            le->onTextChanged.connect([this](const std::string&) { writeZones(); });
            line->add(le);
            return le;
        };
        row.from = edit(zones_[i].start, "from");
        row.to   = edit(zones_[i].end,   "to");
        row.color = static_cast<JColorButton*>(ruleOwn(make_unique<JColorButton>(g_, kSwatchW, kRowH)));
        row.color->setColorHex(zones_[i].color);
        row.color->onColorChanged.connect([this](const std::string&) { writeZones(); });
        line->add(row.color);

        auto* rm = static_cast<JButton*>(ruleOwn(make_unique<JButton>(g_, "x", kRowH, kRowH)));
        rm->onClicked.connect([this, idx] {
            writeZones();
            if (idx >= 0 && idx < static_cast<int>(zones_.size())) zones_.erase(zones_.begin() + idx);
            commitZones();
            rulesRebuild_ = true;
        });
        line->add(rm);
        zoneRows_.push_back(row);
    }

    // A NEW ZONE IS A REDLINE. Seeded across the top quarter of the gauge's own scale in red, because an
    // empty row (0..0, no colour) paints nothing and leaves you typing three fields before you can see
    // whether the thing works at all — and the top of the scale in red is what a zone is for nine times
    // in ten. The bounds come off THIS gauge's min/max, so it lands on the dial you are looking at.
    const double zlo = parseDouble(e->prop("minValue"), 0.0), zhi = parseDouble(e->prop("maxValue"), 100.0);
    ruleCap("");
    {
        JContainer* f = ruleRowBox();
        auto* add = static_cast<JButton*>(ruleOwn(make_unique<JButton>(g_, "+ Add zone", 92.f, kRowH)));
        add->onClicked.connect([this, zlo, zhi] {
            writeZones();                                  // keep what the existing rows hold
            zones_.push_back(ColorRules::Zone{ zlo + (zhi - zlo) * 0.75, zhi, "#ff0000" });
            commitZones();
            rulesRebuild_ = true;
        });
        f->add(add);
    }
}

void PropertiesDock::populateElement(const PanelElement* e) {
    view_ = View::Element; elemId_ = e->id; activeTF_ = typeForm(*e); header_ = e->type;
    if (onSnapshot) editBefore_ = onSnapshot();   // baseline for this element's undo step
    const PanelElement re = resolveElement(*e);
    for (const Field& f : activeTF_->fields) loadField(f, *e, re);
    buildRulesForm(e);          // per ELEMENT, so it cannot live in the per-TYPE form above
}

void PropertiesDock::populateCommon() {
    view_ = View::Common; elemId_ = 0; activeTF_ = nullptr; header_ = std::to_string(selIds_.size()) + " widgets";

    // Tear down the previous selection's rows — the form is rebuilt per-selection (which rows appear depends
    // on which elements are selected and which of their values currently agree).
    commonForm_.clear();
    commonFields_.clear();
    commonEditors_.clear();
    commonCaps_.clear();
    if (selIds_.empty() || !model_) return;   // guard: never front() an empty selection

    // Resolve each selected element once (resolved props + live instance). Instances are owned in a stable
    // map, so these pointers stay valid across the loop.
    const size_t n = selIds_.size();
    std::vector<const PanelElement*> els(n, nullptr);
    std::vector<PanelElement>        resolved(n);
    std::vector<CanvasWidget*>       insts(n, nullptr);
    for (size_t i = 0; i < n; ++i)
        if ((els[i] = model_->get(selIds_[i]))) { resolved[i] = resolveElement(*els[i]); insts[i] = instanceFor(els[i]->type); }
    if (!els[0] || !insts[0]) return;

    if (onSnapshot) editBefore_ = onSnapshot();   // baseline for this multi-widget edit session's undo step

    // The effective value the row would DISPLAY for the i-th selected element: geometry off its rect, every
    // other property off its resolved prop store — the SAME string loadField/writeCommon use.
    float ox = 0.f, oy = 0.f;
    const bool haveOrigin = selectionOrigin(ox, oy);
    auto eff = [&](size_t i, const std::string& key) -> std::string {
        if (!els[i]) return {};
        // X/Y report the SELECTION'S ORIGIN, identically for every member, which is what makes them appear
        // at all: a row is offered only when all members agree, and members of a group agree on their own
        // coordinates roughly never. Reading the origin also makes the row mean the thing the user is
        // trying to set -- where the group sits -- rather than where its first member happens to be.
        if (key == "x") return std::to_string(static_cast<int>(haveOrigin ? ox : els[i]->x));
        if (key == "y") return std::to_string(static_cast<int>(haveOrigin ? oy : els[i]->y));
        if (key == "w") return std::to_string(static_cast<int>(els[i]->w));
        if (key == "h") return std::to_string(static_cast<int>(els[i]->h));
        return resolved[i].prop(key);
    };

    // A property is COMMON when EVERY selected element exposes it (designable + writable there) AND all hold
    // the SAME effective value as the primary. Bindings (signal/unit) are never shared. Sort by meta.order.
    std::vector<const jf::JProperty*> rows;
    for (const jf::JProperty& p : insts[0]->properties().all()) {
        if (!p.meta.designable || !p.writable()) continue;
        if (p.meta.editor == "signal" || p.meta.editor == "unit") continue;
        const std::string prim = eff(0, p.name);
        bool shared = true;
        for (size_t i = 1; i < n && shared; ++i) {
            const jf::JProperty* op = insts[i] ? insts[i]->properties().find(p.name) : nullptr;
            if (!op || !op->meta.designable || !op->writable() || eff(i, p.name) != prim) shared = false;
        }
        if (shared) rows.push_back(&p);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const jf::JProperty* a, const jf::JProperty* b) { return a->meta.order < b->meta.order; });

    for (const jf::JProperty* pp : rows) {
        const jf::JProperty& p = *pp;
        const std::string capStr = p.meta.label.empty() ? p.name : p.meta.label;
        auto [ed, kind] = makeEditor(p, p.name);
        addRow(commonForm_, commonCaps_, commonEditors_, commonFields_, capStr.c_str(), std::move(ed), kind,
               p.name, p.meta.inheritable, /*multi=*/true);
    }

    // Seed every editor from the primary's (shared) effective value.
    for (const Field& f : commonFields_) loadField(f, *els[0], resolved[0]);
    // ... except X/Y, which loadField takes off the primary's own rect. For a selection they are the
    // group origin computed above, so put that back over the top.
    if (haveOrigin)
        for (const Field& f : commonFields_)
            if (f.kind == 'g' && (f.key == "x" || f.key == "y"))
                static_cast<JSpinBox*>(f.editor)->setValue(static_cast<int>(f.key == "x" ? ox : oy));
}

// Top-left of everything selected. For a group this is the only X/Y that means anything: the members sit
// at their own coordinates, and what the user wants to place is the SHAPE they form.
bool PropertiesDock::selectionOrigin(float& x, float& y) const {
    bool any = false;
    for (int id : selIds_) {
        const PanelElement* e = model_ ? model_->get(id) : nullptr;
        if (!e) continue;
        if (!any) { x = e->x; y = e->y; any = true; }
        else      { x = std::min(x, e->x); y = std::min(y, e->y); }
    }
    return any;
}

float PropertiesDock::commonGeometryValue(const std::string& key) const {
    for (const Field& f : commonFields_)
        if (f.kind == 'g' && f.key == key) return static_cast<float>(static_cast<JSpinBox*>(f.editor)->value());
    return 0.f;
}

void PropertiesDock::setCommonGeometry(const std::string& key, int value) {
    for (const Field& f : commonFields_)
        if (f.kind == 'g' && f.key == key) {
            suppress_ = true;                       // seed without re-entering the write from the signal
            static_cast<JSpinBox*>(f.editor)->setValue(value);
            suppress_ = false;
            writeCommon();
            return;
        }
}

// comboIdx 0 = All (common); i>=1 = focus selIds_[i-1]'s full property form (edits go to it alone).
void PropertiesDock::focusSelection(int comboIdx) {
    if (!model_ || selIds_.empty()) return;
    suppress_ = true;
    if (comboIdx <= 0) populateCommon();
    else if (comboIdx - 1 < static_cast<int>(selIds_.size())) {
        if (const PanelElement* e = model_->get(selIds_[comboIdx - 1])) populateElement(e);
    }
    suppress_ = false;
    applyVisibility();
    invalidate();
}

// Point the inspector at a model + selection. The host (a surface, the preferences prototype editor, …) is
// abstracted behind onSnapshot/onApplyEdit/onInvalidate — this dock is not tied to any of them.
void PropertiesDock::showFor(PanelModel* model, const std::vector<int>& sel, bool editing) {
    if (!editing || !model) { view_ = View::None; model_ = nullptr; elemId_ = 0; activeTF_ = nullptr; selIds_.clear(); header_.clear(); applyVisibility(); invalidate(); return; }

    model_ = model;

    // A dictionary entry is showing and the canvas has nothing selected: keep it. Clicking a dictionary
    // leaf moves focus off the canvas, which clears the surface selection and fires selectionChanged —
    // arriving here with an empty `sel` and, without this, immediately replacing the binding view with
    // "Surface canvas". An EXPLICIT widget selection still wins (sel is non-empty), which is the case
    // that matters. Cost: after viewing a binding, clicking bare canvas won't switch to canvas properties
    // until something is selected.
    if (view_ == View::Binding && sel.empty()) return;

    if (sel.size() > 1) {                                        // multi-select → focus combo + common/element
        selIds_ = sel;
        std::vector<std::string> items;
        items.push_back("All " + std::to_string(sel.size()) + " (common)");
        for (int id : sel) if (const PanelElement* e = model_->get(id)) items.push_back(e->type + " #" + std::to_string(id));
        comboGuard_ = true; focusCombo_->setItems(items); focusCombo_->setCurrentIndex(0); comboGuard_ = false;
        focusSelection(0);
        return;
    }
    selIds_.clear();
    suppress_ = true;
    if (sel.size() == 1 && model_->get(sel.front())) {
        populateElement(model_->get(sel.front()));
    } else {                                                     // nothing selected → the surface canvas
        view_ = View::Canvas; elemId_ = 0; activeTF_ = nullptr; header_ = "Surface canvas";
        populateCanvas();
    }
    suppress_ = false;
    applyVisibility();
    invalidate();
}

// Push every canvas field from the model into its editor (suppress_ is set by the caller).
void PropertiesDock::populateCanvas() {
    if (!model_) return;
    if (onCanvasSnapshot) canvasBefore_ = onCanvasSnapshot();   // baseline for the canvas undo step
    auto setInt = [](JWidget* w, int v) { static_cast<JSpinBox*>(w)->setValue(v); };
    for (const Field& f : canvasFields_) {
        if (f.key == "uid")          static_cast<JLineEdit*>(f.editor)->setText(model_->uid());
        else if (f.key == "w")       setInt(f.editor, static_cast<int>(model_->canvasW()));
        else if (f.key == "h")       setInt(f.editor, static_cast<int>(model_->canvasH()));
        else if (f.key == "static")  static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->canvasStatic());
        else if (f.key == "canvasAnchor") static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->canvasAnchor() + 1);   // row 0 = inherit
        else if (f.key == "title")   static_cast<JLineEdit*>(f.editor)->setText(model_->title());
        else if (f.key == "layout")  static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->layout());
        else if (f.key == "gridColumns") setInt(f.editor, model_->gridColumns());
        else if (f.key == "focusIndex")  setInt(f.editor, model_->focusIndex());
        else if (f.key == "borderWidth")  setInt(f.editor, model_->borderWidth());
        else if (f.key == "borderColor")  static_cast<JColorButton*>(f.editor)->setColorHex(model_->borderColor());
        else if (f.key == "borderStyle")  static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->borderStyle() - 1);
        else if (f.key == "borderRadius") setInt(f.editor, model_->borderRadius());
        else if (f.key == "titleFont")    static_cast<JFontButton*>(f.editor)->setFontSpec(model_->titleFont());
        else if (f.key == "titleColor")   static_cast<JColorButton*>(f.editor)->setColorHex(model_->titleColor());
        else if (f.key == "titlePadding") setInt(f.editor, model_->titlePadding());
        else if (f.key == "titleStyle")   static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->titleStyle());
        else if (f.key == "titlePlace")   static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->titlePlace());
        else if (f.key == "titleEdge")    static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->titleEdge());
        else if (f.key == "titleAlign")   static_cast<JComboBox*>(f.editor)->setCurrentIndex(model_->titleAlign());
    }
    guideW_->setValue(static_cast<int>(model_->guideW()));
    guideH_->setValue(static_cast<int>(model_->guideH()));
}

// The current value an editor holds, as the string stored in the prop bag. Geometry ('g') is handled by the
// caller (it writes the rect, not a prop); read-only kinds ('r'/'R'/'S') never reach here.
std::string PropertiesDock::fieldValue(const Field& f) {
    switch (f.kind) {
        case 'n': return std::to_string(static_cast<JSpinBox*>(f.editor)->value());
        case 'c': { auto* cb = static_cast<JComboBox*>(f.editor);
                    return (cb->currentIndex() >= 0 && cb->currentIndex() < static_cast<int>(cb->items().size()))
                               ? cb->items()[cb->currentIndex()] : std::string(); }
        case 'e': return std::to_string(std::max(0, static_cast<JComboBox*>(f.editor)->currentIndex()));
        case 'u': { const int i = static_cast<JComboBox*>(f.editor)->currentIndex();
                    const auto it = unitIds_.find(f.key);
                    return (it != unitIds_.end() && i >= 0 && i < static_cast<int>(it->second.size()))
                               ? it->second[i] : std::string("Auto"); }
        case 'k': return static_cast<JColorButton*>(f.editor)->colorHex();
        case 'f': return static_cast<JFontButton*>(f.editor)->fontSpec();
        case 'b': return static_cast<JCheckBox*>(f.editor)->isChecked() ? "1" : "0";
        case 'd': { char b[32]; std::snprintf(b, sizeof(b), "%g", static_cast<JDoubleSpinBox*>(f.editor)->value()); return b; }
        default:  return static_cast<JLineEdit*>(f.editor)->text();   // 't'
    }
}

void PropertiesDock::writeElement() {
    if (suppress_ || !model_ || !elemId_ || !activeTF_) return;
    const PanelElement* e = model_->get(elemId_);
    if (!e) return;
    float gx = e->x, gy = e->y, gw = e->w, gh = e->h; bool geo = false;
    for (const Field& f : activeTF_->fields) {
        if (f.kind == 'g') {
            const int v = static_cast<JSpinBox*>(f.editor)->value(); geo = true;
            if (f.key == "x") gx = static_cast<float>(v); else if (f.key == "y") gy = static_cast<float>(v);
            else if (f.key == "w") gw = static_cast<float>(v); else gh = static_cast<float>(v);
        } else if (f.kind == 'r' || f.kind == 'R' || f.kind == 'S' || f.kind == 'N') {
            // read-only label / id / source button — value is written by the picker callback, not read here
        } else {
            const std::string v = fieldValue(f);
            if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(elemId_) : nullptr) inst->setOwnProp(f.key, v);
            else model_->setProp(elemId_, f.key, v);   // prefs prototype editor: no live tree → edit the model
        }
    }
    if (geo) model_->setRect(elemId_, gx, gy, gw, gh);   // geometry stays model-owned (the canvas layout reads it)
    // Flush the widget's authored overrides into its model element NOW, so the undo snapshot below captures them
    // (edit()/commitEdit do NOT commit the tree — that would clobber model-authored menu edits). commit() writes
    // props only + never bumps the generation, so it can't retrigger a rehydrate that would undo the edit.
    if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(elemId_) : nullptr)
        if (PanelElement* me = model_->get(elemId_)) inst->commit(*me);
    if (onInvalidate) onInvalidate();
    // One coalesced undo step per element edit-session (keystrokes fold via the shared mergeId). For a host
    // with no undo stack (the prefs prototype editor) this is where the edited defaults get persisted.
    if (onApplyEdit) onApplyEdit("Edit " + header_, editBefore_, 700000 + elemId_);
}

// Multi-select edit: fan every common-form row to the WHOLE selection. Geometry sets each element's rect
// component; every other row writes the same prop value to all. Coalesced into one undo step (fixed mergeId,
// since the selection is stable for the session) against the baseline snapped in populateCommon().
void PropertiesDock::writeCommon() {
    if (suppress_ || !model_ || selIds_.empty()) return;
    for (const Field& f : commonFields_) {
        if (f.kind == 'g') {
            const int v = static_cast<JSpinBox*>(f.editor)->value();
            if (f.key == "x" || f.key == "y") {
                // TRANSLATE, DO NOT ASSIGN. Writing the same absolute x to every member collapsed the
                // selection into a single column -- the layout the user had just arranged, destroyed by
                // typing in the box that appeared to describe its position. X/Y on a selection are the
                // ORIGIN of the group, so setting them moves the whole shape by the difference and every
                // member keeps its offset within it.
                float ox = 0.f, oy = 0.f;
                if (!selectionOrigin(ox, oy)) continue;
                const float dx = (f.key == "x") ? static_cast<float>(v) - ox : 0.f;
                const float dy = (f.key == "y") ? static_cast<float>(v) - oy : 0.f;
                if (dx == 0.f && dy == 0.f) continue;
                for (int id : selIds_) if (PanelElement* e = model_->get(id))
                    model_->setRect(id, e->x + dx, e->y + dy, e->w, e->h);
            } else {
                // Width/height stay per-member assignment: making a selection one common size is a
                // deliberate, useful thing, and unlike position it does not encode the arrangement.
                for (int id : selIds_) if (PanelElement* e = model_->get(id)) {
                    float w = e->w, h = e->h;
                    if (f.key == "w") w = static_cast<float>(v); else h = static_cast<float>(v);
                    model_->setRect(id, e->x, e->y, w, h);
                }
            }
        } else if (f.kind == 'r' || f.kind == 'R' || f.kind == 'S' || f.kind == 'N') {
            // read-only / picker rows never appear in the common form
        } else {
            const std::string val = fieldValue(f);
            for (int id : selIds_) {
                if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(id) : nullptr) inst->setOwnProp(f.key, val);
                else model_->setProp(id, f.key, val);
            }
        }
    }
    // Flush each edited widget's overrides into its model element so the coalesced undo snapshot captures them.
    for (int id : selIds_)
        if (CanvasWidget* inst = onResolveInstance ? onResolveInstance(id) : nullptr)
            if (PanelElement* me = model_->get(id)) inst->commit(*me);
    if (onInvalidate) onInvalidate();
    if (onApplyEdit) onApplyEdit("Edit " + std::to_string(selIds_.size()) + " widgets", editBefore_, 720000);
}

// ✕ on an inheritable row: clear the local override (write an EMPTY value) so the property re-inherits the
// scheme/global default, then refresh the affected form. Its own coalesced undo step.
void PropertiesDock::clearOverride(const std::string& key, bool multi) {
    if (suppress_ || !model_) return;
    std::vector<PanelElement> before = onSnapshot ? onSnapshot() : std::vector<PanelElement>{};
    // Clear stays model-authored: clearProp bumps the model generation, so the widget rehydrates (setSource) with
    // the override gone and re-inherits — and the form re-populates from the already-cleared model just below.
    // (Authoring the clear onto the widget instead would leave the model stale until the post-populate commit.)
    if (multi) { for (int id : selIds_) model_->clearProp(id, key); }
    else if (elemId_) model_->clearProp(elemId_, key);

    // Refresh the now-inherited effective value(s), and re-baseline the edit session's undo snapshot to the
    // post-clear state. The common form is fully rebuilt (clearing an override can change which rows are
    // shared); the element form reloads via populateElement (which also re-snaps editBefore_).
    suppress_ = true;
    if (multi) { populateCommon(); applyVisibility(); }
    else if (activeTF_) { if (const PanelElement* e = model_->get(elemId_)) populateElement(e); }
    suppress_ = false;

    if (onInvalidate) onInvalidate();
    if (onApplyEdit) onApplyEdit(multi ? "Reset " + std::to_string(selIds_.size()) + " widgets" : "Reset " + header_,
                                 before, multi ? 730000 : 730000 + elemId_);
    invalidate();
}

// Open the sigil ExpressionEditor for a property (the "fx" button on an editor=="expr" row). Reuses the
// onPickSource hook (which opens the editor) and applies by setting the field's text, so the result flows
// through the normal write path — coalesced undo, single- and multi-select alike.
void PropertiesDock::openExpr(const std::string& key) {
    if (!onPickSource) return;
    JLineEdit* ed = fieldEditor(key);
    if (!ed) return;
    onPickSource(ed->text(), [this, key](std::string built) {
        if (JLineEdit* e = fieldEditor(key)) e->setText(built);   // re-find: the modal may have repopulated us
    });
}

// The live JLineEdit for `key` in whichever form is showing, or null.
JLineEdit* PropertiesDock::fieldEditor(const std::string& key) {
    const std::vector<Field>& fs = (view_ == View::Common) ? commonFields_
                                 : (activeTF_ ? activeTF_->fields : commonFields_);
    for (const Field& f : fs)
        if (f.key == key && (f.kind == 'X' || f.kind == 't')) return static_cast<JLineEdit*>(f.editor);
    return nullptr;
}

void PropertiesDock::pickSource(const std::string& key) {
    if (!onPickSource || !model_ || !elemId_) return;
    const PanelElement* e = model_->get(elemId_);
    const std::string cur = e ? e->prop(key) : std::string();
    const int id = elemId_;
    onPickSource(cur, [this, key, id](std::string picked) {
        if (!model_ || !model_->get(id) || picked.empty()) return;
        if (onSnapshot) editBefore_ = onSnapshot();   // snapshot before, so the bind is one undo step
        model_->setProp(id, key, picked);
        if (id == elemId_) populateElement(model_->get(id));  // refresh the button label if still shown
        if (onInvalidate) onInvalidate();
        if (onApplyEdit) onApplyEdit("Bind source", editBefore_, 710000 + id);
    });
}

void PropertiesDock::pickNode(const std::string& key) {
    if (!onPickNode || !model_ || !elemId_) return;
    const PanelElement* e = model_->get(elemId_);
    const std::string cur = e ? e->prop(key) : std::string();
    const int id = elemId_;
    onPickNode(cur, [this, key, id](std::string picked) {
        if (!model_ || !model_->get(id) || picked.empty()) return;
        if (onSnapshot) editBefore_ = onSnapshot();   // snapshot before, so the pick is one undo step
        model_->setProp(id, key, picked);
        if (id == elemId_) populateElement(model_->get(id));  // refresh the button label if still shown
        if (onInvalidate) onInvalidate();
        if (onApplyEdit) onApplyEdit("Set link", editBefore_, 711000 + id);
    });
}

void PropertiesDock::writeCanvas() {
    if (suppress_ || !model_) return;
    auto ival = [](JWidget* w) { return static_cast<JSpinBox*>(w)->value(); };
    auto cidx = [](JWidget* w) { return static_cast<JComboBox*>(w)->currentIndex(); };
    float cw = model_->canvasW(), ch = model_->canvasH();
    for (const Field& f : canvasFields_) {
        if (f.key == "w")            cw = static_cast<float>(ival(f.editor));
        else if (f.key == "h")       ch = static_cast<float>(ival(f.editor));
        else if (f.key == "static")  model_->setCanvasStatic(cidx(f.editor));
        else if (f.key == "canvasAnchor") model_->setCanvasAnchor(cidx(f.editor) - 1);   // row 0 = inherit (-1)
        else if (f.key == "title")   model_->setTitle(static_cast<JLineEdit*>(f.editor)->text());
        else if (f.key == "layout")  model_->setLayout(cidx(f.editor));
        else if (f.key == "gridColumns") model_->setGridColumns(ival(f.editor));
        else if (f.key == "focusIndex")  model_->setFocusIndex(ival(f.editor));
        else if (f.key == "borderWidth")  model_->setBorderWidth(ival(f.editor));
        else if (f.key == "borderColor")  model_->setBorderColor(static_cast<JColorButton*>(f.editor)->colorHex());
        else if (f.key == "borderStyle")  model_->setBorderStyle(cidx(f.editor) + 1);   // stored 1-based
        else if (f.key == "borderRadius") model_->setBorderRadius(ival(f.editor));
        else if (f.key == "titleFont")    model_->setTitleFont(static_cast<JFontButton*>(f.editor)->fontSpec());
        else if (f.key == "titleColor")   model_->setTitleColor(static_cast<JColorButton*>(f.editor)->colorHex());
        else if (f.key == "titlePadding") model_->setTitlePadding(ival(f.editor));
        else if (f.key == "titleStyle")   model_->setTitleStyle(cidx(f.editor));
        else if (f.key == "titlePlace")   model_->setTitlePlace(cidx(f.editor));
        else if (f.key == "titleEdge")    model_->setTitleEdge(cidx(f.editor));
        else if (f.key == "titleAlign")   model_->setTitleAlign(cidx(f.editor));
    }
    model_->setCanvasSize(cw, ch);
    model_->setGuideSize(static_cast<float>(guideW_->value()), static_cast<float>(guideH_->value()));
    if (onInvalidate) onInvalidate();
    if (onApplyCanvasEdit) onApplyCanvasEdit("Canvas properties", canvasBefore_, 800001);   // coalesced canvas undo step
}

void PropertiesDock::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    // A rule was added, removed, or had its expression rewritten. Rebuilding the rows HERE and not in the
    // button's own callback is the whole point: the callback runs on a widget that the rebuild destroys,
    // and freeing the control mid-signal is a use-after-free waiting for a slow frame to expose it. The
    // model was already written by the callback; this only catches the view up.
    if (rulesRebuild_) {
        rulesRebuild_ = false;
        if (model_ && elemId_) {
            const bool was = suppress_;
            suppress_ = true;                    // seeding the fresh rows must not write them straight back
            buildRulesForm(model_->get(elemId_));
            suppress_ = was;
        }
    }
    const auto bb = getBoundingBox();
    buf.pushRectangle(bb.x, bb.y, bb.width, bb.height, Colors::Surface1);
    // Text resolves the theme's TEXT role (honours a custom global palette), not a raw shade.
    const JColor txtRole = jstyle::role(JColorRole::Text, jstyle::option(JWidgetState::Normal, false));
    const uint8_t* tc = txtRole.data();
    if (view_ == View::None) {
        if (JTextHelper::hasAtlas()) {
            uint8_t dim[4] = { tc[0], tc[1], tc[2], 150 };   // dimmed "empty" hint
            JTextHelper::pushText(buf, bb.x + 10.f, bb.y + 12.f, "No surface", dim, bb.width - 20.f);
        }
        return;
    }
    if (JTextHelper::hasAtlas() && !header_.empty())
        JTextHelper::pushText(buf, bb.x + 10.f, bb.y + 8.f, header_, tc, bb.width - 20.f);
    // Live "Current Value": the read-only value row tracks the bound channel each frame (the value is live
    // telemetry, not a stored prop), so it updates continuously while the element is selected.
    if (view_ == View::Element && activeTF_ && model_) {
        if (const PanelElement* e = model_->get(elemId_)) {
            for (const Field& f : activeTF_->fields)
                if (f.kind == 'r' && f.key == "value")
                    static_cast<JLabel*>(f.editor)->setText(CanvasWidget::fmtVal(resolveElement(*e), Cache::instance()));
        }
    }
    float formY = bb.y + 30.f;
    if (comboActive()) {                     // multi-select: the "all vs one" chooser sits above the form
        focusCombo_->setBounds({ bb.x + 10.f, bb.y + 30.f, bb.width - 20.f, 26.f });
        focusCombo_->populateRenderPrimitives(buf);
        formY = bb.y + 64.f;
    }
    if (JContainer* f = activeForm()) {
        // Host the form in the scroll area: give the form its NATURAL content height (rows × stride + padding)
        // so the area knows the scroll range, then let the area place/offset/clip it and draw a scrollbar when
        // it overflows. The area sets the form's x/y/width; only the height matters here.
        // Attach the form to the scroll area only when it CHANGES: clearChildren() resets the scroll
        // position, so calling it every frame would pin the panel at the top and make scrolling impossible.
        const bool wantRules = (view_ == View::Element && elemId_ != 0);
        const size_t wantN = wantRules ? 2u : 1u;
        if (scroll_->children().size() != wantN || scroll_->children().front() != f) {
            scroll_->clearChildren();
            scroll_->addChildWidget(f);
            if (wantRules) scroll_->addChildWidget(&rulesForm_);   // stacked under the properties
        }
        const int   rows     = activeRowCount();
        const float contentH = 20.f + rows * (kRowH + 6.f);   // Form padding (~10 top+bottom) + rows*(row+gap)
        f->setBounds({ bb.x + 8.f, formY, bb.width - 16.f, contentH });
        if (wantRules) {
            const float rulesH = 20.f + rulesRowCount() * (kRowH + 6.f);
            rulesForm_.setBounds({ bb.x + 8.f, formY + contentH + 6.f, bb.width - 16.f, rulesH });
        }
        scroll_->setBounds({ bb.x + 8.f, formY, bb.width - 16.f, (bb.y + bb.height) - formY - 8.f });
        scroll_->populateRenderPrimitives(buf);
    } else if (!scroll_->children().empty()) {
        scroll_->clearChildren();
    }
}


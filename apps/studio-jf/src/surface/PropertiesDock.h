#pragma once

// PropertiesDock — the edit-mode inspector. Never empty: nothing selected → the SURFACE CANVAS props
// (size / scaling / anchor); an element selected → that element's props. The element form is
// MODEL-DRIVEN: base rows (binding, caption, geometry) plus the type's UNIQUE props, all read from the
// element's widget-class JPropertyModel. One form is built per widget type and cached (no dynamic teardown). Both are
// two-column Form grids. Edits write straight back to the model. Hidden in run mode.

#include <j/core/JWidget.h>
#include <j/core/JContainer.h>
#include <j/core/JLabel.h>
#include <j/core/JLineEdit.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JColorButton.h>
#include <j/core/JFontButton.h>
#include <j/core/JScrollArea.h>

#include "PanelModel.h"
#include "../model/RangeBands.h"   // ColorRule / ColorRules
#include "WidgetRegistry.h"   // CanvasWidget + makeWidgetInstance — this dock is NOT tied to Surface

#include <functional>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class PropertiesDock : public jf::JWidget {
public:
    explicit PropertiesDock(jf::JSceneGraph& g);

    // Point the inspector at a model + selection. `editing` false (or no model) clears it. The host is NOT a
    // Surface — a surface, the preferences prototype editor, anything with a PanelModel can drive this dock.
    void showFor(PanelModel* model, const std::vector<int>& selection, bool editing);

    // The element a template's "[*]"/"$*" means while this selection is being edited (the host viewport's
    // element, from Surface::elementScope) — so the Display Unit picker can resolve a templated data source
    // to a concrete channel and offer that channel's quantity's units. Empty outside a template scope.
    void setElementScope(const std::string& key) { elementScope_ = key; }

    // Point the inspector at a DICTIONARY binding instead of a canvas selection: a read-only view of what the
    // meta says about `path` (kind, datatype, scale, units, schema min/max, offset/size, table geometry + axes).
    // Empty path clears back to View::None. Selecting a canvas widget replaces it via showFor() as usual.
    void showForBinding(const std::string& path);

    // TEST SEAM: read/write the common (multi-select) form's geometry rows without a pointer. Same path
    // the spin boxes take, so a test exercises the real read/translate logic rather than a copy of it.
    float commonGeometryValue(const std::string& key) const;
    void  setCommonGeometry(const std::string& key, int value);

    // Wired by the app to open the searchable source picker: (current path, apply-callback). The Properties
    // dock calls it when a "Data Source" browse button is clicked; the app opens PopupSignalPicker.
    std::function<void(const std::string&, std::function<void(std::string)>)> onPickSource;
    // Wired by the app to open the navigation-node picker: (current path, apply-callback). Used by a "node"
    // editor (e.g. a hyperlink's Link target); the app opens PopupSignalPicker seeded with the nav tree.
    std::function<void(const std::string&, std::function<void(std::string)>)> onPickNode;

    // Host hooks — repaint + undo are the HOST's concern, injected so the dock stays surface-agnostic. Any
    // unset hook is skipped (e.g. the preferences editor sets onApplyEdit to persist and leaves canvas unset).
    std::function<void()>                                                          onInvalidate;
    std::function<std::vector<PanelElement>()>                                     onSnapshot;
    std::function<void(const std::string&, const std::vector<PanelElement>&, int)> onApplyEdit;
    std::function<PanelModel::CanvasState()>                                        onCanvasSnapshot;
    std::function<void(const std::string&, const PanelModel::CanvasState&, int)>    onApplyCanvasEdit;
    // Resolve the LIVE widget instance for an element id (the running state). When set, prop edits are authored
    // straight onto the widget (setOwnProp) instead of the model — the widget IS the source. Unset (the prefs
    // prototype editor has no live tree) → the dock falls back to editing the model element directly.
    std::function<CanvasWidget*(int)>                                              onResolveInstance;

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override;
    // Input routes through the scroll area (which hosts the active form) so the scroll offset + scrollbar apply.
    void handleMouseMove(float mx, float my) override    { if (comboActive()) focusCombo_->handleMouseMove(mx, my);  scroll_->handleMouseMove(mx, my); }
    void handleMousePress(float mx, float my) override   { if (comboActive()) focusCombo_->handleMousePress(mx, my); scroll_->handleMousePress(mx, my); }
    void handleMouseRelease(float mx, float my) override { if (comboActive()) focusCombo_->handleMouseRelease(mx, my); scroll_->handleMouseRelease(mx, my); }
    bool handleScroll(float mx, float my, float w) override { return scroll_->handleScroll(mx, my, w); }


private:
    enum class View { None, Canvas, Element, Common, Binding };
    struct Field { std::string key; char kind; jf::JWidget* editor; };   // kind: t(ext) n(um) c(hoice) g(eom)
    struct TypeForm {
        std::unique_ptr<jf::JContainer>          form;      // built first → destroyed after its editors
        std::vector<std::unique_ptr<jf::JLabel>> caps;
        std::vector<std::unique_ptr<jf::JWidget>> editors;
        std::vector<Field>                       fields;
    };

    jf::JContainer* activeForm();
    void            applyVisibility();   // only the ACTIVE form's widgets stay visible → tab-focusable
    TypeForm*       typeForm(const PanelElement& e);   // get-or-build the cached form for e's type, from its widget's JPropertyModel

    // Editor construction shared by the per-type form and the multi-select common form: a JProperty →
    // {editor widget, kind char}. The two forms differ only in how a row's change/✕ is wired (single vs fan).
    std::pair<std::unique_ptr<jf::JWidget>, char> makeEditor(const jf::JProperty& p, const std::string& key);
    // Append one caption+editor row (optionally an inheritable ✕ that resets the override) to a form, wiring
    // the editor's change signal to writeElement (multi=false) or writeCommon (multi=true).
    void            addRow(jf::JContainer& form, std::vector<std::unique_ptr<jf::JLabel>>& caps,
                           std::vector<std::unique_ptr<jf::JWidget>>& owned, std::vector<Field>& fields,
                           const char* caption, std::unique_ptr<jf::JWidget> field, char kind,
                           const std::string& key, bool inheritable, bool multi);
    void            wireChangeTo(jf::JWidget* editor, char kind, bool multi);   // change → writeElement / writeCommon
    void            loadField(const Field& f, const PanelElement& e, const PanelElement& re);  // push effective value into an editor
    std::string     fieldValue(const Field& f);           // read an editor's current value as a string (non-geom)
    void            pickSource(const std::string& key);   // open the source picker for a 'signal' field, apply on Select
    void            pickNode(const std::string& key);     // open the node picker for a 'node' field, apply on Select
    void            openExpr(const std::string& key);    // open the sigil ExpressionEditor for an 'expr' field
    jf::JLineEdit*  fieldEditor(const std::string& key); // the live line edit for a key in the active form, or null
    void            clearOverride(const std::string& key, bool multi);   // ✕ — reset the override so the prop re-inherits
    void            writeElement();
    void            writeCommon();                            // fan a common-form edit to the whole selection (one undo step)
    void            writeCanvas();
    void            populateElement(const PanelElement* e);   // element view (base + type-unique)
    void            populateCommon();                          // common view: rebuilt per-selection from shared properties
    // Top-left of the selection's bounding box, in virtual coords. A multi-selection's X/Y mean the
    // GROUP's origin, not any one member's — see writeCommon().
    bool            selectionOrigin(float& x, float& y) const;
    void            focusSelection(int comboIdx);              // 0 = all (common); i>=1 = focus selIds_[i-1]
    bool            comboActive() const { return selIds_.size() > 1 && focusCombo_; }

    jf::JSceneGraph&      g_;

    // Canvas view — the full "Surface canvas" property set (mirrors the studio's PanelDefinition:
    // size, scaling, editing guide, title text, layout, border + title styling). Built as a generic
    // field list; writeCanvas()/populateCanvas() dispatch per key to the model's typed setters.
    void            buildCanvasForm();
    void            addCanvasRow(const char* caption, std::unique_ptr<jf::JWidget> ed, char kind, const std::string& key);
    void            populateCanvas();

    // ------------------------------------------------------------------
    // VALUE RULES — the per-element list of "colour/flash WHEN this expression is true" rows, with an
    // Add button. It lives HERE rather than behind a modal because it is authored the way every other
    // property is: look at the control, change a thing, see it. And it cannot live in the type form
    // above, because that form is cached PER TYPE and shared by every element of that type, while these
    // rows are per ELEMENT and vary in number -- so it is its own container, rebuilt per selection and
    // stacked under the form inside the same scroll area.
    void            buildRulesForm(const PanelElement* e);   // rebuild from e's "ranges" prop ("" clears)
    void            writeRules();                            // rows -> rules_ -> commit
    void            commitRules();                           // rules_ -> "ranges", one undo step, ONE path
    static constexpr float kSwatchW = 62.f;                  // wide enough for a swatch's "(scheme)" caption
    int             rulesRowCount() const;                   // header + rows + Add (both sections), for the scroll extent
    // The three shapes every row in this form is built from — a caption/control pair, with the control a
    // horizontal box. Members rather than locals because the zones section below builds the same shapes
    // into the same container.
    jf::JWidget*    ruleOwn(std::unique_ptr<jf::JWidget> w);
    void            ruleCap(const char* text);
    jf::JContainer* ruleRowBox();

    // SCALE ZONES — the OTHER band a gauge needs: a fixed span of the scale (6500-8000 in red), painted
    // into the face whatever the reading is. Edited here, under the value rules, because it is the same
    // kind of authoring and shares their container; offered only on widgets that paint one (paintsZones).
    void            buildZonesSection(const PanelElement* e);
    void            writeZones();
    void            commitZones();
    int             zonesRowCount() const;                   // 0 when the widget paints none
    struct ZoneRow {
        jf::JLineEdit*    from = nullptr; jf::JLineEdit* to = nullptr;
        jf::JColorButton* color = nullptr;
    };
    std::vector<ZoneRow>            zoneRows_;
    std::vector<ColorRules::Zone>   zones_;
    bool                            zonesOffered_ = false;   // this element's widget paints zones

    jf::JContainer                            rulesForm_;
    std::vector<std::unique_ptr<jf::JWidget>> rulesOwned_;    // every control the rows own
    struct RuleRow {
        jf::JColorButton* bg = nullptr;     jf::JColorButton* fg = nullptr;
        jf::JColorButton* accent = nullptr; jf::JColorButton* border = nullptr;
        jf::JCheckBox*    blink = nullptr;
        jf::JLineEdit*    when = nullptr;
        jf::JLineEdit*    text = nullptr;   // caption while the rule holds (labels; blank on everything else)
    };
    std::vector<RuleRow>    ruleRows_;
    std::vector<ColorRule>  rules_;
    bool                    rulesRebuild_ = false;   // a row asked to be rebuilt (add/remove) — do it next frame

    jf::JContainer canvasForm_;
    std::vector<std::unique_ptr<jf::JLabel>>  canvasCaps_;
    std::vector<std::unique_ptr<jf::JWidget>> canvasEditors_;
    std::vector<Field>                        canvasFields_;
    std::unique_ptr<jf::JContainer> guideRow_;                 // compound "Editing guide" (W + H + clear)
    std::unique_ptr<jf::JSpinBox>   guideW_, guideH_;
    std::unique_ptr<jf::JButton>    guideClear_;

    // Common view (multi-select). Every property EVERY selected element exposes AND currently holds the SAME
    // value for gets a row; editing it fans to the whole selection as one undo step. Built dynamically per
    // selection (its rows depend on which elements are selected and their shared values) with the SAME
    // row-building machinery as the per-type form — the container is a stable member; its rows/editors are
    // cleared and rebuilt in populateCommon().
    jf::JContainer commonForm_;
    std::vector<std::unique_ptr<jf::JLabel>>  commonCaps_;
    std::vector<std::unique_ptr<jf::JWidget>> commonEditors_;
    std::vector<Field>                        commonFields_;
    std::vector<int> selIds_;
    std::unique_ptr<jf::JComboBox> focusCombo_;   // "All (common)" vs a single selected element
    bool comboGuard_ = false;                     // suppress focusCombo_ echo while repopulating it

    // Binding view (a dictionary entry). Every row is read-only text built straight from the meta, so unlike
    // the element views there is nothing to write back and no undo baseline. Rebuilt per selected entry.
    void addBindingRow(const char* caption, const std::string& value);
    jf::JWidget* addBindingValueEditor(const std::string& path);   // editable value row (host-side entries)
    jf::JContainer bindingForm_;
    std::vector<std::unique_ptr<jf::JLabel>>  bindingCaps_;
    std::vector<std::unique_ptr<jf::JWidget>> bindingEditors_;

    // Element views (one per type, cached).
    std::unordered_map<std::string, std::unique_ptr<TypeForm>> typeForms_;
    TypeForm* activeTF_ = nullptr;

    std::unique_ptr<jf::JScrollArea> scroll_;   // hosts the active form so overflow scrolls (scrollbar + wheel)
    int  activeRowCount() const;                // row count of the active form → its natural content height

    // Display-unit combos show unit LABELS but store unit IDs (or "Auto"/"Raw"); this parallel id list, keyed
    // by property key, maps the selected index back to the id on writeback. Rebuilt per element in populate.
    std::unordered_map<std::string, std::vector<std::string>> unitIds_;

    // A prototype instance per widget type — the sole source of a type's JPropertyModel for form-building
    // (replaces reaching into a live surface for the instance, which is what tied this dock to Surface).
    CanvasWidget* instanceFor(const std::string& type);
    std::unordered_map<std::string, std::unique_ptr<CanvasWidget>> protos_;

    PanelModel* model_  = nullptr;
    std::string elementScope_;   // template element for "[*]"/"$*" (see setElementScope)
    int         elemId_ = 0;
    View        view_   = View::None;
    std::string header_;
    bool        suppress_ = false;
    std::vector<PanelElement> editBefore_;   // model snapshot at edit-session start (element undo)
    PanelModel::CanvasState   canvasBefore_;  // canvas-field snapshot at edit-session start (canvas undo)
};

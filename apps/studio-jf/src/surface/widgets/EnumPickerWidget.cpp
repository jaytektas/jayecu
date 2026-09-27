// EnumPickerWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include <algorithm>
#include <functional>
#include <set>
#include "EnumPickerWidget.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"

namespace {
// THE rule for a board-pin picker, in ONE place. Three call sites resolve one — the label to render,
// the list to pop, and which pins a sibling already claims — and each carried its own copy of the
// gating. Updating one and not the others is exactly how a picker came to open and choose correctly
// while the control it belonged to went on reading "(none)".
//
// A GATED picker (a sensor's pin list) selects the set whose ifaces contain the value of the sibling
// field `by` names. An UNGATED one (a trigger stream's capture input) has a single set that always
// applies — there is no sibling to read, and pretending there is means evaluating a path that does
// not exist and hoping it reads back as a match.
//
// `sets` is the caller's storage; the returned pointer aims into it and is valid while it lives.
// `read` resolves a sibling path to its value — the render path evaluates sigils, the sibling scan
// reads the config image, and they are not interchangeable.
const MetaModel::PickerSet* applicablePickerSet(
        const MetaModel* meta, const std::string& bind,
        const std::function<double(const std::string&)>& read,
        std::vector<MetaModel::PickerSet>& sets) {
    if (!meta) return nullptr;
    std::string by;
    if (!meta->fieldPicker(bind, by, sets) || sets.empty()) return nullptr;
    if (by.empty()) return &sets.front();                     // ungated: one list, always
    const auto dot = bind.rfind('.');
    const std::string sib = (dot == std::string::npos) ? bind : bind.substr(0, dot + 1) + by;
    const int gate = static_cast<int>(std::lround(read(sib)));
    for (const auto& ps : sets)
        if (std::find(ps.ifaces.begin(), ps.ifaces.end(), gate) != ps.ifaces.end()) return &ps;
    return nullptr;                                           // no set for this interface (on-board / CAN)
}

// The label a picker set gives to a stored value. Values are FIRMWARE POOL INDICES, not positions,
// so this is a search and never a subscript.
// Does the stored value name real hardware, or a sentinel the picker offers ("None")? Unknown values are
// treated as pins, which is what every option was before a picker could carry an extra.
bool pickerValueIsPin(const MetaModel::PickerSet& ps, int value) {
    for (size_t i = 0; i < ps.values.size(); ++i)
        if (ps.values[i] == value) return i >= ps.pins.size() || ps.pins[i] != 0;
    return false;                                             // not in this set at all: holds nothing here
}

std::string pickerLabelFor(const MetaModel::PickerSet& ps, int value) {
    for (size_t i = 0; i < ps.values.size() && i < ps.options.size(); ++i)
        if (ps.values[i] == value) return ps.options[i];
    return {};
}
}  // namespace

std::string EnumPickerWidget::resolveLabel(const Cache& c, std::vector<std::string>* optsOut) const {
    const std::string bind = bindPath();
    const int cur = static_cast<int>(std::lround(bind.empty() ? -1.0 : evalSource(bind).v));
    const MetaModel* meta = c.meta();
    const bool sigField = meta && !bind.empty() && meta->isSignalField(bind);
    const bool tblField = meta && !bind.empty() && meta->isTableField(bind);
    std::string label;
    std::vector<std::string> opts;
    if (bind.empty() || cur < 0) label = "";                   // unset: signal selector -1, or no binding
    else if (sigField) {                                       // signal selector: resolve the id (incl. id 255)
        for (const auto& [nm, id] : meta->signalMap())
            if (id == cur) { label = c.label(nm); if (label.empty()) label = nm; break; }
        if (label.empty()) label = "(none)";
    }
    else if (cur == 255)         label = "(none)";             // legacy uint8 255-unassigned fallback (pin source is now int8 -1 -> caught above)
    else {
        std::vector<MetaModel::PickerSet> pss;
        auto readSib = [this](const std::string& p) { return evalSource(p).v; };
        if (const MetaModel::PickerSet* ps = applicablePickerSet(meta, bind, readSib, pss)) {
            opts  = ps->options;
            label = pickerLabelFor(*ps, cur);
            if (label.empty()) label = "(none)";       // the value is not a pin in this set
        } else if (meta && !pss.empty()) {
            label = "(none)";                          // has a picker, but no set applies (on-board / CAN)
        } else if (tblField) {
            // A TABLE SELECTOR: the value IS the table id, and the id is the option's index. The
            // generic tables carry the name the tuner gave them, which is what makes one of eight
            // identically-shaped maps identifiable at all.
            opts  = c.tableOptionLabels(bind);
            label = (cur < static_cast<int>(opts.size())) ? opts[cur] : std::to_string(cur);
        } else {
            opts  = meta->enumOptions(bind);
            label = (cur < static_cast<int>(opts.size())) ? opts[cur] : std::to_string(cur);   // plain enum: value == index
        }
    }
    if (optsOut) *optsOut = std::move(opts);
    return label;
}

// THE LIST THIS FIELD OFFERS, in one place. The combo's items, the greying, and what a pick WRITES all
// come from here, so they cannot disagree — which they did while the menu built its own labels and the
// render built its own current-value text.
EnumPickerWidget::Options EnumPickerWidget::optionsNow() const {
    Options o;
    const std::string bind = bindPath();
    if (bind.empty()) return o;
    const Cache& C = Cache::instance();
    const MetaModel* meta = C.meta();
    if (!meta) return o;
    o.current = static_cast<int>(std::lround(evalSource(bind).v));

    // A SIGNAL OR TABLE SELECTOR IS SEARCHABLE, not a drop-down: the catalogue runs to hundreds of
    // channels and the table registry to 167 maps, and a list that long is one nobody finds anything in.
    // The picker field shows the resolved name; pressing it asks this widget to open the dialog.
    if (meta->isSignalField(bind) || meta->isTableField(bind)) {
        o.searchable = true;
        o.labels.push_back(resolveLabel(C));
        o.current = 0;
        return o;
    }

    // A board-pin set, gated by the sibling that says which interface this input uses.
    std::vector<MetaModel::PickerSet> pss;
    auto readSib = [this](const std::string& p) { return evalSource(p).v; };
    if (const MetaModel::PickerSet* set = applicablePickerSet(meta, bind, readSib, pss)) {
        const MetaModel::PickerSet& ps = *set;
        const auto claimed = claimedPoolPins(bind);
        o.labels = ps.options;
        o.values = ps.values;
        o.enabled.assign(o.labels.size(), 1);
        for (size_t k = 0; k < o.labels.size(); ++k) {
            // A FIXED pin is greyed whatever else is true — the board consumes it, so it is shown so the
            // reader recognises the name and never offered.
            if (k < ps.fixed.size() && ps.fixed[k]) {
                o.labels[k] += "  \xE2\x80\x94 fixed";
                o.enabled[k] = 0;
                continue;
            }
            if (o.values[k] == o.current) continue;      // our own pin is never greyed
            const auto c = claimed.find(ps.options[k]);
            if (c == claimed.end()) continue;
            // A pin another enabled sensor holds is SHOWN, greyed, saying who has it — unless the field
            // SHARES (a coil: a distributor runs every cylinder off one), where the holder's name is all
            // you get and the option stays live.
            o.labels[k] += "  \xE2\x80\x94 " + c->second.holder;
            o.enabled[k] = c->second.shareable ? 1 : 0;
        }
        // The combo's index is a position in the list; the value is the option's stored pool index.
        int idx = -1;
        for (size_t k = 0; k < o.values.size(); ++k) if (o.values[k] == o.current) { idx = int(k); break; }
        o.current = idx;
        return o;
    }
    // Has a picker but no set applies (an on-board or CAN device has no source pin at all): offering the
    // plain enum here would show an unrelated list for the same field.
    if (!pss.empty()) return o;

    o.labels = meta->enumOptions(bind);
    if (o.labels.empty()) return o;
    // A SENSOR TYPE a generic input cannot be given is shown greyed rather than offered: effective_type()
    // accepts a stored type only where the catalog marks it selectable, so choosing another leaves the
    // input publishing nothing. Greyed rather than dropped — an option you cannot pick teaches more than
    // one that is not there.
    if (const auto& types = meta->sensorTypes(); !types.empty() && bind.size() > 5
        && bind.compare(bind.size() - 5, 5, ".type") == 0 && o.labels.size() == types.size()) {
        o.enabled.assign(o.labels.size(), 1);
        for (size_t k = 0; k < types.size(); ++k) o.enabled[k] = types[k].selectable ? 1 : 0;
    }
    // AN INTERFACE THIS SENSOR CANNOT BE READ THROUGH, for the same reason: the catalogue declares what
    // each input may bind. The CURRENT value stays pickable whatever it says, so a tune already holding
    // one outside the list can still show it and be changed away from it.
    if (const auto allow = meta->allowedOptionIds(bind); !allow.empty()) {
        const auto ids = meta->enumOptionIds(bind);
        if (ids.size() == o.labels.size()) {
            o.enabled.assign(o.labels.size(), 1);
            for (size_t k = 0; k < ids.size(); ++k)
                o.enabled[k] = (int(k) == o.current
                                || std::find(allow.begin(), allow.end(), ids[k]) != allow.end()) ? 1 : 0;
        }
    }
    // AN OPTION THIS ELEMENT'S HARDWARE CANNOT DO — an output row is its pin, and only an IGN pin can be
    // a coil. Greyed for the same reason as above, and the current value stays pickable.
    if (const auto allow = meta->allowedOptionIndices(bind); !allow.empty()) {
        o.enabled.resize(o.labels.size(), 1);
        for (size_t k = 0; k < o.labels.size(); ++k)
            if (int(k) != o.current && std::find(allow.begin(), allow.end(), int(k)) == allow.end())
                o.enabled[k] = 0;
    }
    return o;   // plain enum: the value IS the index, so `values` stays empty
}

bool EnumPickerWidget::wantsPicker() const {
    const std::string bind = bindPath();
    const MetaModel* meta = Cache::instance().meta();
    return meta && !bind.empty() && (meta->isSignalField(bind) || meta->isTableField(bind));
}

void EnumPickerWidget::clearBinding() {
    const std::string path = writableConfigPath(bindPath());
    if (!path.empty()) Cache::instance().setConfigValue(path, -1.0);
}

jf::JControl* EnumPickerWidget::control() {
    if (wantsPicker()) {
        // A rebind can change the kind under a live element, so the other control goes rather than
        // lingering as a parented child the hover scan still finds at its last bounds.
        if (m_combo) { m_combo.reset(); m_shown.clear(); }
        if (!m_picker) {
            m_picker = std::make_unique<jf::JPickerField>(sceneGraph());
            m_picker->setPlaceholder("Not assigned");
            m_picker->setClearable(true);
            m_picker->onOpenRequested.connect([this] { openPicker(); });
            m_picker->onCleared.connect([this] { clearBinding(); });
        }
        return m_picker.get();
    }
    if (m_picker) m_picker.reset();
    if (!m_combo) {
        m_combo = std::make_unique<jf::JComboBox>(sceneGraph());
        m_combo->onIndexChanged.connect([this](int idx) {
            if (syncing()) return;                       // our own setCurrentIndex echo
            const std::string path = writableConfigPath(bindPath());
            if (path.empty() || idx < 0) return;
            const Options o = optionsNow();
            if (o.searchable) return;                    // the dialog writes; the combo holds one row
            if (idx >= static_cast<int>(o.labels.size())) return;
            // A greyed entry writes nothing. The dropdown will not activate one and the arrows step over
            // it, so this is the belt to that braces — and the one place a future caller cannot get wrong.
            if (idx < static_cast<int>(o.enabled.size()) && !o.enabled[static_cast<size_t>(idx)]) return;
            const double v = o.values.empty() ? static_cast<double>(idx)
                                              : static_cast<double>(o.values[static_cast<size_t>(idx)]);
            Cache::instance().setConfigValue(path, v);
        });
    }
    return m_combo.get();
}

void EnumPickerWidget::syncControl() {
    if (m_picker) {
        // The field shows the resolved name and nothing else — no list to rebuild, no index to hold in
        // step. resolveLabel() already returns "" for an unset selector, which is what makes the
        // placeholder appear and the ✕ disappear together.
        m_picker->setText(resolveLabel(Cache::instance()));
        return;
    }
    if (!m_combo) return;
    const Options o = optionsNow();
    // Rebuild the items only when they actually change: setItems resets the selection, and a list rebuilt
    // every frame is a control that can never be interacted with.
    if (o.labels != m_shown) {
        m_shown = o.labels;
        m_combo->setItems(o.labels);
        m_combo->setItemsEnabled(o.enabled);
    }
    m_combo->setCurrentIndex(o.current);
}

std::unordered_map<std::string, EnumPickerWidget::PinClaim>
EnumPickerWidget::claimedPoolPins(const std::string& myPath) {
    std::unordered_map<std::string, PinClaim> claimed;   // pin name -> what holds it
    const MetaModel* m = Cache::instance().meta();
    if (!m) return claimed;
    const auto lb = myPath.find('['), rb = myPath.find(']'), dot = myPath.rfind('.');
    if (lb == std::string::npos || rb == std::string::npos || dot == std::string::npos || dot < rb)
        return claimed;
    const std::string myArr   = myPath.substr(0, lb);
    const std::string myElem  = myPath.substr(lb + 1, rb - lb - 1);
    const std::string myField = myPath.substr(dot + 1);

    auto readCfg = [](const std::string& pth) { return Cache::instance().configValue(pth); };

    // Is the field being edited itself a SHARED pool? Sharing takes two: a coil pool is shared hardware
    // for everything that draws from it, so a claim from one firing field must not lock another out.
    std::vector<MetaModel::PickerSet> mySets;
    const MetaModel::PickerSet* mine = applicablePickerSet(m, myPath, readCfg, mySets);
    const bool myShared = mine && mine->shared;

    for (const auto& [arrKey, ca] : m->configArrays()) {
        // Which of this array's fields are pin pickers. A field with no PickerSets holds no pin.
        std::vector<const MetaModel::ArrayField*> pinFields;
        bool hasEnabled = false;
        for (const auto& f : ca.fields) {
            if (!f.pickerSets.empty()) pinFields.push_back(&f);
            if (f.name == "enabled")   hasEnabled = true;
        }
        if (pinFields.empty()) continue;

        for (int i = 0; i < ca.count; ++i) {
            const std::string key = (i < int(ca.elementIds.size()) && !ca.elementIds[i].empty())
                                  ? ca.elementIds[i] : std::to_string(i);
            const std::string base = arrKey + "[" + key + "]";
            if (hasEnabled && readCfg(base + ".enabled") == 0.0) continue;   // released its claim
            for (const MetaModel::ArrayField* f : pinFields) {
                // Our own field stays offered — it is the one being changed.
                if (arrKey == myArr && key == myElem && f->name == myField) continue;
                std::vector<MetaModel::PickerSet> sets;
                const MetaModel::PickerSet* ps =
                    applicablePickerSet(m, base + "." + f->name, readCfg, sets);
                if (!ps) continue;
                const int val = static_cast<int>(std::lround(readCfg(base + "." + f->name)));
                // A field resting on a SENTINEL holds no hardware, so it claims nothing. This is not only
                // the obvious "None is greyed out for everyone" — the element being LOOKED at claimed it
                // too, through its OTHER pickers: a cylinder set to IGN1 still has a trailing coil and a
                // secondary injector sitting on None, and only the one field being edited is skipped
                // below. So "None — Cylinder 1" appeared beside the very cylinder that holds IGN1.
                if (!pickerValueIsPin(*ps, val)) continue;
                const std::string lbl = pickerLabelFor(*ps, val);
                if (lbl.empty()) continue;
                // Name the ARRAY as well as the element. The holder may now be in a different one,
                // and "held by 2" is not an answer when the reader is looking at a trigger stream and
                // the pin is on a sensor.
                // Named the way a human counts: its own label, else its id, else its ORDINAL — there is no
                // cylinder 0. `key` is the subscript, which is 0-based and is the right thing to address
                // an element WITH, and the wrong thing to show someone.
                const std::string elem = (i < int(ca.elementLabels.size()) && !ca.elementLabels[i].empty())
                                       ? ca.elementLabels[i]
                                       : (i < int(ca.elementIds.size()) && !ca.elementIds[i].empty())
                                       ? ca.elementIds[i] : std::to_string(i + 1);
                // A SHARED pool is one where naming the same pin twice is the WIRING, not a clash: a
                // distributor is every cylinder on one coil, wasted spark is each companion pair on one,
                // batch injection is a bank on one driver. Both sides have to be shared — then the option
                // is offered with the holder's name beside it, and nothing in a firing pool can lock
                // anyone out of anything.
                //
                // BOTH SIDES, not "the same field on the same array", which is what this said first and
                // which locked pins out for real: a leftover on a cylinder's SECONDARY injector greyed
                // that driver for its primary, and a trailing coil greyed it for a leading one. Those are
                // the same pool and the firmware shares them silently (EventScheduler.cpp:135,144); it is
                // not the studio's business which of them named the pin.
                //
                // A pin held from OUTSIDE a shared pool still greys: a sensor's input, an aux output. That
                // is a genuine cross-owner claim, and PinArbiter::claim would refuse it.
                PinClaim pc;
                pc.holder    = (ca.label.empty() ? arrKey : ca.label) + " " + elem;
                pc.path      = base + "." + f->name;   // the field to clear if the pin is ever moved here
                pc.shareable = myShared && ps->shared;
                claimed.emplace(lbl, pc);
            }
        }
    }
    return claimed;
}

// THE PRESS GOES TO THE CONTROL, always. This used to intercept it and open the dialog itself, which
// is why an ✕ inside the field could never be pressed: the interception happened above the control that
// owned the affordance. The picker field asks (onOpenRequested) and this answers, so every region of it
// means what it draws.
void EnumPickerWidget::openPicker() {
    const std::string bind = bindPath();
    Cache& C = Cache::instance();
    const MetaModel* meta = C.meta();
    if (bind.empty() || !meta || !enumpick::signal()) return;
    const int cur = static_cast<int>(std::lround(evalSource(bind).v));

    if (meta->isSignalField(bind)) {                       // *_src signal selector
        // A "sensor" selector offers only channels a sensor produces. The full bus list let a mis-pick
        // name a channel nothing acquires (reads 0 forever) or, worse, another sensor's channel — and
        // the ETB autocal WRITES the cal of whatever it is pointed at.
        const bool sensorsOnly = meta->isSensorField(bind);
        const std::set<std::string> allowed = sensorsOnly ? meta->sensorChannels() : std::set<std::string>{};
        std::vector<std::string> names; std::string current;
        for (const auto& [nm, id] : meta->signalMap()) {
            if (sensorsOnly && !allowed.count(nm)) continue;
            names.push_back(nm); if (id == cur) current = nm;
        }
        const std::string path = writableConfigPath(bind);
        enumpick::signal()(std::move(names), std::move(current), [path](std::string picked) {
            const MetaModel* m = Cache::instance().meta();
            if (!m || path.empty() || picked.empty()) return;   // the popup only ever CHOOSES now
            auto it = m->signalMap().find(picked);
            if (it != m->signalMap().end()) Cache::instance().setConfigValue(path, static_cast<double>(it->second));
        });
        return;
    }
    // A TABLE SELECTOR opens the same searchable picker for the same reason: 167 tables in a drop-down
    // is a list nobody finds anything in. Unsetting one is the field's ✕, not a row in here.
    if (meta->isTableField(bind)) {
        const std::vector<std::string> opts = C.tableOptionLabels(bind);
        std::vector<std::string> names = opts;
        const std::string current = (cur >= 0 && cur < static_cast<int>(opts.size())) ? opts[cur] : std::string{};
        const std::string path = writableConfigPath(bind);
        enumpick::signal()(std::move(names), current, [path, opts](std::string picked) {
            if (path.empty() || picked.empty()) return;
            for (size_t i = 0; i < opts.size(); ++i)
                if (opts[i] == picked) { Cache::instance().setConfigValue(path, static_cast<double>(i)); return; }
        });
    }
}

double EnumPickerWidget::sigilValue(const std::string& prop, const PanelElement& el, const Cache&) const {
    if (prop == "index" || prop == "value") { return evalSource(bindPath()).v; }
    return std::nan("");
}

std::vector<std::string> EnumPickerWidget::sigilNames() const { return { "index" }; }

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("enum", EnumPickerWidget, 100);

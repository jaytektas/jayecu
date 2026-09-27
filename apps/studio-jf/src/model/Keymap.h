#pragma once

// Keymap — the studio's GLOBAL, user-rebindable keyboard bindings, persisted through JSettings. A KeySpec /
// resolve / persist machinery, general enough that a single binding does the same thing everywhere. Actions fall into three groups:
//
//   • Value   — nudge a number by ±step (or ±10·step). Honored by EVERY numeric editor: table cells, axis
//               breakpoints, ConfigEdit, Array1D, Slider, Curve, and (via JWidget::s_valueKeyHook) the
//               framework spin boxes in the property dock / dialogs. This is why "." = increase works
//               identically across the whole app.
//   • Table   — move / extend / reduce the table cell selection.
//   • Surface — canvas edit-mode ops (group, ungroup).
//
// Each action binds to one keystroke (a named key or a printable char, plus modifiers). Defaults, human
// labels, dialog ordering, and the shifted-punctuation remap (so ">" fires the Shift+"." binding) are fixed.
// Bindings persist under stable string keys ("keys.<Action>"); load() also reads the old
// numeric "tableKeys.<n>" layout once, so a user's existing custom table bindings carry over.

#include <j/core/KeyEvent.h>
#include <j/config/Settings.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Keymap {
public:
    enum class Action {
        None,
        // --- Value (numeric editors everywhere) ---
        IncreaseValue, DecreaseValue,                       // ±step
        IncreaseValueLarge, DecreaseValueLarge,             // ±10·step
        // --- Table navigation ---
        CursorUp, CursorDown, CursorLeft, CursorRight,      // move the single-cell cursor (block-translate)
        ExtendUp, ExtendDown, ExtendLeft, ExtendRight,      // grow the region (push the pressed edge out)
        ShrinkUp, ShrinkDown, ShrinkLeft, ShrinkRight,      // reduce the region (pull the far edge in)
        // --- Surface (canvas edit mode) ---
        Group, Ungroup,
    };
    static constexpr int kCount = 19;                       // None + 18 actions

    enum class Group { Value, Table, Surface };

    // A single keystroke: either a named key (arrows/etc.) or a printable character, plus modifiers.
    struct KeySpec {
        bool               isChar = false;
        jf::JKeyEvent::JKey key   = jf::JKeyEvent::JKey::Unknown;   // used when !isChar
        char               ch     = 0;                              // base char when isChar (e.g. '.')
        bool               shift = false, ctrl = false, alt = false;
        bool valid() const { return isChar ? ch != 0 : key != jf::JKeyEvent::JKey::Unknown; }
        bool operator==(const KeySpec& o) const {
            return isChar == o.isChar && key == o.key && ch == o.ch
                && shift == o.shift && ctrl == o.ctrl && alt == o.alt;
        }
    };

    static Keymap& instance() { static Keymap k; return k; }

    // Resolve a live key event to an action (None if nothing is bound to it).
    Action action(const jf::JKeyEvent& ke) const {
        const KeySpec probe = fromEvent(ke);
        if (!probe.valid()) return Action::None;
        for (int i = 1; i < kCount; ++i)
            if (bindings_[static_cast<size_t>(i)] == probe) return static_cast<Action>(i);
        return Action::None;
    }
    KeySpec binding(Action a) const { return bindings_[static_cast<size_t>(a)]; }
    void    setBinding(Action a, const KeySpec& s) { bindings_[static_cast<size_t>(a)] = s; save(); }
    static KeySpec defaultBinding(Action a) { return defaults()[static_cast<size_t>(a)]; }   // built-in default (for Restore)

    // Every rebindable action, in display order (Value, then Table, then Surface).
    static const std::vector<Action>& allActions() {
        static const std::vector<Action> v = {
            Action::IncreaseValue, Action::DecreaseValue, Action::IncreaseValueLarge, Action::DecreaseValueLarge,
            Action::CursorUp, Action::CursorDown, Action::CursorLeft, Action::CursorRight,
            Action::ExtendUp, Action::ExtendDown, Action::ExtendLeft, Action::ExtendRight,
            Action::ShrinkUp, Action::ShrinkDown, Action::ShrinkLeft, Action::ShrinkRight,
            Action::Group, Action::Ungroup,
        };
        return v;
    }


    // The subset the (table-scoped) Key Bindings dialog shows: value ops + table navigation.
    static const std::vector<Action>& editableActions() {
        static const std::vector<Action> v = [] {
            std::vector<Action> out;
            for (Action a : allActions()) { const Group g = groupOf(a); if (g == Group::Value || g == Group::Table) out.push_back(a); }
            return out;
        }();
        return v;
    }

    static Group groupOf(Action a) {
        switch (a) {
            case Action::IncreaseValue: case Action::DecreaseValue:
            case Action::IncreaseValueLarge: case Action::DecreaseValueLarge: return Group::Value;
            case Action::Group: case Action::Ungroup:                         return Group::Surface;
            default:                                                          return Group::Table;
        }
    }
    static std::string groupLabel(Group g) {
        switch (g) {
            case Group::Value:   return "Value";
            case Group::Table:   return "Table";
            case Group::Surface: return "Surface";
        }
        return {};
    }

    static std::string label(Action a) {
        switch (a) {
            case Action::IncreaseValue:      return "Increase Value";
            case Action::DecreaseValue:      return "Decrease Value";
            case Action::IncreaseValueLarge: return "Increase Value (x10)";
            case Action::DecreaseValueLarge: return "Decrease Value (x10)";
            case Action::CursorUp:           return "Move Up";
            case Action::CursorDown:         return "Move Down";
            case Action::CursorLeft:         return "Move Left";
            case Action::CursorRight:        return "Move Right";
            case Action::ExtendUp:           return "Extend Selection Up";
            case Action::ExtendDown:         return "Extend Selection Down";
            case Action::ExtendLeft:         return "Extend Selection Left";
            case Action::ExtendRight:        return "Extend Selection Right";
            case Action::ShrinkUp:           return "Reduce Selection (top)";
            case Action::ShrinkDown:         return "Reduce Selection (bottom)";
            case Action::ShrinkLeft:         return "Reduce Selection (left)";
            case Action::ShrinkRight:        return "Reduce Selection (right)";
            case Action::Group:              return "Group";
            case Action::Ungroup:            return "Ungroup";
            default:                         return {};
        }
    }

    // --- KeySpec <-> event / text -------------------------------------------------------------
    // Build a KeySpec from a live event, remapping shifted punctuation back to base+Shift so a binding
    // stored as Shift+"." matches a physical ">".
    static KeySpec fromEvent(const jf::JKeyEvent& ke) {
        KeySpec s;
        s.shift = ke.shift; s.ctrl = ke.ctrl; s.alt = ke.alt;
        const unsigned char u = static_cast<unsigned char>(ke.utf8[0]);
        if (u >= 0x20 && !isNav(ke.key)) {                 // printable, non-navigation -> character binding
            char base = static_cast<char>(u);
            switch (u) {                                   // shifted glyph -> base key + Shift
                case '>': base = '.'; s.shift = true; break;
                case '<': base = ','; s.shift = true; break;
                case '+': base = '='; s.shift = true; break;
                case '_': base = '-'; s.shift = true; break;
                case '?': base = '/'; s.shift = true; break;
                case ':': base = ';'; s.shift = true; break;
                default: break;
            }
            s.isChar = true; s.ch = base;
        } else {
            s.isChar = false; s.key = ke.key;
        }
        return s;
    }

    static std::string toString(const KeySpec& s) {
        std::string out;
        if (s.ctrl)  out += "Ctrl+";
        if (s.shift) out += "Shift+";
        if (s.alt)   out += "Alt+";
        out += s.isChar ? std::string(1, s.ch) : keyName(s.key);
        return out;
    }
    static KeySpec fromString(const std::string& str) {
        KeySpec s;
        std::string rest = str;
        auto eat = [&](const char* tok, bool& flag) {
            const size_t n = std::string(tok).size();
            if (rest.size() >= n && rest.compare(0, n, tok) == 0) { flag = true; rest.erase(0, n); }
        };
        for (bool more = true; more;) {                    // strip modifier prefixes in any order
            const std::string before = rest;
            eat("Ctrl+", s.ctrl); eat("Shift+", s.shift); eat("Alt+", s.alt);
            more = (rest != before);
        }
        const jf::JKeyEvent::JKey k = keyFromName(rest);
        if (k != jf::JKeyEvent::JKey::Unknown) { s.isChar = false; s.key = k; }
        else if (rest.size() == 1)             { s.isChar = true;  s.ch = rest[0]; }
        return s;
    }

    void load() {
        auto& st = jf::JSettings::instance();
        for (Action a : allActions()) {
            std::string v = st.get<std::string>(settingsKey(a), std::string());
            if (v.empty()) {                               // migrate the old table-only "tableKeys.<n>" layout
                const int legacy = legacyIndex(a);
                if (legacy >= 0) v = st.get<std::string>("tableKeys." + std::to_string(legacy), std::string());
            }
            if (!v.empty()) { const KeySpec k = fromString(v); if (k.valid()) bindings_[static_cast<size_t>(a)] = k; }
        }
    }
    void save() const {
        auto& st = jf::JSettings::instance();
        for (Action a : allActions())
            st.set(settingsKey(a), jf::JVariant(toString(bindings_[static_cast<size_t>(a)])));
        st.saveJson();
    }

private:
    Keymap() { bindings_ = defaults(); load(); }

    std::array<KeySpec, kCount> bindings_{};

    // A "named" key (arrow / editing / paging / function) rather than a printable character. The named keys
    // sit in TWO contiguous enum blocks — Tab..End and PageUp..F12 — deliberately placed clear of the
    // ASCII-valued letter/digit/punctuation keys (e.g. '.' = 46, ',' = 44). A single Tab..F12 range would now
    // span those printables (F12 = 0xFD) and misclassify '.'/',' as named keys, so a char binding like "."
    // could never match. Test the two blocks explicitly instead.
    static bool isNav(jf::JKeyEvent::JKey k) {
        using K = jf::JKeyEvent::JKey;
        const uint32_t v = static_cast<uint32_t>(k);
        return (v >= static_cast<uint32_t>(K::Tab)    && v <= static_cast<uint32_t>(K::End))
            || (v >= static_cast<uint32_t>(K::PageUp) && v <= static_cast<uint32_t>(K::F12));
    }

    static KeySpec named(jf::JKeyEvent::JKey k, bool shift = false, bool ctrl = false) {
        KeySpec s; s.isChar = false; s.key = k; s.shift = shift; s.ctrl = ctrl; return s;
    }
    static KeySpec chr(char c, bool shift = false) {
        KeySpec s; s.isChar = true; s.ch = c; s.shift = shift; return s;
    }

    static std::array<KeySpec, kCount> defaults() {
        using K = jf::JKeyEvent::JKey;
        std::array<KeySpec, kCount> b{};
        b[static_cast<size_t>(Action::IncreaseValue)]      = chr('.');
        b[static_cast<size_t>(Action::DecreaseValue)]      = chr(',');
        b[static_cast<size_t>(Action::IncreaseValueLarge)] = chr('.', true);
        b[static_cast<size_t>(Action::DecreaseValueLarge)] = chr(',', true);
        b[static_cast<size_t>(Action::CursorUp)]           = named(K::Up);
        b[static_cast<size_t>(Action::CursorDown)]         = named(K::Down);
        b[static_cast<size_t>(Action::CursorLeft)]         = named(K::Left);
        b[static_cast<size_t>(Action::CursorRight)]        = named(K::Right);
        b[static_cast<size_t>(Action::ExtendUp)]           = named(K::Up,    true);
        b[static_cast<size_t>(Action::ExtendDown)]         = named(K::Down,  true);
        b[static_cast<size_t>(Action::ExtendLeft)]         = named(K::Left,  true);
        b[static_cast<size_t>(Action::ExtendRight)]        = named(K::Right, true);
        b[static_cast<size_t>(Action::ShrinkUp)]           = named(K::Up,    false, true);
        b[static_cast<size_t>(Action::ShrinkDown)]         = named(K::Down,  false, true);
        b[static_cast<size_t>(Action::ShrinkLeft)]         = named(K::Left,  false, true);
        b[static_cast<size_t>(Action::ShrinkRight)]        = named(K::Right, false, true);
        b[static_cast<size_t>(Action::Group)]              = chr('g');
        b[static_cast<size_t>(Action::Ungroup)]            = chr('u');
        return b;
    }

    // Stable settings key per action — a fixed token, NOT the enum index (so reordering the enum can never
    // rebind a user's saved key to the wrong action).
    static std::string settingsKey(Action a) {
        switch (a) {
            case Action::IncreaseValue:      return "keys.IncreaseValue";
            case Action::DecreaseValue:      return "keys.DecreaseValue";
            case Action::IncreaseValueLarge: return "keys.IncreaseValueLarge";
            case Action::DecreaseValueLarge: return "keys.DecreaseValueLarge";
            case Action::CursorUp:           return "keys.CursorUp";
            case Action::CursorDown:         return "keys.CursorDown";
            case Action::CursorLeft:         return "keys.CursorLeft";
            case Action::CursorRight:        return "keys.CursorRight";
            case Action::ExtendUp:           return "keys.ExtendUp";
            case Action::ExtendDown:         return "keys.ExtendDown";
            case Action::ExtendLeft:         return "keys.ExtendLeft";
            case Action::ExtendRight:        return "keys.ExtendRight";
            case Action::ShrinkUp:           return "keys.ShrinkUp";
            case Action::ShrinkDown:         return "keys.ShrinkDown";
            case Action::ShrinkLeft:         return "keys.ShrinkLeft";
            case Action::ShrinkRight:        return "keys.ShrinkRight";
            case Action::Group:              return "keys.Group";
            case Action::Ungroup:            return "keys.Ungroup";
            default:                         return {};
        }
    }

    // Index this action held in the OLD table-only "tableKeys.<n>" scheme, for one-time migration; -1 if the
    // action is new (Group/Ungroup) and had no legacy binding.
    static int legacyIndex(Action a) {
        switch (a) {
            case Action::CursorUp:           return 1;
            case Action::CursorDown:         return 2;
            case Action::CursorLeft:         return 3;
            case Action::CursorRight:        return 4;
            case Action::ExtendUp:           return 5;
            case Action::ExtendDown:         return 6;
            case Action::ExtendLeft:         return 7;
            case Action::ExtendRight:        return 8;
            case Action::ShrinkUp:           return 9;
            case Action::ShrinkDown:         return 10;
            case Action::ShrinkLeft:         return 11;
            case Action::ShrinkRight:        return 12;
            case Action::IncreaseValue:      return 13;
            case Action::DecreaseValue:      return 14;
            case Action::IncreaseValueLarge: return 15;
            case Action::DecreaseValueLarge: return 16;
            default:                         return -1;
        }
    }

    static std::string keyName(jf::JKeyEvent::JKey k) {
        using K = jf::JKeyEvent::JKey;
        switch (k) {
            case K::Up: return "Up"; case K::Down: return "Down"; case K::Left: return "Left"; case K::Right: return "Right";
            case K::Home: return "Home"; case K::End: return "End";
            case K::Tab: return "Tab"; case K::Return: return "Return"; case K::Space: return "Space";
            case K::Escape: return "Escape"; case K::Backspace: return "Backspace"; case K::Delete: return "Delete";
            default: break;
        }
        const uint32_t v = static_cast<uint32_t>(k);
        if (v >= static_cast<uint32_t>(K::A) && v <= static_cast<uint32_t>(K::Z)) return std::string(1, static_cast<char>(v));
        if (v >= static_cast<uint32_t>(K::_0) && v <= static_cast<uint32_t>(K::_9)) return std::string(1, static_cast<char>(v));
        return {};
    }
    static jf::JKeyEvent::JKey keyFromName(const std::string& n) {
        using K = jf::JKeyEvent::JKey;
        if (n == "Up") return K::Up; if (n == "Down") return K::Down; if (n == "Left") return K::Left; if (n == "Right") return K::Right;
        if (n == "Home") return K::Home; if (n == "End") return K::End;
        if (n == "Tab") return K::Tab; if (n == "Return") return K::Return; if (n == "Space") return K::Space;
        if (n == "Escape") return K::Escape; if (n == "Backspace") return K::Backspace; if (n == "Delete") return K::Delete;
        if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return static_cast<K>(static_cast<uint32_t>(n[0]));
        return K::Unknown;
    }
};

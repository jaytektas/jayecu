#pragma once

// PresetOptions — the settingselector control's preset model: a list of named options, each writing a set of
// config "path = value" pairs. Serialised to a compact string
// that rides the element's prop map: options joined by '\n', each "label|path=val,path=val" (labels/paths
// never contain '\n' or '|'). Picking an option writes all its pairs; an option "matches" when every pair
// equals the live config value.

#include "Cache.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <cstdint>
#include <vector>

struct PresetOptions {
    // A pair writes ONE field. It used to be a number and only a number, which is all a firing order
    // or a trigger wheel needs — but a preset that installs an output's PERSONALITY has to write the
    // two conditions and the slot's name as well, and those are a program and a string.
    //
    // So a value carries its kind. In the compact form a number is written bare, an expression with a
    // leading '~' (its SOURCE, compiled against the live meta when applied — never bytecode, which
    // would be a transplant across layouts), and text with a leading '$'. The separators of the
    // compact form (',', '|', newline) and '%' itself are percent-escaped inside those, so an
    // expression may contain commas — min(a, b) — without breaking the encoding.
    enum class Kind : uint8_t { Number, Expr, Text };
    struct Pair {
        std::string path;
        Kind        kind = Kind::Number;
        double      value = 0.0;   // Kind::Number
        std::string text;          // Kind::Expr (source) / Kind::Text
    };
    struct Option {
        std::string label;
        std::vector<Pair> pairs;
    };
    std::vector<Option> options;

    static constexpr double kEpsilon = 1e-4;

    bool empty() const { return options.empty(); }

    // First option whose ALL pairs equal the live config values, or -1 ("Custom" / no match).
    //
    // An EXPRESSION pair matches on the compiled PROGRAM, not on the source text: that is what lets a
    // template still recognise itself after a tune has been round-tripped through an ECU (where only
    // bytecode survives), and what makes the dropdown say "Custom" the moment somebody edits a
    // condition by hand — which is exactly the signal a template should give.
    int matchIndex() const {
        Cache& c = Cache::instance();
        for (size_t i = 0; i < options.size(); ++i) {
            const Option& o = options[i];
            if (o.pairs.empty()) continue;
            bool all = true;
            for (const Pair& pr : o.pairs) {
                if (!matches(c, pr)) { all = false; break; }
            }
            if (all) return static_cast<int>(i);
        }
        return -1;
    }
    // Write an option's whole set through to the ECU (one config edit per pair).
    void apply(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(options.size())) return;
        Cache& c = Cache::instance();
        for (const Pair& pr0 : options[idx].pairs) {
            Pair pr = pr0;
            pr.path = resolved(pr0.path);
            switch (pr.kind) {
                case Kind::Number: c.setConfigValue(pr.path, pr.value); break;
                case Kind::Text:   c.setConfigString(pr.path, pr.text); break;
                case Kind::Expr: {
                    // COMPILED HERE, against the live meta. A preset carries SOURCE, never bytecode:
                    // a program is only meaningful against the layout it was compiled for, so shipping
                    // bytecode in a template would be a transplant that silently means something else
                    // on the next firmware. A source that will not compile writes nothing at all.
                    const std::vector<uint8_t> code = compileFor(pr.path, pr.text);
                    if (!code.empty() || pr.text.empty()) c.setConfigBlob(pr.path, code);
                    break;
                }
            }
        }
    }

    // The bytecode a source compiles to for THIS field, or empty if it will not compile.
    static std::vector<uint8_t> compileFor(const std::string& path, const std::string& source);

    std::string toCompact() const {
        std::string s;
        for (size_t i = 0; i < options.size(); ++i) {
            if (i) s += '\n';
            s += options[i].label; s += '|';
            for (size_t k = 0; k < options[i].pairs.size(); ++k) {
                if (k) s += ',';
                s += options[i].pairs[k].path; s += '=';
                const Pair& pr = options[i].pairs[k];
                if      (pr.kind == Kind::Expr) { s += '~'; s += esc(pr.text); }
                else if (pr.kind == Kind::Text) { s += '$'; s += esc(pr.text); }
                else                             s += num(pr.value);
            }
        }
        return s;
    }
    static PresetOptions fromCompact(const std::string& s) {
        PresetOptions po;
        for (const std::string& line : split(s, '\n')) {
            if (line.empty()) continue;
            const size_t bar = line.find('|');
            Option o; o.label = (bar == std::string::npos) ? line : line.substr(0, bar);
            if (bar != std::string::npos) o.pairs = pairsFromText(line.substr(bar + 1));
            po.options.push_back(std::move(o));
        }
        return po;
    }

    // "path=value, path=~source, path=$text" ⇄ pairs — the text form the preset editor shows.
    static std::string pairsToText(const std::vector<Pair>& pairs) {
        std::string t;
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (i) t += ", ";
            t += pairs[i].path + "=";
            if      (pairs[i].kind == Kind::Expr) t += "~" + esc(pairs[i].text);
            else if (pairs[i].kind == Kind::Text) t += "$" + esc(pairs[i].text);
            else                                   t += num(pairs[i].value);
        }
        return t;
    }
    static std::vector<Pair> pairsFromText(const std::string& text) {
        std::vector<Pair> pairs;
        for (std::string tok : split(text, ',')) {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string path = trim(tok.substr(0, eq)), vs = trim(tok.substr(eq + 1));
            if (path.empty()) continue;
            if (!vs.empty() && (vs[0] == '~' || vs[0] == '$')) {
                Pair p; p.path = path;
                p.kind = (vs[0] == '~') ? Kind::Expr : Kind::Text;
                p.text = unesc(vs.substr(1));
                pairs.push_back(std::move(p));
                continue;
            }
            try { Pair p; p.path = path; p.value = std::stod(vs); pairs.push_back(std::move(p)); }
            catch (...) {}
        }
        return pairs;
    }

    // The separators of the compact form, escaped inside a source or a string so an expression may
    // contain a comma — min(a, b) — and a name may contain anything at all.
    static std::string esc(const std::string& in) {
        std::string out;
        for (char ch : in) {
            switch (ch) {
                case '%':  out += "%25"; break;
                case ',':  out += "%2C"; break;
                case '|':  out += "%7C"; break;
                case '\n': out += "%0A"; break;
                default:   out += ch;
            }
        }
        return out;
    }
    static std::string unesc(const std::string& in) {
        std::string out;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                const std::string h = in.substr(i + 1, 2);
                if      (h == "25") { out += '%';  i += 2; continue; }
                else if (h == "2C") { out += ',';  i += 2; continue; }
                else if (h == "7C") { out += '|';  i += 2; continue; }
                else if (h == "0A") { out += '\n'; i += 2; continue; }
            }
            out += in[i];
        }
        return out;
    }

private:
    // A COMPUTED SUBSCRIPT resolves the same way it does for a widget. A preset that sets up "the
    // selected output slot" writes outputs.output[@[#pc.output_sel]].on_expr, and without this the
    // Cache would be handed a path it cannot locate and would write nowhere, silently.
    static std::string resolved(const std::string& path);

    // Does the live config already hold what this pair would write?
    static bool matches(Cache& c, const Pair& pr0) {
        Pair pr = pr0;
        pr.path = resolved(pr0.path);
        if (pr.kind == Kind::Number) return std::fabs(c.configValue(pr.path) - pr.value) <= kEpsilon;
        if (pr.kind == Kind::Text)   return c.configString(pr.path) == pr.text;
        const std::vector<uint8_t> want = compileFor(pr.path, pr.text);
        std::vector<uint8_t> have = c.configBlob(pr.path);
        if (want.empty() && !pr.text.empty()) return false;      // a template that no longer compiles
        // The stored blob is the field's full width, zero-filled past the program; compare the
        // program, not the padding.
        if (have.size() > want.size()) {
            for (size_t i = want.size(); i < have.size(); ++i) if (have[i]) return false;
            have.resize(want.size());
        }
        return have == want;
    }

    static std::string num(double v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return b; }
    static std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }
    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> out; std::string cur;
        for (char ch : s) { if (ch == d) { out.push_back(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty() || !s.empty()) out.push_back(cur);
        return out;
    }
};

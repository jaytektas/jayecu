#pragma once

// ColorRules — value-driven colour + alerting: any number of rules, each an EXPRESSION deciding whether
// it applies, plus the channels it overrides while it does (bg/fg/accent/border) and whether it flashes.
// FIRST match wins. Because every control resolves its colours through the rule-aware cascade, adding a
// rule gives the whole widget set conditional colour + alerting for free.
//
// A rule used to be a numeric [start,end] interval. An interval can only ask one question about one
// number, and the question worth asking is usually narrower — "hot, but only once it is running", "lean
// under boost", "over target while closed loop is engaged". None of those is an interval, so the band
// had to be widened until it was true for the wrong reasons, or gone without. The expression says all of
// it, and says the interval too ("value > 100 && value < 200"), so the interval is not a second way of
// expressing something — it is a strictly weaker one, and it is gone.
//
// `value` is the control's own reading, spliced in before evaluation (CanvasWidget::substValue).
//
// Compact prop form ("ranges"): "bg,fg,accent,border,blink,when,text;…" — colours are "#rrggbb" or empty
// (= don't override that channel), blink is 0/1, `when` is the expression, and `text` is what a CAPTION
// says while the rule holds (empty = the caption keeps its own words). A label with two rules is
// therefore a lamp: red "FAULT" on one condition, green "READY" on the other, in the one widget that
// already knows how to draw text.
//
// `when` is ESCAPED, because an expression contains the delimiters: "\\" -> backslash, "\," -> comma,
// "\;" -> semicolon.
//
// PARSE-TIME NORMALISATION: a rule saved in the old 7-field form (start,end,…) arrives as the expression
// it always meant, "value >= start && value <= end". It is two lines at the parse boundary, not a second
// model kept alive downstream — nothing past fromCompact() knows an interval was ever a thing — and it
// is deletable the day no document holds one.
//
// The retired errorCondition property gets NO such courtesy and is simply ignored: it was a whole
// parallel mechanism rather than a different spelling of this one, and carrying a translation for it
// meant carrying the idea of it for ever. A document that holds one just stops painting until the rule
// is written by hand, which is the honest cost of removing a mechanism.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct ColorRule {
    std::string bg, fg, accent, border;   // "#rrggbb" or empty = inherit
    bool blink = false;                   // flash (alternate override ↔ base) to alert the user
    std::string when;                     // the expression that decides whether this rule applies
    std::string text;                     // caption while it holds ("" = the widget keeps its own)
};

struct ColorRules {
    std::vector<ColorRule> rules;

    // Split on an UNESCAPED delimiter, KEEPING the escapes intact. Walking char by char is what makes an
    // expression containing "," or ";" survive a round trip; find() cannot tell an escaped one apart.
    //
    // Splitting must NOT unescape, because the parse splits TWICE — rows on ';', then fields on ','.
    // Unescaping at the row level turned "\," into "," before the field split ever saw it, so the first
    // comma inside a call's arguments ended the expression and everything after it was lost. Unescape
    // once, at the leaf, where there is no further delimiter to protect.
    static std::vector<std::string> splitRaw(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[i + 1]; ++i; continue; }  // keep both
            if (s[i] == delim) { out.push_back(cur); cur.clear(); continue; }
            cur += s[i];
        }
        out.push_back(cur);
        return out;
    }
    static std::string unescape(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) { out += s[i + 1]; ++i; continue; }
            out += s[i];
        }
        return out;
    }
    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\\' || c == ',' || c == ';') out += '\\'; out += c; }
        return out;
    }

    // Old form: start,end,bg,fg,accent,border,blink[,when]  (7 or 8 fields, field 0 numeric)
    // New form: bg,fg,accent,border,blink,when[,text]        (6 or 7 fields, field 0 a colour or empty)
    // They are told apart by FIELD 0, not by a count: the old form's is a number, the new form's is a
    // "#rrggbb" or empty. The count alone stopped deciding it the moment `text` made a new row seven
    // fields long, which is exactly what an old row is.
    static bool looksOld(const std::vector<std::string>& f) {
        if (f.size() < 7) return false;
        if (f[0].empty()) return false;                 // new form's bg may be empty; old form's start never is
        return f[0][0] == '-' || f[0][0] == '.' || (f[0][0] >= '0' && f[0][0] <= '9');
    }

    static ColorRules fromCompact(const std::string& s) {
        ColorRules rr;
        for (const std::string& row : splitRaw(s, ';')) {
            if (row.empty()) continue;
            const std::vector<std::string> f = splitRaw(row, ',');
            auto at = [&](size_t i) { return i < f.size() ? unescape(f[i]) : std::string(); };
            ColorRule r;
            if (looksOld(f)) {
                r.bg = at(2); r.fg = at(3); r.accent = at(4); r.border = at(5);
                r.blink = (at(6) == "1");
                r.when  = at(7);
                // No text: nothing written in the interval form ever had one.
                if (r.when.empty()) {                   // the interval, written out as what it always meant
                    char e[96];
                    std::snprintf(e, sizeof e, "value >= %s && value <= %s",
                                  at(0).c_str(), at(1).c_str());
                    r.when = e;
                }
            } else {
                r.bg = at(0); r.fg = at(1); r.accent = at(2); r.border = at(3);
                r.blink = (at(4) == "1");
                r.when  = at(5);
                r.text  = at(6);          // absent in every rule written before captions could be banded
            }
            rr.rules.push_back(std::move(r));
        }
        return rr;
    }

    // ---- ZONES: the other kind of colour a gauge needs ------------------------------------------
    // A rule asks about the READING and recolours the control. A zone is a fixed span of the SCALE,
    // painted into the gauge's background whatever the reading is — a tacho's red 6500-8000 is there
    // with the engine off, and its orange 5500-6500 beside it. The two are not variants of one idea:
    // one is state, the other is the dial face, and a gauge wants both at once (the red zone stays put
    // while the needle passes through it and a rule turns the whole cell red).
    //
    // Bounds are in the same space as minValue/maxValue — the gauge converts all three through dispV
    // together, so a zone stays where it was drawn when the display unit changes.
    //
    // Compact prop form ("zones"): "start,end,#rrggbb;…", the same escaping as a rule's fields.
    //
    // A BOUND MAY BE AN EXPRESSION, exactly as minValue and maxValue may be. It has to be: a coolant
    // gauge's overheat band is 95 in Celsius and 203 in Fahrenheit, and the definition states it as
    // `{ useMetricOnInterface ? 95 : 203 }` — the same text the same gauge's maxValue already carries and
    // evaluates live. Parsing bounds with stod and DROPPING the rows that failed silently threw away the
    // bands on every gauge whose numbers move: coolant, RPM, oil pressure, 32 of this ini's 344.
    //
    // So a bound is kept as written, and `start`/`end` hold it as a number when it is one. Consumers ask
    // numOrExpr(startExpr, start), which is the number when there is no expression. The escaping already
    // covers what an expression contains — `select(x , 140 , 284)` is full of the commas the format
    // splits on, and escape() has always escaped those.
    struct Zone {
        double      start = 0.0, end = 0.0;    // the bound as a number, when it is one
        std::string startExpr, endExpr;        // the bound as written, when it is not
        std::string color;
    };

    static std::vector<Zone> zonesFromCompact(const std::string& s) {
        std::vector<Zone> out;
        for (const std::string& row : splitRaw(s, ';')) {
            if (row.empty()) continue;
            const std::vector<std::string> f = splitRaw(row, ',');
            if (f.size() < 3) continue;                       // a zone without all three says nothing
            Zone z;
            // A number stays a number; anything else is an expression to be evaluated where the gauge
            // knows its units and its channels. Nothing is dropped for being unparseable any more —
            // that is what silently cost the moving gauges their bands.
            auto bound = [](const std::string& t, double& num, std::string& expr) {
                try {
                    size_t n = 0;
                    const double v = std::stod(t, &n);
                    while (n < t.size() && (t[n] == ' ' || t[n] == '\t')) ++n;
                    if (n == t.size()) { num = v; expr.clear(); return; }
                } catch (...) {}
                num = 0.0; expr = t;
            };
            bound(unescape(f[0]), z.start, z.startExpr);
            bound(unescape(f[1]), z.end,   z.endExpr);
            z.color = unescape(f[2]);
            // Authored backwards is still a span — but only decidable here when both are plain numbers.
            // An expression is ordered where it is evaluated, which every consumer already does.
            if (z.startExpr.empty() && z.endExpr.empty() && z.end < z.start) std::swap(z.start, z.end);
            out.push_back(std::move(z));
        }
        return out;
    }

    static std::string zonesToCompact(const std::vector<Zone>& zs) {
        std::string out;
        char b[64];
        for (const Zone& z : zs) {
            if (!out.empty()) out += ";";
            auto bound = [&](double n, const std::string& expr) {
                if (!expr.empty()) { out += escape(expr); return; }   // escaped: an expression has commas
                std::snprintf(b, sizeof b, "%g", n);
                out += b;
            };
            bound(z.start, z.startExpr); out += ",";
            bound(z.end,   z.endExpr);   out += ",";
            out += escape(z.color);
        }
        return out;
    }

    // The zone that covers a value — FIRST match, like a rule, so overlapping zones are authored
    // top-to-bottom and read the same way. Bounds are INCLUSIVE at both ends: a tick sitting exactly on
    // 6500 belongs to the redline that starts there, which is where a gauge's own numbers put it.
    static const Zone* zoneAt(const std::vector<Zone>& zs, double v) {
        for (const Zone& z : zs) if (v >= z.start && v <= z.end) return &z;
        return nullptr;
    }

    std::string toCompact() const {
        std::string out;
        for (const ColorRule& r : rules) {
            if (!out.empty()) out += ";";
            out += escape(r.bg) + "," + escape(r.fg) + "," + escape(r.accent) + "," + escape(r.border)
                 + "," + (r.blink ? "1" : "0") + "," + escape(r.when);
            if (!r.text.empty()) out += "," + escape(r.text);   // only when there is one: an unused
                                                                // field on every rule is noise in the file
        }
        return out;
    }
};

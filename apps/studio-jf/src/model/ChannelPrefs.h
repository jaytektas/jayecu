#pragma once

// ChannelPrefs — what a CHANNEL looks like, wherever it is shown.
//
// A warning threshold belongs to the channel, not to the control that happens to be showing it. Oil
// pressure is low below the same number on the watch list, on a gauge and on a trace, and setting it three
// times is three chances to disagree with yourself. So this is one map, keyed by channel, saved with the
// document, and every control that draws a live value asks it.
//
// It is deliberately NOT a replacement for the per-widget ColorRules: those are expressions and can ask
// questions this cannot ("lean, but only under boost"). This is the plain shared layer underneath — the
// unit the number is read in, the scale it is plotted against, and the two thresholds that make it amber
// and red — which is the thing you want to set once and have everything obey.
//
// Every field is optional. An unset threshold is not "0", it is "no threshold": a channel with no warn
// band must not paint every reading amber because someone opened the dialog and pressed OK.

#include <j/config/Json.h>

#include <cmath>
#include <map>
#include <string>

struct ChannelPref {
    std::string unit;                       // display unit ("" = the channel's own)
    std::string format;                     // printf format ("" = the field's own digits)
    double min = NAN, max = NAN;            // plot/gauge scale  (NAN = auto)
    double warnLo = NAN, warnHi = NAN;      // amber outside these
    double alarmLo = NAN, alarmHi = NAN;    // red outside these
    // THE BANDING, AS EXPRESSIONS — the same ColorRules compact form a widget's "ranges" prop holds, but
    // stated once against the CHANNEL. This is the general mechanism and the four numbers above are the
    // quick way to write the common case; a rule can say what numbers cannot ("lean, but only under
    // boost"), and every control reading the channel obeys it without being told.
    std::string ranges;

    bool empty() const {
        return unit.empty() && format.empty() && ranges.empty() && !has(min) && !has(max)
            && !has(warnLo) && !has(warnHi) && !has(alarmLo) && !has(alarmHi);
    }
    static bool has(double v) { return !std::isnan(v); }

    // 0 = normal, 1 = warning, 2 = alarm. Checked outermost-first: a reading past the alarm is also past
    // the warning, and it is the alarm you need to be told about.
    int severity(double v) const {
        if ((has(alarmLo) && v < alarmLo) || (has(alarmHi) && v > alarmHi)) return 2;
        if ((has(warnLo)  && v < warnLo)  || (has(warnHi)  && v > warnHi))  return 1;
        return 0;
    }
};

class ChannelPrefs {
public:
    static ChannelPrefs& instance() { static ChannelPrefs p; return p; }

    // Null when the channel has no preferences of its own — the common case, and the caller then does
    // exactly what it did before this existed.
    const ChannelPref* find(const std::string& channel) const {
        const auto it = m_.find(channel);
        return it == m_.end() ? nullptr : &it->second;
    }
    ChannelPref& at(const std::string& channel) { return m_[channel]; }
    void set(const std::string& channel, const ChannelPref& p) {
        if (p.empty()) m_.erase(channel); else m_[channel] = p;   // an empty entry is not worth saving
    }
    void clear() { m_.clear(); }
    const std::map<std::string, ChannelPref>& all() const { return m_; }

    // The colours severity paints with — one place, so a warning is the same amber on a list row, a
    // readout and a trace.
    static const uint8_t* colorFor(int severity, const uint8_t* normal) {
        static const uint8_t amber[4] = { 0xff, 0x8c, 0x28, 0xff };
        static const uint8_t red[4]   = { 0xff, 0x45, 0x3a, 0xff };
        return severity == 2 ? red : severity == 1 ? amber : normal;
    }

    jf::JJson toJson() const {
        jf::JJson o = jf::JJson::object();
        for (const auto& [ch, p] : m_) {
            jf::JJson e = jf::JJson::object();
            if (!p.unit.empty())   e["unit"] = p.unit;
            if (!p.format.empty()) e["format"] = p.format;
            if (!p.ranges.empty()) e["ranges"] = p.ranges;
            auto put = [&e](const char* k, double v) { if (ChannelPref::has(v)) e[k] = jf::JJson(v); };
            put("min", p.min); put("max", p.max);
            put("warnLo", p.warnLo); put("warnHi", p.warnHi);
            put("alarmLo", p.alarmLo); put("alarmHi", p.alarmHi);
            o[ch] = std::move(e);
        }
        return o;
    }
    void load(const jf::JJson& j) {
        m_.clear();
        if (!j.isObject()) return;
        for (const auto& [ch, e] : j.obj()) {
            ChannelPref p;
            if (e.contains("unit"))   p.unit = e["unit"].str();
            if (e.contains("format")) p.format = e["format"].str();
            if (e.contains("ranges")) p.ranges = e["ranges"].str();
            auto get = [&e](const char* k, double& v) { if (e.contains(k)) v = e[k].number<double>(); };
            get("min", p.min); get("max", p.max);
            get("warnLo", p.warnLo); get("warnHi", p.warnHi);
            get("alarmLo", p.alarmLo); get("alarmHi", p.alarmHi);
            if (!p.empty()) m_[ch] = p;
        }
    }

private:
    std::map<std::string, ChannelPref> m_;
};

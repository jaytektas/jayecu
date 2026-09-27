#pragma once

// LineGraphModel — the livegraph control's multi-line list: each line is a channel + its own min/max range
// (with auto flags). Serialised compactly onto the element's "lines" prop ("channel,min,max,autoMin,autoMax"
// per line, plus a hidden flag, '\n'-separated). Line colours are auto-assigned from a fixed palette by index,
// so the editor need not host a colour picker.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct LineGraphModel {
    struct Line {
        std::string channel;
        double min = 0.0, max = 100.0;
        bool autoMin = true, autoMax = true;
        // Drawn? The legend's eye toggles this. A hidden line is still a LINE — it keeps its place, its
        // colour and its range, because hiding one to read the others is a glance, not an edit, and coming
        // back to a trace whose colour changed while it was away is worse than not hiding it at all.
        bool hidden = false;
    };
    std::vector<Line> lines;

    bool empty() const { return lines.empty(); }

    // A distinct colour per line index (RGBA), rotating through the palette.
    static const uint8_t* colorFor(int i, uint8_t out[4]) {
        static const uint8_t pal[8][3] = {
            {0x4f,0xc3,0xf7}, {0xff,0xb3,0x4d}, {0x81,0xc7,0x84}, {0xe5,0x73,0x73},
            {0xba,0x68,0xc8}, {0xff,0xf1,0x76}, {0x4d,0xb6,0xac}, {0xf0,0x6d,0xb4} };
        const uint8_t* p = pal[((i % 8) + 8) % 8];
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 0xff;
        return out;
    }

    std::string toCompact() const {
        std::string s;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) s += '\n';
            const Line& l = lines[i];
            s += l.channel; s += ','; s += num(l.min); s += ','; s += num(l.max);
            s += ','; s += (l.autoMin ? "1" : "0"); s += ','; s += (l.autoMax ? "1" : "0");
            s += ','; s += (l.hidden ? "1" : "0");
        }
        return s;
    }
    static LineGraphModel fromCompact(const std::string& s) {
        LineGraphModel m;
        for (const std::string& row : split(s, '\n')) {
            if (row.empty()) continue;
            const std::vector<std::string> f = split(row, ',');
            if (f.empty() || f[0].empty()) continue;
            Line l; l.channel = f[0];
            if (f.size() > 1) l.min = toD(f[1], 0.0);
            if (f.size() > 2) l.max = toD(f[2], 100.0);
            if (f.size() > 3) l.autoMin = (f[3] == "1");
            if (f.size() > 4) l.autoMax = (f[4] == "1");
            if (f.size() > 5) l.hidden  = (f[5] == "1");   // absent in rows written before the eye existed
            m.lines.push_back(std::move(l));
        }
        return m;
    }

private:
    static std::string num(double v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return b; }
    static double toD(const std::string& s, double d) { try { return std::stod(s); } catch (...) { return d; } }
    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> out; std::string cur;
        for (char ch : s) { if (ch == d) { out.push_back(cur); cur.clear(); } else cur += ch; }
        out.push_back(cur);   // trailing field (callers skip empty rows)
        return out;
    }
};

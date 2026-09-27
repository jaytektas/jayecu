// BandsWidget — edits a multi-position switch's voltage bands. See the header for what and why.

#include "BandsWidget.h"
#include "../../ui/WrapText.h"
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"
#include "../../model/TableImage.h"
#include "../../model/UnitManager.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using RGBA = std::array<uint8_t, 4>;
const RGBA kBtn{ 0x2f, 0x6f, 0xd6, 255 }, kBtnDim{ 0x3a, 0x3a, 0x3a, 255 }, kBtnTxt{ 0xff, 0xff, 0xff, 255 },
           kDim{ 0x8a, 0x8f, 0x98, 255 }, kBad{ 0xe0, 0x62, 0x5a, 255 }, kLive{ 0x4c, 0xc2, 0x6a, 255 };

// Column geometry, relative to the content rect's left edge — ONE table, read by the paint and the hit
// test, so every control is hit exactly where it is drawn.
constexpr float kDotX = 4.f, kPosX = 18.f, kPosW = 60.f, kLoX = 88.f, kEdgeW = 92.f, kHiX = 190.f,
                kCapX = 292.f, kCapW = 76.f, kRemX = 374.f, kRemW = 26.f, kAddW = 110.f, kBoxH = 22.f;

// How wide a captured band is, and the least clearance it keeps from a neighbour — in millivolts, turned
// into counts through the board's own conversion. A ladder's steps are hundreds of millivolts apart and the
// reading is steady to a few, so ±100 mV holds a button comfortably and 50 mV of clearance is still a gap.
constexpr double kHalfMv = 100.0, kGapMv = 50.0;

std::string fmt(double v, int digits) {
    char b[32]; std::snprintf(b, sizeof(b), "%.*f", std::max(0, digits), v); return b;
}

// Break `text` into lines no wider than `w` at the current font. The page authors captions with the real
// ruler; a widget drawing its own sentences has to do the same or they run off its right edge.
std::vector<std::string> wrap(const std::string& text, float w) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&] { if (!line.empty()) { out.push_back(line); line.clear(); } };
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ' ') {
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && jf::JTextHelper::measureWidth(trial) > w) { flush(); line = word; }
            else line = trial;
            word.clear();
        } else {
            word += text[i];
        }
    }
    flush();
    return out;
}
}  // namespace

// ---- where the bytes are ----------------------------------------------------------------------------

BandsWidget::Store BandsWidget::_store() const {
    Store s;
    Cache& c = Cache::instance();
    const MetaModel* meta = c.meta();
    std::string bind = bindPath();
    if (bind.size() >= 4 && bind[0] == '[' && bind[1] == '#' && bind.back() == ']')
        bind = bind.substr(2, bind.size() - 3);
    const size_t lb = bind.find('['), rb = bind.find(']');
    if (!meta || lb == std::string::npos || rb == std::string::npos || rb < lb) return s;
    const std::string arrayKey = bind.substr(0, lb);
    const std::string elemKey  = bind.substr(lb + 1, rb - lb - 1);
    if (arrayKey != "sensors.sensor") return s;
    const auto ai = meta->configArrays().find(arrayKey);
    if (ai == meta->configArrays().end()) return s;
    int idx = -1;
    for (size_t i = 0; i < ai->second.elementIds.size(); ++i)
        if (ai->second.elementIds[i] == elemKey) { idx = static_cast<int>(i); break; }
    if (idx < 0 && !elemKey.empty() && elemKey[0] >= '0' && elemKey[0] <= '9') idx = std::atoi(elemKey.c_str());
    if (idx < 0 || idx >= ai->second.count) return s;

    // The type the FIRMWARE will decode it as: the catalog's, or a generic input's configured one.
    const MetaModel::SensorType* st = c.sensorTypeOf(arrayKey, idx);
    if (!st || st->id != "multi_switch") return s;
    s.base = bind.substr(0, rb + 1);
    const std::string iface = c.elementOption(arrayKey, idx, "interface");
    s.analog = (iface == "analog_voltage" || iface == "engine_sync_voltage");

    const TableImage ti = c.resolveTable(s.base + ".cal");
    if (!ti.valid || ti.axes.empty() || ti.axes[0].nBase < 0) return s;
    const TableImage::Axis& ax = ti.axes[0];
    s.breaks = ax.breaksBase; s.bStride = ax.breakSize; s.bType = ax.breakType;
    s.nBase  = ax.nBase;      s.nMax    = ax.nMax;
    s.cells  = ti.cellBase;   s.cStride = ti.cellSize;  s.cType = ti.cellType; s.cScale = ti.cellScale;

    UnitManager& um = UnitManager::instance();
    s.unit = um.preferredUnit("AnalogRaw");
    if (s.unit.empty()) s.unit = "ADC";
    s.unitLabel = um.unitLabel(s.unit);
    s.perCount  = um.convert(1.0, "ADC", s.unit);
    if (s.perCount <= 0.0) s.perCount = 1.0;
    s.digits    = std::max(0, um.unitDigits(s.unit));

    // THE USER'S OWN FAULT LINE, and nothing else. This type used to carry a hard 200 mV floor that no
    // band could be calibrated under, on the reasoning that 0 V must never read as a press — but a
    // button that GROUNDS the line is a real position on most cruise stalks, and a dead 5 V reference is
    // caught by the power-good inputs anyway. So the only line here is Raw Min, WHEN the user has armed
    // it: below that they have said the pin is faulted, and a band down there contradicts them.
    s.floorCounts = 0.0;
    const MetaModel::Location en = meta->locate(s.base + ".diag_enable");
    const MetaModel::Location rm = meta->locate(s.base + ".diag_raw_min");
    if (en.valid() && rm.valid()) {
        const int bits = static_cast<int>(std::lround(c.readAt(en.offset, en.datatype, 1.0)));
        if (bits & 1) s.floorCounts = c.readAt(rm.offset, rm.datatype, 1.0);   // counts, as the firmware reads it
    }

    // The pin's RAW channel — the number the ECU reads on it, which is what a band is captured from.
    if (const std::vector<std::string>* pool = meta->hwPoolSignals(iface)) {
        const long src = std::lround(c.configValue(s.base + ".source"));
        if (src >= 0 && src < static_cast<long>(pool->size())) s.rawChan = (*pool)[static_cast<size_t>(src)];
    }
    s.isMulti = true;
    return s;
}

std::vector<BandsWidget::Band> BandsWidget::_read(const Store& s) const {
    std::vector<Band> out;
    if (!s.isMulti) return out;
    const Cache& c = Cache::instance();
    const int n = std::clamp(static_cast<int>(std::lround(c.readAt(s.nBase, "U08", 1.0))), 0, s.nMax);
    for (int k = 0; k + 1 < n; k += 2) {
        Band b;
        b.lo  = c.readAt(s.breaks +  k      * s.bStride, s.bType, 1.0);
        b.hi  = c.readAt(s.breaks + (k + 1) * s.bStride, s.bType, 1.0);
        b.pos = static_cast<int>(std::lround(c.readAt(s.cells + k * s.cStride, s.cType, s.cScale)));
        out.push_back(b);
    }
    return out;
}

// Store `bands`, ascending by voltage (the axis must ascend), as ONE undo step. The count goes LAST when
// the calibration grows and FIRST when it shrinks, so the ECU never holds a count reaching past the
// points that have been written.
void BandsWidget::_write(const Store& s, std::vector<Band> bands, int oldN, const char* what) {
    std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.lo < b.lo; });
    const int n = 2 * static_cast<int>(bands.size());
    Cache& c = Cache::instance();
    c.beginEdit();
    if (n < oldN) c.writeAt(s.nBase, "U08", 1.0, n);
    for (size_t k = 0; k < bands.size(); ++k) {
        const int p = 2 * static_cast<int>(k);
        c.writeAt(s.breaks +  p      * s.bStride, s.bType, 1.0, std::round(bands[k].lo));
        c.writeAt(s.breaks + (p + 1) * s.bStride, s.bType, 1.0, std::round(bands[k].hi));
        c.writeAt(s.cells  +  p      * s.cStride, s.cType, s.cScale, bands[k].pos);
        c.writeAt(s.cells  + (p + 1) * s.cStride, s.cType, s.cScale, bands[k].pos);
    }
    if (n >= oldN) c.writeAt(s.nBase, "U08", 1.0, n);
    c.endEdit(what);
}

// ---- the firmware's rules, said in words ------------------------------------------------------------

std::string BandsWidget::_problem(const Store& s, const std::vector<Band>& b) const {
    if (!s.isMulti) return {};
    if (!s.analog)
        return "Read through a digital pin, a multi-position switch is an ordinary on/off switch. These bands "
               "are used only on an analog pin.";
    const int n = static_cast<int>(std::lround(Cache::instance().readAt(s.nBase, "U08", 1.0)));
    if (n % 2) return "The calibration holds half a band (an odd number of points), so the ECU will not decode "
                      "it until it is whole.";
    if (b.empty()) return "No bands yet. Hold a button and press + Band.";
    for (size_t k = 0; k < b.size(); ++k)
        if (b[k].lo > b[k].hi)
            return "Band " + std::to_string(k + 1) + ": its low edge is above its high edge.";
    for (size_t k = 1; k < b.size(); ++k)
        if (b[k].lo <= b[k - 1].hi)
            return "Bands " + std::to_string(k) + " and " + std::to_string(k + 1) + " touch or overlap: a voltage "
                   "between them would be two positions at once.";
    if (s.floorCounts > 0.0 && b[0].lo < s.floorCounts)
        return "Band 1 reaches below Raw Low Threshold (" + fmt(s.floorCounts * s.perCount, s.digits) + " " +
               s.unitLabel + "). Detect Raw Low is armed, so the ECU would call that pin faulted at the "
               "very moment this position is pressed. Lower the threshold, or switch that check off.";
    return {};
}

// ---- capture / add / remove -------------------------------------------------------------------------

std::string BandsWidget::_capture(int row, double counts) {
    const Store s = _store();
    auto V = [&](double cnt) { return fmt(cnt * s.perCount, s.digits) + " " + s.unitLabel; };
    auto fail = [&](const std::string& why) { m_note = why; m_noteBad = true; return why; };
    if (!s.isMulti) return fail("This is not a multi-position switch's calibration.");
    if (!s.analog)  return fail("Bands are used only on an analog pin.");
    const int oldN = static_cast<int>(std::lround(Cache::instance().readAt(s.nBase, "U08", 1.0)));
    std::vector<Band> b = _read(s);
    if (s.floorCounts > 0.0 && counts < s.floorCounts)
        return fail("The pin reads " + V(counts) + ", under Raw Low Threshold (" + V(s.floorCounts) + "). You "
                    "have armed Detect Raw Low, so the ECU calls that a fault rather than a position. Lower "
                    "the threshold, or switch that check off, then capture again.");
    Band t;
    if (row >= 0) {
        if (row >= static_cast<int>(b.size())) return fail("There is no such band.");
        t = b[static_cast<size_t>(row)];
        b.erase(b.begin() + row);
    } else {
        if (2 * static_cast<int>(b.size()) + 2 > s.nMax)
            return fail("All " + std::to_string(s.nMax / 2) + " bands are in use.");
        for (int p = 0; p <= 15; ++p)       // the lowest position number not already taken
            if (std::none_of(b.begin(), b.end(), [p](const Band& x) { return x.pos == p; })) { t.pos = p; break; }
    }
    for (size_t k = 0; k < b.size(); ++k)
        if (counts >= b[k].lo && counts <= b[k].hi)
            return fail("The pin reads " + V(counts) + ", which is already the band for position " +
                        std::to_string(b[k].pos) + ". Hold a different button, or remove that band first.");

    UnitManager& um = UnitManager::instance();
    const double half = um.convert(kHalfMv, "ADC_mV", "ADC"), gap = um.convert(kGapMv, "ADC_mV", "ADC");
    double lo = counts - half, hi = counts + half;
    for (const Band& o : b) {                 // stay a gap clear of the neighbours on either side
        if (o.hi < counts) lo = std::max(lo, o.hi + gap);
        else if (o.lo > counts) hi = std::min(hi, o.lo - gap);
    }
    lo = std::ceil(std::max(lo, s.floorCounts));
    hi = std::floor(hi);
    if (lo > counts || hi < counts)
        return fail("The pin reads " + V(counts) + ", within " + V(gap) + " of the band beside it. Those two "
                    "positions read too close together to tell apart.");
    t.lo = lo; t.hi = hi;
    b.push_back(t);
    _write(s, b, oldN, row >= 0 ? "Capture band" : "Add band");
    m_note = "Position " + std::to_string(t.pos) + " captured at " + V(counts) + ": band " + V(lo) + " to " + V(hi) + ".";
    m_noteBad = false;
    return {};
}

bool BandsWidget::_remove(int row) {
    const Store s = _store();
    std::vector<Band> b = _read(s);
    if (!s.isMulti || row < 0 || row >= static_cast<int>(b.size()) || b.size() <= 1) return false;
    const int oldN = static_cast<int>(std::lround(Cache::instance().readAt(s.nBase, "U08", 1.0)));
    b.erase(b.begin() + row);
    m_active = nullptr;
    _write(s, b, oldN, "Remove band");
    m_note.clear();
    return true;
}

void BandsWidget::_writeEdge(int row, bool high, double displayValue) {
    const Store s = _store();
    if (!s.isMulti) return;
    Cache::instance().writeAt(s.breaks + (2 * row + (high ? 1 : 0)) * s.bStride, s.bType, 1.0,
                              std::round(displayValue / s.perCount));   // stored in counts, as the ECU reads it
}

void BandsWidget::_writePos(int row, double pos) {
    const Store s = _store();
    if (!s.isMulti) return;
    Cache& c = Cache::instance();
    c.beginEdit();
    c.writeAt(s.cells +  2 * row      * s.cStride, s.cType, s.cScale, std::round(pos));
    c.writeAt(s.cells + (2 * row + 1) * s.cStride, s.cType, s.cScale, std::round(pos));
    c.endEdit("Band position");
}

bool BandsWidget::_liveNow(const Cache& c, const std::string& chan, double& counts) {
    if (chan.empty() || !c.has(chan)) return false;
    const uint64_t f = c.telemetryFrames();
    const auto now = std::chrono::steady_clock::now();
    if (f != m_lastFrames) { m_lastFrames = f; m_lastFrameAt = now; }
    if (f == 0 || now - m_lastFrameAt > std::chrono::seconds(1)) return false;   // frames have stopped
    counts = c.value(chan);
    return true;
}

// ---- the row controls -------------------------------------------------------------------------------

void BandsWidget::_syncRows(const Store& s, const std::vector<Band>& b) {
    if (m_unitBuilt != s.unit) { m_rows.clear(); m_active = nullptr; m_unitBuilt = s.unit; }
    if (m_rows.size() != b.size()) { m_active = nullptr; m_rows.resize(b.size()); }
    const double step = (s.unit == "ADC") ? 1.0 : 0.01;
    for (size_t k = 0; k < m_rows.size(); ++k) {
        Row& row = m_rows[k];
        const int ri = static_cast<int>(k);
        auto mk = [&](double lo, double hi, double st, int dec, float w) {
            auto box = std::make_unique<jf::JDoubleSpinBox>(sceneGraph(), lo, hi, st, dec, w, kBoxH);
            box->setFocusPolicy(jf::JFocusPolicy::NoFocus);   // the canvas is its own focus domain
            return box;
        };
        if (!row.pos) {
            row.pos = mk(0.0, 15.0, 1.0, 0, kPosW);
            row.pos->onValueChanged.connect([this, ri](double v) { if (!m_syncing) _writePos(ri, v); });
        }
        if (!row.lo) {
            row.lo = mk(0.0, 65535.0 * s.perCount, step, s.digits, kEdgeW);
            row.lo->onValueChanged.connect([this, ri](double v) { if (!m_syncing) _writeEdge(ri, false, v); });
        }
        if (!row.hi) {
            row.hi = mk(0.0, 65535.0 * s.perCount, step, s.digits, kEdgeW);
            row.hi->onValueChanged.connect([this, ri](double v) { if (!m_syncing) _writeEdge(ri, true, v); });
        }
        m_syncing = true;
        row.pos->setValue(b[k].pos);
        row.lo ->setValue(b[k].lo * s.perCount);
        row.hi ->setValue(b[k].hi * s.perCount);
        m_syncing = false;
    }
}

// ---- paint ------------------------------------------------------------------------------------------

void BandsWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    uint8_t fgb[4];
    const uint8_t* fg = m_el ? fgOf(*m_el, fgb) : kDim.data();
    const bool text = jf::JTextHelper::hasAtlas();
    const float lh = text ? jf::JTextHelper::lineHeight() : 12.f;
    const Store s = _store();
    if (!s.isMulti) {   // unbound, or (in the editor) pointed at something that is not a band calibration
        if (text) wraptext::draw(buf, r.x + 4.f, r.y + 4.f,
                                 "Switch positions: shown for a multi-position switch.", kDim.data(), r.width - 8.f);
        return;
    }
    const std::vector<Band> b = _read(s);
    auto V = [&](double cnt) { return fmt(cnt * s.perCount, s.digits) + " " + s.unitLabel; };

    // --- the pin, now, and where the ECU puts it -------------------------------------------------------
    double live = 0.0;
    const bool isLive = _liveNow(c, s.rawChan, live);
    int liveBand = -1;
    std::string where;
    if (s.rawChan.empty())  where = "This input has no pin assigned.";
    else if (!isLive)       where = "Pin: no live reading. Connect the ECU to capture.";
    else {
        for (size_t k = 0; k < b.size(); ++k) if (live >= b[k].lo && live <= b[k].hi) liveBand = static_cast<int>(k);
        where = "Pin now " + V(live) + "   ->   " +
                (s.floorCounts > 0.0 && live < s.floorCounts ? std::string("under Raw Low: a fault")
                 : liveBand >= 0 ? "position " + std::to_string(b[static_cast<size_t>(liveBand)].pos)
                                 : std::string("in no band"));
    }
    // The pin line WRAPS in a narrow box, and the rows start below it (m_liveH, which the hit test reads).
    m_liveH = kLiveH;
    if (text) m_liveH += wraptext::extra(where, r.width - 8.f);
    if (text) wraptext::draw(buf, r.x + 4.f, r.y + 4.f, where, fg, r.width - 8.f);

    float y = r.y + m_liveH;
    if (s.analog) {
        // --- header + one row per band ------------------------------------------------------------------
        if (text) {
            const std::string u = s.unitLabel.empty() ? std::string() : " (" + s.unitLabel + ")";
            jf::JTextHelper::pushText(buf, r.x + kPosX, y, "Position", kDim.data(), kPosW + 8.f);
            jf::JTextHelper::pushText(buf, r.x + kLoX,  y, "Low" + u,  kDim.data(), kEdgeW);
            jf::JTextHelper::pushText(buf, r.x + kHiX,  y, "High" + u, kDim.data(), kEdgeW);
        }
        y += kHeadH;
        _syncRows(s, b);
        for (size_t k = 0; k < m_rows.size(); ++k, y += kRowH) {
            if (static_cast<int>(k) == liveBand)          // the band the pin is in right now
                buf.pushRectangle(r.x + kDotX, y + kBoxH * 0.5f - 4.f, 8.f, 8.f, kLive.data(), 4.f);
            m_rows[k].pos->setBounds({ r.x + kPosX, y, kPosW,  kBoxH });
            m_rows[k].lo ->setBounds({ r.x + kLoX,  y, kEdgeW, kBoxH });
            m_rows[k].hi ->setBounds({ r.x + kHiX,  y, kEdgeW, kBoxH });
            m_rows[k].pos->populateRenderPrimitives(buf);
            m_rows[k].lo ->populateRenderPrimitives(buf);
            m_rows[k].hi ->populateRenderPrimitives(buf);
            buf.pushRectangle(r.x + kCapX, y, kCapW, kBoxH, (isLive ? kBtn : kBtnDim).data(), 4.f);
            if (text) jf::JTextHelper::pushText(buf, r.x + kCapX + 10.f, y + (kBoxH - lh) * 0.5f, "Capture", kBtnTxt.data());
            if (b.size() > 1) {
                buf.pushRectangle(r.x + kRemX, y, kRemW, kBoxH, kBtnDim.data(), 4.f);
                if (text) jf::JTextHelper::pushText(buf, r.x + kRemX + 9.f, y + (kBoxH - lh) * 0.5f, "x", kBtnTxt.data());
            }
        }
        // --- + Band ----------------------------------------------------------------------------------------
        if (2 * static_cast<int>(b.size()) + 2 <= s.nMax) {
            buf.pushRectangle(r.x + kPosX, y + 4.f, kAddW, kBoxH, (isLive ? kBtn : kBtnDim).data(), 4.f);
            if (text) jf::JTextHelper::pushText(buf, r.x + kPosX + 12.f, y + 4.f + (kBoxH - lh) * 0.5f, "+ Band", kBtnTxt.data());
        }
        y += kBoxH + 14.f;
    } else {
        m_rows.clear(); m_active = nullptr;
    }

    // --- what is wrong, else what just happened, else how to use it -----------------------------------
    const std::string problem = _problem(s, b);
    std::string msg = problem;
    const uint8_t* col = kBad.data();
    if (msg.empty() && !m_note.empty()) { msg = m_note; col = m_noteBad ? kBad.data() : fg; }
    if (msg.empty()) {
        msg = "Hold a button and press Capture on its row, or + Band for a new position. Each band is centred "
              "on what the ECU reads. Position 0 is rest, the one position that may be held indefinitely.";
        col = kDim.data();
    }
    if (text)
        for (const std::string& line : wrap(msg, r.width - 8.f)) {
            if (y + lh > r.y + r.height) break;
            jf::JTextHelper::pushText(buf, r.x + 4.f, y, line, col);
            y += lh + 2.f;
        }
}

// ---- input ------------------------------------------------------------------------------------------

bool BandsWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    // Everything that is not a press belongs to the box the last press picked (see WiringWidget).
    if (in.kind != ControlInput::Kind::Press) {
        if (!m_active) return false;
        switch (in.kind) {
            case ControlInput::Kind::Move:    m_active->handleMouseMove(in.mx, in.my);    return true;
            case ControlInput::Kind::Release: m_active->handleMouseRelease(in.mx, in.my); return true;
            case ControlInput::Kind::Key:     return in.key && m_active->handleKeyEvent(*in.key);
            case ControlInput::Kind::Blur:    m_active->endEdit(); m_active->setFocused(false);
                                              m_active = nullptr;                          return true;
            default:                                                                       return false;
        }
    }
    const jf::JRect r = contentRect(screen);
    const Store s = _store();
    if (!s.isMulti || !s.analog) return false;
    const std::vector<Band> b = _read(s);
    auto hit = [&](float x, float y, float w, float h) {
        return in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
    };
    auto capture = [&](int row) {
        double live = 0.0;
        if (_liveNow(Cache::instance(), s.rawChan, live)) _capture(row, live);
        else { m_note = "No live reading from the ECU, so there is nothing to capture."; m_noteBad = true; }
    };
    float y = r.y + m_liveH + kHeadH;
    for (size_t k = 0; k < b.size(); ++k, y += kRowH) {
        if (hit(r.x + kCapX, y, kCapW, kBoxH)) { capture(static_cast<int>(k)); return true; }
        if (b.size() > 1 && hit(r.x + kRemX, y, kRemW, kBoxH)) { _remove(static_cast<int>(k)); return true; }
    }
    if (2 * static_cast<int>(b.size()) + 2 <= s.nMax && hit(r.x + kPosX, y + 4.f, kAddW, kBoxH)) { capture(-1); return true; }
    // A press in a box goes to that box, and the keys that follow go to the box the press picked.
    for (Row& row : m_rows)
        for (jf::JControl* box : { static_cast<jf::JControl*>(row.pos.get()), static_cast<jf::JControl*>(row.lo.get()),
                                   static_cast<jf::JControl*>(row.hi.get()) }) {
            if (!box) continue;
            const jf::JRect bb = box->bounds();
            if (!hit(bb.x, bb.y, bb.width, bb.height)) continue;
            if (m_active && m_active != box) { m_active->endEdit(); m_active->setFocused(false); }
            m_active = box;
            box->setFocused(true);
            box->handleMousePress(in.mx, in.my);
            return true;
        }
    return false;
}

// Self-registration — type key, palette order (after the wiring selector at 101), factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("bands", BandsWidget, 102);

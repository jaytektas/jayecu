#include "Autotune.h"

#include <algorithm>
#include <cmath>

namespace autotune {

double fullWeightOf(Resistance r) {
    switch (r) {
        case Resistance::Easy:   return 1.0;
        case Resistance::Hard:   return 12.0;
        case Resistance::Normal: break;
    }
    return 4.0;
}

DelayTable defaultDelay() {
    DelayTable d;
    d.xb = { 650.0, 2295.0, 7000.0 };          // rpm
    d.yb = { 10.0, 59.0, 160.0 };              // load, kPa
    // Row-major, load ascending: slowest at low flow, falling toward the sensor's own ~100 ms response.
    d.cells = { 350.0, 300.0, 200.0,
                250.0, 150.0, 100.0,
                200.0,  70.0,  40.0 };
    return d;
}

double DelayTable::at(double x, double y) const {
    if (!valid()) return 0.0;
    const auto clampSite = [](const std::vector<double>& b, double v, int& i, double& f) {
        const int n = static_cast<int>(b.size());
        if (n == 1 || v <= b.front()) { i = 0; f = 0.0; return; }
        if (v >= b.back())            { i = n - 2; f = 1.0; return; }
        i = 0;
        while (i + 2 < n && v >= b[i + 1]) ++i;
        const double span = b[i + 1] - b[i];
        f = span > 0.0 ? (v - b[i]) / span : 0.0;
    };
    int xi = 0, yi = 0; double xf = 0.0, yf = 0.0;
    clampSite(xb, x, xi, xf);
    clampSite(yb, y, yi, yf);
    const int nx = static_cast<int>(xb.size());
    const int x1 = std::min(xi + 1, nx - 1), y1 = std::min(yi + 1, static_cast<int>(yb.size()) - 1);
    const double c00 = cells[static_cast<size_t>(yi) * nx + xi], c10 = cells[static_cast<size_t>(yi) * nx + x1];
    const double c01 = cells[static_cast<size_t>(y1) * nx + xi], c11 = cells[static_cast<size_t>(y1) * nx + x1];
    return (c00 * (1 - xf) + c10 * xf) * (1 - yf) + (c01 * (1 - xf) + c11 * xf) * yf;
}

// WHERE A VALUE SITS, and the rule that keeps a reading inside the table. Past either end the site
// pins to the end bin with no fraction: a reading at 9000 rpm on a map that stops at 7000 was made at
// the last cell the table has, and crediting it there is the only honest thing available. (Refusing it
// would be defensible too, but it would silently make the top of every map unlearnable.)
Engine::Site Engine::siteOn(const std::vector<double>& b, double v) {
    Site s;
    const int n = static_cast<int>(b.size());
    if (n <= 1) return s;
    if (v <= b.front()) { s.i = 0;     s.f = 0.0; return s; }
    if (v >= b.back())  { s.i = n - 2; s.f = 1.0; return s; }
    int i = 0;
    while (i + 2 < n && v >= b[i + 1]) ++i;
    const double span = b[i + 1] - b[i];
    s.i = i;
    s.f = span > 0.0 ? (v - b[i]) / span : 0.0;
    return s;
}

void Engine::setGrid(std::vector<double> xb, std::vector<double> yb) {
    xb_ = std::move(xb);
    yb_ = std::move(yb);
    base_.clear();
    clear();
}

void Engine::setBase(std::vector<double> cells) { base_ = std::move(cells); }

void Engine::clear() {
    const size_t n = xb_.size() * yb_.size();
    w_.assign(n, 0.0);
    sum_.assign(n, 0.0);
    ring_.clear();
    st_ = Stats{};
    st_.cells = static_cast<int>(n);
    lastCol_ = lastRow_ = -1;
}

// The recent operating point nearest an instant. Nearest rather than interpolated because what is
// wanted is a PLACE the engine actually was, and half way between two of them is a place it was not.
const Sample* Engine::production(uint32_t at) const {
    const Sample* best = nullptr;
    uint32_t bestD = 0xFFFFFFFFu;
    for (const Sample& s : ring_) {
        const uint32_t d = s.ms > at ? s.ms - at : at - s.ms;
        if (d < bestD) { bestD = d; best = &s; }
    }
    return best;
}

void Engine::add(const Sample& s) {
    ++st_.total;
    lastCol_ = lastRow_ = -1;

    // The ring is the record of where the engine has BEEN, so it takes every sample — including the
    // ones a filter is about to throw away. A record rejected for a cold engine is still the truthful
    // answer to "what was the engine doing 300 ms ago", and dropping it would leave holes exactly
    // where the conditions were changing.
    ring_.push_back(s);
    const uint32_t keep = 3000;   // comfortably past the delay table's ceiling
    while (!ring_.empty() && s.ms - ring_.front().ms > keep) ring_.pop_front();

    const auto reject = [&](const char* why) { ++st_.filtered; st_.activeFilter = why; };

    if (xb_.size() < 2 || yb_.size() < 2) { reject("no grid"); return; }
    if (!s.ok)                            { reject("lambda not readable"); return; }

    // The delay is a property of the flow the gas is travelling through NOW, so it is read at the
    // current operating point — the same place the firmware and the reference implementation read it.
    const double dly = delay_.at(s.x, s.y);
    const uint32_t when = s.ms > static_cast<uint32_t>(dly) ? s.ms - static_cast<uint32_t>(dly) : 0u;

    // NOT ENOUGH HISTORY IS NOT A ZERO DELAY. Early in a session the ring does not reach back far
    // enough, and falling back to the newest point would credit the first second of every session to
    // the wrong cells — the one stretch where the operating point is guaranteed to be moving.
    const Sample* p = production(when);
    if (!p || (s.ms - p->ms) + 30u < static_cast<uint32_t>(dly)) { reject("waiting for history"); return; }

    for (size_t i = 0; i < filters_.size(); ++i) {
        const double v = i < p->filt.size() ? p->filt[i] : 0.0;
        if (filters_[i].rejects(v)) { reject(filters_[i].name.c_str()); return; }
    }

    if (p->target <= 0.0 || s.lambda <= 0.0) { reject("no target"); return; }

    // THE JUDGEMENT. The mixture that reached the sensor is `lambda`; it was asked for `target` and
    // was already being pushed by `ego`. So the fuel table at that point should have been delivering
    // ego x (lambda/target) of what it did — lean (lambda above target) asking for more.
    const double mult = p->ego * (s.lambda / p->target);
    const double pct  = (mult - 1.0) * 100.0;

    const Site sx = siteOn(xb_, p->x), sy = siteOn(yb_, p->y);
    const int nx = static_cast<int>(xb_.size());
    const int x0 = sx.i, x1 = std::min(sx.i + 1, nx - 1);
    const int y0 = sy.i, y1 = std::min(sy.i + 1, static_cast<int>(yb_.size()) - 1);
    const double wx[2] = { 1.0 - sx.f, sx.f }, wy[2] = { 1.0 - sy.f, sy.f };
    const int cx[2] = { x0, x1 }, cy[2] = { y0, y1 };
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            const double w = wx[a] * wy[b];
            if (w <= 0.0) continue;
            const size_t k = static_cast<size_t>(cy[b]) * nx + cx[a];
            w_[k]   += w;
            sum_[k] += w * pct;
        }

    ++st_.used;
    st_.activeFilter.clear();
    // The cell the record actually landed in — the nearest one, which is where a person looking at the
    // grid expects to see the cursor.
    lastCol_ = sx.f >= 0.5 ? x1 : x0;
    lastRow_ = sy.f >= 0.5 ? y1 : y0;
}

double Engine::weight(int col, int row) const {
    if (col < 0 || row < 0 || col >= cols() || row >= rows()) return 0.0;
    return w_[static_cast<size_t>(row) * xb_.size() + col];
}

double Engine::changePct(int col, int row) const {
    if (col < 0 || row < 0 || col >= cols() || row >= rows()) return 0.0;
    const size_t k = static_cast<size_t>(row) * xb_.size() + col;
    const double w = w_[k];
    if (w <= 0.0) return 0.0;

    // The weighted mean of what every record credited to this cell asked for, then scaled by how much
    // of the evidence bar it has cleared. A cell with a quarter of the weight moves a quarter of the
    // way — and the next pass, finding the error still there, moves it again.
    const double mean       = sum_[k] / w;
    const double confidence = std::min(1.0, w / fullWeightOf(set_.resistance));
    double pct = mean * confidence;

    pct = std::clamp(pct, -set_.maxCellPct, set_.maxCellPct);
    // …and the same limit expressed in the cell's own units, which needs the cell.
    if (k < base_.size() && base_[k] != 0.0) {
        const double absPct = std::fabs(set_.maxCellAbs / base_[k]) * 100.0;
        pct = std::clamp(pct, -absPct, absPct);
    }
    return pct;
}

double Engine::base(int col, int row) const {
    if (col < 0 || row < 0 || col >= cols() || row >= rows()) return 0.0;
    const size_t k = static_cast<size_t>(row) * xb_.size() + col;
    return k < base_.size() ? base_[k] : 0.0;
}

double Engine::proposed(int col, int row) const {
    const size_t k = static_cast<size_t>(row) * xb_.size() + col;
    const double b = k < base_.size() ? base_[k] : 0.0;
    return b * (1.0 + changePct(col, row) / 100.0);
}

bool Engine::hasProposal() const {
    for (int r = 0; r < rows(); ++r)
        for (int c = 0; c < cols(); ++c)
            if (changePct(c, r) != 0.0) return true;
    return false;
}

Stats Engine::stats() const {
    Stats s = st_;
    s.cells = cols() * rows();
    double wsum = 0.0, csum = 0.0;
    int    wn = 0;
    for (int r = 0; r < rows(); ++r)
        for (int c = 0; c < cols(); ++c) {
            const double w = weight(c, r);
            if (w > 0.0) { wsum += w; ++wn; }
            const double ch = std::fabs(changePct(c, r));
            if (ch > 0.0) { ++s.altered; csum += ch; s.maxChange = std::max(s.maxChange, ch); }
        }
    s.avgWeight = wn ? wsum / wn : 0.0;
    s.avgChange = s.altered ? csum / s.altered : 0.0;
    return s;
}

}  // namespace autotune

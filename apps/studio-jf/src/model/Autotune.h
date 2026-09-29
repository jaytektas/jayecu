#pragma once

// Autotune — the VE autotuner's arithmetic, with no window and no ECU attached.
//
// WHAT IT DOES. A wideband says what the mixture actually was; the tune says what it was asked to be.
// The ratio between them is how wrong the fuel table is AT THE POINT THAT MADE THAT GAS, and this
// accumulates those judgements per cell until each has enough evidence to be worth acting on. The
// output is a PROPOSAL — a percentage change per cell — that somebody else decides whether to apply.
//
// THREE THINGS MAKE IT MORE THAN A DIVISION:
//
//   TRANSPORT DELAY. A wideband reads gas that left the cylinder tens to hundreds of milliseconds
//   ago. Credit the reading to where the engine is NOW and every correction lands in the cell after
//   the one that earned it — worst exactly during a pull, which is the part of the map a tuner most
//   wants filled. So the recent operating points are kept and each reading is credited BACKWARDS.
//
//   THE CORRECTION ALREADY BEING APPLIED. If the ECU's own closed loop is adding 5 % and the mixture
//   is on target, the table is still 5 % wrong — the loop is simply hiding it. So a sample carries
//   the correction multiplier the ECU was applying when that gas was made (`ego`), and it MULTIPLIES
//   the measured error rather than being ignored. With the trims off it is 1.000 and this collapses
//   to the plain measurement, which is the same equation, not a special case.
//
//   WEIGHT. A reading taken between two cells belongs to both, in the proportion the table itself
//   would have interpolated them — anything else credits a cell for fuel it did not supply. A cell
//   moves the whole way only once it has collected enough weight to deserve it (`Resistance`), so one
//   stray sample nudges rather than rewrites.
//
// FILTERS ARE EVALUATED AT THE PRODUCTION INSTANT, not at the reading. Every one of them names a
// condition about the FUELLING that made this gas — cold engine, closed throttle, a tip-in being
// covered by accel enrichment — and by the time that gas reaches the sensor the throttle has stopped
// moving and the tip-in filter would pass a record it exists to reject. The reference implementation
// tests the log row as it stands; testing the row the gas came from is the same test asked at the
// moment it means something.
//
// UNITS. Everything internal is a PERCENT CHANGE on the cell (0 = leave it alone), never an absolute
// VE number: the caller owns the base table, so the engine never has to know what a VE of 85 means.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace autotune {

// How much evidence a cell needs before it is moved the whole way. Each accepted record contributes a
// total weight of 1.0, spread over the (up to four) cells it interpolates between — so a cell sitting
// dead on a breakpoint needs `Normal` = four records of its own to earn full authority, and one on a
// corner between four cells needs sixteen. Easy trusts what it is told; Hard wants it said repeatedly.
enum class Resistance { Easy, Normal, Hard };
double fullWeightOf(Resistance r);

struct Settings {
    Resistance resistance = Resistance::Normal;
    // AUTHORITY IS TWO LIMITS BECAUSE A CELL HAS TWO SIZES. A 50 % ceiling is generous on a VE of 90
    // and meaningless on a VE of 8, where the same percentage is under half a point; an absolute
    // ceiling is the reverse. Whichever binds first wins, so neither a tiny cell nor a large one can
    // be moved further in one pass than a person would have moved it by hand.
    double maxCellPct = 50.0;    // percent of the cell's own value
    double maxCellAbs = 50.0;    // in the cell's own units (VE points)
};

// One filter, in the shape a TunerStudio ini declares one: REJECT the record when `channel` stands in
// `op` relation to `value`. "Minimum RPM: 500" is therefore (Below, 500) — reject below it — which is
// why the display name and the comparison read in opposite directions.
struct Filter {
    enum class Op { Below, Above };
    std::string name;              // what to call it when it is the reason nothing is being learned
    std::string channel;           // the live channel it reads (resolved by the caller)
    Op          op    = Op::Below;
    double      value = 0.0;
    bool        enabled = true;
    bool rejects(double reading) const {
        return enabled && (op == Op::Below ? reading < value : reading > value);
    }
};

// One record. The caller fills it from wherever the data comes from — live telemetry or a replayed
// log — so the engine has no opinion about which, and a test can hand it a driving cycle by hand.
struct Sample {
    uint32_t ms     = 0;         // monotonic milliseconds; the delay is measured against this
    double   x      = 0.0;       // the table's own axis channels, at THIS instant
    double   y      = 0.0;
    double   lambda = 0.0;       // what the wideband reads now
    double   target = 1.0;       // what was commanded now (the production sample's is the one used)
    double   ego    = 1.0;       // the correction the ECU is applying now, as a multiplier (1 = none)
    bool     ok     = false;     // is the lambda reading usable at all (value mode: the value reading)
    std::vector<double> filt;    // one reading per configured filter, in order
    // VALUE MODE only (see Engine::setValueMode): the measured answer itself, and one reading per
    // steadiness channel, in the order they were declared.
    double   value  = 0.0;
    std::vector<double> steady;
};

// A channel that has to hold still before a VALUE-mode record counts: it may wander by at most `span`
// over the settle time. "rpm, 150" is an engine holding its speed; "tps, 1.0" a foot holding still.
struct Steady {
    std::string channel;
    double      span = 0.0;
};

// A coarse rpm x load surface of milliseconds: how long ago the gas being read now actually left the
// cylinder. Exhaust transport plus the sensor's own response, falling as mass flow rises.
struct DelayTable {
    std::vector<double> xb, yb;      // breakpoints (ascending)
    std::vector<double> cells;       // row-major, yb.size() rows x xb.size() cols
    double at(double x, double y) const;
    bool   valid() const { return !xb.empty() && !yb.empty() && cells.size() == xb.size() * yb.size(); }
};
// The reference implementation's own measured surface (650/2295/7000 rpm against 10/59/160 kPa),
// which is also where the firmware's 8x8 came from. A starting point for an ECU that states nothing.
DelayTable defaultDelay();

struct Stats {
    long        total    = 0;    // records offered
    long        filtered = 0;    // …rejected, by a filter or for being unusable
    long        used     = 0;    // …accumulated into a cell
    int         cells    = 0;    // cells in the grid
    int         altered  = 0;    // …that the proposal would change
    double      avgWeight = 0.0; // mean weight over the cells that have any
    double      avgChange = 0.0; // mean |change| over the altered cells, percent
    double      maxChange = 0.0; // largest |change|, percent
    std::string activeFilter;    // why the LAST record was rejected ("" = it was used)
};

class Engine {
public:
    // The grid being tuned: the live breakpoints of the table itself, so a cell here IS a cell there.
    // Resets everything accumulated — a proposal against one grid means nothing against another.
    void setGrid(std::vector<double> xb, std::vector<double> yb);
    void setBase(std::vector<double> cells);      // row-major rows*cols; only the authority clamp reads it
    void setDelay(DelayTable d) { delay_ = std::move(d); }
    void setFilters(std::vector<Filter> f) { filters_ = std::move(f); }
    void setSettings(Settings s) { set_ = s; }
    // VALUE MODE: the table is learned from a channel that MEASURES the cell's answer directly — the
    // manifold pressure a steady throttle and speed settle to IS what the Predicted MAP table should hold
    // there — rather than from a ratio against a target. No transport delay (the reading is about now),
    // no ego fold, and the proposal is the cell's new VALUE, not a percentage. A record counts only once
    // every steadiness channel has held within its span for `settleMs`: a value read while the engine is
    // still getting there is the transient, which is the one thing this table must not learn from.
    // Resets the accumulated records, like a new grid.
    void setValueMode(double settleMs, std::vector<Steady> steady);
    void setRatioMode();
    bool valueMode() const { return valueMode_; }
    const std::vector<Steady>& steadiness() const { return steady_; }

    const std::vector<Filter>& filters()  const { return filters_; }
    const Settings&            settings() const { return set_; }
    const DelayTable&          delay()    const { return delay_; }
    int cols() const { return static_cast<int>(xb_.size()); }
    int rows() const { return static_cast<int>(yb_.size()); }
    const std::vector<double>& xBins() const { return xb_; }
    const std::vector<double>& yBins() const { return yb_; }
    bool   hasBase() const { return base_.size() == xb_.size() * yb_.size(); }
    double base(int col, int row) const;

    void clear();                                  // forget every record; the grid and setup stay
    void add(const Sample& s);                     // offer one record

    // What the proposal would do to a cell, as a percentage of its current value. Already clamped by
    // both authority limits and scaled by how much evidence the cell has. (Value mode: the change the
    // proposed VALUE makes, expressed the same way, so every view and stat reads either mode.)
    double changePct(int col, int row) const;
    double weight(int col, int row) const;         // evidence accumulated, in records
    // The cell as the proposal would leave it. Needs setBase(); without it returns the change alone.
    double proposed(int col, int row) const;
    bool   hasProposal() const;                    // any cell would move

    Stats stats() const;
    // Which cell the LAST accepted record was credited to (-1 when there is none). The live cursor:
    // it is the difference between "nothing is arriving" and "everything is landing in one cell".
    int lastCol() const { return lastCol_; }
    int lastRow() const { return lastRow_; }

private:
    // Where a value sits on an axis: the lower bin and how far past it, 0..1.
    struct Site { int i = 0; double f = 0.0; };
    static Site siteOn(const std::vector<double>& b, double v);
    const Sample* production(uint32_t at) const;   // the ring entry nearest a past instant
    void credit(double x, double y, double v);     // spread one record over its (up to) four cells
    void addValue(const Sample& s);                // Engine::add, value mode
    double valueDelta(int col, int row) const;     // value mode: the clamped move, in the cell's units

    std::vector<double> xb_, yb_, base_;
    std::vector<double> w_, sum_;                  // per cell: weight, and weight x percent
    std::deque<Sample>  ring_;                     // recent operating points, for the delay lookback
    std::vector<Filter> filters_;
    DelayTable          delay_ = defaultDelay();
    Settings            set_;
    Stats               st_;
    int                 lastCol_ = -1, lastRow_ = -1;
    bool                valueMode_ = false;
    double              settleMs_  = 0.0;
    std::vector<Steady> steady_;
};

}  // namespace autotune

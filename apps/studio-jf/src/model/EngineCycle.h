#pragma once

// EngineCycle — one engine cycle's worth of events, in ENGINE DEGREES.
//
// The model behind the engine-cycle view: what each coil, injector and trigger input did across a
// single cycle, laid against crank (or eccentric-shaft) angle. Dwell start, spark, injector open and
// close, trigger teeth and TDC — the picture you want when asking "is it firing where I told it to".
//
// NATIVE-FIRST, and that decides the two things that matter here:
//
//   DEGREES, not milliseconds. Our scheduler already works in angle — it knows the angle it armed
//   each event at — so the producer emits what it knows. A time-domain record would force the host
//   to un-integrate it against an assumed RPM, which is wrong exactly when it matters most: during
//   a transient, where the speed is not constant across the revolution.
//
//   EDGES, not level samples. An edge carries its own angle, so a coil trace is two edges per cycle
//   rather than a stream of samples, and the dwell span between them is exact rather than
//   quantised by the sampling rate.
//
// A foreign ECU is adapted INTO this at the comms boundary (see TsCacheBridge for the same pattern
// applied to config): a rusEFI bridge converts its millisecond composite records into degrees and
// fills this in. Nothing above the link knows another protocol exists — and because that conversion
// is a reconstruction, the bridge marks it, so the view can be honest about which it is showing.
//
// The cycle SPAN is data, not a constant: 360 (two-stroke), 720 (four-stroke), 1080 (rotary — three
// eccentric-shaft revolutions, so the three rotor faces are distinct positions rather than overlays).

#include <cstdint>
#include <string>
#include <vector>

namespace enginecycle {

// What a trace represents. Drives grouping and default ordering in the view; a bridge that cannot
// tell sets Unknown rather than guessing.
enum class Signal : uint8_t { Unknown, Crank, Cam, Coil, Injector, Marker,
                              // The firing clock's own grid — where the ECU BELIEVED it was, as
                              // opposed to where a real tooth arrived. Its own kind so the two are
                              // never overlaid: the difference between them is the PLL error, and
                              // that is only visible if they are drawn apart.
                              Virtual };

// LEVEL or EVENT — the distinction that decides how a trace is stored and drawn.
//
// A coil or injector is a LEVEL: it goes high, stays high, comes back down, and the SPAN between
// those edges is the information (dwell, injector open time). Consecutive same-state edges are
// redundant there and get collapsed.
//
// A trigger tooth is an EVENT: an instant, with no "off" to pair it with. The decoder captures one
// polarity, so a tooth stream is 35 rising edges and no falling ones — collapsing same-state edges
// would reduce a whole wheel to a single mark, and asking for its span would produce one bar across
// the entire cycle. Both of those happened before this distinction existed.
inline bool isEventSignal(Signal s) {
    return s == Signal::Crank || s == Signal::Cam || s == Signal::Marker || s == Signal::Virtual;
}

// Is this angle MEASURED, or derived from something else? Our own ECU reports the angle it scheduled
// against. A time-domain source (rusEFI's composite log) has to interpolate angle from TDC markers
// and the RPM between them, which smears under acceleration. Whoever did the interpolation is the
// only one who knows, so it is recorded here rather than inferred by the view.
enum class Fidelity : uint8_t { Measured, Reconstructed };

// One state change. `high` is the electrical/logical sense the producer reports: a coil high is
// DWELLING (the spark is its falling edge), an injector high is OPEN.
struct Edge {
    double angle = 0.0;    // engine degrees, [0, cycleAngle)
    bool   high  = false;
};

// A high span, resolved from consecutive edges. `wrapped` marks a span that crosses the cycle
// origin — a dwell that starts at 700 deg and fires at 10 deg is one span, not two, and its
// duration is 30 deg rather than a negative number.
struct Span {
    double from = 0.0, to = 0.0;
    bool   wrapped = false;
};

// One channel's activity across the cycle.
struct Trace {
    std::string label;                 // display name ("Coil 3", "Inj 1", "Crank")
    Signal      signal = Signal::Unknown;
    int         index  = -1;           // 1-based channel number where meaningful, else -1

    // Rotary identity. A rotary's "coil 3" is really rotor 1 / face 3 / leading, and the scheduler
    // knows that mapping while a foreign ECU does not — so a bridge leaves these unset and the view
    // falls back to the label.
    int  rotor = -1, face = -1;
    bool trailing = false;             // rotary trailing plug

    std::vector<Edge> edges;           // ordered by angle once sort() has run
};

class Cycle {
public:
    // The cycle span in degrees: 360 / 720 / 1080. Everything is wrapped into [0, span).
    void   setCycleAngle(double deg) { cycle_ = (deg > 0.0) ? deg : 720.0; }
    double cycleAngle() const { return cycle_; }

    void     setFidelity(Fidelity f) { fidelity_ = f; }
    Fidelity fidelity() const { return fidelity_; }
    bool     isReconstructed() const { return fidelity_ == Fidelity::Reconstructed; }

    // RPM at the moment this cycle was captured, for the view's caption. 0 = unknown.
    void   setRpm(double rpm) { rpm_ = rpm; }
    double rpm() const { return rpm_; }

    // A caveat about THIS capture that the angles alone do not carry: "crank sync — one revolution",
    // "TRUNCATED: 7 edges lost". Only the producer knows these, and both change how the picture
    // should be read, so the view prints it rather than letting a short span or a missing lane be
    // mistaken for something the engine did. Empty when there is nothing to qualify.
    void               setNote(std::string n) { note_ = std::move(n); }
    const std::string& note() const { return note_; }

    // WHICH engine cycle this frame is, as the producer counted it. Not the frame's position in a
    // recording: consecutive captures are not consecutive cycles, and the gap between two frames'
    // sequence numbers is how many the engine turned in between. 0 = the producer did not say.
    void     setSequence(uint32_t s) { sequence_ = s; }
    uint32_t sequence() const { return sequence_; }

    // Find or create a trace. Identity is (signal, index) when index is meaningful, else the label —
    // so a producer can add edges without tracking handles.
    Trace& trace(const std::string& label, Signal signal = Signal::Unknown, int index = -1);

    // Add an edge, wrapping the angle into the cycle. Order does not matter; sort() fixes it.
    void addEdge(const std::string& label, double angle, bool high,
                 Signal signal = Signal::Unknown, int index = -1);

    // Sort every trace's edges by angle and drop consecutive duplicates of the same state (a
    // repeated "high" carries no information and would produce a zero-length span).
    void sort();

    // The HIGH spans of a trace, wrap-aware. An odd trailing edge (a rise with no matching fall
    // within the cycle) closes against the first edge, because on a cycle that is what happens next.
    std::vector<Span> spans(const Trace& t) const;

    // Angular length of a span, always positive, wrap included.
    double spanLength(const Span& s) const;

    // Wrap an arbitrary angle into [0, cycleAngle).
    double wrap(double angle) const;

    const std::vector<Trace>& traces() const { return traces_; }
    std::vector<Trace>&       traces()       { return traces_; }
    bool  empty() const { return traces_.empty(); }
    void  clear() { traces_.clear(); rpm_ = 0.0; fidelity_ = Fidelity::Measured; note_.clear(); sequence_ = 0; }

    // Stable display order: by signal group, then channel index, then label — so traces do not jump
    // around between frames as a producer adds them in whatever order events happened to fire.
    void sortTracesForDisplay();

private:
    std::vector<Trace> traces_;
    double      cycle_    = 720.0;
    double      rpm_      = 0.0;
    Fidelity    fidelity_ = Fidelity::Measured;
    uint32_t    sequence_ = 0;
    std::string note_;
};

} // namespace enginecycle

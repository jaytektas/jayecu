#include "CycleWire.h"

#include <cstring>

namespace cyclewire {
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Header {
    uint16_t magic, cycle_angle, total, first, count, cycle_seq;
    uint8_t  version, state, sync_level, dropped;
    uint32_t rpm_x10;
};

bool read_header(const uint8_t* d, size_t len, Header& h) {
    if (!d || len < kHeaderBytes) return false;
    h.magic       = rd16(d + 0);
    h.version     = d[2];
    h.state       = d[3];
    h.cycle_angle = rd16(d + 4);
    h.total       = rd16(d + 6);
    h.first       = rd16(d + 8);
    h.count       = rd16(d + 10);
    h.rpm_x10     = rd32(d + 12);
    h.sync_level  = d[16];
    h.dropped     = d[17];
    // Version 1 sent a reserved zero here; 2 puts the engine cycle number in it. Reading it
    // unconditionally would report cycle 0 for every v1 frame, which looks like an answer.
    h.cycle_seq   = (h.version >= 2) ? rd16(d + 18) : 0;
    return h.magic == kMagic;
}

const char* state_word(State s) {
    switch (s) {
        case State::Idle:      return "no capture requested";
        case State::Armed:     return "armed — waiting for the cycle to start";
        case State::Recording: return "recording";
        case State::Complete:  return "complete";
        case State::Stale:     return "stalled — last cycle before the trigger was lost";
    }
    return "unknown";
}

// The firmware's CycleSignal, and the label a tuner reads. Channels are 0-based on the wire and
// 1-based on screen, matching how the firmware's own pin log numbers them (IGN1 is channel 0).
struct Lane {
    enginecycle::Signal signal;
    const char*         prefix;
};

bool lane_of(uint8_t sig, Lane& out) {
    switch (sig) {
        case 0: out = { enginecycle::Signal::Coil,     "Coil"  }; return true;
        case 1: out = { enginecycle::Signal::Injector, "Inj"   }; return true;
        case 2: out = { enginecycle::Signal::Crank,    "Crank" }; return true;
        case 3: out = { enginecycle::Signal::Cam,      "Cam"   }; return true;
        case 4: out = { enginecycle::Signal::Virtual,  "Grid"  }; return true;
        default: return false;
    }
}

} // namespace

Result peek(const uint8_t* data, size_t len) {
    Result r;
    Header h{};
    if (!read_header(data, len, h)) {
        r.message = (len < kHeaderBytes) ? "capture reply too short" : "capture reply has a bad magic";
        return r;
    }
    r.ok      = true;
    r.state   = static_cast<State>(h.state);
    r.total   = h.total;
    r.dropped = h.dropped;
    r.message = state_word(r.state);
    return r;
}

Result decode(const uint8_t* data, size_t len, enginecycle::Cycle& out) {
    out.clear();

    Result r = peek(data, len);
    if (!r.ok) return r;

    Header h{};
    read_header(data, len, h);

    // CRANK-only sync means the ECU folded every angle into ONE revolution — it does not know which
    // half of the four-stroke cycle it is in, so it cannot place an event in 720 degrees. Drawing
    // those folded angles against a 720 axis would cram the whole engine into the left half and,
    // worse, make companion cylinders look like they fired at unrelated times. So the span shown IS
    // the span the data lives in, and the note says why it is short.
    const bool phase = (h.sync_level >= 2);
    out.setCycleAngle(phase ? (h.cycle_angle / 10.0) : 360.0);
    out.setRpm(h.rpm_x10 / 10.0);
    out.setFidelity(enginecycle::Fidelity::Measured);   // our ECU records the angle it fired at
    out.setSequence(h.cycle_seq);

    std::string note;
    if (!phase) {
        note = "crank sync — one revolution; companion cylinders overlay";
    }
    if (h.dropped) {
        if (!note.empty()) note += "   ";
        note += "TRUNCATED: " + std::to_string(h.dropped) + " edge(s) lost — the ECU's buffer filled";
    }
    // A stalled capture is the last cycle the engine turned, and the rpm in its header is the rpm it
    // was captured at — not the engine's now. Nothing in the ANGLES says so, and the frame is drawn
    // exactly like a live one, so it has to be stated. Without this a stopped engine reads as a
    // running one for as long as the user leaves the tab open.
    if (static_cast<State>(h.state) == State::Stale) {
        if (!note.empty()) note += "   ";
        note += "STALLED: the trigger was lost after this cycle — history, not a live reading";
    }
    out.setNote(note);

    const size_t have  = (len - kHeaderBytes) / kEdgeBytes;
    const size_t count = (h.count < have) ? h.count : have;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e   = data + kHeaderBytes + i * kEdgeBytes;
        const double   ang = rd16(e) / 10.0;
        const uint8_t  ch  = e[2];
        const uint8_t  fl  = e[3];

        Lane lane{};
        if (!lane_of(static_cast<uint8_t>((fl >> 1) & 0x07), lane))
            continue;                       // an unknown signal kind: skip it rather than mislabel it

        const int index = static_cast<int>(ch) + 1;     // 0-based on the wire, 1-based on screen
        out.addEdge(std::string(lane.prefix) + " " + std::to_string(index),
                    ang, (fl & 1) != 0, lane.signal, index);
    }

    // sort() also collapses consecutive same-state edges. That matters here and is not cosmetic: at
    // CRANK sync both half-buckets dispatch and a wasted-spark mask carries both companions, so the
    // ECU genuinely drives a coil twice at the same angle. The record keeps both (it reports what
    // happened); the display shows one dwell, which is what happened electrically.
    out.sort();
    out.sortTracesForDisplay();
    return r;
}

} // namespace cyclewire

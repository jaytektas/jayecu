#include "Platform/AssignmentResolver.h"
#include <cstring>

void descriptor_to_assignment(const BoardProfile&    profile,
                              const TriggerConfig&   trig,
                              EcuHardwareAssignment& out) noexcept
{
    std::memset(&out, 0, sizeof(out));   // all pointers null / edges RISING
    out.timebase = profile.timebase;

    // Generic trigger: ONE SLOT PER ROLE, and the index IS the role. Slots 0-1 are the cranks,
    // 2-5 the cams (which index cam[0..3] directly, so VVT identity is the slot). A stream is live
    // iff `enabled`; capture_index >= capture_count (the 255 sentinel) means it names no pin.
    //
    // A DUPLICATE PIN IS DROPPED, NOT BOUND. Two streams on one capture input used to bind the same
    // channel, and register_callback assigns unconditionally — so the second registration replaced
    // the first and that stream silently stopped receiving edges, with nothing anywhere reporting
    // it. The first claim wins and the loser is left unbound, which is visible as a stream that
    // never syncs rather than one that vanishes.
    const uint8_t max_streams = static_cast<uint8_t>(sizeof(trig.streams) / sizeof(trig.streams[0]));
    // Sized to the byte the pool index is stored in — capture_count is a runtime board
    // property, so a fixed 256 is what makes this a plain array rather than a search.
    bool taken[256] = {};
    for (uint8_t s = 0; s < max_streams; ++s) {
        const StreamsConfig& st = trig.streams[s];
        if (!st.enabled) continue;
        if (st.capture_index >= profile.capture_count) continue;        // names no pin
        if (taken[st.capture_index]) continue;                          // first claim wins
        taken[st.capture_index] = true;
        ICaptureChannel* ch    = profile.capture_resources[st.capture_index].channel;
        // A WIDTH STREAM ALWAYS CAPTURES BOTH EDGES, whatever `edge` says. It identifies its
        // reference by PULSE LENGTH, so it needs the high time, so it needs the closing edge — and
        // `edge` means something different here: it says which edge LEADS the pulse (Falling = the
        // pulse is low-going), which is a property of the signal, not a capture setting. The schema
        // has always documented exactly that; this took the field literally anyway and handed the
        // hardware RISING, so the matcher was fed opening edges with no closes, never measured a
        // width, never locked, and never became absolute.
        //
        // The symptom is quiet and points the wrong way: the cam edges ARE captured and show up in
        // the engine-cycle view, sync sits at CRANK for ever, and the error counters read zero —
        // because nothing is being REJECTED. Nothing is being decoded. A 36-2 crank plus a
        // single-pulse cam on the bench sat at sync level 1 with a perfectly good cam signal on the
        // pin, and everything downstream that needs phase (sequential fuel, wasted-spark pairing,
        // the cycle view's second revolution) silently did without it.
        const CaptureEdge edge = (st.primitive == 2) ? CaptureEdge::BOTH
                                                     : static_cast<CaptureEdge>(st.edge);
        const int crank_slot = crank_slot_of(s);
        const int cam_slot   = cam_slot_of(s);
        if (crank_slot == 0)      { out.crank_primary   = ch; out.crank_primary_edge   = edge; }
        else if (crank_slot == 1) { out.crank_secondary = ch; out.crank_secondary_edge = edge; }
        else if (cam_slot >= 0 && cam_slot < MAX_CAM_CHANNELS) {
            out.cam[cam_slot]               = ch;
            out.cam_edge[cam_slot]          = edge;
            out.cam_nominal_angle[cam_slot] = st.nominal_angle;
        }
    }

    // ---- Output-compare (ignition / low-side) -------------------------------
    // Fixed-function 1:1 binding: ign[i] = the i-th ignition output, ls[i] = the
    // i-th low-side output. The cyl→channel remap lives in CylinderConfig, not
    // here. The board's compare pool is laid out [IGN..., LS...], split at
    // ign_compare_count.
    if (profile.compare_resources && profile.compare_count > 0) {
        const uint8_t ign_n = (profile.ign_compare_count <= profile.compare_count)
                              ? profile.ign_compare_count : profile.compare_count;
        for (uint8_t i = 0; i < ign_n && i < MAX_IGN_CHANNELS; ++i) {
            out.ign[i] = profile.compare_resources[i].channel;
        }
        const uint8_t ls_n = static_cast<uint8_t>(profile.compare_count - ign_n);
        for (uint8_t i = 0; i < ls_n && i < MAX_INJ_CHANNELS; ++i) {
            out.ls[i] = profile.compare_resources[ign_n + i].channel;
        }
    }
}

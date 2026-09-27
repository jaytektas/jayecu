#pragma once

#include "ITransport.h"
#include "PacketParser.h"
#include "../../generated/ecu_telemetry.h"
#include "../../generated/ecu_config.h"
#include "../Signal/SignalBus.h"
#include "../Integration/OutputGate.h"   // the SD log gate: latch + timings      // packs the telemetry frame from the bus on demand


namespace Comms {

// ---------------------------------------------------------------------------
// CommsManager
//
// On each update() call it drains all registered transports byte-by-byte
// through the PacketParser state machine, then handles the resulting command.
//
// Telemetry reads ('A') pack the Omni wire frame FROM the SignalBus on demand
// (one read per channel, scaled per channel type) into a private telem_frame_,
// then CRC + transmit over that — so the CRC always covers exactly the bytes sent
// (no producer can rewrite it mid-flight). Config reads/writes ('r'/'w') operate
// directly on the global g_config.  A burn ('b') sets save_requested_ — the main
// loop persists to flash.
// ---------------------------------------------------------------------------

class CommsManager {
public:
    static constexpr size_t MAX_TRANSPORTS = 2;

    CommsManager();

    // The signal layer comms packs telemetry from (injected at startup).
    void set_signal_bus(SignalBus* bus) { signal_bus_ = bus; }

private:
    // Should the card be logging this instant? Two expressions through OutputGate; both empty means
    // the built-in "engine is turning" rule this replaced.
    bool gate_says_log(uint32_t now_ms);
public:

    bool add_transport(ITransport* transport);

    // Inject the CAN broker so the 'h' command can halt/resume periodic TX.

    void update(uint32_t delta_ms);

    // True if a burn ('b') wrote new constants; caller should flush to flash then clear.
    // Request a burn. The wire 'b' command and the `burn` CLI command both land here, so there is
    // ONE path that flags persistence rather than two that can drift apart.
    void request_save()         { save_requested_ = true; }
    bool save_requested() const { return save_requested_; }
    void clear_save_request()   { save_requested_ = false; }

    // Per-module reconfig-pending bitmask (bit i = JayecuShadowModule i was written).
    // The new values are already live in g_config RAM, but each owning subsystem must
    // re-read them only at its safe boundary (trigger/scheduler: engine-stopped). The
    // main loop polls this, applies the relevant modules' reconfigure() when safe, and
    // clears those bits. A write to one module never sets another's bit.
    uint32_t shadow_pending_mask() const { return shadow_pending_mask_; }
    void     clear_shadow(uint32_t mask)  { shadow_pending_mask_ &= ~mask; }
    // Raise a module's bit without a wire write. The 'w' handler is the only producer in the
    // firmware; a HOST TEST needs the same edge to exercise what a subsystem does when its region is
    // written, and reaching into the private mask from the test would be testing a different thing.
    void     mark_shadow(uint32_t mask)   { shadow_pending_mask_ |= mask; }

    // Pack the wire telemetry frame from the SignalBus into telem_frame_ and return it.
    // One read per channel, scaled per channel type (generated). Called at the start of
    // each realtime read so CRC + transmit operate on a stable, comms-private snapshot.
    const EcuTelemetry& packed_frame();
    // …into a buffer the CALLER owns. The 1 kHz datalog sampler runs on its own task and must not
    // write the comms snapshot while it is being transmitted, so it packs its own.
    void pack_into(EcuTelemetry& out);

private:
    void handle_packet(size_t stream_idx, uint8_t type_id, uint16_t sequence,
                       const uint8_t* data, uint16_t length);
    // sequence is the correlation id stamped into the frame — for a reply it's the REQUEST's sequence
    // (echoed), so the host resolves it against its pending pool; for an unsolicited frame it's a fresh id.
    void send_packet(size_t stream_idx, uint16_t sequence, uint8_t flag,
                     const uint8_t* payload, uint16_t length);


    static void packet_callback_bridge(void* ctx, size_t stream_idx, uint8_t type_id, uint16_t sequence,
                                       const uint8_t* data, uint16_t length);

    struct Stream {
        ITransport* transport = nullptr;
        PacketParser parser;
        uint16_t     logged_oversize = 0;   // last parser reject counts we've already logged (edge-trigger)
        uint16_t     logged_crc      = 0;
    };

    Stream   streams_[MAX_TRANSPORTS];
    size_t   stream_count_ = 0;

    SignalBus*   signal_bus_ = nullptr;       // packed-from on demand
    // THE SD LOG'S GATE — the latch and timers behind Datalog.log_when / log_until. Held here rather
    // than in SdProtocol because the CONDITION is a control decision and the file is not: the manager
    // decides whether to log, the card layer does the logging.
    OutputGate   log_gate_{};
    EcuTelemetry telem_frame_ = {};           // private snapshot for the in-flight response
    bool         save_requested_ = false;
    uint32_t     shadow_pending_mask_ = 0;   // per-module: a structural write awaits a safe reconfigure

    uint32_t     rx_packet_count_ = 0;

    // Write control removed: config/burn writes are accepted from any valid frame (no lock, no controller).
    uint8_t      last_command_ = 0;
};

// The composed CommsManager's packed telemetry frame, reachable without naming the instance — the same
// composition hook App uses for `pedalcal`. The bench 'datarec' command needs the EXACT record the
// datalogger writes, without the CLI knowing where the manager lives.
const EcuTelemetry& comms_packed_frame();
// The one instance, for code that must pack a frame of its own (the datalog sampler).
CommsManager& comms_manager();

} // namespace Comms

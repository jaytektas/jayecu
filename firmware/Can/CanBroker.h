#pragma once

#include "ICanChannel.h"
#include "ObdResponder.h"   // owned OBD-II responder (Mode 09 ISO-TP TX state)
#include "GenericCan.h"      // owned user-defined frame set — both directions, from the tune
#include "../Signal/SignalBus.h"
#include "../../generated/ecu_telemetry.h"

class Sensors;     // Tier-3 sensor runtime — source of active per-sensor DTCs (Mode 03)
class DtcManager;  // the unified DTC table — Mode 03 reads it, Mode 04 clears it

// ---------------------------------------------------------------------------
// CanBroker — central CAN subsystem.
//
// TX: GenericCan sends the tune's transmit frames, each on its own period. update() fires
//     whatever is due. Lua canSend() still goes straight out through send_frame().
//
// RX: every frame is offered to GenericCan (which indexes its own ids), to the sensor frame
//     cache, and to registered subscriber callbacks — the OBD responder and Lua.
//
// Thread safety: post_telemetry() and snapshot_inputs() may be called from
//     a different task than update().  A lightweight critical section protects
//     the shared data (disable-scheduler on FreeRTOS, no-op on native).
//
// Usage:
//   broker.add_bus(0, &can0);
//   broker.add_bus(1, &can1);
//   broker.reconfigure_generic(g_config.can, now_ms);   // the tune's own frames, both ways
//   broker.enable_obd(1);                               // OBD-II on bus 1
//   // In CanTask:
//   broker.update(platform_get_tick_ms());
//   // From EngineTask after each frame:
//   broker.post_telemetry(telem);
// ---------------------------------------------------------------------------

class CanBroker {
public:
    static constexpr uint8_t MAX_BUSES   = 2;
    static constexpr uint8_t MAX_RX_SUBS = 24;

    using RxCallback = void (*)(void* ctx, uint8_t bus, const CanFrame& frame, uint32_t tick_ms);

    struct RxSub {
        uint8_t    bus;        // matched bus, or ANY_BUS (0xFF)
        uint32_t   id;
        bool       ext;
        RxCallback cb;
        void*      ctx;
        uint32_t   mask;       // match when (frame.id & mask) == (id & mask); 0xFFFFFFFF=exact, 0=any id
    };
    static constexpr uint8_t ANY_BUS = 0xFF;

    // Registration
    bool add_bus(uint8_t idx, ICanChannel* ch);
    bool add_rx(uint8_t bus, uint32_t id, bool ext, RxCallback cb, void* ctx,
                uint32_t mask = 0xFFFFFFFFu);
    void clear_rx(void* ctx);   // drop all subscriptions owned by ctx (e.g. on a script reload)

    // OBD-II responder (registers RX 0x7DF on the given bus).
    void enable_obd(uint8_t bus);
    // Apply obd_enabled / obd_bus AFTER begin — the responder's RX subscriptions are registered for one
    // bus, so a config change had no effect until the next reset: you moved OBD to the other bus, the
    // tune said so, and the ECU went on answering (or not) on the old one. Cheap and idempotent; call it
    // whenever the config generation moves.
    void reconfigure_obd(bool enabled, uint8_t bus);

    // The tune's own frames, both directions. Mirrors reconfigure_obd(): idempotent, so a frame,
    // a rate or a per-bus enable edited in the studio takes effect now rather than at the next reset.
    void reconfigure_generic(const CanConfig& cfg, uint32_t now_ms);
    [[nodiscard]] const GenericCan& generic() const { return generic_; }

    // Bind a SignalBus that RX handlers write sensor data into.
    void set_signal_bus(SignalBus* bus) { signal_bus_ = bus; }

    // Generic CAN, so a CAN-interface sensor can acquire the field it reads. One decode serves the
    // signal bus and the sensor pipeline both; there is no second frame cache.
    GenericCan& generic_mut() { return generic_; }

    // Bind the Sensors module. A stale comment claimed this was for OBD Mode 03, which reads the DTC
    // table directly and never needed it. It is GenericCan that needs to know which channels a sensor
    // already publishes, so a receive field claiming one of them is reported rather than left to
    // alternate on the bus. Optional: without it the frames still run, the conflict just goes unsaid.
    void set_sensors(const Sensors* s) { sensors_ = s; }

    // Bind the unified DTC table — Mode 03 reads active codes, Mode 04 clears it.
    void set_dtc(DtcManager* d) { dtc_ = d; }

    // OBD Mode 04 (clear DTC) sets this flag; EngineTask polls + clears it.
    bool fault_clear_pending() const    { return fault_clear_pending_; }
    void consume_fault_clear()          { fault_clear_pending_ = false; }
    void request_fault_clear()          { fault_clear_pending_ = true; }

    // Repack telem_cache_ from the SignalBus. Called on the OBD request path — the only reader —
    // rather than every cycle, because 786 bytes of channel reads for a request that arrives a few
    // times a second is not work the engine cycle should carry.
    void refresh_telemetry();

    // Called by CanTask (or EngineTask) every ~1ms.
    void update(uint32_t tick_ms);

    // One-shot TX — used by Lua canSend().
    bool send_frame(uint8_t bus, const CanFrame& frame);

    // The tune's bit rate for a bus, so load can be expressed as a PERCENTAGE rather than a bit count.
    // main owns the config->platform translation and passes it on; the broker never reads g_config.
    void set_bus_bitrate(uint8_t idx, uint32_t hz) { if (idx < MAX_BUSES) bitrate_[idx] = hz; }
    // …and whether it is allowed to talk at all, so the no-acknowledgement check can exempt a bus
    // that is deliberately silent rather than reporting it as a fault every second.
    void set_bus_listen_only(uint8_t idx, bool on) { if (idx < MAX_BUSES) listen_only_[idx] = on; }

    // Publish per-bus health to the signal bus. Called once per update(); the window is a second, so
    // this does real work about once in a thousand ticks.
    void publish_stats(SignalBus& bus, uint32_t now_ms);

    // Accessors for RX callback implementations (ObdResponder, schema parsers).
    const EcuTelemetry& telemetry_ref() const     { return telem_cache_; }
    DtcManager*         dtc_ref()        const    { return dtc_; }
    uint8_t             obd_bus()        const    { return obd_bus_; }

private:
    void process_rx(uint32_t tick_ms);

    ICanChannel* buses_[MAX_BUSES]    = {};
    RxSub        rx_subs_[MAX_RX_SUBS];
    uint8_t      rx_count_ = 0;

    EcuTelemetry   telem_cache_         = {};
    SignalBus*     signal_bus_          = nullptr;
    DtcManager*    dtc_                 = nullptr;
    const Sensors* sensors_             = nullptr;   // which channels already have a producer
    bool           fault_clear_pending_ = false;

    // Per-bus traffic accounting for the load estimate. Bits rather than frames, because a frame is
    // between 47 and 130-odd bits depending on its payload and how much stuffing it provokes.
    struct BusStats {
        uint32_t tx_frames = 0, rx_frames = 0;   // since the last window
        uint32_t tx_bits   = 0, rx_bits   = 0;
        uint32_t tx_fail   = 0;                  // cumulative: the driver refused (mailboxes full)
    };
    BusStats stats_[MAX_BUSES];
    uint32_t bitrate_[MAX_BUSES]  = { 500000u, 500000u };
    bool     listen_only_[MAX_BUSES] = { false, false };   // a silent bus cannot ACK-fail
    uint32_t tx_fail_seen_[MAX_BUSES] = { 0, 0 };          // last window's refusal total
    uint32_t stats_window_ms_     = 0;

    // Bits a frame occupies on the wire, DLC included, at the WORST-CASE bit-stuffing allowance.
    // Overhead is 47 bits for an 11-bit id (SOF, id, RTR, IDE, r0, DLC, CRC + delimiter, ACK, EOF, IFS)
    // and 67 for a 29-bit one; stuffing can insert a bit every five, over the stuffable part only.
    // Erring high is deliberate: a load figure that flatters the headroom is the one that misleads.
    static uint32_t frame_bits(const CanFrame& f) {
        const uint32_t overhead = f.ext ? 67u : 47u;
        const uint32_t stuffable = overhead - 13u + 8u * f.dlc;   // CRC delimiter onward is not stuffed
        return overhead + 8u * f.dlc + stuffable / 4u;
    }

    bool          obd_enabled_ = false;
    uint8_t       obd_bus_     = 0;
    ObdResponder  obd_responder_;   // OBD-II responder (owns the Mode 09 ISO-TP TX state)
    GenericCan    generic_;         // the tune's frames (owns the deadlines and the receive index)
};

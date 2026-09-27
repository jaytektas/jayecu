#pragma once

#include "CanFrame.h"
#include "IsoTpTx.h"
#include "../../generated/ecu_telemetry.h"

class CanBroker;

// ---------------------------------------------------------------------------
// ObdResponder — ISO 15765-4 OBD-II over CAN.
//
// Owned by CanBroker; begin() registers it as an RX subscriber for the functional request ID
// (0x7DF) and the ECU's physical request ID (0x7E0, for multi-frame Flow Control). Single-frame
// modes answer inline; the multi-frame Mode 09 items (VIN / Calibration ID / ECU name) are sent
// via an ISO-TP segmenter (IsoTpTx) that the broker pumps through tick().
//
// Supported modes / PIDs:
//   0x01 — Current Data: 0x00,0x04,0x05,0x0B,0x0C,0x0D,0x0F,0x11,0x13,0x44,0x46
//   0x03 — Read Stored DTCs (single frame, first active code)
//   0x04 — Clear DTC Storage
//   0x09 — Vehicle Information: 0x00 (supported), 0x02 (VIN), 0x04 (Cal ID), 0x06 (CVN), 0x0A (ECU name)
// ---------------------------------------------------------------------------

class ObdResponder {
public:
    // Register RX subscriptions and bind the owning broker. Call from CanBroker::enable_obd().
    void begin(CanBroker* broker);

    // Pump the multi-frame ISO-TP sender. Call every CanBroker::update() tick.
    void tick(uint32_t tick_ms);

    // RxCallback trampolines — ctx is this ObdResponder*.
    static void on_request(void* ctx, uint8_t bus, const CanFrame& req, uint32_t tick_ms);
    static void on_flow_control(void* ctx, uint8_t bus, const CanFrame& fc, uint32_t tick_ms);

private:
    void handle_request(const CanFrame& req, uint32_t tick_ms);
    void handle_mode09(const CanFrame& req);
    static bool build_response(uint8_t pid, const struct EcuTelemetry& telem,
                               uint8_t* out_data, uint8_t* out_len);
    // Fill `out` with the full Mode 09 response payload for `pid` ([0x49][pid][NODI][data...]).
    // Returns the byte count, or 0 if the PID is unsupported.
    static uint8_t build_mode09(uint8_t pid, uint8_t* out, uint8_t cap);

    CanBroker* broker_ = nullptr;
    IsoTpTx    isotp_;
};

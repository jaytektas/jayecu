#include "ObdResponder.h"
#include "well_known_telem.h"   // wkt:: rename-safe telemetry accessors
#include "CanBroker.h"
#include "../Diagnostics/DtcManager.h"   // the one DTC table — Mode 03/04 read/clear it
#include "../../generated/ecu_telemetry.h"
#include "../version.h"         // JAYECU_SIGNATURE / version numbers -> Cal ID / CVN / ECU name
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// OBD-II over CAN (ISO 15765-4)
//
// Handled modes:
//   0x01 — Current Data (PIDs)
//   0x03 — Read Stored DTCs
//   0x04 — Clear DTC Storage
//
// Request:  ID=0x7DF, DLC=8, [length, mode, param...]
// Response: ID=0x7E8, DLC=8, [length, 0x40|mode, ...]
//
// Mode 03 reads active codes straight from the unified DTC table (DtcManager) as
// real P-codes — up to 2 per single ISO 15765-2 frame (7 data bytes).
// ---------------------------------------------------------------------------

static constexpr uint32_t SUPPORTED_PIDS_0120 =
    (1u << (32 - 0x04)) |
    (1u << (32 - 0x05)) |
    (1u << (32 - 0x0B)) |
    (1u << (32 - 0x0C)) |
    (1u << (32 - 0x0D)) |
    (1u << (32 - 0x0F)) |
    (1u << (32 - 0x11)) |
    (1u << (32 - 0x13)) |
    (1u << (32 - 0x20));    // PIDs 0x21-0x40 follow

// A SCAN TOOL ONLY ASKS FOR WHAT THE BITMASKS LIST. 0x44 and 0x46 were answered but never listed —
// 0x00 did not say "more at 0x20" and 0x20 itself was not answered — so no tool ever requested them.
static constexpr uint32_t SUPPORTED_PIDS_2140 = (1u << (32 - (0x40 - 0x20)));   // only 0x40: more at 0x41-0x60
static constexpr uint32_t SUPPORTED_PIDS_4160 =
    (1u << (32 - (0x44 - 0x40))) |
    (1u << (32 - (0x46 - 0x40)));

// Mode 09 supported InfoTypes (0x01-0x20): VIN, Cal ID, CVN, ECU name.
static constexpr uint32_t SUPPORTED_INFO_0120 =
    (1u << (32 - 0x02)) |   // VIN
    (1u << (32 - 0x04)) |   // Calibration ID
    (1u << (32 - 0x06)) |   // CVN
    (1u << (32 - 0x0A));    // ECU name

// The ECU's OBD identity strings. Fixed for the build (aftermarket ECUs report a placeholder VIN).
static constexpr char VIN_STR[17]      = {'J','A','Y','E','C','U','E','F','I','0','0','0','0','0','0','0','1'};
static constexpr char ECU_NAME_STR[20] = {'J','A','Y','E','C','U',' ','E','C','U',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};

static void send_resp(CanBroker* broker, const CanFrame& resp) {
    broker->send_frame(broker->obd_bus(), resp);
}

static CanFrame make_resp() {
    CanFrame r{};
    r.id  = 0x7E8;
    r.ext = false;
    r.dlc = 8;
    for (uint8_t i = 0; i < 8; i++) r.data[i] = 0xCC;
    return r;
}

void ObdResponder::begin(CanBroker* broker) {
    broker_ = broker;
    const uint8_t bus = broker->obd_bus();
    broker->add_rx(bus, 0x7DF, false, &ObdResponder::on_request,      this);  // functional request
    broker->add_rx(bus, 0x7E0, false, &ObdResponder::on_request,      this);  // physical request
    broker->add_rx(bus, 0x7E0, false, &ObdResponder::on_flow_control, this);  // multi-frame Flow Control
}

void ObdResponder::tick(uint32_t tick_ms) {
    if (!broker_) return;
    CanFrame f;
    // Emit at most a handful of frames per tick so an STmin-0 burst drains without starving TX.
    for (int i = 0; i < 8 && isotp_.poll(tick_ms, f); i++) send_resp(broker_, f);
}

void ObdResponder::on_flow_control(void* ctx, uint8_t /*bus*/, const CanFrame& fc, uint32_t /*tick_ms*/) {
    // A physical-address frame with an FC PCI (0x3n) belongs to an in-flight multi-frame TX;
    // a request PCI (single-frame 0x0n) is handled by on_request instead — don't double-dispatch.
    if ((fc.data[0] & 0xF0) != 0x30) return;
    static_cast<ObdResponder*>(ctx)->isotp_.on_flow_control(fc);
}

void ObdResponder::on_request(void* ctx, uint8_t /*bus*/, const CanFrame& req, uint32_t tick_ms) {
    static_cast<ObdResponder*>(ctx)->handle_request(req, tick_ms);
}

void ObdResponder::handle_request(const CanFrame& req, uint32_t /*tick_ms*/) {
    CanBroker* broker = broker_;
    if (!broker) return;

    if (req.dlc < 2) return;
    const uint8_t pci = req.data[0] & 0xF0;
    if (pci != 0x00) return;   // only Single-Frame requests (SF PCI 0x0n) carry a mode here
    const uint8_t mode = req.data[1];

    // -----------------------------------------------------------------------
    // Mode 0x01 — Current Data
    // -----------------------------------------------------------------------
    if (mode == 0x01) {
        if (req.dlc < 3 || req.data[0] < 2) return;
        const uint8_t pid = req.data[2];

        uint8_t resp_data[4] = {};
        uint8_t resp_len     = 0;
        broker->refresh_telemetry();    // the cache is filled here, on the request, or it is zeros
        if (!build_response(pid, broker->telemetry_ref(), resp_data, &resp_len)) return;

        CanFrame resp   = make_resp();
        resp.data[0]    = static_cast<uint8_t>(1 + resp_len);
        resp.data[1]    = 0x41;
        resp.data[2]    = pid;
        for (uint8_t i = 0; i < resp_len && i < 4; i++) resp.data[3 + i] = resp_data[i];
        send_resp(broker, resp);
        return;
    }

    // -----------------------------------------------------------------------
    // Mode 0x03 — Read DTCs
    //
    // SAE J1979 over ISO 15765-4: [0x43][count][DTC hi][DTC lo]... — TWO bytes a code. This used to send
    // three (a status byte J1979 does not have, so every scan tool mis-framed the codes) and capped the
    // answer at ONE code, because a single frame could not hold more that way. Two bytes a code fits two
    // in a single frame; beyond that the reply goes multi-frame through the same ISO-TP sender Mode 09
    // uses, up to its capacity — every active code, not the first.
    // -----------------------------------------------------------------------
    if (mode == 0x03) {
        constexpr uint8_t MAX_CODES = (IsoTpTx::CAP - 2) / 2;   // 31
        uint16_t dtcs[MAX_CODES] = {};
        uint8_t  found = 0;
        if (DtcManager* dtc = broker->dtc_ref()) found = dtc->list_active(dtcs, MAX_CODES);

        uint8_t payload[IsoTpTx::CAP];
        uint8_t n = 0;
        payload[n++] = 0x43;                         // 0x40 | 0x03
        payload[n++] = found;
        for (uint8_t i = 0; i < found; ++i) {
            payload[n++] = static_cast<uint8_t>((dtcs[i] >> 8) & 0xFFu);
            payload[n++] = static_cast<uint8_t>( dtcs[i]       & 0xFFu);
        }
        if (n <= 7) {                                // single frame: up to two codes
            CanFrame resp = make_resp();
            resp.data[0]  = n;                       // PCI: single-frame length
            for (uint8_t i = 0; i < n; i++) resp.data[1 + i] = payload[i];
            for (uint8_t i = 1 + n; i < 8; i++) resp.data[i] = 0x00;
            send_resp(broker, resp);
        } else if (!isotp_.busy()) {                 // multi-frame (one tester, one transfer at a time)
            isotp_.start(0x7E8, payload, n);
            CanFrame ff;
            if (isotp_.poll(0, ff)) send_resp(broker, ff);
        }
        return;
    }

    // -----------------------------------------------------------------------
    // Mode 0x04 — Clear DTC Storage
    // Sets a flag that EngineTask polls; actual clear happens there.
    // -----------------------------------------------------------------------
    if (mode == 0x04) {
        broker->request_fault_clear();
        CanFrame resp  = make_resp();
        resp.data[0]   = 0x01;
        resp.data[1]   = 0x44;   // positive response
        send_resp(broker, resp);
        return;
    }

    // -----------------------------------------------------------------------
    // Mode 0x09 — Vehicle Information (VIN / Calibration ID / CVN / ECU name)
    // -----------------------------------------------------------------------
    if (mode == 0x09) {
        handle_mode09(req);
        return;
    }
}

// Build the full Mode 09 response payload ([0x49][pid][NODI][data...]) for `pid`.
uint8_t ObdResponder::build_mode09(uint8_t pid, uint8_t* out, uint8_t cap) {
    auto emit = [&](const void* data, uint8_t n) -> uint8_t {
        const uint8_t total = 3 + n;                 // 0x49, pid, NODI, then n data bytes
        if (total > cap) return 0;
        out[0] = 0x49; out[1] = pid; out[2] = 0x01;  // NODI = 1 data item
        memcpy(out + 3, data, n);
        return total;
    };
    switch (pid) {
        case 0x00: {                                 // supported InfoTypes bitmask (2 bytes header + 4)
            if (cap < 6) return 0;
            out[0] = 0x49; out[1] = 0x00;
            out[2] = (SUPPORTED_INFO_0120 >> 24) & 0xFF;
            out[3] = (SUPPORTED_INFO_0120 >> 16) & 0xFF;
            out[4] = (SUPPORTED_INFO_0120 >>  8) & 0xFF;
            out[5] =  SUPPORTED_INFO_0120        & 0xFF;
            return 6;
        }
        case 0x02: return emit(VIN_STR, 17);         // VIN (multi-frame)
        case 0x04: {                                 // Calibration ID: firmware signature, 16 bytes
            char cal[16] = {};
            const char* sig = JAYECU_SIGNATURE;
            for (uint8_t i = 0; i < 16 && sig[i]; i++) cal[i] = sig[i];
            return emit(cal, 16);
        }
        case 0x06: {                                 // CVN — 4 bytes (version-derived, single frame)
            const uint8_t cvn[4] = { JAYECU_FIRMWARE_VERSION_MAJOR,
                                     JAYECU_FIRMWARE_VERSION_MINOR,
                                     JAYECU_FIRMWARE_VERSION_PATCH, 0x00 };
            return emit(cvn, 4);
        }
        case 0x0A: return emit(ECU_NAME_STR, 20);    // ECU name (multi-frame)
        default:   return 0;
    }
}

void ObdResponder::handle_mode09(const CanFrame& req) {
    if (req.dlc < 3 || req.data[0] < 2) return;
    const uint8_t pid = req.data[2];

    uint8_t payload[32];
    const uint8_t n = build_mode09(pid, payload, sizeof(payload));
    if (n == 0) return;                              // unsupported InfoType

    if (n <= 7) {                                    // single-frame (supported bitmask, CVN)
        CanFrame resp = make_resp();
        resp.data[0]  = n;                           // PCI: single-frame length
        for (uint8_t i = 0; i < n; i++) resp.data[1 + i] = payload[i];
        for (uint8_t i = 1 + n; i < 8; i++) resp.data[i] = 0x00;
        send_resp(broker_, resp);
        return;
    }

    // Multi-frame: hand the whole payload to the ISO-TP sender (VIN / Cal ID / ECU name). Drop the
    // request if a transfer is already in flight (one tester, one transfer at a time).
    if (!isotp_.busy()) {
        isotp_.start(0x7E8, payload, n);
        CanFrame ff;                                 // emit the First Frame right away
        if (isotp_.poll(0, ff)) send_resp(broker_, ff);
    }
}

bool ObdResponder::build_response(uint8_t pid, const EcuTelemetry& t,
                                   uint8_t* out, uint8_t* out_len) {
    switch (pid) {
        case 0x00: {
            // Supported PIDs 0x01-0x20 bitmask
            out[0] = (SUPPORTED_PIDS_0120 >> 24) & 0xFF;
            out[1] = (SUPPORTED_PIDS_0120 >> 16) & 0xFF;
            out[2] = (SUPPORTED_PIDS_0120 >>  8) & 0xFF;
            out[3] =  SUPPORTED_PIDS_0120        & 0xFF;
            *out_len = 4;
            return true;
        }
        case 0x20:
        case 0x40: {
            const uint32_t m = (pid == 0x20) ? SUPPORTED_PIDS_2140 : SUPPORTED_PIDS_4160;
            out[0] = (m >> 24) & 0xFF;
            out[1] = (m >> 16) & 0xFF;
            out[2] = (m >>  8) & 0xFF;
            out[3] =  m        & 0xFF;
            *out_len = 4;
            return true;
        }
        case 0x04: {
            // Calculated engine load: A = load_pct * 255 / 100
            // Use VE as a proxy for load
            const float ve = wkt::ve(t) / 10.0f;
            out[0] = static_cast<uint8_t>(std::min(ve * 255.0f / 100.0f, 255.0f));
            *out_len = 1;
            return true;
        }
        case 0x05: {
            // Coolant temp: A = clt_c + 40  (range -40..215 °C)
            const float clt = wkt::clt(t) / 10.0f;
            const int   raw = static_cast<int>(clt + 40.5f);
            out[0] = static_cast<uint8_t>(std::max(0, std::min(raw, 255)));
            *out_len = 1;
            return true;
        }
        case 0x0B: {
            // Intake MAP: A = kPa (0-255)
            const float kpa = wkt::map(t) / 10.0f;
            out[0] = static_cast<uint8_t>(std::min(kpa, 255.0f));
            *out_len = 1;
            return true;
        }
        case 0x0C: {
            // RPM: (256*A + B) / 4  →  A,B = rpm * 4   (rev_limiter.rpm is whole RPM)
            // OBD-II PID 0x0C tops out at 16383.75 RPM by spec; clamp the raw word.
            uint32_t raw = static_cast<uint32_t>(wkt::rpm(t)) * 4u;
            if (raw > 0xFFFFu) raw = 0xFFFFu;
            out[0] = (raw >> 8) & 0xFF;
            out[1] =  raw       & 0xFF;
            *out_len = 2;
            return true;
        }
        case 0x0D: {
            // Vehicle speed km/h: A = speed (0-255). The road speed Vehicle Speed (or CAN, or Lua)
            // publishes; a car with no road speed reads 0, which is what a scan tool expects of one.
            const float kph = wkt::vehicle_spd(t) / 10.0f;
            out[0] = static_cast<uint8_t>(std::max(0.0f, std::min(kph + 0.5f, 255.0f)));
            *out_len = 1;
            return true;
        }
        case 0x0F: {
            // IAT: A = iat_c + 40
            const float iat = wkt::iat(t) / 10.0f;
            const int   raw = static_cast<int>(iat + 40.5f);
            out[0] = static_cast<uint8_t>(std::max(0, std::min(raw, 255)));
            *out_len = 1;
            return true;
        }
        case 0x11: {
            // Throttle position: A = tps_pct * 255 / 100
            const float tps = wkt::tps(t) / 10.0f;
            out[0] = static_cast<uint8_t>(std::min(tps * 255.0f / 100.0f, 255.0f));
            *out_len = 1;
            return true;
        }
        case 0x13: {
            // Oxygen sensors present bitmask — report sensor 1 on bank 1
            out[0] = 0x01;
            *out_len = 1;
            return true;
        }
        case 0x44: {
            // Lambda (equivalence ratio): (256*A + B) / 32768 = lambda
            // Range 0-2, 16-bit: raw = lambda * 32768
            const float lam = wkt::lambda_target(t) / 1000.0f;
            const uint32_t raw = static_cast<uint32_t>(lam * 32768.0f);
            out[0] = (raw >> 8) & 0xFF;
            out[1] =  raw       & 0xFF;
            *out_len = 2;
            return true;
        }
        case 0x46: {
            // Ambient air temperature (use IAT as proxy): A = temp + 40
            const float iat = wkt::iat(t) / 10.0f;
            const int   raw = static_cast<int>(iat + 40.5f);
            out[0] = static_cast<uint8_t>(std::max(0, std::min(raw, 255)));
            *out_len = 1;
            return true;
        }
        default:
            return false;
    }
}

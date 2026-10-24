#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/start_control_config.h"

class ElectronicThrottle;
class DtcManager;

// StartControl — the start sequence. A press of the start button is LATCHED, and the crank request
// (crank_req, what the starter output reads) is held back until the engine is fit to crank:
//   - every enabled electronic throttle has passed its key-on check (ElectronicThrottle::start_proven —
//     cranking into a sweep only meets the interlock that holds fuel and spark), and
//   - with Prime Before Crank, the fuel rail is up: to Crank Above Pressure while a rail sensor reads
//     (cranking anyway after Longest Pressure Wait), for Prime Time when none does.
// start_pump asks the fuel pump to run for the wait. Once the request has been granted it lasts until
// the engine leaves STOPPED (the starter has engaged) and then FOLLOWS THE BUTTON, so holding it still
// gets another crank after the starter's rest and a released button does not. Key-off, a running
// engine, or Give Up After without the engine becoming fit all end the sequence; a new press starts it.
//
// A START THAT WENT WRONG SAYS SO. A rail that never reached Crank Above Pressure (and was cranked anyway)
// raises P0087 at level 1 and writes a console line with the pressure it got to; the next start that
// reaches pressure heals it. A press dropped at Give Up After writes a console line naming what it was
// still waiting on. Neither stops a start: they are the record of why one was slow or never happened.
class StartControl : public EngineModule {
public:
    enum State : uint8_t { IDLE = 0, WAIT_THROTTLE = 1, PRIMING = 2, CRANK = 3 };

    void init(const StartControlConfig& cfg, const ElectronicThrottle* etb = nullptr) { cfg_ = &cfg; etb_ = etb; }
    void on_config_change(const StartControlConfig& cfg) { cfg_ = &cfg; }
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    [[nodiscard]] State state() const { return state_; }
private:
    const StartControlConfig* cfg_ = nullptr;
    const ElectronicThrottle* etb_ = nullptr;
    DtcManager* dtc_ = nullptr;
    State    state_      = IDLE;
    bool     sw_prev_    = false;
    bool     follow_sw_  = false;   // CRANK: the starter has had its chance — the request follows the button
    uint32_t press_ms_   = 0;       // when the press was latched (the pump started then)
    uint32_t grant_ms_   = 0;       // when crank_req was granted
};

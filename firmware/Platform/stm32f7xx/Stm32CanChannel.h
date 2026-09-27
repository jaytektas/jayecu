#pragma once
//
// Stm32CanChannel — the real bxCAN driver behind ICanChannel (replaces CanChannelStub).
// jaytek_v1: CAN1 = PD0(RX)/PD1(TX), CAN2 = PB12(RX)/PB13(TX), both AF9.  CAN clock = APB1
// = 54 MHz.  Polled RX (the broker's process_rx drains FIFO0 each CanTask tick); the frame
// cache keeps the latest frame so an occasional missed poll only costs freshness, not data.
//
#include "../../Can/ICanChannel.h"
#include "stm32f7xx_hal.h"

class Stm32CanChannel : public ICanChannel {
public:
    explicit Stm32CanChannel(CAN_TypeDef* instance) : instance_(instance) {}

    // Bring the bus up: GPIO (AF9) + clock + bit timing + accept-all filter + start.
    // bitrate_hz: 500000 (OBD default) or 1000000 (most CAN sensors). Returns false on any
    // HAL failure (bus left down; is_up() stays false).
    //
    // loopback puts the peripheral in LOOPBACK mode, where every frame it transmits is ALSO delivered to
    // its own receive FIFO and no external node has to ACK. That makes the on-ECU CAN stack — OBD request
    // parsing, response building, ISO-TP segmentation and its flow control — testable on a bench with
    // nothing else on the wire, which is otherwise impossible: a lone CAN node cannot even complete a
    // transmission. It is a diagnostic mode, never a running one; the CLI turns it on and back off.
    // listen_only = SILENT mode: the peripheral receives everything and never transmits or ACKs, so a
    // bus you are only TAPPING cannot be disturbed by us. It is a bus property, not a per-frame one.
    bool begin(uint32_t bitrate_hz = 500000, bool loopback = false, bool listen_only = false);
    // Re-init this bus in (or out of) loopback at the current bitrate. Returns false if the bus is down.
    bool set_loopback(bool on);
    // Re-init at a different bitrate, keeping the current loopback setting.
    bool set_bitrate(uint32_t bitrate_hz);
    // Re-init with a whole settings set (composition tier applies config through platform_can).
    bool apply(uint32_t bitrate_hz, bool listen_only);
    void shutdown();                       // take the bus down (a bus wired to nothing should not sit on the wire)

    bool send(const CanFrame& frame) override;
    bool receive(CanFrame& frame) override;
    bool is_up() const override;
    CanErrorStatus error_status() const override;

private:
    // Bus-off is a TRANSITION worth counting, not just a state worth reading: the peripheral is set to
    // recover automatically (AutoBusOff), so a bus that drops out and recovers repeatedly looks healthy
    // every time you happen to look. The count is what makes that visible.
    mutable uint32_t  bus_off_n_  = 0;
    mutable bool      was_off_    = false;

    CAN_TypeDef*      instance_;
    CAN_HandleTypeDef hcan_{};
    bool              up_ = false;
    uint32_t          bitrate_ = 500000;   // remembered so set_loopback() can re-init at the same timing
    bool              loopback_ = false;
    bool              listen_only_ = false;
};

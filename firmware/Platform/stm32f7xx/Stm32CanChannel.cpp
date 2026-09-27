#include "Stm32CanChannel.h"

// Bit timing at CAN clock = APB1 = 54 MHz.  1 bit = (1 sync + BS1 + BS2) tq; we use
// BS1=15, BS2=2 -> 18 tq/bit, sample point ~88.9%.  prescaler = 54e6 / (bitrate * 18):
// 500k -> 6, 1M -> 3 (both exact).
bool Stm32CanChannel::begin(uint32_t bitrate_hz, bool loopback, bool listen_only) {
    bitrate_ = bitrate_hz;
    loopback_ = loopback;
    listen_only_ = listen_only;
    GPIO_InitTypeDef g{};
    g.Mode  = GPIO_MODE_AF_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    if (instance_ == CAN1) {
        __HAL_RCC_GPIOD_CLK_ENABLE();
        g.Pin = GPIO_PIN_0 | GPIO_PIN_1;            // PD0 = CAN1_RX, PD1 = CAN1_TX
        g.Alternate = GPIO_AF9_CAN1;
        HAL_GPIO_Init(GPIOD, &g);
        __HAL_RCC_CAN1_CLK_ENABLE();
    } else {                                         // CAN2
        __HAL_RCC_GPIOB_CLK_ENABLE();
        g.Pin = GPIO_PIN_12 | GPIO_PIN_13;          // PB12 = CAN2_RX, PB13 = CAN2_TX
        g.Alternate = GPIO_AF9_CAN2;
        HAL_GPIO_Init(GPIOB, &g);
        __HAL_RCC_CAN1_CLK_ENABLE();                // CAN2 is a slave of CAN1 — needs its clock
        __HAL_RCC_CAN2_CLK_ENABLE();
    }

    const uint32_t presc = 54000000u / (bitrate_hz * 18u);
    hcan_.Instance                  = instance_;
    hcan_.Init.Prescaler            = presc ? presc : 1u;
    hcan_.Init.Mode                 = loopback ? (listen_only ? CAN_MODE_SILENT_LOOPBACK : CAN_MODE_LOOPBACK)
                                               : (listen_only ? CAN_MODE_SILENT : CAN_MODE_NORMAL);
    hcan_.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    hcan_.Init.TimeSeg1             = CAN_BS1_15TQ;
    hcan_.Init.TimeSeg2             = CAN_BS2_2TQ;
    hcan_.Init.TimeTriggeredMode    = DISABLE;
    hcan_.Init.AutoBusOff           = ENABLE;        // auto-recover from bus-off
    hcan_.Init.AutoWakeUp           = DISABLE;
    hcan_.Init.AutoRetransmission   = ENABLE;
    hcan_.Init.ReceiveFifoLocked    = DISABLE;
    hcan_.Init.TransmitFifoPriority = DISABLE;
    if (HAL_CAN_Init(&hcan_) != HAL_OK) return false;

    // Accept-all filter -> RX FIFO0 (the broker dispatches/caches by id in software).
    CAN_FilterTypeDef f{};
    f.FilterBank           = (instance_ == CAN1) ? 0u : 14u;   // CAN1: 0-13, CAN2: 14-27
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = 0; f.FilterIdLow      = 0;
    f.FilterMaskIdHigh     = 0; f.FilterMaskIdLow  = 0;        // mask 0 = don't care = accept all
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;
    if (HAL_CAN_ConfigFilter(&hcan_, &f) != HAL_OK) return false;

    if (HAL_CAN_Start(&hcan_) != HAL_OK) return false;
    up_ = true;
    return true;
}

bool Stm32CanChannel::send(const CanFrame& frame) {
    if (!up_) return false;
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan_) == 0) return false;   // TX full
    CAN_TxHeaderTypeDef h{};
    if (frame.ext) { h.ExtId = frame.id; h.IDE = CAN_ID_EXT; }
    else           { h.StdId = frame.id; h.IDE = CAN_ID_STD; }
    h.RTR               = frame.rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    h.DLC               = frame.dlc;
    h.TransmitGlobalTime = DISABLE;
    uint32_t mailbox;
    return HAL_CAN_AddTxMessage(&hcan_, &h, const_cast<uint8_t*>(frame.data), &mailbox) == HAL_OK;
}

bool Stm32CanChannel::receive(CanFrame& frame) {
    if (!up_) return false;
    if (HAL_CAN_GetRxFifoFillLevel(&hcan_, CAN_RX_FIFO0) == 0) return false;
    CAN_RxHeaderTypeDef h{};
    if (HAL_CAN_GetRxMessage(&hcan_, CAN_RX_FIFO0, &h, frame.data) != HAL_OK) return false;
    frame.ext = (h.IDE == CAN_ID_EXT);
    frame.id  = frame.ext ? h.ExtId : h.StdId;
    frame.rtr = (h.RTR == CAN_RTR_REMOTE);
    // Classic CAN carries at most 8 bytes, but the DLC FIELD is 4 bits and a sender may put 9..15 in it.
    // Clamped here so nothing downstream can index past frame.data on the strength of a wire value.
    frame.dlc = static_cast<uint8_t>(h.DLC > 8u ? 8u : h.DLC);
    return true;
}

// The controller's own account of the wire, straight out of ESR. Cheap — one register read — so the
// CAN task can afford it every publish rather than only when something already looks wrong.
CanErrorStatus Stm32CanChannel::error_status() const {
    CanErrorStatus st;
    if (!up_) return st;
    const uint32_t esr = instance_->ESR;
    st.tec      = static_cast<uint8_t>((esr & CAN_ESR_TEC_Msk) >> CAN_ESR_TEC_Pos);
    st.rec      = static_cast<uint8_t>((esr & CAN_ESR_REC_Msk) >> CAN_ESR_REC_Pos);
    st.last_err = static_cast<uint8_t>((esr & CAN_ESR_LEC_Msk) >> CAN_ESR_LEC_Pos);
    const bool off = (esr & CAN_ESR_BOFF) != 0;
    st.state = off                        ? 3
             : (esr & CAN_ESR_EPVF) != 0  ? 2
             : (esr & CAN_ESR_EWGF) != 0  ? 1
                                          : 0;
    // Count the EDGE into bus-off. Reading the flag alone under AutoBusOff shows a bus that keeps
    // dropping and recovering as healthy whenever the sample lands between the two.
    if (off && !was_off_) bus_off_n_++;
    was_off_ = off;
    st.bus_off_n = bus_off_n_;
    return st;
}

bool Stm32CanChannel::is_up() const {
    if (!up_) return false;
    const HAL_CAN_StateTypeDef s = HAL_CAN_GetState(const_cast<CAN_HandleTypeDef*>(&hcan_));
    return s == HAL_CAN_STATE_READY || s == HAL_CAN_STATE_LISTENING;
}

// Re-init in (or out of) loopback at the SAME bitrate. HAL_CAN_Init cannot change Mode on a started
// peripheral, so this stops and fully re-initialises it — filters and all — which begin() already does.
bool Stm32CanChannel::set_loopback(bool on) {
    if (!up_) return false;
    if (loopback_ == on) return true;
    // Abort anything still pending FIRST. With no other node on the wire a frame never completes, and
    // AutoRetransmission keeps it in its mailbox forever; HAL_CAN_DeInit does not clear those requests.
    // Carry them across a re-init and all three mailboxes stay full, so every send afterwards fails with
    // "TX full" — which reads exactly like the new setting being broken. It poisoned a bitrate sweep:
    // only the FIRST rate probed was measuring the wire, the rest were measuring stuck frames.
    HAL_CAN_AbortTxRequest(&hcan_, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
    HAL_CAN_Stop(&hcan_);
    HAL_CAN_DeInit(&hcan_);
    up_ = false;
    return begin(bitrate_, on);
}

// Apply a settings set in one re-init. Idempotent: identical settings are a no-op, so the composition
// tier can call this on every config change without churning the bus.
bool Stm32CanChannel::apply(uint32_t bitrate_hz, bool listen_only) {
    if (bitrate_hz == 0) return false;
    if (up_ && bitrate_ == bitrate_hz && listen_only_ == listen_only) return true;
    if (up_) {
        HAL_CAN_AbortTxRequest(&hcan_, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
        HAL_CAN_Stop(&hcan_);
        HAL_CAN_DeInit(&hcan_);
        up_ = false;
    }
    return begin(bitrate_hz, loopback_, listen_only);
}

void Stm32CanChannel::shutdown() {
    if (!up_) return;
    HAL_CAN_AbortTxRequest(&hcan_, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
    HAL_CAN_Stop(&hcan_);
    HAL_CAN_DeInit(&hcan_);
    up_ = false;
}

bool Stm32CanChannel::set_bitrate(uint32_t bitrate_hz) {
    if (!up_ || bitrate_hz == 0) return false;
    if (bitrate_ == bitrate_hz) return true;
    // Abort anything still pending FIRST. With no other node on the wire a frame never completes, and
    // AutoRetransmission keeps it in its mailbox forever; HAL_CAN_DeInit does not clear those requests.
    // Carry them across a re-init and all three mailboxes stay full, so every send afterwards fails with
    // "TX full" — which reads exactly like the new setting being broken. It poisoned a bitrate sweep:
    // only the FIRST rate probed was measuring the wire, the rest were measuring stuck frames.
    HAL_CAN_AbortTxRequest(&hcan_, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
    HAL_CAN_Stop(&hcan_);
    HAL_CAN_DeInit(&hcan_);
    up_ = false;
    return begin(bitrate_hz, loopback_);
}

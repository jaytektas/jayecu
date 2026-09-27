#pragma once
#include "../II2cBus.h"
#include "stm32f7xx_hal.h"

// ---------------------------------------------------------------------------
// Stm32I2cBus — II2cBus over one STM32 I2C peripheral. Mechanism only: which
// peripheral, which pins and what timing word are the board's to say.
//
// BLOCKING, with a short timeout. Every call here may stall the caller for up to
// `timeout_ms`, so nothing on the engine frame may touch it — the environmental
// sensor is serviced from a background task for exactly that reason (it used to
// cost ~505 us inside a 1000 us frame that normally runs in 180).
// ---------------------------------------------------------------------------
class Stm32I2cBus final : public II2cBus {
public:
    struct Cfg {
        I2C_TypeDef*  i2c;          // I2C1 / I2C2 / I2C3
        GPIO_TypeDef* port;         // the port carrying SCL and SDA
        uint16_t      scl_pin;      // GPIO_PIN_x
        uint16_t      sda_pin;
        uint8_t       af;           // GPIO_AF4_I2Cx
        uint32_t      timing;       // TIMINGR word for the wanted rate at this APB1 clock
        uint32_t      timeout_ms;
    };

    void init(const Cfg& cfg) noexcept;

    [[nodiscard]] bool read_reg(uint8_t dev_addr, uint8_t reg,
                                uint8_t* buf, uint8_t len) noexcept override;
    [[nodiscard]] bool write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value) noexcept override;

private:
    I2C_HandleTypeDef h_{};
    uint32_t          timeout_ms_ = 10;
};

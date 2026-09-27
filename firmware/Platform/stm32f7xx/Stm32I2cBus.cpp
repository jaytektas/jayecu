#include "Stm32I2cBus.h"

void Stm32I2cBus::init(const Cfg& cfg) noexcept {
    timeout_ms_ = cfg.timeout_ms ? cfg.timeout_ms : 10u;

    if      (cfg.i2c == I2C1) { __HAL_RCC_I2C1_CLK_ENABLE(); }
    else if (cfg.i2c == I2C2) { __HAL_RCC_I2C2_CLK_ENABLE(); }
    else if (cfg.i2c == I2C3) { __HAL_RCC_I2C3_CLK_ENABLE(); }

    GPIO_InitTypeDef g = {};
    g.Mode      = GPIO_MODE_AF_OD;          // I2C is open-drain; the board carries the pull-ups
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = cfg.af;
    g.Pin       = static_cast<uint32_t>(cfg.scl_pin) | cfg.sda_pin;
    HAL_GPIO_Init(cfg.port, &g);

    h_.Instance              = cfg.i2c;
    h_.Init.Timing           = cfg.timing;
    h_.Init.OwnAddress1      = 0;
    h_.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    h_.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    h_.Init.OwnAddress2      = 0;
    h_.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h_.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    h_.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&h_);
}

bool Stm32I2cBus::read_reg(uint8_t dev_addr, uint8_t reg, uint8_t* buf, uint8_t len) noexcept {
    if (!buf || len == 0u) return false;
    return HAL_I2C_Mem_Read(&h_, dev_addr, reg, I2C_MEMADD_SIZE_8BIT,
                            buf, len, timeout_ms_) == HAL_OK;
}

bool Stm32I2cBus::write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value) noexcept {
    return HAL_I2C_Mem_Write(&h_, dev_addr, reg, I2C_MEMADD_SIZE_8BIT,
                             &value, 1, timeout_ms_) == HAL_OK;
}

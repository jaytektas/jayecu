# Jaytek V1 Hardware Reference

Source: the EasyEDA Pro schematic's netlist export of 2026-05-22 (not shipped; the schematic itself is in `PDF_JAYTEK_2026-04-29/`).
All U43 pin → GPIO mappings are exact; no estimates. Peripheral AF numbers are from the
STM32F767ZIT6 datasheet (DS11532).

---

## MCU

| Item | Value |
|------|-------|
| Part | STM32F767ZIT6 |
| Package | LQFP-144 |
| Core | Cortex-M7, 216 MHz target |
| Flash | 2 MB |
| SRAM | 512 KB + 16 KB TCM |
| Reference designator | U43 |
| Schematic silkscreen | Still reads F429ZIT6 — hardware is F767ZIT6 |

---

## Complete MCU Pin Map (U43)

Derived directly from netlist `U43.<pin>` entries.
GPIO name = LQFP-144 pin-number → GPIO mapping (STM32F767ZIT6 datasheet Table 11).

### Port A

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 34 | PA0 | AV1 | Analog | ADC1_IN0 |
| 35 | PA1 | AV2 | Analog | ADC1_IN1 |
| 36 | PA2 | AV3 | Analog | ADC1_IN2 |
| 37 | PA3 | AV4 | Analog | ADC1_IN3 |
| 40 | PA4 | AV5 | Analog | ADC1_IN4 |
| 41 | PA5 | AV6 | Analog | ADC1_IN5 |
| 42 | PA6 | AV7 | Analog | ADC1_IN6 |
| 43 | PA7 | AV8 | Analog | ADC1_IN7 |
| 100 | PA8 | RUNNING | GPIO_Output | Status LED (active LOW) |
| 101 | PA9 | UART_TX | Alternate | USART1_TX AF7 |
| 102 | PA10 | UART_RX | Alternate | USART1_RX AF7 |
| 103 | PA11 | USB_D− | Alternate | USB_OTG_FS_DM |
| 104 | PA12 | USB_D+ | Alternate | USB_OTG_FS_DP |
| 105 | PA13 | SWDIO | Alternate | SYS_JTMS-SWDIO AF0 |
| 109 | PA14 | SWCLK | Alternate | SYS_JTCK-SWCLK AF0 |
| 110 | PA15 | SD_CS | GPIO_Output | SPI3 chip-select (active LOW) |

### Port B

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 46 | PB0 | AV11 | Analog | ADC1_IN8 |
| 47 | PB1 | AV12 (12V_DIVIDED) | Analog | ADC1_IN9 |
| 48 | PB2 | BOOT1 | — | Boot mode / SW5 |
| 69 | PB10 | AUX_SPI_SCK | Alternate | SPI2_SCK AF5 |
| 70 | PB11 | AUX_SPI_CS | GPIO_Output | SPI2 chip-select (active LOW) |
| 73 | PB12 | CAN2_RX | Alternate | CAN2_RX AF9 |
| 74 | PB13 | CAN2_TX | Alternate | CAN2_TX AF9 |
| 75 | PB14 | AUX_SPI_MISO | Alternate | SPI2_MISO AF5 |
| 76 | PB15 | AUX_SPI_MOSI | Alternate | SPI2_MOSI AF5 |
| 133 | PB3 | SD_SCK | Alternate | SPI3_SCK AF6 (Conflict: JTDO) |
| 134 | PB4 | SD_MISO | Alternate | SPI3_MISO AF6 (Conflict: NJTRST) |
| 135 | PB5 | SD_MOSI | Alternate | SPI3_MOSI AF6 |
...
### JTAG / SPI3 Conflict Notice (PB3, PB4)
PB3 and PB4 are dedicated JTAG pins (JTDO and NJTRST) by default. On Jaytek V1, these pins are wired to the SD card SPI interface. 

**Resolution:**
To use the SD card, the software MUST explicitly switch PB3 and PB4 from their JTAG functions to Alternate Function mode (AF6/SPI3). 
- **Warning:** Using these pins for SPI will prevent JTAG debugging of these specific pins, but SWD (PA13/PA14) remains functional for full debugging and programming.
- **Bootloader Note:** The firmware must perform this switch early in the initialization sequence to ensure the SD card is accessible even while a debugger is attached.
| 136 | PB6 | BARO_SCL | Alternate | I2C1_SCL AF4 |
| 137 | PB7 | BARO_SDA | Alternate | I2C1_SDA AF4 |
| 139 | PB8 | 5V_SENSOR2_PG | GPIO_Input | Power-good monitor |
| 140 | PB9 | 5V_SENSOR1_PG | GPIO_Input | Power-good monitor |

### Port C

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 7 | PC13 | NC | — | Not connected in netlist |
| 8 | PC14 | OSC32_IN | — | RCC_OSC32_IN (X2 32.768 kHz) |
| 9 | PC15 | OSC32_OUT | — | RCC_OSC32_OUT |
| 26 | PC0 | AT1 | Analog | ADC1_IN10 |
| 27 | PC1 | AT2 | Analog | ADC1_IN11 |
| 28 | PC2 | AT3 | Analog | ADC1_IN12 |
| 29 | PC3 | AT4 | Analog | ADC1_IN13 |
| 44 | PC4 | AV9 | Analog | ADC1_IN14 |
| 45 | PC5 | AV10 | Analog | ADC1_IN15 |
| 96 | PC6 | HS8 | GPIO_Output | High-side driver 8 input |
| 97 | PC7 | WARNING | GPIO_Output | Status LED (active LOW) |
| 98 | PC8 | ERROR | GPIO_Output | Status LED (active LOW) |
| 99 | PC9 | COMMS | GPIO_Output | Status LED (active LOW) |
| 111 | PC10 | IGN8 | GPIO_Output | Ignition coil 8 |
| 112 | PC11 | IGN7 | GPIO_Output | Ignition coil 7 |
| 113 | PC12 | IGN6 | GPIO_Output | Ignition coil 6 |

### Port D

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 77 | PD8 | DIGITAL1 | GPIO_Input + EXTI | Cam / digital input 1 |
| 78 | PD9 | DIGITAL2 | GPIO_Input + EXTI | Cam / digital input 2 |
| 79 | PD10 | DIGITAL3 | GPIO_Input + EXTI | Cam / digital input 3 |
| 80 | PD11 | DIGITAL4 | GPIO_Input + EXTI | Cam / digital input 4 |
| 81 | PD12 | DIGITAL5 | GPIO_Input + EXTI | Cam / digital input 5 |
| 82 | PD13 | DIGITAL6 | GPIO_Input + EXTI | Cam / digital input 6 |
| 85 | PD14 | DIGITAL7 | GPIO_Input + EXTI | Cam / digital input 7 |
| 86 | PD15 | DIGITAL8 | GPIO_Input + EXTI | Cam / digital input 8 |
| 114 | PD0 | CAN1_RX | Alternate | CAN1_RX AF9 |
| 115 | PD1 | CAN1_TX | Alternate | CAN1_TX AF9 |
| 116 | PD2 | ETB2_DIS | GPIO_Output | ETB2 disable (active HIGH disables) |
| 117 | PD3 | ETB2_PWM | Alternate | TIM2_CH2 AF1 (PWM) |
| 118 | PD4 | ETB2_DIR | GPIO_Output | ETB2 direction |
| 119 | PD5 | ETB1_DIS | GPIO_Output | ETB1 disable |
| 122 | PD6 | ETB1_PWM | Alternate | TIM2_CH4 AF1 (PWM) |
| 123 | PD7 | ETB1_DIR | GPIO_Output | ETB1 direction |

> Note: ETB PWM pins PD3/PD6 share TIM2. If TIM2 is the free-running engine timebase,
> use a different timer for ETB PWM (e.g. TIM3, TIM4, TIM12) remapped to software GPIO
> toggle, or allocate TIM2 CH2/CH4 as output-compare with the same free-running counter.

### Port E

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 1 | PE2 | IGN1 | GPIO_Output | Ignition coil 1 |
| 2 | PE3 | IGN2 | GPIO_Output | Ignition coil 2 |
| 3 | PE4 | IGN3 | GPIO_Output | Ignition coil 3 |
| 4 | PE5 | IGN4 | GPIO_Output | Ignition coil 4 |
| 5 | PE6 | IGN5 | GPIO_Output | Ignition coil 5 (TIM9_CH2 AF3 available) |
| 58 | PE7 | LS8 | GPIO_Output | Injector / low-side 8 |
| 59 | PE8 | LS9 | GPIO_Output | Injector / low-side 9 |
| 60 | PE9 | LS10 | GPIO_Output | Injector / low-side 10 |
| 63 | PE10 | LS11 | GPIO_Output | Injector / low-side 11 |
| 64 | PE11 | LS12 | GPIO_Output | Injector / low-side 12 |
| 65 | PE12 | LS13 | GPIO_Output | Injector / low-side 13 |
| 66 | PE13 | LS14 | GPIO_Output | Injector / low-side 14 |
| 67 | PE14 | LS15 | GPIO_Output | Injector / low-side 15 |
| 68 | PE15 | LS16 | GPIO_Output | Injector / low-side 16 |
| 141 | PE0 | VR1 | GPIO_Input + EXTI0 | Crank trigger (MAX9924 output) |
| 142 | PE1 | VR2 | GPIO_Input + EXTI1 | Secondary VR trigger |

### Port F

| Pin | GPIO | Net / Signal | Mode | Peripheral / AF |
|-----|------|-------------|------|-----------------|
| 10 | PF0 | IGN9 | GPIO_Output | Ignition coil 9 |
| 11 | PF1 | IGN10 | GPIO_Output | Ignition coil 10 |
| 12 | PF2 | IGN11 | GPIO_Output | Ignition coil 11 |
| 13 | PF3 | IGN12 | GPIO_Output | Ignition coil 12 |
| 14 | PF4 | KNOCK1 | Analog | ADC3_IN14 |
| 15 | PF5 | KNOCK2 | Analog | ADC3_IN15 |
| 18 | PF6 | AV13 | Analog | ADC3_IN4 |
| 19 | PF7 | AV14 | Analog | ADC3_IN5 |
| 20 | PF8 | AV15 | Analog | ADC3_IN6 |
| 21 | PF9 | AV16 | Analog | ADC3_IN7 |
| 22 | PF10 | NC | — | Not connected in netlist |
| 49 | PF11 | LS1 | GPIO_Output | Injector / low-side 1 |
| 50 | PF12 | LS2 | GPIO_Output | Injector / low-side 2 |
| 53 | PF13 | LS3 | GPIO_Output | Injector / low-side 3 |
| 54 | PF14 | LS4 | GPIO_Output | Injector / low-side 4 |
| 55 | PF15 | LS5 | GPIO_Output | Injector / low-side 5 |

### Port G

| Pin | GPIO | Net / Signal | Mode | Notes |
|-----|------|-------------|------|-------|
| 56 | PG0 | LS6 | GPIO_Output | Injector / low-side 6 |
| 57 | PG1 | LS7 | GPIO_Output | Injector / low-side 7 |
| 87 | PG2 | HS1 | GPIO_Output | High-side driver 1 input |
| 88 | PG3 | HS2 | GPIO_Output | High-side driver 2 input |
| 89 | PG4 | HS3 | GPIO_Output | High-side driver 3 input |
| 90 | PG5 | HS4 | GPIO_Output | High-side driver 4 input |
| 91 | PG6 | HS5 | GPIO_Output | High-side driver 5 input |
| 92 | PG7 | HS6 | GPIO_Output | High-side driver 6 input |
| 93 | PG8 | HS7 | GPIO_Output | High-side driver 7 input |
| 124 | PG9 | NC | — | Not connected in netlist |
| 125 | PG10 | LS17 | GPIO_Output | Injector / low-side 17 |
| 126 | PG11 | LS18 | GPIO_Output | Injector / low-side 18 |
| 127 | PG12 | LS19 | GPIO_Output | Injector / low-side 19 |
| 128 | PG13 | LS20 | GPIO_Output | Injector / low-side 20 |
| 129 | PG14 | LS21 | GPIO_Output | Injector / low-side 21 |
| 132 | PG15 | LS22 | GPIO_Output | Injector / low-side 22 |

### Port H / Special

| Pin | GPIO | Net | Notes |
|-----|------|-----|-------|
| 23 | PH0 | OSC_IN | RCC_OSC_IN — 8 MHz crystal X1 (C105 20 pF) |
| 24 | PH1 | OSC_OUT | RCC_OSC_OUT — 8 MHz crystal X1 (C106 20 pF) |
| 6 | VBAT | VBAT | CR2032 coin cell B1 |
| 25 | NRST | ~NRST | Reset: SW4 + R131 10 kΩ to VDD |
| 138 | BOOT0 | BOOT0 | SW5 pushbutton + R132 10 kΩ to VDD; LOW = normal boot |
| 48 | PB2 | BOOT1 | SW5 shares connection; must be LOW for flash boot |
| 32 | VREF+ | VREF+ | REF3033AIDBZR → 3.000 V precision ADC reference |
| 33 | VDDA | VDDA | +3.3 V analog via L3 ferrite from VDD |
| 31 | VSSA | GND | Analog ground |
| 71 | VCAP_1 | — | Internal LDO decoupling — 1 µF cap C108 to GND |
| 106 | VCAP_2 | — | Internal LDO decoupling — 1 µF cap C107 to GND |
| 143 | PDR_ON | VDD | Pulled HIGH (enables power-down reset) |

---

## ADC Channel Summary

### ADC1 (shared PA/PB/PC pins)

| ADC1 channel | Pin | Signal |
|-------------|-----|--------|
| IN0 | PA0 | AV1 |
| IN1 | PA1 | AV2 |
| IN2 | PA2 | AV3 |
| IN3 | PA3 | AV4 |
| IN4 | PA4 | AV5 |
| IN5 | PA5 | AV6 |
| IN6 | PA6 | AV7 |
| IN7 | PA7 | AV8 |
| IN8 | PB0 | AV11 |
| IN9 | PB1 | AV12 (12V_DIVIDED) |
| IN10 | PC0 | AT1 |
| IN11 | PC1 | AT2 |
| IN12 | PC2 | AT3 |
| IN13 | PC3 | AT4 |
| IN14 | PC4 | AV9 |
| IN15 | PC5 | AV10 |

### ADC3 (exclusive PF pins)

| ADC3 channel | Pin | Signal |
|-------------|-----|--------|
| IN4 | PF6 | AV13 |
| IN5 | PF7 | AV14 |
| IN6 | PF8 | AV15 |
| IN7 | PF9 | AV16 |
| IN14 | PF4 | KNOCK1 |
| IN15 | PF5 | KNOCK2 |

**VREF+** = 3.000 V (REF3033AIDBZR). Set this as the ADC reference in software.

**12V_DIVIDED scale**: R143 (39 kΩ) / R144 (8.2 kΩ) → factor = 8.2/(39+8.2) ≈ 0.1737.
At 12 V input → ~2.084 V ADC reading. At VREF+ 3.0 V, full-scale → ~17.3 V input.

---

## Power Tree

```
CON_12V_RAW → D15 SMCJ30CA (TVS) → F1 → F2 → CON_12V_PROT
              → LMR14020SDDAR (buck, L2 2.2 µH) → +5V
                    → AMS1117-3.3 (U41) → VDD (3.3 V digital)
                    → TLS115D0LD ×2 (U37, U38) → CON_5V_SENSOR1/2 + PG_OUT
              → REF3033AIDBZR (U40) → VREF+ (3.000 V)
VDD → L3 ferrite → VDDA (3.3 V analog, decoupled)
CR2032 B1 → VBAT
```

---

## Peripheral Configuration Summary

| Peripheral | Instance | Pins | Notes |
|------------|----------|------|-------|
| RCC HSE | — | PH0, PH1 | 8 MHz → PLL → 216 MHz |
| RCC LSE | — | PC14, PC15 | 32.768 kHz → RTC |
| USART1 | USART1 | PA9 (TX), PA10 (RX) | External UART header H6 |
| CAN1 | bxCAN1 | PD0 (RX), PD1 (TX) | TJA1051T/3/1J; 120 Ω term |
| CAN2 | bxCAN2 | PB12 (RX), PB13 (TX) | TJA1051T/3/1J; 120 Ω term |
| I2C1 | I2C1 | PB6 (SCL), PB7 (SDA) | LPS22HBTR baro; 1 kΩ pullups |
| SPI3 | SPI3 | PB3 (SCK), PB4 (MISO), PB5 (MOSI) | SD card; PA15 = CS |
| SPI2 | SPI2 | PB10 (SCK), PB14 (MISO), PB15 (MOSI) | AUX SPI header H5; PB11 = CS |
| USB OTG FS | USB_OTG_FS | PA11 (DM), PA12 (DP) | Device mode; VBUS from +5V |
| SWD | — | PA13 (SWDIO), PA14 (SWCLK) | Debug header H3 |
| ADC1 | ADC1 | PA0–PA7, PB0–PB1, PC0–PC5 | AV1–AV12, AT1–AT4 |
| ADC3 | ADC3 | PF4–PF9 | KNOCK1/2, AV13–AV16 |
| TIM2 | TIM2 | (no output pin needed) | 32-bit free-running engine timebase |
| TIM2 CH2/CH4 | TIM2 | PD3, PD6 | ETB1/ETB2 PWM (share TIM2 — see note below) |
| GPIO_Output | — | PE2–PE6, PF0–PF3, PC10–PC12 | IGN1–12 (EventScheduler GPIO toggle) |
| GPIO_Output | — | PF11–PF15, PG0–PG1, PE7–PE15, PG10–PG15 | LS1–22 |
| GPIO_Output | — | PG2–PG8, PC6 | HS1–8 |
| GPIO_Output | — | PA8, PC7–PC9 | Status LEDs (RUNNING, WARNING, ERROR, COMMS) |
| GPIO_Input | — | PE0, PE1 | VR1, VR2 crank/secondary triggers (EXTI) |
| GPIO_Input | — | PD8–PD15 | DIGITAL1–8 cam inputs (EXTI) |
| GPIO_Input | — | PB8, PB9 | 5V_SENSOR2_PG, 5V_SENSOR1_PG |

**ETB PWM / TIM2 conflict note**: PD3 = TIM2_CH2 and PD6 = TIM2_CH4. If TIM2 is used
as the free-running engine timebase counter (no prescaler, up mode), channels 2 and 4 can
still issue output-compare events — the counter keeps running. Configure OC2M/OC4M in
PWM1 mode. The ETB PWM frequency (~10–20 kHz) must be achievable via the ARR/CCR
registers against the 216 MHz (or chosen) clock without conflicting with engine event
scheduling. Alternatively, assign ETB PWM to TIM3 or TIM12 and drive PD3/PD6 as plain
GPIO toggle from an ISR.

---

## Signal Polarity & Driver Notes

| Subsystem | Driver IC | Signal polarity |
|-----------|-----------|-----------------|
| IGN1–12 | IX4427NTR (non-inverting dual gate driver) | HIGH = coil dwell; LOW = coil fires (spark) |
| LS1–22 | VNLD5090TR-E (dual low-side MOSFET) | HIGH = switch ON → injector/load opens |
| HS1–8 | VNQ7140AJTR (quad high-side switch) | HIGH = switch ON → load energized |
| ETB DIS | IFX9201SGAUMA1 (H-bridge) | DIS LOW = bridge enabled (run); HIGH = disabled |
| ETB DIR | IFX9201SGAUMA1 | DIR selects rotation direction |
| Status LEDs | — | Drive LOW to illuminate (VDD → 1 kΩ → LED → GPIO) |
| VR1/VR2 | MAX9924UAUB+ | Rising edge = VR zero-crossing (tooth/gap transition) |
| DIGITAL1–8 | 74HC2G17GW Schmitt | 3.3 V-compatible; rising/falling EXTI as configured |
| 5V_SENSORx_PG | TLS115D0LD open-drain | HIGH (pulled to VDD) = regulator OK |

---

## Connector Map

**CN2 (776231-1 white, 35-pin)** — ignitions + low-sides:
Pins 24–35: IGN1–IGN12; Pins 1–22 (interleaved): LS1–LS22; Pin 23: GND.

**CN3 (776231-1 blue, 35-pin)** — sensors, analog, VR, digital:
Pin 1: CON_AT4, 2: CON_AT2, 3: CON_AV16, 4: CON_AV14, 5: CON_AV13,
6: CON_VR1+, 7: CON_VR1−, 8: CON_VR2+, 9: CON_VR2−,
10–12: GND, 13: CON_AT3, 14: CON_AT1, 15: CON_AV15,
16: CON_DIG8, 17: CON_DIG7, 18: CON_DIG6, 19: CON_DIG5,
20: CON_DIG4, 21: CON_DIG3, 22: CON_DIG2, 23: CON_DIG1,
24: CON_AV4, 25: CON_AV3, 26: CON_AV2, 27: CON_AV1,
28: CON_AV11, 29: CON_AV10, 30: CON_AV9, 31: CON_AV8,
32: CON_AV7, 33: CON_AV6, 34: CON_AV5, 35: GND.

**CN4 (776228-1 black, 23-pin)** — power, HS, ETB, knock, CAN1:
1: CON_HS5, 2: CON_HS6, 3: CON_HS8, 4: CON_HS7, 5: CON_HS3, 6: CON_HS4,
7: CON_HS1, 8: CON_HS2, 9: CON_12V_RAW, 10: CON_CAN1_H,
11–12: GND, 13: CON_12V_MR, 14: CON_5V_SENSOR1, 15: CON_5V_SENSOR2,
16: CON_12V_PROT, 17: CON_CAN1_L, 18: CON_ETB2−, 19: CON_ETB2+,
20: CON_ETB1+, 21: CON_ETB1−, 22: CON_KNOCK2, 23: CON_KNOCK1.

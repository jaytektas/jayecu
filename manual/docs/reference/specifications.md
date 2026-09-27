# Specifications

The jaytek_v1 board and the firmware's limits, in one place. Each line points to the chapter that
explains it.

## Hardware

| | |
|---|---|
| Processor | STM32F767ZIT6, ARM Cortex-M7, 216 MHz (chapter 7) |
| Analogue reference | 12-bit converters against a 3.0 V precision reference |
| Real-time clock | 32.768 kHz crystal, kept running by a CR2032 coin cell |
| Harness connectors | CN2 (white, 35 ways), CN3 (blue, 35 ways), CN4 (black, 23 ways) |
| Analogue inputs | 15, 0–5 V (AV1–AV11, AV13–AV16); AV12 reads the ECU's supply |
| Temperature inputs | 4 (AT1–AT4), 2.7 kΩ pull-up to 5 V |
| Trigger / frequency inputs | 2 VR (MAX9924, also take Hall sensors), 8 digital (DIG1–DIG8) |
| Knock inputs | 2, with a 3.5 mm listening jack (CN1) |
| Ignition outputs | 12, 5 V logic (IGN1–IGN12) |
| Low-side outputs | 22 (LS1–LS22), VNLD5090 |
| High-side outputs | 8 (HS1–HS8), VNQ7140 |
| H-bridges | 2 (IFX9201), for electronic throttle, idle valves, steppers |
| CAN | 2 buses (CAN1 on CN4, CAN2 on header H4), TJA1051, 120 Ω fitted; 125 kbit–1 Mbit |
| USB | USB-B; the studio link, and the SD card as a drive at key-off |
| Storage | microSD card, FAT32 (not exFAT) |
| Barometer | on board (LPS22HB) |
| Sensor supplies | two 5 V supplies, each monitored; a protected 12 V output |
| Key on / off | from battery voltage: on above 8 V, off below 7 V (chapter 10) |
| Recovery | built-in USB bootloader: SW5 BOOT + SW4 RESET (chapter 47) |

The board definition gives no continuous current rating for the outputs: use each driver's data sheet
(chapter 12).

## Firmware limits

| | |
|---|---|
| Cylinders | up to 12 |
| Trigger wheel library | every shipped wheel is listed in [Trigger wheels](trigger-wheels.md) |
| Sensors | every catalogued input, each on any suitable pin or CAN (chapter 17) |
| PWM outputs at once | 16 (chapter 12) |
| Generic tables | 8, up to 16 × 16 (chapter 34) |
| Expression program | 48 or 64 bytes per setting (chapter 34) |
| Lua script | 4095 characters (chapter 35) |
| CAN frames / fields | 96 frames, 512 fields, shared by both buses (chapter 33) |
| Trouble code table | 64 codes (chapter 44) |
| Trigger Log | 4096 edges per capture (chapter 16) |
| Onboard log rate | 1–1000 Hz (chapter 42) |
| Output channels | every one is listed in [Output channels](channels.md) |
| Stored tune | two flash banks plus the SD card (chapter 36) |

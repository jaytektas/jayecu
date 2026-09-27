# Third-party components

Everything JayECU compiles or links, and the licence it comes under. Several of
these ask only that their notice travels with the code; this file is where that
notice lives, so it is kept accurate rather than complete-looking.

None of these obliges JayECU to publish its source or restricts selling it. The
GPL that does ask for source is JayECU's own choice, not an inherited condition —
see LICENSE.

## Firmware (runs on the ECU)

| Component | Licence | Note |
|---|---|---|
| STM32 USB Device Library (`usbd_core.c`, `usbd_ctlreq.c`, `usbd_ioreq.c`, `usbd_cdc.c`, composite builder) | **SLA0044** (STMicroelectronics) | Proprietary. Use permitted only on ST devices. See LICENSE.exception — the GPL needs an explicit permission to be combined with this. ST ships no copy of the licence text with the library; it is named only in `STM32CubeF7/LICENSE.md`. |
| STM32F7 HAL, BSP | BSD-3-Clause | STMicroelectronics |
| CMSIS, CMSIS Device | Apache-2.0 | ARM Limited / STMicroelectronics |
| FreeRTOS kernel | MIT | Amazon.com, Inc. |
| FatFs (`ff.c`, `ffunicode.c`, `ffsystem.c`) | BSD-1-Clause style | Copyright (C) 2021, ChaN |
| USB device glue (`usbd_conf.c`, `usbd_conf.h`, `usbd_desc.c` in `firmware/Platform/stm32f7xx/`) | **SLA0044** (STMicroelectronics) | Adapted from STM32CubeMX's generated templates; ST's notice is kept in each file. |
| SD card SPI protocol (`firmware/Platform/stm32f7xx/SdCardSpi.c`) | FatFs sample licence (free use, notice retained) | Adapted from ChaN's FatFs "MMCv3/SDv1/SDv2 (in SPI mode) control module", Copyright (C) 2013, ChaN |
| Lua 5.4 | MIT | Lua.org, PUC-Rio |

STM32CubeF7 is fetched at build time (v1.17.1) rather than vendored; its own
`LICENSE.md` carries the per-component table this row set is drawn from.

## Studio (`apps/studio-jf`)

| Component | Licence | Note |
|---|---|---|
| JFramework | GPL-3.0 | Same author. The studio links it, so the studio is GPL-3 too. |
| `stb_image.h` | Public domain (or MIT) | Sean Barrett. Also supplies the zlib decoder. |
| `stb_truetype.h` (via JFramework) | Public domain (or MIT) | Sean Barrett |
| Vulkan loader | Apache-2.0 | dynamically linked |
| OpenSSL (libssl, libcrypto) | Apache-2.0 | dynamically linked |
| SQLite3 | Public domain | dynamically linked |
| libxcb, libxcb-keysyms, libxcb-sync | MIT/X11 | dynamically linked |
| libdbus | GPL-2+ **or** AFL-2.1 | Dual-licensed. Combined with the GPL-3 studio, the GPL-2-or-later branch applies; AFL-2.1 is not GPL-compatible, so the choice matters. |
| libatspi | LGPL-2+ | dynamically linked, which is what the LGPL asks for |
| Ubuntu Sans font (`UbuntuSans.ttf`) | Ubuntu Font Licence 1.0 | Canonical Ltd. / Dalton Maag. Shipped unmodified beside the studio so every system draws the face the pages were laid out in; licence text in `apps/studio-jf/assets/UbuntuSans-LICENCE.txt`, installed beside it. |
| libwdi `wdi-simple.exe` (Windows only) | LGPL-3.0-or-later | A separate program beside `studio.exe` that installs the WinUSB driver for the ECU's bootloader. Source in `apps/studio-jf/third_party/libwdi-win/`. It embeds Microsoft's redistributable WinUSB co-installers. |

## Hardware

The Jaytek ECU board design is not covered by any of the above and is not
licensed for reuse. A published schematic is published for understanding and
repair; it grants no right to manufacture.

`definition/boards/proteus_f7.board.yaml` describes third-party hardware so that
JayECU can run on it. It contains pin assignments — facts about a board — in
JayECU's own schema, checked against the manufacturer's schematic. No files from
that project are included here.

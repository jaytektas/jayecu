/*
 * Adapted from STM32CubeMX's generated usbd_conf.h.
 * Portions Copyright (c) STMicroelectronics. All rights reserved.
 * This software component is licensed by ST under Ultimate Liberty license SLA0044, the "License";
 * you may not use this file except in compliance with the License. You may obtain a copy of the
 * License at: www.st.com/SLA0044. See LICENSE.exception for how it combines with the GPL.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f7xx.h"
#include "stm32f7xx_hal.h"
#include "usbd_def.h"

// USBD_MAX_NUM_INTERFACES: CDC (2: control + data) + MSC (1) = 3.
// usbd_def.h (included above) defaults this to 1U; undef before overriding
// so we deliberately replace it rather than warning on a redefine.
#undef  USBD_MAX_NUM_INTERFACES
#define USBD_MAX_NUM_INTERFACES     3U

// USBD_COMPOSITE_USE_IAD: EMIT THE INTERFACE ASSOCIATION DESCRIPTOR. Without this the composite
// builder's IAD blocks are compiled out (they are `#if USBD_COMPOSITE_USE_IAD == 1`, and an
// undefined macro is 0), so the configuration descriptor contained none — while usbd_desc.c's
// DEVICE descriptor declares 0xEF/0x02/0x01, "I am an IAD composite". The two contradicted each
// other, and the IAD is the only thing that says the CDC control and data interfaces are ONE
// function.
//
// Linux never minded: cdc_acm pairs the two interfaces from the CDC union functional descriptor.
// Windows does mind. usbccgp split the device into one child per interface, so usbser.sys bound
// the bare control interface and could not start without its data interface (Code 10,
// FAILED_START), and the orphaned data interface matched no driver at all (Code 28,
// FAILED_INSTALL) — a "JayECU Virtual COM Port" sitting in Other devices with no COM number.
//
// MSC is unaffected: its descriptor builder emits no IAD either way, which is right for a
// single-interface function.
#define USBD_COMPOSITE_USE_IAD      1
// USBD_MAX_NUM_CONFIGURATION: Maximum number of supported configurations
#define USBD_MAX_NUM_CONFIGURATION  1U
// USBD_MAX_STR_DESC_SIZ: Maximum string descriptor size
#define USBD_MAX_STR_DESC_SIZ       512U
// USBD_SELF_POWERED: Self powered device
#define USBD_SELF_POWERED           1U

// MSC_MEDIA_PACKET: SD block size the MSC BOT moves per transfer. The default of
// 512 forces one-sector-per-USB-microframe (CMD17) and crawls (~150 KB/s). 4096
// lets the BOT issue 8-sector multi-block (CMD18) reads → far fewer round-trips.
// Defined here so it reaches every MSC TU (usbd_def.h includes this file before
// usbd_msc.h's #ifndef default), keeping bot_data[] consistent everywhere.
// Grows the MSC handle by ~3.5 KB → USBD_POOL_SIZE (usbd_conf.c) is sized to fit.
// Safe: MSC runs only key-off (engine stopped) and SD waits are DWT-bounded, so
// the larger per-call ISR hold can't disturb engine timing.
#define MSC_MEDIA_PACKET            4096U

// Memory management — static allocator. USBD_malloc is called from USB ISR
// context during enumeration; newlib malloc is not ISR-safe and would fault.
#ifdef __cplusplus
extern "C" {
#endif
void* USBD_static_malloc(uint32_t size);
void  USBD_static_free(void* p);
#ifdef __cplusplus
}
#endif
#define USBD_malloc(size)           USBD_static_malloc(size)
#define USBD_free(p)                USBD_static_free(p)
#define USBD_memset                 memset
#define USBD_memcpy                 memcpy

// Debugging macros
#define USBD_UsrLog(...)            printf(__VA_ARGS__); printf("\n")
#define USBD_ErrLog(...)            fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")
#define USBD_DbgLog(...)

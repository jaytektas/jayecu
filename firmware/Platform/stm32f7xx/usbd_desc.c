/*
 * Adapted from STM32CubeMX's generated usbd_desc.c.
 * Portions Copyright (c) STMicroelectronics. All rights reserved.
 * This software component is licensed by ST under Ultimate Liberty license SLA0044, the "License";
 * you may not use this file except in compliance with the License. You may obtain a copy of the
 * License at: www.st.com/SLA0044. See LICENSE.exception for how it combines with the GPL.
 */
#include <stdio.h>
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_conf.h"
#include "../../version.h"

static void hex8(char* out, uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) { out[i] = digits[v & 0xF]; v >>= 4; }
}

#define USBD_VID                      0x0483
#define USBD_PID                      0x5740
#define USBD_LANGID_STRING            0x409
#define USBD_MANUFACTURER_STRING      (uint8_t*)"JayECU"
#define USBD_PRODUCT_FS_STRING        (uint8_t*)"JayECU Virtual COM Port"
#define USBD_CONFIGURATION_FS_STRING  (uint8_t*)"VCP Config"
#define USBD_INTERFACE_FS_STRING      (uint8_t*)"VCP Interface"

__ALIGN_BEGIN uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

uint8_t *USBD_VCP_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_VCP_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef VCP_Desc = {
    USBD_VCP_DeviceDescriptor,
    USBD_VCP_LangIDStrDescriptor,
    USBD_VCP_ManufacturerStrDescriptor,
    USBD_VCP_ProductStrDescriptor,
    USBD_VCP_SerialStrDescriptor,
    USBD_VCP_ConfigStrDescriptor,
    USBD_VCP_InterfaceStrDescriptor,
};

// Device Descriptor
__ALIGN_BEGIN uint8_t USBD_DeviceDesc[USB_LEN_DEV_DESC] __ALIGN_END = {
    0x12,                       /* bLength */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType */
    0x00,                       /* bcdUSB */
    0x02,
    0xEF,                       /* bDeviceClass: Miscellaneous (IAD composite) */
    0x02,                       /* bDeviceSubClass: Common Class */
    0x01,                       /* bDeviceProtocol: Interface Association Descriptor */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize */
    LOBYTE(USBD_VID),           /* idVendor */
    HIBYTE(USBD_VID),           /* idVendor */
    LOBYTE(USBD_PID),           /* idProduct */
    HIBYTE(USBD_PID),           /* idProduct */
    0x00,                       /* bcdDevice rel. 2.00 */
    0x02,
    USBD_IDX_MFC_STR,           /* Index of manufacturer string */
    USBD_IDX_PRODUCT_STR,       /* Index of product string */
    USBD_IDX_SERIAL_STR,        /* Index of serial number string */
    USBD_MAX_NUM_CONFIGURATION  /* bNumConfigurations */
};

uint8_t *USBD_VCP_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    *length = sizeof(USBD_DeviceDesc);
    return USBD_DeviceDesc;
}

// Language ID Descriptor
__ALIGN_BEGIN uint8_t USBD_LangIDDesc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING),
    HIBYTE(USBD_LANGID_STRING),
};

uint8_t *USBD_VCP_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    *length = sizeof(USBD_LangIDDesc);
    return USBD_LangIDDesc;
}

// Product String Descriptor
uint8_t *USBD_VCP_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString(USBD_PRODUCT_FS_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

// Manufacturer String Descriptor
uint8_t *USBD_VCP_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString(USBD_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

// Serial String Descriptor.
// Format: "JE-<chip-uid-low64>" e.g. "JE-A1B2C3D4E5F60718" — board prefix, then a 16-hex-char fold
// of the chip's 96-bit UID.
//
// THIS IDENTIFIES THE BOARD, NOT THE BUILD, and the distinction is not cosmetic. iSerial is what an
// operating system uses as the device's identity. It used to carry JAYECU_GIT_HASH "for firmware
// traceability", which meant the serial changed with EVERY BUILD — so every flash presented what the
// host could only read as a brand-new device it had never seen.
//
// On Linux that was merely untidy: a different /dev/serial/by-id/ symlink each time, which is the
// opposite of the stability the old comment claimed for it. On Windows it is severe, because the
// serial is the device instance id: a new one means a fresh driver install, a NEWLY ALLOCATED COM
// number, and the previous serial left behind as a phantom device forever. After a flash the port
// had moved and the studio could not open it for minutes, and every flash before that had left
// another phantom COM port behind.
//
// Firmware version still travels in the identity reply ("jayecu <board> <ver> <build> ..."), which
// is where traceability belongs: it is queryable, it is what the studio already matches on, and it
// costs the host nothing.
uint8_t *USBD_VCP_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    const uint32_t uid0 = *(uint32_t*)DEVICE_ID1;
    const uint32_t uid1 = *(uint32_t*)DEVICE_ID2;
    const uint32_t uid2 = *(uint32_t*)DEVICE_ID3;
    // Fold three 32-bit words into two by XOR so the serial fits as 16 hex chars.
    const uint32_t fold_a = uid0 ^ uid2;
    const uint32_t fold_b = uid1;

    static char serial[3 + 16 + 1];  // "JE-" + 16 hex + NUL
    char* p = serial;
    *p++ = 'J'; *p++ = 'E'; *p++ = '-';
    hex8(p, fold_a); p += 8;
    hex8(p, fold_b); p += 8;
    *p = '\0';

    USBD_GetString((uint8_t*)serial, USBD_StrDesc, length);
    return USBD_StrDesc;
}

uint8_t *USBD_VCP_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString(USBD_CONFIGURATION_FS_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

uint8_t *USBD_VCP_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length) {
    (void)speed;
    USBD_GetString(USBD_INTERFACE_FS_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

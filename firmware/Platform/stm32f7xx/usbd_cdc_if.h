#pragma once

#include "usbd_cdc.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

USBD_StatusTypeDef USBD_Interface_Init_FS(USBD_HandleTypeDef *pdev);
void    UsbCdc_PreInit(void);  // call BEFORE USBD_Init to create RX buffer
void    UsbCdc_StartTxTask(void);  // call AFTER scheduler-ready setup to start TX task
size_t  UsbCdc_Read(uint8_t* buffer, size_t max_length);   // non-blocking: return what's queued now
// Block up to timeout_ms for the FIRST bytes (woken immediately by the RX ISR), then return them.
// Lets the comms task sleep until data arrives instead of polling. 0 = non-blocking.
size_t  UsbCdc_ReadBlocking(uint8_t* buffer, size_t max_length, uint32_t timeout_ms);
bool    UsbCdc_Send(const uint8_t* data, size_t length);
bool    UsbCdc_IsConnected(void);
// USB bus-idle suspend / resume hooks (called from the PCD callbacks). On this board VBUS sensing
// is off, so a cable pull never raises a USB reset — but it does stop SOFs, which the core reports
// as SUSPEND. Gating "connected" on this is how a cable pull is detected.
void    UsbCdc_OnSuspend(void);
void    UsbCdc_OnResume(void);

#ifdef __cplusplus
}
#endif

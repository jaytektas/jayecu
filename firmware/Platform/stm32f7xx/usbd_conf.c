/*
 * Adapted from STM32CubeMX's generated usbd_conf.c.
 * Portions Copyright (c) STMicroelectronics. All rights reserved.
 * This software component is licensed by ST under Ultimate Liberty license SLA0044, the "License";
 * you may not use this file except in compliance with the License. You may obtain a copy of the
 * License at: www.st.com/SLA0044. See LICENSE.exception for how it combines with the GPL.
 */
#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"   // UsbCdc_OnSuspend / UsbCdc_OnResume
#include "usb_msc.h"
#include "FreeRTOS.h"
#include "task.h"

PCD_HandleTypeDef hpcd_USB_OTG_FS;

// ---------------------------------------------------------------------------
// USB servicing model. EVERYTHING is serviced
// straight from the OTG ISR via the HAL_PCD_* callbacks — EP0 control + CDC bulk
// run inline (stock CubeMX), carrying no SD I/O. The ONLY special case is the MSC
// bulk EP (EP3): its DataIn/DataOut just wake the MSC BOT thread (usb_msc.cpp),
// which does the slow SD I/O in thread context. So the ISR is NEVER masked from a
// task, and CDC stays live during MSC streaming.
//
// USBD_Start (device connect) is performed once by the MSC thread after the
// scheduler is running; doing it pre-scheduler left descriptor reads unanswered.
// ---------------------------------------------------------------------------
#define MSC_BULK_EPNUM 3u   // MSC owns bulk EP3 (endpoint map in usb_device.cpp)

// ---------------------------------------------------------------------------
// Static allocator for USB device class handles.
// Sized for ~2× USBD_CDC_HandleTypeDef (well under 1 KB). Single-slot
// free-list: USBD_CDC_DeInit calls free() once, then USBD_CDC_Init calls
// malloc() again on the next reset — we reuse the same slot.
// ---------------------------------------------------------------------------

// Sized for the CDC + MSC composite: the MSC BOT handle embeds a MSC_MEDIA_PACKET
// media buffer (now 4096 B, see usbd_conf.h) plus its other fields, and CDC's
// handle is small. 8 KiB covers both live handles with comfortable headroom.
#define USBD_POOL_SIZE 8192U
static uint32_t s_usbd_pool[USBD_POOL_SIZE / sizeof(uint32_t)];
static uint32_t s_usbd_pool_used  = 0U;
static uint32_t s_usbd_pool_count = 0U;   // outstanding allocations

void* USBD_static_malloc(uint32_t size) {
    // Round up to 4-byte alignment
    const uint32_t aligned = (size + 3U) & ~3U;
    if (s_usbd_pool_used + aligned > sizeof(s_usbd_pool)) {
        return 0;   // pool exhausted
    }
    void* p = (uint8_t*)s_usbd_pool + s_usbd_pool_used;
    s_usbd_pool_used += aligned;
    s_usbd_pool_count++;
    return p;
}

void USBD_static_free(void* /*p*/) {
    // Bump allocator with reference counting: a composite device holds several
    // live class handles (CDC + MSC) simultaneously, so the pool must only be
    // rewound once EVERY block has been freed (e.g. on re-enumeration). The
    // old "reset on any free" logic corrupted the surviving class handle.
    if (s_usbd_pool_count > 0U) {
        s_usbd_pool_count--;
    }
    if (s_usbd_pool_count == 0U) {
        s_usbd_pool_used = 0U;
    }
}

// ---------------------------------------------------------------------------
// PCD (USB Peripheral) MSP Init/DeInit
// ---------------------------------------------------------------------------

void HAL_PCD_MspInit(PCD_HandleTypeDef* pcdHandle) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if (pcdHandle->Instance == USB_OTG_FS) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        
        // USB_OTG_FS GPIO Configuration: PA11 -> DM, PA12 -> DP
        GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF10_OTG_FS;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        // Peripheral clock enable
        __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

        // Peripheral interrupt init.
        // Priority 9 keeps OTG_FS comfortably below configMAX_SYSCALL_INTERRUPT_PRIORITY
        // (=5) so the FromISR calls in CDC_Receive_FS are safe under FreeRTOS, and
        // matches the CubeMX-generated default. Priority 5 sits *at* the boundary
        // and can preempt the kernel mid-critical-section.
        HAL_NVIC_SetPriority(OTG_FS_IRQn, 9, 0);
        HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcdHandle) {
    if (pcdHandle->Instance == USB_OTG_FS) {
        __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);
        HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    }
}

// ---------------------------------------------------------------------------
// Interrupt Handlers
// ---------------------------------------------------------------------------

void OTG_FS_IRQHandler(void) {
    HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
}

// ---------------------------------------------------------------------------
// USBD Low Level Driver interface (called by the USB library)
// ---------------------------------------------------------------------------

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev) {
    hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
    // OTG_FS has exactly 6 endpoints (EP0..EP5). Setting this to 9 caused
    // HAL IRQ loops to iterate past the real endpoint registers — touching
    // OUT_ep[6..8]/IN_ep[6..8] fields each interrupt. That worked most of
    // the time, but after a multi-packet TX (the 112-byte 'a' compat reply)
    // the trash overlapping registers left TxState wedged and silently
    // dropped every subsequent send.
    hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
    hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;  // OTG_FS is Full-Speed only
    hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
    hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
    hpcd_USB_OTG_FS.Init.ep0_mps = 0x40; // 64 bytes
    hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.battery_charging_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.use_external_vbus = DISABLE;
    hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;

    // Cross-link the two handles BEFORE HAL_PCD_Init: HAL PCD callbacks fire
    // via the interrupt enabled inside HAL_PCD_MspInit, and they look up the
    // USBD_HandleTypeDef via hpcd->pData. If we set this after, an early IRQ
    // would dereference NULL.
    hpcd_USB_OTG_FS.pData = pdev;
    pdev->pData = &hpcd_USB_OTG_FS;

    if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK) {
        return USBD_FAIL;
    }

    // OTG_FS has only 1.25 KiB (320 words) of dedicated FIFO SRAM. Sizes in
    // 32-bit words. CDC+MSC composite layout — RX FIFO is shared by both OUT
    // endpoints (CDC 0x01 + MSC 0x03):
    //   Rx 0x50 + EP0 0x20 + EP1 0x40 + EP2 0x20 + EP3 0x40 = 0x110 (272) <= 320.
    HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_FS, 0x50);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 0, 0x20); // EP0
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 1, 0x40); // EP1 CDC Data IN
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 2, 0x20); // EP2 CDC Cmd IN
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 3, 0x40); // EP3 MSC Data IN

    // No USB processing task: EP0 + CDC are serviced inline in the ISR, and the MSC
    // bulk EP is handled by the MSC BOT thread (created via UsbMsc_StartTask()).
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev) {
    HAL_PCD_DeInit((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev) {
    HAL_PCD_Start((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev) {
    HAL_PCD_Stop((PCD_HandleTypeDef *)pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps) {
    HAL_PCD_EP_Open((PCD_HandleTypeDef *)pdev->pData, ep_addr, ep_mps, ep_type);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_Close((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_Flush((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_SetStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    HAL_PCD_EP_ClrStall((PCD_HandleTypeDef *)pdev->pData, ep_addr);
    return USBD_OK;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
    if ((ep_addr & 0x80) == 0x80) {
        return hpcd->IN_ep[ep_addr & 0x7F].is_stall;
    } else {
        return hpcd->OUT_ep[ep_addr & 0x7F].is_stall;
    }
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr) {
    HAL_PCD_SetAddress((PCD_HandleTypeDef *)pdev->pData, dev_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size) {
    HAL_PCD_EP_Transmit((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size) {
    HAL_PCD_EP_Receive((PCD_HandleTypeDef *)pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr) {
    return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef *)pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay) {
    HAL_Delay(Delay);
}

// PCD Callbacks. EP0/control + device events stay INLINE in the ISR — they're
// fast, carry no SD I/O, and the EP0 control state machine is timing-coupled
// (deferring it desyncs request/response, e.g. garbled string descriptors).
// Only the BULK endpoints (MSC EP3, CDC EP1) defer to usb_task(), since that's
// where the slow MSC SD I/O runs — moving it off the ISR is the whole point.
// The OTG ISR has already drained RX-FIFO data into xfer_buff before DataOut
// fires, so capturing the buffer pointer here is safe.
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum) {
    if (epnum == MSC_BULK_EPNUM) {   // MSC bulk OUT — wake the BOT thread (no USBD dispatch)
        UsbMsc_OnEpOut(epnum, HAL_PCD_EP_GetRxCount(hpcd, epnum));
        return;
    }
    // EP0 control + CDC bulk — inline (no SD I/O); keeps the VCP serviced by the ISR.
    USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum) {
    if (epnum == MSC_BULK_EPNUM) {   // MSC bulk IN complete — wake the BOT thread
        UsbMsc_OnEpIn(epnum);
        return;
    }
    // EP0 control + CDC bulk — inline.
    USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd) {
    // Speed must be set to FULL before Reset (OTG_FS is FS-only; otherwise the
    // stack emits illegal HS-sized 512 B bulk descriptors and the host rejects
    // SET_CONFIGURATION). Inline — reset is rare, fast, and must be immediate.
    USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
    USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
    UsbCdc_OnSuspend();   // bus idle = cable pull / host sleep -> drop the "connected" view
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
    UsbCdc_OnResume();
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum) {
    USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);  // no iso EPs — never fires
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum) {
    USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd) {
    USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

#pragma once
// ---------------------------------------------------------------------------
// Thread-based USB Mass Storage (BOT/SCSI) for the CDC+MSC composite device.
//
// Replaces ST's USBD MSC class (usbd_msc/usbd_msc_bot/usbd_msc_scsi/usbd_msc_data).
// Those ran the BOT state machine *inside* USBD_LL_DataIn/DataOut, which we deferred
// to a single usb_task that masked OTG_FS_IRQn across each stage — starving the CDC
// VCP whenever the host streamed the card. Here MSC instead
// is a blocking THREAD (msc_task) that drives bulk EP3 via a per-EP blocking transfer
// primitive built on HAL PCD, so the OTG ISR is NEVER masked and keeps servicing CDC
// inline throughout.
// ---------------------------------------------------------------------------
#include "usbd_def.h"

#ifdef __cplusplus
extern "C" {
#endif

// Our minimal USBD MSC class — register with the composite builder as CLASS_TYPE_MSC
// (it supplies the descriptor; this class only opens EP3 + answers EP0 class requests).
extern USBD_ClassTypeDef USBD_MSC_Min;

// Create the MSC BOT thread. Also performs the one-shot USBD_Start (device connect),
// so it must be called once, after the scheduler is running.
void UsbMsc_StartTask(void);

// OTG-ISR hooks, called from usbd_conf.c's HAL_PCD callbacks for the MSC bulk EP only.
// They just wake the BOT thread — no USBD dispatch, so they never touch classId.
void UsbMsc_OnEpIn(uint8_t epnum);                 // EP3 IN transfer complete
void UsbMsc_OnEpOut(uint8_t epnum, uint32_t rxlen); // EP3 OUT data ready

#ifdef __cplusplus
}
#endif

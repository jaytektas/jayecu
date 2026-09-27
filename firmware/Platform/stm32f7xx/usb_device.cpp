#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "usb_msc.h"     // our thread-based MSC class (replaces ST's USBD_MSC)

// usbd_composite_builder.h can't be included from C++ (it has a parameter named
// `class`). Forward-declare the one builder entry point we call. CLASS_TYPE_*
// come from usbd_def.h (via usbd_core.h).
extern "C" uint32_t USBD_CMPSIT_SetClassID(USBD_HandleTypeDef* pdev,
                                           USBD_CompositeClassTypeDef Class,
                                           uint32_t Instance);

// CDC + MSC composite device. CDC presents the VCP (the comms
// protocol); MSC exposes the SD card as a removable drive. The ST composite
// builder assembles the combined config descriptor (with the CDC IAD) and
// assigns endpoints from the per-class address arrays below.
//
// Endpoint map (must not collide):
//   CDC: data IN 0x81, data OUT 0x01, cmd IN 0x82
//   MSC: bulk IN 0x83, bulk OUT 0x03

USBD_HandleTypeDef hUsbDeviceFS;

extern "C" void platform_usb_init(void) {
    UsbCdc_PreInit();   // create the RX stream buffer NOW, not from USB ISR

    static uint8_t cdc_ep[3] = { CDC_IN_EP, CDC_OUT_EP, CDC_CMD_EP };  // 0x81, 0x01, 0x82
    static uint8_t msc_ep[2] = { 0x83U, 0x03U };

    USBD_Init(&hUsbDeviceFS, &VCP_Desc, USBD_SPEED_FULL);

    USBD_RegisterClassComposite(&hUsbDeviceFS, &USBD_CDC, CLASS_TYPE_CDC, cdc_ep);
    // Register our minimal MSC class AS CLASS_TYPE_MSC: the composite builder still
    // emits the standard MSC interface + bulk-EP descriptor from the class type; our
    // class only opens EP3 and answers EP0 class requests. The bulk data path lives
    // in the MSC BOT thread (usb_msc.cpp), not in any USBD class callback.
    USBD_RegisterClassComposite(&hUsbDeviceFS, &USBD_MSC_Min, CLASS_TYPE_MSC, msc_ep);

    // Bind the CDC interface callbacks at its class slot (the Register* helper writes
    // pUserData[pdev->classId]). MSC needs no storage registration — the thread owns it.
    USBD_CMPSIT_SetClassID(&hUsbDeviceFS, CLASS_TYPE_CDC, 0);
    USBD_Interface_Init_FS(&hUsbDeviceFS);   // -> USBD_CDC_RegisterInterface

    // NOTE: USBD_Start (which connects D+ and makes the host begin enumerating)
    // is deliberately NOT called here. With the deferred USBD-in-task design,
    // enumeration control transfers are processed by usb_task — which isn't
    // running until the scheduler starts. So usb_task itself calls USBD_Start as
    // its first action, guaranteeing the host only sees the device once we can
    // answer it. (Calling it here, pre-scheduler, caused descriptor-read -71
    // glitches.)
}

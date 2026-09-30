// ---------------------------------------------------------------------------
// Thread-based USB Mass Storage (BOT/SCSI) — see usb_msc.h.
//
// The OTG ISR (usbd_conf.c) services EP0 + CDC inline and, for the MSC bulk EP only,
// just wakes this thread via a semaphore. msc_task runs the Bulk-Only-Transport loop
// (recv CBW -> exec SCSI [SD read/write + data phase] -> send CSW) using a per-EP
// BLOCKING transfer primitive over HAL PCD. Because the SD blocking happens in this
// thread (never in the ISR, never under an OTG mask), the ISR keeps pumping CDC the
// whole time. SD ownership stays gated by SdArbitrator (USB owns it key-off).
// ---------------------------------------------------------------------------
#include "usb_msc.h"
#include "usbd_core.h"
#include "usbd_conf.h"     // MSC_MEDIA_PACKET
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
#include "SdCardSpi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32f7xx_hal.h"
#include <string.h>

extern PCD_HandleTypeDef  hpcd_USB_OTG_FS;
extern USBD_HandleTypeDef hUsbDeviceFS;
extern "C" bool SdArbitrator_UsbHasCard(void);
extern "C" void SdArbitrator_UsbNoteWrite(void);

// EP map — must match usb_device.cpp's msc_ep[] = {0x83, 0x03}.
#define MSC_EP_IN     0x83U
#define MSC_EP_OUT    0x03U
#define MSC_EPNUM     3U
#define MSC_FS_MPS    64U
#define BLK_SIZE      512U
#define CHUNK_SECTORS (MSC_MEDIA_PACKET / BLK_SIZE)   // 4096/512 = 8 sectors per transfer

// Bulk-Only-Transport framing.
#define CBW_SIGNATURE 0x43425355U   // "USBC"
#define CSW_SIGNATURE 0x53425355U   // "USBS"
#define CBW_LEN       31U
#define CSW_LEN       13U
#define CSW_PASSED    0x00U
#define CSW_FAILED    0x01U

// SCSI opcodes (subset; from lib_scsi.h).
#define SCSI_TEST_UNIT_READY              0x00U
#define SCSI_REQUEST_SENSE                0x03U
#define SCSI_INQUIRY                      0x12U
#define SCSI_MODE_SENSE_6                 0x1AU
#define SCSI_START_STOP_UNIT              0x1BU
#define SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1EU
#define SCSI_READ_FORMAT_CAPACITIES       0x23U
#define SCSI_READ_CAPACITY_10             0x25U
#define SCSI_READ_10                      0x28U
#define SCSI_WRITE_10                     0x2AU
#define SCSI_VERIFY_10                    0x2FU

// SCSI sense (key, ASC, ASCQ) used in REQUEST_SENSE replies.
#define SENSE_NONE         0x00U, 0x00U, 0x00U
#define SENSE_NOT_READY    0x02U, 0x3AU, 0x00U   // medium not present
#define SENSE_MEDIUM_ERR   0x03U, 0x11U, 0x00U   // unrecovered read error
#define SENSE_ILLEGAL_REQ  0x05U, 0x20U, 0x00U   // invalid command

#pragma pack(push, 1)
typedef struct {
    uint32_t signature; uint32_t tag; uint32_t data_len;
    uint8_t  flags; uint8_t lun; uint8_t cmd_len; uint8_t cmd[16];
} cbw_t;
typedef struct {
    uint32_t signature; uint32_t tag; uint32_t residue; uint8_t status;
} csw_t;
#pragma pack(pop)
static_assert(sizeof(cbw_t) == 31, "CBW must be 31 bytes");
static_assert(sizeof(csw_t) == 13, "CSW must be 13 bytes");

// --- per-EP blocking transfer state (single consumer: msc_task) -------------
static SemaphoreHandle_t s_in_done, s_out_done, s_halt_cleared;
static StaticSemaphore_t s_in_cb, s_out_cb, s_halt_cb;
static volatile uint32_t s_out_rxlen;
static volatile bool     s_reset;        // BOT reset / deconfig — abort the current exchange
static volatile bool     s_configured;   // SET_CONFIG opened EP3

static StaticTask_t s_msc_tcb;
static StackType_t  s_msc_stack[768];

// THE CBW IS RECEIVED INTO A WHOLE PACKET. The HAL arms a bulk OUT endpoint for at least one full packet
// (MSC_FS_MPS) and its RXFLVL handler copies every byte the host sent, whatever length was asked for. Out of
// step with the host — a WRITE(10) abandoned mid-data leaves the host still sending 64-byte data packets —
// the next "31-byte" CBW receive took a 64-byte packet and wrote 33 bytes past a bare cbw_t: into the MSC
// task's stack, which sits right after it, where FreeRTOS reported it as a stack overflow in 'MSC'.
static union { cbw_t cbw; uint8_t packet[MSC_FS_MPS]; } s_cbw_rx;
static cbw_t& s_cbw = s_cbw_rx.cbw;
static csw_t    s_csw;
static uint8_t  s_blkbuf[MSC_MEDIA_PACKET];
static uint8_t  s_sense[3] = { SENSE_NONE };   // key, ASC, ASCQ for the next REQUEST_SENSE

// ===== OTG-ISR hooks (wake the thread; no USBD dispatch) =====================
void UsbMsc_OnEpIn(uint8_t epnum) {
    if (epnum != MSC_EPNUM) return;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(s_in_done, &w);
    portYIELD_FROM_ISR(w);
}
void UsbMsc_OnEpOut(uint8_t epnum, uint32_t rxlen) {
    if (epnum != MSC_EPNUM) return;
    s_out_rxlen = rxlen;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(s_out_done, &w);
    portYIELD_FROM_ISR(w);
}

// Called from the (ISR-context) class callbacks below to unblock the thread on a
// BOT reset or (de)configuration so it re-syncs to a fresh CBW.
static inline void wake_thread_isr(void) {
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(s_out_done, &w);
    xSemaphoreGiveFromISR(s_in_done,  &w);
    xSemaphoreGiveFromISR(s_halt_cleared, &w);
    portYIELD_FROM_ISR(w);
}

// ===== blocking EP transfer primitive (thread context only) =================
// Arm the HAL PCD transfer under a brief critical section (serialises the EP-register
// R-M-W against the OTG ISR + the CDC TX task — the ChibiOS osalSysLock equivalent),
// then sleep until the ISR signals completion. NEVER masks across the SD I/O itself.
static bool ep_send(const void* buf, uint32_t len) {
    if (s_reset) return false;
    (void)xSemaphoreTake(s_in_done, 0);   // drain any stale give (e.g. from a reset wake)
    taskENTER_CRITICAL();
    HAL_PCD_EP_Transmit(&hpcd_USB_OTG_FS, MSC_EP_IN, (uint8_t*)buf, len);
    taskEXIT_CRITICAL();
    if (xSemaphoreTake(s_in_done, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
    return !s_reset;
}
static bool ep_recv(void* buf, uint32_t len, uint32_t* got, TickType_t to) {
    if (s_reset) return false;
    (void)xSemaphoreTake(s_out_done, 0);  // drain any stale give before arming
    taskENTER_CRITICAL();
    HAL_PCD_EP_Receive(&hpcd_USB_OTG_FS, MSC_EP_OUT, (uint8_t*)buf, len);
    taskEXIT_CRITICAL();
    if (xSemaphoreTake(s_out_done, to) != pdTRUE) return false;
    if (got) *got = s_out_rxlen;
    return !s_reset;
}

// ===== minimal USBD MSC class (descriptor via composite builder) ============
// All three run in OTG-ISR context (EP0 control is inline) -> use *FromISR waking.
static uint8_t s_class_data;   // sentinel: a non-null, real address for pClassData

static uint8_t min_init(USBD_HandleTypeDef* pdev, uint8_t /*cfgidx*/) {
    (void)USBD_LL_OpenEP(pdev, MSC_EP_IN,  USBD_EP_TYPE_BULK, MSC_FS_MPS);
    pdev->ep_in[MSC_EP_IN & 0xFU].is_used = 1U;
    (void)USBD_LL_OpenEP(pdev, MSC_EP_OUT, USBD_EP_TYPE_BULK, MSC_FS_MPS);
    pdev->ep_out[MSC_EP_OUT & 0xFU].is_used = 1U;
    pdev->pClassDataCmsit[pdev->classId] = &s_class_data;   // non-null = "inited" to the core
    s_reset = true;          // force the thread to (re)sync to a fresh CBW
    s_configured = true;
    // NOTE: do NOT wake here — the thread is in its config-poll delay, not blocked on
    // a sem. Giving the sems now would leave them pre-signalled and make the first real
    // ep_send/ep_recv return early. The poll loop picks up s_configured on its own.
    return (uint8_t)USBD_OK;
}
static uint8_t min_deinit(USBD_HandleTypeDef* pdev, uint8_t /*cfgidx*/) {
    (void)USBD_LL_CloseEP(pdev, MSC_EP_IN);  pdev->ep_in[MSC_EP_IN & 0xFU].is_used = 0U;
    (void)USBD_LL_CloseEP(pdev, MSC_EP_OUT); pdev->ep_out[MSC_EP_OUT & 0xFU].is_used = 0U;
    pdev->pClassDataCmsit[pdev->classId] = NULL;
    s_configured = false; s_reset = true;
    wake_thread_isr();
    return (uint8_t)USBD_OK;
}
static uint8_t min_setup(USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req) {
    switch (req->bmRequest & USB_REQ_TYPE_MASK) {
    case USB_REQ_TYPE_CLASS:
        switch (req->bRequest) {
        case 0xFEU:  // BOT GET_MAX_LUN
            if (req->wValue == 0U && req->wLength == 1U && (req->bmRequest & 0x80U)) {
                static uint8_t lun = 0U;            // single LUN
                (void)USBD_CtlSendData(pdev, &lun, 1U);
            } else { USBD_CtlError(pdev, req); return (uint8_t)USBD_FAIL; }
            break;
        case 0xFFU:  // BOT MASS STORAGE RESET
            if (req->wValue == 0U && req->wLength == 0U && !(req->bmRequest & 0x80U)) {
                s_reset = true; wake_thread_isr();
            } else { USBD_CtlError(pdev, req); return (uint8_t)USBD_FAIL; }
            break;
        default: USBD_CtlError(pdev, req); return (uint8_t)USBD_FAIL;
        }
        break;
    case USB_REQ_TYPE_STANDARD:
        switch (req->bRequest) {
        case USB_REQ_GET_STATUS: { static uint16_t st = 0U; (void)USBD_CtlSendData(pdev, (uint8_t*)&st, 2U); } break;
        case USB_REQ_GET_INTERFACE: { static uint8_t z = 0U; (void)USBD_CtlSendData(pdev, &z, 1U); } break;
        case USB_REQ_SET_INTERFACE: break;
        case USB_REQ_CLEAR_FEATURE:
            // The core has already cleared the halt (USBD_StdEPReq). The thread may be waiting on it:
            // after stalling Bulk-In it sends the CSW only once the host has cleared the stall.
            if ((req->bmRequest & 0x1FU) == 0x02U && (req->wIndex & 0xFFU) == MSC_EP_IN) {
                BaseType_t w = pdFALSE;
                xSemaphoreGiveFromISR(s_halt_cleared, &w);
                portYIELD_FROM_ISR(w);
            }
            break;
        default: USBD_CtlError(pdev, req); return (uint8_t)USBD_FAIL;
        }
        break;
    default: USBD_CtlError(pdev, req); return (uint8_t)USBD_FAIL;
    }
    return (uint8_t)USBD_OK;
}

USBD_ClassTypeDef USBD_MSC_Min = {
    min_init, min_deinit, min_setup,
    NULL, NULL,           // EP0_TxSent, EP0_RxReady
    NULL, NULL,           // DataIn, DataOut — EP3 is serviced by the ISR hooks, never here
    NULL, NULL, NULL,     // SOF, IsoIN, IsoOUT
    NULL, NULL, NULL, NULL // composite: descriptor getters supplied by the builder
};

// ===== SCSI command handling ================================================
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void set_sense(uint8_t k, uint8_t asc, uint8_t ascq) {
    s_sense[0] = k; s_sense[1] = asc; s_sense[2] = ascq;
}

// 36-byte standard INQUIRY response.
static const uint8_t INQUIRY36[36] = {
    0x00, 0x80, 0x02, 0x02, 0x1F, 0x00, 0x00, 0x00,
    'J','a','y','E','C','U',' ',' ',
    'S','D',' ','C','a','r','d',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    '1','.','0','0',
};

// Returns CSW status; sets *residue. Performs the data phase via ep_send/ep_recv.
// Returns false (not a CSW status) only if the exchange was aborted (reset/timeout) —
// caller skips the CSW in that case.
static bool scsi_exec(uint8_t* out_status, uint32_t* out_residue) {
    const uint8_t* cmd = s_cbw.cmd;
    const uint32_t hlen = s_cbw.data_len;     // host-expected data length
    *out_residue = hlen;
    *out_status  = CSW_FAILED;

    switch (cmd[0]) {
    case SCSI_TEST_UNIT_READY:
        if (!SdArbitrator_UsbHasCard() || !SdCard_IsAvailable()) { set_sense(SENSE_NOT_READY); return true; }
        *out_status = CSW_PASSED; *out_residue = 0; return true;

    case SCSI_REQUEST_SENSE: {
        uint8_t r[18]; memset(r, 0, sizeof(r));
        r[0] = 0x70; r[2] = s_sense[0]; r[7] = 0x0A; r[12] = s_sense[1]; r[13] = s_sense[2];
        uint32_t n = (hlen < sizeof(r)) ? hlen : sizeof(r);
        if (n && !ep_send(r, n)) return false;
        set_sense(SENSE_NONE);
        *out_status = CSW_PASSED; *out_residue = hlen - n; return true;
    }
    case SCSI_INQUIRY: {
        uint32_t n = (hlen < sizeof(INQUIRY36)) ? hlen : sizeof(INQUIRY36);
        if (n && !ep_send(INQUIRY36, n)) return false;
        *out_status = CSW_PASSED; *out_residue = hlen - n; return true;
    }
    case SCSI_MODE_SENSE_6: {
        uint8_t r[4] = { 0x03, 0x00, 0x00, 0x00 };   // mode data len 3, no pages
        uint32_t n = (hlen < sizeof(r)) ? hlen : sizeof(r);
        if (n && !ep_send(r, n)) return false;
        *out_status = CSW_PASSED; *out_residue = hlen - n; return true;
    }
    case SCSI_READ_CAPACITY_10: {
        if (!SdArbitrator_UsbHasCard()) { set_sense(SENSE_NOT_READY); return true; }
        uint32_t sectors = SdCard_GetSectorCount();
        uint8_t r[8]; put_be32(&r[0], sectors ? sectors - 1U : 0U); put_be32(&r[4], BLK_SIZE);
        if (!ep_send(r, sizeof(r))) return false;
        *out_status = CSW_PASSED; *out_residue = hlen - sizeof(r); return true;
    }
    case SCSI_READ_FORMAT_CAPACITIES: {
        if (!SdArbitrator_UsbHasCard()) { set_sense(SENSE_NOT_READY); return true; }
        uint32_t sectors = SdCard_GetSectorCount();
        uint8_t r[12]; memset(r, 0, sizeof(r));
        r[3] = 0x08;                              // capacity list length
        put_be32(&r[4], sectors);                 // number of blocks
        r[8] = 0x02;                              // formatted media
        r[9] = 0x00; r[10] = (BLK_SIZE >> 8) & 0xFF; r[11] = BLK_SIZE & 0xFF;
        uint32_t n = (hlen < sizeof(r)) ? hlen : sizeof(r);
        if (n && !ep_send(r, n)) return false;
        *out_status = CSW_PASSED; *out_residue = hlen - n; return true;
    }
    case SCSI_READ_10: {
        if (!SdArbitrator_UsbHasCard()) { set_sense(SENSE_NOT_READY); return true; }
        uint32_t lba = be32(&cmd[2]); uint32_t nblk = be16(&cmd[7]);
        uint32_t sent = 0;
        while (nblk) {
            uint32_t chunk = (nblk < CHUNK_SECTORS) ? nblk : CHUNK_SECTORS;
            if (!SdCard_Read(s_blkbuf, lba, chunk)) { set_sense(SENSE_MEDIUM_ERR); *out_residue = hlen - sent; return true; }
            uint32_t bytes = chunk * BLK_SIZE;
            if (!ep_send(s_blkbuf, bytes)) return false;
            lba += chunk; nblk -= chunk; sent += bytes;
        }
        *out_status = CSW_PASSED; *out_residue = hlen - sent; return true;
    }
    case SCSI_WRITE_10: {
        if (!SdArbitrator_UsbHasCard()) { set_sense(SENSE_NOT_READY); return true; }
        SdArbitrator_UsbNoteWrite();
        uint32_t lba = be32(&cmd[2]); uint32_t nblk = be16(&cmd[7]);
        uint32_t recvd = 0;
        while (nblk) {
            uint32_t chunk = (nblk < CHUNK_SECTORS) ? nblk : CHUNK_SECTORS;
            uint32_t bytes = chunk * BLK_SIZE, got = 0;
            if (!ep_recv(s_blkbuf, bytes, &got, pdMS_TO_TICKS(3000)) || got != bytes) return false;
            if (!SdCard_Write(s_blkbuf, lba, chunk)) { set_sense(SENSE_MEDIUM_ERR); *out_residue = hlen - recvd; return true; }
            lba += chunk; nblk -= chunk; recvd += bytes;
        }
        *out_status = CSW_PASSED; *out_residue = hlen - recvd; return true;
    }
    case SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL:
    case SCSI_START_STOP_UNIT:
    case SCSI_VERIFY_10:
        *out_status = CSW_PASSED; *out_residue = 0; return true;

    default:
        set_sense(SENSE_ILLEGAL_REQ);
        return true;   // FAILED, no data
    }
}

// ===== BOT thread ===========================================================
static void msc_task(void* /*arg*/) {
    // One-shot device connect — deferred to here (post-scheduler) so enumeration is
    // answered immediately by the inline-ISR EP0 path (pre-scheduler USBD_Start left
    // descriptor reads unanswered). CDC enumerates off the same call.
    USBD_Start(&hUsbDeviceFS);

    for (;;) {
        if (!s_configured) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        s_reset = false;

        uint32_t got = 0;
        if (!ep_recv(s_cbw_rx.packet, CBW_LEN, &got, portMAX_DELAY)) continue;   // reset/deconfig → resync
        if (got != CBW_LEN || s_cbw.signature != CBW_SIGNATURE) {
            // Invalid/lost CBW → stall the data EP; the host recovers with a BOT reset.
            USBD_LL_StallEP(&hUsbDeviceFS, MSC_EP_IN);
            continue;
        }

        uint8_t status; uint32_t residue;
        (void)xSemaphoreTake(s_halt_cleared, 0);        // drain a stale give before this exchange
        if (!scsi_exec(&status, &residue)) continue;   // aborted mid-exchange → resync

        // THE DATA PHASE MUST END THE WAY THE HOST CAN SEE (BOT 1.0, 6.7.2 / 6.7.3). A command the host
        // expects data for, that sends less, has to say so on the wire — or the host goes on reading the
        // data stage and takes the CSW for data. Windows answers that phase error with a RESET of the
        // whole device, which takes the CDC link down with it: the studio's SD fetch died mid-transfer
        // every time Windows polled the card while the ECU held it (READ CAPACITY / READ(10) answered
        // "not ready" with no data and no stall). Linux tolerated it, which is why it only showed there.
        //   Data-In short, with no short packet to end it (nothing sent, or a whole number of packets):
        //     STALL Bulk-In, wait for the host to clear it, then send the CSW.
        //   Data-Out not all taken: STALL Bulk-Out so the host stops sending; the CSW follows as usual.
        if (residue && s_cbw.data_len) {
            const uint32_t sent = s_cbw.data_len - residue;
            if (s_cbw.flags & 0x80U) {
                if (sent == 0U || (sent % MSC_FS_MPS) == 0U) {
                    USBD_LL_StallEP(&hUsbDeviceFS, MSC_EP_IN);
                    if (xSemaphoreTake(s_halt_cleared, pdMS_TO_TICKS(5000)) != pdTRUE || s_reset) continue;
                }
            } else {
                USBD_LL_StallEP(&hUsbDeviceFS, MSC_EP_OUT);
            }
        }

        s_csw.signature = CSW_SIGNATURE;
        s_csw.tag       = s_cbw.tag;
        s_csw.residue   = residue;
        s_csw.status    = status;
        (void)ep_send(&s_csw, CSW_LEN);
    }
}

void UsbMsc_StartTask(void) {
    s_in_done  = xSemaphoreCreateBinaryStatic(&s_in_cb);
    s_out_done = xSemaphoreCreateBinaryStatic(&s_out_cb);
    s_halt_cleared = xSemaphoreCreateBinaryStatic(&s_halt_cb);
    // Priority 1 — the SAME as comms_task, BELOW can/cdc_tx (2) and the engine (3).
    // MSC is a key-off background diagnostic; it must NOT outrank the live tuning link
    // (comms_task builds the telemetry replies). At 2 it starved comms under MSC load
    // and CDC polls timed out; at 1 they time-slice fairly.
    (void)xTaskCreateStatic(msc_task, "MSC", sizeof(s_msc_stack) / sizeof(s_msc_stack[0]),
                            nullptr, 1, s_msc_stack, &s_msc_tcb);
}

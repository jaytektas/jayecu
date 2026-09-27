#include "usbd_cdc_if.h"
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"
#include "semphr.h"
#include "stm32f7xx_hal.h"
#include "../../../generated/schema_meta.h"   // JAYECU_BLOCK_SIZE (the read/write blocking factor)
#include "../../Comms/OmniProtocol.h"         // OMNI_MAX_FRAME — the TX FIFO must hold one full frame
#include <atomic>

// SD-card ownership hooks (SdArbitratorShim.cpp) — drive MSC/ECU arbitration
// from the USB connection state.
extern "C" void SdArbitrator_OnUsbConfigured(void);
extern "C" void SdArbitrator_OnUsbReset(void);

// ---------------------------------------------------------------------------
// CDC Interface configuration
// ---------------------------------------------------------------------------

#define APP_RX_DATA_SIZE  Comms::OMNI_MAX_FRAME   // usable capacity = one max frame (ACK-gated: only one in flight)
#define APP_TX_DATA_SIZE  2048

static uint8_t UserRxBufferFS[64];   // USB FS bulk packet size; DMA fills this per-packet then streams it out

extern USBD_HandleTypeDef hUsbDeviceFS;

// Statically-allocated stream buffer. xStreamBufferCreateStatic is safe from
// any context (no malloc). Created lazily on first use via this storage.
// (CDC_Init_FS is called from USB ISR during SET_CONFIGURATION — calling
// xStreamBufferCreate there would call pvPortMalloc which is not ISR-safe.)
static StaticStreamBuffer_t   xRxStreamBufferStruct;
static uint8_t                xRxStreamStorage[APP_RX_DATA_SIZE + 1];
static StreamBufferHandle_t   xRxStreamBuffer = NULL;
static std::atomic<bool>      g_UsbConnected{false};
static std::atomic<bool>      g_UsbSuspended{false};   // bus idle (cable pull / host sleep) — see OnSuspend

// Called once from main()/platform_init() BEFORE USBD_Init so the buffer is
// ready by the time the USB IRQ asks for it.
extern "C" void UsbCdc_PreInit(void) {
    if (xRxStreamBuffer == NULL) {
        // xStreamBufferCreateStatic takes the storage SIZE not the desired capacity:
        // usable bytes = xLength - 1. Pass APP_RX_DATA_SIZE + 1 so usable = APP_RX_DATA_SIZE.
        xRxStreamBuffer = xStreamBufferCreateStatic(
            APP_RX_DATA_SIZE + 1, 1,
            xRxStreamStorage, &xRxStreamBufferStruct);
    }
}

// ---------------------------------------------------------------------------
// CDC Interface Callbacks
// ---------------------------------------------------------------------------

static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t* pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {
    CDC_Init_FS,
    CDC_DeInit_FS,
    CDC_Control_FS,
    CDC_Receive_FS,
    CDC_TransmitCplt_FS,   // DataIn done — wakes the TX task off the completion instead of polling TxState
};

// Set by CDC_Init_FS (ISR) when a new USB session starts; cleared by UsbCdc_FlushRxOnReconnect
// (task context) which drains stale bytes from the previous session.
static std::atomic<bool> g_flush_rx_pending{false};

static int8_t CDC_Init_FS(void) {
    // Buffer was created via UsbCdc_PreInit() in main() — DO NOT allocate here,
    // we're running in USB ISR context.
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
    g_flush_rx_pending = true;   // ask task to drain any leftover bytes from previous session
    g_UsbConnected = true;
    g_UsbSuspended = false;   // fresh enumeration: bus is active
    // Host configured the device — hand the SD card to USB (MSC) unless the
    // engine is running, so MSC and the ECU never touch SPI3 concurrently.
    SdArbitrator_OnUsbConfigured();
    return (USBD_OK);
}

static int8_t CDC_DeInit_FS(void) {
    g_UsbConnected = false;
    SdArbitrator_OnUsbReset();   // host gone — ECU reclaims the card
    return (USBD_OK);
}

static int8_t CDC_Control_FS(uint8_t /*cmd*/, uint8_t* /*pbuf*/, uint16_t /*length*/) {
    return (USBD_OK);
}

static int8_t CDC_Receive_FS(uint8_t* pbuf, uint32_t *Len) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xRxStreamBuffer != NULL) {
        xStreamBufferSendFromISR(xRxStreamBuffer, pbuf, *Len, &xHigherPriorityTaskWoken);
    }
    // Re-arm RX with the original buffer base BEFORE re-prepping the EP.
    // The CDC library advances internal state on each packet; jaytek's
    // working impl resets the buffer pointer here, we previously didn't.
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return (USBD_OK);
}

USBD_StatusTypeDef USBD_Interface_Init_FS(USBD_HandleTypeDef *pdev) {
    return (USBD_StatusTypeDef)USBD_CDC_RegisterInterface(pdev, &USBD_Interface_fops_FS);
}

// ---------------------------------------------------------------------------
// UsbTransport Extern Functions
// ---------------------------------------------------------------------------

extern "C" size_t UsbCdc_Read(uint8_t* buffer, size_t max_length) {
    if (xRxStreamBuffer == NULL) return 0;
    // On new USB session: flush any leftover bytes from the previous session so
    // stale partial frames don't corrupt the first command from the new host.
    if (g_flush_rx_pending.exchange(false))
        xStreamBufferReset(xRxStreamBuffer);
    // Non-blocking read (wait 0 ticks)
    return xStreamBufferReceive(xRxStreamBuffer, buffer, max_length, 0);
}

extern "C" size_t UsbCdc_ReadBlocking(uint8_t* buffer, size_t max_length, uint32_t timeout_ms) {
    if (xRxStreamBuffer == NULL) return 0;
    if (g_flush_rx_pending.exchange(false))
        xStreamBufferReset(xRxStreamBuffer);
    // Blocks until the RX ISR's stream-buffer send wakes us (or timeout). The stream buffer's
    // trigger level is 1, so we wake on the first byte and then take everything available.
    return xStreamBufferReceive(xRxStreamBuffer, buffer, max_length, pdMS_TO_TICKS(timeout_ms));
}

// ---------------------------------------------------------------------------
// TX path — producer/consumer with a dedicated TX task
//
// Application code calls UsbCdc_Send(), which copies bytes into a FreeRTOS
// stream buffer and returns immediately. The TX task drains the stream
// buffer, hands the data to the USBD core via USBD_CDC_TransmitPacket, and
// waits for the DataIn callback (observed indirectly via TxState→0) to
// signal completion before submitting the next chunk.
//
// This decouples comms_task from USB timing — no busy-waits in caller
// context — and matches the canonical CubeMX-FreeRTOS CDC pattern.
// ---------------------------------------------------------------------------

// Byte-stream FIFO. CDC ACM is a byte stream and the comms protocol frames
// itself (Size + CRC32 header), so the transport doesn't need to preserve
// send() boundaries — a StreamBuffer lets a frame's header/payload/CRC, and
// even adjacent frames, coalesce into a single drain. Capacity holds the
// largest single response: the §8.8 SD block read (2 KB block + block_no +
// framing ~= 2057 bytes).
// FIFO must hold ONE COMPLETE max frame at once: send_packet() enqueues the 3-byte
// header, the payload, and the 4-byte CRC as SEPARATE all-or-nothing writes, so if the
// header is still queued when the payload arrives there must be room for header+payload
// +trailer together. The largest payload is a full blocking-factor page read, so:
//   FIFO must hold one whole omnidyno frame = 16 (header) + JAYECU_BLOCK_SIZE (payload) + 2 (CRC).
//   OMNI_MAX_FRAME is exactly that (derived from the block size), so the FIFO can never under-size.
// (Under-sizing this silently dropped the payload of a block-sized read — the host saw header+CRC, no
// data, and timed out. The omnidyno header is bigger than the old msEnvelope one, so +16 was too small.)
#define APP_TX_BUF_SIZE  (Comms::OMNI_MAX_FRAME)
#define APP_TX_CHUNK     2112  // bytes drained per TransmitPacket
static_assert(APP_TX_BUF_SIZE >= JAYECU_BLOCK_SIZE + Comms::OMNI_HEADER_SIZE + Comms::OMNI_CRC_SIZE,
              "TX FIFO must hold one full omnidyno frame");

static StaticStreamBuffer_t   xTxStreamBufferStruct;
static uint8_t                xTxStreamStorage[APP_TX_BUF_SIZE + 1];
static StreamBufferHandle_t   xTxStreamBuffer = NULL;

// Signalled by CDC_TransmitCplt_FS (USB IN-endpoint done, ISR context) so the TX task can sleep on
// the transfer completing instead of polling hcdc->TxState. Binary: at most one transfer in flight.
static StaticSemaphore_t      xTxDoneSemBuf;
static SemaphoreHandle_t      xTxDoneSem = NULL;

static StaticTask_t s_tx_tcb;
static StackType_t  s_tx_stack[256];

// Scratch the TX task drains into before handing the address to the USBD core,
// which streams straight out of it as IN tokens arrive.
static uint8_t s_tx_scratch[APP_TX_CHUNK];

// CDC is registered first in platform_usb_init(), so it owns composite class
// slot 0. In composite mode the live CDC handle is pClassDataCmsit[0] —
// pdev->pClassData is the *current* dispatched class and may point at MSC.
static constexpr uint8_t CDC_CLASS_ID = 0;

// DataIn (IN endpoint) completion — runs in USB ISR context. Gives the TX-done semaphore so the
// TX task can wake the instant the transfer finishes instead of polling hcdc->TxState. ST's CDC
// class null-checks this before calling, and only invokes it for our CDC IN endpoint.
static int8_t CDC_TransmitCplt_FS(uint8_t* /*pbuf*/, uint32_t* /*Len*/, uint8_t /*epnum*/) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xTxDoneSem != NULL) {
        xSemaphoreGiveFromISR(xTxDoneSem, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return (USBD_OK);
}

static void UsbCdc_TxTask(void* /*pv*/) {
    for (;;) {
        // 1) Block until the producer queues bytes, coalescing whatever it enqueued —
        //    header, payload, CRC, and back-to-back frames — into a single chunk.
        const size_t got = xStreamBufferReceive(xTxStreamBuffer,
                                                 s_tx_scratch, sizeof(s_tx_scratch),
                                                 portMAX_DELAY);
        if (got == 0) continue;

        // 2) Submit on the CDC class slot.
        USBD_CDC_HandleTypeDef* hcdc =
            (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassDataCmsit[CDC_CLASS_ID];
        if (!g_UsbConnected || hcdc == nullptr) continue;   // disconnected — drop this chunk
        xSemaphoreTake(xTxDoneSem, 0);                      // clear any stale completion signal
        USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_tx_scratch, (uint16_t)got, CDC_CLASS_ID);
        if (USBD_CDC_TransmitPacket(&hUsbDeviceFS, CDC_CLASS_ID) != USBD_OK) continue;

        // 3) Wait for the IN transfer to complete before reusing s_tx_scratch — the HAL streams
        //    straight out of it. CDC_TransmitCplt_FS gives the semaphore from the USB ISR.
        //
        //    A SLOW HOST IS NOT A LOST TRANSFER. This used to give up after 50 ms and go round again —
        //    which refilled s_tx_scratch while the core was still sending from it (the rest of that
        //    reply went out as bytes of the next one) and then had TransmitPacket refuse the new chunk
        //    as busy, dropping it. A host behind a VM's USB passthrough is late by more than 50 ms
        //    often enough that replies went missing under load. So the scratch is not touched until
        //    the core has finished with it; the 50 ms is only how often the task re-checks. If the
        //    host stops reading altogether, the task waits here and the FIFO fills — and UsbCdc_Send
        //    already drops a frame it cannot queue whole, so the comms task is never blocked by it.
        while (xSemaphoreTake(xTxDoneSem, pdMS_TO_TICKS(50)) != pdTRUE) {
            hcdc = (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassDataCmsit[CDC_CLASS_ID];
            if (!g_UsbConnected || hcdc == nullptr || hcdc->TxState == 0U) break;
        }
    }
}

extern "C" void UsbCdc_StartTxTask(void) {
    if (xTxDoneSem == NULL) {
        // Starts empty: the first take() blocks until a transfer actually completes.
        xTxDoneSem = xSemaphoreCreateBinaryStatic(&xTxDoneSemBuf);
    }
    if (xTxStreamBuffer == NULL) {
        // Trigger level 1: wake the task as soon as any byte is queued so small
        // replies aren't held back; it still grabs everything available at once.
        xTxStreamBuffer = xStreamBufferCreateStatic(
            APP_TX_BUF_SIZE, 1,
            xTxStreamStorage, &xTxStreamBufferStruct);
    }
    xTaskCreateStatic(UsbCdc_TxTask, "UsbTx", 256, nullptr,
                      2, s_tx_stack, &s_tx_tcb);
}

extern "C" bool UsbCdc_Send(const uint8_t* data, size_t length) {
    if (!g_UsbConnected) return false;
    if (length == 0) return true;
    if (length > APP_TX_BUF_SIZE) return false;   // cannot ever fit the FIFO
    if (xTxStreamBuffer == NULL) return false;

    // Single producer (comms task), so checking space then sending is race-free:
    // only the TX task frees space, never another writer. This keeps each send
    // all-or-nothing — we never splice a truncated frame into the stream.
    if (xStreamBufferSpacesAvailable(xTxStreamBuffer) < length) return false;
    const size_t written = xStreamBufferSend(xTxStreamBuffer, data, length, 0);
    return written == length;
}

extern "C" bool UsbCdc_IsConnected() {
    // Configured AND the bus is active. The suspend flag is what catches a cable pull on this board
    // (VBUS sensing off -> no reset on unplug), so "connected" falls back within one comms poll.
    return g_UsbConnected && !g_UsbSuspended;
}

// Bus went idle (no SOFs for ~3 ms). With VBUS sensing off this is how a CABLE PULL presents — there
// is no USB reset to run CDC_DeInit. Clearing the connected view here lets the transport + comms LED
// fall back to "no host". A genuine host suspend looks the same and is treated the same (no comms).
extern "C" void UsbCdc_OnSuspend() { g_UsbSuspended = true; }
extern "C" void UsbCdc_OnResume()  { g_UsbSuspended = false; }

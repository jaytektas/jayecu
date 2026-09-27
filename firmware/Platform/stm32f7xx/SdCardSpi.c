/*
 * The SPI-mode card protocol here is adapted from ChaN's FatFs sample
 * "MMCv3/SDv1/SDv2 (in SPI mode) control module":
 *
 *   Copyright (C) 2013, ChaN, all right reserved.
 *
 *   * This software is a free software and there is NO WARRANTY.
 *   * No restriction on use. You can use, modify and redistribute it for
 *     personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
 *   * Redistributions of source code must retain the above copyright notice.
 */
#include "SdCardSpi.h"
#include "platform_config.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "../../Scheduler/Log.h"

SPI_HandleTypeDef hspi3;   // the SD card's SPI, whichever peripheral the board named

// Set by SdCard_Configure() from the board's board_init(). Null until then, which
// SdCard_Init() refuses rather than dereferencing.
static SPI_TypeDef*  s_spi     = NULL;
static GPIO_TypeDef* s_cs_port = NULL;
static uint16_t      s_cs_pin  = 0;

void SdCard_Configure(SPI_TypeDef* spi, GPIO_TypeDef* cs_port, uint16_t cs_pin) {
    s_spi = spi; s_cs_port = cs_port; s_cs_pin = cs_pin;
}

// ---------------------------------------------------------------------------
// Mutex helpers
// Mutex is NOT ISR-safe. USB MSC diskio.c runs from an ISR context;
// SdArbitrator guarantees USB and ECU never overlap, so ISR callers skip it.
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_mutex = NULL;
static StaticSemaphore_t s_mutex_buf;

void SdCard_InitMutex(void) {
    if (s_mutex == NULL)
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
}

static inline bool in_isr(void) {
    return (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) != 0;
}

// ---------------------------------------------------------------------------
// THE ENGINE FRAME MUST NEVER WAIT ON THIS CARD.
//
// SD here is polled byte-at-a-time SPI with no DMA, and a card that has gone away to do an internal
// erase can hold the bus for a hundred milliseconds — a hundred 1 kHz frames. Everything that
// touches the card today sits at priority 1 (Comms, and the config/learned save task); the engine
// frame is 3 and the per-cycle compute is 2, so the logger only ever runs in their slack and is
// preempted mid-sector. That is a property of the current call sites, not of anything enforced —
// which is exactly the kind of guarantee that is quietly lost later.
//
// So: one warning, the first time anything above SD_MAX_CALLER_PRIORITY takes this mutex. Loud on
// a bench, where it will be found, and harmless in a car.
//
// It is deliberately NOT configASSERT(): that disables interrupts and spins forever, turning a
// programming mistake into a dead engine on the side of a road. Priority inheritance already bounds
// the real damage to one card operation; this is here to name the mistake, not to punish it.
// ---------------------------------------------------------------------------
#define SD_MAX_CALLER_PRIORITY 1

static bool s_prio_warned = false;

static inline void sd_check_caller_priority(void) {
    if (s_prio_warned || in_isr()) return;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;   // boot-time access is fine
    const UBaseType_t prio = uxTaskPriorityGet(NULL);
    if (prio <= SD_MAX_CALLER_PRIORITY) return;
    s_prio_warned = true;   // once: a violation repeats every sector, and the first one says it all
    EFI_LOG_WARN("sd", "SD accessed at task priority %u (max %u) - can stall the 1 kHz frame",
                 (unsigned)prio, (unsigned)SD_MAX_CALLER_PRIORITY);
}

#define MUTEX_LOCK() \
    sd_check_caller_priority(); \
    const bool _use_mutex = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) \
                             && s_mutex && !in_isr(); \
    if (_use_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY)

#define MUTEX_UNLOCK() \
    if (_use_mutex) xSemaphoreGive(s_mutex)

// ---------------------------------------------------------------------------
// SPI helpers — register-level polled I/O for maximum throughput
// ---------------------------------------------------------------------------

static inline uint8_t spi_exchange(uint8_t tx) {
    while (!(s_spi->SR & SPI_SR_TXE)) {}
    *(volatile uint8_t *)&s_spi->DR = tx;
    while (!(s_spi->SR & SPI_SR_RXNE)) {}
    return *(volatile uint8_t *)&s_spi->DR;
}

#define SPI_TX(b)   spi_exchange(b)
#define SPI_RX()    spi_exchange(0xFF)

static void spi_set_speed(uint32_t prescaler) {
    s_spi->CR1 &= ~SPI_CR1_SPE;
    s_spi->CR1 = (s_spi->CR1 & ~(0x7u << SPI_CR1_BR_Pos))
                          | (prescaler << SPI_CR1_BR_Pos);
    s_spi->CR1 |= SPI_CR1_SPE;
}

static void sd_cs_low (void) { HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET); }
static void sd_cs_high(void) { HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);   }

static void sd_delay_ms(uint32_t ms) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        vTaskDelay(pdMS_TO_TICKS(ms));
    else
        for (uint32_t i = 0; i < ms * 22000u; i++) __asm("nop");
}

// ---------------------------------------------------------------------------
// SD protocol
// ---------------------------------------------------------------------------

/* Card type flags (CardType) */
#define CT_MMC   0x01
#define CT_SD1   0x02
#define CT_SD2   0x04
#define CT_BLOCK 0x08

static uint8_t  s_card_type    = 0;
static uint32_t s_sector_count = 0;

static void sd_deselect(void) { sd_cs_high(); SPI_RX(); }

// Wall-clock timeout via the DWT cycle counter. We CANNOT use HAL_GetTick here:
// the OTG_FS ISR (priority 9) outranks SysTick, so the systick is frozen while
// the USB MSC callbacks run in ISR context — a tick-based timeout would never
// expire and a marginal card would hang the USB device (→ host reset). DWT->CYCCNT
// free-runs at SystemCoreClock in any context. (Enabled in SdCard_Init.)
static inline uint32_t sd_cyc(void) { return DWT->CYCCNT; }
static inline uint32_t sd_timeout_cyc(void) {
    // Short in ISR (USB MSC) so a stuck card can't stall USB and trip a host
    // reset; generous in task context (a write can keep the card busy ~250 ms).
    const uint32_t ms = in_isr() ? 120u : 500u;
    return ms * (SystemCoreClock / 1000u);
}

static bool sd_wait_ready(void) {
    const uint32_t t0 = sd_cyc(), tmo = sd_timeout_cyc();
    do {
        if (SPI_RX() == 0xFF) return true;
    } while ((sd_cyc() - t0) < tmo);
    return false;
}

static bool sd_select(void) {
    sd_cs_low();
    SPI_RX();
    if (s_card_type == 0) return true;   // before init — skip ready check
    if (sd_wait_ready()) return true;
    sd_deselect();
    return false;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t res, n;

    if (cmd & 0x80) {                    // ACMD prefix
        cmd &= 0x7F;
        res = sd_send_cmd(55, 0);
        if (res > 1) return res;
    }

    if (cmd != 12) {
        sd_deselect();
        if (!sd_select()) return 0xFF;
    }

    SPI_TX(0x40 | cmd);
    SPI_TX((uint8_t)(arg >> 24));
    SPI_TX((uint8_t)(arg >> 16));
    SPI_TX((uint8_t)(arg >>  8));
    SPI_TX((uint8_t) arg);
    /* CRC required for CMD0 and CMD8 only; dummy otherwise */
    n = (cmd == 0) ? 0x95 : (cmd == 8) ? 0x87 : 0x01;
    SPI_TX(n);

    if (cmd == 12) SPI_RX();   // discard one byte after CMD12

    n = 255;
    do { res = SPI_RX(); } while ((res & 0x80) && --n);
    return res;
}

static bool sd_rx_datablock(uint8_t* buf, uint32_t len) {
    uint8_t token;
    const uint32_t t0 = sd_cyc(), tmo = sd_timeout_cyc();
    do { token = SPI_RX(); } while (token == 0xFF && (sd_cyc() - t0) < tmo);
    if (token != 0xFE) return false;
    for (uint32_t j = 0; j < len; j++) buf[j] = spi_exchange(0xFF);
    SPI_RX(); SPI_RX();   // discard CRC
    return true;
}

static bool sd_tx_datablock(const uint8_t* buf, uint8_t token) {
    if (!sd_wait_ready()) return false;
    SPI_TX(token);
    if (token == 0xFD) return true;   // stop token — no data

    for (uint16_t i = 0; i < 512; i++) spi_exchange(buf[i]);
    SPI_TX(0xFF); SPI_TX(0xFF);        // dummy CRC

    const uint8_t resp = SPI_RX();
    if ((resp & 0x1F) != 0x05) return false;

    // AND THEN DO NOT WAIT FOR IT TO FINISH PROGRAMMING.
    //
    // This used to spin here until the card released DO, which reads like diligence and is the one
    // thing that must not happen inside a multi-block write. CMD25 exists so the host can push the
    // next block while the card programs the previous one; waiting here serialised it, making a
    // sixteen-sector streaming write cost exactly what sixteen single-block writes cost. Measured:
    // 94 KB/s on a card rated for 6 MB/s — 5.3 ms per 512-byte sector, which is a full program
    // cycle per block with no overlap at all.
    //
    // Nothing is lost by leaving. sd_tx_datablock() opens with wait_ready(), so the NEXT block
    // blocks only if the card has not caught up; sd_select() does the same before any command; and
    // a card left programming across a deselect finishes on its own — the spec allows exactly that.
    // This is also what ChaN's reference driver does, which is where the rest of this file came
    // from; the wait was our addition.
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SdCard_Invalidate(void) { s_card_type = 0; s_sector_count = 0; }

bool SdCard_Init(void) {
    // A board that never called SdCard_Configure() has no SD, and poking a null peripheral
    // would fault rather than report that.
    if (s_spi == NULL || s_cs_port == NULL) return false;

    MUTEX_LOCK();

    // Idempotent: if the card was successfully typed on a previous call, skip the
    // full SPI re-probe. Call SdCard_Invalidate() first to force a re-init.
    if (s_card_type != 0) { MUTEX_UNLOCK(); return true; }

    // Enable the DWT cycle counter (free-running) for ISR-safe SD timeouts.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    uint8_t n, ty, ocr[4];

    /* Re-init SPI at slow speed for card init (≤400 kHz) */
    hspi3.Instance               = s_spi;
    hspi3.Init.Mode              = SPI_MODE_MASTER;
    hspi3.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi3.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi3.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi3.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi3.Init.NSS               = SPI_NSS_SOFT;
    hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    hspi3.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi3.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi3.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi3.Init.CRCPolynomial     = 7;
    hspi3.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi3.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;

    if (HAL_SPI_Init(&hspi3) != HAL_OK) { MUTEX_UNLOCK(); return false; }

    s_spi->CR1 |= SPI_CR1_SSI | SPI_CR1_SSM;
    s_spi->CR2 |= SPI_CR2_FRXTH;
    s_spi->CR1 |= SPI_CR1_SPE;

    s_card_type = 0; s_sector_count = 0;
    sd_cs_high();
    sd_delay_ms(100);
    for (n = 20; n; n--) SPI_RX();   // ≥74 clocks with CS high

    /* CMD0 — reset (SPI mode) */
    uint8_t cmd0_tries = 100;
    while (sd_send_cmd(0, 0) != 1 && --cmd0_tries) sd_delay_ms(5);
    if (!cmd0_tries) { sd_deselect(); MUTEX_UNLOCK(); return false; }

    ty = 0;
    if (sd_send_cmd(8, 0x1AA) == 1) {
        /* SDv2 */
        for (n = 0; n < 4; n++) ocr[n] = SPI_RX();
        if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
            for (uint32_t i = 1000; i && sd_send_cmd(0x80 + 41, 1UL << 30); i--) sd_delay_ms(1);
            if (sd_send_cmd(0x80 + 41, 1UL << 30) == 0 && sd_send_cmd(58, 0) == 0) {
                for (n = 0; n < 4; n++) ocr[n] = SPI_RX();
                ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
            }
        }
    } else {
        /* SDv1 or MMC */
        uint8_t cmd;
        if (sd_send_cmd(0x80 + 41, 0) <= 1) { ty = CT_SD1; cmd = 0x80 + 41; }
        else                                 { ty = CT_MMC; cmd = 1; }
        for (uint32_t i = 1000; i && sd_send_cmd(cmd, 0); i--) sd_delay_ms(1);
        if (!sd_send_cmd(cmd, 0) && sd_send_cmd(16, 512) != 0) ty = 0;
    }

    s_card_type = ty;

    if (ty) {
        /* Read CSD for sector count */
        uint8_t csd[16];
        if (sd_send_cmd(9, 0) == 0 && sd_rx_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) {
                /* CSD v2 — sector count = (C_SIZE + 1) * 1024 */
                const uint32_t c_size = (((uint32_t)csd[7] & 0x3F) << 16)
                                      | ((uint32_t)csd[8] << 8)
                                      |  (uint32_t)csd[9];
                s_sector_count = (c_size + 1u) << 10;
            } else {
                /* CSD v1 */
                uint32_t nc = (csd[5] & 15) + ((csd[10] & 128) >> 7)
                             + ((csd[9] & 3) << 1) + 2;
                s_sector_count = ((uint32_t)((csd[8] >> 6) | (csd[7] << 2)
                                 | ((csd[6] & 3) << 10)) + 1) << (nc - 9);
            }
        }
        sd_deselect();
        spi_set_speed(1);   // APB1 54 MHz / 4 = 13.5 MHz — full speed (SD SPI ≤ 25 MHz)
    }

    sd_deselect();
    MUTEX_UNLOCK();
    return ty != 0;
}

bool SdCard_Read(uint8_t* buf, uint32_t sector, uint32_t count) {
    MUTEX_LOCK();
    if (!s_card_type) { MUTEX_UNLOCK(); return false; }

    /* SDSC uses byte addresses, SDHC/SDXC use block addresses */
    if (!(s_card_type & CT_BLOCK)) sector <<= 9;

    bool ok = false;
    if (count == 1) {
        ok = (sd_send_cmd(17, sector) == 0) && sd_rx_datablock(buf, 512);
    } else {
        if (sd_send_cmd(18, sector) == 0) {
            do {
                if (!sd_rx_datablock(buf, 512)) break;
                buf += 512;
            } while (--count);
            sd_send_cmd(12, 0);
            ok = (count == 0);
        }
    }

    sd_deselect();
    MUTEX_UNLOCK();
    return ok;
}

// WHAT THE DRIVER IS ACTUALLY ASKED FOR. FatFS issues a multi-sector disk_write when the file
// position is sector-aligned and the buffer holds whole sectors, and a single otherwise — but which
// of those a datalogger produces is a question about FatFS's internals, and the answer decides
// whether the card is being streamed to or poked one block at a time. Counted rather than assumed.
static uint32_t s_w_calls, s_w_single, s_w_multi, s_w_sectors, s_w_max_run;
void SdCard_WriteStats(uint32_t* calls, uint32_t* single, uint32_t* multi,
                       uint32_t* sectors, uint32_t* max_run) {
    if (calls)   *calls   = s_w_calls;
    if (single)  *single  = s_w_single;
    if (multi)   *multi   = s_w_multi;
    if (sectors) *sectors = s_w_sectors;
    if (max_run) *max_run = s_w_max_run;
}
void SdCard_WriteStatsReset(void) {
    s_w_calls = s_w_single = s_w_multi = s_w_sectors = s_w_max_run = 0;
}

bool SdCard_Write(const uint8_t* buf, uint32_t sector, uint32_t count) {
    s_w_calls++;
    s_w_sectors += count;
    if (count == 1) s_w_single++; else s_w_multi++;
    if (count > s_w_max_run) s_w_max_run = count;
    MUTEX_LOCK();
    if (!s_card_type) { MUTEX_UNLOCK(); return false; }

    if (!(s_card_type & CT_BLOCK)) sector <<= 9;

    bool ok = false;
    if (count == 1) {
        ok = (sd_send_cmd(24, sector) == 0) && sd_tx_datablock(buf, 0xFE);
    } else {
        if (sd_send_cmd(25, sector) == 0) {
            do {
                if (!sd_tx_datablock(buf, 0xFC)) break;
                buf += 512;
            } while (--count);
            ok = sd_tx_datablock(NULL, 0xFD) && (count == 0);
        }
    }

    sd_deselect();
    MUTEX_UNLOCK();
    return ok;
}

uint32_t SdCard_GetSectorCount(void) { return s_sector_count; }
bool     SdCard_IsAvailable(void)    { return s_card_type != 0; }

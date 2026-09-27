// Custom CMSIS SystemInit for JayECU on Jaytek V1 board.
// Brings the CPU to full 216 MHz BEFORE returning to startup — so BSS clear,
// data copy, __libc_init_array all run at full speed instead of HSI 16 MHz.
// No HAL, no globals, no BSS dependencies.

#include "stm32f7xx.h"

// AHBPrescTable / APBPrescTable are referenced by HAL_RCC. Provided here so
// the stock system_stm32f7xx.c doesn't need to be linked.
const uint8_t  AHBPrescTable[16] = {0,0,0,0,0,0,0,0,1,2,3,4,6,7,8,9};
const uint8_t  APBPrescTable[8]  = {0,0,0,0,1,2,3,4};
uint32_t SystemCoreClock = 216000000U;

void SystemInit(void) {
    // 1. FPU access
    SCB->CPACR |= ((3UL << (10*2)) | (3UL << (11*2)));
    // 2. VTOR — app is at 0x08000000 (no bootloader), startup .s already
    //    points VTOR here too; harmless to write again.
    SCB->VTOR = 0x08000000U;
}

// HAL calls SystemCoreClockUpdate after re-configuring the clock tree;
// provide a stub that just keeps the cached value, since we never reconfigure.
void SystemCoreClockUpdate(void) {
    SystemCoreClock = 216000000U;
}

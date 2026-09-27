/* JayECU STM32F767ZIT6 Linker Script
   Flash layout (no bootloader — app boots directly from 0x08000000).
   Config bank base addresses MUST be the start of the flash sector the
   firmware erases (single-bank STM32F767 256 KB sectors 5..11), otherwise
   erase and program/read target different regions — see platform_config.h.
     Application:   0x08000000  1536K  (sectors 0..9)
     Config bank A: 0x08180000   256K  (sector 10)
     Config bank B: 0x081C0000   256K  (sector 11)
*/

MEMORY
{
    DTCM        (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    RAM         (xrw) : ORIGIN = 0x20020000, LENGTH = 384K
    FLASH_APP    (rx) : ORIGIN = 0x08000000, LENGTH = 1536K
    FLASH_CFG_A  (rx) : ORIGIN = 0x08180000, LENGTH = 256K
    FLASH_CFG_B  (rx) : ORIGIN = 0x081C0000, LENGTH = 256K
}

/* Symbols exported for the config banks. Must match FLASH_BANK_*_BASE in
   platform_config.h, which main.cpp pins with static_assert. */
_flash_cfg_a_start = ORIGIN(FLASH_CFG_A);
_flash_cfg_a_end   = ORIGIN(FLASH_CFG_A) + LENGTH(FLASH_CFG_A);
_flash_cfg_b_start = ORIGIN(FLASH_CFG_B);
_flash_cfg_b_end   = ORIGIN(FLASH_CFG_B) + LENGTH(FLASH_CFG_B);

/* Link-time guards: keep the flash map sector-aligned, contiguous, and within
   the 2 MB device. Each config bank must own exactly one 256 KB sector. */
ASSERT(ORIGIN(FLASH_CFG_A) == 0x08180000,
       "Config bank A must start at flash sector 10 (0x08180000)")
ASSERT(ORIGIN(FLASH_CFG_B) == 0x081C0000,
       "Config bank B must start at flash sector 11 (0x081C0000)")
ASSERT(ORIGIN(FLASH_APP) + LENGTH(FLASH_APP) == ORIGIN(FLASH_CFG_A),
       "FLASH_APP must end exactly where config bank A begins")
ASSERT(ORIGIN(FLASH_CFG_A) + LENGTH(FLASH_CFG_A) == ORIGIN(FLASH_CFG_B),
       "Config banks A and B must be contiguous")
ASSERT(ORIGIN(FLASH_CFG_B) + LENGTH(FLASH_CFG_B) == 0x08200000,
       "Config bank B must end at the top of 2 MB flash")

/* The ISR / main stack lives in DTCM, not SRAM.
   DTCM is a dedicated core bus: no AXI arbitration, no D-cache line to miss or evict, and
   deterministic latency — which is what an ISR stack wants, because every interrupt entry and exit
   pushes and pops through it and this ECU takes one per trigger tooth, per virtual tooth and per
   scheduled event. It is also the one thing that is safe to put in an UNZEROED section (.dtcm is
   NOLOAD, so it is never cleared — that is deliberate, s_boot_to_dfu survives a reset there) and
   that DMA never touches, which on the F7 matters because DMA cannot address DTCM at all.
   Grows DOWN from the top of DTCM; .dtcm data is placed up from the bottom. */
_estack          = ORIGIN(DTCM) + LENGTH(DTCM);
/* NEWLIB heap. Deliberately tiny, not zero: nothing in this firmware calls malloc — FreeRTOS
   manages its own heap (heap_4.c, configTOTAL_HEAP_SIZE, ucHeap in .bss) and Lua allocates through
   pvPortMalloc into that, while -fno-exceptions/-fno-rtti keep the C++ runtime out of it.
   syscalls.c states the same and deliberately does not provide _sbrk. This was reserving 64 KB of
   SRAM for a heap with no consumer — 16% of the whole region. A small non-zero reservation is kept
   so `end`/`_end` stay meaningful and a stray single allocation fails by returning NULL rather than
   running off into whatever follows. */
/* Trimmed 0x400 -> 0x100 to land the vehicle-speed work: the region was 128 bytes short, and this is a
   reservation for a heap with no consumer at all — the paragraph above says so, and syscalls.c still does
   not provide _sbrk, so a stray malloc fails by returning NULL whatever this number is. It stays non-zero
   for the reason it always was: `end`/`_end` have to mean something. 256 bytes is the smallest amount
   that still says that out loud.
   This is NOT spare capacity. RAM is genuinely full — see the note in ecu.schema.yaml about the four
   wheel-speed rows that would not fit. The real recovery is the sensor pipeline pool, which allocates one
   411-byte entry per CATALOGUE ROW rather than per ENABLED sensor: 122 rows, most of them never fitted to
   any car, for about 40 KB. */
_Min_Heap_Size   = 0x100;
_Min_Stack_Size  = 0x1000;

ENTRY(Reset_Handler)

SECTIONS
{
  /* Config banks occupy dedicated flash regions — no code lives here.
     Bank A is seeded at flash time with the CRC-valid default tune image
     (generated/default_tune.cpp → .flash_cfg_a), so a freshly-flashed board
     boots the default tune. Bank B starts erased (0xFF); the StorageManager
     ping-pongs subsequent burns between the two.  */
  .flash_cfg_a (ORIGIN(FLASH_CFG_A)) : { KEEP(*(.flash_cfg_a)) } >FLASH_CFG_A
  .flash_cfg_b (ORIGIN(FLASH_CFG_B)) : { } >FLASH_CFG_B

  .dtcm (NOLOAD) :
  {
    . = ALIGN(4);
    *(.dtcm) *(.dtcm*)
    . = ALIGN(4);
  } >DTCM

  /* The DTCM stack grows down from _estack toward .dtcm data. They only meet if the two together
     exceed 128K, which is a link-time error rather than a mysterious runtime corruption. */
  ASSERT(SIZEOF(.dtcm) + _Min_Stack_Size <= LENGTH(DTCM),
         "DTCM data + ISR stack exceed the 128K DTCM region")

  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH_APP

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH_APP

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata) *(.rodata*)
    . = ALIGN(4);
  } >FLASH_APP

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH_APP
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH_APP

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH_APP
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH_APP
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH_APP

  /* Non-cacheable region for DMA buffers. The D-cache is enabled (SCB_EnableDCache), and DMA masters
   * write/read SRAM behind the CPU's cache, so any DMA buffer MUST live here or transfers go stale.
   * First in RAM: ORIGIN(RAM)=0x20020000 is already 8K-aligned, so this reserves an 8K MPU region at the
   * base with no padding waste. An MPU region (platform_hal_stm32.cpp) marks it Normal/non-cacheable. */
  .dma_nocache (NOLOAD) :
  {
    . = ALIGN(8K);
    _dma_nocache_start = .;
    *(.dma_nocache) *(.dma_nocache*)
    _dma_nocache_used_end = .;
    . = _dma_nocache_start + 8K;      /* reserve the full 8K so the MPU region size matches exactly */
    _dma_nocache_end = .;
  } >RAM

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data) *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH_APP

  .bss :
  {
    . = ALIGN(4);
    _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .; __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .); PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    /* Stack no longer reserved here — it is in DTCM (see _estack). */
    . = ALIGN(8);
  } >RAM

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}

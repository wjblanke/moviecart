/*
*****************************************************************************
**
**  File        : stm32_flash.ld
**
**  Abstract    : Linker script for STM32F407VG Device with
**                1024KByte FLASH, 128KByte RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**  Environment : Atollic TrueSTUDIO(R)
**
**  Distribution: The file is distributed "as is", without any warranty
**                of any kind.
**
**  (c)Copyright Atollic AB.
**  You may use this file as-is or modify it according to the needs of your
**  project. This file may only be built (assembled or compiled and linked)
**  using the Atollic TrueSTUDIO(R) product. The use of this file together
**  with other tools than Atollic TrueSTUDIO(R) is not permitted.
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/*
 * Stack lives at the top of CCM (D-bus only, not on the AHB matrix).
 *
 * r_coreInfo and romData sit in the same bank so serve-path loads do not share
 * a slave with GPIO (AHB1) or SDIO DMA (SRAM1/SRAM2). DMA cannot reach CCM.
 */
_estack = 0x10010000;    /* top of CCM (64 K) */

/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0;      /* required amount of heap  */
_Min_Stack_Size = 0x400; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 64K
  RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 112K
  RAM2 (xrw)      : ORIGIN = 0x2001C000, LENGTH = 16K
  MEMORY_B1 (rx)  : ORIGIN = 0x60000000, LENGTH = 0K
  CCMRAM (rw)     : ORIGIN = 0x10000000, LENGTH = 64K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /*
   * Atari bus critical path, before optional SD/driver code.
   *
   * Placed immediately after the fixed-size vector table so no optional code can
   * precede it, and every input function is ART-line aligned (see HOTFUNC), so
   * these addresses do not move when --gc-sections drops an unrelated helper or
   * when one hot function changes size.
   */
  .hottext :
  {
    . = ALIGN(16);
    KEEP(*(.hottext))
    KEEP(*(.hottext*))
    . = ALIGN(16);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /*
   * CPU data in CCM (D-bus). DMA cannot reach this bank — keep DMA buffers
   * in .sram1 / .sram2. Reset_Handler copies .data and zeros .bss here.
   */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)

    . = ALIGN(4);
    _edata = .;
  } >CCMRAM AT> FLASH

  _siccmram = LOADADDR(.ccmram);

  .ccmram :
  {
    . = ALIGN(4);
    _sccmram = .;
    *(.ccmram)
    *(.ccmram*)

    . = ALIGN(4);
    _eccmram = .;
  } >CCMRAM AT> FLASH

  .ccmbss (NOLOAD) :
  {
    . = ALIGN(4);
    _sccmbss = .;
    *(.ccmbss)
    *(.ccmbss*)

    . = ALIGN(4);
    _eccmbss = .;
  } >CCMRAM

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(.queueInfo)
    *(.fileSystemInfo)
    *(.updateInfo)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >CCMRAM

  ASSERT(_ebss <= (ORIGIN(CCMRAM) + LENGTH(CCMRAM) - _Min_Stack_Size),
         "CCM overflow: data collided with the stack reservation")

  /*
   * SRAM1 DMA targets only. Executable RAM stubs stay here too — F4 CCM
   * is not on the I-bus.
   */
  .sram1 (NOLOAD) :
  {
    . = ALIGN(16);
    _ssram1 = .;
    *(.sram1)
    *(.sram1*)
    . = ALIGN(16);
    _esram1 = .;
  } >RAM

  .ramfunc :
  {
    . = ALIGN(4);
    *(.ramfunc)
    *(.ramfunc*)
    . = ALIGN(4);
  } >RAM AT> FLASH

  ._user_heap_stack :
  {
    . = ALIGN(4);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = ALIGN(4);
  } >RAM

  /*
   * SRAM2 (0x2001C000, 16 K): mr_buffer2 only. mr_buffer1 and diskBuffer
   * are .sram1 so the two field buffers sit on different AHB slaves.
   */
  .sram2 (NOLOAD) :
  {
    . = ALIGN(16);
    _ssram2 = .;
    *(.sram2)
    *(.sram2*)
    . = ALIGN(16);
    _esram2 = .;
  } >RAM2

  /* MEMORY_bank1 section, code must be located here explicitly            */
  /* Example: extern int foo(void) __attribute__ ((section (".mb1text"))); */
  .memory_b1_text :
  {
    *(.mb1text)        /* .mb1text sections (code) */
    *(.mb1text*)       /* .mb1text* sections (code)  */
    *(.mb1rodata)      /* read-only data (constants) */
    *(.mb1rodata*)
  } >MEMORY_B1

  ._end_of_flash_marker :
  {
    _END_OF_FLASH = .;
  } >FLASH

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

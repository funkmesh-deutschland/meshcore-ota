SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x29000, LENGTH = 0xC1000
  RAM (rwx) : ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

OUTPUT_FORMAT ("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
ENTRY(Reset_Handler)

__StackSize = 1024*2;
PROVIDE(__StackTop = ORIGIN(RAM) + LENGTH(RAM));
PROVIDE(__StackLimit = __StackTop - __StackSize);
PROVIDE(__stack = __StackTop);
__flash_arduino_start = ORIGIN(FLASH);
__flash_arduino_end = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
  .text :
  {
    KEEP(*(.isr_vector))
    *.a:*(.text .text.*)
    KEEP(*(.init))
    KEEP(*(.fini))
    *crtbegin.o(.ctors)
    *crtbegin?.o(.ctors)
    *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
    *(SORT(.ctors.*))
    *(.ctors)
    *crtbegin.o(.dtors)
    *crtbegin?.o(.dtors)
    *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
    *(SORT(.dtors.*))
    *(.dtors)
    *.a:*(.rodata .rodata.*)
  } > FLASH

  .text_app ALIGN(0x1000) :
  {
    *(.text*)
    . = ALIGN(0x100);
    *(.rodata*)
    KEEP(*(.eh_frame*))
  } > FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > FLASH

  __exidx_start = .;
  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > FLASH
  __exidx_end = .;

  __etext = .;

  .data : AT (__etext)
  {
    __data_start__ = .;
    *(vtable)
    *.a:*(.data .data.*)
    *(.data*)
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN (__fini_array_end = .);
    KEEP(*(.jcr*))
    . = ALIGN(4);
    __data_end__ = .;
  } > RAM

  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM

  .bss ALIGN(4):
  {
    . = ALIGN(4);
    __bss_start__ = .;
    *.a:*(.bss .bss.*)
    *.a:*(COMMON)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > RAM

  .heap :
  {
    __HeapBase = .;
    __end__ = .;
    PROVIDE(end = .);
    KEEP(*(.heap*))
    . = ALIGN(__StackLimit);
    __HeapLimit = .;
  } > RAM

  ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed with stack")
  ASSERT(__StackLimit >= __bss_end__, "region RAM overflowed with stack")
}

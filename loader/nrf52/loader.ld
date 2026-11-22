MEMORY
{
  FLASH (rx) : ORIGIN = LOADER_ORIGIN, LENGTH = LOADER_SIZE
  RAM (rwx) : ORIGIN = 0x20006000, LENGTH = 0x3A000
}

ENTRY(reset_handler)

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
  } > FLASH

  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.attributes)
    *(.comment)
  }

  .data (NOLOAD) :
  {
    *(.data*)
  } > RAM

  .bss (NOLOAD) :
  {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  ASSERT(SIZEOF(.data) == 0, "The loader must not have initialised data")
}

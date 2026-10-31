ENTRY(_start)
SECTIONS {
  . = 0x80200000;
  .text : { *(.text) }
  .rodata : { *(.rodata) }
  .bss : { *(.bss) }
}

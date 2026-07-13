/* Linker script for mps2-an385 (Cortex-M3): code RAM at 0x0, data RAM at 0x20000000 */
MEMORY {
  CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00400000
  DATA (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00400000
}

SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > CODE
  .text : { *(.text*) *(.rodata*) } > CODE

  _etext = .;

  .data : {
    _data_start = .;
    *(.data*)
    _data_end = .;
  } > DATA AT > CODE
  _data_load = LOADADDR(.data);

  .bss : {
    _bss_start = .;
    *(.bss*) *(COMMON)
    _bss_end = .;
  } > DATA

  . = ALIGN(8);
  _heap_start = .;
  PROVIDE(end = .);
  _heap_end = ORIGIN(DATA) + LENGTH(DATA) - 0x1000;
  _stack_top = ORIGIN(DATA) + LENGTH(DATA);
}

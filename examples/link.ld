MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x00040000
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x00010000
}
SECTIONS {
  .text : { *(.isr_vector) *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss  : { *(.bss*) *(COMMON) } > RAM
}

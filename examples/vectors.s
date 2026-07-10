.syntax unified
.cpu cortex-m3
.thumb

.section .isr_vector,"a"
.word 0x20008000
.word _start+1

.syntax unified
.cpu cortex-m3
.thumb

.section .isr_vector,"a"
.word 0x20008000
.word _start+1

.section .text
.thumb_func
.global _start
_start:
    bl main
1:  b 1b

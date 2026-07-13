.syntax unified
.cpu cortex-m3
.thumb

.section .isr_vector,"a"
.word _stack_top
.word reset_handler+1

.section .text
.thumb_func
.global reset_handler
reset_handler:
    ldr r0, =_data_load
    ldr r1, =_data_start
    ldr r2, =_data_end
copy_data:
    cmp r1, r2
    bge copy_data_done
    ldr r3, [r0], #4
    str r3, [r1], #4
    b copy_data
copy_data_done:

    movs r0, #0
    ldr r1, =_bss_start
    ldr r2, =_bss_end
zero_bss:
    cmp r1, r2
    bge zero_bss_done
    str r0, [r1], #4
    b zero_bss
zero_bss_done:

    bl main
hang:
    b hang

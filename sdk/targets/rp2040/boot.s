    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .boot2, "ax"
    .align 2
    .global __boot2
__boot2:
    .byte 0x00, 0xb5, 0x32, 0x4b, 0x21, 0x20, 0x58, 0x60, 0x98, 0x68, 0x02, 0x21, 0x88, 0x43, 0x98, 0x60
    .byte 0xd8, 0x60, 0x18, 0x61, 0x58, 0x61, 0x2e, 0x4b, 0x00, 0x21, 0x99, 0x60, 0x02, 0x21, 0x59, 0x61
    .byte 0x01, 0x21, 0xf0, 0x22, 0x99, 0x50, 0x2b, 0x49, 0x19, 0x60, 0x01, 0x21, 0x99, 0x60, 0x35, 0x20
    .byte 0x00, 0xf0, 0x44, 0xf8, 0x02, 0x22, 0x90, 0x42, 0x14, 0xd0, 0x06, 0x21, 0x19, 0x66, 0x00, 0xf0
    .byte 0x34, 0xf8, 0x19, 0x6e, 0x01, 0x21, 0x19, 0x66, 0x00, 0x20, 0x18, 0x66, 0x1a, 0x66, 0x00, 0xf0
    .byte 0x2c, 0xf8, 0x19, 0x6e, 0x19, 0x6e, 0x19, 0x6e, 0x05, 0x20, 0x00, 0xf0, 0x2f, 0xf8, 0x01, 0x21
    .byte 0x08, 0x42, 0xf9, 0xd1, 0x00, 0x21, 0x99, 0x60, 0x1b, 0x49, 0x19, 0x60, 0x00, 0x21, 0x59, 0x60
    .byte 0x1a, 0x49, 0x1b, 0x48, 0x01, 0x60, 0x01, 0x21, 0x99, 0x60, 0xeb, 0x21, 0x19, 0x66, 0xa0, 0x21
    .byte 0x19, 0x66, 0x00, 0xf0, 0x12, 0xf8, 0x00, 0x21, 0x99, 0x60, 0x16, 0x49, 0x14, 0x48, 0x01, 0x60
    .byte 0x01, 0x21, 0x99, 0x60, 0x01, 0xbc, 0x00, 0x28, 0x00, 0xd0, 0x00, 0x47, 0x12, 0x48, 0x13, 0x49
    .byte 0x08, 0x60, 0x03, 0xc8, 0x80, 0xf3, 0x08, 0x88, 0x08, 0x47, 0x03, 0xb5, 0x99, 0x6a, 0x04, 0x20
    .byte 0x01, 0x42, 0xfb, 0xd0, 0x01, 0x20, 0x01, 0x42, 0xf8, 0xd1, 0x03, 0xbd, 0x02, 0xb5, 0x18, 0x66
    .byte 0x18, 0x66, 0xff, 0xf7, 0xf2, 0xff, 0x18, 0x6e, 0x18, 0x6e, 0x02, 0xbd, 0x00, 0x00, 0x02, 0x40
    .byte 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x07, 0x00, 0x00, 0x03, 0x5f, 0x00, 0x21, 0x22, 0x00, 0x00
    .byte 0xf4, 0x00, 0x00, 0x18, 0x22, 0x20, 0x00, 0xa0, 0x00, 0x01, 0x00, 0x10, 0x08, 0xed, 0x00, 0xe0
    .byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0xb2, 0x4e, 0x7a

    .section .vectors, "ax"
    .align 2
    .global __vectors
__vectors:
    .word __stack_top
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word 0
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    .section .text.Reset_Handler, "ax"
    .align 2
    .global Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    ldr r0, =__stack_top
    msr msp, r0
    ldr r1, =_sdata
    ldr r2, =_edata
    ldr r3, =_sidata
copy_data_loop:
    cmp r1, r2
    bge copy_data_done
    ldr r0, [r3]
    str r0, [r1]
    adds r1, #4
    adds r3, #4
    b copy_data_loop
copy_data_done:
    ldr r1, =_sbss
    ldr r2, =_ebss
    movs r0, #0
zero_bss_loop:
    cmp r1, r2
    bge zero_bss_done
    str r0, [r1]
    adds r1, #4
    b zero_bss_loop
zero_bss_done:
    bl Main
hang_loop:
    wfi
    b hang_loop

    .global NMI_Handler
    .thumb_set NMI_Handler, Default_Handler
    .global HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler
    .global SVC_Handler
    .thumb_set SVC_Handler, Default_Handler
    .global PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler
    .global SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler

    .section .text.Default_Handler, "ax"
    .type Default_Handler, %function
Default_Handler:
    b .

    .section .text.putchar, "ax"
    .global putchar
    .type putchar, %function
putchar:
    ldr r1, =0x40034000
wait_txff:
    ldr r2, [r1, #0x18]
    movs r3, #0x20
    tst r2, r3
    bne wait_txff
    str r0, [r1, #0x00]
    bx lr

    .section .text.puts, "ax"
    .global puts
    .type puts, %function
puts:
    push {r4, lr}
    mov r4, r0
.Lputs_loop:
    ldrb r0, [r4]
    cmp r0, #0
    beq .Lputs_newline
    bl putchar
    adds r4, #1
    b .Lputs_loop
.Lputs_newline:
    movs r0, #10
    bl putchar
    movs r0, #0
    pop {r4, pc}

    .section .text.strlen, "ax"
    .global strlen
    .type strlen, %function
strlen:
    movs r1, r0
strlen_loop:
    ldrb r2, [r1]
    cmp r2, #0
    beq strlen_done
    adds r1, #1
    b strlen_loop
strlen_done:
    subs r0, r1, r0
    bx lr

    .section .text.printf, "ax"
    .global printf
    .type printf, %function
printf:
    push {r0, r1, r2, r3, lr}
    mov r4, r0
    mov r5, sp
    adds r5, #4
format_loop:
    ldrb r0, [r4]
    cmp r0, #0
    beq printf_done
    adds r4, #1
    cmp r0, #37
    bne print_normal_char
skip_flags:
    ldrb r0, [r4]
    adds r4, #1
    cmp r0, #108
    beq skip_flags
    cmp r0, #115
    beq print_string
    cmp r0, #99
    beq print_arg_char
    cmp r0, #100
    beq print_int
    cmp r0, #117
    beq print_uint
    cmp r0, #112
    beq print_uint
    cmp r0, #120
    beq print_uint
    b print_normal_char
print_normal_char:
    bl putchar
    b format_loop
print_arg_char:
    ldr r0, [r5]
    adds r5, #4
    bl putchar
    b format_loop
print_string:
    ldr r6, [r5]
    adds r5, #4
print_str_loop:
    ldrb r0, [r6]
    cmp r0, #0
    beq format_loop
    adds r6, #1
    bl putchar
    b print_str_loop
print_int:
    ldr r1, [r5]
    adds r5, #4
    cmp r1, #0
    bge print_pos
    movs r0, #45
    bl putchar
    rsbs r1, r1, #0
print_pos:
    mov r0, r1
    bl print_u32
    b format_loop
print_uint:
    ldr r0, [r5]
    adds r5, #4
    bl print_u32
    b format_loop
printf_done:
    pop {r0, r1, r2, r3, pc}
print_u32:
    push {r4, lr}
    sub sp, #16
    mov r4, sp
    movs r2, #0
    strb r2, [r4, #12]
    adds r4, #11
div_loop:
    movs r1, #0
    movs r3, #10
div_step:
    cmp r0, r3
    blo div_step_done
    subs r0, r3
    adds r1, #1
    b div_step
div_step_done:
    adds r0, #48
    strb r0, [r4]
    subs r4, #1
    mov r0, r1
    cmp r0, #0
    bne div_loop
    adds r4, #1
num_print_loop:
    ldrb r0, [r4]
    cmp r0, #0
    beq num_print_done
    adds r4, #1
    bl putchar
    b num_print_loop
num_print_done:
    add sp, #16
    pop {r4, pc}

    .section .text.__aeabi_uidivmod, "ax"
    .global __aeabi_uidiv
    .global __aeabi_uidivmod
    .global __udivsi3
    .type __udivsi3, %function
    .type __aeabi_uidiv, %function
    .type __aeabi_uidivmod, %function
__udivsi3:
__aeabi_uidiv:
__aeabi_uidivmod:
    cmp r1, #0
    bne .Ldiv_start
    movs r0, #0
    movs r1, #0
    bx lr
.Ldiv_start:
    push {r4, r5, lr}
    movs r2, #0
    movs r3, #0
    movs r4, #32
.Lbit_loop:
    lsls r2, r2, #1
    asrs r5, r0, #31
    lsls r0, r0, #1
    cmp r5, #0
    beq 1f
    adds r2, #1
1:
    cmp r2, r1
    blo 2f
    subs r2, r1
    adds r3, #1
2:
    subs r4, #1
    beq .Ldiv_end
    lsls r3, r3, #1
    b .Lbit_loop
.Ldiv_end:
    mov r0, r3
    mov r1, r2
    pop {r4, r5, pc}

    .section .text.__divsi3, "ax"
    .global __divsi3
    .type __divsi3, %function
    .global __aeabi_idiv
    .type __aeabi_idiv, %function
    .global __aeabi_idivmod
    .type __aeabi_idivmod, %function
__divsi3:
__aeabi_idiv:
__aeabi_idivmod:
    push {r4, r5, lr}
    movs r4, #0
    cmp r0, #0
    bge .Lnum_pos
    rsbs r0, r0, #0
    movs r4, #1
.Lnum_pos:
    cmp r1, #0
    bge .Lden_pos
    rsbs r1, r1, #0
    movs r5, #1
    eors r4, r5
.Lden_pos:
    bl __aeabi_uidivmod
    cmp r4, #0
    beq .Ldiv_signed_done
    rsbs r0, r0, #0
.Ldiv_signed_done:
    pop {r4, r5, pc}

    .section .text.__ashldi3, "ax"
    .global __ashldi3
    .type __ashldi3, %function
__ashldi3:
    push {r4, lr}
    cmp r2, #0
    beq .Lshl_done
    cmp r2, #32
    bhs .Lshl_ge32
    lsls r1, r2
    movs r3, #32
    subs r3, r3, r2
    mov r4, r0
    lsrs r4, r3
    orrs r1, r4
    lsls r0, r2
    b .Lshl_done
.Lshl_ge32:
    cmp r2, #64
    bhs .Lshl_zero
    subs r2, #32
    mov r1, r0
    lsls r1, r2
    movs r0, #0
    b .Lshl_done
.Lshl_zero:
    movs r0, #0
    movs r1, #0
.Lshl_done:
    pop {r4, pc}

    .section .text.__lshrdi3, "ax"
    .global __lshrdi3
    .type __lshrdi3, %function
__lshrdi3:
    push {r4, lr}
    cmp r2, #0
    beq .Llshr_done
    cmp r2, #32
    bhs .Llshr_ge32
    lsrs r0, r2
    movs r3, #32
    subs r3, r3, r2
    mov r4, r1
    lsls r4, r3
    orrs r0, r4
    lsrs r1, r2
    b .Llshr_done
.Llshr_ge32:
    cmp r2, #64
    bhs .Llshr_zero
    subs r2, #32
    mov r0, r1
    lsrs r0, r2
    movs r1, #0
    b .Llshr_done
.Llshr_zero:
    movs r0, #0
    movs r1, #0
.Llshr_done:
    pop {r4, pc}

    .section .text.__divdi3, "ax"
    .global __divdi3
    .type __divdi3, %function
__divdi3:
    push {r4, lr}
    mov r4, r1
    orrs r4, r3
    cmp r4, #0
    bne .Ldiv64_full
    mov r1, r2
    bl __aeabi_uidiv
    movs r1, #0
    pop {r4, pc}
.Ldiv64_full:
    pop {r4, pc}

    .section .text.__moddi3, "ax"
    .global __moddi3
    .type __moddi3, %function
__moddi3:
    push {r4, lr}
    mov r4, r1
    orrs r4, r3
    cmp r4, #0
    bne .Lmod64_full
    mov r1, r2
    bl __aeabi_uidivmod
    mov r0, r1
    movs r1, #0
    pop {r4, pc}
.Lmod64_full:
    pop {r4, pc}

    .section .text.__muldi3, "ax"
    .global __muldi3
    .type __muldi3, %function
__muldi3:
    push {r4, r5, r6, lr}
    mov r4, r1
    muls r4, r2
    mov r5, r3
    muls r5, r0
    adds r4, r4, r5
    movs r1, #255
    lsls r1, r1, #8
    adds r1, #255
    mov r3, r2
    ands r3, r1
    lsrs r2, r2, #16
    mov r5, r0
    ands r5, r1
    lsrs r0, r0, #16
    mov r6, r5
    muls r6, r3
    mov r1, r0
    muls r1, r3
    muls r5, r2
    muls r0, r2
    adds r1, r1, r5
    lsrs r5, r1, #16
    adds r4, r4, r5
    adds r4, r4, r0
    lsls r1, r1, #16
    adds r0, r6, r1
    movs r2, #0
    adcs r4, r2
    mov r1, r4
    pop {r4, r5, r6, pc}

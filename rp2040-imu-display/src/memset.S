.syntax unified
.cpu cortex-m0plus
.thumb

.global memset
.type memset, %function
.thumb_func
memset:
    mov r3, r0
    cmp r2, #0
    beq 2f
1:
    strb r1, [r3]
    adds r3, #1
    subs r2, #1
    bne 1b
2:
    bx lr
.size memset, .-memset

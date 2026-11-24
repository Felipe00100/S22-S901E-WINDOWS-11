    AREA |.text|, CODE, READONLY, ALIGN=4

    EXPORT S22SmcCall64

S22SmcCall64 PROC
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    stp     x19, xzr, [sp, #16]
    mov     x19, x0

    ldp     x0, x1, [x19, #0]
    ldp     x2, x3, [x19, #16]
    ldp     x4, x5, [x19, #32]
    ldp     x6, x7, [x19, #48]

    smc     #0

    stp     x0, x1, [x19, #0]
    stp     x2, x3, [x19, #16]
    stp     x4, x5, [x19, #32]
    stp     x6, x7, [x19, #48]

    ldp     x19, xzr, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    ENDP

    END

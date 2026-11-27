; A loop closed by JL, then one closed by JGE, each 100 times. Exit status is
; the number of loops that ran another count: an unrolled JL/JGE loop once left
; through its head instead of the next instruction, so it ran until the guard.
;   nasm -f bin jlloop.asm -o jlloop.com
        org     0x100
        cpu     386
        mov     cx, 100                 ; the program's first loop was not unrolled
.warm:  loop    .warm
        xor     di, di
        xor     bx, bx
        xor     cx, cx
.jl:    inc     bx
        cmp     bx, 1000                ; guard: ends the loop if JL never falls through
        ja      .jl_end
        inc     cx
        cmp     cx, 100
        jl      .jl
.jl_end:
        cmp     bx, 100
        je      .jge_start
        inc     di
.jge_start:
        xor     bx, bx
        mov     cx, 99
.jge:   inc     bx
        cmp     bx, 1000
        ja      .jge_end
        dec     cx
        cmp     cx, 0
        jge     .jge
.jge_end:
        cmp     bx, 100
        je      .done
        inc     di
.done:  mov     dx, 0x501
        mov     ax, di
        out     dx, al
        mov     ax, 0x4c00
        int     0x21

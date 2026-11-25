; HLPROF n: 1 restarts the profile's counts, 0 stops counting, so a batch file
; samples one program alone:  HLPROF 1, PROG.COM, HLPROF 0
;   nasm -f bin hlprof.asm -o HLPROF.COM
        org     0x100
        cpu     386
        mov     si, 0x81
        xor     bx, bx
.skip:  lodsb
        cmp     al, ' '
        je      .skip
.digit: sub     al, '0'
        cmp     al, 9
        ja      .out
        mov     ah, 0
        imul    bx, bx, 10
        add     bx, ax
        lodsb
        jmp     .digit
.out:   mov     dx, 0x502
        mov     ax, bx
        out     dx, al
        mov     ax, 0x4c00
        int     0x21

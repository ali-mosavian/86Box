; HLEXIT n: ends the emulator with exit status n (decimal), so a batch file can
; finish a run:  PROG.COM  then  HLEXIT 0
;   nasm -f bin hlexit.asm -o HLEXIT.COM
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
.out:   mov     dx, 0x501
        mov     ax, bx
        out     dx, al
        mov     ax, 0x4c00
        int     0x21

; FTST of 1.0 right after a load of +Inf, 100 times. Exit status 1 when any
; FTST reported other than "greater than zero": the arm64 recompiler made its
; zero as scratch - scratch, NaN when the scratch register still held the
; infinity, so FTST read unordered.
;   nasm -f bin ftstinf.asm -o ftstinf.com
        org     0x100
        cpu     386
        xor     di, di
        mov     cx, 100
        fninit
.again: fld     qword [one]
        fld     qword [inf]
        fstp    st0
        ftst
        fnstsw  ax
        fstp    st0
        and     ax, 0x4500              ; C3, C2, C0
        or      di, ax
        loop    .again
        mov     dx, 0x501
        mov     ax, di
        test    ax, ax
        setnz   al
        out     dx, al
        mov     ax, 0x4c00
        int     0x21
one:    dq      1.0
inf:    dq      0x7ff0000000000000

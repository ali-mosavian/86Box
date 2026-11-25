; Prints "SHORT <hex linear addr> LONG <hex> END <hex>" to port 0xE9, then
; loops forever: a two-instruction block, then a long block of dependent
; imuls that ends in a jump back. Used by selftest.py to check that profiler
; samples land in the long block, where the time goes.
;   nasm -f bin blocks.asm -o BLOCKS.COM
        org     0x100
        cpu     386
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        mov     ebp, eax
        mov     si, s_short
        call    puts
        mov     eax, short_
        add     eax, ebp
        call    hex
        mov     si, s_long
        call    puts
        mov     eax, long_
        add     eax, ebp
        call    hex
        mov     si, s_end
        call    puts
        mov     eax, end_
        add     eax, ebp
        call    hex
        mov     al, 10
        out     0xe9, al
        mov     ebx, 3
short_: inc     ax
        jmp     long_
long_:
        %rep 16
        imul    ebx, ebx
        %endrep
        jmp     short_
end_:

puts:   lodsb
        test    al, al
        jz      .r
        out     0xe9, al
        jmp     puts
.r:     ret

hex:    mov     cx, 8
.n:     rol     eax, 4
        push    ax
        and     al, 15
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 7
.p:     out     0xe9, al
        pop     ax
        loop    .n
        ret

s_short: db     'SHORT ', 0
s_long: db      ' LONG ', 0
s_end:  db      ' END ', 0

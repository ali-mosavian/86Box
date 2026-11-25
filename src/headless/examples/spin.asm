; Prints "LOOP <hex linear addr> DATA <hex linear addr>" to port 0xE9, then
; spins in a two-byte loop at the first address. DATA is a 16-byte marker.
; Used by selftest.py for profile and mem_read.
;   nasm -f bin spin.asm -o SPIN.COM
        org     0x100
        cpu     386
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        mov     ebp, eax
        mov     si, s_loop
        call    puts
        mov     eax, spin
        add     eax, ebp
        call    hex
        mov     si, s_data
        call    puts
        mov     eax, data
        add     eax, ebp
        call    hex
        mov     al, 10
        out     0xe9, al
spin:   jmp     short spin

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

s_loop: db      'LOOP ', 0
s_data: db      ' DATA ', 0
data:   db      'HL86-MEMREAD-OK', 0

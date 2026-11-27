; ACPI power-on suspend through the PIIX4 (8086:7113, function 3), then back.
; Prints SLEEP, sleeps, prints WOKE and exits 0; exit 2 without a PIIX4. The
; CPU runs on to the end of its 1 ms block before it stops, hence the spin.
;   nasm -f bin acpisl.asm -o acpisl.com
        org     0x100
        cpu     386

PM      equ     0xe400

        mov     ebx, 0x80000300         ; bus 0, device 0, function 3
.scan:  mov     eax, ebx
        call    cfg_read
        cmp     eax, 0x71138086
        je      .found
        add     ebx, 0x800
        cmp     ebx, 0x80010000
        jb      .scan
        mov     al, 2
        jmp     exit
.found: lea     eax, [ebx + 0x40]       ; PMBA
        mov     dx, 0xcf8
        out     dx, eax
        mov     dx, 0xcfc
        mov     eax, PM | 1
        out     dx, eax
        lea     eax, [ebx + 0x80]       ; PMREGMISC: PMIOSE
        call    cfg_read
        or      al, 1
        out     dx, al
        mov     si, s_sleep
        call    puts
        mov     dx, PM + 5
        mov     al, 0x30                ; SLP_EN, SLP_TYP 4: power-on suspend
        out     dx, al
        mov     ecx, 2000000
.spin:  loop    .spin, ecx
        mov     si, s_woke
        call    puts
        xor     al, al
exit:   mov     dx, 0x501
        out     dx, al
        mov     ax, 0x4c00
        int     0x21

cfg_read:
        mov     dx, 0xcf8
        out     dx, eax
        mov     dx, 0xcfc
        in      eax, dx
        ret

puts:   lodsb
        test    al, al
        jz      .r
        out     0xe9, al
        jmp     puts
.r:     ret

s_sleep: db "SLEEP", 10, 0
s_woke:  db "WOKE", 10, 0

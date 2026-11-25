; Writes a line to the Bochs debug port (0xE9), then ends the emulator with
; exit status 7 through the headless exit port (0x501).
;   nasm -f bin hello.asm -o HELLO.COM
        org     0x100
        cpu     386
        mov     si, msg
.next:  lodsb
        test    al, al
        jz      .done
        out     0xe9, al
        jmp     .next
.done:  mov     dx, 0x501
        mov     al, 7
        out     dx, al
        mov     ax, 0x4c00              ; not reached when the port is wired
        int     0x21
msg:    db      'hello from the guest', 10, 0

; Moves the CRTC start address down one row and writes TOPLINE into what is now
; the top row, then hangs. Used by selftest.py: the screen must read back as TOPLINE.
;   nasm -f bin crtcstart.asm -o CRTCSTART.COM
        org     0x100
        cpu     386
        mov     dx, 0x3d4
        mov     ax, 0x000c              ; start address high = 0
        out     dx, ax
        mov     ax, 0x500d              ; start address low = 80 (one row)
        out     dx, ax
        mov     ax, 0xb800
        mov     es, ax
        mov     di, 160                 ; row 1 in memory is row 0 on screen
        mov     si, msg
.put:   lodsb
        test    al, al
        jz      .done
        mov     ah, 0x07
        stosw
        jmp     .put
.done:  jmp     .done                   ; stay up until the host has read the screen
msg:    db      'TOPLINE', 0

; Times 16x unrolled loops of "imul eax,ebx" and "shl eax,1" and prints the
; counts to the Bochs debug port (0xE9), then exits the emulator (port 0x501).
; Every measurement runs three times, so a first-pass effect shows up.
;
;   pit  lines: elapsed PIT channel-0 ticks (1.193182 MHz) for 64000 ops, all CPUs
;   tsc  lines: elapsed CPU cycles by RDTSC for 320000 ops, only where CPUID
;               reports a TSC
;
;   nasm -f bin imulshl.asm -o IMULSHL.COM
        org     0x100
        cpu     586

; Each pass runs ITERS x 16 ops. The 16-bit PIT count wraps every 54.9 ms, so
; the PIT passes are short: an imul at ~32 cycles on a 66 MHz 486 takes ~31 ms.
ITERS_PIT equ   4000
ITERS_TSC equ   20000

start:
        cli
        call    detect_tsc
        mov     al, 0x34                ; channel 0, mode 2, 16-bit: counts down by 1
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al

        mov     byte [round], 3
.round: mov     si, s_pit_imul
        call    puts
        call    time_imul_pit
        call    print_dec
        call    nl
        mov     si, s_pit_shl
        call    puts
        call    time_shl_pit
        call    print_dec
        call    nl
        cmp     byte [have_tsc], 0
        je      .notsc
        mov     si, s_tsc_imul
        call    puts
        call    time_imul_tsc
        call    print_dec
        call    nl
        mov     si, s_tsc_shl
        call    puts
        call    time_shl_tsc
        call    print_dec
        call    nl
.notsc: dec     byte [round]
        jnz     .round

        mov     al, 0x36                ; put channel 0 back to mode 3 for DOS
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        sti
        mov     dx, 0x501
        xor     al, al
        out     dx, al
        mov     ax, 0x4c00
        int     0x21

; --- CPUID / TSC detection -------------------------------------------------
detect_tsc:
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 0x200000           ; ID bit flips only if CPUID exists
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax, ecx
        test    eax, 0x200000
        jz      .no
        xor     eax, eax
        cpuid
        test    eax, eax
        jz      .no
        mov     eax, 1
        cpuid
        test    edx, 0x10
        jz      .no
        mov     byte [have_tsc], 1
.no:    ret

; --- PIT ---------------------------------------------------------------------
; Returns the current channel-0 count in ax.
pit_read:
        xor     al, al
        out     0x43, al
        in      al, 0x40
        mov     ah, al
        in      al, 0x40
        xchg    al, ah
        ret

%macro  IMUL16 0
        %rep    16
        imul    eax, ebx
        %endrep
%endmacro

%macro  SHL16 0
        %rep    16
        shl     eax, 1
        %endrep
%endmacro

%macro  PIT_TIMED 1
        mov     ebx, 3
        mov     eax, 1
        mov     ecx, ITERS_PIT
        call    pit_read
        mov     di, ax
        align   16
%%l:    %1
        dec     ecx
        jnz     %%l
        call    pit_read
        sub     di, ax                  ; counts down: start - end
        movzx   eax, di
%endmacro

time_imul_pit:
        PIT_TIMED IMUL16
        ret
time_shl_pit:
        PIT_TIMED SHL16
        ret

%macro  TSC_TIMED 1
        mov     ebx, 3
        mov     eax, 1
        mov     ecx, ITERS_TSC
        rdtsc
        mov     ebp, eax
        align   16
%%l:    %1
        dec     ecx
        jnz     %%l
        rdtsc
        sub     eax, ebp
%endmacro

time_imul_tsc:
        TSC_TIMED IMUL16
        ret
time_shl_tsc:
        TSC_TIMED SHL16
        ret

; --- output through port 0xE9 -----------------------------------------------
puts:   lodsb
        test    al, al
        jz      .r
        out     0xe9, al
        jmp     puts
.r:     ret

nl:     mov     al, 10
        out     0xe9, al
        ret

print_dec:                              ; eax, unsigned
        mov     ebx, 10
        xor     cx, cx
.d:     xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .d
.p:     pop     ax
        add     al, '0'
        out     0xe9, al
        loop    .p
        ret

round:      db 0
have_tsc:   db 0
s_pit_imul: db 'pit imul ', 0
s_pit_shl:  db 'pit shl  ', 0
s_tsc_imul: db 'tsc imul ', 0
s_tsc_shl:  db 'tsc shl  ', 0

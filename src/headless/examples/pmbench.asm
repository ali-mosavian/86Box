; Micro-benchmark in 32-bit flat protected mode (no 66h/67h prefixes). It times
; variant A and variant B of a loop body and prints, over port 0xE9:
;     <tsc|pit> A=<elapsed> B=<elapsed> N=<iterations>
; three times, then exits the emulator through port 0x501. The elapsed value is
; in RDTSC cycles, or in PIT ticks (1.193182 MHz) where CPUID reports no TSC.
; Choose the test with -DTEST=n:
;   1  A: dependent imul chain           B: independent imuls        (8 imuls)
;   2  A: mov al,[m] ; add ebx,eax       B: movzx eax,byte [m] ; add (4x)
;   3  A: mov [esi],ebx ; movzx eax,word [esi+2]   B: ... mov eax,[esi]  (4x)
;   4  A: alternating jz                 B: always-taken jz          (4 branches)
;   5  A: mov esi,ebx ; mov eax,[esi] ; add     B: add placed between  (4x)
;   6  A: mov eax,[esi+edi]   B: mov eax,[esi]  (8x)
;   7  A: jz on an xorshift bit   B: jz never taken
;   8  A: fdiv + 40 independent adds   B: fdiv alone
;   9  A: fdiv ; fdiv   B: fdiv alone
;   nasm -f bin -DTEST=1 pmbench.asm -o pmimul.com
        org     0x100
        bits    16
        cpu     586

N       equ     2000                    ; PIT-safe: < 54.9 ms even at 40 cycles an iteration

start:
        cli
        call    detect_tsc
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        mov     [base], eax
        mov     [gdt_code + 2], ax
        mov     [gdt_data + 2], ax
        shr     eax, 16
        mov     [gdt_code + 4], al
        mov     [gdt_data + 4], al
        mov     eax, [base]
        add     eax, gdt
        mov     [gdtr + 2], eax
        lgdt    [gdtr]
        mov     al, 0x34                ; PIT channel 0: mode 2, counts down by 1
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        db      0x66, 0xea              ; jmp far 0x08:pm
        dd      pm
        dw      0x08

        bits    32
pm:     mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     esp, 0xfff0
        mov     ebp, 3
.round: call    measure
        dec     ebp
        jnz     .round
        mov     dx, 0x501
        xor     al, al
        out     dx, al
.hang:  jmp     .hang

; Reads the clock into eax: RDTSC, or the negated PIT count so that later minus earlier is elapsed.
%macro  CLOCK 0
        cmp     byte [have_tsc], 0
        je      %%pit
        rdtsc
        jmp     %%done
%%pit:  xor     eax, eax
        out     0x43, al
        in      al, 0x40
        mov     ah, al
        in      al, 0x40
        xchg    al, ah
        movzx   eax, ax
        neg     eax
%%done:
%endmacro

%macro  SETUP 0
        mov     ebx, buf
        mov     esi, buf
        xor     eax, eax
        mov     edx, 1
        mov     edi, 1
%if TEST >= 8
        fninit
        fld1
        fld1
%endif
%endmacro

%if TEST = 1
%macro  BODY_A 0
        %rep 8
        imul    eax, ebx
        %endrep
%endmacro
%macro  BODY_B 0
        %rep 2
        imul    eax, ebx
        imul    edx, ebx
        imul    esi, ebx
        imul    edi, ebx
        %endrep
%endmacro
%elif TEST = 2
%macro  BODY_A 0
        %rep 4
        mov     al, [buf]
        add     ebx, eax
        %endrep
%endmacro
%macro  BODY_B 0
        %rep 4
        movzx   eax, byte [buf]
        add     ebx, eax
        %endrep
%endmacro
%elif TEST = 3
%macro  BODY_A 0
        %rep 4
        mov     [esi], ebx
        movzx   eax, word [esi + 2]
        add     ebx, eax
        %endrep
%endmacro
%macro  BODY_B 0
        %rep 4
        mov     [esi], ebx
        mov     eax, [esi]
        add     ebx, eax
        %endrep
%endmacro
%elif TEST = 4
%macro  BR_A 0
        test    dl, 1
        jz      %%s
        nop
%%s:
%endmacro
%macro  BR_B 0
        test    eax, eax
        jz      %%s
        nop
%%s:
%endmacro
%macro  BODY_A 0
        inc     edx
        %rep 4
        BR_A
        %endrep
%endmacro
%macro  BODY_B 0
        inc     edx
        %rep 4
        BR_B
        %endrep
%endmacro
%elif TEST = 5
%macro  BODY_A 0
        %rep 4
        mov     esi, ebx
        mov     eax, [esi]
        add     edx, 1
        %endrep
%endmacro
%macro  BODY_B 0
        %rep 4
        mov     esi, ebx
        add     edx, 1
        mov     eax, [esi]
        %endrep
%endmacro
%elif TEST = 6
%macro  BODY_A 0
        xor     edi, edi
        %rep 8
        mov     eax, [esi + edi]
        %endrep
%endmacro
%macro  BODY_B 0
        xor     edi, edi
        %rep 8
        mov     eax, [esi]
        %endrep
%endmacro
%elif TEST = 7
%macro  XORSHIFT 0
        mov     eax, edi
        shl     eax, 13
        xor     edi, eax
        mov     eax, edi
        shr     eax, 17
        xor     edi, eax
        mov     eax, edi
        shl     eax, 5
        xor     edi, eax
%endmacro
%macro  BODY_A 0
        XORSHIFT
        test    edi, 1
        jz      %%s
        nop
%%s:
%endmacro
%macro  BODY_B 0
        XORSHIFT
        test    ebx, ebx
        jz      %%s
        nop
%%s:
%endmacro
%elif TEST = 8
%macro  ADDS 0
        %rep 10
        add     eax, 1
        add     ebx, 1
        add     edx, 1
        add     edi, 1
        %endrep
%endmacro
%macro  BODY_A 0
        fdiv    st0, st1
        ADDS
%endmacro
%macro  BODY_B 0
        fdiv    st0, st1
%endmacro
%elif TEST = 9
%macro  BODY_A 0
        fdiv    st0, st1
        fdiv    st0, st1
%endmacro
%macro  BODY_B 0
        fdiv    st0, st1
%endmacro
%endif

measure:
        SETUP
        mov     ecx, N
        CLOCK
        mov     [t0], eax
        align   16
.la:    BODY_A
        dec     ecx
        jnz     .la
        CLOCK
        sub     eax, [t0]
        mov     [res_a], eax
        SETUP
        mov     ecx, N
        CLOCK
        mov     [t0], eax
        align   16
.lb:    BODY_B
        dec     ecx
        jnz     .lb
        CLOCK
        sub     eax, [t0]
        mov     [res_b], eax
        mov     esi, s_unit_tsc
        cmp     byte [have_tsc], 0
        jne     .u
        mov     esi, s_unit_pit
.u:     call    puts
        mov     esi, s_a
        call    puts
        mov     eax, [res_a]
        call    print_dec
        mov     esi, s_b
        call    puts
        mov     eax, [res_b]
        call    print_dec
        mov     esi, s_n
        call    puts
        mov     eax, N
        call    print_dec
        mov     al, 10
        out     0xe9, al
        ret

puts:   lodsb
        test    al, al
        jz      .r
        out     0xe9, al
        jmp     puts
.r:     ret

print_dec:
        mov     ebx, 10
        xor     ecx, ecx
.d:     xor     edx, edx
        div     ebx
        push    edx
        inc     ecx
        test    eax, eax
        jnz     .d
.p:     pop     eax
        add     al, '0'
        out     0xe9, al
        loop    .p
        ret

        bits    16
detect_tsc:
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 0x200000
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

        align   8
gdt:    dq      0
gdt_code: dw    0xffff, 0
        db      0, 0x9a, 0xcf, 0
gdt_data: dw    0xffff, 0
        db      0, 0x92, 0xcf, 0
gdtr:   dw      23
        dd      0
base:       dd  0
have_tsc:   db  0
t0:         dd  0
res_a:      dd  0
res_b:      dd  0
s_unit_tsc: db  'tsc', 0
s_unit_pit: db  'pit', 0
s_a:        db  ' A=', 0
s_b:        db  ' B=', 0
s_n:        db  ' N=', 0
        align   16
buf:        times 64 db 0

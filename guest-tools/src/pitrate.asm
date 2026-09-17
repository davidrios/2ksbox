; pitrate.asm — the PIT at 1 kHz, counted by the guest: IRQ 0 is what a
; 1 kHz guest clock is made of, and this asks whether all of them arrive.
;
;   PITRATE.COM [H] [n]    DOS real mode; n lines (10 by default), H to
;                          halt between ticks instead of spinning
;
; Windows 9x's multimedia timer reprograms counter 0 to 1 ms for
; timeBeginPeriod(1) — what a MIDI sequencer and many games run on — and
; then keeps time by counting IRQ 0. So does this program: counter 0 in
; mode 2 at 1193 (1000.15 Hz), an INT 8 handler of its own that counts and
; acknowledges, and one "T" line on COM1 per 1000 ticks, written straight to
; the UART. The guest prints nothing else, so the *host* is the clock: it
; timestamps the lines as they arrive, and 1.000 s between two of them is a
; guest clock at 100 %. `tools/pit-guest-test.py` reads them.
;
; Why (2026-09-17): QEMU raises every transition that came due while the
; main loop slept back to back, and on the edge-triggered 8259 those are
; one interrupt — so the guest's clock ran at the rate of the host's
; wakeups: 50 % with 2 ms waits, 6 % with Windows' default 15.6 ms ones
; (MIDI "too slow" on a Windows host). Patch 65 reinjects them.
;
; The data sits a page past the code: the handler writes the tick count
; every millisecond, and a store into a code page is TCG's self-modifying-
; code path.
        org 100h
        cpu 386
start:  mov si, 81h
        xor bp, bp               ; bp = 1: hlt between ticks
        mov word [want], 0
.arg:   lodsb
        cmp al, 13
        je .go
        cmp al, 'H'
        jne .num
        mov bp, 1
        jmp .arg
.num:   cmp al, '0'
        jb .arg
        cmp al, '9'
        ja .arg
        sub al, '0'
        movzx cx, al
        mov ax, [want]
        mov dx, 10
        mul dx
        add ax, cx
        mov [want], ax
        jmp .arg
.go:    cmp word [want], 0
        jne .uart
        mov word [want], 10
.uart:  ; UART: 115200 8N1
        mov dx, 3FBh
        mov al, 80h
        out dx, al
        mov dx, 3F8h
        mov al, 1
        out dx, al
        inc dx
        xor al, al
        out dx, al
        mov dx, 3FBh
        mov al, 3
        out dx, al
        ; hook INT 8
        xor ax, ax
        mov es, ax
        cli
        mov ax, [es:8*4]
        mov [old8], ax
        mov ax, [es:8*4+2]
        mov [old8+2], ax
        mov word [es:8*4], isr
        mov [es:8*4+2], cs
        mov al, 34h
        out 43h, al
        mov al, 169              ; 1193 = 0x04A9
        out 40h, al
        mov al, 4
        out 40h, al
        sti
        mov word [lines], 0
        mov ebx, 1000
.loop:  cmp bp, 1
        jne .busy
        hlt
        jmp .chk
.busy:  mov cx, 2000
        mov di, buf
.b1:    mov [di], cl
        inc di
        cmp di, buf+256
        jb .b2
        mov di, buf
.b2:    loop .b1
.chk:   mov eax, [ticks]
        cmp eax, ebx
        jb .loop
        add ebx, 1000
        mov al, 'T'
        call putc
        mov al, 13
        call putc
        mov al, 10
        call putc
        inc word [lines]
        mov ax, [lines]
        cmp ax, [want]
        jb .loop
        ; restore
        cli
        mov al, 34h
        out 43h, al
        xor al, al
        out 40h, al
        out 40h, al
        xor ax, ax
        mov es, ax
        mov ax, [old8]
        mov [es:8*4], ax
        mov ax, [old8+2]
        mov [es:8*4+2], ax
        sti
        mov si, done
.d:     lodsb
        or al, al
        jz .x
        call putc
        jmp .d
.x:     mov ax, 4C00h
        int 21h

putc:   push dx
        push ax
        mov dx, 3FDh
.w:     in al, dx
        test al, 20h
        jz .w
        pop ax
        mov dx, 3F8h
        out dx, al
        pop dx
        ret

isr:    inc dword [cs:ticks]
        push ax
        mov al, 20h
        out 20h, al
        pop ax
        iret

        times 4096-($-start) db 0   ; data off the code page (TCG SMC)
buf:    times 256 db 0
ticks:  dd 0
old8:   dd 0
lines:  dw 0
want:   dw 0
done:   db 'DONE', 13, 10, 0

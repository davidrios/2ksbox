; padtest.asm — the gameport at 0x201 as a DOS game reads it (M13 path B,
; docs/tracks/m13-gamepads.md).
;
;   PADTEST.COM        DOS real mode: FreeDOS, a Win98 "Restart in MS-DOS
;                      mode" screen, or the harness
;                      (tools/pad-guest-test.py).
;
; DOS is the family the USB pad cannot reach — no USB stack — so this port
; is the only controller a DOS game can have, and it is read by the game
; itself: nothing is installed, there is no driver and no API. What that
; reading looks like is the whole of this program, and it is what the
; hardware forces on everybody:
;
;   * one write to 0x201 (any value) triggers four RC one-shots;
;   * bits 0-3 of a read stay *set* while each axis is still charging;
;   * bits 4-7 are the four buttons, *clear* while held;
;   * so an axis is a *count* — how many times the loop went round before
;     the bit fell — and it is in units of nothing at all: a faster CPU
;     counts higher for the same stick position, which is why every game
;     of the era had a calibration step.
;
; A sample is therefore one arm and one counting loop, with interrupts off
; so the BIOS tick cannot land inside the count. The four counts and the
; button nibble go to COM1 (the harness reads them there) *and* to the DOS
; console, so the same program is useful by hand in a machine that has no
; serial line.
;
; The presence test is the era's own and worth keeping in mind when a
; guest says there is no joystick: an idle port reads 0xf0 here — the
; one-shots expired, no button held — while a machine with *no* gameport
; reads 0xff off the open bus. The low nibble is the answer, and it is
; only meaningful when nothing was armed.
;
;   ESC   stop early
;
; nasm -f bin -o PADTEST.COM padtest.asm
;
; SPDX-License-Identifier: GPL-2.0-or-later

        cpu     386
        bits    16
        org     0x100

PORT    equ     0x201
; The counting loop's cap. An unthrottled TCG guest runs the loop far
; faster than any period CPU and would otherwise count past a 16-bit
; counter on the long end of the axis (~1.1 ms), so the counts are 32-bit
; and this only stops a runaway when nothing ever clears the bit — a
; missing device, or a model that arms and never expires.
LIMIT   equ     2000000
; Samples, and the gap between them in BIOS ticks (~55 ms). 480 x 4 ticks
; is about 105 s: longer than the harness needs (it stops the machine as
; soon as it has seen every pose, which takes about half a minute) and
; short enough that a by-hand run ends on its own.
SAMPLES equ     480
GAP     equ     4

start:
        mov     si, str_banner
        call    puts

        ; Is anything decoding the port at all? Nothing has been armed
        ; yet, so every axis bit must be clear. Read twice a tick apart
        ; before believing otherwise: a stray 1 here would otherwise be
        ; reported as an absent port.
        mov     dx, PORT
        in      al, dx
        mov     [idle], al
        test    al, 0x0f
        jz      .present
        mov     cx, 1
        call    wait_ticks
        mov     dx, PORT
        in      al, dx
        mov     [idle], al
        test    al, 0x0f
        jnz     .noport
.present:
        mov     si, str_port
        call    puts
        mov     al, [idle]              ; puts clobbers al
        call    put_hex8
        call    newline

        mov     word [samples], SAMPLES
.loop:
        call    read_pad
        call    print_line
        mov     cx, GAP
        call    wait_ticks
        ; ESC ends a by-hand run; the harness stops the machine itself.
        mov     ah, 0x01
        int     0x16
        jz      .nokey
        mov     ah, 0x00
        int     0x16
        cmp     al, 27
        je      .done
.nokey:
        dec     word [samples]
        jnz     .loop
.done:
        mov     si, str_done
        call    puts
        mov     ax, 0x4c00
        int     0x21

.noport:
        mov     si, str_noport
        call    puts
        mov     al, [idle]
        call    put_hex8
        call    newline
        mov     si, str_done
        call    puts
        mov     ax, 0x4c01
        int     0x21

; ---------------------------------------------------------------- sample

; One arm and one count, into cnt[4] and btn. Interrupts off for the
; duration: at a period pace the whole thing is about a millisecond, and a
; timer tick inside the loop would add its own service time to whichever
; axis was still charging — which reads as a stick that jumps.
read_pad:
        pushad
        pushf
        cli
        xor     eax, eax
        mov     [cnt + 0], eax
        mov     [cnt + 4], eax
        mov     [cnt + 8], eax
        mov     [cnt + 12], eax
        mov     dx, PORT
        out     dx, al                  ; the value is ignored; the write is the trigger
        mov     ecx, LIMIT
.spin:
        in      al, dx
        test    al, 0x0f
        jz      .ended
        test    al, 0x01
        jz      .n0
        inc     dword [cnt + 0]
.n0:    test    al, 0x02
        jz      .n1
        inc     dword [cnt + 4]
.n1:    test    al, 0x04
        jz      .n2
        inc     dword [cnt + 8]
.n2:    test    al, 0x08
        jz      .n3
        inc     dword [cnt + 12]
.n3:    dec     ecx
        jnz     .spin
        ; Fell out on the cap: say so rather than printing a count that
        ; means "we gave up".
        mov     byte [capped], 1
.ended:
        ; The buttons come from a read of their own. The one that ended
        ; the loop would do, but not the one that hit the cap.
        in      al, dx
        mov     [btn], al
        popf
        popad
        ret

print_line:
        mov     si, str_x
        call    puts
        mov     eax, [cnt + 0]
        call    put_dec32
        mov     si, str_y
        call    puts
        mov     eax, [cnt + 4]
        call    put_dec32
        mov     si, str_z
        call    puts
        mov     eax, [cnt + 8]
        call    put_dec32
        mov     si, str_r
        call    puts
        mov     eax, [cnt + 12]
        call    put_dec32
        mov     si, str_b
        call    puts
        mov     al, [btn]
        call    put_hex8
        cmp     byte [capped], 0
        je      .out
        mov     byte [capped], 0
        mov     si, str_capped
        call    puts
.out:
        call    newline
        ret

; --------------------------------------------------------------- helpers

; cx BIOS ticks (~55 ms each), off the counter at 0040:006c.
wait_ticks:
        pushad
        push    es
        xor     ax, ax
        mov     es, ax
.next:
        mov     bx, [es:0x46c]
.spin:  mov     ax, [es:0x46c]
        cmp     ax, bx
        je      .spin
        loop    .next
        pop     es
        popad
        ret

; al -> COM1 and the DOS console. The serial half is what the harness
; reads; the console half is what makes this usable in a real DOS box.
; A machine with no COM1 reads 0xff from the line status and falls
; straight through rather than hanging.
putc:
        pushad
        mov     bl, al                  ; the character, across everything below
        mov     dx, 0x3fd
        mov     cx, 0xffff              ; bounded: a line that never drains
.wait:  in      al, dx                  ; must not take the program with it
        test    al, 0x20                ; THR empty — and 0xff (no UART at
        jnz     .send                   ; all) has that bit set too
        loop    .wait
.send:  mov     al, bl
        mov     dx, 0x3f8
        out     dx, al
        mov     dl, bl
        mov     ah, 0x02
        int     0x21
        popad
        ret

puts:                                   ; si = asciiz
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     puts
.done:  ret

put_hex8:
        pushad
        push    ax
        shr     al, 4
        call    put_nib
        pop     ax
        call    put_nib
        popad
        ret

put_nib:
        and     al, 0x0f
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 'a' - '0' - 10
.out:   call    putc
        ret

; eax as decimal, no padding.
put_dec32:
        pushad
        mov     di, decbuf + 11
        mov     byte [di], 0
        mov     ebx, 10
.digit:
        xor     edx, edx
        div     ebx
        add     dl, '0'
        dec     di
        mov     [di], dl
        test    eax, eax
        jnz     .digit
        mov     si, di
        call    puts
        popad
        ret

newline:
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        ret

; ------------------------------------------------------------------ data

samples:     dw 0
cnt:         dd 0, 0, 0, 0
btn:         db 0
idle:        db 0
capped:      db 0
decbuf:      times 12 db 0

str_banner:  db "padtest: gameport at 0x201", 13, 10, 0
str_port:    db "idle port ", 0
str_noport:  db "NOPORT idle read ", 0
str_x:       db "X=", 0
str_y:       db " Y=", 0
str_z:       db " Z=", 0
str_r:       db " R=", 0
str_b:       db " B=", 0
str_capped:  db " CAPPED", 0
str_done:    db "DONE", 13, 10, 0

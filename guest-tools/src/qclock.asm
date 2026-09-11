; qclock.asm — DOS Quake's clock, read the way DOS Quake reads it, held
; against the processor's time-stamp counter.
;
;   QCLOCK.COM [seconds]   DOS real mode or a Win9x DOS box; 30 s by
;                          default, 120 at most. A key ends it early.
;
; The question behind it (2026-09-10): quake.exe in a Windows 98 DOS box
; on an unthrottled machine "speeds up momentarily for no reason". DOS
; Quake has one clock, Sys_FloatTime (WinQuake/sys_dos.c in id's GPL
; release), and it is built from two sources that are only consistent on
; an idle real PC:
;
;   * the BIOS tick count, the low word at 0040:006C, which INT 8 bumps
;     18.2 times a second — in a DOS box, Windows' *simulated* INT 8,
;     delivered when the VM is scheduled and caught up in bursts after;
;   * PIT counter 0, latched and read straight off ports 0x43 / 0x40, put
;     in mode 2 by Sys_Init so it counts 65536 down to 1 once per tick.
;
;   t  = tick << 16; latch; r = (counter - 1) & 0xffff; re-read tick,
;        and if it moved while r is in the counter's upper half, use it;
;   ft = t + 65536 - r                     (in 1/1193200 s)
;   time = ft - oldtime; oldtime = ft;     (always, even going backward)
;   if (time < 0) time = 0;                (the -3000 s case aside)
;   curtime += time; 100000 calls in a row with time == 0 add 1.0 s.
;
; So a reading that goes *backward* — the counter wrapped and the tick it
; owes has not arrived yet — counts nothing and lowers the reference, and
; the next reading after the tick does arrive counts in full from there.
; Every late tick is time Quake counts twice. Ticks that arrive in a burst
; after the VM was starved become a burst of forward steps, and Quake
; hands each frame up to 0.1 s of them (Host_FilterTime): a fast-forward.
;
; This program is that loop, with nothing else in it: Quake's read in a
; tight loop, interrupts on (the ticks have to arrive the way they do for
; the game), and the TSC read beside it as the clock nobody queues or
; virtualizes — RDTSC is not trapped in V86 mode on 9x, and under TCG it
; follows the host's own counter. Every 18 ticks (~1 s) it closes a
; window, and at the end prints one line per window:
;
;   true_ms   the window by the TSC
;   tick_ms   the window by the raw tick+counter reading, no clamping
;   quake_ms  what Quake's curtime advanced by
;   speed%    quake_ms / true_ms — the fast-forward, if there is one,
;             is a window well above 100 (FAST at 110 and over)
;   back      readings that went backward (a tick owed and not yet
;             delivered), backmax the largest of them in ms
;   jumps     reads that saw the tick count move by more than one since
;             the read before (a burst), jumpmax the most ticks at once
;   gapmax    the longest time between two consecutive reads, in ms —
;             how long this VM was not running at all
;   stall     Quake's 100000-identical-reads second
;
; The TSC's rate is not assumed: it is calibrated end to end against the
; raw reading over the whole run, which is right on average once every
; tick owed has been delivered. So the total line's gain_ms (Quake's time
; minus the clock's) is the extra time Quake counted, and each window's
; speed% is against that same average.
;
; The same program in pure DOS on the same QEMU is the control: INT 8 is
; then the real interrupt, and whatever is left is ours (i8254.c raising
; late IRQ 0 edges in a burst after a starved main loop) rather than
; Windows' VTD.
;
; Output goes to COM1 and the console after the run, never during it: a
; serial write in a DOS box is a trip through Windows' serial virtual
; device, and the loop must be Quake's loop and nothing else. For the
; same reason the key that ends it early is looked for in the BIOS
; keyboard buffer directly, not through INT 16h, whose polling is exactly
; what Windows' idle detection counts against a DOS box.
;
; nasm -f bin -o QCLOCK.COM qclock.asm
;
; SPDX-License-Identifier: GPL-2.0-or-later

        cpu     586
        bits    16
        org     0x100

WIN_TICKS  equ  18              ; one window, in BIOS ticks (~0.99 s)
DEFWIN     equ  30
MAXWIN     equ  120
SAME_LIMIT equ  100000          ; Sys_FloatTime's sametimecount
PIT_HZ     equ  1193200         ; Quake's constant, not the true 1193182

; A window's record.
R_TSC   equ     0               ; qword, TSC ticks
R_QK    equ     8               ; Quake's counts
R_RAW   equ     12              ; the raw reading's counts
R_BACK  equ     16              ; from here on a copy of w_back..w_stall
R_MAXB  equ     20
R_TJ    equ     24
R_MAXJ  equ     28
R_GAP   equ     32              ; qword, TSC ticks
R_STALL equ     40
REC     equ     48
W_DWORDS equ    7               ; w_back .. w_stall

start:
        cld
        ; DOS does not clear what lies past the image.
        mov     di, bss_start
        mov     cx, bss_end - bss_start
        xor     al, al
        rep     stosb
        call    parse_args
        mov     word [recptr], records

        mov     si, str_banner
        call    puts
        movzx   eax, word [nwin]
        mov     cl, 0
        call    put_sdec_w
        mov     si, str_banner2
        call    puts

        xor     ax, ax
        mov     es, ax                  ; es:046c is 0040:006c

        ; Sys_Init: counter 0 in mode 2, period 65536 — the same 18.2 Hz
        ; the BIOS runs it at, but counting once per tick instead of
        ; mode 3's twice.
        mov     al, 0x34
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al

        ; Start right after a tick, when the tick count owes nothing.
        call    wait_edge
        rdtsc
        mov     [cur_tsc], eax
        mov     [cur_tsc + 4], edx
        call    quake_read
        mov     [cur_ft], eax
        mov     [cur_tick], cx
        mov     [prev_ft], eax
        mov     [t0_ft], eax
        mov     [win_ft0], eax
        mov     [prev_tick], cx
        mov     [win_tick0], cx
        mov     eax, [cur_tsc]
        mov     edx, [cur_tsc + 4]
        mov     [prev_tsc], eax
        mov     [prev_tsc + 4], edx
        mov     [t0_tsc], eax
        mov     [t0_tsc + 4], edx
        mov     [win_tsc0], eax
        mov     [win_tsc0 + 4], edx

; ------------------------------------------------------------------ loop

.loop:
        rdtsc
        mov     [cur_tsc], eax
        mov     [cur_tsc + 4], edx
        call    quake_read
        mov     [cur_ft], eax
        mov     [cur_tick], cx

        ; The longest gap between two reads: time this VM did not run.
        mov     eax, [cur_tsc]
        mov     edx, [cur_tsc + 4]
        sub     eax, [prev_tsc]
        sbb     edx, [prev_tsc + 4]
        cmp     edx, [w_gap + 4]
        jb      .gapok
        ja      .gapnew
        cmp     eax, [w_gap]
        jbe     .gapok
.gapnew:
        mov     [w_gap], eax
        mov     [w_gap + 4], edx
.gapok:
        mov     eax, [cur_tsc]
        mov     [prev_tsc], eax
        mov     eax, [cur_tsc + 4]
        mov     [prev_tsc + 4], eax

        ; The tick count moving by more than one between two reads is a
        ; burst: ticks delivered faster than they are due.
        mov     ax, [cur_tick]
        sub     ax, [prev_tick]
        cmp     ax, 1
        jbe     .nojump
        inc     dword [w_tj]
        movzx   eax, ax
        cmp     eax, [w_maxj]
        jbe     .nojump
        mov     [w_maxj], eax
.nojump:
        mov     ax, [cur_tick]
        mov     [prev_tick], ax

        ; Quake's rule. oldtime = ft whichever way it went.
        mov     eax, [cur_ft]
        sub     eax, [prev_ft]
        mov     edx, [cur_ft]
        mov     [prev_ft], edx
        test    eax, eax
        jns     .fwd
        inc     dword [w_back]
        neg     eax
        cmp     eax, [w_maxb]
        jbe     .zero
        mov     [w_maxb], eax
.zero:  xor     eax, eax
.fwd:
        add     [quake_acc], eax
        test    eax, eax
        jnz     .moved
        inc     dword [samecount]
        cmp     dword [samecount], SAME_LIMIT
        jbe     .check
        add     dword [quake_acc], PIT_HZ       ; curtime += 1.0
        inc     dword [w_stall]
        mov     dword [samecount], 0
        jmp     .check
.moved:
        mov     dword [samecount], 0

.check:
        mov     ax, [cur_tick]
        sub     ax, [win_tick0]
        cmp     ax, WIN_TICKS
        jb      .loop
        call    close_window
        mov     ax, [iw]
        cmp     ax, [nwin]
        jae     .done
        ; A key in the BIOS buffer (head != tail) ends the run; take it
        ; out so the shell does not get it.
        mov     ax, [es:0x41c]
        cmp     ax, [es:0x41a]
        je      .loop
        mov     [es:0x41a], ax

.done:
        ; The BIOS's own setting back: mode 3, period 65536.
        mov     al, 0x36
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        push    ds
        pop     es
        call    report
        mov     si, str_done
        call    puts
        mov     ax, 0x4c00
        int     0x21

; ------------------------------------------------------------ the read

; Sys_FloatTime's read, instruction for instruction in what it touches:
; eax = ft (t + 65536 - r, in 1/1193200 s, wrapping at 2^32 like
; Quake's unsigned), cx = the tick word it used. es = 0. Clobbers ebx,
; edx.
quake_read:
        movzx   ecx, word [es:0x46c]    ; t
        xor     al, al
        out     0x43, al                ; latch counter 0
        in      al, 0x40
        mov     dl, al
        in      al, 0x40
        mov     dh, al
        dec     dx                      ; r = (r - 1) & 0xffff
        mov     bx, [es:0x46c]          ; tick
        cmp     bx, cx
        je      .same
        test    dh, 0x80
        jz      .same
        mov     cx, bx                  ; ticked during the read, early in the count
.same:
        mov     eax, ecx
        shl     eax, 16
        add     eax, 65536
        movzx   edx, dx
        sub     eax, edx
        ret

; Spin until the tick count moves.
wait_edge:
        mov     ax, [es:0x46c]
.spin:  cmp     ax, [es:0x46c]
        je      .spin
        ret

; File the window that just ended and open the next at this read.
close_window:
        push    es
        push    ds
        pop     es
        mov     di, [recptr]
        mov     eax, [cur_tsc]
        mov     edx, [cur_tsc + 4]
        sub     eax, [win_tsc0]
        sbb     edx, [win_tsc0 + 4]
        mov     [di + R_TSC], eax
        mov     [di + R_TSC + 4], edx
        mov     eax, [quake_acc]
        sub     eax, [win_qk0]
        mov     [di + R_QK], eax
        mov     eax, [cur_ft]
        sub     eax, [win_ft0]
        mov     [di + R_RAW], eax
        add     di, R_BACK
        mov     si, w_back
        mov     cx, W_DWORDS
        rep     movsd
        mov     di, w_back
        mov     cx, W_DWORDS
        xor     eax, eax
        rep     stosd
        pop     es

        mov     eax, [cur_tsc]
        mov     [win_tsc0], eax
        mov     eax, [cur_tsc + 4]
        mov     [win_tsc0 + 4], eax
        mov     eax, [quake_acc]
        mov     [win_qk0], eax
        mov     eax, [cur_ft]
        mov     [win_ft0], eax
        mov     ax, [cur_tick]
        mov     [win_tick0], ax
        add     word [recptr], REC
        inc     word [iw]
        ret

; ---------------------------------------------------------------- report

report:
        finit
        mov     eax, [cur_tsc]
        mov     edx, [cur_tsc + 4]
        sub     eax, [t0_tsc]
        sbb     edx, [t0_tsc + 4]
        mov     [tot_tsc], eax
        mov     [tot_tsc + 4], edx
        mov     eax, [cur_ft]
        sub     eax, [t0_ft]
        mov     [tot_raw], eax
        ; TSC ticks per ms, against the raw reading end to end.
        fild    qword [tot_tsc]
        fidiv   dword [tot_raw]
        fmul    qword [c_cms]
        fstp    qword [f_tscms]

        mov     si, str_head
        call    puts
        mov     bx, records
        xor     bp, bp
.win:
        cmp     bp, [iw]
        jae     .total
        movzx   eax, bp
        mov     cl, 4
        call    put_sdec_w
        fild    qword [bx + R_TSC]      ; true_ms
        fdiv    qword [f_tscms]
        fst     qword [f_true]
        mov     cl, 9
        call    put_st0_w
        fild    dword [bx + R_RAW]      ; tick_ms
        fdiv    qword [c_cms]
        mov     cl, 9
        call    put_st0_w
        fild    dword [bx + R_QK]       ; quake_ms
        fdiv    qword [c_cms]
        fst     qword [f_q]
        mov     cl, 9
        call    put_st0_w
        fld     qword [f_q]             ; speed%
        fmul    qword [c_100]
        fdiv    qword [f_true]
        mov     cl, 7
        call    put_st0_w
        mov     eax, [tmpi]
        mov     [speed], eax
        mov     eax, [bx + R_BACK]
        add     [tot_back], eax
        mov     cl, 6
        call    put_sdec_w
        fild    dword [bx + R_MAXB]     ; backmax ms
        fdiv    qword [c_cms]
        mov     cl, 8
        call    put_st0_w
        mov     eax, [bx + R_TJ]
        add     [tot_tj], eax
        mov     cl, 7
        call    put_sdec_w
        mov     eax, [bx + R_MAXJ]
        cmp     eax, [tot_maxj]
        jbe     .jm
        mov     [tot_maxj], eax
.jm:    mov     cl, 8
        call    put_sdec_w
        fild    qword [bx + R_GAP]      ; gapmax ms
        fdiv    qword [f_tscms]
        mov     cl, 7
        call    put_st0_w
        mov     eax, [bx + R_STALL]
        add     [tot_stall], eax
        mov     cl, 6
        call    put_sdec_w
        cmp     dword [speed], 110
        jl      .notfast
        mov     si, str_fast
        call    puts
        jmp     .eol
.notfast:
        cmp     dword [speed], 90
        jg      .eol
        mov     si, str_slow
        call    puts
.eol:
        call    newline
        add     bx, REC
        inc     bp
        jmp     .win

.total:
        mov     si, str_tclock
        call    puts
        fild    dword [tot_raw]
        fdiv    qword [c_cms]
        mov     cl, 0
        call    put_st0_w
        mov     si, str_tquake
        call    puts
        fild    dword [quake_acc]
        fdiv    qword [c_cms]
        mov     cl, 0
        call    put_st0_w
        mov     si, str_tgain
        call    puts
        mov     eax, [quake_acc]
        sub     eax, [tot_raw]
        mov     [tmpi], eax
        fild    dword [tmpi]
        fdiv    qword [c_cms]
        mov     cl, 0
        call    put_st0_w
        mov     si, str_tback
        call    puts
        mov     eax, [tot_back]
        mov     cl, 0
        call    put_sdec_w
        mov     si, str_tjumps
        call    puts
        mov     eax, [tot_tj]
        mov     cl, 0
        call    put_sdec_w
        mov     si, str_tjmax
        call    puts
        mov     eax, [tot_maxj]
        mov     cl, 0
        call    put_sdec_w
        mov     si, str_tstall
        call    puts
        mov     eax, [tot_stall]
        mov     cl, 0
        call    put_sdec_w
        mov     si, str_ttsc
        call    puts
        fld     qword [f_tscms]
        mov     cl, 0
        call    put_st0_w
        call    newline
        ret

; ------------------------------------------------------------- arguments

; The first decimal number on the command line, in seconds (windows).
parse_args:
        mov     word [nwin], DEFWIN
        mov     si, 0x81
        movzx   cx, byte [0x80]
        xor     ax, ax
        xor     bx, bx                  ; digits seen
.c:     jcxz    .end
        mov     dl, [si]
        inc     si
        dec     cx
        cmp     dl, '0'
        jb      .sep
        cmp     dl, '9'
        ja      .sep
        imul    ax, ax, 10
        sub     dl, '0'
        movzx   dx, dl
        add     ax, dx
        inc     bx
        jmp     .c
.sep:   test    bx, bx
        jz      .c
.end:   test    bx, bx
        jz      .ret
        test    ax, ax
        jz      .ret
        cmp     ax, MAXWIN
        jbe     .set
        mov     ax, MAXWIN
.set:   mov     [nwin], ax
.ret:   ret

; --------------------------------------------------------------- helpers

; al -> COM1 and the DOS console (padtest.asm's, same reasons: bounded
; wait, and a machine with no UART falls straight through).
putc:
        pushad
        mov     bl, al
        mov     dx, 0x3fd
        mov     cx, 0xffff
.wait:  in      al, dx
        test    al, 0x20
        jnz     .send
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

newline:
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        ret

; st0 rounded and popped, printed like put_sdec_w; the integer is left in
; tmpi.
put_st0_w:
        fistp   dword [tmpi]
        mov     eax, [tmpi]
        ; fall through

; eax as signed decimal, right-aligned in cl columns (0: no padding).
put_sdec_w:
        pushad
        mov     di, decbuf + 15
        mov     byte [di], 0
        xor     bp, bp
        test    eax, eax
        jns     .pos
        neg     eax
        inc     bp
.pos:   mov     ebx, 10
.digit: xor     edx, edx
        div     ebx
        add     dl, '0'
        dec     di
        mov     [di], dl
        test    eax, eax
        jnz     .digit
        test    bp, bp
        jz      .pad
        dec     di
        mov     byte [di], '-'
.pad:   mov     ax, decbuf + 15
        sub     ax, di
        xor     ch, ch
.sp:    cmp     ax, cx
        jae     .out
        push    ax
        mov     al, ' '
        call    putc
        pop     ax
        inc     ax
        jmp     .sp
.out:   mov     si, di
        call    puts
        popad
        ret

; ------------------------------------------------------------------ data

c_cms:       dq 1193.2                  ; counts per ms, Quake's rate
c_100:       dq 100.0

str_banner:  db "qclock: DOS Quake's Sys_FloatTime against the TSC, ", 0
str_banner2: db " windows of 18 ticks, PIT counter 0 in mode 2", 13, 10
             db "running (a key ends it early)", 13, 10, 0
str_head:    db " win  true_ms  tick_ms quake_ms speed%  back backmax"
             db "  jumps jumpmax gapmax stall", 13, 10, 0
str_fast:    db "  FAST", 0
str_slow:    db "  SLOW", 0
str_tclock:  db "total clock_ms=", 0
str_tquake:  db " quake_ms=", 0
str_tgain:   db " gain_ms=", 0
str_tback:   db " backsteps=", 0
str_tjumps:  db " tickjumps=", 0
str_tjmax:   db " jumpmax=", 0
str_tstall:  db " stall_s=", 0
str_ttsc:    db " tsc_khz=", 0
str_done:    db "DONE", 13, 10, 0

; The loop writes its state every iteration, and a store to a page QEMU
; has translated code from invalidates that code (CLAUDE.md's gotcha for
; a DOS .COM): so everything written lives at 0x3000, a page or more past
; the last byte of code whatever paragraph the segment starts on.
        times   -(($ - $$) > 0x3000 - 0x100 - 0x1000) db 0

        absolute 0x3000
bss_start:
cur_tsc:     resd 2
prev_tsc:    resd 2
t0_tsc:      resd 2
win_tsc0:    resd 2
tot_tsc:     resd 2
f_tscms:     resq 1
f_true:      resq 1
f_q:         resq 1
cur_ft:      resd 1
prev_ft:     resd 1
t0_ft:       resd 1
win_ft0:     resd 1
tot_raw:     resd 1
quake_acc:   resd 1
win_qk0:     resd 1
samecount:   resd 1
tmpi:        resd 1
speed:       resd 1
tot_back:    resd 1
tot_tj:      resd 1
tot_maxj:    resd 1
tot_stall:   resd 1
w_back:      resd 1                     ; w_back .. w_stall: R_BACK's order
w_maxb:      resd 1
w_tj:        resd 1
w_maxj:      resd 1
w_gap:       resd 2
w_stall:     resd 1
cur_tick:    resw 1
prev_tick:   resw 1
win_tick0:   resw 1
nwin:        resw 1
iw:          resw 1
recptr:      resw 1
decbuf:      resb 16
             alignb 16
records:     resb REC * MAXWIN
bss_end:

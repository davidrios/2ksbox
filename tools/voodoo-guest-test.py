#!/usr/bin/env python3
"""The Voodoo 2 device as a *guest* meets it (doc 21, M14), with no 3dfx
driver in the picture: a DOS program finds the card in PCI configuration
space, maps its 16 MiB BAR, checks the Voodoo 2 strap in initEnable, runs
the init sequence 3dfx's own sst1init runs (frame-buffer geometry, video
timing, the DAC's PLL, the colour lookup table, VGA pass-through), fills
the back buffer with red through the linear frame buffer, reads a pixel
of it back through the same window (which makes the chip flush its FIFO:
a store that never landed reads back as something else), swaps, and
then gives the monitor back to the VGA.

The evidence is on the host side: a QMP screendump while the Voodoo has
the monitor must be the 640x480 red frame (the whole path from a guest
`mov` to the console surface -- register decode, the memory FIFO, the
swap on the display timer's retrace, the CLUT, the pass-through switch),
and one after it lets go must be the VGA's text screen again. Every
`voodoo2:` line QEMU printed is shown.

    tools/voodoo-guest-test.py          # needs nasm, mtools, build/qemu

Outputs in build/voodoo-guest/. The `voodoo-guest` check in the guest
stage of scripts/test.sh.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "build/qemu/qemu-system-i386")
OUT = os.path.join(ROOT, "build/voodoo-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)
spec = importlib.util.spec_from_file_location("vgadirty", os.path.join(ROOT, "tools/vga-dirty-guest-test.py"))
vgadirty = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vgadirty)

# Where the guest maps the BAR (16 MiB, above RAM, below the APIC).
BAR = 0xE0000000
WIDTH, HEIGHT = 640, 480

ASM = r"""
org 100h
bits 16

BAR         equ 0E0000000h
LFB         equ BAR + 400000h
RED565      equ 0F800F800h          ; two RGB565 pixels of (248, 0, 0)

; register offsets (vid_voodoo_regs.h)
SST_status          equ 000h
SST_lfbMode         equ 114h
SST_swapbufferCMD   equ 128h
SST_videoDimensions equ 20Ch
SST_fbiInit0        equ 210h
SST_fbiInit1        equ 214h
SST_fbiInit2        equ 218h
SST_hSync           equ 220h
SST_vSync           equ 224h
SST_clutData        equ 228h
SST_dacData         equ 22Ch

start:
    mov si, str_hello
    call puts
    call a20_on
    call unreal_mode            ; FS = 4 GiB flat data segment

    ; --- find the card: bus 0, devices 0..31, function 0 ---------------
    xor bx, bx                  ; bx = device number
.scan:
    mov eax, 80000000h
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx                 ; bus 0, dev bx, func 0, reg 0
    mov dx, 0CF8h
    out dx, eax
    mov dx, 0CFCh
    in eax, dx
    cmp eax, 0002121Ah          ; vendor 121a, device 0002
    je .found
    inc bx
    cmp bx, 32
    jb .scan
    mov si, str_nocard
    call puts
    jmp exit
.found:
    mov [pci_dev], bx
    mov si, str_found
    call puts
    mov eax, ebx
    call puthex8
    call putnl

    ; --- map it and enable memory decoding -----------------------------
    mov cl, 10h
    mov eax, BAR
    call pci_write
    mov cl, 10h
    call pci_read
    mov si, str_bar
    call puts
    call puthex32
    call putnl
    mov cl, 04h
    call pci_read
    or eax, 2                   ; memory space enable
    mov cl, 04h
    call pci_write

    ; --- initEnable: bit 0 unlocks the fbiInit registers; byte 1 reads
    ;     back 0x50 | its high nibble on a Voodoo 2 (the strap the 3dfx
    ;     driver checks) ---------------------------------------------------
    mov cl, 40h
    mov eax, 1
    call pci_write
    mov cl, 40h
    call pci_read
    mov si, str_init
    call puts
    call puthex32
    call putnl
    mov [init_enable], eax

    ; --- the chip's own init: what sst1init does before a game draws ----
    mov edi, BAR
    mov eax, 0A0h               ; fbiInit1: 10 x 2 blocks of 32 px = 640 px rows
    mov [fs:edi + SST_fbiInit1], eax
    mov eax, 150 << 11          ; fbiInit2: buffers 150 x 4 KiB apart (640 x 480 x 2)
    mov [fs:edi + SST_fbiInit2], eax
    mov eax, (480 << 16) | 639  ; videoDimensions
    mov [fs:edi + SST_videoDimensions], eax
    mov eax, 009F027Fh          ; hSync: 640 active + 159 blank
    mov [fs:edi + SST_hSync], eax
    mov eax, 002D01E0h          ; vSync: 480 active + 45 blank
    mov [fs:edi + SST_vSync], eax
    mov eax, 0400h              ; dacData: PLL register select = 0
    mov [fs:edi + SST_dacData], eax
    mov eax, 0556h              ; PLL0 low byte: m = 0x56 + 2 = 88
    mov [fs:edi + SST_dacData], eax
    mov eax, 0537h              ; PLL0 high byte: n1 = 0x17 + 2 = 25, n2 = 1 -> 25.2 MHz
    mov [fs:edi + SST_dacData], eax

    ; the CLUT: 33 entries, a linear ramp (entry i = 8i; the last one is
    ; "set to 255" through bit 29, as the driver writes it)
    xor ecx, ecx
.clut:
    mov eax, ecx
    shl eax, 3                  ; 8i
    mov ebx, eax
    shl ebx, 8
    or eax, ebx                 ; g
    shl ebx, 8
    or eax, ebx                 ; r
    mov ebx, ecx
    shl ebx, 24
    or eax, ebx                 ; index
    cmp ecx, 32
    jb .clut_write
    mov eax, (32 << 24) | 20000000h
.clut_write:
    mov [fs:edi + SST_clutData], eax
    inc ecx
    cmp ecx, 33
    jb .clut

    mov eax, 050h               ; lfbMode: RGB565, write the back buffer, read the back buffer
    mov [fs:edi + SST_lfbMode], eax
    mov eax, 1                  ; fbiInit0: VGA pass-through -- the Voodoo takes the monitor
    mov [fs:edi + SST_fbiInit0], eax
    mov eax, [fs:edi + SST_fbiInit0]
    mov si, str_fbiinit0
    call puts
    call puthex32
    call putnl
    mov eax, [fs:edi + SST_status]
    mov si, str_status
    call puts
    call puthex32
    call putnl

    ; --- fill the back buffer through the LFB: 480 rows of 320 dwords,
    ;     2 KiB apart in the window --------------------------------------
    mov si, str_fill
    call puts
    mov edi, LFB
    mov edx, 480
.row:
    mov ecx, 320
    mov ebx, edi
.px:
    mov dword [fs:ebx], RED565
    add ebx, 4
    dec ecx
    jnz .px
    add edi, 2048
    dec edx
    jnz .row

    ; read one pixel pair back: row 10, x = 20..21
    mov edi, LFB + (10 << 11) + 40
    mov eax, [fs:edi]
    mov si, str_readback
    call puts
    call puthex32
    call putnl
    cmp eax, RED565
    je .rb_ok
    mov si, str_rb_fail
    call puts
    jmp .swap
.rb_ok:
    mov si, str_rb_pass
    call puts
.swap:
    ; --- swap on the next vertical retrace -----------------------------
    mov edi, BAR
    mov eax, 1
    mov [fs:edi + SST_swapbufferCMD], eax
    mov cx, 9                   ; ~0.5 s
    call delay_ticks
    mov si, str_swapped
    call puts

    mov cx, 36                  ; ~2 s: the host takes its screendump
    call delay_ticks

    ; --- give the monitor back to the VGA -------------------------------
    mov edi, BAR
    xor eax, eax
    mov [fs:edi + SST_fbiInit0], eax
    mov cx, 6
    call delay_ticks
    mov si, str_done
    call puts
exit:
    mov ax, 4C00h
    int 21h

; ---------------------------------------------------------------- helpers

a20_on:
    in al, 92h
    or al, 2
    out 92h, al
    ret

; Unreal mode: FS gets a 4 GiB limit from a protected-mode descriptor
; and keeps it after the switch back (a real-mode segment load changes
; the base, never the cached limit). No memory manager is loaded, so the
; CPU is really in real mode and the switch is legal.
unreal_mode:
    cli
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, gdt
    mov [gdtr + 2], eax
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm
.pm:
    mov ax, 08h
    mov fs, ax
    mov eax, cr0
    and al, 0FEh
    mov cr0, eax
    jmp short .rm
.rm:
    sti
    ret

; CL = register, EAX = value / result; the device found by the scan
pci_addr:
    movzx eax, byte [pci_dev]
    shl eax, 11
    or eax, 80000000h
    movzx ecx, cl
    or eax, ecx
    mov dx, 0CF8h
    out dx, eax
    ret
pci_read:
    call pci_addr
    mov dx, 0CFCh
    in eax, dx
    ret
pci_write:
    push eax
    call pci_addr
    pop eax
    mov dx, 0CFCh
    out dx, eax
    ret

; CX = BIOS ticks (18.2/s) to wait
delay_ticks:
    push es
    push ax
    mov ax, 40h
    mov es, ax
    mov ax, [es:6Ch]
.wait:
    mov bx, [es:6Ch]
    sub bx, ax
    cmp bx, cx
    jb .wait
    pop ax
    pop es
    ret

; serial console on COM1 (QEMU -serial), so the runner can read it
putc:
    push dx
    push ax
    mov dx, 3FDh
.lsr:
    in al, dx
    test al, 20h
    jz .lsr
    pop ax
    mov dx, 3F8h
    out dx, al
    pop dx
    ret
puts:                           ; DS:SI, keeps EAX (a value follows it)
    push eax
.next:
    lodsb
    test al, al
    jz .done
    call putc
    jmp .next
.done:
    pop eax
    ret
putnl:
    push eax
    mov al, 10
    call putc
    pop eax
    ret
puthex32:                       ; EAX
    push ecx
    mov ecx, 8
.digit:
    rol eax, 4
    push eax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    call putc
    pop eax
    dec ecx
    jnz .digit
    pop ecx
    ret
puthex8:                        ; AL
    push ecx
    push eax
    mov ecx, 2
    shl eax, 24
.digit:
    rol eax, 4
    push eax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    call putc
    pop eax
    dec ecx
    jnz .digit
    pop eax
    pop ecx
    ret

align 8
gdt:
    dq 0
    dw 0FFFFh, 0000h
    db 00h, 92h, 0CFh, 00h      ; data, base 0, limit 4 GiB, 32-bit
gdtr:
    dw 15
    dd 0

pci_dev:     dw 0
init_enable: dd 0

str_hello:    db "VOODOO2 guest test", 10, 0
str_nocard:   db "NOCARD: no 121a:0002 on bus 0", 10, 0
str_found:    db "FOUND dev ", 0
str_bar:      db "BAR0 ", 0
str_init:     db "INITENABLE ", 0
str_fbiinit0: db "FBIINIT0 ", 0
str_status:   db "STATUS ", 0
str_fill:     db "FILL 640x480 red", 10, 0
str_readback: db "READBACK ", 0
str_rb_pass:  db "LFB PASS", 10, 0
str_rb_fail:  db "LFB FAIL", 10, 0
str_swapped:  db "SWAPPED", 10, 0
str_done:     db "DONE", 10, 0
"""


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    asm = os.path.join(OUT, "voodoo.asm")
    com = os.path.join(OUT, "VOODOO.COM")
    img = os.path.join(OUT, "voodoo.img")
    with open(asm, "w") as f:
        f.write(ASM)
    sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        # no HIMEM / EMM386: the program switches to unreal mode itself,
        # which a V86 monitor would refuse
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nVOODOO.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::VOODOO.COM")
    return img


def wait_for(log, marker, p, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if p.poll() is not None:
            raise SystemExit("QEMU exited while waiting for %s" % what)
        if os.path.exists(log):
            text = open(log, "rb").read()
            if marker in text:
                return text.decode("latin-1")
            if b"NOCARD" in text:
                raise SystemExit("the guest found no Voodoo 2 on the PCI bus")
        time.sleep(0.1)
    raise SystemExit("timeout waiting for %s" % what)


def red_fraction(path):
    """(width, height, fraction of pixels that are the CLUT-ramped red)."""
    w, h, px = vgadirty.read_ppm(path)
    n = w * h
    red = 0
    for i in range(0, n * 3, 3):
        if px[i] >= 240 and px[i + 1] < 8 and px[i + 2] < 8:
            red += 1
    return w, h, red / n if n else 0.0


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    img = build()
    log = os.path.join(OUT, "serial.log")
    qlog = os.path.join(OUT, "qemu.log")
    # short: a socket path deep in a scratch tree is over AF_UNIX's limit
    sock = "/tmp/voodoo-guest-%d.qmp" % os.getpid()
    shot_on = os.path.join(OUT, "voodoo.ppm")
    shot_off = os.path.join(OUT, "vga.ppm")
    for f in (log, qlog, shot_on, shot_off, sock):
        if os.path.exists(f):
            os.unlink(f)
    ok = True
    with open(qlog, "wb") as qf:
        p = subprocess.Popen([
            QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
            "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
            "-vga", "std", "-device", "voodoo2",
            "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
            "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
            "-qmp", "unix:%s,server,nowait" % sock, "-audiodev", "none,id=a0",
        ], stdout=qf, stderr=subprocess.STDOUT)
        t0 = time.time()
        try:
            q = vgadirty.Qmp(sock)
            text = wait_for(log, b"SWAPPED", p, 180, "the guest's swap")
            q.screendump(shot_on)
            text = wait_for(log, b"DONE", p, 60, "the guest to finish")
            q.screendump(shot_off)
        finally:
            if p.poll() is None:
                p.terminate()
                p.wait()
            if os.path.exists(sock):
                os.unlink(sock)
    print("voodoo-guest: %.0f s in the guest" % (time.time() - t0))
    for line in text.splitlines():
        if line.strip():
            print("   ", line.strip())
    for line in open(qlog, "rb").read().decode("latin-1").splitlines():
        if "voodoo2" in line:
            print("   ", line.strip())

    # the guest's own findings
    if "INITENABLE 00005001" not in text:
        print("FAIL initEnable did not read back the Voodoo 2 strap (0x50) and bit 0")
        ok = False
    if "FBIINIT0 00000001" not in text:
        print("FAIL fbiInit0 did not read back VGA pass-through")
        ok = False
    if "LFB PASS" not in text:
        print("FAIL the LFB did not read back what was written")
        ok = False
    # and the host's: the frame on the console while the Voodoo has the
    # monitor, and the VGA's screen once it gives it back
    w, h, frac = red_fraction(shot_on)
    print("    screendump with the Voodoo on: %dx%d, %.1f%% red" % (w, h, frac * 100))
    if (w, h) != (WIDTH, HEIGHT) or frac < 0.99:
        print("FAIL the console did not show the Voodoo's 640x480 red frame")
        ok = False
    w, h, frac = red_fraction(shot_off)
    print("    screendump with the Voodoo off: %dx%d, %.1f%% red" % (w, h, frac * 100))
    if frac > 0.5:
        print("FAIL the console did not go back to the VGA")
        ok = False
    qtext = open(qlog, "rb").read().decode("latin-1")
    if "display on (VGA pass-through)" not in qtext or "display off (VGA back)" not in qtext:
        print("FAIL the device did not log both pass-through switches")
        ok = False
    if not ok:
        raise SystemExit("voodoo-guest-test: failed")
    print("voodoo-guest-test: passed")


if __name__ == "__main__":
    main()

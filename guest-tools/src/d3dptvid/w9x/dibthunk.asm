;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; dibthunk.asm - the drawing half of the Win98/Me display driver (doc 19,
; M10). Every GDI drawing entry this driver exports is the DIB Engine's,
; reached by a jump: the driver accelerates nothing on purpose, exactly as
; the XP driver's M7a step did, and the frame buffer the Engine draws into
; is guest VRAM itself.
;
; Two shapes of entry. The plain ones take the same arguments the Engine
; does and are a bare jump. The Ext ones take one extra argument - this
; device's PDEVICE - so the thunk pops the 16:16 return address into ECX,
; pushes the extra argument, pushes the return address back and jumps;
; AX, ECX and ES are free because the Pascal convention passes nothing in
; them.
;
; SPDX-License-Identifier: GPL-2.0-or-later
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

DIBTHK  macro   name, param
extrn   DIB_&name&Ext : far
public  name
name:
        mov     ax, DGROUP
        mov     es, ax
        assume  es:DGROUP
        pop     ecx                     ; save the 16:16 return address
        push    param                   ; the extra argument
        push    ecx                     ; and the return address back
        jmp     DIB_&name&Ext
        endm

DIBFWD  macro   name
extrn   DIB_&name : far
public  name
name:
        jmp     DIB_&name
        endm

_DATA   segment public 'DATA'
extrn   _lpDriverPDevice : dword
extrn   _wPalettized : word
_DATA   ends

DGROUP  group   _DATA

_TEXT   segment public 'CODE'
.386
assume  ds:nothing, es:nothing

; the entries that need this PDEVICE passed along
DIBTHK  EnumObj,              _lpDriverPDevice
DIBTHK  RealizeObject,        _lpDriverPDevice
DIBTHK  DibBlt,               _wPalettized
DIBTHK  GetPalette,           _lpDriverPDevice
DIBTHK  SetPaletteTranslate,  _lpDriverPDevice
DIBTHK  GetPaletteTranslate,  _lpDriverPDevice
DIBTHK  UpdateColors,         _lpDriverPDevice

; SetCursor, MoveCursor and CheckCursor (ordinals 102-104) are **not** here
; any more: this driver draws the pointer with the adapter's cursor sprite
; instead of the Engine's software one, so those three are C functions in
; d3dpt9x.c. They still fall back to `DIB_…CursorExt` on an adapter with no
; sprite, which is why the Engine's entries stay imported.

; ExtTextOut is **not** one of the thunked ones, and the reason is worth
; keeping: `DIB_ExtTextOutExt` (ordinal 403) does not take this device as
; its extra argument the way every other `…Ext` entry does — it takes *two*
; more pointers, `lpDrawTextBitmap` and `lpDrawRect`. Thunking it with one
; dword leaves the whole argument list four bytes low, and the Engine's very
; first instruction, `lds si,[bp+0x32]`, then loads a garbage selector: a
; fatal exception 0D the moment anything draws text, which on this adapter
; is a message written in VGA text mode that nothing on screen shows
; (doc 19 Section 15). The plain entry (ordinal 14) pushes the two nulls
; itself, so forwarding to it is both correct and what the reference driver
; does.
;
; and the ones that are the Engine's unchanged
DIBFWD  ExtTextOut
DIBFWD  BitBlt
DIBFWD  ColorInfo
DIBFWD  EnumDFonts
DIBFWD  Output
DIBFWD  Pixel
DIBFWD  StrBlt
DIBFWD  ScanLR
DIBFWD  DeviceMode
DIBFWD  GetCharWidth
DIBFWD  DeviceBitmap
DIBFWD  FastBorder
DIBFWD  SetAttribute
DIBFWD  CreateDIBitmap
DIBFWD  DibToDevice
DIBFWD  StretchBlt
DIBFWD  StretchDIBits
DIBFWD  SelectBitmap
DIBFWD  BitmapBits
DIBFWD  Inquire

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; The screen-switch hook (doc 19 section 29). The main VDD announces a
; screen switch to the Windows VM with INT 2Fh AX=4001h (the screen is being
; taken away: a DOS box is going full-screen) and AX=4002h (it is back), and
; a 9x display driver is expected to hook the vector and act on both -
; nothing else tells GDI to stop drawing while a DOS program owns the VGA,
; and nothing else repaints the desktop when it comes back. The C side is
; SwitchToBgnd / SwitchToFgnd in d3dpt9x.c; this is the interrupt handler,
; which chains everything else to the previous owner.
;
; The saved vector lives in the *code* segment, so that the chain needs no
; DS; code segments are read-only, so SetOldInt2Fh takes a writable alias
; selector of this segment from the caller (KERNEL's AllocCStoDSAlias).

SCREEN_SWITCH_OUT equ 4001h
SCREEN_SWITCH_IN  equ 4002h
FLAG_CF           equ 0001h

extrn   _SwitchToBgnd : near
extrn   _SwitchToFgnd : near
public  _SWHook
public  _SetOldInt2Fh
public  _GetOldInt2Fh

OldInt2Fh       dd      0

_SWHook proc    far
        cmp     ax, SCREEN_SWITCH_OUT
        jz      ssw_handle
        cmp     ax, SCREEN_SWITCH_IN
        jz      ssw_handle
        jmp     dword ptr cs:[OldInt2Fh]        ; not ours: registers untouched

ssw_handle:
        push    ds
        pushad
        mov     bx, DGROUP
        mov     ds, bx
        assume  ds:DGROUP
        ; the caller's flags: past pushad (32), ds (2) and the return
        ; address (4); carry clear tells the VDD the switch may go ahead
        mov     bp, sp
        and     word ptr [bp + 38], not FLAG_CF
        cmp     ax, SCREEN_SWITCH_OUT
        jne     ssw_in
        call    _SwitchToBgnd
        jmp     ssw_done
ssw_in:
        call    _SwitchToFgnd
ssw_done:
        assume  ds:nothing
        popad
        pop     ds
        iret
_SWHook endp

; void __cdecl SetOldInt2Fh(WORD alias, void __far *vec)
_SetOldInt2Fh proc near
        push    bp
        mov     bp, sp
        push    es
        mov     es, [bp + 4]
        mov     ax, [bp + 6]
        mov     word ptr es:[OldInt2Fh], ax
        mov     ax, [bp + 8]
        mov     word ptr es:[OldInt2Fh + 2], ax
        pop     es
        pop     bp
        ret
_SetOldInt2Fh endp

; void __far * __cdecl GetOldInt2Fh(void)
_GetOldInt2Fh proc near
        mov     ax, word ptr cs:[OldInt2Fh]
        mov     dx, word ptr cs:[OldInt2Fh + 2]
        ret
_GetOldInt2Fh endp

_TEXT   ends

        end

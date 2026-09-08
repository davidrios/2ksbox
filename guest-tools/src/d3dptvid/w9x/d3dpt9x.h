/*
 * d3dpt9x.h — the Win98/Me display driver for the d3dpt-vga adapter
 * (doc 19, ADR-012 / M10): what the driver's own translation units share.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT9X_H
#define D3DPT9X_H

#define DRV_VERSION         0x0400      /* Windows 4.0 (Windows 95). */

/* Int 2Fh subfunctions the display driver uses. */
#define STOP_IO_TRAP        0x4000      /* stop trapping video I/O */

/* the mode the driver is running, filled from the registry / SYSTEM.INI */
extern WORD wScrX, wScrY, wBpp, wDpi, wPalettized;
extern WORD OurVMHandle;
extern DWORD VDDEntryPoint;

/* the adapter, as the mini-VDD hands it to us */
extern DWORD dwVramSize;
extern DWORD dwVramLin, dwRegsLin;      /* linear, for the ring-3 HAL DLL */
extern WORD  wRegsSel, wVramSel;        /* selectors onto the registers and VRAM */
extern DWORD dwPitch;                   /* bytes per line of the current mode */

extern LPDIBENGINE lpDriverPDevice;
extern WORD wEnabled;

int  PhysicalEnable(void);
void PhysicalDisable(void);
BOOL AdapterFind(void);
void ReadDisplayConfig(void);
void dbg_str(const char *s);
void dbg_val(const char *tag, DWORD v);
void ZeroFar(void __far *p, WORD n);

/* 16x16 -> 32, inline: the driver links no C runtime, so a DWORD multiply
 * has to come from here rather than from Watcom's __U4M. It lives in the
 * header because a `#pragma aux` is not a symbol — a second translation
 * unit that only declares it gets an undefined `MulW_`. */
DWORD MulW(WORD a, WORD b);
#pragma aux MulW = "mul bx" parm [ax] [bx] value [dx ax];

/* **The 9x half of `-device d3dpt-vga,ddflags=N`** (doc 19 §21). One
 * register, two drivers: the NT core owns the low half of DDFLAGS
 * (`core/d3dpt_core.h`'s `DDF_*`) and everything 9x-only lives in the
 * high half, so the two can never collide.
 *
 * `D9F_CERTIFIED` is the repro for the bug that cost this step: it puts
 * `DDCAPS2_CERTIFIED` back in the caps, which makes the 32-bit runtime
 * throw the whole HAL away *after* the 16-bit half accepted it. Kept
 * because the failure is silent at both ends and this is the only way
 * to see it happen again on purpose. */
#define D9F_CERTIFIED    0x01000000ul   /* the repro: claim DDCAPS2_CERTIFIED again */

/* the DirectDraw half (d3dpt9dd.c): the DCICOMMAND escapes through which
 * a 16-bit .drv publishes its 32-bit HAL (doc 19 §2) */
struct DCICMD;
struct DD32BITDRIVERDATA;
struct DDVERSIONDATA;
BOOL DDGet32BitDriverName(struct DD32BITDRIVERDATA __far *dd32);
BOOL DDNewCallbackFns(struct DCICMD __far *lpCmd);
void DDGetVersion(struct DDVERSIONDATA __far *lpVer);
BOOL DDCreateDriverObject(void);
DWORD DDHinstance(void);

/* the adapter's registers, through wRegsSel */
DWORD RegGet(WORD off);
void  RegPut(WORD off, DWORD val);

#endif

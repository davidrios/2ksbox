/*
 * d3dpt_esc.h: the private display-driver escape shared by the 9x driver
 * and the programs that query it (doc 19 §43).
 *
 * It answers one question: does the *host* have a Direct3D executor? The
 * adapter reports it in `D3DPT_FB_REG_D3D_STATUS`, which the display driver
 * reads anyway. A ring-3 program cannot read it, because the register page
 * is mapped only for the driver and its HAL. Creating a DirectDraw object
 * and reading the HAL caps is circular for the one program that needs the
 * answer *before* DirectDraw loads (`D3DPRE.EXE`, which decides whether
 * this session's DirectDraw should be WineD3D's).
 *
 * Both sides run in the guest, so this header is the whole contract. Call
 * `ExtEscape` (or `Escape`) with this code and a `D3DPT_ESC_HOSTINFO`
 * output buffer. `QUERYESCSUPPORT` answers 1 for it, as GDI expects. A
 * driver that is not ours answers nothing, which means there is no d3dpt
 * adapter and so no executor either.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_ESC_H
#define D3DPT_ESC_H

/* Escape codes below 0x8000 are Microsoft's. This one is in the private
 * range, at a value nothing else on these machines has been seen to query
 * (the driver logs every code GDI queries, doc 19 §41). */
#define D3DPT_ESC_HOSTINFO      0x3d33

#define D3DPT_ESC_HOSTINFO_MAGIC  0x4f483344ul   /* 'D3HO' */

typedef struct {
    unsigned long magic;        /* D3DPT_ESC_HOSTINFO_MAGIC */
    unsigned long size;         /* sizeof this structure */
    unsigned long d3d_status;   /* D3DPT_FB_REG_D3D_STATUS: D3DPT_STATUS_* */
    unsigned long fb_version;   /* the adapter's register set version */
} D3DPT_ESC_HOSTINFO_T;

#endif /* D3DPT_ESC_H */

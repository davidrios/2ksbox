/*
 * d3dpt_esc.h — the private display-driver escape, shared by the 9x driver
 * and the programs that ask it a question (doc 19 §43).
 *
 * One question so far: does the *host* have a Direct3D executor? The
 * adapter answers it in `D3DPT_FB_REG_D3D_STATUS` and the display driver
 * reads that register anyway; a ring-3 program cannot, because the register
 * page is mapped for the driver and its HAL, not for anyone who asks. The
 * obvious route — create a DirectDraw object and look at the HAL caps — is
 * circular for the one program that needs the answer *before* DirectDraw is
 * loaded (`D3DPRE.EXE`, which decides whether this session's DirectDraw
 * should be WineD3D's).
 *
 * Both sides are guest-side, so this header is the whole contract: an
 * `ExtEscape` (or `Escape`) with this code and a `D3DPT_ESC_HOSTINFO`
 * output buffer. `QUERYESCSUPPORT` answers 1 for it, as GDI expects, and a
 * driver that is not ours answers nothing at all — which is its own answer
 * (there is no d3dpt adapter, so there is no executor either).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_ESC_H
#define D3DPT_ESC_H

/* Escape codes below 0x8000 are Microsoft's; this sits in the private range
 * with a value nothing else on these machines has been seen to ask for
 * (the driver logs every code GDI queries: doc 19 §41's escape census). */
#define D3DPT_ESC_HOSTINFO      0x3d33

#define D3DPT_ESC_HOSTINFO_MAGIC  0x4f483344ul   /* 'D3HO' */

typedef struct {
    unsigned long magic;        /* D3DPT_ESC_HOSTINFO_MAGIC */
    unsigned long size;         /* sizeof this structure */
    unsigned long d3d_status;   /* D3DPT_FB_REG_D3D_STATUS: D3DPT_STATUS_* */
    unsigned long fb_version;   /* the adapter's register set version */
} D3DPT_ESC_HOSTINFO_T;

#endif /* D3DPT_ESC_H */

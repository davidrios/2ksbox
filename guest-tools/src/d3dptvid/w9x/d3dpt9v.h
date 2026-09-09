/*
 * d3dpt9v.h — what the Win98/Me mini-VDD and the display driver agree on
 * (doc 19, M10).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT9V_H
#define D3DPT9V_H

/* Our VxD's identity. The device ID is what int 2Fh AX=1684h looks up;
 * 0x4334 is in the range the RBIL lists as unassigned, one past the one
 * vmdisp9x took, so both can be installed on the same machine. */
#define D3DPT_VXD_ID     0x4334
#define D3DPT_VXD_NAME   "D3DPTVXD"
#define D3DPT_VXD_MAJOR  4      /* 4 for Windows 95 and newer */
#define D3DPT_VXD_MINOR  0

/* The mini-VDD function the display driver reaches through the main VDD's
 * VDD_REGISTER_DISPLAY_DRIVER_INFO, and the main VDD service that hands
 * out the dispatch table it lives in. */
#define VDD_REGISTER_DISPLAY_DRIVER 0
#define VDD__Get_Mini_Dispatch_Table 14

/* The four the main VDD calls around a screen switch — a full-screen DOS
 * box taking the adapter to VGA and giving it back. Repeated from
 * minivdd.h's function list (which the VxD does not include; it builds
 * against vmm.h alone) for the same reason as the line above. */
#define VDD_PRE_HIRES_TO_VGA        4
#define VDD_POST_HIRES_TO_VGA       5
#define VDD_PRE_VGA_TO_HIRES        6
#define VDD_POST_VGA_TO_HIRES       7

/* What the display driver calls on the main VDD to reach the function
 * above: minivdd.h's VDD_REGISTER_DISPLAY_DRIVER_INFO. Repeated here so
 * the two halves cannot drift. The answer is
 *   EAX = selector onto the register page   ECX = VRAM bytes
 *   EDX = selector onto VRAM                ESI = VRAM's linear address
 *                                           EDI = the register page's linear
 * and carry set means the adapter is not usable.
 *
 * The two linear addresses are not a convenience: the ring-3 HAL DLL is
 * 32-bit and has no idea what a selector is, and DPMI cannot tell it —
 * these selectors are GDT ones the mini-VDD built, and DPMI's "get
 * segment base" only knows the LDT (2026-09-07). The VxD is the only
 * thing that knows both, so it says both. */
#define D3DPT_VDD_REGISTER_INFO 0x83

/* The selectors handed back are DPL 3: the display driver is ring-3 code
 * and the DIB Engine draws through them. */
#define D3DPT_SEL_TYPE 0xf2

#endif

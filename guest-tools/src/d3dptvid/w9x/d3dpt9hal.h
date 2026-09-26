/*
 * d3dpt9hal.h: what the 16-bit display driver and the ring-3 HAL DLL
 * agree on (doc 19 §2, §8).
 *
 * On 9x the DirectDraw/Direct3D HAL is not part of the display driver. It
 * is an ordinary user-mode Win32 DLL that DirectDraw loads into the game's
 * own process. The 16-bit `.drv` names it in its answer to the
 * `DCICOMMAND` escape `DDGET32BITDRIVERNAME`, together with a 32-bit
 * "context" value, which is the linear address of this structure.
 * DirectDraw calls the DLL's `DriverInit` with it, the DLL fills in the
 * `cb32` table with its callbacks, and the 16-bit side copies those into
 * the `DDHAL_DDCALLBACKS` / `DDHAL_DDSURFACECALLBACKS` it hands DirectDraw.
 *
 * One linear address works in both halves because 9x maps everything
 * above 2 GiB into every process. The `.drv` allocates the block through
 * DPMI (so it lands in the shared arena) and reaches it through a selector
 * it made for it, and the DLL dereferences the linear address directly.
 * The adapter's two mappings work the same way. The mini-VDD commits them
 * `PC_USER`, so their linear addresses (`d3dptvxd: regs lin=80018000`,
 * `vram lin=80019000` in the boot log) are readable and writable from
 * ring 3 with no ioctl and no ring transition per batch. That lets the DLL
 * ring the DOORBELL as the XP driver does.
 *
 * Two toolchains compile the halves (Open Watcom 16-bit for the `.drv`,
 * mingw-w64 32-bit for the DLL), so every field here is a fixed-width type
 * and both sides check the layout at build time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT9HAL_H
#define D3DPT9HAL_H

/* The DLL, and the entry point DirectDraw calls in it. 8.3 because the
 * guest-tools ISO is ISO 9660 and the INF copies it by that name. */
#define D3DPT_HAL_DLL   "d3dpt9hl.dll"
#define D3DPT_HAL_ENTRY "DriverInit"

/* Bumped whenever anything below moves. The DLL refuses a block whose
 * version is not its own and logs it rather than read a layout it does not
 * know. The two halves ship together, but a stale copy of one in
 * WINDOWS\SYSTEM happens, and on 9x a wrong pointer is a silent reboot. */
#define D3DPT_HAL9_MAGIC   0x39335044ul     /* 'DP39' */
#define D3DPT_HAL9_VERSION 7

#define D3DPT_HAL9_MAX_MODES 32

/* The 32-bit callbacks the DLL publishes. Each is a flat function
 * pointer, zero when the DLL does not implement it. The names are the
 * DirectDraw HAL's own (ddrawi.h). Order is layout: append only. */
typedef struct d3dpt_hal9_cb32 {
    unsigned long DestroyDriver;
    unsigned long CreateSurface;
    unsigned long SetColorKey;
    unsigned long SetMode;
    unsigned long WaitForVerticalBlank;
    unsigned long CanCreateSurface;
    unsigned long CreatePalette;
    unsigned long GetScanLine;
    /* surface callbacks */
    unsigned long DestroySurface;
    unsigned long Flip;
    unsigned long SetClipList;
    unsigned long Lock;
    unsigned long Unlock;
    unsigned long Blt;
    unsigned long SetColorKeySurface;
    unsigned long AddAttachedSurface;
    unsigned long GetBltStatus;
    unsigned long GetFlipStatus;
    unsigned long UpdateOverlay;
    unsigned long SetOverlayPosition;
    unsigned long SetPalette;
    /* the ones DirectDraw asks for by GUID rather than by table */
    unsigned long GetDriverInfo;
    /* execute buffer pseudo-object callbacks */
    unsigned long CanCreateExecuteBuffer;
    unsigned long CreateExecuteBuffer;
    unsigned long DestroyExecuteBuffer;
    unsigned long LockExecuteBuffer;
    unsigned long UnlockExecuteBuffer;
} d3dpt_hal9_cb32;

typedef struct d3dpt_hal9 {
    unsigned long magic;                /* D3DPT_HAL9_MAGIC */
    unsigned long version;              /* D3DPT_HAL9_VERSION */
    unsigned long size;                 /* sizeof(struct d3dpt_hal9) */

    /* The adapter as the mini-VDD mapped it (linear, PC_USER, shared
     * arena), valid in the .drv and in every process that loads the DLL. */
    unsigned long regs_linear;          /* the register page */
    unsigned long vram_linear;          /* the frame buffer */
    unsigned long vram_size;

    /* the mode the .drv has the adapter in */
    unsigned long width, height, bpp, pitch;

    /* filled by DriverInit in the DLL */
    d3dpt_hal9_cb32 cb32;
    unsigned long dll_ready;            /* non-zero once DriverInit has run */
    /* The DLL's own module handle, which only the DLL knows. The 16-bit
     * half puts it in DDHALINFO::hInstance and returns it from
     * DDCREATEDRIVEROBJECT. DirectDraw uses it to tie the 32-bit callbacks
     * to a module it has loaded. Without it the runtime accepts the HAL
     * and never calls it, keeping its own HEL. */
    unsigned long dll_hinstance;
    /* What the DLL read from the adapter's MAGIC and VERSION registers
     * through regs_linear, from ring 3, in the game's process. The 16-bit
     * half logs them, because a DLL that cannot reach the hardware cannot
     * log that it failed. */
    unsigned long dll_reg_magic;
    unsigned long dll_reg_version;

    /* Direct3D exports */
    unsigned long d3dhal_global;        /* flat pointer to D3DHAL_GLOBALDRIVERDATA */
    unsigned long d3dhal_callbacks;     /* flat pointer to D3DHAL_CALLBACKS */

    /* The DirectDraw tables live here, not in the 16-bit driver's data
     * segment. DDHALINFO carries pointers (to the callback tables, the mode
     * list, the video-memory heap) and the runtime's 32-bit half follows
     * them. A 16-bit driver's `&table` is a selector:offset that half
     * cannot dereference, so the tables need a flat linear address, which
     * this block has. A HALINFO the 32-bit half rejects is still accepted
     * by the 16-bit half, and the runtime then uses its own HEL for
     * everything, which looks the same as no HAL at all (doc 19 §21).
     *
     * Raw words rather than the structures, because the two toolchains'
     * DDK headers spell those types differently. The
     * 16-bit half lays the real structures into them and asserts at
     * compile time that each one fits. */
    unsigned long halinfo[160];         /* DDHALINFO (it carries DDCORECAPS whole) */
    unsigned long cb_dd[24];            /* DDHAL_DDCALLBACKS */
    unsigned long cb_surf[32];          /* DDHAL_DDSURFACECALLBACKS */
    unsigned long cb_pal[8];            /* DDHAL_DDPALETTECALLBACKS */
    unsigned long cb_exebuf[8];         /* DDHAL_DDEXEBUFCALLBACKS */
    unsigned long fourcc[15];           /* FourCC codes (DX9 formats since v7) */
    unsigned long modeinfo[D3DPT_HAL9_MAX_MODES * 9]; /* DDHALMODEINFO (32 modes * 9 dwords) */
    unsigned long heap[8];              /* VIDMEM */

    /* The one command window at the top of VRAM has one writer on NT,
     * where the HAL is in the kernel behind dxg. Here every process with
     * a Direct3D device has its own copy of the DLL and they share it,
     * so submissions are serialised on this word with a locked exchange
     * (doc 19 §8). Zero means free. */
    unsigned long cmd_lock;
    unsigned long cmd_lock_owner;
    unsigned long cmd_lock_depth;
} d3dpt_hal9;

#endif

/*
 * d3dpthal.c — the ring-3 DirectDraw/Direct3D HAL for Windows 98/Me
 * (doc 19 §1, §2, §8; M10 step 3). Built as `d3dpt9hl.dll`.
 *
 * This is the third of the driver's three 9x binaries, and the only one
 * that is an ordinary user-mode Win32 DLL: DirectDraw loads it into
 * *the game's own process*, because on 9x the HAL runs in ring 3 rather
 * than behind a kernel-mode dxg the way it does on NT. It is therefore
 * also the only one that links our OS-independent core (doc 19 §19) —
 * the 16-bit `.drv` and the ring-0 `.vxd` never see it.
 *
 * How it is reached: the `.drv` answers the `DCICOMMAND` escape
 * `DDGET32BITDRIVERNAME` with this DLL's name, the entry point
 * `DriverInit`, and a context value that is the linear address of the
 * `d3dpt_hal9` block the two halves share (`d3dpt9dd.c`). `DriverInit`
 * fills that block's `cb32` table; the `.drv` copies the entries into
 * the tables DirectDraw wants and calls `lpSetInfo`.
 *
 * The adapter is reached with no ioctl and no ring transition: the
 * mini-VDD committed the register page and VRAM `PC_USER` in the shared
 * arena above 2 GiB, which 9x maps into every process, so the linear
 * addresses in the block are ordinary pointers here. That is what lets
 * this write the DOORBELL register directly, exactly as the XP driver
 * does from kernel mode (doc 19 §8, the first of the two options).
 *
 * Freestanding, no CRT — like the XP driver and like `vmhal9x`, so that
 * nothing drags a runtime into a DLL that is loaded into every game.
 *
 * Build: guest-tools/build-driver9x.sh (mingw-w64, i686).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <ddraw.h>

#include "ddhal32.h"
#include "d3dpt9hal.h"
#include "../../../../d3dpt/d3dpt_fb.h"

static d3dpt_hal9 *hal;                 /* the block, shared with the .drv */
static HINSTANCE dll_instance;          /* ours, from DllMain */
static volatile ULONG *regs;            /* the adapter's register page */

/* ------------------------------------------------------------ debug log */

/* Through the adapter's DEBUG register, into the QEMU log — the same
 * channel both other halves use, and the only one that has ever been
 * seen from ring 3 in this guest (port 0xE9 has not: doc 19's traps). */
static void dbg_puts(const char *s)
{
    if (!regs) {
        return;
    }
    while (*s) {
        regs[D3DPT_FB_REG_DEBUG / 4] = (unsigned char)*s++;
    }
}

static void dbg_hex(const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    int i;

    dbg_puts(tag);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(buf);
}

/* --------------------------------------------------------- the callbacks */

/* Wait for the adapter's frame counter to move. This is the whole of the
 * HAL for now, and it is here first on purpose: it needs no surface, no
 * heap and no protocol, so what it proves is exactly the thing that has
 * to be proved before anything else is written — that DirectDraw loads
 * this DLL into a game's process, calls into it, and that the register
 * page the mini-VDD mapped is readable from ring 3 there.
 *
 * Bounded like the XP driver's (doc 15): a device that has stopped
 * counting must not stop the guest with it, so the wait gives up. There
 * is no performance counter here that is worth a ring transition, so the
 * bound is a spin count rather than a clock — generous, because being
 * late is harmless and being early is a busy loop that never ends. */
static DWORD __stdcall WaitForVerticalBlank32(d3dpt_ddhal_waitvb *d)
{
    ULONG f, i;
    volatile ULONG spin = 0;
    static int said;

    /* Once, and only once: that this is entered at all is the thing
     * being proved, and a line per frame would drown the log. */
    if (!said) {
        said = 1;
        dbg_hex("d3dpthal: WaitForVerticalBlank, flags ", d->dwFlags);
        dbg_puts("\n");
    }
    d->ddRVal = DD_OK;
    if (!regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    switch (d->dwFlags) {
    case DDWAITVB_BLOCKBEGIN:
    case DDWAITVB_BLOCKEND:
        f = regs[D3DPT_FB_REG_FRAMES / 4];
        for (i = 0; i < 2000000ul; i++) {
            if (regs[D3DPT_FB_REG_FRAMES / 4] != f) {
                break;
            }
            spin = i;
        }
        (void)spin;
        break;
    case DDWAITVB_I_TESTVB:
        /* "is it in a vertical blank now": we have no such register, and
         * saying yes is the answer that keeps a caller from spinning */
        d->bIsInVB = TRUE;
        break;
    default:
        d->ddRVal = DDERR_INVALIDPARAMS;
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* The two surface-creation callbacks, published so that the question the
 * vertical blank left open can be answered before a surface layer is
 * written on top of the assumption: **is a callback this DLL publishes
 * entered at all?** Both decline, so the runtime allocates out of our
 * heap exactly as it already does and nothing that works stops working;
 * all they add is a line saying what was asked for. When the surface
 * layer proper is written (M7b) these are where it starts. */
static DWORD __stdcall CanCreateSurface32(d3dpt_ddhal_cancreatesurface *d)
{
    static int said;

    if (!said) {
        said = 1;
        dbg_hex("d3dpthal: CanCreateSurface, caps ",
                d->lpDDSurfaceDesc ? d->lpDDSurfaceDesc->ddsCaps.dwCaps : 0);
        dbg_puts("\n");
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall CreateSurface32(d3dpt_ddhal_createsurface *d)
{
    static int said;

    if (!said) {
        said = 1;
        dbg_hex("d3dpthal: CreateSurface, caps ",
                d->lpDDSurfaceDesc ? d->lpDDSurfaceDesc->ddsCaps.dwCaps : 0);
        dbg_hex(" count ", d->dwSCnt);
        dbg_puts("\n");
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

/* DirectDraw is done with us. Nothing to release: the block belongs to
 * the .drv and the mappings to the mini-VDD. */
static DWORD __stdcall DestroyDriver32(d3dpt_ddhal_destroydriver *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ------------------------------------------------------------ DriverInit */

/* Called by DirectDraw, in the game's process, with the linear address
 * the .drv put in DD32BITDRIVERDATA::dwContext. Returning zero means
 * "no 32-bit HAL", which DirectDraw accepts quietly — so every reason to
 * fail says so through the register first, while it still can. */
DWORD __stdcall DriverInit(LPVOID ptr)
{
    d3dpt_hal9 *h = (d3dpt_hal9 *)ptr;

    if (!h) {
        return 0;
    }
    /* Validate before dereferencing anything else: a stale .drv or a
     * stale DLL in WINDOWS\SYSTEM is a real possibility, and on 9x a
     * wrong pointer is a silent reboot rather than an error. The
     * register page is not usable until the magic says the block is
     * ours, so this check cannot be logged — it is the one failure that
     * has to be silent. */
    if (h->magic != D3DPT_HAL9_MAGIC || h->size != sizeof(d3dpt_hal9)) {
        return 0;
    }
    regs = (volatile ULONG *)h->regs_linear;
    if (h->version != D3DPT_HAL9_VERSION) {
        dbg_hex("d3dpthal: shared block version ", h->version);
        dbg_hex(" but this DLL speaks ", D3DPT_HAL9_VERSION);
        dbg_puts(" — install both halves from the same guest-tools ISO\n");
        regs = 0;
        return 0;
    }
    hal = h;
    /* Before anything else, and through the block rather than the log:
     * see the note in d3dpt9hal.h. */
    h->dll_reg_magic = regs[D3DPT_FB_REG_MAGIC / 4];
    h->dll_reg_version = regs[D3DPT_FB_REG_VERSION / 4];

    dbg_hex("d3dpthal: DriverInit, block at ", (ULONG)(ULONG_PTR)h);
    dbg_hex(" regs ", h->regs_linear);
    dbg_hex(" vram ", h->vram_linear);
    dbg_hex(" mode ", (h->width << 16) | h->height);
    dbg_hex(" bpp ", h->bpp);
    dbg_puts("\n");

    /* **Below 2 GiB is not a working HAL.** DirectDraw loads this DLL in
     * DDHELP.EXE and publishes the callbacks below as flat addresses, and
     * the process that validates them is the *game's*: a module in the
     * private arena is mapped only where it was loaded, and every
     * application gets its own address for it (measured — DDHELP got
     * 0x00b50000 and the probe's own LoadLibrary got 0x00ca0000). Only a
     * module in the shared arena above 0x80000000 means the same thing in
     * both, which is why this is linked where it is. If the loader
     * relocated us anyway, the callbacks would be refused with no message
     * at either end, so say it here while there is somewhere to say it. */
    if ((ULONG)(ULONG_PTR)dll_instance < 0x80000000ul) {
        dbg_hex("d3dpthal: relocated out of the shared arena, to ",
                (ULONG)(ULONG_PTR)dll_instance);
        dbg_puts(" — the callbacks would be bad pointers in every game\n");
    }
    h->dll_hinstance = (unsigned long)(ULONG_PTR)dll_instance;
    h->cb32.WaitForVerticalBlank = (unsigned long)(ULONG_PTR)WaitForVerticalBlank32;
    h->cb32.CanCreateSurface = (unsigned long)(ULONG_PTR)CanCreateSurface32;
    h->cb32.CreateSurface = (unsigned long)(ULONG_PTR)CreateSurface32;
    h->cb32.DestroyDriver = (unsigned long)(ULONG_PTR)DestroyDriver32;
    h->dll_ready = 1;
    /* **Whose process is this?** The callbacks published here are flat
     * addresses in it, and DirectDraw's HALINFO validator `IsBadCodePtr`s
     * every one of them — in whichever process is building a DirectDraw
     * object, which is not necessarily this one. If the escapes are
     * answered once for the system rather than once per process, a
     * pointer that is perfectly good here is a bad pointer there
     * (2026-09-08). The name is the only way to tell the two stories
     * apart, and it costs one kernel32 call at init. */
    {
        char name[128];
        DWORD n = GetModuleFileNameA(NULL, name, sizeof(name) - 1);

        name[n < sizeof(name) ? n : sizeof(name) - 1] = 0;
        dbg_puts("d3dpthal:   in ");
        dbg_puts(name);
        dbg_hex(" pid ", GetCurrentProcessId());
        dbg_puts("\n");
    }
    dbg_hex("d3dpthal:   published vblank ", h->cb32.WaitForVerticalBlank);
    dbg_hex(" cansurf ", h->cb32.CanCreateSurface);
    dbg_hex(" hinstance ", h->dll_hinstance);
    dbg_puts("\n");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        dll_instance = inst;
    }
    return TRUE;
}

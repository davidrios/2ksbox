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
#include <ddrawi.h>

#include "d3dpt_ddi.h"
#include "d3dpt_core.h"
#include "ddhal32.h"
#include "d3dpt9hal.h"
#include "../../../../d3dpt/d3dpt_fb.h"

static d3dpt_hal9 *hal;                 /* the block, shared with the .drv */
static HINSTANCE dll_instance;          /* ours, from DllMain */
static volatile ULONG *regs;            /* the adapter's register page */
static d3dpt_core core;

/* ------------------------------------------------------------ freestanding helpers */

void *memcpy(void *dst, const void *src, size_t n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    char *d = (char *)dst;
    while (n--) {
        *d++ = (char)c;
    }
    return dst;
}

/* ------------------------------------------------------------ OS hooks for core */

void *d3dpt_os_alloc(ULONG bytes)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}

void d3dpt_os_free(void *p)
{
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

void d3dpt_os_ticks(LONGLONG *now, LONGLONG *freq)
{
    if (freq) {
        QueryPerformanceFrequency((LARGE_INTEGER *)freq);
    }
    QueryPerformanceCounter((LARGE_INTEGER *)now);
}

/*
 * DirectDraw hands the 9x HAL a DDRAWI_DDRAWSURFACE_LCL, the same object the
 * NT layer casts directly. This used to sniff the pointer first, because a
 * DirectX 6 runtime passes an interface (DDRAWI_DDRAWSURFACE_INT) in
 * D3DHAL_CONTEXTCREATEDATA / D3DHAL_SETRENDERTARGETDATA instead. That branch
 * was never taken once the guest ran DirectX 9 — checked with a log in it
 * across a full ebtest (the DirectX 3 path, which is where an interface
 * pointer would have shown up) and a d3d7test — and a 2ksbox Win98 machine
 * runs the last DirectX by decision, so the guess is gone rather than kept
 * unexercised.
 */
static inline LPDDRAWI_DDRAWSURFACE_LCL surf_lcl(void *p)
{
    return (LPDDRAWI_DDRAWSURFACE_LCL)p;
}

static inline ULONG surf_handle(void *p)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(p);
    if (!s || !s->lpSurfMore) return 0;
    if (!s->lpSurfMore->dwSurfaceHandle) {
        static DWORD next_handle = 100;
        s->lpSurfMore->dwSurfaceHandle = ++next_handle;
    }
    return s->lpSurfMore->dwSurfaceHandle;
}

BOOL d3dpt_os_surf(d3dpt_core *c, void *os, d3dpt_surf_desc *out)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(os);

    ULONG fmt = 0;

    if (!s || !s->lpGbl) {
        return FALSE;
    }
    if (hal && hal->bpp) {
        c->w = hal->width;
        c->h = hal->height;
        c->bpp = hal->bpp;
        c->pitch = hal->pitch;
    }
    out->os = s;
    out->handle = surf_handle(s);
    out->caps = s->ddsCaps.dwCaps;
    out->caps2 = (s->lpSurfMore) ? s->lpSurfMore->ddsCapsEx.dwCaps2 : 0;
    out->flags = s->dwFlags;
    out->w = s->lpGbl->wWidth;
    out->h = s->lpGbl->wHeight;
    out->pitch = (ULONG)s->lpGbl->lPitch;
    out->linear = s->lpGbl->dwLinearSize;
    out->vidmem = (ULONG)s->lpGbl->fpVidMem;
    if (hal && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && out->vidmem >= hal->vram_linear) {
        out->vidmem -= hal->vram_linear;
    }
    if ((s->dwFlags & DDRAWISURF_HASPIXELFORMAT) ||
        (s->lpGbl->ddpfSurface.dwFlags & (DDPF_RGB | DDPF_FOURCC | DDPF_ZBUFFER | DDPF_PALETTEINDEXED8))) {
        fmt = pf_format(&s->lpGbl->ddpfSurface);
        out->pf_flags = s->lpGbl->ddpfSurface.dwFlags;
    }
    if (!fmt) {
        ULONG bpp = (hal && hal->bpp) ? hal->bpp : c->bpp;
        fmt = (bpp == 32) ? D3DFMT_X8R8G8B8_ : D3DFMT_R5G6B5_;
        out->pf_flags = 0xffffffffu;
    }
    out->fmt = fmt;
    out->ck_lo = s->ddckCKSrcBlt.dwColorSpaceLowValue;
    out->ck_hi = s->ddckCKSrcBlt.dwColorSpaceHighValue;
    out->ck_dst_lo = s->ddckCKDestBlt.dwColorSpaceLowValue;
    return TRUE;
}

ULONG d3dpt_os_attached(void *os, void **out, ULONG max)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(os);
    LPATTACHLIST a;
    ULONG n = 0;

    if (!s) return 0;
    for (a = s->lpAttachList; a && n < max; a = a->lpLink) {
        LPDDRAWI_DDRAWSURFACE_LCL t = surf_lcl(a->lpAttached);
        if (t && t->lpGbl && !(t->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) {
            out[n++] = t;
        }
    }
    return n;
}

void *d3dpt_os_next_mip(void *os)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(os);
    LPATTACHLIST a;

    if (!s) return NULL;
    for (a = s->lpAttachList; a; a = a->lpLink) {
        LPDDRAWI_DDRAWSURFACE_LCL t = surf_lcl(a->lpAttached);
        if (t && t->lpGbl && (t->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && t != s &&
            (t->lpGbl->wWidth < s->lpGbl->wWidth || t->lpGbl->wHeight < s->lpGbl->wHeight)) {
            return t;
        }
    }
    return NULL;
}

/* --------------------------------------------------------- DirectDraw callbacks */

static DWORD __stdcall WaitForVerticalBlank32(d3dpt_ddhal_waitvb *d)
{
    static int said;

    if (!said) {
        said = 1;
        dbg_hex(&core, "d3dpthal: WaitForVerticalBlank, flags ", d->dwFlags);
        dbg_puts(&core, "\n");
    }
    d->ddRVal = DD_OK;
    if (!regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    switch (d->dwFlags) {
    case DDWAITVB_BLOCKBEGIN:
    case DDWAITVB_BLOCKEND:
        wait_frame(&core);
        break;
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = FALSE;
        break;
    default:
        d->ddRVal = DDERR_INVALIDPARAMS;
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall CanCreateSurface32(d3dpt_ddhal_cancreatesurface *d)
{
    static int said;

    if (!said) {
        said = 1;
        dbg_hex(&core, "d3dpthal: CanCreateSurface, caps ",
                d->lpDDSurfaceDesc ? d->lpDDSurfaceDesc->ddsCaps.dwCaps : 0);
        dbg_puts(&core, "\n");
    }
    if (!d->bIsDifferentPixelFormat) {
        d->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    if (core.d3d && d->lpDDSurfaceDesc && pf_format(&d->lpDDSurfaceDesc->ddpfPixelFormat) != 0) {
        d->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    d->ddRVal = DDERR_INVALIDPIXELFORMAT;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall CreateSurface32(d3dpt_ddhal_createsurface *d)
{
    DDSURFACEDESC *sd = d->lpDDSurfaceDesc;
    ULONG i, f;
    static int said;

    if (!said) {
        said = 1;
        dbg_hex(&core, "d3dpthal: CreateSurface, caps ",
                sd ? sd->ddsCaps.dwCaps : 0);
        dbg_hex(&core, " count ", d->dwSCnt);
        dbg_puts(&core, "\n");
    }
    d->ddRVal = DD_OK;
    if (!sd || !(sd->ddpfPixelFormat.dwFlags & DDPF_FOURCC) || !fmt_is_dxt(sd->ddpfPixelFormat.dwFourCC)) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    f = sd->ddpfPixelFormat.dwFourCC;
    for (i = 0; i < d->dwSCnt; i++) {
        LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(d->lplpSList[i]);
        LPDDRAWI_DDRAWSURFACE_GBL g = s ? s->lpGbl : NULL;
        ULONG size;

        if (!g) {
            continue;
        }
        size = surf_dxt_size(f, g->wWidth, g->wHeight);
        g->dwLinearSize = size;
        if (!(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            g->dwBlockSizeX = size;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        if (i == 0) {
            sd->dwFlags |= DDSD_LINEARSIZE;
            sd->dwLinearSize = size;
        }
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

/* ----------------------------------------------- command window serialisation */

static void cmd_lock_acquire(void)
{
    DWORD self = GetCurrentProcessId();

    if (hal) {
        if (hal->cmd_lock_owner == self) {
            hal->cmd_lock_depth++;
            return;
        }
        while (InterlockedCompareExchange((LONG *)&hal->cmd_lock, 1, 0) != 0) {
            Sleep(0);
        }
        hal->cmd_lock_owner = self;
        hal->cmd_lock_depth = 1;
    }
}

static void cmd_lock_release(void)
{
    if (hal) {
        if (--hal->cmd_lock_depth == 0) {
            hal->cmd_lock_owner = 0;
            InterlockedExchange((LONG *)&hal->cmd_lock, 0);
        }
    }
}

static DWORD __stdcall DestroyDriver32(d3dpt_ddhal_destroydriver *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall SetMode32(d3dpt_ddhal_setmode *d)
{
    const d3dpt_ddhal_modeinfo *mi;
    const d3dpt_ddhal_modeinfo *m;

    dbg_hex(&core, "d3dpthal: SetMode, index ", d ? d->dwModeIndex : 0);
    dbg_puts(&core, "\n");

    if (!d || !hal || d->dwModeIndex >= D3DPT_HAL9_MAX_MODES) {
        if (d) d->ddRVal = DDERR_INVALIDMODE;
        return DDHAL_DRIVER_HANDLED;
    }

    mi = (const d3dpt_ddhal_modeinfo *)hal->modeinfo;
    m = &mi[d->dwModeIndex];
    if (!m->dwWidth || !m->dwHeight || !m->dwBPP) {
        d->ddRVal = DDERR_INVALIDMODE;
        return DDHAL_DRIVER_HANDLED;
    }

    cmd_lock_acquire();

    if (regs) {
        regs[D3DPT_FB_REG_ENABLE / 4] = 0;
        regs[D3DPT_FB_REG_WIDTH / 4] = m->dwWidth;
        regs[D3DPT_FB_REG_HEIGHT / 4] = m->dwHeight;
        regs[D3DPT_FB_REG_BPP / 4] = m->dwBPP;
        regs[D3DPT_FB_REG_PITCH / 4] = m->lPitch;
        regs[D3DPT_FB_REG_OFFSET / 4] = 0;
        regs[D3DPT_FB_REG_ENABLE / 4] = 1;
    }

    hal->width = m->dwWidth;
    hal->height = m->dwHeight;
    hal->bpp = m->dwBPP;
    hal->pitch = m->lPitch;

    core.w = m->dwWidth;
    core.h = m->dwHeight;
    core.bpp = m->dwBPP;
    core.pitch = m->lPitch;

    cmd_lock_release();

    dbg_hex(&core, "d3dpthal: SetMode switched to ", (m->dwWidth << 16) | m->dwHeight);
    dbg_hex(&core, " bpp ", m->dwBPP);
    dbg_puts(&core, "\n");

    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DestroySurface32(d3dpt_ddhal_destroysurface *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);
    ULONG h = surf_handle(s);

    surf_forget(h);
    if (s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        cmd_lock_acquire();
        d3d_handle_op(&core, D3DPT_OP_VRAM_RELEASE, h);
        cmd_lock_release();
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall Flip32(d3dpt_ddhal_flip *d)
{
    ULONG offset;
    LPDDRAWI_DDRAWSURFACE_LCL curr;
    LPDDRAWI_DDRAWSURFACE_LCL targ;
    static int said;

    if (!regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    if (!said) {
        said = 1;
        dbg_hex(&core, "d3dpthal: Flip curr ", (ULONG)(ULONG_PTR)d->lpSurfCurr);
        dbg_hex(&core, " targ ", (ULONG)(ULONG_PTR)d->lpSurfTarg);
        dbg_puts(&core, "\n");
    }
    if (!flip_done(&core)) {
        if (!(d->dwFlags & DDFLIP_WAIT)) {
            d->ddRVal = DDERR_WASSTILLDRAWING;
            return DDHAL_DRIVER_HANDLED;
        }
        wait_frame(&core);
        core.flip_pending = FALSE;
    }
    curr = surf_lcl((void *)d->lpSurfCurr);
    targ = surf_lcl((void *)d->lpSurfTarg);
    if (d3d_ctx_live) {
        d3d_register_moved(&core, curr);
        d3d_register_moved(&core, targ);
        cmd_lock_acquire();
        d3d_readback(&core, surf_handle(targ));
        cmd_lock_release();
    }
    if (targ && targ->lpGbl) {
        offset = (ULONG)targ->lpGbl->fpVidMem;
        if (hal && offset >= hal->vram_linear) {
            offset -= hal->vram_linear;
        }
        regs[D3DPT_FB_REG_OFFSET / 4] = offset;
        flip_issued(&core);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall GetFlipStatus32(d3dpt_ddhal_getflipstatus *d)
{
    d->ddRVal = (regs && !flip_done(&core)) ? DDERR_WASSTILLDRAWING : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall GetBltStatus32(d3dpt_ddhal_getbltstatus *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static ULONG locks_said;

static DWORD __stdcall Lock32(d3dpt_ddhal_lock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);

    if (locks_said < 8) {
        locks_said++;
        dbg_hex(&core, "d3dpthal: Lock32 s=", (ULONG)(ULONG_PTR)s);
        dbg_hex(&core, " caps=", s ? s->ddsCaps.dwCaps : 0);
        dbg_hex(&core, " ctx_live=", d3d_ctx_live);
        dbg_puts(&core, "\n");
    }
    if (d3d_ctx_live && s && surf_is_target(s->ddsCaps.dwCaps)) {
        d3d_register_moved(&core, s);
        cmd_lock_acquire();
        d3d_readback(&core, surf_handle(s));
        cmd_lock_release();
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall Unlock32(d3dpt_ddhal_unlock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);

    if (core.d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        ((s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (d3d_ctx_live && surf_is_target(s->ddsCaps.dwCaps)))) {
        cmd_lock_acquire();
        d3d_handle_op(&core, D3DPT_OP_VRAM_DIRTY, surf_handle(s));
        cmd_lock_release();
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* Declined, always — this driver has no blitter and DirectDraw's own is
 * what draws. It logs, because "which blits does the runtime hand us, and
 * with what" is not answerable any other way and is the question behind a
 * 2D title that comes out wrong. First 24 only. */
static ULONG blts_said;

static DWORD __stdcall Blt32(d3dpt_ddhal_blt *d)
{
    if (blts_said < 24) {
        LPDDRAWI_DDRAWSURFACE_LCL dst = d ? surf_lcl(d->lpDDDestSurface) : NULL;
        LPDDRAWI_DDRAWSURFACE_LCL src = d ? surf_lcl(d->lpDDSrcSurface) : NULL;

        blts_said++;
        dbg_hex(&core, "d3dpthal: Blt flags ", d ? d->dwFlags : 0);
        dbg_hex(&core, " rop ", d ? d->dwROPFlags : 0);
        dbg_hex(&core, " dst caps ", dst ? dst->ddsCaps.dwCaps : 0);
        dbg_hex(&core, " src caps ", src ? src->ddsCaps.dwCaps : 0);
        if (src && src->lpGbl) {
            dbg_hex(&core, " src bpp ", src->lpGbl->ddpfSurface.dwRGBBitCount);
            dbg_hex(&core, " src pf ", src->lpGbl->ddpfSurface.dwFlags);
            dbg_hex(&core, " src rmask ", src->lpGbl->ddpfSurface.dwRBitMask);
        }
        dbg_puts(&core, "\n");
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

static ULONG keys_said;

static DWORD __stdcall SetColorKey32(d3dpt_ddhal_setcolorkey *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);
    BOOL ours;

    /* **Only a texture's key is ours.** This layer sets a colour key on the
     * host's copy of a *texture*, for the Direct3D path; it has no blitter,
     * so a key on any other surface is one DirectDraw's own blitter has to
     * apply. Saying DDHAL_DRIVER_HANDLED for those told the runtime the
     * hardware had taken the key when nothing had, and the key was then
     * applied by nobody. */
    ours = core.d3d && s && (d->dwFlags & DDCKEY_SRCBLT) &&
           !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
           (s->ddsCaps.dwCaps & DDSCAPS_TEXTURE);

    if (keys_said < 16) {
        keys_said++;
        dbg_hex(&core, "d3dpthal: SetColorKey flags ", d ? d->dwFlags : 0);
        dbg_hex(&core, " caps ", s ? s->ddsCaps.dwCaps : 0);
        dbg_hex(&core, " lo ", d ? d->ckNew.dwColorSpaceLowValue : 0);
        dbg_hex(&core, " hi ", d ? d->ckNew.dwColorSpaceHighValue : 0);
        dbg_puts(&core, ours ? " -> texture\n" : " -> DirectDraw's\n");
    }

    if (!ours) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    cmd_lock_acquire();
    surf_colorkey_set(&core, surf_handle(s), d->ckNew.dwColorSpaceLowValue, d->ckNew.dwColorSpaceHighValue);
    cmd_lock_release();
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ------------------------------------------------------------ Direct3D HAL */

static D3DHAL_CALLBACKS d3d_callbacks;

static const GUID guid_d3dcallbacks2 = {
    0x0ba584e1, 0x70b6, 0x11d0, { 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a }
};
static const GUID guid_d3dcallbacks3 = {
    0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e }
};
static const GUID guid_d3dextendedcaps = {
    0x7de41f80, 0x9d93, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 }
};
static const GUID guid_zpixelformats = {
    0x93869880, 0x36cf, 0x11d1, { 0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae }
};
static const GUID guid_misc2callbacks = {
    0x406b2f00, 0x3e5a, 0x11d1, { 0xb6, 0x40, 0x00, 0xaa, 0x00, 0xa1, 0xf9, 0x6a }
};
static const GUID guid_parseunknown = {
    0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 }
};
static const GUID guid_stereomode = {
    0xf828169c, 0xa8e8, 0x11d2, { 0xa1, 0xf2, 0x00, 0xa0, 0xc9, 0x83, 0xea, 0xf6 }
};

static inline BOOL guid_eq(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

static DWORD __stdcall ContextCreate32(D3DHAL_CONTEXTCREATEDATA *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL rt = surf_lcl(d->lpDDSLcl), z = surf_lcl(d->lpDDSZLcl);

    d->ddrval = DDERR_GENERIC;
    if (!core.d3d || !rt || !rt->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    cmd_lock_acquire();
    d3d_register_chain(&core, rt);
    if (z) {
        d3d_register_chain(&core, z);
    }
    d->ddrval = ctx_create(&core, &d->dwhContext, d->dwPID, surf_handle(rt), z ? surf_handle(z) : 0);
    cmd_lock_release();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall ContextDestroy32(D3DHAL_CONTEXTDESTROYDATA *d)
{
    if (core.d3d) {
        cmd_lock_acquire();
        d->ddrval = ctx_destroy_one(&core, d->dwhContext);
        cmd_lock_release();
    } else {
        d->ddrval = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall ContextDestroyAll32(D3DHAL_CONTEXTDESTROYALLDATA *d)
{
    if (core.d3d) {
        cmd_lock_acquire();
        ctx_destroy_all(&core, d->dwPID);
        cmd_lock_release();
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall SceneCapture32(D3DHAL_SCENECAPTUREDATA *d)
{
    if (core.d3d) {
        cmd_lock_acquire();
        d->ddrval = ctx_scene_capture(&core, d->dwhContext, d->dwFlag == D3DHAL_SCENE_CAPTURE_END);
        cmd_lock_release();
    } else {
        d->ddrval = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall SetRenderTarget32(D3DHAL_SETRENDERTARGETDATA *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL rt = surf_lcl(d->lpDDSLcl), z = surf_lcl(d->lpDDSZLcl);

    d->ddrval = DDERR_GENERIC;
    if (!core.d3d || !ctx_of(&core, d->dwhContext) || !rt || !rt->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    cmd_lock_acquire();
    d3d_register_chain(&core, rt);
    if (z) {
        d3d_register_chain(&core, z);
    }
    d->ddrval = ctx_set_render_target(&core, d->dwhContext, surf_handle(rt),
                                      z ? surf_handle(z) : 0);
    cmd_lock_release();
    return DDHAL_DRIVER_HANDLED;
}


static DWORD __stdcall Clear2_32(D3DHAL_CLEAR2DATA *d)
{
    if (!core.d3d) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    cmd_lock_acquire();
    d->ddrval = ctx_clear2(&core, d->dwhContext, d->dwFlags, d->dwFillColor, d->dvFillDepth,
                           d->dwFillStencil, d->lpRects, d->dwNumRects);
    cmd_lock_release();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall ValidateTextureStageState32(D3DHAL_VALIDATETEXTURESTAGESTATEDATA *d)
{
    d->dwNumPasses = 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall TextureCreate32(D3DHAL_TEXTURECREATEDATA *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(d->lpDDSLcl);

    if (!core.d3d || !s || !s->lpGbl) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    cmd_lock_acquire();
    d3d_register_chain(&core, s);
    d->dwHandle = surf_handle(s);
    d->ddrval = DD_OK;
    cmd_lock_release();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall TextureDestroy32(D3DHAL_TEXTUREDESTROYDATA *d)
{
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall TextureSwap32(D3DHAL_TEXTURESWAPDATA *d)
{
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall TextureGetSurf32(D3DHAL_TEXTUREGETSURFDATA *d)
{
    SURF *s;
    if (!core.d3d) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    s = surf_slot(d->dwHandle, FALSE);
    if (!s || !s->lcl) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    d->lpDDSLcl = (LPDDRAWI_DDRAWSURFACE_LCL)s->lcl;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DrawPrimitives2_32(D3DHAL_DRAWPRIMITIVES2DATA *d)
{
    d3dpt_dp2_call call;
    d3dpt_dp2_result res;
    LPDDRAWI_DDRAWSURFACE_LCL cmds = surf_lcl(d->lpDDCommands);
    LPDDRAWI_DDRAWSURFACE_LCL vtxs = (d->dwFlags & D3DHALDP2_USERMEMVERTICES) ? NULL : surf_lcl(d->lpDDVertex);

    if (!core.d3d || !cmds || !cmds->lpGbl) {
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    cmd_lock_acquire();
    memset(&call, 0, sizeof(call));
    call.ctx = d->dwhContext;
    call.cmd = (const UCHAR *)cmds->lpGbl->fpVidMem + d->dwCommandOffset;
    call.clen = d->dwCommandLength;
    if (d->dwFlags & D3DHALDP2_USERMEMVERTICES) {
        call.vtx = (const UCHAR *)d->lpVertices + d->dwVertexOffset;
    } else if (vtxs && vtxs->lpGbl) {
        call.vtx = (const UCHAR *)vtxs->lpGbl->fpVidMem + d->dwVertexOffset;
    }
    call.vsize = d->dwVertexSize;
    if (d->dwFlags & D3DHALDP2_EXECUTEBUFFER) {
        call.vsize = 32;
    } else if (call.vsize == 0 || call.vsize >= 0x10000) {
        call.vsize = fvf_stride(d->dwVertexType);
        if (!call.vsize) call.vsize = 32;
    }
    call.vcount = d->dwVertexLength;
    call.vlen = call.vtx ? call.vcount * call.vsize : 0;
    if (call.vlen > (32u << 20)) call.vlen = (32u << 20);
    call.vall = call.vlen;
    if (call.vtx && !(d->dwFlags & D3DHALDP2_USERMEMVERTICES) &&
        vtxs && vtxs->lpGbl && vtxs->lpGbl->dwLinearSize > d->dwVertexOffset) {
        call.vall = vtxs->lpGbl->dwLinearSize - d->dwVertexOffset;
        if (call.vall > (32u << 20)) call.vall = call.vlen;
    }
    call.flags = d->dwFlags;
    call.vertex_type = d->dwVertexType;
    call.eb = (d->dwFlags & D3DHALDP2_EXECUTEBUFFER) != 0;
    call.rstates = d->lpdwRStates;

    dp2_run(&core, &call, &res);
    d->ddrval = res.hr;
    d->dwErrorOffset = res.bounce ? d->dwCommandOffset + res.offset : res.offset;
    cmd_lock_release();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall CreateSurfaceEx32(DDHAL_CREATESURFACEEXDATA *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(d->lpDDSLcl);

    if (core.d3d && s && s->lpGbl) {
        d3d_register_chain(&core, s);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall GetDriverState32(DDHAL_GETDRIVERSTATEDATA *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---------------------------------------------- Execute Buffer callbacks */

static DWORD __stdcall CanCreateExecuteBuffer32(d3dpt_ddhal_cancreatesurface *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall CreateExecuteBuffer32(d3dpt_ddhal_createsurface *d)
{
    ULONG i;

    if (!core.d3d) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    if (!(ddflags(&core) & DDF_NO_HWVB) && d->lpDDSurfaceDesc &&
        (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) &&
        (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER) && d->lpDDSurfaceDesc->dwLinearSize &&
        d->dwSCnt && d->lplpSList) {
        for (i = 0; i < d->dwSCnt; i++) {
            LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(d->lplpSList[i]);
            LPDDRAWI_DDRAWSURFACE_GBL g = s ? s->lpGbl : NULL;
            if (!g) continue;
            g->dwLinearSize = d->lpDDSurfaceDesc->dwLinearSize;
            g->dwBlockSizeX = d->lpDDSurfaceDesc->dwLinearSize;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        d->lpDDSurfaceDesc->dwFlags |= DDSD_LINEARSIZE;
        d->ddRVal = DD_OK;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    for (i = 0; i < d->dwSCnt; i++) {
        LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl(d->lplpSList[i]);
        if (s) {
            s->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
            s->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
        }
    }
    if (d->lpDDSurfaceDesc) {
        d->lpDDSurfaceDesc->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
        d->lpDDSurfaceDesc->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall DestroyExecuteBuffer32(d3dpt_ddhal_destroysurface *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);
    ULONG h = surf_handle(s);

    surf_forget(h);
    if (core.d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        cmd_lock_acquire();
        d3d_handle_op(&core, D3DPT_OP_VRAM_RELEASE, h);
        cmd_lock_release();
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall LockExecuteBuffer32(d3dpt_ddhal_lock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);

    if (core.d3d && s && s->lpGbl && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        surf_lock_range(surf_handle(s), d->bHasRect, d->rArea.left, d->rArea.right);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall UnlockExecuteBuffer32(d3dpt_ddhal_unlock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = surf_lcl((void *)d->lpDDSurface);

    if (core.d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        cmd_lock_acquire();
        surf_unlock_dirty(&core, surf_handle(s));
        cmd_lock_release();
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --------------------------------------------------------- GetDriverInfo */

static void info_copy(DDHAL_GETDRIVERINFODATA *d, const void *src, ULONG n)
{
    ULONG i;
    if (n > d->dwExpectedSize) n = d->dwExpectedSize;
    for (i = 0; i < n; i++) ((UCHAR *)d->lpvData)[i] = ((const UCHAR *)src)[i];
    d->dwActualSize = n;
    d->ddRVal = DD_OK;
}

static void gdi2_answer(d3dpt_core *p, DDHAL_GETDRIVERINFODATA *d)
{
    DD_GETDRIVERINFO2DATA_ *g = (DD_GETDRIVERINFO2DATA_ *)d->lpvData;
    ULONG want = g->dwExpectedSize, n;

    switch (g->dwType) {
    case D3DGDI2_TYPE_GETD3DCAPS8_:
        n = sizeof(d3d_caps8);
        if (n > want) n = want;
        memcpy(d->lpvData, &d3d_caps8, n);
        d->dwActualSize = n;
        d->ddRVal = DD_OK;
        break;
    case D3DGDI2_TYPE_GETFORMATCOUNT_: {
        DD_GETFORMATCOUNTDATA_ *c = (DD_GETFORMATCOUNTDATA_ *)g;
        if (want < sizeof(*c)) { d->ddRVal = DDERR_CURRENTLYNOTAVAIL; break; }
        c->dwFormatCount = d3d_fmt8_n;
        d->dwActualSize = sizeof(*c);
        d->ddRVal = DD_OK;
        break;
    }
    case D3DGDI2_TYPE_GETFORMAT_: {
        DD_GETFORMATDATA_ *f = (DD_GETFORMATDATA_ *)g;
        if (want < sizeof(*f) || f->dwFormatIndex >= d3d_fmt8_n) { d->ddRVal = DDERR_CURRENTLYNOTAVAIL; break; }
        f->format = d3d_fmt8[f->dwFormatIndex];
        d->dwActualSize = sizeof(*f);
        d->ddRVal = DD_OK;
        break;
    }
    case D3DGDI2_TYPE_DXVERSION_: {
        DD_DXVERSION_ *v = (DD_DXVERSION_ *)g;
        d->dwActualSize = sizeof(*v) <= want ? sizeof(*v) : want;
        d->ddRVal = DD_OK;
        break;
    }
    default:
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        break;
    }
}

static DWORD __stdcall GetDriverInfo32(DDHAL_GETDRIVERINFODATA *d)
{
    dbg_hex(&core, "d3dpthal: dd getinfo ", d->guidInfo.Data1);
    dbg_hex(&core, " expected ", d->dwExpectedSize);
    dbg_puts(&core, "\n");
    if (core.d3d && (ddflags(&core) & DDF_NO_D3D_INFO)) {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    } else if (core.d3d && !(ddflags(&core) & DDF_NO_D3D_CB3) && guid_eq(&d->guidInfo, &guid_d3dcallbacks3)) {
        D3DHAL_CALLBACKS3 cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DHAL3_CB32_CLEAR2 | D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE | D3DHAL3_CB32_DRAWPRIMITIVES2;
        cb.Clear2 = Clear2_32;
        cb.ValidateTextureStageState = ValidateTextureStageState32;
        cb.DrawPrimitives2 = DrawPrimitives2_32;
        info_copy(d, &cb, sizeof(cb));
    } else if (core.d3d && guid_eq(&d->guidInfo, &guid_d3dcallbacks2)) {
        D3DHAL_CALLBACKS2 cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        /* SetRenderTarget only: Clear belongs to CALLBACKS3 as Clear2, and the
         * CALLBACKS2 Clear was never entered on a DirectX 7+ runtime (the NT
         * layer has never published it either). */
        cb.dwFlags = D3DHAL2_CB32_SETRENDERTARGET;
        cb.SetRenderTarget = SetRenderTarget32;
        info_copy(d, &cb, sizeof(cb));
    } else if (core.d3d && guid_eq(&d->guidInfo, &guid_d3dextendedcaps)) {
        D3DHAL_D3DEXTENDEDCAPS_ ext = d3d_extcaps;
        ext.dwSize = d->dwExpectedSize <= sizeof(ext) ? d->dwExpectedSize : sizeof(ext);
        info_copy(d, &ext, ext.dwSize);
    } else if (core.d3d && guid_eq(&d->guidInfo, &guid_zpixelformats)) {
        info_copy(d, &d3d_zformats, sizeof(d3d_zformats));
    } else if (core.d3d && !(ddflags(&core) & DDF_NO_MISC2) && guid_eq(&d->guidInfo, &guid_misc2callbacks)) {
        DDHAL_DDMISCELLANEOUS2CALLBACKS cb;
        memset(&cb, 0, sizeof(cb));
        cb.dwSize = sizeof(cb);
        cb.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE;
        cb.CreateSurfaceEx = (LPDDHAL_CREATESURFACEEX)CreateSurfaceEx32;
        cb.GetDriverState = (LPDDHAL_GETDRIVERSTATE)GetDriverState32;
        info_copy(d, &cb, sizeof(cb));
    } else if (core.d3d && !(ddflags(&core) & DDF_NO_PARSEUNKNOWN) && guid_eq(&d->guidInfo, &guid_parseunknown)) {
        core.parse_unknown = (HRESULT (WINAPI *)(PVOID, PVOID *))d->lpvData;
        d->dwActualSize = d->dwExpectedSize;
        d->ddRVal = DD_OK;
    } else if (core.d3d && !(ddflags(&core) & DDF_NO_DX8) && guid_eq(&d->guidInfo, &guid_stereomode) &&
               d->lpvData && d->dwExpectedSize >= sizeof(DD_GETDRIVERINFO2DATA_) &&
               ((DD_GETDRIVERINFO2DATA_ *)d->lpvData)->dwMagic == D3DGDI2_MAGIC_) {
        gdi2_answer(&core, d);
    } else {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* ------------------------------------------------------------ DriverInit */

DWORD __stdcall DriverInit(LPVOID ptr)
{
    d3dpt_hal9 *h = (d3dpt_hal9 *)ptr;

    if (!h) {
        return 0;
    }
    if (h->magic != D3DPT_HAL9_MAGIC || h->size != sizeof(d3dpt_hal9)) {
        return 0;
    }
    regs = (volatile ULONG *)h->regs_linear;
    if (h->version != D3DPT_HAL9_VERSION) {
        dbg_hex(&core, "d3dpthal: shared block version ", h->version);
        dbg_hex(&core, " but this DLL speaks ", D3DPT_HAL9_VERSION);
        dbg_puts(&core, " — install both halves from the same guest-tools ISO\n");
        regs = 0;
        return 0;
    }
    hal = h;
    h->dll_reg_magic = regs[D3DPT_FB_REG_MAGIC / 4];
    h->dll_reg_version = regs[D3DPT_FB_REG_VERSION / 4];

    core.regs = regs;
    core.fb = (PVOID)h->vram_linear;
    core.fb_len = h->vram_size;
    core.w = h->width;
    core.h = h->height;
    core.bpp = h->bpp;
    core.pitch = h->pitch;
    /* The adapter says where its command window is; deriving it from the
     * VRAM size is how the two layers drift. This layer did derive it, and
     * subtracted the cursor as well, so it encoded batches 16 KiB below the
     * window the device reads — every call then succeeds against an
     * untouched header and every frame comes back black (ebtest 5/5 failed,
     * with no `ddi:` line on the host at all). The NT layer has always read
     * the register; do the same. */
    core.cmd_offset = (regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_D3D) ?
                      regs[D3DPT_FB_REG_CMD_OFFSET / 4] : 0;
    d3d_init(&core);

    dbg_hex(&core, "d3dpthal: DriverInit, block at ", (ULONG)(ULONG_PTR)h);
    dbg_hex(&core, " regs ", h->regs_linear);
    dbg_hex(&core, " vram ", h->vram_linear);
    dbg_hex(&core, " mode ", (h->width << 16) | h->height);
    dbg_hex(&core, " bpp ", h->bpp);
    dbg_puts(&core, "\n");

    if ((ULONG)(ULONG_PTR)dll_instance < 0x80000000ul) {
        dbg_hex(&core, "d3dpthal: relocated out of the shared arena, to ",
                (ULONG)(ULONG_PTR)dll_instance);
        dbg_puts(&core, " — the callbacks would be bad pointers in every game\n");
    }
    h->dll_hinstance = (unsigned long)(ULONG_PTR)dll_instance;
    h->cb32.WaitForVerticalBlank = (unsigned long)(ULONG_PTR)WaitForVerticalBlank32;
    h->cb32.CanCreateSurface     = (unsigned long)(ULONG_PTR)CanCreateSurface32;
    h->cb32.CreateSurface        = (unsigned long)(ULONG_PTR)CreateSurface32;
    h->cb32.SetMode              = (unsigned long)(ULONG_PTR)SetMode32;
    h->cb32.DestroyDriver        = (unsigned long)(ULONG_PTR)DestroyDriver32;
    h->cb32.DestroySurface       = (unsigned long)(ULONG_PTR)DestroySurface32;
    h->cb32.Flip                 = (unsigned long)(ULONG_PTR)Flip32;
    h->cb32.GetFlipStatus        = (unsigned long)(ULONG_PTR)GetFlipStatus32;
    h->cb32.GetBltStatus         = (unsigned long)(ULONG_PTR)GetBltStatus32;
    h->cb32.Lock                 = (unsigned long)(ULONG_PTR)Lock32;
    h->cb32.Unlock               = (unsigned long)(ULONG_PTR)Unlock32;
    h->cb32.Blt                  = (unsigned long)(ULONG_PTR)Blt32;
    h->cb32.SetColorKeySurface   = (unsigned long)(ULONG_PTR)SetColorKey32;
    h->cb32.GetDriverInfo        = (unsigned long)(ULONG_PTR)GetDriverInfo32;
    h->cb32.CanCreateExecuteBuffer = (unsigned long)(ULONG_PTR)CanCreateExecuteBuffer32;
    h->cb32.CreateExecuteBuffer    = (unsigned long)(ULONG_PTR)CreateExecuteBuffer32;
    h->cb32.DestroyExecuteBuffer   = (unsigned long)(ULONG_PTR)DestroyExecuteBuffer32;
    h->cb32.LockExecuteBuffer      = (unsigned long)(ULONG_PTR)LockExecuteBuffer32;
    h->cb32.UnlockExecuteBuffer    = (unsigned long)(ULONG_PTR)UnlockExecuteBuffer32;

    d3d_callbacks.dwSize = sizeof(d3d_callbacks);
    d3d_callbacks.ContextCreate = ContextCreate32;
    d3d_callbacks.ContextDestroy = ContextDestroy32;
    d3d_callbacks.ContextDestroyAll = ContextDestroyAll32;
    d3d_callbacks.SceneCapture = SceneCapture32;
    d3d_callbacks.TextureCreate = TextureCreate32;
    d3d_callbacks.TextureDestroy = TextureDestroy32;
    d3d_callbacks.TextureSwap = TextureSwap32;
    d3d_callbacks.TextureGetSurf = TextureGetSurf32;
    h->d3dhal_global = (unsigned long)(ULONG_PTR)&d3d_global;
    h->d3dhal_callbacks = (unsigned long)(ULONG_PTR)&d3d_callbacks;

    h->dll_ready = 1;

    {
        char name[128];
        DWORD n = GetModuleFileNameA(NULL, name, sizeof(name) - 1);

        name[n < sizeof(name) ? n : sizeof(name) - 1] = 0;
        dbg_puts(&core, "d3dpthal:   in ");
        dbg_puts(&core, name);
        dbg_hex(&core, " pid ", GetCurrentProcessId());
        dbg_puts(&core, "\n");
    }
    dbg_hex(&core, "d3dpthal:   published vblank ", h->cb32.WaitForVerticalBlank);
    dbg_hex(&core, " cansurf ", h->cb32.CanCreateSurface);
    dbg_hex(&core, " setmode ", h->cb32.SetMode);
    dbg_hex(&core, " flip ", h->cb32.Flip);
    dbg_hex(&core, " getinfo ", h->cb32.GetDriverInfo);
    dbg_hex(&core, " d3dhal ", h->d3dhal_callbacks);
    dbg_hex(&core, " hinstance ", h->dll_hinstance);
    dbg_puts(&core, "\n");
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


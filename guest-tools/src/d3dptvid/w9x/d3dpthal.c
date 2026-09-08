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

BOOL d3dpt_os_surf(d3dpt_core *c, void *os, d3dpt_surf_desc *out)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)os;

    if (!s || !s->lpGbl) {
        return FALSE;
    }
    out->os = os;
    out->handle = (s->lpSurfMore) ? s->lpSurfMore->dwSurfaceHandle : 0;
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
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) {
        out->fmt = pf_format(&s->lpGbl->ddpfSurface);
        out->pf_flags = s->lpGbl->ddpfSurface.dwFlags;
    } else {
        out->fmt = c->bpp == 32 ? D3DFMT_X8R8G8B8_ : D3DFMT_R5G6B5_;
        out->pf_flags = 0xffffffffu;
    }
    out->ck_lo = s->ddckCKSrcBlt.dwColorSpaceLowValue;
    out->ck_hi = s->ddckCKSrcBlt.dwColorSpaceHighValue;
    out->ck_dst_lo = s->ddckCKDestBlt.dwColorSpaceLowValue;
    return TRUE;
}

ULONG d3dpt_os_attached(void *os, void **out, ULONG max)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)os;
    LPATTACHLIST a;
    ULONG n = 0;

    for (a = s->lpAttachList; a && n < max; a = a->lpLink) {
        LPDDRAWI_DDRAWSURFACE_LCL t = a->lpAttached;
        if (t && t->lpGbl && !(t->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) {
            out[n++] = t;
        }
    }
    return n;
}

void *d3dpt_os_next_mip(void *os)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)os;
    LPATTACHLIST a;

    for (a = s->lpAttachList; a; a = a->lpLink) {
        LPDDRAWI_DDRAWSURFACE_LCL t = a->lpAttached;
        if (t && t->lpGbl && (t->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && t != s &&
            (t->lpGbl->wWidth < s->lpGbl->wWidth || t->lpGbl->wHeight < s->lpGbl->wHeight)) {
            return t;
        }
    }
    return NULL;
}

static inline ULONG surf_handle(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    return (s && s->lpSurfMore) ? s->lpSurfMore->dwSurfaceHandle : 0;
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
        LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)d->lplpSList[i];
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

static DWORD __stdcall DestroyDriver32(d3dpt_ddhal_destroydriver *d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DestroySurface32(d3dpt_ddhal_destroysurface *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpDDSurface;
    ULONG h = surf_handle(s);

    surf_forget(h);
    if (s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        d3d_handle_op(&core, D3DPT_OP_VRAM_RELEASE, h);
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
    curr = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpSurfCurr;
    targ = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpSurfTarg;
    if (d3d_ctx_live) {
        d3d_register_moved(&core, curr);
        d3d_register_moved(&core, targ);
        d3d_readback(&core, surf_handle(targ));
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

static DWORD __stdcall Lock32(d3dpt_ddhal_lock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpDDSurface;

    if (d3d_ctx_live && s && surf_is_target(s->ddsCaps.dwCaps)) {
        d3d_register_moved(&core, s);
        d3d_readback(&core, surf_handle(s));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall Unlock32(d3dpt_ddhal_unlock *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpDDSurface;

    if (core.d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        ((s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (d3d_ctx_live && surf_is_target(s->ddsCaps.dwCaps)))) {
        d3d_handle_op(&core, D3DPT_OP_VRAM_DIRTY, surf_handle(s));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall Blt32(d3dpt_ddhal_blt *d)
{
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall SetColorKey32(d3dpt_ddhal_setcolorkey *d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = (LPDDRAWI_DDRAWSURFACE_LCL)d->lpDDSurface;

    if (core.d3d && s && (d->dwFlags & DDCKEY_SRCBLT) && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        (s->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) {
        surf_colorkey_set(&core, surf_handle(s), d->ckNew.dwColorSpaceLowValue, d->ckNew.dwColorSpaceHighValue);
    }
    d->ddRVal = DD_OK;
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
    core.cmd_offset = (h->vram_size > D3DPT_FB_CURSOR_BYTES + D3DPT_SHM_SIZE) ?
                      h->vram_size - D3DPT_FB_CURSOR_BYTES - D3DPT_SHM_SIZE : 0;
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
    h->cb32.DestroyDriver        = (unsigned long)(ULONG_PTR)DestroyDriver32;
    h->cb32.DestroySurface       = (unsigned long)(ULONG_PTR)DestroySurface32;
    h->cb32.Flip                 = (unsigned long)(ULONG_PTR)Flip32;
    h->cb32.GetFlipStatus        = (unsigned long)(ULONG_PTR)GetFlipStatus32;
    h->cb32.GetBltStatus         = (unsigned long)(ULONG_PTR)GetBltStatus32;
    h->cb32.Lock                 = (unsigned long)(ULONG_PTR)Lock32;
    h->cb32.Unlock               = (unsigned long)(ULONG_PTR)Unlock32;
    h->cb32.Blt                  = (unsigned long)(ULONG_PTR)Blt32;
    h->cb32.SetColorKeySurface   = (unsigned long)(ULONG_PTR)SetColorKey32;
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
    dbg_hex(&core, " flip ", h->cb32.Flip);
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


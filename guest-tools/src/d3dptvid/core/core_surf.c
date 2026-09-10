/*
 * core_surf.c — the surface table and everything the driver knows about
 * a surface (doc 19, "The split"): handles, mip levels, formats and
 * their row / pitch arithmetic (DXT included), VRAM offsets, the
 * registrations the host is told about, the colour-key bookkeeping, the
 * dirty ranges of a video-memory buffer, and readback.
 *
 * The OS's own surface object never appears here. The layer fills a
 * d3dpt_surf_desc (d3dpt_os_surf) and walks the attachment lists
 * (d3dpt_os_attached, d3dpt_os_next_mip); NT's DD_SURFACE_LOCAL and 9x's
 * DDRAWI_DDRAWSURFACE_LCL hold the same facts at different offsets, and
 * that hook is where they meet. The core does keep the object as an
 * opaque pointer (SURF::lcl), because a colour key set later is read off
 * it when the texture is next bound.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

/* --- the surface table (DX8 DDI): every surface dxg told us about, by
 * handle, VRAM and system memory alike. The DX8 tokens name vertex / index
 * buffers and the system-memory side of a TEXBLT by handle; the memory is
 * the caller's (user-mode pointers for system-memory surfaces, read only
 * inside DrawPrimitives2, which runs in the caller's context) --- */

static SURF *surf_tab;
static ULONG surf_tab_n;

SURF *surf_slot(ULONG handle, BOOL create)
{
    if (handle >= surf_tab_n) {
        SURF *t;
        ULONG n = surf_tab_n ? surf_tab_n : 256;

        if (!create || handle >= 0x100000) {
            return NULL;
        }
        while (n <= handle) n *= 2;
        t = d3dpt_os_alloc(n * sizeof(SURF));
        if (!t) {
            return NULL;
        }
        if (surf_tab) {
            memcpy(t, surf_tab, surf_tab_n * sizeof(SURF));
            d3dpt_os_free(surf_tab);
        }
        surf_tab = t;
        surf_tab_n = n;
    }
    if (!create && !surf_tab[handle].used) {
        return NULL;
    }
    return &surf_tab[handle];
}

/* bytes of one row of w pixels (blocks for DXT); 0 = unknown format */
ULONG fmt_row_bytes(ULONG f, ULONG w)
{
    switch (f) {
    case D3DFMT_X8R8G8B8_: case D3DFMT_A8R8G8B8_: case D3DFMT_D24X8_: case D3DFMT_D24S8_: case D3DFMT_D32_:
        return w * 4;
    case D3DFMT_R5G6B5_: case D3DFMT_X1R5G5B5_: case D3DFMT_A1R5G5B5_: case D3DFMT_A4R4G4B4_: case D3DFMT_X4R4G4B4_:
    case D3DFMT_D16_: case D3DFMT_D15S1_:
        return w * 2;
    case D3DFMT_P8_:
        return w;
    default:
        if (f == FOURCC_('D', 'X', 'T', '1')) return ((w + 3) / 4) * 8;
        if (f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
            f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5')) return ((w + 3) / 4) * 16;
        return 0;
    }
}

BOOL fmt_is_dxt(ULONG f)
{
    return f == FOURCC_('D', 'X', 'T', '1') || f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
           f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5');
}

/* the row pitch of a surface: dxg's lPitch, except that for a compressed
 * format the union holds the linear size (DDSD_LINEARSIZE), and a block
 * row is what the copies and the host want */
ULONG surf_pitch(ULONG fmt, ULONG w, ULONG lpitch)
{
    return fmt_is_dxt(fmt) ? fmt_row_bytes(fmt, w) : lpitch;
}

ULONG surf_rows(ULONG fmt, ULONG h)
{
    return fmt_is_dxt(fmt) ? (h + 3) / 4 : h;
}


/* the bytes a compressed surface of this size occupies, which dxg has to
 * be told before it can take one out of the heap (the layer's
 * DdCreateSurface) */
ULONG surf_dxt_size(ULONG fourcc, ULONG w, ULONG h)
{
    return fmt_row_bytes(fourcc, w) * ((h + 3) / 4);
}

/* a render target, a primary or a member of a flip chain: the surfaces
 * the host may have drawn into and so must read back before a lock */
BOOL surf_is_target(ULONG caps)
{
    return (caps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                    DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) != 0;
}

/* A Lock on a video-memory buffer: what the runtime locked — the byte
 * range it gave, or the whole buffer when it gave none — kept until the
 * Unlock reports it dirty. DISCARD / NOOVERWRITE need nothing: every draw
 * before the Lock has run, because a DP2 record executes in the doorbell
 * write. */
void surf_lock_range(ULONG handle, BOOL has_rect, LONG left, LONG right)
{
    SURF *t = surf_slot(handle, FALSE);

    if (!t || !t->buffer) {
        return;
    }
    t->lock_off = 0;
    t->lock_len = t->size;
    if (has_rect && left >= 0 && right > left && (ULONG)right <= t->size) {
        t->lock_off = (ULONG)left;
        t->lock_len = (ULONG)(right - left);
    }
}

/* and the Unlock: the locked range is the guest's now (v9) */
void surf_unlock_dirty(d3dpt_core *p, ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);

    if (p->d3d && t && t->buffer && !t->sysmem) {
        d3d_dirty_range(p, handle, t->lock_off, t->lock_len);
    }
}

/* The runtime set a source colour key on a video-memory texture: the
 * host keys the texels as it expands them, so it hears the key once, and
 * the table remembers it so surf_colorkey_check does not say it again. */
void surf_colorkey_set(d3dpt_core *p, ULONG handle, ULONG lo, ULONG hi)
{
    SURF *t = surf_slot(handle, FALSE);

    if (t) {
        t->ck_on = 1;
        t->ck_lo = lo;
        t->ck_hi = hi;
    }
    d3d_colorkey_op(p, handle, lo, hi, 1);
}

/* the handle is gone (the OS destroyed the surface) */
void surf_forget(ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);

    if (t) {
        t->used = 0;
    }
}

void d3d_colorkey_op(d3dpt_core *p, ULONG handle, ULONG lo, ULONG hi, ULONG flags)
{
    d3dpt_u32x4 *k;

    if (!p->d3d || !handle) {
        return;
    }
    k = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_COLORKEY, sizeof(*k), 0);
    if (k) {
        k->a = handle;
        k->b = lo;
        k->c = hi;
        k->d = flags;
    }
}

void d3d_handle_op(d3dpt_core *p, ULONG op, ULONG handle)
{
    d3dpt_handle *h;

    if (!p->d3d || !handle) {
        return;
    }
    h = d3dpt_enc_cmd(&p->enc, op, sizeof(*h), 0);
    if (h) {
        h->handle = handle;
        h->pad = 0;
    }
}

/* VRAM_DIRTY_RANGE (v9): the guest wrote len bytes at off of a VRAM buffer */
void d3d_dirty_range(d3dpt_core *p, ULONG handle, ULONG off, ULONG len)
{
    d3dpt_u32x3 *r;

    if (!p->d3d || !handle || !len) {
        return;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_DIRTY_RANGE, sizeof(*r), 0);
    if (r) {
        r->a = handle;
        r->b = off;
        r->c = len;
        r->pad = 0;
    }
}


/* VRAM_SURFACE for s at the given VRAM offset (its own, or the one a flip hands it) */
void d3d_register_at(d3dpt_core *p, const d3dpt_surf_desc *s, ULONG offset, BOOL quiet)
{
    d3dpt_vram_surface *r;
    d3dpt_u32x2 lv[15];
    ULONG handle, fmt, caps, n = 1, i;
    void *m;
    BOOL sysmem, buffer;
    SURF *t;
    ULONG pitch0, rows0, bsize;

    if (!p->d3d || !s) {
        return;
    }
    handle = s->handle;
    fmt = s->fmt;
    caps = 0;
    if (s->caps & DDSCAPS_TEXTURE) caps |= D3DPT_VS_TEXTURE;
    if (s->caps & DDSCAPS_ZBUFFER) caps |= D3DPT_VS_ZBUFFER;
    if (s->caps & DDSCAPS_3DDEVICE) caps |= D3DPT_VS_RENDER_TARGET;
    if (s->caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) caps |= D3DPT_VS_PRIMARY;
    pitch0 = surf_pitch(fmt, s->w, s->pitch);
    rows0 = surf_rows(fmt, s->h);
    sysmem = (s->caps & DDSCAPS_SYSTEMMEMORY) != 0;
    buffer = (s->caps & DDSCAPS_EXECUTEBUFFER_) != 0 ||
             (s->caps2 & (DDSCAPS2_VERTEXBUFFER_ | DDSCAPS2_INDEXBUFFER_)) != 0;
    bsize = buffer ? s->linear : 0;
    if (buffer) {
        /* a vertex / index buffer: no pixel format; in VRAM it is the host's
         * D3DPT_VS_BUFFER (v9), a byte range a DRAW8 names */
        fmt = 0;
        caps = D3DPT_VS_BUFFER;
    }
    /* one line per surface in the QEMU log: what the host will know it as
     * (a "skipped" surface is one a later SETRENDERTARGET / TEXTUREMAP
     * would report unknown) */
    if (!quiet && p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: surface ", handle);
        dbg_hex(p, " caps ", s->caps);
        dbg_hex(p, " w ", s->w);
        dbg_hex(p, " h ", s->h);
        dbg_hex(p, " fmt ", fmt);
        dbg_hex(p, " pitch ", s->pitch);
        dbg_hex(p, " pf ", s->pf_flags);
        dbg_hex(p, " at ", offset);
        if (buffer) dbg_hex(p, " buffer of ", s->linear);
        if (sysmem) dbg_puts(p, " sysmem");
        else if (!fmt && !buffer) dbg_puts(p, " no format, skipped");
        dbg_puts(p, "\n");
    }
    if (!handle) {
        return;
    }
    if (caps & D3DPT_VS_TEXTURE) {
        for (m = d3dpt_os_next_mip(s->os); m && n < 16; m = d3dpt_os_next_mip(m)) {
            d3dpt_surf_desc d;

            if (!d3dpt_os_surf(p, m, &d)) {
                break;
            }
            lv[n - 1].a = d.vidmem;
            lv[n - 1].b = surf_pitch(fmt, d.w, d.pitch);
            n++;
        }
    }
    /* the table entry (system-memory surfaces live only here) */
    t = surf_slot(handle, TRUE);
    if (t) {
        t->used = 1;
        t->lcl = s->os;
        t->ck_on = 0xff;
        t->sysmem = sysmem;
        t->buffer = buffer;
        t->mem = sysmem ? (ULONG_PTR)s->vidmem : (ULONG_PTR)p->fb + offset;
        t->pitch = pitch0;
        t->w = s->w;
        t->h = s->h;
        t->fmt = fmt;
        t->size = buffer ? bsize : pitch0 * rows0;
        t->levels = (UCHAR)n;
        t->vram_off = offset;
        t->lock_off = 0;
        t->lock_len = t->size;
        for (i = 0; i + 1 < n; i++) {
            t->lv[i].mem = sysmem ? (ULONG_PTR)lv[i].a : (ULONG_PTR)p->fb + lv[i].a;
            t->lv[i].pitch = lv[i].b;
        }
    }
    if (sysmem) {
        return;
    }
    if (buffer ? (!bsize || (ULONGLONG)offset + bsize > heap_end(p))
               : (!fmt || (ULONGLONG)offset + (ULONGLONG)pitch0 * rows0 > heap_end(p))) {
        return;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*r), (n - 1) * sizeof(d3dpt_u32x2));
    if (!r) {
        return;
    }
    r->handle = handle;
    r->offset = offset;
    r->width = buffer ? bsize : s->w;       /* a buffer: width = pitch = its bytes, one row */
    r->height = buffer ? 1 : s->h;
    r->pitch = buffer ? bsize : pitch0;
    r->format = fmt;
    r->caps = caps;
    r->levels = n;
    for (i = 0; i + 1 < n; i++) ((d3dpt_u32x2 *)(r + 1))[i] = lv[i];
}

void d3d_register(d3dpt_core *p, const d3dpt_surf_desc *s)
{
    d3d_register_at(p, s, s->vidmem, FALSE);
}

/* The texture's source colour key, read off the OS's surface when a
 * TEXTURESTAGESTATE binds it (the HASCKEYSRCBLT flag + the key itself, as
 * the DDK's sample drivers do). The runtime records it there — and calls
 * the driver's SetColorKey — only for a driver with the colour-key
 * DirectDraw caps; this check covers a key set before the surface was
 * mirrored and a surface re-created under the same handle. The host hears
 * of it once per change. */
void surf_colorkey_check(d3dpt_core *p, ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);
    d3dpt_surf_desc s;
    UCHAR on;
    ULONG lo, hi;

    if (!t || !t->lcl || t->sysmem || t->buffer || !d3dpt_os_surf(p, t->lcl, &s)) {
        return;
    }
    on = (s.flags & DDRAWISURF_HASCKEYSRCBLT_) ? 1 : 0;
    lo = on ? s.ck_lo : 0;
    hi = on ? s.ck_hi : 0;
    if (t->ck_on == 0xff && p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: texture ", handle);
        dbg_hex(p, " bound, surface flags ", s.flags);
        dbg_hex(p, " src key ", s.ck_lo);
        dbg_hex(p, "..", s.ck_hi);
        dbg_hex(p, " dst key ", s.ck_dst_lo);
        dbg_puts(p, "\n");
    }
    if (t->ck_on == on && t->ck_lo == lo && t->ck_hi == hi) {
        return;
    }
    if (t->ck_on == 0xff && !on) {
        t->ck_on = 0;               /* never keyed: nothing to tell */
        return;
    }
    t->ck_on = on;
    t->ck_lo = lo;
    t->ck_hi = hi;
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: colour key of surface ", handle);
        dbg_hex(p, on ? " on " : " off ", lo);
        dbg_hex(p, "..", hi);
        dbg_puts(p, "\n");
    }
    d3d_colorkey_op(p, handle, lo, hi, on);
}

/* s and what is attached to it — a flip chain's other buffers, a Z buffer
 * — each under its own handle, the ones the host does not know yet or
 * knows at another offset. The runtime's CreateSurfaceEx comes for the
 * root of a complex surface; a DirectX 7 interface's flip chain gets one
 * call per member as well, a DirectX 6 title's arrives as its primary
 * alone (GTA 2, 2026-09-05): the back buffer, handle 2, was never
 * registered, the runtime's SETRENDERTARGET 2 was unknown to the host,
 * every frame went into the front buffer's VRAM while the flips
 * alternated the scanout, and half the frames showed the buffer nobody
 * drew. A flip chain's attach list is a ring; mip levels go with their
 * texture (d3d_register_at), so the layer leaves them out. */
void d3d_register_chain(d3dpt_core *p, void *os)
{
    void *seen[8];
    ULONG n = 0, i, j;
    d3dpt_surf_desc d;

    if (!d3dpt_os_surf(p, os, &d)) {
        return;
    }
    d3d_register(p, &d);
    seen[n++] = os;
    for (i = 0; i < n; i++) {
        void *att[8];
        ULONG na = d3dpt_os_attached(seen[i], att, 8), k;

        for (k = 0; k < na && n < 8; k++) {
            SURF *slot;

            for (j = 0; j < n && seen[j] != att[k]; j++) {
            }
            if (j < n || !d3dpt_os_surf(p, att[k], &d)) {
                continue;
            }
            seen[n++] = att[k];
            slot = (d.caps & DDSCAPS_SYSTEMMEMORY) ? NULL : surf_slot(d.handle, FALSE);
            if (!slot || slot->mem != (ULONG_PTR)p->fb + d.vidmem) {
                d3d_register(p, &d);
            }
        }
    }
}

/* a video-memory surface the host knows at another offset: registered
 * again where it is now, silently. Under the runtime's flip model nothing
 * moves (DdFlip); a runtime that swaps two buffers' memory instead is
 * caught here at the next flip or lock */
void d3d_register_moved(d3dpt_core *p, void *os)
{
    d3dpt_surf_desc d;
    SURF *k;

    if (!p->d3d || !os || !d3dpt_os_surf(p, os, &d) || (d.caps & DDSCAPS_SYSTEMMEMORY)) {
        return;
    }
    k = surf_slot(d.handle, FALSE);
    if (!k) {
        d3d_register(p, &d);
    } else if (k->mem != (ULONG_PTR)p->fb + d.vidmem) {
        d3d_register_at(p, &d, d.vidmem, TRUE);
    }
}

/* the host's rendering into the surface, into its VRAM (S_FALSE = nothing pending) */
static ULONG readbacks_said;

HRESULT d3d_readback(d3dpt_core *p, ULONG handle)
{
    HRESULT hr;
    if (!p->d3d || !handle) {
        return DD_OK;
    }
    hr = (HRESULT)d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, handle);
    /* a readback is per-frame, so say the first few and then be quiet: the
     * thing this ever has to answer is whether readbacks happen at all */
    if (readbacks_said < 8) {
        readbacks_said++;
        dbg_hex(p, "d3dptdisp: d3d_readback handle ", handle);
        dbg_hex(p, " -> ", (ULONG)hr);
        dbg_puts(p, "\n");
    }
    return hr;
}

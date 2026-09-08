/*
 * core_ctx.c — Direct3D contexts (doc 19, "The split"): the table of
 * render-target / Z pairs the host keeps for us, the render target a
 * context draws into, Clear2, and the readback at EndScene that puts the
 * frame where Flip / Blt / Lock expect it.
 *
 * The table is global rather than per-device on purpose: a game's
 * exclusive mode switch gives GDI a new device object (the context is
 * created on that one), and its switch back at exit another, before the
 * runtime's ContextDestroyAll for the process arrives; a table in the
 * device object was empty by then, the host kept the context, and the
 * game's next run had its CTX_CREATE of the same handle refused — E_FAIL
 * from CreateDevice, a crash (GTA 2, 2026-09-05).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

D3DCTX d3d_ctx[D3D_MAX_CTX];
ULONG d3d_ctx_live;

D3DCTX *ctx_of(d3dpt_core *p, ULONG_PTR h)
{
    (void)p;
    if (h == 0 || h > D3D_MAX_CTX || !d3d_ctx[h - 1].used) {
        return NULL;
    }
    return &d3d_ctx[h - 1];
}

/* A context for the render target rt and the Z buffer z (both already
 * registered by the layer, which knows how to reach the surfaces). On
 * success *handle is the context the runtime will name from now on. */
HRESULT ctx_create(d3dpt_core *p, ULONG_PTR *handle, ULONG pid, ULONG rt, ULONG z)
{
    d3dpt_ctx_create *c;
    ULONG i, off, hr;

    if (!p || !p->d3d) {
        return DDERR_GENERIC;
    }
    for (i = 0; i < D3D_MAX_CTX && d3d_ctx[i].used; i++) {
    }
    if (i == D3D_MAX_CTX) {
        dbg_puts(p, "d3dptdisp: out of contexts\n");
        return DDERR_GENERIC;
    }
    off = d3dpt_enc_ret(&p->enc, 0);
    c = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CREATE, sizeof(*c), 0);
    if (!c) {
        return DDERR_GENERIC;
    }
    c->handle = i + 1;
    c->ret_off = off;
    c->rt = rt;
    c->z = z;
    d3dpt_enc_flush(&p->enc);
    hr = p->enc.last_status ? 0x80004005u : d3dpt_enc_result(&p->enc, off)->hr;
    dbg_hex(p, "d3dptdisp: d3d context ", i + 1);
    dbg_hex(p, " rt ", c->rt);
    dbg_hex(p, " z ", c->z);
    dbg_hex(p, " pid ", pid);
    dbg_hex(p, " -> ", hr);
    dbg_puts(p, "\n");
    if (hr & 0x80000000u) {
        return DDERR_GENERIC;
    }
    memset(&d3d_ctx[i], 0, sizeof(d3d_ctx[i]));
    d3d_ctx[i].used = TRUE;
    d3d_ctx[i].pid = pid;
    d3d_ctx[i].rt = c->rt;
    d3d_ctx[i].z = c->z;
    d3d_ctx_live++;
    *handle = i + 1;
    return DD_OK;
}

void ctx_destroy(d3dpt_core *p, ULONG i)
{
    d3d_handle_op(p, D3DPT_OP_CTX_DESTROY, i + 1);
    d3d_ctx[i].used = FALSE;
    d3d_ctx_live--;
    d3dpt_enc_flush(&p->enc);
    dbg_hex(p, "d3dptdisp: d3d context gone ", i + 1);
    dbg_hex(p, " dp2 calls ", p->dp2_calls);
    dbg_puts(p, "\n");
}

HRESULT ctx_destroy_one(d3dpt_core *p, ULONG_PTR h)
{
    if (p && ctx_of(p, h)) {
        ctx_destroy(p, (ULONG)h - 1);
    }
    return DD_OK;
}

void ctx_destroy_all(d3dpt_core *p, ULONG pid)
{
    ULONG i;

    if (!p) {
        return;
    }
    for (i = 0; i < D3D_MAX_CTX; i++) {
        if (d3d_ctx[i].used && d3d_ctx[i].pid == pid) {
            ctx_destroy(p, i);
        }
    }
}

/* EndScene: the frame into the target's VRAM, where Flip / Blt / Lock expect it */
HRESULT ctx_scene_capture(d3dpt_core *p, ULONG_PTR h, BOOL end)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;

    if (c && end) {
        d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, c->rt);
    }
    return DD_OK;
}

/* the surfaces are the layer's to register before it calls this */
HRESULT ctx_set_render_target(d3dpt_core *p, ULONG_PTR h, ULONG rt, ULONG z)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;
    d3dpt_u32x3 *r;

    if (!c) {
        return DDERR_GENERIC;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_SET_RT, sizeof(*r), 0);
    if (!r) {
        return DDERR_GENERIC;
    }
    r->a = (ULONG)h;
    r->b = rt;
    r->c = z;
    r->pad = 0;
    c->rt = rt;
    c->z = z;
    return DD_OK;
}

/* the rectangles are D3DRECT_ (four LONGs), the same on every Windows */
HRESULT ctx_clear2(d3dpt_core *p, ULONG_PTR h, ULONG flags, ULONG colour, float z, ULONG stencil,
                   const void *rects, ULONG nrects)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;
    ULONG done = 0, n;

    if (!c) {
        return DDERR_GENERIC;
    }
    do {
        d3dpt_ctx_clear *r;

        n = nrects - done;
        if (n > 64) n = 64;
        r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CLEAR, sizeof(*r), n * sizeof(D3DRECT_));
        if (!r) {
            return DDERR_GENERIC;
        }
        r->ctx = (ULONG)h;
        r->flags = flags;
        r->color = colour;
        r->count = n;
        r->z = z;
        r->stencil = stencil;
        if (n) {
            memcpy(r + 1, (const D3DRECT_ *)rects + done, n * sizeof(D3DRECT_));
        }
        done += n;
    } while (done < nrects);
    return DD_OK;
}

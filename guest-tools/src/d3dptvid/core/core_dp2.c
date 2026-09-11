/*
 * core_dp2.c — the DrawPrimitives2 token stream (doc 19, "The split"):
 * the walk that turns the runtime's tokens into D3DPT_DP2_* records, the
 * DirectX 8 rewrite (a draw becomes a self-contained DRAW8 naming its
 * vertex range and indices), TEXBLT, BUFFERBLT, the vs / ps 1.x
 * validation and the body-sizing table.
 *
 * The single most expensive piece of the driver, and the one that is
 * about the protocol and nothing else: the per-call DDI structures are
 * field-for-field identical on NT and 9x and the opcode values agree, so
 * this walks the same bytes on both. What differs — where the command
 * and vertex buffers live, and how a result is handed back — is the
 * layer's, and arrives in a d3dpt_dp2_call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

/* --- the DP2 stream: the DX7 tokens pass through, the DX8 ones are
 * rewritten (DX8 DDI, doc 15). The runtime's draw tokens name vertex and
 * index buffers the host cannot see, so each draw becomes a self-contained
 * D3DPT_DP2_DRAW8 token carrying its vertex range and indices; TEXBLT is a
 * copy done here (system memory -> VRAM, then VRAM_DIRTY); the shader
 * tokens pass through (the host keeps the shaders per context, protocol
 * v7, and a DRAW8 under a shader carries the handle in its fvf field); the
 * rest (patches, buffer blits, dirty rects) is dropped. Two passes over the
 * stream: the first measures the output and does the blits, the second
 * writes into the record. --- */

typedef struct _DP2WALK {
    d3dpt_core *p;
    const UCHAR *cmd;
    ULONG clen;
    const UCHAR *vtx;           /* the DP2 vertex buffer (user memory), from dwVertexOffset on */
    ULONG vlen, vsize, vcount;  /* its bytes (dwVertexLength * dwVertexSize), stride, vertex count */
    ULONG vall;                 /* the whole buffer from vtx on (a dxg buffer's linear size) */
    ULONG fvf;                  /* the current vertex format (SETVERTEXSHADER): an FVF, or a vertex shader handle (bit 0) */
    DP2STREAM st[D3D_MAX_STREAMS], ib;  /* the streams and the index buffer */
    ULONG st_um;                /* bit n: stream n is the DP2 vertex buffer */
    BOOL shader;                /* fvf is a vertex shader handle: the host reads the vertices through its declaration */
    BOOL one_stream;            /* DDF_ONE_STREAM: a draw carries stream 0 alone (the A/B) */
    BOOL needs_vb;              /* a DX7 draw token references the DP2 vertex buffer */
    UCHAR *out;                 /* pass 2: the record's command area (NULL in pass 1) */
    ULONG outlen;
    ULONG skipped;              /* draws skipped (bad ranges, unknown buffers) */
    ULONG skip_why;             /* bits: 2 no fvf, 4 no stream, 8 stride < fvf, 16 vertex range, 32 index range, 64 prim */
    ULONG skip_info[6];         /* the first skipped draw: prim, count, voff, nverts, ioff, nindices */
    DWORD *rstates;
    BOOL eb;                    /* the stream is a DX3 execute buffer's instructions (doc 15) */
    ULONG bounce;               /* eb: offset of the first instruction the runtime must execute itself (~0 = none) */
    ULONG stop;                 /* eb: bytes of the stream walked (an EXIT or the bounce ends it early) */
} DP2WALK;

static ULONG prim_verts(ULONG prim, ULONG n)
{
    switch (prim) {
    case 1: return n;               /* points */
    case 2: return n * 2;           /* line list */
    case 3: return n + 1;           /* line strip */
    case 4: return n * 3;           /* triangle list */
    case 5: case 6: return n + 2;   /* strip, fan */
    default: return 0;
    }
}

/* the vertex size of an FVF (the host computes the same) */
ULONG fvf_stride(ULONG fvf)
{
    ULONG n = 0, tex = (fvf >> 8) & 0xf, i;

    switch (fvf & 0xe) {
    case 0x2: n = 12; break;
    case 0x4: n = 16; break;
    case 0x6: n = 16; break;
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    default: return 0;
    }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    for (i = 0; i < tex; i++) {
        switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: n += 8; break;
        case 1: n += 12; break;
        case 2: n += 16; break;
        case 3: n += 4; break;
        }
    }
    return n;
}

static void walk_put(DP2WALK *w, const void *src, ULONG bytes)
{
    if (w->out && bytes) {
        memcpy(w->out + w->outlen, src, bytes);
    }
    w->outlen += bytes;
}

static void walk_pad(DP2WALK *w)
{
    static const UCHAR zero[4] = { 0, 0, 0, 0 };
    ULONG pad = (4 - (w->outlen & 3)) & 3;

    walk_put(w, zero, pad);
}

/* a stream / the index buffer bound to a buffer of the table by handle
 * (SETSTREAMSOURCE / SETINDICES, or the context's bindings at the start of
 * a call): its memory and size; in VRAM (v9) the draws name it */
static void stream_bind(DP2STREAM *s, ULONG handle, ULONG stride)
{
    SURF *t = handle ? surf_slot(handle, FALSE) : NULL;

    s->mem = t ? t->mem : 0;
    s->bytes = t ? t->size : 0;
    s->stride = stride;
    s->handle = handle;
    s->vram = t && t->buffer && !t->sysmem;
}

/* a stream bound to the DP2 call's own vertex buffer (SETSTREAMSOURCEUM) */
static void stream_bind_um(DP2WALK *w, ULONG n, ULONG stride)
{
    DP2STREAM *s = &w->st[n];

    s->mem = (ULONG_PTR)w->vtx;
    s->bytes = w->vall;
    s->stride = stride;
    s->handle = 0;
    s->vram = FALSE;
    w->st_um |= 1u << n;
}

/* one stream's vertices in the record: a VRAM buffer's handle and the
 * offset of the draw's first vertex, or the bytes themselves */
static void walk_stream_data(DP2WALK *w, const DP2STREAM *s, ULONG off, ULONG bytes)
{
    d3dpt_u32x2 ref;

    if (s->vram) {
        ref.a = s->handle;
        ref.b = off;
        walk_put(w, &ref, sizeof(ref));
    } else {
        walk_put(w, (const void *)(s->mem + off), bytes);
        walk_pad(w);
    }
}

/* one self-contained draw for the host: the vertex range and the indices
 * copied into the record, or (v9) a VRAM buffer's handle and offset each.
 * Under a vertex shader (v10) every other stream bound whose range is
 * there goes along too: the driver does not know which streams the
 * declaration reads (the host does), and a DX8 draw indexes all its
 * streams with one vertex number, so stream n's range starts at the same
 * vertex as stream 0's — voff / stride — at its own stride */
static void walk_draw(DP2WALK *w, ULONG prim, ULONG count, const DP2STREAM *vs, ULONG voff, ULONG nverts,
                      ULONG ioff, ULONG nindices, ULONG min_index)
{
    D3DHAL_DP2COMMAND_ h;
    d3dpt_dp2_draw8 t;
    d3dpt_dp2_draw8_stream sh;
    d3dpt_u32x2 ref;
    ULONG stride = vs->stride, vbytes, first = 0, ext[D3D_MAX_STREAMS], next = 0, i;

    /* a stride wider than the FVF is legal (the runtime passes the
     * application's stride for user-memory draws); under a vertex shader
     * the declaration decides what the stride must cover (checked on the
     * host, which has the declaration) */
    if (!w->fvf) w->skip_why |= 2;
    else if (!stride || !vs->mem) w->skip_why |= 4;
    else if (!w->shader && fvf_stride(w->fvf) > stride) w->skip_why |= 8;
    else if (!prim_verts(prim, count)) w->skip_why |= 64;
    if (!w->fvf || !stride || !vs->mem || (!w->shader && fvf_stride(w->fvf) > stride) || !prim_verts(prim, count)) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = w->fvf; w->skip_info[4] = stride; w->skip_info[5] = nindices;
        }
        w->skipped++;
        return;
    }
    if (!nindices) {
        nverts = prim_verts(prim, count);
    }
    vbytes = nverts * stride;
    if (nverts > 0x10000 || voff > vs->bytes || vbytes > vs->bytes - voff ||
        (nindices && (!w->ib.mem || w->ib.stride != 2 || ioff > w->ib.bytes || nindices * 2 > w->ib.bytes - ioff))) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = nverts; w->skip_info[4] = ioff; w->skip_info[5] = nindices;
        }
        w->skipped++;
        w->skip_why |= (nverts > 0x10000 || voff > vs->bytes || vbytes > vs->bytes - voff) ? 16 : 32;
        return;
    }
    /* the other streams: a shader's draw only, and only where stream 0's
     * offset is a whole vertex (it always is for the runtime's own draws; a
     * stream that is bound but short — a stale binding a stream-0 shader
     * does not read — is left out, and the host skips the draw if its
     * declaration wanted it) */
    if (w->shader && !w->one_stream && vs == &w->st[0] && voff % stride == 0) {
        first = voff / stride;
        for (i = 1; i < D3D_MAX_STREAMS; i++) {
            const DP2STREAM *s = &w->st[i];
            if (s->mem && s->stride && s->stride <= 1024 && first <= s->bytes / s->stride &&
                nverts <= (s->bytes - first * s->stride) / s->stride) {
                ext[next++] = i;
            }
        }
    }
    h.bCommand = (BYTE)D3DPT_DP2_DRAW8;
    h.bReserved = 0;
    h.wPrimitiveCount = 0;
    t.prim_type = prim;
    t.prim_count = count;
    t.fvf = w->fvf;
    t.stride = stride;
    t.nverts = nverts;
    t.nindices = nindices;
    t.min_index = min_index;
    t.flags = (vs->vram ? D3DPT_DRAW8_VRAM_VB : 0) | (nindices && w->ib.vram ? D3DPT_DRAW8_VRAM_IB : 0) |
              (next ? D3DPT_DRAW8_STREAMS : 0);
    walk_put(w, &h, sizeof(h));
    walk_put(w, &t, sizeof(t));
    walk_stream_data(w, vs, voff, vbytes);
    if (nindices) {
        if (w->ib.vram) {
            ref.a = w->ib.handle;
            ref.b = ioff;
            walk_put(w, &ref, sizeof(ref));
        } else {
            walk_put(w, (const void *)(w->ib.mem + ioff), nindices * 2);
            walk_pad(w);
        }
    }
    if (next) {
        ref.a = next;
        ref.b = 0;
        walk_put(w, &ref, sizeof(ref));
        for (i = 0; i < next; i++) {
            const DP2STREAM *s = &w->st[ext[i]];
            sh.stream = ext[i];
            sh.stride = s->stride;
            sh.flags = s->vram ? D3DPT_DRAW8_VRAM_VB : 0;
            sh.pad = 0;
            walk_put(w, &sh, sizeof(sh));
            walk_stream_data(w, s, first * s->stride, nverts * s->stride);
        }
    }
}

/* a TEXBLT's rectangle (b: the token) from one level list to another, level
 * 0 first, every level both have; DXT in blocks */
static void blt_levels(ULONG fmt, ULONG src_w, ULONG src_h, const SURF_LEVEL *slv, ULONG dst_w, ULONG dst_h,
                       const SURF_LEVEL *dlv, ULONG levels, const ULONG *b)
{
    LONG dx = (LONG)b[2], dy = (LONG)b[3], sl = (LONG)b[4], st = (LONG)b[5], sr = (LONG)b[6], sb = (LONG)b[7];
    BOOL dxt = fmt_is_dxt(fmt);
    ULONG bpp = fmt_row_bytes(fmt, 1), lv;

    for (lv = 0; lv < levels; lv++) {
        ULONG_PTR smem = slv[lv].mem, dmem = dlv[lv].mem;
        ULONG spitch = slv[lv].pitch, dpitch = dlv[lv].pitch;
        ULONG sw = src_w >> lv, sh = src_h >> lv, dw = dst_w >> lv, dh = dst_h >> lv;
        ULONG x0 = (ULONG)sl >> lv, y0 = (ULONG)st >> lv, x1 = (ULONG)dx >> lv, y1 = (ULONG)dy >> lv;
        ULONG cw = (ULONG)(sr - sl) >> lv, ch = (ULONG)(sb - st) >> lv, rows, rowbytes, y;

        if (!sw) sw = 1;
        if (!sh) sh = 1;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        if (!cw) cw = 1;
        if (!ch) ch = 1;
        if (x0 + cw > sw) cw = sw > x0 ? sw - x0 : 0;
        if (y0 + ch > sh) ch = sh > y0 ? sh - y0 : 0;
        if (x1 + cw > dw) cw = dw > x1 ? dw - x1 : 0;
        if (y1 + ch > dh) ch = dh > y1 ? dh - y1 : 0;
        if (!cw || !ch || !smem || !dmem) {
            continue;
        }
        if (dxt) {
            rows = (ch + 3) / 4;
            rowbytes = fmt_row_bytes(fmt, cw);
            smem += (y0 / 4) * spitch + fmt_row_bytes(fmt, x0);
            dmem += (y1 / 4) * dpitch + fmt_row_bytes(fmt, x1);
        } else {
            rows = ch;
            rowbytes = cw * bpp;
            smem += y0 * spitch + x0 * bpp;
            dmem += y1 * dpitch + x1 * bpp;
        }
        for (y = 0; y < rows; y++) {
            memcpy((void *)(dmem + y * dpitch), (const void *)(smem + y * spitch), rowbytes);
        }
    }
}

/* TEXBLT: system memory -> the VRAM texture, every level (a cube's every
 * face, v11), then VRAM_DIRTY */
static void walk_texblt(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    LONG dx = (LONG)b[2], dy = (LONG)b[3], sl = (LONG)b[4], st = (LONG)b[5], sr = (LONG)b[6], sb = (LONG)b[7];
    SURF_LEVEL slv[16], dlv[16];
    ULONG lv, levels, f;

    if (!dst || !src || !dst->fmt || dst->fmt != src->fmt || !src->sysmem || dst->buffer || src->buffer) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: texblt refused, dst ", b[0]);
            dbg_hex(p, " fmt ", dst ? dst->fmt : 0);
            dbg_hex(p, " w ", dst ? dst->w : 0);
            dbg_hex(p, " h ", dst ? dst->h : 0);
            dbg_hex(p, " src ", b[1]);
            dbg_hex(p, " fmt ", src ? src->fmt : 0);
            dbg_hex(p, " w ", src ? src->w : 0);
            dbg_hex(p, " h ", src ? src->h : 0);
            dbg_hex(p, " pitch ", src ? src->pitch : 0);
            dbg_hex(p, " sysmem ", src ? src->sysmem : 0);
            dbg_hex(p, " rect ", (b[4] << 16) | (b[5] & 0xffff));
            dbg_hex(p, "..", (b[6] << 16) | (b[7] & 0xffff));
            dbg_puts(p, "\n");
        }
        return;
    }
    if (sl < 0 || st < 0 || sr <= sl || sb <= st || dx < 0 || dy < 0) {
        return;
    }
    levels = dst->levels < src->levels ? dst->levels : src->levels;
    if (levels > 16) {
        levels = 16;
    }
    if (dst->cube || src->cube) {
        /* a cube's root names the whole cube: the rectangle on every face
         * (a face's own handle is an ordinary entry, the path below) */
        if (!dst->cube || !src->cube) {
            if (p->dp2_errors < 8) {
                p->dp2_errors++;
                dbg_hex(p, "d3dptdisp: texblt between a cube and a texture refused, dst ", b[0]);
                dbg_hex(p, " src ", b[1]);
                dbg_puts(p, "\n");
            }
            return;
        }
        for (f = 0; f < 6; f++) {
            blt_levels(dst->fmt, src->w, src->h, src->cube->f[f], dst->w, dst->h, dst->cube->f[f], levels, b);
        }
    } else {
        slv[0].mem = src->mem;
        slv[0].pitch = src->pitch;
        dlv[0].mem = dst->mem;
        dlv[0].pitch = dst->pitch;
        for (lv = 1; lv < levels; lv++) {
            slv[lv] = src->lv[lv - 1];
            dlv[lv] = dst->lv[lv - 1];
        }
        blt_levels(dst->fmt, src->w, src->h, slv, dst->w, dst->h, dlv, levels, b);
    }
    if (!dst->sysmem) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, b[0]);
    }
}

/* BUFFERBLT (v9): the DX8 runtime fills a managed vertex / index buffer's
 * video-memory copy from its system-memory one (dwDDDestSurface,
 * dwDDSrcSurface, dwOffset, D3DRANGE rSrc); copied here, the range is the
 * guest's (VRAM_DIRTY_RANGE). Never sent while every buffer was in system
 * memory (before v9). */
static void walk_bufferblt(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    ULONG doff = b[2], soff = b[3], len = b[4];

    if (!dst || !src || !dst->buffer || !src->buffer || !dst->mem || !src->mem ||
        doff > dst->size || len > dst->size - doff || soff > src->size || len > src->size - soff) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: bufferblt refused, dst ", b[0]);
            dbg_hex(p, " size ", dst ? dst->size : 0);
            dbg_hex(p, " at ", doff);
            dbg_hex(p, " src ", b[1]);
            dbg_hex(p, " size ", src ? src->size : 0);
            dbg_hex(p, " at ", soff);
            dbg_hex(p, " len ", len);
            dbg_puts(p, "\n");
        }
        return;
    }
    if (p->bufblt_lines < 8) {
        p->bufblt_lines++;
        dbg_hex(p, "d3dptdisp: bufferblt dst ", b[0]);
        dbg_hex(p, " +", doff);
        dbg_hex(p, " src ", b[1]);
        dbg_hex(p, " +", soff);
        dbg_hex(p, " len ", len);
        dbg_hex(p, " sixth ", b[5]);                /* the 24-byte token's last dword (the first cut parsed 20 and desynchronised) */
        dbg_puts(p, dst->sysmem ? " (sysmem)\n" : " (vram)\n");
    }
    if (len) {
        memcpy((void *)(dst->mem + doff), (const void *)(src->mem + soff), len);
    }
    if (!dst->sysmem) {
        d3d_dirty_range(p, b[0], doff, len);
    }
}

/* the body size of a token, ~0 when unknown or truncated; the IMM tokens'
 * payload is DWORD-aligned by offset (their size depends on where they sit) */
static ULONG walk_body_size(ULONG op, ULONG count, const UCHAR *q, ULONG left, ULONG pos, ULONG stride)
{
    ULONG pad = (4 - ((pos + 4) & 3)) & 3, n, i, sz;

    switch (op) {
    case 1: return count * 4;                                   /* POINTS */
    case 2: return count * 4;                                   /* INDEXEDLINELIST */
    case 3: return count * 8;                                   /* INDEXEDTRIANGLELIST */
    case 8: return count * 8;                                   /* RENDERSTATE */
    case 15: case 16: case 18: case 19: case 21: return 2;      /* LINELIST .. TRIANGLEFAN: wVStart */
    case 17: return 2 + (count + 1) * 2;                        /* INDEXEDLINESTRIP */
    case 20: case 22: return 2 + (count + 2) * 2;               /* INDEXEDTRIANGLESTRIP / FAN */
    case 23: return pad + 4 + (count + 2) * stride;             /* TRIANGLEFAN_IMM (+ the pad after) */
    case 24: return pad + count * 2 * stride;                   /* LINELIST_IMM */
    case 25: return count * 8;                                  /* TEXTURESTAGESTATE */
    case 26: return 2 + count * 6;                              /* INDEXEDTRIANGLELIST2 */
    case 27: return 2 + count * 4;                              /* INDEXEDLINELIST2 */
    case 28: return count * 16;                                 /* VIEWPORTINFO */
    case 29: return count * 8;                                  /* WINFO */
    case 30: return count * 12;                                 /* SETPALETTE */
    case 31: return left < 8 ? ~0u : 8 + (ULONG)((const USHORT *)q)[3] * 4;   /* UPDATEPALETTE */
    case 32: return count * 8;                                  /* ZRANGE */
    case 33: return count * 68;                                 /* SETMATERIAL */
    case 34:                                                    /* SETLIGHT: 8, + 104 with data */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1] == 2 ? 112 : 8;
            sz += n;
        }
        return sz;
    case 35: return count * 4;                                  /* CREATELIGHT */
    case 36: return count * 68;                                 /* SETTRANSFORM */
    case 38: return count * 36;                                 /* TEXBLT */
    case 39: return count * 12;                                 /* STATESET */
    case 40: return count * 8;                                  /* SETPRIORITY */
    case 41: return count * 8;                                  /* SETRENDERTARGET */
    case 42: return 16 + count * 16;                            /* CLEAR */
    case 43: return count * 8;                                  /* SETTEXLOD */
    case 44: return count * 20;                                 /* SETCLIPPLANE */
    case 45:                                                    /* CREATEVERTEXSHADER: handle, decl size, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 12) return ~0u;
            sz += 12 + ((const ULONG *)(q + sz))[1] + ((const ULONG *)(q + sz))[2];
        }
        return sz;
    case 46: case 47: case 55: case 56: return count * 4;       /* shader handles */
    case 48: case 57:                                           /* shader constants: register, count, count * 16 */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1] * 16;
        }
        return sz;
    case 49: return count * 12;                                 /* SETSTREAMSOURCE */
    case 50: return count * 8;                                  /* SETSTREAMSOURCEUM */
    case 51: return count * 8;                                  /* SETINDICES */
    case 52: return count * 12;                                 /* DRAWPRIMITIVE */
    case 53: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE */
    case 54:                                                    /* CREATEPIXELSHADER: handle, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1];
        }
        return sz;
    case 58: return count * 12;                                 /* CLIPPEDTRIANGLEFAN */
    case 59: return count * 12;                                 /* DRAWPRIMITIVE2 */
    case 60: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE2 */
    case 61: case 62:                                           /* patches: handle, flags [, segments] [, info] */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1];
            sz += 8 + ((n & 1) ? (op == 61 ? 16 : 12) : 0) + ((n & 2) ? (op == 61 ? 28 : 16) : 0);
        }
        return sz;
    case 63: return count * 48;                                 /* VOLUMEBLT */
    case 64: return count * 24;                                 /* BUFFERBLT: dst, src, dst offset, D3DRANGE (offset, size), one more dword (see walk_bufferblt) */
    case 65: return count * 68;                                 /* MULTIPLYTRANSFORM */
    case 66: return count * 20;                                 /* ADDDIRTYRECT */
    case 67: return count * 28;                                 /* ADDDIRTYBOX */
    default: return ~0u;
    }
}

/* one pass over the runtime's stream; FALSE = an unknown token stopped it
 * there (the rest is copied verbatim for the host to report) */
static BOOL walk(DP2WALK *w)
{
    ULONG pos = 0, i;

    while (pos + 4 <= w->clen) {
        const D3DHAL_DP2COMMAND_ *c = (const D3DHAL_DP2COMMAND_ *)(w->cmd + pos);
        const UCHAR *q = w->cmd + pos + 4;
        ULONG op = c->bCommand, count = c->wPrimitiveCount, left = w->clen - pos - 4, size;
        ULONG stride = w->vsize ? w->vsize : (w->fvf ? fvf_stride(w->fvf) : 0);

        if (w->eb) {
            /* A DX3 execute buffer (IDirect3DDevice::Execute, d3dim.dll's
             * UNCLIPPED path): the stream is the buffer's own D3DINSTRUCTION
             * list from the current instruction on, and the runtime is a
             * pass-through — it executes nothing here itself. The opcodes
             * share the DP2 numbering where the payloads match (POINT /
             * LINE / TRIANGLE / STATERENDER are 1 / 2 / 3 / 8: POINTS,
             * INDEXEDLINELIST, the 8-byte INDEXEDTRIANGLELIST, RENDERSTATE),
             * and those the driver must consume, along with SPAN (13,
             * skipped) and EXIT (11, the end). Everything else —
             * PROCESSVERTICES (9) first of all, the matrix / light opcodes
             * 4..7, TEXTURELOAD, BRANCHFORWARD, SETSTATUS — is the runtime's:
             * the call ends *before* it with D3DERR_COMMAND_UNPARSED and its
             * offset in dwErrorOffset, the runtime executes it (a
             * PROCESSVERTICES COPY / TRANSFORM fills the TL vertex buffer
             * we draw from) and calls again from the next instruction.
             * Skipping it instead leaves the vertices unwritten (doc 15
             * "Execute buffers"). */
            if (op == 11) {                                     /* EXIT */
                w->stop = pos;
                return TRUE;
            }
            if (op == 13) {                                     /* SPAN: bSize x count, nothing for the host */
                size = (ULONG)c->bReserved * count;
                if (size > left) size = left;
                pos += 4 + size;
                continue;
            }
            if (op != 1 && op != 2 && op != 3 && op != 8) {
                w->bounce = pos;
                w->stop = pos;
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: execute buffer: opcode ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " bounced to the runtime at ", pos);
                    dbg_puts(w->p, "\n");
                }
                return TRUE;
            }
        }
        size = walk_body_size(op, count, q, left, pos, stride);
        if (size == ~0u && w->p->parse_unknown && !(ddflags(w->p) & DDF_NO_PARSEUNKNOWN_CALL)) {
            /* not one of ours and not a legacy opcode: the runtime's parser
             * gets a chance (it knows VIEWPORTINFO / WINFO, which the host
             * handles anyway); the token is dropped from the host's stream */
            PVOID next = NULL;
            HRESULT hr = w->p->parse_unknown((PVOID)c, &next);
            if (hr == DD_OK && next && (const UCHAR *)next > q && (const UCHAR *)next <= w->cmd + w->clen) {
                size = (ULONG)((const UCHAR *)next - q);
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " parsed by the runtime, bytes ", size);
                    dbg_puts(w->p, "\n");
                }
                pos += 4 + size;
                continue;
            }
            if (!w->out && w->p->parse_lines < 8) {
                w->p->parse_lines++;
                dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                dbg_hex(w->p, " unparsed by the runtime too, hr ", (ULONG)hr);
                dbg_puts(w->p, "\n");
            }
        }
        if (size == ~0u || size > left) {
            walk_put(w, w->cmd + pos, w->clen - pos);
            return FALSE;
        }
        if (op == 23 || op == 24) {                             /* the next token is DWORD-aligned by offset */
            ULONG e = (pos + 4 + size + 3) & ~3u;
            size = e - pos - 4 <= left ? e - pos - 4 : left;
        }
        switch (op) {
        case 8:                                                 /* RENDERSTATE: mirrored for the runtime */
            if (w->rstates && !w->out) {
                for (i = 0; i < count; i++) {
                    const ULONG *e = (const ULONG *)(q + i * 8);
                    if (e[0] < 256) w->rstates[e[0]] = e[1];
                }
            }
            walk_put(w, c, 4 + size);
            break;
        case 23: case 24: {                                     /* IMM: re-pad for the output offset */
            ULONG ipad = (4 - ((pos + 4) & 3)) & 3;
            walk_put(w, c, 4);
            walk_pad(w);
            walk_put(w, q + ipad, size - ipad);
            walk_pad(w);
            w->needs_vb = TRUE;
            break;
        }
        case 1: case 2: case 3: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22:
        case 26: case 27:
            w->needs_vb = TRUE;
            walk_put(w, c, 4 + size);
            break;
        case 25:                                                /* TEXTURESTAGESTATE: a bound texture's colour key, in pass 1 */
            if (!w->out) {
                for (i = 0; i < count; i++) {
                    const USHORT *e = (const USHORT *)(q + i * 8);
                    if (e[1] == 0 && ((const ULONG *)(q + i * 8))[1] != 0) {
                        surf_colorkey_check(w->p, ((const ULONG *)(q + i * 8))[1]);
                    }
                }
            }
            walk_put(w, c, 4 + size);
            break;
        case 38:                                                /* TEXBLT: done here, in pass 1 */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_texblt(w, (const ULONG *)(q + i * 36));
            }
            break;
        case 47:                                                /* SETVERTEXSHADER: an FVF, or a shader handle (bit 0) */
            for (i = 0; i < count; i++) {
                ULONG h = ((const ULONG *)q)[i];
                w->fvf = h;
                w->shader = (h & 1) != 0;
            }
            walk_put(w, c, 4 + size);
            break;
        case 49:                                                /* SETSTREAMSOURCE: stream, handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                if (e[0] < D3D_MAX_STREAMS) {
                    stream_bind(&w->st[e[0]], e[1], e[2]);
                    w->st_um &= ~(1u << e[0]);
                }
            }
            break;
        case 50:                                                /* SETSTREAMSOURCEUM: stream, stride (the DP2 vertex buffer) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                if (e[0] < D3D_MAX_STREAMS) {
                    stream_bind_um(w, e[0], e[1]);
                }
            }
            break;
        case 51:                                                /* SETINDICES: handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                stream_bind(&w->ib, e[0], e[1]);
            }
            break;
        case 52:                                                /* DRAWPRIMITIVE: type, VStart, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->st[0], e[1] * w->st[0].stride, 0, 0, 0, 0);
            }
            break;
        case 59:                                                /* DRAWPRIMITIVE2: type, first vertex offset (bytes into stream 0), count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->st[0], e[1], 0, 0, 0, 0);
            }
            break;
        case 53:                                                /* DRAWINDEXEDPRIMITIVE: type, base, min, nverts, start index, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->st[0], (e[1] + e[2]) * w->st[0].stride, e[3], e[4] * 2, prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 60:                                                /* DRAWINDEXEDPRIMITIVE2: type, base offset, min, nverts, start offset, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->st[0], (ULONG)((LONG)e[1] + (LONG)(e[2] * w->st[0].stride)), e[3], e[4], prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 58:                                                /* CLIPPEDTRIANGLEFAN: first vertex offset, edge flags, count */
            /* the DX8 runtime clips pre-transformed triangles itself into
             * a vertex buffer of its own and binds that buffer as stream 0
             * (SETSTREAMSOURCE, its stride) before these tokens: the offset
             * is a byte offset into stream 0, not into the DP2 vertex
             * buffer (which is a dummy under d3d8.dll; doc 15) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, 6, e[2], &w->st[0], e[0], 0, 0, 0, 0);
            }
            break;
        case 64:                                                /* BUFFERBLT: done here, in pass 1 (v9) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_bufferblt(w, (const ULONG *)(q + i * 24));
            }
            break;
        case 61: case 62: case 63: case 66: case 67:            /* patches, volume blits, dirty rects */
            break;
        default:                                                /* the DX7 state tokens and the shader tokens (45, 46, 48, 54..57) */
            walk_put(w, c, 4 + size);
            break;
        }
        pos += 4 + size;
    }
    if (pos < w->clen) {
        walk_put(w, w->cmd + pos, w->clen - pos);
    }
    w->stop = w->clen;
    return TRUE;
}


/* One DrawPrimitives2 call: two passes over the runtime's stream (the
 * first measures the output and does the blits, the second writes into
 * the record), then the record into the command window and the doorbell.
 * The layer has already resolved where the buffers are and which context
 * this is; what comes back is what it must tell the runtime. */
void dp2_run(d3dpt_core *p, const d3dpt_dp2_call *call, d3dpt_dp2_result *out)
{
    D3DCTX *c = p ? ctx_of(p, call->ctx) : NULL;
    ULONG vcopy, off, i;
    d3dpt_dp2 *r;
    d3dpt_ret *res;
    DP2WALK w, w0;

    out->hr = DDERR_GENERIC;
    out->offset = 0;
    out->bounce = FALSE;
    if (!c) {
        if (p && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, context ", (ULONG)call->ctx);
            dbg_puts(p, "\n");
        }
        return;
    }
    if (call->clen > (16u << 20) || call->vlen > (32u << 20)) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, command bytes ", call->clen);
            dbg_hex(p, " vertex bytes ", call->vlen);
            dbg_puts(p, "\n");
        }
        return;
    }
    /* pass 1: the output size, the render-state mirror, the TEXBLTs; both
     * passes start from the context's DX8 state */
    memset(&w0, 0, sizeof(w0));
    w0.p = p;
    w0.cmd = call->cmd;
    w0.clen = call->clen;
    w0.vtx = call->vtx;
    w0.vlen = call->vlen;
    w0.vsize = call->vsize;
    w0.vcount = call->vcount;
    w0.vall = call->vall;
    w0.eb = call->eb;
    w0.bounce = ~0u;
    w0.fvf = c->fvf ? c->fvf : (call->vertex_type & 1 ? 0 : call->vertex_type);
    w0.shader = c->shader;
    w0.one_stream = (ddflags(p) & DDF_ONE_STREAM) != 0;
    for (i = 0; i < D3D_MAX_STREAMS; i++) {
        if (c->st_um & (1u << i)) {
            stream_bind_um(&w0, i, c->st_stride[i]);
        } else {
            stream_bind(&w0.st[i], c->st_handle[i], c->st_stride[i]);
        }
    }
    stream_bind(&w0.ib, c->ib_handle, c->ib_stride);
    w = w0;
    w.rstates = call->rstates;
    walk(&w);
    if (w.eb && w.bounce == 0) {
        /* nothing of ours before the runtime's instruction: bounce at once */
        out->hr = D3DERR_COMMAND_UNPARSED_;
        out->bounce = TRUE;
        out->offset = 0;
        return;
    }
    vcopy = w.needs_vb ? call->vlen : 0;
    if (w.outlen > (48u << 20) || D3DPT_ALIGN8(w.outlen) + vcopy + sizeof(*r) + sizeof(d3dpt_cmd) > D3DPT_CMD_SIZE) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, output bytes ", w.outlen);
            dbg_hex(p, " vertex copy ", vcopy);
            dbg_puts(p, "\n");
        }
        return;
    }
    p->dp2_calls++;
    off = d3dpt_enc_ret(&p->enc, 0);
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_DP2, sizeof(*r), D3DPT_ALIGN8(w.outlen) + vcopy);
    if (!r) {
        return;
    }
    /* pass 2: the same walk, writing into the record; its end state is the
     * context's for the next call */
    {
        ULONG outlen = w.outlen, skipped = w.skipped;
        w = w0;
        w.out = (UCHAR *)(r + 1);
        walk(&w);
        if (w.outlen != outlen) {           /* cannot happen: the same stream twice */
            w.outlen = outlen;
        }
        c->fvf = w.fvf;
        c->shader = w.shader;
        c->st_um = w.st_um;
        for (i = 0; i < D3D_MAX_STREAMS; i++) {
            c->st_handle[i] = w.st[i].handle;
            c->st_stride[i] = w.st[i].stride;
        }
        c->ib_handle = w.ib.handle;
        c->ib_stride = w.ib.stride;
        if (skipped && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dx8 draws skipped ", skipped);
            dbg_hex(p, " why ", w.skip_why);
            dbg_hex(p, " fvf ", w0.fvf);
            dbg_hex(p, " stride ", w0.st[0].stride);
            dbg_hex(p, " bytes ", w0.st[0].bytes);
            dbg_hex(p, " ib ", w0.ib.bytes);
            dbg_hex(p, "; first: prim ", w.skip_info[0]);
            dbg_hex(p, " count ", w.skip_info[1]);
            dbg_hex(p, " voff ", w.skip_info[2]);
            dbg_hex(p, " nverts ", w.skip_info[3]);
            dbg_hex(p, " ioff ", w.skip_info[4]);
            dbg_hex(p, " nindices ", w.skip_info[5]);
            dbg_puts(p, "\n");
        }
    }
    r->ctx = (ULONG)call->ctx;
    r->ret_off = off;
    r->flags = call->flags;
    r->fvf = call->vertex_type;
    r->vertex_stride = call->vsize;
    r->command_bytes = w.outlen;
    r->vertex_bytes = vcopy;
    r->pad = 0;
    if (vcopy) {
        memcpy((UCHAR *)(r + 1) + D3DPT_ALIGN8(w.outlen), call->vtx, vcopy);
    }
    d3dpt_enc_flush(&p->enc);
    res = d3dpt_enc_result(&p->enc, off);
    if (p->enc.last_status) {
        out->hr = DDERR_GENERIC;
        out->offset = 0;
    } else if (w.eb && w.bounce != ~0u && res->hr == DD_OK) {
        /* the host drew what came before it; the runtime takes over at
         * this instruction and calls again from the next one */
        out->hr = D3DERR_COMMAND_UNPARSED_;
        out->bounce = TRUE;
        out->offset = w.bounce;
    } else {
        out->hr = (HRESULT)res->hr;
        out->offset = res->bytes;
    }
    if (out->hr != DD_OK && !out->bounce && p->dp2_errors < 8) {
        p->dp2_errors++;
        dbg_hex(p, "d3dptdisp: dp2 ", (ULONG)out->hr);
        dbg_hex(p, " at ", out->offset);
        dbg_hex(p, " len ", call->clen);
        dbg_hex(p, " fvf ", call->vertex_type);
        dbg_puts(p, "\n");
    }
}

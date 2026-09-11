/*
 * d3dpt_core.h — the OS-independent half of the display driver (doc 19,
 * "The split"). Everything here is about *our* adapter and *our*
 * protocol: the surface table and its format arithmetic, the caps, the
 * contexts, the flip chain, the DrawPrimitives2 walker.
 *
 * The core is linked into a **kernel-mode** DLL on NT and a **user-mode**
 * one on 9x, so it calls no operating-system service at all: the four it
 * needs (allocate, free, read a tick, describe a surface) arrive through
 * the d3dpt_os_* hooks below, which the per-OS layer implements. It also
 * includes no DDK header of either family — NT's DD_SURFACE_LOCAL and
 * 9x's DDRAWI_DDRAWSURFACE_LCL never reach it; the layer fills a
 * d3dpt_surf_desc instead and the core works on that.
 *
 * The includer must have pulled in <windef.h>, <ddraw.h> and the DDI
 * types (core/d3dpt_ddi.h, which its DDK header includes) first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_CORE_H
#define D3DPT_CORE_H

#include "../../../../d3dpt/d3dpt_fb.h"
#include "../../../../d3dpt/d3dpt_enc.h"

/* -device d3dpt-vga,ddflags=N: bisection knobs while the DDI is brought
 * up. Read off the adapter's DDFLAGS register, so they are the core's:
 * every one of them switches behaviour the core owns. */
#define DDF_NO_GETDRIVERINFO   0x1    /* no GetDriverInfo / GETDRIVERINFOSET */
#define DDF_NO_SURFACE_CB      0x4    /* only MapMemory + CanCreateSurface */
#define DDF_ENGINE_BITMAP      0x8    /* EngCreateBitmap primary instead of a device surface */
#define DDF_GDI_CAP            0x10   /* add DDCAPS_GDI to dwCaps: dxg then drops the HAL (kept as the repro) */
#define DDF_NO_D3D             0x20   /* no Direct3D: DirectDraw HAL only, as in M7b */
#define DDF_NO_3D_CAP          0x40   /* bisection: Direct3D data without DDCAPS_3D / the 3D ddsCaps */
#define DDF_NO_D3D_INFO        0x80   /* bisection: no D3D answers from GetDriverInfo */
#define DDF_NO_D3D_BUF         0x100  /* bisection: no D3D buffer callbacks */
#define DDF_NO_D3D_CB3         0x200  /* bisection: no GUID_D3DCallbacks3 answer */
#define DDF_NO_MISC2           0x400  /* bisection: no GUID_Miscellaneous2Callbacks answer */
#define DDF_NO_PARSEUNKNOWN    0x800  /* bisection: refuse GUID_D3DParseUnknownCommandCallback */
#define DDF_NO_TNL             0x1000 /* bisection: no HWTRANSFORMANDLIGHT claim (the runtime transforms) */
#define DDF_NO_DX8             0x2000 /* bisection: refuse GetDriverInfo2 (a DirectX 7 driver to d3d8.dll) */
#define DDF_NO_SHADERS         0x4000 /* bisection: no vertex / pixel shader versions in D3DCAPS8 */
#define DDF_NO_VSYNC           0x8000 /* flips complete instantly again (M7b): throughput runs */
#define DDF_NO_CKEY            0x10000 /* bisection: no colour keying (caps, callbacks, the key check), no P8 textures */
#define DDF_CKEY_NOBLTCB       0x20000 /* the repro: the colour-key caps without a Blt callback make dxg drop the HAL */
#define DDF_EB_MAXVERT_65535   0x40000 /* the repro: dwMaxVertexCount 65535 makes every DX3 Execute fail with E_OUTOFMEMORY (doc 15) */
#define DDF_NO_PARSEUNKNOWN_CALL 0x80000 /* bisection: never call the runtime's D3DParseUnknownCommand (legacy tokens reach the host) */
#define DDF_NO_HWVB            0x100000 /* the A/B: no video-memory vertex / index buffers (every buffer in system memory, every
                                         * draw's vertices copied into the record, as before protocol v9) */
#define DDF_ONE_STREAM         0x200000 /* the A/B: MaxStreams 1 and every draw carrying stream 0 alone, as before protocol v10 */
#define DDF_NO_CUBE            0x400000 /* the A/B: no cube textures (caps, format ops), as before protocol v11 */
#define DDF_NO_BUMP            0x800000 /* the A/B: no bump-map format in either texture list — V8U8, and in the DX8 one
                                         * L6V5U5 / X8L8V8U8 too (EMBM's ops stay claimed, as before) */
#define DDF_NO_VOLUME          0x1000000 /* the A/B: no volume textures (caps, format ops) */
#define DDF_NO_ANISO           0x2000000 /* the A/B: MaxAnisotropy 1, no anisotropic filter caps */
#define DDF_NO_MORE_FMTS       0x4000000 /* the A/B: none of L8 A8L8 A4L4 A8 X4R4G4B4 R3G3B2 A8R3G3B2 DXT2 DXT4 in the DX8 format list */

/* DDI-only DX8 device caps (d3dhal.h): the runtime puts vertex / index
 * buffers in video memory through the buffer callbacks when they are set */
#define D3DDEVCAPS_HWVERTEXBUFFER_ 0x02000000
#define D3DDEVCAPS_HWINDEXBUFFER_  0x04000000
#define DDSCAPS2_VERTEXBUFFER_ 0x02000000
#define DDSCAPS2_INDEXBUFFER_  0x04000000

#define D3DFMT_X8R8G8B8_  22u
#define D3DFMT_A8R8G8B8_  21u
#define D3DFMT_R5G6B5_    23u
#define D3DFMT_X1R5G5B5_  24u
#define D3DFMT_A1R5G5B5_  25u
#define D3DFMT_A4R4G4B4_  26u
#define D3DFMT_X4R4G4B4_  30u
#define D3DFMT_R3G3B2_    27u
#define D3DFMT_A8_        28u
#define D3DFMT_A8R3G3B2_  29u
#define D3DFMT_P8_        41u
#define D3DFMT_L8_        50u
#define D3DFMT_A8L8_      51u
#define D3DFMT_A4L4_      52u
#define D3DFMT_V8U8_      60u
#define D3DFMT_L6V5U5_    61u
#define D3DFMT_X8L8V8U8_  62u
#define D3DFMT_Q8W8V8U8_  63u
#define D3DFMT_D16_       80u
#define D3DFMT_D24X8_     77u
#define D3DFMT_D24S8_     75u
#define D3DFMT_D15S1_     73u
#define D3DFMT_D32_       71u
#define FOURCC_(a, b, c, d) ((ULONG)(UCHAR)(a) | ((ULONG)(UCHAR)(b) << 8) | ((ULONG)(UCHAR)(c) << 16) | ((ULONG)(UCHAR)(d) << 24))

#define D3D_MAX_CTX 16
#define D3D_MAX_STREAMS 16          /* D3DCAPS8.MaxStreams (D3DPT_DRAW8_MAX_STREAMS) */

/* Three DirectDraw-internal bits the core acts on, spelled out because
 * they are in a DDK header on both families and the core includes
 * neither. They are the runtime's, not the OS's — the same values in
 * NT's ddrawint.h and 9x's ddrawi.h — and each per-OS layer checks its
 * copy against the DDK's, because a constant transcribed wrong is the
 * one mistake this arrangement makes silently: EXECUTEBUFFER went in as
 * 0x800 and cost the video-memory vertex buffers a shtest case
 * (2026-09-07). The DDKs spell that one DDSCAPS_RESERVED2, which is what
 * it was renamed to when execute buffers left the public API. */
#define DDRAWISURF_HASCKEYSRCBLT_   0x00000800
#define DDRAWISURF_HASPIXELFORMAT_  0x00002000
#define DDSCAPS_EXECUTEBUFFER_      0x00800000
/* a cube texture's faces (ddsCapsEx.dwCaps2; public ddraw.h values, the
 * same on both families): the root is +X's level 0, the other five faces
 * hang off it, and each face carries its own mip chain */
#define DDSCAPS2_CUBEMAP_           0x00000200
#define DDSCAPS2_CUBEMAP_POSITIVEX_ 0x00000400   /* face n is this << n, in D3DCUBEMAP_FACES order */
#define DDSCAPS2_CUBEMAP_ALLFACES_  0x0000fc00
#define DDSCAPS2_VOLUME_            0x00200000   /* a volume texture (public ddraw.h); its depth in dwCaps4's low word */
#define D3DPTEXTURECAPS_CUBEMAP_    0x00000800
#define D3DPTEXTURECAPS_MIPCUBEMAP_ 0x00010000
#define D3DPTEXTURECAPS_VOLUMEMAP_  0x00002000
#define D3DPTEXTURECAPS_MIPVOLUMEMAP_ 0x00008000
#define D3DPTFILTERCAPS_MINFANISOTROPIC_ 0x00000400
#define D3DPTFILTERCAPS_MAGFANISOTROPIC_ 0x04000000
#define D3DPRASTERCAPS_ANISOTROPY_  0x00020000
#define D3DFORMAT_OP_VOLUMETEXTURE_ 0x00000002
#define D3DFORMAT_OP_CUBETEXTURE_   0x00000004
#define D3DFORMAT_OP_BUMPMAP_       0x00010000   /* the format is a bump map for BUMPENVMAP (d3dhal.h) */

/* a DX8 stream binding: where the vertices / indices are */
typedef struct _DP2STREAM {
    ULONG_PTR mem;
    ULONG bytes, stride;
    ULONG handle;               /* the buffer's surface handle (0: the DP2 call's own vertex buffer, or nothing bound) */
    BOOL vram;                  /* the buffer lives in VRAM (v9): a draw names it instead of copying it */
} DP2STREAM;

/* the surface table's entries (DX8 DDI): every surface the OS told us
 * about, by handle */
typedef struct _SURF_LEVEL {
    ULONG_PTR mem;
    ULONG pitch;
} SURF_LEVEL;

/* a cube texture's six faces (v11), level 0 included, and the runtime's
 * handle of each face's level 0 (0 where it has none); allocated for a
 * cube root only */
typedef struct _SURF_CUBE {
    SURF_LEVEL f[6][16];
    ULONG handle[6];
} SURF_CUBE;

typedef struct _SURF {
    ULONG_PTR mem;              /* system memory: the user pointer; VRAM: the mapped address */
    ULONG size;                 /* bytes (the linear size of a buffer, pitch * height otherwise) */
    ULONG pitch, w, h, fmt;
    UCHAR used, sysmem, buffer, levels;
    SURF_LEVEL lv[15];          /* mip levels 1.. */
    void *lcl;                  /* the OS's surface object (valid until it is destroyed): the colour key lives in it */
    UCHAR ck_on;                /* the key the host was told (0xff: not yet) */
    ULONG ck_lo, ck_hi;
    ULONG vram_off;             /* VRAM: the offset the host knows the surface at */
    ULONG lock_off, lock_len;   /* a VRAM buffer: the range of the current Lock (the whole buffer when the
                                 * runtime gave none), reported as VRAM_DIRTY_RANGE at Unlock */
    SURF_CUBE *cube;            /* a cube root (v11): every face's levels, for a TEXBLT between two cubes */
    ULONG depth;                /* a volume texture (v12): level 0's slices, each pitch * h apart (0: not a volume) */
} SURF;

typedef struct _D3DCTX {
    ULONG pid;
    ULONG rt, z;                /* VRAM surface handles */
    BOOL used;
    /* the runtime's interface version (ContextCreate's dwhContext on input):
     * 4 is DirectX 8's d3d8.dll, which hands its own D3DTEXF_* filter values
     * to the driver where the DX7 runtime used D3DTFG_* / D3DTFP_* (dp2_run
     * rewrites them: the host speaks the DDI's DX7 numbering) */
    ULONG iface;
    /* the DX8 device state that persists between DrawPrimitives2 calls (the
     * runtime sends SETVERTEXSHADER / SETSTREAMSOURCE / SETINDICES only on
     * change): the vertex format, the streams, the index buffer */
    ULONG fvf;                  /* the SETVERTEXSHADER value: an FVF, or a vertex shader handle (bit 0) */
    BOOL shader;                /* fvf is a vertex shader handle */
    ULONG st_um;                /* bit n: stream n is the call's own vertex buffer (user memory) */
    ULONG st_handle[D3D_MAX_STREAMS], ib_handle;    /* the bound buffers (their memory can move between calls: a
                                 * Lock with DISCARD gives a buffer new memory, CreateSurfaceEx again) */
    ULONG st_stride[D3D_MAX_STREAMS], ib_stride;
} D3DCTX;

/* The device, as the core sees it. The per-OS layer's own device object
 * starts with one of these (NT's PDEV does), so a pointer to it is a
 * pointer to the layer's, and the doorbell callback finds its way back
 * from the encoder by offset. */
typedef struct d3dpt_core {
    volatile ULONG *regs;       /* public access range (register page) */
    PVOID fb;                   /* mapped frame buffer */
    ULONG fb_len;
    ULONG w, h, bpp, pitch;     /* the mode */

    /* the flip chain's vertical blank (see d3dpt_flip_done) */
    BOOL flip_pending;          /* a flip is still waiting to be scanned out */
    ULONG flip_frame;           /* FRAMES when it was issued */
    LONGLONG flip_qpc;          /* and when, so a stalled refresh cannot hang a game */

    /* the command window and the Direct3D state */
    ULONG cmd_offset;           /* window offset in VRAM (0 = the device has none) */
    BOOL d3d;                   /* window mapped and the host executor answered */
    d3dpt_enc enc;
    ULONG dp2_calls, dp2_errors, reg_lines;
    ULONG parse_lines, bufblt_lines;
    /* the runtime's parser for the tokens a DrawPrimitives2 stream may carry
     * that are not the driver's: the DX3 execute-buffer opcodes
     * (D3DOP_PROCESSVERTICES and friends) on the legacy path (doc 15) */
    HRESULT (APIENTRY *parse_unknown)(PVOID cmd, PVOID *next);
} d3dpt_core;

/* the core whose Direct3D is on (the primary display) */
extern d3dpt_core *d3d_core;

/* One surface as the layer describes it. Everything the core reads off
 * the OS's surface object is in here — NT's DD_SURFACE_LOCAL and 9x's
 * DDRAWI_DDRAWSURFACE_LCL hold the same facts at different offsets, and
 * this is where they meet. */
typedef struct d3dpt_surf_desc {
    void *os;                   /* the OS's own object; the core only stores it and hands it back */
    ULONG handle;
    ULONG caps;                 /* the DDSCAPS_* the OS gave it (DDSCAPS_SYSTEMMEMORY etc.) */
    ULONG caps2;                /* ddsCapsEx.dwCaps2 (0 when the surface has no "more" block) */
    ULONG depth;                /* a volume texture's depth (ddsCapsEx.dwCaps4's low word, DDSCAPS2_VOLUME); 0 otherwise */
    ULONG flags;                /* the surface's own flags: DDRAWISURF_HASPIXELFORMAT / HASCKEYSRCBLT */
    ULONG w, h;
    ULONG pitch;                /* lPitch as the OS gave it (the linear size for a compressed surface) */
    ULONG linear;               /* dwLinearSize */
    ULONG vidmem;               /* fpVidMem: the VRAM offset, or the system-memory pointer */
    ULONG fmt;                  /* what pf_format made of its pixel format; 0 = none */
    ULONG pf_flags;             /* its pixel format's flags, ~0u when it has none (the log only) */
    ULONG ck_lo, ck_hi;         /* the source colour key (ddckCKSrcBlt) */
    ULONG ck_dst_lo;            /* the destination key's low value (the log only) */
} d3dpt_surf_desc;

/* --- the hooks the per-OS layer provides --- */

/* zeroed memory, and its release; NULL when there is none */
void *d3dpt_os_alloc(ULONG bytes);
void d3dpt_os_free(void *p);
/* a monotonic tick and its frequency, for the flip timeout; freq may be NULL */
void d3dpt_os_ticks(LONGLONG *now, LONGLONG *freq);
/* describe one surface; FALSE when it has no global half (nothing to say) */
BOOL d3dpt_os_surf(d3dpt_core *c, void *os, d3dpt_surf_desc *out);
/* the surfaces attached to this one that are *not* mip levels — a flip
 * chain's other buffers, a Z buffer — written into out, at most max of
 * them; the count is the return */
ULONG d3dpt_os_attached(void *os, void **out, ULONG max);
/* everything attached to this surface, mip levels and cube faces included,
 * at most max of them; the count is the return */
ULONG d3dpt_os_attached_all(void *os, void **out, ULONG max);
/* the next (smaller) mip level attached to this surface, or NULL */
void *d3dpt_os_next_mip(void *os);

/* --- the debug log, through the adapter's DEBUG register --- */
void dbg_puts(d3dpt_core *c, const char *s);
void dbg_hex(d3dpt_core *c, const char *tag, ULONG v);

/* --- core_flip.c: the heap layout, the frame counter, the vertical blank --- */
ULONG heap_start(d3dpt_core *c);
ULONG heap_end(d3dpt_core *c);
ULONG dd_heap_end(d3dpt_core *c);
ULONG cursor_offset(d3dpt_core *c);
ULONG ddflags(d3dpt_core *c);
void wait_frame(d3dpt_core *c);
BOOL flip_done(d3dpt_core *c);
void flip_issued(d3dpt_core *c);
BOOL d3d_init(d3dpt_core *c);

/* --- core_caps.c: the caps the layer hands the runtime --- */
extern D3DHAL_GLOBALDRIVERDATA_ d3d_global;
extern D3DHAL_D3DEXTENDEDCAPS_ d3d_extcaps;
extern D3DCAPS8_ d3d_caps8;
extern DDPIXELFORMAT d3d_fmt8[32];
extern ULONG d3d_fmt8_n;
extern DDSURFACEDESC d3d_texformats[11];
extern ULONG d3d_texformats_n;
extern struct d3dpt_zformats { DWORD count; DDPIXELFORMAT pf[3]; } d3d_zformats;
void d3d_caps_init(d3dpt_core *c);
ULONG pf_format(const DDPIXELFORMAT *f);
void pf_rgb(DDPIXELFORMAT *f, ULONG bits, ULONG r, ULONG g, ULONG b, ULONG a);

/* --- core_surf.c: the surface table and its format arithmetic --- */
SURF *surf_slot(ULONG handle, BOOL create);
ULONG fmt_row_bytes(ULONG f, ULONG w);
BOOL fmt_is_dxt(ULONG f);
ULONG surf_pitch(ULONG fmt, ULONG w, ULONG lpitch);
ULONG surf_rows(ULONG fmt, ULONG h);
BOOL surf_is_target(ULONG caps);
void d3d_register_at(d3dpt_core *c, const d3dpt_surf_desc *s, ULONG offset, BOOL quiet);
void d3d_register(d3dpt_core *c, const d3dpt_surf_desc *s);
void d3d_register_chain(d3dpt_core *c, void *os);
void d3d_register_moved(d3dpt_core *c, void *os);
void d3d_handle_op(d3dpt_core *c, ULONG op, ULONG handle);
void d3d_dirty_range(d3dpt_core *c, ULONG handle, ULONG off, ULONG len);
void d3d_colorkey_op(d3dpt_core *c, ULONG handle, ULONG lo, ULONG hi, ULONG flags);
void surf_colorkey_check(d3dpt_core *c, ULONG handle);
HRESULT d3d_readback(d3dpt_core *c, ULONG handle);
void surf_forget(ULONG handle);
void surf_colorkey_set(d3dpt_core *c, ULONG handle, ULONG lo, ULONG hi);
void surf_lock_range(ULONG handle, BOOL has_rect, LONG left, LONG right);
void surf_unlock_dirty(d3dpt_core *c, ULONG handle);
ULONG surf_dxt_size(ULONG fourcc, ULONG w, ULONG h);

/* --- core_ctx.c: contexts, render targets, Clear2, scene capture --- */
extern D3DCTX d3d_ctx[D3D_MAX_CTX];
extern ULONG d3d_ctx_live;
D3DCTX *ctx_of(d3dpt_core *c, ULONG_PTR h);
HRESULT ctx_create(d3dpt_core *c, ULONG_PTR *handle, ULONG pid, ULONG rt, ULONG z);
void ctx_destroy(d3dpt_core *c, ULONG i);
HRESULT ctx_destroy_one(d3dpt_core *c, ULONG_PTR h);
void ctx_destroy_all(d3dpt_core *c, ULONG pid);
HRESULT ctx_scene_capture(d3dpt_core *c, ULONG_PTR h, BOOL end);
HRESULT ctx_set_render_target(d3dpt_core *c, ULONG_PTR h, ULONG rt, ULONG z);
HRESULT ctx_clear2(d3dpt_core *c, ULONG_PTR h, ULONG flags, ULONG colour, float z, ULONG stencil,
                   const void *rects, ULONG nrects);

/* --- core_dp2.c: the DrawPrimitives2 token stream --- */
/* everything one DrawPrimitives2 call gives the core, in neutral form */
typedef struct d3dpt_dp2_call {
    ULONG_PTR ctx;              /* the context handle */
    const UCHAR *cmd;           /* the token stream, from the command offset on */
    ULONG clen;                 /* its bytes */
    const UCHAR *vtx;           /* the DP2 vertex buffer from the vertex offset on (NULL: none) */
    ULONG vlen, vsize, vcount;  /* its bytes (count * size), the stride, the vertex count */
    ULONG vall;                 /* the whole buffer from vtx on (a runtime buffer's linear size) */
    ULONG flags;                /* the call's flags, passed through into the record */
    ULONG vertex_type;          /* the call's FVF / vertex shader handle */
    BOOL eb;                    /* the stream is a DX3 execute buffer's instructions (doc 15) */
    DWORD *rstates;             /* the runtime's render-state array, or NULL */
} d3dpt_dp2_call;

typedef struct d3dpt_dp2_result {
    HRESULT hr;                 /* what the runtime is told */
    ULONG offset;               /* the error offset, from the stream's start */
    BOOL bounce;                /* hr is COMMAND_UNPARSED and offset is where the runtime
                                 * takes over; it counts from the command buffer's start,
                                 * so the layer adds back the command offset it passed in */
} d3dpt_dp2_result;

ULONG fvf_stride(ULONG fvf);
void dp2_run(d3dpt_core *c, const d3dpt_dp2_call *call, d3dpt_dp2_result *out);

#endif /* D3DPT_CORE_H */

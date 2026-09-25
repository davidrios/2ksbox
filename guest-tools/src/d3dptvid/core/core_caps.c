/*
 * core_caps.c: what the driver claims (doc 19, "The split"). This is the
 * DX7 HAL's D3DDEVICEDESC_V1 and extended caps, the texture and Z-buffer
 * format lists, the DirectX 8 D3DCAPS8 and its format list, and the
 * pixel-format arithmetic that turns a DDPIXELFORMAT into the D3DFORMAT
 * the host knows. Every ddflags bisection knob that changes an answer is
 * read here.
 *
 * The tables are the same on every Windows; who is handed them, and the
 * callback tables that go beside them, are the layer's (doc 15 has the
 * rule that differs: DDCAPS_GDI is normal on 9x and fatal on NT).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"
#define D3DPTEXTURECAPS_POW2_ 0x00000002
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL_ 0x00000100

D3DHAL_GLOBALDRIVERDATA_ d3d_global;
D3DHAL_D3DEXTENDEDCAPS_ d3d_extcaps;
D3DCAPS8_ d3d_caps8;                        /* the DX8 DDI's caps (GetDriverInfo2) */
D3DCAPS9_ d3d_caps9;                        /* the DX9 DDI's (M16): the DX8 ones, shader model 3.0 and the DX9 fields */
DDPIXELFORMAT d3d_fmt8[32];                 /* its format list */
ULONG d3d_fmt8_n;
struct d3dpt_zformats d3d_zformats;

/* the pixel format of a DirectDraw surface as a D3DFORMAT the host knows; 0 = not mirrored */
ULONG pf_format(const DDPIXELFORMAT *f)
{
    if (f->dwFlags & DDPF_D3DFORMAT_) {
        return f->dwFourCC;                 /* the D3DFORMAT itself (the DX8 format list's entries) */
    }
    if (f->dwFlags & DDPF_FOURCC) {
        ULONG cc = f->dwFourCC;
        if (cc < 256) {
            return cc;                      /* a D3DFORMAT in the FOURCC slot */
        }
        if (cc == FOURCC_('D', 'X', 'T', '1') || cc == FOURCC_('D', 'X', 'T', '2') || cc == FOURCC_('D', 'X', 'T', '3') ||
            cc == FOURCC_('D', 'X', 'T', '4') || cc == FOURCC_('D', 'X', 'T', '5')) {
            return cc;
        }
        return 0;
    }
    if (f->dwFlags & DDPF_ZBUFFER) {
        ULONG bits = f->dwZBufferBitDepth, st = (f->dwFlags & DDPF_STENCILBUFFER) ? f->dwStencilBitDepth : 0;
        if (bits == 16) return st ? D3DFMT_D15S1_ : D3DFMT_D16_;
        if (bits == 32 || bits == 24) return st ? D3DFMT_D24S8_ : D3DFMT_D24X8_;
        return 0;
    }
    if ((f->dwFlags & DDPF_PALETTEINDEXED8) && f->dwRGBBitCount == 8) {
        return D3DFMT_P8_;                  /* the palette reaches the host in the DP2 stream (SETPALETTE / UPDATEPALETTE) */
    }
    if ((f->dwFlags & DDPF_BUMPDUDV) && !(f->dwFlags & DDPF_BUMPLUMINANCE) && f->dwBumpBitCount == 16 &&
        f->dwBumpDuBitMask == 0x00ff && f->dwBumpDvBitMask == 0xff00) {
        return D3DFMT_V8U8_;                /* a bump map (EMBM): signed du, dv, passed to the host as is */
    }
    if ((f->dwFlags & DDPF_BUMPDUDV) && (f->dwFlags & DDPF_BUMPLUMINANCE_)) {
        /* a bump map with a luminance (BUMPENVMAPLUMINANCE): the luminance
         * mask sits in the dwBBitMask slot (dwBumpLuminanceBitMask) */
        if (f->dwBumpBitCount == 16 && f->dwBumpDuBitMask == 0x001f && f->dwBumpDvBitMask == 0x03e0 && f->dwBBitMask == 0xfc00) {
            return D3DFMT_L6V5U5_;
        }
        if (f->dwBumpBitCount == 32 && f->dwBumpDuBitMask == 0xff && f->dwBumpDvBitMask == 0xff00 && f->dwBBitMask == 0xff0000) {
            return D3DFMT_X8L8V8U8_;
        }
        return 0;
    }
    /* luminance with or without its own alpha, and alpha alone: the RGB
     * names are the union slots of dwLuminanceBitCount / BitMask /
     * AlphaBitMask and dwAlphaBitDepth, spelled so because the 9x DDK's
     * DDPIXELFORMAT has no luminance names */
    if ((f->dwFlags & DDPF_LUMINANCE_) && !(f->dwFlags & DDPF_BUMPLUMINANCE_)) {
        BOOL alpha = (f->dwFlags & DDPF_ALPHAPIXELS) != 0;
        if (f->dwRGBBitCount == 8 && f->dwRBitMask == 0xff && !alpha) return D3DFMT_L8_;
        if (f->dwRGBBitCount == 16 && f->dwRBitMask == 0xff && alpha && f->dwRGBAlphaBitMask == 0xff00) return D3DFMT_A8L8_;
        if (f->dwRGBBitCount == 8 && f->dwRBitMask == 0x0f && alpha && f->dwRGBAlphaBitMask == 0xf0) return D3DFMT_A4L4_;
        return 0;
    }
    if ((f->dwFlags & (DDPF_ALPHA_ | DDPF_RGB | DDPF_LUMINANCE_)) == DDPF_ALPHA_ && f->dwRGBBitCount == 8) {
        return D3DFMT_A8_;
    }
    if (f->dwFlags & DDPF_RGB) {
        BOOL alpha = (f->dwFlags & DDPF_ALPHAPIXELS) && f->dwRGBAlphaBitMask;
        if (f->dwRGBBitCount == 32 && f->dwRBitMask == 0x00ff0000) return alpha ? D3DFMT_A8R8G8B8_ : D3DFMT_X8R8G8B8_;
        if (f->dwRGBBitCount == 16) {
            if (f->dwRBitMask == 0xf800) return D3DFMT_R5G6B5_;
            if (f->dwRBitMask == 0x7c00) return alpha ? D3DFMT_A1R5G5B5_ : D3DFMT_X1R5G5B5_;
            if (f->dwRBitMask == 0x0f00) return alpha ? D3DFMT_A4R4G4B4_ : D3DFMT_X4R4G4B4_;
            if (f->dwRBitMask == 0x00e0 && alpha && f->dwRGBAlphaBitMask == 0xff00) return D3DFMT_A8R3G3B2_;
        }
        if (f->dwRGBBitCount == 8 && f->dwRBitMask == 0xe0 && f->dwGBitMask == 0x1c && f->dwBBitMask == 0x03) {
            return D3DFMT_R3G3B2_;
        }
    }
    return 0;
}

void pf_rgb(DDPIXELFORMAT *f, ULONG bits, ULONG r, ULONG g, ULONG b, ULONG a)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    f->dwRGBBitCount = bits;
    f->dwRBitMask = r; f->dwGBitMask = g; f->dwBBitMask = b; f->dwRGBAlphaBitMask = a;
}

static void pf_p8(DDPIXELFORMAT *f)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    f->dwRGBBitCount = 8;
}

static void pf_bump(DDPIXELFORMAT *f)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_BUMPDUDV;             /* V8U8: signed du in the low byte, dv in the high */
    f->dwBumpBitCount = 16;
    f->dwBumpDuBitMask = 0x00ff;
    f->dwBumpDvBitMask = 0xff00;
}

static void pf_fourcc(DDPIXELFORMAT *f, ULONG cc)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_FOURCC;
    f->dwFourCC = cc;
}

static void pf_z(DDPIXELFORMAT *f, ULONG bits, ULONG stencil)
{
    ULONG i;
    for (i = 0; i < sizeof(*f) / 4; i++) ((ULONG *)f)[i] = 0;
    f->dwSize = sizeof(*f);
    f->dwFlags = DDPF_ZBUFFER | (stencil ? DDPF_STENCILBUFFER : 0);
    f->dwZBufferBitDepth = bits;
    f->dwStencilBitDepth = stencil;
    f->dwZBitMask = bits == 16 ? 0xffff : stencil ? 0xffffff00 : 0xffffff;
    f->dwStencilBitMask = stencil ? 0xff : 0;
}

DDSURFACEDESC d3d_texformats[11];
ULONG d3d_texformats_n;

static void fmt8_add(ULONG fmt, ULONG ops)
{
    DDPIXELFORMAT *f = &d3d_fmt8[d3d_fmt8_n++];

    f->dwSize = sizeof(*f);
    /* all D3DFORMAT-coded, the compressed ones too: d3d8.dll matches the
     * application's format against dwFourCC under this flag (a FOURCC
     * entry made CreateTexture(DXT1) fail; the surface itself needs the
     * code in DrvGetDirectDrawInfo's FOURCC list) */
    f->dwFlags = DDPF_D3DFORMAT_;
    f->dwFourCC = fmt;
    f->dwRBitMask = ops;                    /* dwOperations */
}

void d3d_caps_init(d3dpt_core *p)
{
    D3DHAL_GLOBALDRIVERDATA_ *g = &d3d_global;
    D3DCAPS8_ *c8 = &d3d_caps8;
    BOOL tnl = !(ddflags(p) & DDF_NO_TNL);
    D3DDEVICEDESC_V1_ *c = &g->hwCaps;
    D3DPRIMCAPS_ *t = &c->dpcTriCaps;
    D3DHAL_D3DEXTENDEDCAPS_ *e = &d3d_extcaps;
    ULONG i;

    for (i = 0; i < sizeof(*g) / 4; i++) ((ULONG *)g)[i] = 0;
    for (i = 0; i < sizeof(*e) / 4; i++) ((ULONG *)e)[i] = 0;
    for (i = 0; i < sizeof(d3d_texformats) / 4; i++) ((ULONG *)d3d_texformats)[i] = 0;

    g->dwSize = sizeof(*g);
    c->dwSize = sizeof(*c);
    c->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                 D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                 D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    c->dcmColorModel = D3DCOLOR_RGB_;
    /* a T&L device (ddflags 0x1000 makes it a rasterizer again): the
     * executor maps SETTRANSFORM / SETLIGHT / SETMATERIAL and the lighting
     * states onto DXVK's fixed-function pipeline, so the runtime hands us
     * untransformed vertices instead of transforming them itself */
    c->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION;
    if (tnl) c->dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    c->dtcTransformCaps.dwSize = sizeof(c->dtcTransformCaps);
    c->dtcTransformCaps.dwCaps = tnl ? D3DTRANSFORMCAPS_CLIP : 0;
    c->bClipping = tnl;
    c->dlcLightingCaps.dwSize = sizeof(c->dlcLightingCaps);
    c->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    c->dlcLightingCaps.dwCaps = tnl ? D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL : 0;
    c->dlcLightingCaps.dwNumLights = tnl ? 8 : 0;
    t->dwSize = sizeof(*t);
    t->dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    t->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS;
    t->dwZCmpCaps = D3DPCMPCAPS_ALL;
    t->dwSrcBlendCaps = D3DPBLENDCAPS_ALL;
    t->dwDestBlendCaps = D3DPBLENDCAPS_ALL;
    t->dwAlphaCmpCaps = D3DPCMPCAPS_ALL;
    t->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB |
                     D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                     D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    t->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED;
    /* Powers of two, and other sizes only conditionally (clamped, no mip
     * chain). A GeForce 2 to 4 or a Radeon claims this, and the era's
     * titles branch on it. Claiming any size at all (no POW2) sent Crimson
     * Skies down a text path that never makes its strings' textures, so
     * every list entry and name field drew its 8x8 placeholder (doc 19
     * §34). The host has no such limit; the claim is for the titles. */
    if (!(ddflags(p) & DDF_TEX_ANYSIZE)) {
        t->dwTextureCaps |= D3DPTEXTURECAPS_POW2_ | D3DPTEXTURECAPS_NONPOW2CONDITIONAL_;
    }
    t->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                             D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
                             D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                             D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    if (!(ddflags(p) & DDF_NO_ANISO)) {
        /* anisotropic filtering on the DX7 face too: its runtime sends
         * D3DTFN_ANISOTROPIC (3) / D3DTFG_ANISOTROPIC (5), which the executor
         * maps; the DX8 raster caps below start from these */
        t->dwTextureFilterCaps |= D3DPTFILTERCAPS_MINFANISOTROPIC_ | D3DPTFILTERCAPS_MAGFANISOTROPIC_;
        t->dwRasterCaps |= D3DPRASTERCAPS_ANISOTROPY_;
    }
    t->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    t->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    c->dpcLineCaps = *t;
    c->dwDeviceRenderBitDepth = DDBD_16_ | DDBD_32_;
    c->dwDeviceZBufferBitDepth = DDBD_16_ | DDBD_24_ | DDBD_32_;
    /* The DX3 execute-buffer path (d3dim.dll's IDirect3DDevice::Execute)
     * sizes its vertex buffer as max(the buffer's vertex count, this cap)
     * vertices plus a page, and its vertex-buffer constructor refuses more
     * than 65535 vertices. With 65535 here every Execute failed with
     * E_OUTOFMEMORY before a single token was built (doc 15 "Execute
     * buffers"). 2048 vertices are exactly the 64 KiB DP2 vertex buffer
     * dxg gives every context, so the runtime never has to regrow it at
     * Execute time (a regrow there left the TL buffer it then handed us
     * empty: the copied vertices went into the old one). */
    c->dwMaxBufferSize = 0;
    c->dwMaxVertexCount = (ddflags(p) & DDF_EB_MAXVERT_65535) ? 65535 : 2048;

    /* texture formats: what the host reads straight from VRAM */
    pf_rgb(&d3d_texformats[0].ddpfPixelFormat, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    pf_rgb(&d3d_texformats[1].ddpfPixelFormat, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    pf_rgb(&d3d_texformats[2].ddpfPixelFormat, 16, 0xf800, 0x07e0, 0x001f, 0);
    pf_rgb(&d3d_texformats[3].ddpfPixelFormat, 16, 0x7c00, 0x03e0, 0x001f, 0);
    pf_rgb(&d3d_texformats[4].ddpfPixelFormat, 16, 0x7c00, 0x03e0, 0x001f, 0x8000);
    pf_rgb(&d3d_texformats[5].ddpfPixelFormat, 16, 0x0f00, 0x00f0, 0x000f, 0xf000);
    pf_fourcc(&d3d_texformats[6].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '1'));
    pf_fourcc(&d3d_texformats[7].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '3'));
    pf_fourcc(&d3d_texformats[8].ddpfPixelFormat, FOURCC_('D', 'X', 'T', '5'));
    d3d_texformats_n = 9;
    if (!(ddflags(p) & DDF_NO_CKEY)) {
        /* 8-bit palettized textures (a palette per texture, what a 1997
         * title's art is stored as) and colour keying: the host expands
         * both to A8R8G8B8 (doc 15 "Palettized textures and colour keying") */
        pf_p8(&d3d_texformats[9].ddpfPixelFormat);
        d3d_texformats_n = 10;
        t->dwTextureCaps |= D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_ALPHAPALETTE;
    }
    if (!(ddflags(p) & DDF_NO_BUMP)) {
        /* the V8U8 bump map, for the DirectX 6 / 7 EMBM titles (they
         * enumerate a DDPF_BUMPDUDV texture format before offering it) */
        pf_bump(&d3d_texformats[d3d_texformats_n++].ddpfPixelFormat);
    }
    for (i = 0; i < d3d_texformats_n; i++) {
        d3d_texformats[i].dwSize = sizeof(DDSURFACEDESC);
        d3d_texformats[i].dwFlags = DDSD_PIXELFORMAT;
    }
    g->dwNumTextureFormats = d3d_texformats_n;
    g->lpTextureFormats = d3d_texformats;

    d3d_zformats.count = 3;
    pf_z(&d3d_zformats.pf[0], 16, 0);
    pf_z(&d3d_zformats.pf[1], 32, 0);
    pf_z(&d3d_zformats.pf[2], 32, 8);

    e->dwSize = sizeof(*e);
    e->dwMinTextureWidth = e->dwMinTextureHeight = 1;
    e->dwMaxTextureWidth = e->dwMaxTextureHeight = 4096;
    /* any shape a legal size makes. Left at 0 (the struct is zeroed above),
     * which says no aspect ratio at all to a title that checks one; every
     * real driver publishes a power of two here (doc 19 §34) */
    e->dwMaxTextureAspectRatio = 4096;
    if (ddflags(p) & DDF_TEX_256) {
        e->dwMaxTextureWidth = e->dwMaxTextureHeight = e->dwMaxTextureAspectRatio = 256;
    }
    e->dwMaxTextureRepeat = 8192;
    e->dwMaxAnisotropy = (ddflags(p) & DDF_NO_ANISO) ? 1 : 16;
    e->dwStencilCaps = D3DSTENCILCAPS_ALL;
    e->dwFVFCaps = 8;
    e->dwTextureOpCaps = D3DTEXOPCAPS_ALL;
    e->wMaxTextureBlendStages = 8;
    e->wMaxSimultaneousTextures = 8;
    e->dvMaxVertexW = 1.0e10f;
    if (tnl) {
        e->dwMaxActiveLights = 8;
        e->wMaxUserClipPlanes = 6;
        e->wMaxVertexBlendMatrices = 4;
        e->dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_VERTEXFOG |
                                    D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    }

    /* the DX8 DDI's caps: the same device, in D3DCAPS8 form (vertex and
     * pixel shaders 1.x run on the host; sixteen streams since v10: a draw
     * under a shader carries every bound stream's range, see walk_draw) */
    for (i = 0; i < sizeof(*c8) / 4; i++) ((ULONG *)c8)[i] = 0;
    c8->DeviceType = D3DDEVTYPE_HAL_;
    c8->Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_DYNAMICTEXTURES;
    if (p->gamma) {
        c8->Caps2 |= D3DCAPS2_FULLSCREENGAMMA_;     /* SetGammaRamp reaches the adapter's GAMMA block (register set v5) */
    }
    c8->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    c8->DevCaps = c->dwDevCaps | D3DDEVCAPS_PUREDEVICE;
    /* vertex / index buffers in video memory (v9): the runtime creates
     * D3DPOOL_DEFAULT buffers through the buffer callbacks with
     * DDSCAPS_VIDEOMEMORY, fills them by Lock / Unlock (its MANAGED ones by
     * BUFFERBLT from their system-memory copy), and a draw names a buffer
     * the host reads from VRAM instead of copying the vertices into the
     * record (D3dDrawPrimitives2) */
    if (!(ddflags(p) & DDF_NO_HWVB)) c8->DevCaps |= D3DDEVCAPS_HWVERTEXBUFFER_ | D3DDEVCAPS_HWINDEXBUFFER_;
    /* no CLIPTLVERTS: with it the runtime stops clipping pre-transformed
     * vertices and hands us polygons crossing the camera plane, which the
     * host rasterizes as garbage (Max Payne's alley walls). Without it the
     * runtime clips them itself, as the DX7 runtime did */
    c8->PrimitiveMiscCaps = t->dwMiscCaps | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP;
    c8->RasterCaps = t->dwRasterCaps | D3DPRASTERCAPS_COLORPERSPECTIVE | ((ddflags(p) & DDF_NO_ANISO) ? 0 : D3DPRASTERCAPS_ANISOTROPY_);
    c8->ZCmpCaps = t->dwZCmpCaps;
    c8->SrcBlendCaps = t->dwSrcBlendCaps;
    c8->DestBlendCaps = t->dwDestBlendCaps;
    c8->AlphaCmpCaps = t->dwAlphaCmpCaps;
    c8->ShadeCaps = t->dwShadeCaps;
    c8->TextureCaps = (t->dwTextureCaps & ~D3DPTEXTURECAPS_TRANSPARENCY) | D3DPTEXTURECAPS_MIPMAP;   /* DX8 has no colour key; bit 3 is unused there */
    c8->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                            D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    if (!(ddflags(p) & DDF_NO_ANISO)) {
        /* anisotropic filtering, DXVK's own (the cube and volume caps below
         * copy these): the executor maps the anisotropic filters and
         * MAXANISOTROPY already */
        c8->TextureFilterCaps |= D3DPTFILTERCAPS_MINFANISOTROPIC_ | D3DPTFILTERCAPS_MAGFANISOTROPIC_;
    }
    if (!(ddflags(p) & DDF_NO_CUBE)) {
        /* cube textures (v11), mip-mapped too: the DX8 face only (a
         * DirectX 7 cube map is created through DirectDraw's own surface
         * caps, which this driver does not answer). Power-of-two edges
         * wherever 2D textures are, as the era's cards: without CUBEMAP_POW2
         * the runtime made a cube of edge 3 that real drivers refuse (Wine's
         * d3d9 device.c:10178, M16) */
        c8->TextureCaps |= D3DPTEXTURECAPS_CUBEMAP_ | D3DPTEXTURECAPS_MIPCUBEMAP_;
        if (t->dwTextureCaps & D3DPTEXTURECAPS_POW2_) c8->TextureCaps |= D3DPTEXTURECAPS_CUBEMAP_POW2_;
        c8->CubeTextureFilterCaps = c8->TextureFilterCaps;
    }
    if (!(ddflags(p) & DDF_NO_VOLUME)) {
        /* volume textures, mip-mapped too, up to 256: the DX8 face only,
         * like the cubes, and power-of-two extents by the same rule
         * (device.c:10194) */
        c8->TextureCaps |= D3DPTEXTURECAPS_VOLUMEMAP_ | D3DPTEXTURECAPS_MIPVOLUMEMAP_;
        if (t->dwTextureCaps & D3DPTEXTURECAPS_POW2_) c8->TextureCaps |= D3DPTEXTURECAPS_VOLUMEMAP_POW2_;
        c8->VolumeTextureFilterCaps = c8->TextureFilterCaps;
        c8->VolumeTextureAddressCaps = t->dwTextureAddressCaps | D3DPTADDRESSCAPS_MIRRORONCE;
        c8->MaxVolumeExtent = 256;
    }
    c8->TextureAddressCaps = t->dwTextureAddressCaps | D3DPTADDRESSCAPS_MIRRORONCE;
    c8->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    c8->MaxTextureWidth = e->dwMaxTextureWidth;
    c8->MaxTextureHeight = e->dwMaxTextureHeight;
    c8->MaxTextureAspectRatio = e->dwMaxTextureAspectRatio;
    c8->MaxTextureRepeat = 8192;
    c8->MaxAnisotropy = (ddflags(p) & DDF_NO_ANISO) ? 1 : 16;
    c8->MaxVertexW = 1.0e10f;
    c8->StencilCaps = D3DSTENCILCAPS_ALL;
    c8->FVFCaps = 8 | D3DFVFCAPS_PSIZE;         /* a per-vertex point size: the driver and the host carry D3DFVF_PSIZE */
    c8->TextureOpCaps = D3DTEXOPCAPS_ALL;
    c8->MaxTextureBlendStages = 8;
    c8->MaxSimultaneousTextures = 8;
    c8->VertexProcessingCaps = e->dwVertexProcessingCaps;
    c8->MaxActiveLights = e->dwMaxActiveLights;
    c8->MaxUserClipPlanes = e->wMaxUserClipPlanes;
    c8->MaxVertexBlendMatrices = e->wMaxVertexBlendMatrices;
    c8->MaxPointSize = 64.0f;
    c8->MaxPrimitiveCount = 0xffff;
    c8->MaxVertexIndex = 0xffff;
    c8->MaxStreams = (ddflags(p) & DDF_ONE_STREAM) ? 1 : D3D_MAX_STREAMS;
    c8->MaxStreamStride = 256;
    if (ddflags(p) & DDF_NO_SHADERS) {
        c8->VertexShaderVersion = D3DVS_VERSION_0;
        c8->PixelShaderVersion = D3DPS_VERSION_0;
    } else {
        /* vs 1.1 / ps 1.4 (what DXVK's d3d9 compiles of the 1.x models; the
         * runtime validates every shader against these before the
         * CREATE*SHADER token reaches us); 96 vertex constants as the era's
         * hardware, ps 1.x values clamped to +-8 */
        c8->VertexShaderVersion = D3DVS_VERSION_(1, 1);
        c8->MaxVertexShaderConst = 96;
        c8->PixelShaderVersion = D3DPS_VERSION_(1, 4);
        c8->MaxPixelShaderValue = 8.0f;
    }

    /* its format list (DDPF_D3DFORMAT entries: the D3DFORMAT in dwFourCC,
     * the operations in the dwRBitMask slot) */
    for (i = 0; i < sizeof(d3d_fmt8) / 4; i++) ((ULONG *)d3d_fmt8)[i] = 0;
    d3d_fmt8_n = 0;
    {
        /* v11: every RGB and DXT texture format as a cube too (a
         * render-target cube wherever the format is a render target); not
         * P8, whose palettes the host keeps per 2D texture */
        ULONG cube = (ddflags(p) & DDF_NO_CUBE) ? 0 : D3DFORMAT_OP_CUBETEXTURE_;
        /* volume textures on the RGB and DXT formats (DdCreateSurface sizes
         * the box in the format's own rows: block rows for DXT) */
        ULONG vol = (ddflags(p) & DDF_NO_VOLUME) ? 0 : D3DFORMAT_OP_VOLUMETEXTURE_;

        fmt8_add(D3DFMT_X8R8G8B8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_DISPLAYMODE_ | D3DFORMAT_OP_3DACCELERATION_ |
                                   D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_ | cube | vol);
        /* UP_TO_ALPHA: an A8R8G8B8 back buffer on the X8R8G8B8 desktop, as
         * every 32-bit card allows. Without it d3d8.dll refused a windowed
         * device with one (every Wine d3d8 test, M16) */
        fmt8_add(D3DFMT_A8R8G8B8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_ |
                                   D3DFORMAT_OP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET_ | cube | vol);
        fmt8_add(D3DFMT_R5G6B5_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_DISPLAYMODE_ | D3DFORMAT_OP_3DACCELERATION_ |
                                 D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_ | cube | vol);
        fmt8_add(D3DFMT_X1R5G5B5_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_ | cube | vol);
        fmt8_add(D3DFMT_A1R5G5B5_, D3DFORMAT_OP_TEXTURE_ | cube | vol);
        fmt8_add(D3DFMT_A4R4G4B4_, D3DFORMAT_OP_TEXTURE_ | cube | vol);
        fmt8_add(FOURCC_('D', 'X', 'T', '1'), D3DFORMAT_OP_TEXTURE_ | cube | vol);
        fmt8_add(FOURCC_('D', 'X', 'T', '3'), D3DFORMAT_OP_TEXTURE_ | cube | vol);
        fmt8_add(FOURCC_('D', 'X', 'T', '5'), D3DFORMAT_OP_TEXTURE_ | cube | vol);
    }
    if (!(ddflags(p) & DDF_NO_MORE_FMTS)) {
        /* the rest of DX8's texture formats (FMTTEST): the luminance ones,
         * alpha alone, and the premultiplied DXTs go to the host as they are;
         * the host expands R3G3B2 / A8R3G3B2 (and whatever else its device
         * lacks) to A8R8G8B8. 2D textures only: no cube or volume op yet */
        fmt8_add(D3DFMT_L8_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_A8L8_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_A4L4_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_A8_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_X4R4G4B4_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_R3G3B2_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(D3DFMT_A8R3G3B2_, D3DFORMAT_OP_TEXTURE_);
        fmt8_add(FOURCC_('D', 'X', 'T', '2'), D3DFORMAT_OP_TEXTURE_);
        fmt8_add(FOURCC_('D', 'X', 'T', '4'), D3DFORMAT_OP_TEXTURE_);
    }
    if (!(ddflags(p) & DDF_NO_CKEY)) {
        fmt8_add(D3DFMT_P8_, D3DFORMAT_OP_TEXTURE_);
    }
    if (!(ddflags(p) & DDF_NO_BUMP)) {
        /* the bump map EMBM needs (TextureOpCaps claims BUMPENVMAP): DXVK's
         * fixed function does the op, the texels go to the host as they are */
        fmt8_add(D3DFMT_V8U8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_BUMPMAP_);
        /* the luminance bump maps BUMPENVMAPLUMINANCE reads: DXVK converts
         * both to float itself at upload */
        fmt8_add(D3DFMT_L6V5U5_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_BUMPMAP_);
        fmt8_add(D3DFMT_X8L8V8U8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_BUMPMAP_);
        /* the signed normal map (DXVK: R8G8B8A8_SNORM). 3DMark2001 SE asks
         * for it (or W11V11U10) before Nature and refuses the test without
         * ("device does not support bump normal maps"). It has no
         * DDPIXELFORMAT of its own, so the runtime creates it as a FOURCC
         * surface whose code is the D3DFORMAT: the layers list 63 among
         * their FOURCC codes and size it in CreateSurface. Without that,
         * DirectDraw refused it before CanCreateSurface and the texture had
         * no video-memory copy (doc 15, BUMPTEST on XP) */
        fmt8_add(D3DFMT_Q8W8V8U8_, D3DFORMAT_OP_TEXTURE_ | D3DFORMAT_OP_BUMPMAP_);
    }
    fmt8_add(D3DFMT_D16_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);
    fmt8_add(D3DFMT_D24X8_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);
    fmt8_add(D3DFMT_D24S8_, D3DFORMAT_OP_ZSTENCIL_ | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_);
    if (!(ddflags(p) & DDF_NO_MSAA)) {
        /* multisampling (v13): 2 and 4 samples on the render-target and depth
         * formats, full screen only. MultiSampleCaps shares the green-mask
         * slot: wFlipMSTypes the low word, wBltMSTypes the high, bit n - 1 for
         * D3DMULTISAMPLE_n_SAMPLES (MSAATEST: d3d8.dll reports 2 and 4). No blt
         * types: a windowed Present of a multisampled back buffer is a driver
         * blt, and this driver has no blitter. With them claimed, d3d8.dll
         * made the device and Present drew nothing (no DdBlt, no readback).
         * A flip needs nothing new: its readback goes through the host's
         * resolve. Bit 0 is DX9's NONMASKABLE: d3d9.dll refuses a DX9
         * driver whose format claims sample counts without it (the last
         * check of its caps validation, M16); DX8 has no type 1 */
        const ULONG ms = 1u | (1u << (2 - 1)) | (1u << (4 - 1));

        for (i = 0; i < d3d_fmt8_n; i++) {
            ULONG f = d3d_fmt8[i].dwFourCC;
            if (f == D3DFMT_X8R8G8B8_ || f == D3DFMT_A8R8G8B8_ || f == D3DFMT_R5G6B5_ || f == D3DFMT_X1R5G5B5_ ||
                f == D3DFMT_D16_ || f == D3DFMT_D24X8_ || f == D3DFMT_D24S8_) {
                d3d_fmt8[i].dwGBitMask = ms;
            }
        }
    }

    /* the DX9 DDI's caps (M16): the device of the DX8 ones, with shader
     * model 3.0 as a GeForce 6 claims it (the reference rig's card, doc
     * 09), or 2.0 as a Radeon 9700 does (DDF_SM2, the A/B). The shader
     * bytecode goes to the host as it is and DXVK compiles every version,
     * so the claim is these fields; the DP2 tokens are the work (core_dp2.c).
     * d3d8.dll never sees them: it asks for GETD3DCAPS8 */
    for (i = 0; i < sizeof(d3d_caps9) / 4; i++) ((ULONG *)&d3d_caps9)[i] = 0;
    d3d_caps9.c8 = *c8;
    if (!(ddflags(p) & DDF_NO_SHADERS)) {
        BOOL sm3 = !(ddflags(p) & DDF_SM2);

        d3d_caps9.c8.VertexShaderVersion = sm3 ? D3DVS_VERSION_(3, 0) : D3DVS_VERSION_(2, 0);
        d3d_caps9.c8.MaxVertexShaderConst = 256;
        d3d_caps9.c8.PixelShaderVersion = sm3 ? D3DPS_VERSION_(3, 0) : D3DPS_VERSION_(2, 0);
        d3d_caps9.VS20Caps.Caps = sm3 ? 1 : 0;                         /* D3DVS20CAPS_PREDICATION */
        d3d_caps9.VS20Caps.DynamicFlowControlDepth = sm3 ? 24 : 0;
        d3d_caps9.VS20Caps.NumTemps = sm3 ? 32 : 12;
        d3d_caps9.VS20Caps.StaticFlowControlDepth = sm3 ? 4 : 1;
        /* arbitrary swizzle, gradients, predication, no dependent-read
         * and no texture-instruction limit */
        d3d_caps9.PS20Caps.Caps = sm3 ? 0x1f : 0;
        d3d_caps9.PS20Caps.DynamicFlowControlDepth = sm3 ? 24 : 0;
        d3d_caps9.PS20Caps.NumTemps = sm3 ? 32 : 12;
        d3d_caps9.PS20Caps.StaticFlowControlDepth = sm3 ? 4 : 0;
        d3d_caps9.PS20Caps.NumInstructionSlots = sm3 ? 512 : 96;
        d3d_caps9.MaxVShaderInstructionsExecuted = 65535;
        d3d_caps9.MaxPShaderInstructionsExecuted = sm3 ? 65535 : 96;
        d3d_caps9.MaxVertexShader30InstructionSlots = sm3 ? 512 : 0;
        d3d_caps9.MaxPixelShader30InstructionSlots = sm3 ? 512 : 0;
    }
    /* What d3d9.dll's check of a DX9 driver's caps demands (XP SP3's
     * d3d9.dll at 0x4fcf6d20, read in its disassembly and confirmed under
     * the QEMU gdbstub; a driver that fails it gets the runtime's DX7-level
     * caps: no T&L, no streams, no shaders). The render states behind them
     * reach DXVK as they are (rs_passthrough in the executor): scissor
     * enable 174, depth bias 175 / 195, two-sided stencil 185..188, blend
     * factor 193. Every DX9 driver: scissor. vs / ps 2.0 and up: FOGINFVF.
     * ps 3.0: depth bias, the blend factor, two-sided stencil, texture
     * repeat in texels. vs 3.0: a guard band of at least 8192 each way and
     * point-sampled vertex textures. The DX8 caps stay as they were */
    d3d_caps9.c8.RasterCaps |= D3DPRASTERCAPS_SCISSORTEST_ | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS_ | D3DPRASTERCAPS_DEPTHBIAS_;
    d3d_caps9.c8.PrimitiveMiscCaps |= D3DPMISCCAPS_FOGINFVF_;
    d3d_caps9.c8.SrcBlendCaps |= D3DPBLENDCAPS_BLENDFACTOR_;
    d3d_caps9.c8.DestBlendCaps |= D3DPBLENDCAPS_BLENDFACTOR_;
    d3d_caps9.c8.StencilCaps |= D3DSTENCILCAPS_TWOSIDED_;
    d3d_caps9.c8.TextureCaps |= D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE_;
    d3d_caps9.c8.GuardBandLeft = -8192.0f;
    d3d_caps9.c8.GuardBandTop = -8192.0f;
    d3d_caps9.c8.GuardBandRight = 8192.0f;
    d3d_caps9.c8.GuardBandBottom = 8192.0f;
    if (d3d_caps9.MaxVertexShader30InstructionSlots) {
        d3d_caps9.VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT;
    }
    /* SETSTREAMSOURCE2's offset is the walker's (core_dp2.c) */
    d3d_caps9.DevCaps2 = D3DDEVCAPS2_STREAMOFFSET_ | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET_;
    d3d_caps9.DeclTypes = 0x3ff;                /* UBYTE4 .. FLOAT16_4: DXVK reads every one */
    d3d_caps9.NumSimultaneousRTs = 1;           /* SETRENDERTARGET2 beyond index 0 is not walked yet */
    d3d_caps9.NumberOfAdaptersInGroup = 1;
}

/* The GetDriverInfo2 queries (DX8 DDI, and DX9's since M16). The answer
 * goes into the same buffer; the size that counts is the one inside the
 * GDI2 header. d3d8.dll leaves the outer dwExpectedSize at the previous
 * query's 24 bytes and rejects the driver unless dwActualSize equals the
 * inner one (found by disassembling d3d8.dll, doc 15). A DX9 query is
 * answered only where the layer offers the DX9 face (c->dx9) and
 * DDF_NO_DX9 is clear; refused, d3d9.dll treats the driver as a DX8 one,
 * which it was before M16. */
HRESULT core_gdi2_answer(d3dpt_core *c, void *data, ULONG *actual)
{
    DD_GETDRIVERINFO2DATA_ *g = (DD_GETDRIVERINFO2DATA_ *)data;
    ULONG want = g->dwExpectedSize, n, i;
    BOOL dx9 = c->dx9 && !(ddflags(c) & DDF_NO_DX9);

    dbg_hex(c, "d3dptdisp: gdi2 type ", g->dwType);
    dbg_hex(c, " expected ", want);
    dbg_puts(c, "\n");
    *actual = 0;
    switch (g->dwType) {
    case D3DGDI2_TYPE_GETD3DCAPS8_:
        n = sizeof(d3d_caps8);
        if (n > want) n = want;
        memcpy(data, &d3d_caps8, n);
        *actual = n;
        return DD_OK;
    case D3DGDI2_TYPE_GETD3DCAPS9_:
        if (!dx9) return DDERR_CURRENTLYNOTAVAIL;
        n = sizeof(d3d_caps9);
        if (n > want) n = want;
        memcpy(data, &d3d_caps9, n);
        *actual = n;
        return DD_OK;
    case D3DGDI2_TYPE_GETFORMATCOUNT_: {
        DD_GETFORMATCOUNTDATA_ *f = (DD_GETFORMATCOUNTDATA_ *)g;
        if (want < sizeof(*f)) return DDERR_CURRENTLYNOTAVAIL;
        f->dwFormatCount = d3d_fmt8_n;
        *actual = sizeof(*f);
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETFORMAT_: {
        DD_GETFORMATDATA_ *f = (DD_GETFORMATDATA_ *)g;
        if (want < sizeof(*f) || f->dwFormatIndex >= d3d_fmt8_n) return DDERR_CURRENTLYNOTAVAIL;
        f->format = d3d_fmt8[f->dwFormatIndex];
        *actual = sizeof(*f);
        return DD_OK;
    }
    case D3DGDI2_TYPE_DXVERSION_: {
        DD_DXVERSION_ *v = (DD_DXVERSION_ *)g;
        if (want >= sizeof(*v)) {
            dbg_hex(c, "d3dptdisp: runtime DirectX version ", v->dwDXVersion);
            dbg_puts(c, "\n");
        }
        *actual = sizeof(*v) <= want ? sizeof(*v) : want;
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETDDIVERSION_: {
        /* the question that makes d3d9.dll a DX9 runtime to us: from here
         * on it sends the DX9 tokens (vertex declarations and shader code
         * apart, SETSTREAMSOURCE2, SETRENDERTARGET2, ...) */
        DD_GETDDIVERSIONDATA_ *v = (DD_GETDDIVERSIONDATA_ *)g;
        if (want >= sizeof(*v)) {
            dbg_hex(c, "d3dptdisp: ddi version asked, runtime ", v->dwDXVersion);
            dbg_hex(c, " dx9 face ", dx9);
            dbg_puts(c, "\n");
        }
        /* the runtime's major version here: 9 (measured on XP's d3d9.dll,
         * whose DXVERSION notice says 0x902) */
        if (!dx9 || want < sizeof(*v) || v->dwDXVersion < 9) return DDERR_CURRENTLYNOTAVAIL;
        v->dwDDIVersion = DX9_DDI_VERSION_;
        *actual = sizeof(*v);
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETEXTENDEDMODECOUNT_: {
        DD_GETEXTENDEDMODECOUNTDATA_ *m = (DD_GETEXTENDEDMODECOUNTDATA_ *)g;
        if (!dx9 || want < sizeof(*m)) return DDERR_CURRENTLYNOTAVAIL;
        m->dwModeCount = 0;                     /* no A2R10G10B10 desktop */
        *actual = sizeof(*m);
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETADAPTERGROUP_: {
        DD_GETADAPTERGROUPDATA_ *a = (DD_GETADAPTERGROUPDATA_ *)g;
        if (!dx9 || want < sizeof(*a)) return DDERR_CURRENTLYNOTAVAIL;
        a->ulUniqueAdapterGroupId = (ULONG_PTR)c;   /* one head, its own group */
        *actual = sizeof(*a);
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETMULTISAMPLEQUALITYLEVELS_: {
        /* one quality level per sample count the format list gives the
         * format (its dwGBitMask slot, bit n - 1); NONMASKABLE has one level
         * per such count */
        DD_MULTISAMPLEQUALITYLEVELSDATA_ *m = (DD_MULTISAMPLEQUALITYLEVELSDATA_ *)g;
        ULONG ms = 0, t;
        if (!dx9 || want < sizeof(*m)) return DDERR_CURRENTLYNOTAVAIL;
        for (i = 0; i < d3d_fmt8_n; i++) {
            if (d3d_fmt8[i].dwFourCC == m->Format) ms = d3d_fmt8[i].dwGBitMask & 0xffff;
        }
        t = m->MSType;
        if (t == 1) {
            for (n = 0, ms &= ~1u; ms; ms &= ms - 1) n++;   /* bit 0 is NONMASKABLE itself */
            m->QualityLevels = n;
        } else {
            m->QualityLevels = (t >= 2 && t <= 16 && (ms & (1u << (t - 1)))) ? 1 : 0;
        }
        *actual = sizeof(*m);
        return DD_OK;
    }
    case D3DGDI2_TYPE_GETD3DQUERYCOUNT_: {
        /* no query types yet: RESPONSEQUERY is M16 step 2's */
        DD_GETD3DQUERYCOUNTDATA_ *q = (DD_GETD3DQUERYCOUNTDATA_ *)g;
        if (!dx9 || want < sizeof(*q)) return DDERR_CURRENTLYNOTAVAIL;
        q->dwNumQueries = 0;
        *actual = sizeof(*q);
        return DD_OK;
    }
    default:
        return DDERR_CURRENTLYNOTAVAIL;
    }
}

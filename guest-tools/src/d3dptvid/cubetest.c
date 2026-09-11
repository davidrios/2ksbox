/*
 * cubetest.c — cube textures through XP's own d3d8.dll on our driver's DX8
 * DDI (doc 15 "Cube textures", protocol v11): the runtime creates a cube as
 * six DirectDraw faces hanging off +X, the driver mirrors them as one host
 * cube, and a draw samples it with a 3D texture coordinate. A probe
 * (d3d8probe.h): with no D3DPTEXTURECAPS_CUBEMAP it says "not offered"
 * (what ddflags=0x400000 turns it back into).
 *
 *   CUBETEST
 *
 * The caps and CheckDeviceFormat answers for cube textures first, then
 * one case after the other, each a draw read back at a few pixels:
 *   a MANAGED A8R8G8B8 cube of two levels, every face and level its own
 *   colour: a quad at each face's direction (XYZRHW + a 3D texture
 *   coordinate), and a small quad +Z is minified onto (level 1);
 *   one of its faces locked and rewritten (the host must read it again);
 *   a DEFAULT cube filled by UpdateTexture from a SYSTEMMEM one (TEXBLT);
 *   a DXT1 cube (the driver sizes compressed faces for dxg's heap);
 *   a render-target cube whose six faces are cleared through their own
 *   surfaces (SetRenderTarget on GetCubeMapSurface), then sampled;
 *   fixed-function texture generation (TCI_CAMERASPACENORMAL) into a cube;
 *   a vs 1.1 writing the direction to oT0.
 * Every HRESULT and pixel is in cubetest.log.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#define FVF_CUBE (D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0))
struct cvtx { float x, y, z, rhw; float u, v, w; };

/* the faces' colours, D3DCUBEMAP_FACES order (+X -X +Y -Y +Z -Z): level 0,
 * level 1, and the render-target cube's */
static const DWORD C0[6] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff };
static const DWORD C1[6] = { 0xff800000, 0xff008000, 0xff000080, 0xff808000, 0xff800080, 0xff008080 };
static const DWORD RC[6] = { 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffff0000, 0xff00ff00, 0xff0000ff };
static const float DIR[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

/* shader model 1.x by hand (as SHTEST): the version, instruction tokens,
 * parameter tokens (bit 31, register type in bits 28..30) */
#define VS11        0xFFFE0101u
#define SH_END      0x0000FFFFu
#define OP_MOV      1u
#define R_INPUT     1u
#define R_RASTOUT   4u
#define R_TEXCRDOUT 6u
#define DST(type, n) (0x80000000u | ((type) << 28) | (0xFu << 16) | (n))
#define SRC(type, n) (0x80000000u | ((type) << 28) | (0xE4u << 16) | (n))

/* oPos = v0, oT0 = v7 (the direction) */
static const DWORD vs_code[] = {
    VS11, OP_MOV, DST(R_RASTOUT, 0), SRC(R_INPUT, D3DVSDE_POSITION), OP_MOV, DST(R_TEXCRDOUT, 0), SRC(R_INPUT, D3DVSDE_TEXCOORD0), SH_END,
};
static const DWORD vs_decl[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT4), D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT3), D3DVSD_END()
};

/* the six quads' centres: 40 pixels each along y = 20..60, face f at 30 + 50 f */
static void check_faces(const char *name, const DWORD *want)
{
    int xs[6], ys[6], f;
    for (f = 0; f < 6; f++) {
        xs[f] = 30 + 50 * f;
        ys[f] = 40;
    }
    check(name, 6, xs, ys, want);
}

/* a quad of sz pixels at (x0, y0), every vertex the same direction */
static void quad_dir(float x0, float y0, float sz, const float *d)
{
    struct cvtx v[4];
    int i;
    for (i = 0; i < 4; i++) {
        v[i].x = x0 + ((i & 1) ? sz : 0.0f);
        v[i].y = y0 + ((i & 2) ? sz : 0.0f);
        v[i].z = 0.5f;
        v[i].rhw = 1.0f;
        v[i].u = d[0];
        v[i].v = d[1];
        v[i].w = d[2];
    }
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

/* the cube bound, a quad at each face's direction (inside a scene) */
static void draw_faces(IDirect3DCubeTexture8 *c)
{
    int f;
    IDirect3DDevice8_SetVertexShader(dev, FVF_CUBE);
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)c);
    for (f = 0; f < 6; f++) {
        quad_dir(10.0f + 50.0f * f, 20.0f, 40.0f, DIR[f]);
    }
}

static WORD rgb565(DWORD c)
{
    return (WORD)((((c >> 19) & 0x1f) << 11) | (((c >> 10) & 0x3f) << 5) | ((c >> 3) & 0x1f));
}

/* every texel of one face level one colour (32-bit formats, and DXT1 as
 * blocks whose two colours are both it) */
static HRESULT fill_level(IDirect3DCubeTexture8 *c, int face, int level, D3DFORMAT fmt, DWORD col)
{
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    UINT x, y;
    HRESULT hr = IDirect3DCubeTexture8_GetLevelDesc(c, level, &sd);

    if (FAILED(hr)) return hr;
    hr = IDirect3DCubeTexture8_LockRect(c, (D3DCUBEMAP_FACES)face, level, &lr, NULL, 0);
    if (FAILED(hr)) return hr;
    if (fmt == D3DFMT_DXT1) {
        WORD c565 = rgb565(col);
        for (y = 0; y < (sd.Height + 3) / 4; y++) {
            BYTE *row = (BYTE *)lr.pBits + y * lr.Pitch;
            for (x = 0; x < (sd.Width + 3) / 4; x++) {
                memcpy(row + x * 8, &c565, 2);
                memcpy(row + x * 8 + 2, &c565, 2);
                memset(row + x * 8 + 4, 0, 4);
            }
        }
    } else {
        for (y = 0; y < sd.Height; y++) {
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
            for (x = 0; x < sd.Width; x++) row[x] = col;
        }
    }
    return IDirect3DCubeTexture8_UnlockRect(c, (D3DCUBEMAP_FACES)face, level);
}

static IDirect3DCubeTexture8 *make_cube(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                        const DWORD *c0, const DWORD *c1, const char *what)
{
    IDirect3DCubeTexture8 *c = NULL;
    HRESULT hr = IDirect3DDevice8_CreateCubeTexture(dev, edge, levels, usage, fmt, pool, &c);
    int f;

    logp("CreateCubeTexture (%s: edge %u, %u levels, format %d, pool %d) 0x%08lx\n", what, edge, levels, (int)fmt, (int)pool, (unsigned long)hr);
    if (FAILED(hr) || !c) return NULL;
    for (f = 0; c0 && f < 6; f++) {
        hr = fill_level(c, f, 0, fmt, c0[f]);
        if (SUCCEEDED(hr) && c1 && levels > 1) hr = fill_level(c, f, 1, fmt, c1[f]);
        if (FAILED(hr)) logp("  face %d: fill 0x%08lx\n", f, (unsigned long)hr);
    }
    return c;
}

int main(void)
{
    IDirect3DCubeTexture8 *cm = NULL, *cs = NULL, *cd = NULL, *cx = NULL, *cr = NULL;
    DWORD h_vs = 0;
    HRESULT hr;
    int f, dxt1 = 0;
    static const D3DFORMAT fmts[] = { D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_DXT1 };
    static const char *fmt_names[] = { "X8R8G8B8", "A8R8G8B8", "R5G6B5", "DXT1" };

    if (!probe_open("cubetest")) return 1;
    logp("cubetest: cube %s, mip-mapped cube %s, cube filter caps 0x%08lx\n", (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) ? "yes" : "no",
         (caps.TextureCaps & D3DPTEXTURECAPS_MIPCUBEMAP) ? "yes" : "no", (unsigned long)caps.CubeTextureFilterCaps);
    for (f = 0; f < 4; f++) {
        HRESULT tex = probe_format(fmt_names[f], D3DRTYPE_CUBETEXTURE, 0, fmts[f]);
        probe_format(fmt_names[f], D3DRTYPE_CUBETEXTURE, D3DUSAGE_RENDERTARGET, fmts[f]);
        if (fmts[f] == D3DFMT_DXT1) dxt1 = SUCCEEDED(tex);
    }
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)) {
        return probe_close("no D3DPTEXTURECAPS_CUBEMAP in the caps");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    /* LINEAR, not POINT: every level-1 texel is one colour, so the two agree,
     * and the executor maps the DX7 and DX8 encodings of this state
     * differently (doc 15: a lead on D3DGAME8's filtering difference) */
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);

    /* --- a managed cube of two levels --- */
    cm = make_cube(16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, C0, C1, "managed");
    if (cm) {
        /* +Z spread over 4 x 4 pixels: 4 texels a pixel, level 2 asked, level 1 the last */
        struct cvtx mq[4] = { { 150, 150, 0.5f, 1, -1, 1, 1 }, { 154, 150, 0.5f, 1, 1, 1, 1 },
                              { 150, 154, 0.5f, 1, -1, -1, 1 }, { 154, 154, 0.5f, 1, 1, -1, 1 } };
        int xs[2] = { 151, 300 }, ys[2] = { 151, 230 };
        DWORD w[6];

        begin();
        draw_faces(cm);
        end();
        check_faces("managed cube: a quad at each face's direction", C0);
        begin();
        draw_faces(cm);
        IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, mq, sizeof mq[0]);
        end();
        w[0] = C1[4];
        w[1] = CLEAR_COLOR;
        check("managed cube: level 1 where +Z is minified", 2, xs, ys, w);
        hr = fill_level(cm, D3DCUBEMAP_FACE_POSITIVE_Y, 0, D3DFMT_A8R8G8B8, 0xffffffff);
        logp("face +Y locked and rewritten white 0x%08lx\n", (unsigned long)hr);
        memcpy(w, C0, sizeof w);
        w[2] = 0xffffffff;
        begin();
        draw_faces(cm);
        end();
        check_faces("managed cube: face +Y rewritten by Lock", w);
    } else {
        fail_case("managed cube");
    }

    /* --- a default-pool cube filled by UpdateTexture (the DDI's TEXBLT) --- */
    cs = make_cube(16, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, C1, NULL, "system memory");
    cd = make_cube(16, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL, "default");
    if (cs && cd) {
        hr = IDirect3DDevice8_UpdateTexture(dev, (IDirect3DBaseTexture8 *)cs, (IDirect3DBaseTexture8 *)cd);
        logp("UpdateTexture 0x%08lx\n", (unsigned long)hr);
        begin();
        draw_faces(cd);
        end();
        check_faces("default cube filled by UpdateTexture", C1);
        /* again, into a cube the host has already uploaded: this time only
         * a write the host is told about can change what it draws */
        for (f = 0; f < 6; f++) fill_level(cs, f, 0, D3DFMT_X8R8G8B8, RC[f]);
        hr = IDirect3DDevice8_UpdateTexture(dev, (IDirect3DBaseTexture8 *)cs, (IDirect3DBaseTexture8 *)cd);
        logp("UpdateTexture (new colours) 0x%08lx\n", (unsigned long)hr);
        begin();
        draw_faces(cd);
        end();
        check_faces("default cube: UpdateTexture again after it was drawn", RC);
    } else {
        fail_case("default cube filled by UpdateTexture");
    }

    /* --- a DXT1 cube --- */
    if (dxt1) {
        cx = make_cube(16, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, C0, NULL, "DXT1");
        if (cx) {
            begin();
            draw_faces(cx);
            end();
            check_faces("DXT1 cube", C0);
        } else {
            fail_case("DXT1 cube");
        }
    } else {
        logp("DXT1 cube: not offered, skipped\n");
    }

    /* --- a render-target cube: each face cleared through its own surface --- */
    cr = make_cube(32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL, "render target");
    if (cr) {
        IDirect3DSurface8 *bb = NULL, *z = NULL, *fs = NULL;

        IDirect3DDevice8_GetRenderTarget(dev, &bb);
        IDirect3DDevice8_GetDepthStencilSurface(dev, &z);
        for (f = 0; f < 6; f++) {
            hr = IDirect3DCubeTexture8_GetCubeMapSurface(cr, (D3DCUBEMAP_FACES)f, 0, &fs);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderTarget(dev, fs, NULL);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, RC[f], 1.0f, 0);
            logp("face %d: GetCubeMapSurface / SetRenderTarget / Clear 0x%08lx\n", f, (unsigned long)hr);
            if (fs) {
                IDirect3DSurface8_Release(fs);
                fs = NULL;
            }
        }
        hr = IDirect3DDevice8_SetRenderTarget(dev, bb, z);
        logp("SetRenderTarget (back buffer) 0x%08lx\n", (unsigned long)hr);
        if (bb) IDirect3DSurface8_Release(bb);
        if (z) IDirect3DSurface8_Release(z);
        begin();
        draw_faces(cr);
        end();
        check_faces("render-target cube: faces cleared one by one, sampled", RC);
    } else {
        fail_case("render-target cube");
    }

    /* --- texture generation: a quad facing the viewer has the camera-space
     * normal (0, 0, -1), so it samples -Z --- */
    if (cm) {
        struct nvtx { float x, y, z, nx, ny, nz; } nq[4] = {
            { -1, 1, 0.5f, 0, 0, -1 }, { 0, 1, 0.5f, 0, 0, -1 }, { -1, 0, 0.5f, 0, 0, -1 }, { 0, 0, 0.5f, 0, 0, -1 },
        };
        int xs[2] = { 80, 240 }, ys[2] = { 60, 180 };
        DWORD w[2];

        IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZ | D3DFVF_NORMAL);
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)cm);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
        begin();
        hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, nq, sizeof nq[0]);
        end();
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
        logp("DrawPrimitiveUP (XYZ | NORMAL, TCI_CAMERASPACENORMAL) 0x%08lx\n", (unsigned long)hr);
        w[0] = C0[5];
        w[1] = CLEAR_COLOR;
        check("texture generation: the camera-space normal into the cube", 2, xs, ys, w);
    }

    /* --- a vs 1.1 writing the direction (-1, 0, 0) to oT0: -X --- */
    if (cm) {
        struct svtx { float x, y, z, w, u, v, t; } sq[4] = {
            { -1, 1, 0.5f, 1, -1, 0, 0 }, { 0, 1, 0.5f, 1, -1, 0, 0 }, { -1, 0, 0.5f, 1, -1, 0, 0 }, { 0, 0, 0.5f, 1, -1, 0, 0 },
        };
        int xs[2] = { 80, 240 }, ys[2] = { 60, 180 };
        DWORD w[2];

        hr = IDirect3DDevice8_CreateVertexShader(dev, vs_decl, vs_code, &h_vs, 0);
        logp("CreateVertexShader (oT0 = v7) 0x%08lx handle 0x%08lx\n", (unsigned long)hr, (unsigned long)h_vs);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice8_SetVertexShader(dev, h_vs);
            IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)cm);
            begin();
            hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, sq, sizeof sq[0]);
            end();
            logp("DrawPrimitiveUP (vs 1.1) 0x%08lx\n", (unsigned long)hr);
            w[0] = C0[1];
            w[1] = CLEAR_COLOR;
            check("vs 1.1: the direction through oT0 into the cube", 2, xs, ys, w);
            IDirect3DDevice8_SetVertexShader(dev, FVF_CUBE);
            IDirect3DDevice8_DeleteVertexShader(dev, h_vs);
        } else {
            fail_case("vs 1.1: the direction through oT0 into the cube");
        }
    }

    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    if (cm) IDirect3DCubeTexture8_Release(cm);
    if (cs) IDirect3DCubeTexture8_Release(cs);
    if (cd) IDirect3DCubeTexture8_Release(cd);
    if (cx) IDirect3DCubeTexture8_Release(cx);
    if (cr) IDirect3DCubeTexture8_Release(cr);
    return probe_close(NULL);
}

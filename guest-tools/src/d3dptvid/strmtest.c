/*
 * strmtest.c — more than one vertex stream through XP's own d3d8.dll on our
 * driver's DX8 DDI (doc 15 "More than one vertex stream", protocol v10). A
 * probe (d3d8probe.h): with MaxStreams 1 it says "not offered" (what
 * ddflags=0x200000 turns it back into); otherwise every case below is a
 * check. SHTEST's two-stream cases are the shader half; this is the rest:
 *   three streams — position, colour, texture coordinate, each its own
 *   buffer at its own stride — under a vs 1.1, from a StartVertex;
 *   the fixed function on a three-stream declaration (no function);
 *   streams 0 and 3, nothing bound at 1 and 2;
 *   DrawIndexedPrimitive with a BaseVertexIndex and a MinIndex over three
 *   strides;
 *   the colour stream in a SYSTEMMEM buffer (copied into the record rather
 *   than named in VRAM);
 *   a stream-0 FVF draw with stale streams still bound at 1 and 2.
 * Every buffer starts with decoy vertices — red, and off to the right — so
 * a stream read from the wrong vertex shows as the wrong colour or place.
 *
 *   STRMTEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#define DECOY 3
#define VS11        0xFFFE0101u
#define SH_END      0x0000FFFFu
#define OP_MOV      1u
#define OP_MUL      5u
#define R_TEMP      0u
#define R_INPUT     1u
#define R_RASTOUT   4u
#define R_ATTROUT   5u
#define R_TEXCRDOUT 6u
#define DST(type, n) (0x80000000u | ((type) << 28) | (0xFu << 16) | (n))
#define SRC(type, n) (0x80000000u | ((type) << 28) | (0xE4u << 16) | (n))

/* oPos = v0, oD0 = v5, oT0 = v7 */
static const DWORD vs_code[] = {
    VS11, OP_MOV, DST(R_RASTOUT, 0), SRC(R_INPUT, D3DVSDE_POSITION), OP_MOV, DST(R_ATTROUT, 0), SRC(R_INPUT, D3DVSDE_DIFFUSE),
    OP_MOV, DST(R_TEXCRDOUT, 0), SRC(R_INPUT, D3DVSDE_TEXCOORD0), SH_END,
};
/* oPos = v0, oD0 = v5: the streams-0-and-3 declaration feeds nothing else,
 * and d3d8.dll refuses a function that reads an input its declaration does
 * not (D3DERR_INVALIDCALL at CreateVertexShader) */
static const DWORD vs_code_pc[] = {
    VS11, OP_MOV, DST(R_RASTOUT, 0), SRC(R_INPUT, D3DVSDE_POSITION), OP_MOV, DST(R_ATTROUT, 0), SRC(R_INPUT, D3DVSDE_DIFFUSE), SH_END,
};
/* position (16) | colour (8, of which 4 read) | texture coordinate (12, of which 8 read) */
static const DWORD decl_3s[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT4),
    D3DVSD_STREAM(1), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
    D3DVSD_STREAM(2), D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2), D3DVSD_END()
};
static const DWORD decl_3s_ff[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
    D3DVSD_STREAM(1), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
    D3DVSD_STREAM(2), D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2), D3DVSD_END()
};
static const DWORD decl_gap[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT4),
    D3DVSD_STREAM(3), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()
};

struct p4 { float x, y, z, w; };
struct p3 { float x, y, z; };
struct c8 { DWORD color, pad; };
struct t12 { float u, v, pad; };

/* a quad over the top-left quadrant of clip space (pixels 0..160 x 0..120) as a triangle list */
static const float QX[6] = { -1, 0, -1, -1, 0, 0 }, QY[6] = { 1, 1, 0, 0, 1, 0 };

/* DECOY decoy elements then 6 real ones, each sz bytes, in a buffer of the pool */
static IDirect3DVertexBuffer8 *make_vb(D3DPOOL pool, UINT sz, const void *decoy, const void *real, UINT real_step, const char *what)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    BYTE *p = NULL;
    UINT i;
    HRESULT hr = IDirect3DDevice8_CreateVertexBuffer(dev, (DECOY + 6) * sz, D3DUSAGE_WRITEONLY, 0, pool, &vb);

    logp("CreateVertexBuffer (%s: %u x %u bytes, pool %d) 0x%08lx\n", what, DECOY + 6, sz, (int)pool, (unsigned long)hr);
    if (FAILED(hr) || !vb) return NULL;
    if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(vb, 0, 0, &p, 0))) {
        for (i = 0; i < DECOY + 6; i++) memcpy(p + i * sz, i < DECOY ? decoy : (const BYTE *)real + (i - DECOY) * real_step, sz);
        IDirect3DVertexBuffer8_Unlock(vb);
    }
    return vb;
}

static void check_quad(const char *name, DWORD want)
{
    int xs[2] = { 80, 240 }, ys[2] = { 60, 180 };
    DWORD w[2];
    w[0] = want;
    w[1] = CLEAR_COLOR;
    check(name, 2, xs, ys, w);
}

int main(void)
{
    IDirect3DVertexBuffer8 *vp4 = NULL, *vp3 = NULL, *vc = NULL, *vcs = NULL, *vt = NULL;
    IDirect3DIndexBuffer8 *ib = NULL;
    IDirect3DTexture8 *tex = NULL;
    DWORD h_3s = 0, h_ff = 0, h_gap = 0;
    struct p4 P4[6], p4_decoy = { 0.5f, -0.5f, 0.5f, 1.0f };
    struct p3 P3[6], p3_decoy = { 0.5f, -0.5f, 0.5f };
    struct c8 c_white = { 0xffffffff, 0 }, c_decoy = { 0xffff0000, 0 };
    struct t12 T[6], t_decoy = { 0.75f, 0.5f, 0.0f };
    D3DLOCKED_RECT lr;
    HRESULT hr;
    UINT i, x, y;

    if (!probe_open("strmtest")) return 1;
    logp("strmtest: MaxStreams %lu, MaxStreamStride %lu, vs %lu.%lu\n", (unsigned long)caps.MaxStreams, (unsigned long)caps.MaxStreamStride,
         (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion));
    if (caps.MaxStreams < 4) {
        return probe_close(caps.MaxStreams < 2 ? "MaxStreams 1" : "fewer than four streams");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    for (i = 0; i < 6; i++) {
        P4[i].x = QX[i]; P4[i].y = QY[i]; P4[i].z = 0.5f; P4[i].w = 1.0f;
        P3[i].x = QX[i]; P3[i].y = QY[i]; P3[i].z = 0.5f;
        T[i].u = 0.25f; T[i].v = 0.5f; T[i].pad = 0.0f;         /* the texture's left half: green; a decoy reads the right: blue */
    }
    vp4 = make_vb(D3DPOOL_MANAGED, sizeof(struct p4), &p4_decoy, P4, sizeof P4[0], "position, 16");
    vp3 = make_vb(D3DPOOL_MANAGED, sizeof(struct p3), &p3_decoy, P3, sizeof P3[0], "position, 12");
    vc = make_vb(D3DPOOL_MANAGED, sizeof(struct c8), &c_decoy, &c_white, 0, "colour, 8");
    vcs = make_vb(D3DPOOL_SYSTEMMEM, sizeof(struct c8), &c_decoy, &c_white, 0, "colour, 8, system memory");
    vt = make_vb(D3DPOOL_MANAGED, sizeof(struct t12), &t_decoy, T, sizeof T[0], "texture coordinate, 12");
    hr = IDirect3DDevice8_CreateIndexBuffer(dev, 6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
    if (SUCCEEDED(hr) && ib) {
        BYTE *p;
        static const WORD idx[6] = { 1, 2, 3, 4, 5, 6 };        /* MinIndex 1: vertex 0 of the range is the decoy before */
        if (SUCCEEDED(IDirect3DIndexBuffer8_Lock(ib, 0, 0, &p, 0))) { memcpy(p, idx, sizeof idx); IDirect3DIndexBuffer8_Unlock(ib); }
    }
    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (SUCCEEDED(hr) && tex && SUCCEEDED(IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0))) {
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = x < 32 ? 0xff00ff00 : 0xff0000ff;
        IDirect3DTexture8_UnlockRect(tex, 0);
    }
    if (!vp4 || !vp3 || !vc || !vcs || !vt || !ib || !tex) {
        fail_case("the buffers and the texture");
        return probe_close(NULL);
    }
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_3s, vs_code, &h_3s, 0);
    logp("CreateVertexShader (three streams, vs 1.1) 0x%08lx\n", (unsigned long)hr);
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_3s_ff, NULL, &h_ff, 0);
    logp("CreateVertexShader (three streams, no function) 0x%08lx\n", (unsigned long)hr);
    hr = IDirect3DDevice8_CreateVertexShader(dev, decl_gap, vs_code_pc, &h_gap, 0);
    logp("CreateVertexShader (streams 0 and 3) 0x%08lx\n", (unsigned long)hr);
    /* the colour times the texture: white x green = green; a decoy colour
     * (red) or a decoy coordinate (blue) cannot give green */
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    if (h_3s) {
        IDirect3DDevice8_SetVertexShader(dev, h_3s);
        IDirect3DDevice8_SetStreamSource(dev, 0, vp4, sizeof(struct p4));
        IDirect3DDevice8_SetStreamSource(dev, 1, vc, sizeof(struct c8));
        IDirect3DDevice8_SetStreamSource(dev, 2, vt, sizeof(struct t12));
        begin();
        hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLELIST, DECOY, 2);
        end();
        logp("DrawPrimitive (three streams, StartVertex %d) 0x%08lx\n", DECOY, (unsigned long)hr);
        check_quad("three streams under a vs 1.1, from vertex 3", 0xff00ff00);

        IDirect3DDevice8_SetIndices(dev, ib, DECOY - 1);
        begin();
        hr = IDirect3DDevice8_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 1, 6, 0, 2);
        end();
        IDirect3DDevice8_SetIndices(dev, NULL, 0);
        logp("DrawIndexedPrimitive (three streams, BaseVertexIndex %d, MinIndex 1) 0x%08lx\n", DECOY - 1, (unsigned long)hr);
        check_quad("three streams, indexed: BaseVertexIndex 2, MinIndex 1", 0xff00ff00);

        IDirect3DDevice8_SetStreamSource(dev, 1, vcs, sizeof(struct c8));
        begin();
        hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLELIST, DECOY, 2);
        end();
        logp("DrawPrimitive (the colour stream in system memory) 0x%08lx\n", (unsigned long)hr);
        check_quad("three streams, the colour in system memory", 0xff00ff00);
        IDirect3DDevice8_SetStreamSource(dev, 1, vc, sizeof(struct c8));
    } else {
        fail_case("three streams under a vs 1.1");
    }
    if (h_ff) {
        IDirect3DDevice8_SetVertexShader(dev, h_ff);
        IDirect3DDevice8_SetStreamSource(dev, 0, vp3, sizeof(struct p3));
        begin();
        hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLELIST, DECOY, 2);
        end();
        logp("DrawPrimitive (fixed function, three streams) 0x%08lx\n", (unsigned long)hr);
        check_quad("the fixed function on a three-stream declaration", 0xff00ff00);
    } else {
        fail_case("the fixed function on a three-stream declaration");
    }
    if (h_gap) {
        /* no texture coordinate: the colour alone */
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        IDirect3DDevice8_SetVertexShader(dev, h_gap);
        IDirect3DDevice8_SetStreamSource(dev, 0, vp4, sizeof(struct p4));
        IDirect3DDevice8_SetStreamSource(dev, 1, NULL, 0);
        IDirect3DDevice8_SetStreamSource(dev, 2, NULL, 0);
        IDirect3DDevice8_SetStreamSource(dev, 3, vc, sizeof(struct c8));
        begin();
        hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLELIST, DECOY, 2);
        end();
        logp("DrawPrimitive (streams 0 and 3) 0x%08lx\n", (unsigned long)hr);
        check_quad("streams 0 and 3, nothing at 1 and 2", 0xffffffff);
        IDirect3DDevice8_SetStreamSource(dev, 3, NULL, 0);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    } else {
        fail_case("streams 0 and 3, nothing at 1 and 2");
    }
    {
        /* an FVF draw from user memory with streams 1 and 2 still bound:
         * stream 0 only, the stale ones ignored */
        struct { float x, y, z, rhw; DWORD c; float u, v; } q[4] = {
            { 0, 0, 0.5f, 1, 0xffffffff, 0.25f, 0.5f }, { 160, 0, 0.5f, 1, 0xffffffff, 0.25f, 0.5f },
            { 0, 120, 0.5f, 1, 0xffffffff, 0.25f, 0.5f }, { 160, 120, 0.5f, 1, 0xffffffff, 0.25f, 0.5f },
        };
        IDirect3DDevice8_SetStreamSource(dev, 1, vc, sizeof(struct c8));
        IDirect3DDevice8_SetStreamSource(dev, 2, vt, sizeof(struct t12));
        IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        begin();
        hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        end();
        logp("DrawPrimitiveUP (FVF, streams 1 and 2 still bound) 0x%08lx\n", (unsigned long)hr);
        check_quad("an FVF draw with stale streams bound", 0xff00ff00);
    }

    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (i = 0; i < 4; i++) IDirect3DDevice8_SetStreamSource(dev, i, NULL, 0);
    if (h_3s) IDirect3DDevice8_DeleteVertexShader(dev, h_3s);
    if (h_ff) IDirect3DDevice8_DeleteVertexShader(dev, h_ff);
    if (h_gap) IDirect3DDevice8_DeleteVertexShader(dev, h_gap);
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DTexture8_Release(tex);
    IDirect3DIndexBuffer8_Release(ib);
    IDirect3DVertexBuffer8_Release(vp4);
    IDirect3DVertexBuffer8_Release(vp3);
    IDirect3DVertexBuffer8_Release(vc);
    IDirect3DVertexBuffer8_Release(vcs);
    IDirect3DVertexBuffer8_Release(vt);
    return probe_close(NULL);
}

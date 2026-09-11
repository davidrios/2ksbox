/*
 * bumptest.c — bump mapping through XP's own d3d8.dll on our driver's DX8
 * DDI: environment-mapped bump mapping (a V8U8 bump map at stage 0 under
 * D3DTOP_BUMPENVMAP perturbing stage 1's lookup) and DOT3
 * (D3DTOP_DOTPRODUCT3 of a normal map against the texture factor). A probe
 * (d3d8probe.h): EMBM needs the op in TextureOpCaps *and* V8U8 in the
 * format list, DOT3 the op alone; what is missing is logged and skipped,
 * and with neither the last line says "not offered". Today the caps claim
 * every texture op but list no bump format, so EMBM reads as absent and
 * DOT3 — claimed, never exercised before — is a real check.
 *
 *   BUMPTEST
 *
 * The EMBM draw: stage 1's environment map is red for u < 0.5 and green
 * above, the quad's environment coordinates span u 0.30..0.40 (red), and
 * every bump texel is du = 0.5; a bump matrix of 0.5 moves the lookup by
 * 0.25 (green), a zero matrix not at all (red). The DOT3 draw: a flat
 * normal map (0, 0, 1) against the light (0, 0, 1) is white, against
 * (1, 0, 0) black.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

struct uv2_vtx { float x, y, z, rhw, u0, v0, u1, v1; };
#define FVF_UV2 (D3DFVF_XYZRHW | D3DFVF_TEX2)

static void quad_uv2(float x0, float y0, float sz)
{
    struct uv2_vtx q[4] = {
        { x0, y0, 0.5f, 1.0f, 0.0f, 0.0f, 0.30f, 0.0f }, { x0 + sz, y0, 0.5f, 1.0f, 1.0f, 0.0f, 0.40f, 0.0f },
        { x0, y0 + sz, 0.5f, 1.0f, 0.0f, 1.0f, 0.30f, 1.0f }, { x0 + sz, y0 + sz, 0.5f, 1.0f, 1.0f, 1.0f, 0.40f, 1.0f },
    };
    IDirect3DDevice8_SetVertexShader(dev, FVF_UV2);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static void embm(void)
{
    IDirect3DTexture8 *bump = NULL, *env = NULL;
    D3DLOCKED_RECT lr;
    DWORD passes = 0;
    UINT x, y;
    HRESULT hr;
    int xs[1] = { 70 }, ys[1] = { 70 };
    DWORD w[1];

    hr = IDirect3DDevice8_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_V8U8, D3DPOOL_MANAGED, &bump);
    logp("CreateTexture (V8U8 bump map) 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture8_LockRect(bump, 0, &lr, NULL, 0))) {
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) ((WORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = 0x0040;    /* dv 0, du +0.5 */
        IDirect3DTexture8_UnlockRect(bump, 0);
    }
    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &env);
    logp("CreateTexture (environment map) 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture8_LockRect(env, 0, &lr, NULL, 0))) {
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = x < 32 ? 0xffff0000 : 0xff00ff00;
        IDirect3DTexture8_UnlockRect(env, 0);
    }
    if (!bump || !env) {
        fail_case("EMBM: the textures");
        goto out;
    }
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)bump);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice8_SetTexture(dev, 1, (IDirect3DBaseTexture8 *)env);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_TEXCOORDINDEX, 1);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(dev, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_BUMPENVMAT00, fbits(0.5f));
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_BUMPENVMAT01, fbits(0.0f));
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_BUMPENVMAT10, fbits(0.0f));
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_BUMPENVMAT11, fbits(0.0f));
    hr = IDirect3DDevice8_ValidateDevice(dev, &passes);
    logp("ValidateDevice (BUMPENVMAP at stage 0) 0x%08lx, %lu passes\n", (unsigned long)hr, (unsigned long)passes);
    begin();
    quad_uv2(40.0f, 40.0f, 60.0f);
    end();
    w[0] = 0xff00ff00;
    check("EMBM: a bump matrix of 0.5 moves the lookup onto green", 1, xs, ys, w);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_BUMPENVMAT00, fbits(0.0f));
    begin();
    quad_uv2(40.0f, 40.0f, 60.0f);
    end();
    w[0] = 0xffff0000;
    check("EMBM: a zero bump matrix leaves it on red", 1, xs, ys, w);
out:
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DDevice8_SetTexture(dev, 1, NULL);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (bump) IDirect3DTexture8_Release(bump);
    if (env) IDirect3DTexture8_Release(env);
}

static void dot3(void)
{
    IDirect3DTexture8 *nm = NULL;
    HRESULT hr = IDirect3DDevice8_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &nm);
    int xs[1] = { 70 }, ys[1] = { 70 };
    DWORD w[1];

    logp("CreateTexture (normal map) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !nm) {
        fail_case("DOT3: the normal map");
        return;
    }
    fill_texture32(nm, 0, 0xff8080ff);                  /* the normal (0, 0, 1) */
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)nm);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_TEXTUREFACTOR, 0xff8080ff);   /* the light along the normal */
    begin();
    quad_uv(40.0f, 40.0f, 60.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    end();
    w[0] = 0xffffffff;
    check("DOT3: normal (0, 0, 1) against light (0, 0, 1) is white", 1, xs, ys, w);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_TEXTUREFACTOR, 0xffff8080);   /* the light across it */
    begin();
    quad_uv(40.0f, 40.0f, 60.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    end();
    w[0] = 0xff000000;
    check("DOT3: normal (0, 0, 1) against light (1, 0, 0) is black", 1, xs, ys, w);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DTexture8_Release(nm);
}

int main(void)
{
    int embm_ok, dot3_ok, v8u8;

    if (!probe_open("bumptest")) return 1;
    logp("bumptest: BUMPENVMAP %s, BUMPENVMAPLUMINANCE %s, DOTPRODUCT3 %s\n",
         (caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP) ? "yes" : "no", (caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAPLUMINANCE) ? "yes" : "no",
         (caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3) ? "yes" : "no");
    v8u8 = SUCCEEDED(probe_format("V8U8", D3DRTYPE_TEXTURE, 0, D3DFMT_V8U8));
    probe_format("L6V5U5", D3DRTYPE_TEXTURE, 0, D3DFMT_L6V5U5);
    probe_format("X8L8V8U8", D3DRTYPE_TEXTURE, 0, D3DFMT_X8L8V8U8);
    probe_format("Q8W8V8U8", D3DRTYPE_TEXTURE, 0, D3DFMT_Q8W8V8U8);
    embm_ok = (caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP) && v8u8;
    dot3_ok = (caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3) != 0;
    if (!embm_ok && !dot3_ok) {
        return probe_close("neither BUMPENVMAP with a V8U8 format nor DOTPRODUCT3");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    if (embm_ok) {
        embm();
    } else {
        logp("EMBM: %s, skipped\n", v8u8 ? "no BUMPENVMAP op" : "the op is claimed but no V8U8 format is listed");
    }
    if (dot3_ok) {
        dot3();
    } else {
        logp("DOT3: no DOTPRODUCT3 op, skipped\n");
    }
    return probe_close(NULL);
}

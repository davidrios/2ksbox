/*
 * sprtest.c — point sprites through XP's own d3d8.dll on our driver's DX8
 * DDI. A probe (d3d8probe.h): with MaxPointSize 1 it says "not offered";
 * the caps claim 64, which nothing had exercised before this. The texture
 * is red on its left half and blue on its right; the point is at
 * (100, 100) with the texture coordinate (0.25, 0.5):
 *   sprites on, POINTSIZE 32: a 32-pixel square whose texture runs across
 *   it (red left of centre, blue right of it, nothing 40 pixels up);
 *   POINTSIZE 8: an 8-pixel square (drawn at 98, empty at 110);
 *   a per-vertex size of 24 (D3DFVF_PSIZE) over a POINTSIZE of 4;
 *   sprites off, POINTSIZE 32: the point's own coordinate everywhere (red
 *   right of centre too).
 *
 *   SPRTEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

struct pt_vtx { float x, y, z, rhw, u, v; };
struct pts_vtx { float x, y, z, rhw, size, u, v; };

static void point(void)
{
    struct pt_vtx p = { 100.0f, 100.0f, 0.5f, 1.0f, 0.25f, 0.5f };
    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, &p, sizeof p);
}

int main(void)
{
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    UINT x, y;

    if (!probe_open("sprtest")) return 1;
    logp("sprtest: MaxPointSize %g, per-vertex size (D3DFVFCAPS_PSIZE) %s\n", (double)caps.MaxPointSize,
         (caps.FVFCaps & D3DFVFCAPS_PSIZE) ? "yes" : "no");
    if (caps.MaxPointSize <= 1.0f) {
        return probe_close("MaxPointSize 1");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
    logp("CreateTexture (red | blue) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !t || FAILED(IDirect3DTexture8_LockRect(t, 0, &lr, NULL, 0))) {
        fail_case("the texture");
        return probe_close(NULL);
    }
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = x < 32 ? 0xffff0000 : 0xff0000ff;
    IDirect3DTexture8_UnlockRect(t, 0);
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSCALEENABLE, FALSE);

    {
        int xs[3] = { 92, 108, 100 }, ys[3] = { 100, 100, 60 };
        DWORD w[3] = { 0xffff0000, 0xff0000ff, CLEAR_COLOR };
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSPRITEENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSIZE, fbits(32.0f));
        begin();
        point();
        end();
        check("sprite of 32: the texture across it", 3, xs, ys, w);
    }
    {
        int xs[2] = { 98, 110 }, ys[2] = { 100, 100 };
        DWORD w[2] = { 0xffff0000, CLEAR_COLOR };
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSIZE, fbits(8.0f));
        begin();
        point();
        end();
        check("sprite of 8", 2, xs, ys, w);
    }
    if (caps.FVFCaps & D3DFVFCAPS_PSIZE) {
        struct pts_vtx p = { 100.0f, 100.0f, 0.5f, 1.0f, 24.0f, 0.25f, 0.5f };
        int xs[2] = { 90, 80 }, ys[2] = { 100, 100 };
        DWORD w[2] = { 0xffff0000, CLEAR_COLOR };
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSIZE, fbits(4.0f));
        IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_TEX1);
        begin();
        hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, &p, sizeof p);
        end();
        logp("DrawPrimitiveUP (a per-vertex size) 0x%08lx\n", (unsigned long)hr);
        check("sprite of a per-vertex 24 over a POINTSIZE of 4", 2, xs, ys, w);
    } else {
        logp("per-vertex size: no D3DFVFCAPS_PSIZE, skipped\n");
    }
    {
        int xs[2] = { 92, 108 }, ys[2] = { 100, 100 };
        DWORD w[2] = { 0xffff0000, 0xffff0000 };
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSPRITEENABLE, FALSE);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSIZE, fbits(32.0f));
        begin();
        point();
        end();
        check("sprites off, a point of 32: its own coordinate everywhere", 2, xs, ys, w);
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_POINTSIZE, fbits(1.0f));
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DTexture8_Release(t);
    return probe_close(NULL);
}

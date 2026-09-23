/*
 * mgdtest.c: a managed resource changed between two draws of one frame,
 * through XP's own d3d8.dll on our driver's DX8 DDI. A probe (d3d8probe.h).
 *
 * The runtime keeps a managed vertex buffer's or texture's copy in system
 * memory. Before a draw that reads a changed one it puts a BUFFERBLT or a
 * TEXBLT into the DrawPrimitives2 stream, so one call can carry fill, draw,
 * fill, draw. The driver makes each copy as it walks the stream, and the
 * host reads the memory when it runs the draw. So the driver cuts the
 * stream before a blit that follows a draw (doc 19 §32); without the cut,
 * every draw of the call sees the last fill.
 *
 *   UpdateTexture: a default-pool texture updated from a red system-memory
 *   texture, a quad on the left; updated from a green one, a quad on the
 *   right (the explicit TEXBLT);
 *   a managed vertex buffer: four vertices filled as a red quad on the left
 *   and drawn, then refilled as a green quad on the right and drawn;
 *   a managed texture: red, a quad on the left; green, a quad on the right.
 *
 * Both quads must be there, each in its own colour, and the gap between
 * them the clear colour.
 *
 *   MGDTEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

struct c_vtx { float x, y, z, rhw; DWORD c; };
#define FVF_C (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* the buffer's four vertices as an 80-pixel quad at (x0, 60), one colour */
static HRESULT fill_quad(IDirect3DVertexBuffer8 *vb, float x0, DWORD col)
{
    struct c_vtx *v;
    HRESULT hr = IDirect3DVertexBuffer8_Lock(vb, 0, 4 * sizeof *v, (BYTE **)&v, 0);

    if (FAILED(hr)) return hr;
    v[0] = (struct c_vtx){ x0, 60.0f, 0.5f, 1.0f, col };
    v[1] = (struct c_vtx){ x0 + 80.0f, 60.0f, 0.5f, 1.0f, col };
    v[2] = (struct c_vtx){ x0, 140.0f, 0.5f, 1.0f, col };
    v[3] = (struct c_vtx){ x0 + 80.0f, 140.0f, 0.5f, 1.0f, col };
    return IDirect3DVertexBuffer8_Unlock(vb);
}

int main(void)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DTexture8 *t = NULL;
    int xs[3] = { 80, 220, 150 }, ys[3] = { 100, 100, 100 };
    DWORD w[3] = { 0xffff0000, 0xff00ff00, CLEAR_COLOR };
    HRESULT hr;

    if (!probe_open("mgdtest")) return 1;
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }

    /* UpdateTexture is a TEXBLT the runtime batches like any other command:
     * a default-pool texture updated from a red system-memory copy, drawn,
     * updated from a green one and drawn again, all in one scene */
    {
        IDirect3DTexture8 *dst = NULL, *red = NULL, *green = NULL;
        HRESULT h1 = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst);
        HRESULT h2 = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &red);
        HRESULT h3 = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &green);

        logp("CreateTexture (default, system red, system green) 0x%08lx 0x%08lx 0x%08lx\n",
             (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
        if (SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3) && SUCCEEDED(fill_texture32(red, 0, 0xffff0000)) &&
            SUCCEEDED(fill_texture32(green, 0, 0xff00ff00))) {
            IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)dst);
            begin();
            hr = IDirect3DDevice8_UpdateTexture(dev, (IDirect3DBaseTexture8 *)red, (IDirect3DBaseTexture8 *)dst);
            quad_uv(40.0f, 60.0f, 80.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_UpdateTexture(dev, (IDirect3DBaseTexture8 *)green, (IDirect3DBaseTexture8 *)dst);
            quad_uv(180.0f, 60.0f, 80.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            end();
            logp("UpdateTexture, draw, UpdateTexture, draw 0x%08lx\n", (unsigned long)hr);
            check("UpdateTexture between two draws", 3, xs, ys, w);
            IDirect3DDevice8_SetTexture(dev, 0, NULL);
        } else {
            fail_case("UpdateTexture between two draws");
        }
        if (dst) IDirect3DTexture8_Release(dst);
        if (red) IDirect3DTexture8_Release(red);
        if (green) IDirect3DTexture8_Release(green);
    }

    hr = IDirect3DDevice8_CreateVertexBuffer(dev, 4 * sizeof(struct c_vtx), D3DUSAGE_WRITEONLY, FVF_C,
                                             D3DPOOL_MANAGED, &vb);
    logp("CreateVertexBuffer (managed) 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && vb) {
        IDirect3DDevice8_SetTexture(dev, 0, NULL);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetVertexShader(dev, FVF_C);
        IDirect3DDevice8_SetStreamSource(dev, 0, vb, sizeof(struct c_vtx));
        begin();
        hr = fill_quad(vb, 40.0f, 0xffff0000);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
        if (SUCCEEDED(hr)) hr = fill_quad(vb, 180.0f, 0xff00ff00);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
        end();
        logp("managed vertex buffer: fill, draw, fill, draw 0x%08lx\n", (unsigned long)hr);
        check("a managed vertex buffer refilled between two draws", 3, xs, ys, w);
        IDirect3DDevice8_SetStreamSource(dev, 0, NULL, 0);
        IDirect3DVertexBuffer8_Release(vb);
    } else {
        fail_case("a managed vertex buffer refilled between two draws");
    }

    hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
    logp("CreateTexture (managed) 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && t) {
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
        begin();
        hr = fill_texture32(t, 0, 0xffff0000);
        quad_uv(40.0f, 60.0f, 80.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        if (SUCCEEDED(hr)) hr = fill_texture32(t, 0, 0xff00ff00);
        quad_uv(180.0f, 60.0f, 80.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        end();
        logp("managed texture: fill, draw, fill, draw 0x%08lx\n", (unsigned long)hr);
        check("a managed texture refilled between two draws", 3, xs, ys, w);
        IDirect3DDevice8_SetTexture(dev, 0, NULL);
        IDirect3DTexture8_Release(t);
    } else {
        fail_case("a managed texture refilled between two draws");
    }
    return probe_close(NULL);
}

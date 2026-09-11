/*
 * anistest.c — anisotropic filtering through XP's own d3d8.dll on our
 * driver's DX8 DDI. A probe (d3d8probe.h): with MaxAnisotropy 1 or no
 * D3DPTFILTERCAPS_MINFANISOTROPIC it says "not offered"; once the driver
 * claims it, this is the check.
 *
 *   ANISTEST
 *
 * A floor recedes from the bottom of the window to a narrow far edge, a
 * texture of vertical stripes 32 texels apart on it (every mip level the
 * same stripes, until a level whose texel is a whole period: grey). Near
 * the far edge a pixel covers ~3 texels across and dozens along the floor;
 * trilinear filtering takes its level from the larger of the two and draws
 * grey there, anisotropic filtering from the smaller and keeps the
 * stripes. The case measures the stripes' contrast along one far row under
 * both and passes when the anisotropic one is clearly higher; both
 * numbers are in the log.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#define ROW 104                                 /* the far row measured, x 130..189 */

struct fl_vtx { float x, y, z, rhw, u, v; };

/* the floor: near edge y 239 across the window, far edge y 100 from x 120
 * to 200 at an eighth of the near depth; one texture width across, eight
 * lengths along */
static void floor_quad(void)
{
    const float far_rhw = 1.0f / 16.0f;
    struct fl_vtx q[4] = {
        { 120.0f, 100.0f, 0.5f, far_rhw, 0.0f, 8.0f }, { 200.0f, 100.0f, 0.5f, far_rhw, 1.0f, 8.0f },
        { 0.0f, 239.0f, 0.5f, 1.0f, 0.0f, 0.0f }, { 320.0f, 239.0f, 0.5f, 1.0f, 1.0f, 0.0f },
    };
    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

/* the stripes' contrast along the far row: the green channel's max - min */
static int contrast(void)
{
    DWORD row[60];
    int i, lo = 255, hi = 0;

    readback_row(ROW, 130, 60, row);
    for (i = 0; i < 60; i++) {
        int g = (int)((row[i] >> 8) & 0xff);
        if (g < lo) lo = g;
        if (g > hi) hi = g;
    }
    return hi - lo;
}

int main(void)
{
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT lr;
    DWORD levels, l, x, y, aniso;
    HRESULT hr;
    int c_lin, c_ani;

    if (!probe_open("anistest")) return 1;
    logp("anistest: MaxAnisotropy %lu, D3DPRASTERCAPS_ANISOTROPY %s, MINFANISOTROPIC %s, MAGFANISOTROPIC %s\n",
         (unsigned long)caps.MaxAnisotropy, (caps.RasterCaps & D3DPRASTERCAPS_ANISOTROPY) ? "yes" : "no",
         (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? "yes" : "no",
         (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) ? "yes" : "no");
    if (caps.MaxAnisotropy < 2 || !(caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)) {
        return probe_close("MaxAnisotropy below 2 or no MINFANISOTROPIC filter");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    hr = IDirect3DDevice8_CreateTexture(dev, 256, 256, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &t);
    logp("CreateTexture (stripes, full chain) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !t) {
        fail_case("the texture");
        return probe_close(NULL);
    }
    levels = IDirect3DTexture8_GetLevelCount(t);
    for (l = 0; l < levels; l++) {
        DWORD w = 256 >> l, period = 32 >> l;
        if (FAILED(IDirect3DTexture8_LockRect(t, l, &lr, NULL, 0))) continue;
        for (y = 0; y < w; y++)
            for (x = 0; x < w; x++)
                ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = period < 2 ? 0xff808080 : (x % period) < period / 2 ? 0xff000000 : 0xffffffff;
        IDirect3DTexture8_UnlockRect(t, l);
    }
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);

    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    begin();
    floor_quad();
    end();
    c_lin = contrast();
    present();
    aniso = caps.MaxAnisotropy < 16 ? caps.MaxAnisotropy : 16;
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_ANISOTROPIC);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAXANISOTROPY, aniso);
    begin();
    floor_quad();
    end();
    c_ani = contrast();
    logp("far-row stripe contrast: trilinear %d, anisotropic x%lu %d\n", c_lin, (unsigned long)aniso, c_ani);
    verdict("anisotropic filtering keeps the far stripes trilinear loses", c_ani >= c_lin + 0x40);
    present();
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DTexture8_Release(t);
    return probe_close(NULL);
}

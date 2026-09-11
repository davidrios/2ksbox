/*
 * voltest.c — volume textures through XP's own d3d8.dll on our driver's DX8
 * DDI. A probe (d3d8probe.h): with no D3DPTEXTURECAPS_VOLUMEMAP in the caps
 * it says "not offered" and stops; once the driver claims volumes, every
 * case below is their check:
 *   a MANAGED A8R8G8B8 16x16x4 volume, every slice its own colour, a quad
 *   at each slice (a 3D coordinate whose w picks the slice);
 *   a two-level volume minified onto a small quad (level 1);
 *   one slice rewritten through LockBox (the host must read it again);
 *   a DEFAULT volume filled by UpdateTexture from a SYSTEMMEM one (the DDI's
 *   VOLUMEBLT, which the executor drops today);
 *   a DXT1 volume when the format list offers one.
 * Every HRESULT and pixel is in voltest.log.
 *
 *   VOLTEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#define FVF_UVW (D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0))
struct uvw_vtx { float x, y, z, rhw, u, v, w; };

static const DWORD S0[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00 };     /* level 0's four slices */
static const DWORD S1[2] = { 0xff800080, 0xff008080 };                             /* level 1's two */
static const DWORD SR[4] = { 0xff00ffff, 0xffff00ff, 0xff808080, 0xffffffff };     /* the UpdateTexture source's */

static void quad_uvw(float x0, float y0, float sz, float w)
{
    struct uvw_vtx q[4] = {
        { x0, y0, 0.5f, 1.0f, 0.0f, 0.0f, w }, { x0 + sz, y0, 0.5f, 1.0f, 1.0f, 0.0f, w },
        { x0, y0 + sz, 0.5f, 1.0f, 0.0f, 1.0f, w }, { x0 + sz, y0 + sz, 0.5f, 1.0f, 1.0f, 1.0f, w },
    };
    IDirect3DDevice8_SetVertexShader(dev, FVF_UVW);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static WORD rgb565(DWORD c)
{
    return (WORD)((((c >> 19) & 0x1f) << 11) | (((c >> 10) & 0x3f) << 5) | ((c >> 3) & 0x1f));
}

/* every texel of one slice of one level one colour (32-bit, or DXT1 blocks) */
static HRESULT fill_slice(IDirect3DVolumeTexture8 *t, UINT level, UINT slice, D3DFORMAT fmt, DWORD col)
{
    D3DVOLUME_DESC vd;
    D3DLOCKED_BOX lb;
    D3DBOX box;
    UINT x, y;
    HRESULT hr = IDirect3DVolumeTexture8_GetLevelDesc(t, level, &vd);

    if (FAILED(hr)) return hr;
    box.Left = 0;
    box.Top = 0;
    box.Right = vd.Width;
    box.Bottom = vd.Height;
    box.Front = slice;
    box.Back = slice + 1;
    hr = IDirect3DVolumeTexture8_LockBox(t, level, &lb, &box, 0);
    if (FAILED(hr)) return hr;
    if (fmt == D3DFMT_DXT1) {
        WORD c565 = rgb565(col);
        for (y = 0; y < (vd.Height + 3) / 4; y++) {
            BYTE *row = (BYTE *)lb.pBits + y * lb.RowPitch;
            for (x = 0; x < (vd.Width + 3) / 4; x++) {
                memcpy(row + x * 8, &c565, 2);
                memcpy(row + x * 8 + 2, &c565, 2);
                memset(row + x * 8 + 4, 0, 4);
            }
        }
    } else {
        for (y = 0; y < vd.Height; y++) {
            DWORD *row = (DWORD *)((BYTE *)lb.pBits + y * lb.RowPitch);
            for (x = 0; x < vd.Width; x++) row[x] = col;
        }
    }
    return IDirect3DVolumeTexture8_UnlockBox(t, level);
}

static IDirect3DVolumeTexture8 *make_volume(UINT levels, D3DFORMAT fmt, D3DPOOL pool, const DWORD *l0, const DWORD *l1, const char *what)
{
    IDirect3DVolumeTexture8 *t = NULL;
    HRESULT hr = IDirect3DDevice8_CreateVolumeTexture(dev, 16, 16, 4, levels, 0, fmt, pool, &t);
    UINT d;

    logp("CreateVolumeTexture (%s: 16x16x4, %u levels, format %d, pool %d) 0x%08lx\n", what, levels, (int)fmt, (int)pool, (unsigned long)hr);
    if (FAILED(hr) || !t) return NULL;
    for (d = 0; l0 && d < 4; d++) {
        hr = fill_slice(t, 0, d, fmt, l0[d]);
        if (SUCCEEDED(hr) && l1 && levels > 1 && d < 2) hr = fill_slice(t, 1, d, fmt, l1[d]);
        if (FAILED(hr)) logp("  slice %u: fill 0x%08lx\n", d, (unsigned long)hr);
    }
    return t;
}

/* a quad at each slice's centre, 40 pixels each along y = 20..60, slice d at 30 + 60 d */
static void draw_slices(IDirect3DVolumeTexture8 *t)
{
    UINT d;
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
    for (d = 0; d < 4; d++) {
        quad_uvw(10.0f + 60.0f * d, 20.0f, 40.0f, (d + 0.5f) / 4.0f);
    }
}

static void check_slices(const char *name, const DWORD *want)
{
    int xs[4], ys[4], d;
    for (d = 0; d < 4; d++) {
        xs[d] = 30 + 60 * d;
        ys[d] = 40;
    }
    check(name, 4, xs, ys, want);
}

int main(void)
{
    IDirect3DVolumeTexture8 *vm = NULL, *vs = NULL, *vd = NULL, *vx = NULL;
    HRESULT hr;
    int dxt1;

    if (!probe_open("voltest")) return 1;
    logp("voltest: volume maps %s, mip-mapped %s, power of two only %s, max extent %lu, filter caps 0x%08lx, address caps 0x%08lx\n",
         (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) ? "yes" : "no", (caps.TextureCaps & D3DPTEXTURECAPS_MIPVOLUMEMAP) ? "yes" : "no",
         (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP_POW2) ? "yes" : "no", (unsigned long)caps.MaxVolumeExtent,
         (unsigned long)caps.VolumeTextureFilterCaps, (unsigned long)caps.VolumeTextureAddressCaps);
    probe_format("X8R8G8B8", D3DRTYPE_VOLUMETEXTURE, 0, D3DFMT_X8R8G8B8);
    probe_format("A8R8G8B8", D3DRTYPE_VOLUMETEXTURE, 0, D3DFMT_A8R8G8B8);
    probe_format("R5G6B5", D3DRTYPE_VOLUMETEXTURE, 0, D3DFMT_R5G6B5);
    dxt1 = SUCCEEDED(probe_format("DXT1", D3DRTYPE_VOLUMETEXTURE, 0, D3DFMT_DXT1));
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)) {
        return probe_close("no D3DPTEXTURECAPS_VOLUMEMAP in the caps");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSW, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);

    vm = make_volume(2, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, S0, S1, "managed");
    if (vm) {
        int xs[2] = { 151, 300 }, ys[2] = { 151, 230 };
        DWORD w[4];

        begin();
        draw_slices(vm);
        end();
        check_slices("managed volume: a quad at each slice", S0);
        /* 16 texels over 4 pixels: level 2 asked, level 1 the last; w 0.25 is level 1's slice 0 */
        begin();
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)vm);
        quad_uvw(150.0f, 150.0f, 4.0f, 0.25f);
        end();
        w[0] = S1[0];
        w[1] = CLEAR_COLOR;
        check("managed volume: level 1 where it is minified", 2, xs, ys, w);
        hr = fill_slice(vm, 0, 2, D3DFMT_A8R8G8B8, 0xffffffff);
        logp("slice 2 locked and rewritten white 0x%08lx\n", (unsigned long)hr);
        memcpy(w, S0, sizeof w);
        w[2] = 0xffffffff;
        begin();
        draw_slices(vm);
        end();
        check_slices("managed volume: slice 2 rewritten by LockBox", w);
    } else {
        fail_case("managed volume");
    }

    vs = make_volume(1, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, SR, NULL, "system memory");
    vd = make_volume(1, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL, "default");
    if (vs && vd) {
        hr = IDirect3DDevice8_UpdateTexture(dev, (IDirect3DBaseTexture8 *)vs, (IDirect3DBaseTexture8 *)vd);
        logp("UpdateTexture 0x%08lx\n", (unsigned long)hr);
        begin();
        draw_slices(vd);
        end();
        check_slices("default volume filled by UpdateTexture (VOLUMEBLT)", SR);
    } else {
        fail_case("default volume filled by UpdateTexture (VOLUMEBLT)");
    }

    if (dxt1) {
        vx = make_volume(1, D3DFMT_DXT1, D3DPOOL_MANAGED, S0, NULL, "DXT1");
        if (vx) {
            begin();
            draw_slices(vx);
            end();
            check_slices("DXT1 volume", S0);
        } else {
            fail_case("DXT1 volume");
        }
    } else {
        logp("DXT1 volume: not in the format list, skipped\n");
    }

    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    if (vm) IDirect3DVolumeTexture8_Release(vm);
    if (vs) IDirect3DVolumeTexture8_Release(vs);
    if (vd) IDirect3DVolumeTexture8_Release(vd);
    if (vx) IDirect3DVolumeTexture8_Release(vx);
    return probe_close(NULL);
}

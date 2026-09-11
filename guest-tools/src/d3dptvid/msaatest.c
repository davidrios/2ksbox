/*
 * msaatest.c — multisampling (full-scene antialiasing) through XP's own
 * d3d8.dll on our driver's DX8 DDI. A probe (d3d8probe.h): first the
 * multisample types CheckDeviceMultiSampleType reports for the display
 * format and for D16, windowed and full screen, 2..16 samples each (which
 * is also how the format list's MultiSampleCaps bits read back); with none
 * for a full-screen device the last line says "not offered". Otherwise a
 * full-screen 640 x 480 device with 4 samples (else 2) draws a white
 * triangle with a slanted edge on black, presents (a flip), and reads the
 * screen back from the *front* buffer — a multisampled back buffer can be
 * neither locked nor copied. With D3DRS_MULTISAMPLEANTIALIAS on, pixels
 * along the edge come out between black and white; with it off, none do.
 *
 * Full screen because the driver offers multisampling only there: a
 * windowed Present of a multisampled back buffer is a driver blt, and the
 * driver has no blitter (doc 15, "Multisampling").
 *
 *   MSAATEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#define SW 640
#define SH 480

struct tl_vtx { float x, y, z, rhw; DWORD c; };
#define FVF_TL (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* black, then a white triangle whose long edge runs from (270, 20) to (60, 200); Present */
static void draw(void)
{
    struct tl_vtx v[3] = {
        { 60.0f, 20.0f, 0.5f, 1.0f, 0xffffffff }, { 270.0f, 20.0f, 0.5f, 1.0f, 0xffffffff },
        { 60.0f, 200.0f, 0.5f, 1.0f, 0xffffffff },
    };
    HRESULT hr;

    IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    IDirect3DDevice8_BeginScene(dev);
    IDirect3DDevice8_SetTexture(dev, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetVertexShader(dev, FVF_TL);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof v[0]);
    IDirect3DDevice8_EndScene(dev);
    hr = IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
    logp("  Present 0x%08lx\n", (unsigned long)hr);
    pump();
    Sleep(300);
}

/* the screen from the front buffer: how many pixels on rows 30..190 (every
 * 10th, x 0..319) are neither black nor white (red channel 0x20..0xe0), how
 * many of those rows have one, and whether a point inside the triangle is
 * white and one outside black (so the frame read is the one drawn); -1 when
 * the front buffer could not be read */
static int edge_pixels(int *rows_with, int *rows_read, int *frame_ok)
{
    IDirect3DSurface8 *img = NULL;
    D3DLOCKED_RECT lr;
    int x, y, total = 0;
    HRESULT hr = IDirect3DDevice8_CreateImageSurface(dev, SW, SH, D3DFMT_A8R8G8B8, &img);

    *rows_with = *rows_read = *frame_ok = 0;
    if (FAILED(hr) || !img) {
        logp("  CreateImageSurface (%dx%d A8R8G8B8) 0x%08lx\n", SW, SH, (unsigned long)hr);
        return -1;
    }
    hr = IDirect3DDevice8_GetFrontBuffer(dev, img);
    if (FAILED(hr) || FAILED(IDirect3DSurface8_LockRect(img, &lr, NULL, D3DLOCK_READONLY))) {
        logp("  GetFrontBuffer / LockRect 0x%08lx\n", (unsigned long)hr);
        IDirect3DSurface8_Release(img);
        return -1;
    }
#define PX(xx, yy) (((const DWORD *)((const BYTE *)lr.pBits + (yy) * lr.Pitch))[xx])
    *frame_ok = ((PX(70, 30) >> 16) & 0xff) >= 0xe0 && ((PX(300, 190) >> 16) & 0xff) <= 0x20;
    logp("  inside (70, 30) %06lx, outside (300, 190) %06lx\n", (unsigned long)(PX(70, 30) & 0xffffff),
         (unsigned long)(PX(300, 190) & 0xffffff));
    for (y = 30; y <= 190; y += 10) {
        int n = 0;
        for (x = 0; x < 320; x++) {
            DWORD r = (PX(x, y) >> 16) & 0xff;
            if (r > 0x20 && r < 0xe0) n++;
        }
        (*rows_read)++;
        if (n) (*rows_with)++;
        total += n;
    }
    /* the edge crosses row 110 at x = 165: the pixels around it */
    logp("  row 110, x 160..170:");
    for (x = 160; x <= 170; x++) logp(" %02lx", (unsigned long)((PX(x, 110) >> 16) & 0xff));
    logp("\n");
#undef PX
    IDirect3DSurface8_UnlockRect(img);
    IDirect3DSurface8_Release(img);
    return total;
}

int main(void)
{
    D3DFORMAT fmts[2];
    DWORD win_types = ~0u, full_types = ~0u;
    int f, t, best, n, rows_with, rows_read, frame_ok;
    int hwvp;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    char name[96];

    if (!probe_open("msaatest")) return 1;
    fmts[0] = mode.Format;
    fmts[1] = D3DFMT_D16;
    for (f = 0; f < 2; f++) {
        DWORD w = 0, fs = 0;
        for (t = 2; t <= 16; t++) {
            if (SUCCEEDED(IDirect3D8_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, fmts[f], TRUE, (D3DMULTISAMPLE_TYPE)t)))
                w |= 1u << t;
            if (SUCCEEDED(IDirect3D8_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, fmts[f], FALSE, (D3DMULTISAMPLE_TYPE)t)))
                fs |= 1u << t;
        }
        logp("CheckDeviceMultiSampleType %-8s (format %3d): windowed 0x%05lx, full screen 0x%05lx (bit n = n samples)\n",
             f ? "D16" : "display", (int)fmts[f], (unsigned long)w, (unsigned long)fs);
        win_types &= w;                     /* the back buffer and its depth buffer both */
        full_types &= fs;
    }
    logp("msaatest: windowed 0x%05lx and full screen 0x%05lx for both (the driver offers full screen only)\n",
         (unsigned long)win_types, (unsigned long)full_types);
    best = (full_types & (1u << 4)) ? 4 : (full_types & (1u << 2)) ? 2 : 0;
    if (!best) {
        return probe_close("no full-screen multisample type for the display format and D16");
    }

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = SW;
    pp.BackBufferHeight = SH;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = (D3DMULTISAMPLE_TYPE)best;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;             /* what multisampling requires */
    pp.hDeviceWindow = hwnd;
    pp.Windowed = FALSE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    hwvp = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
    SetForegroundWindow(hwnd);
    pump();
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 hwvp ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    logp("msaatest: CreateDevice (full screen %dx%d, format %d, %d samples) 0x%08lx\n", SW, SH, (int)mode.Format, best, (unsigned long)hr);
    if (FAILED(hr)) {
        dev = NULL;
        fail_case("the full-screen multisampled device");
        return probe_close(NULL);
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

    draw();
    n = edge_pixels(&rows_with, &rows_read, &frame_ok);
    logp("  antialiased: %d edge pixels between black and white, on %d of %d rows\n", n, rows_with, rows_read);
    snprintf(name, sizeof name, "%d samples: the slanted edge blends", best);
    verdict(name, n > 0 && frame_ok && rows_with >= rows_read - 1);

    IDirect3DDevice8_SetRenderState(dev, D3DRS_MULTISAMPLEANTIALIAS, FALSE);
    draw();
    n = edge_pixels(&rows_with, &rows_read, &frame_ok);
    logp("  MULTISAMPLEANTIALIAS off: %d edge pixels between black and white, on %d of %d rows\n", n, rows_with, rows_read);
    verdict("MULTISAMPLEANTIALIAS off: the edge is hard", n == 0 && frame_ok);
    return probe_close(NULL);
}

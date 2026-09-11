/*
 * d3d8probe.h — what the DirectX 8 feature probes of DRIVER\ share
 * (CUBETEST, VOLTEST, FMTTEST, BUMPTEST, SPRTEST, ANISTEST, PATCHTST): a
 * windowed 320x240 device on XP's own d3d8.dll, the log, the back buffer
 * read back at a pixel or along a row, and the case bookkeeping whose last
 * line the harness reads — "<name>: N cases, M failed", with "(not
 * offered: …)" after it when the driver's caps say it has no such feature
 * at all. That is what the probe of a feature not built yet prints; once
 * the driver claims the feature, the same program is its check (doc 15;
 * `tools/xp-driver-test.sh <image> probe <NAME>` / `probes`).
 *
 * One program per feature and one include each: everything is static.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3D8PROBE_H
#define D3D8PROBE_H

/* every probe uses some of these helpers, none uses all of them */
#pragma GCC diagnostic ignored "-Wunused-function"

#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define CLEAR_COLOR 0xff004000

static FILE *logf;
static unsigned cases, failed;
static const char *probe_name;
static HWND hwnd;
static IDirect3D8 *d3d;
static IDirect3DDevice8 *dev;
static D3DCAPS8 caps;
static D3DDISPLAYMODE mode;

static void logp(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static DWORD fbits(float f)
{
    DWORD v;
    memcpy(&v, &f, 4);
    return v;
}

/* <name>.log, the window, Direct3D, the caps and the display mode; FALSE
 * when there is no d3d8 at all */
static BOOL probe_open(const char *name)
{
    WNDCLASSA wc;
    char path[64];

    probe_name = name;
    snprintf(path, sizeof path, "%s.log", name);
    logf = fopen(path, "w");
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = name;
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, name, name, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) {
        logp("%s: Direct3DCreate8 failed\n", name);
        return FALSE;
    }
    IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    logp("%s: display format %lu, devcaps 0x%08lx, texture caps 0x%08lx, texture op caps 0x%08lx, raster caps 0x%08lx, filter caps 0x%08lx\n",
         name, (unsigned long)mode.Format, (unsigned long)caps.DevCaps, (unsigned long)caps.TextureCaps,
         (unsigned long)caps.TextureOpCaps, (unsigned long)caps.RasterCaps, (unsigned long)caps.TextureFilterCaps);
    return TRUE;
}

/* CheckDeviceFormat against the display mode, logged */
static HRESULT probe_format(const char *what, D3DRESOURCETYPE rtype, DWORD usage, D3DFORMAT fmt)
{
    HRESULT hr = IDirect3D8_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, usage, rtype, fmt);
    logp("CheckDeviceFormat %-12s (type %d, usage 0x%lx) 0x%08lx\n", what, (int)rtype, (unsigned long)usage, (unsigned long)hr);
    return hr;
}

/* the device: hardware vertex processing when the caps offer it, no Z, no
 * lighting, no culling, identity transforms, stage 0 the texture alone
 * with point sampling */
static BOOL probe_device(void)
{
    D3DPRESENT_PARAMETERS pp;
    D3DMATRIX ident;
    int hwvp = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
    HRESULT hr;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 hwvp ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    logp("%s: CreateDevice 0x%08lx (%s vertex processing)\n", probe_name, (unsigned long)hr, hwvp ? "hardware" : "software");
    if (FAILED(hr)) {
        dev = NULL;
        return FALSE;
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    memset(&ident, 0, sizeof ident);
    ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
    IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &ident);
    IDirect3DDevice8_SetTransform(dev, D3DTS_VIEW, &ident);
    IDirect3DDevice8_SetTransform(dev, D3DTS_PROJECTION, &ident);
    pump();
    return TRUE;
}

/* the back buffer's row y, pixels x0 .. x0 + n - 1, as 8-bit RGB (before
 * Present); FALSE when it could not be read */
static BOOL readback_row(int y, int x0, int n, DWORD *out)
{
    IDirect3DSurface8 *bb = NULL, *img = NULL;
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    BOOL ok = FALSE;
    HRESULT hr;
    int i;

    for (i = 0; i < n; i++) {
        out[i] = 0xdeadbeef;
    }
    hr = IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        logp("  GetBackBuffer 0x%08lx\n", (unsigned long)hr);
        return FALSE;
    }
    IDirect3DSurface8_GetDesc(bb, &sd);
    hr = IDirect3DDevice8_CreateImageSurface(dev, sd.Width, sd.Height, sd.Format, &img);
    if (SUCCEEDED(hr) && img) {
        hr = IDirect3DDevice8_CopyRects(dev, bb, NULL, 0, img, NULL);
        if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface8_LockRect(img, &lr, NULL, D3DLOCK_READONLY))) {
            const BYTE *row = (const BYTE *)lr.pBits + y * lr.Pitch;
            for (i = 0; i < n; i++) {
                if (sd.Format == D3DFMT_R5G6B5) {
                    WORD v = ((const WORD *)row)[x0 + i];
                    out[i] = ((DWORD)(v >> 11) << 19) | ((DWORD)((v >> 5) & 0x3f) << 10) | ((DWORD)(v & 0x1f) << 3);
                } else {
                    out[i] = ((const DWORD *)row)[x0 + i] & 0xffffff;
                }
            }
            IDirect3DSurface8_UnlockRect(img);
            ok = TRUE;
        } else {
            logp("  CopyRects / LockRect 0x%08lx\n", (unsigned long)hr);
        }
        IDirect3DSurface8_Release(img);
    } else {
        logp("  CreateImageSurface 0x%08lx\n", (unsigned long)hr);
    }
    IDirect3DSurface8_Release(bb);
    return ok;
}

static DWORD readback(int x, int y)
{
    DWORD v;
    readback_row(y, x, 1, &v);
    return v;
}

static int near_(DWORD a, DWORD b, int tol)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

static void present(void)
{
    IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
    pump();
    Sleep(200);
}

/* one case whose verdict the caller worked out (then present() it) */
static void verdict(const char *name, int ok)
{
    cases++;
    if (!ok) failed++;
    logp("%-60s %s\n", name, ok ? "PASS" : "FAIL");
}

/* one case: n pixels of the frame against what they must be (within 12 of
 * 255: a 16-bit desktop rounds), then Present */
static void check(const char *name, int n, const int *xs, const int *ys, const DWORD *want)
{
    DWORD got[16];
    int i, ok = 1;

    for (i = 0; i < n && i < 16; i++) {
        got[i] = readback(xs[i], ys[i]);
        if (!near_(got[i], want[i] & 0xffffff, 12)) ok = 0;
    }
    verdict(name, ok);
    for (i = 0; i < n && i < 16; i++) {
        logp("    (%3d,%3d) %06lx want %06lx\n", xs[i], ys[i], (unsigned long)got[i], (unsigned long)(want[i] & 0xffffff));
    }
    present();
}

static void fail_case(const char *name)
{
    cases++;
    failed++;
    logp("%-60s FAIL (could not be set up)\n", name);
}

static void begin(void)
{
    IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
    IDirect3DDevice8_BeginScene(dev);
}

static void end(void)
{
    IDirect3DDevice8_EndScene(dev);
}

/* a quad of sz pixels at (x0, y0), FVF XYZRHW | TEX1: u0..u1 across, v0..v1 down */
struct uv_vtx { float x, y, z, rhw, u, v; };
#define FVF_UV (D3DFVF_XYZRHW | D3DFVF_TEX1)
static void quad_uv(float x0, float y0, float sz, float u0, float u1, float v0, float v1)
{
    struct uv_vtx q[4] = {
        { x0, y0, 0.5f, 1.0f, u0, v0 }, { x0 + sz, y0, 0.5f, 1.0f, u1, v0 },
        { x0, y0 + sz, 0.5f, 1.0f, u0, v1 }, { x0 + sz, y0 + sz, 0.5f, 1.0f, u1, v1 },
    };
    IDirect3DDevice8_SetVertexShader(dev, FVF_UV);
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

/* every texel of a 32-bit 2D texture's level one colour */
static HRESULT fill_texture32(IDirect3DTexture8 *t, UINT level, DWORD col)
{
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    UINT x, y;
    HRESULT hr = IDirect3DTexture8_GetLevelDesc(t, level, &sd);

    if (FAILED(hr)) return hr;
    hr = IDirect3DTexture8_LockRect(t, level, &lr, NULL, 0);
    if (FAILED(hr)) return hr;
    for (y = 0; y < sd.Height; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (x = 0; x < sd.Width; x++) row[x] = col;
    }
    return IDirect3DTexture8_UnlockRect(t, level);
}

/* the last line and the exit code; not_offered: why the feature is absent
 * from the caps altogether (NULL when it is there) */
static int probe_close(const char *not_offered)
{
    if (dev) {
        IDirect3DDevice8_SetTexture(dev, 0, NULL);
        IDirect3DDevice8_SetTexture(dev, 1, NULL);
        IDirect3DDevice8_Release(dev);
        dev = NULL;
    }
    if (not_offered) {
        logp("%s: %u cases, %u failed (not offered: %s)\n", probe_name, cases, failed, not_offered);
    } else {
        logp("%s: %u cases, %u failed\n", probe_name, cases, failed);
    }
    if (d3d) IDirect3D8_Release(d3d);
    if (logf) fclose(logf);
    return failed ? 1 : 0;
}

#endif /* D3D8PROBE_H */

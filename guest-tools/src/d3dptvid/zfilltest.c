/*
 * zfilltest.c: a Z buffer reset by a DirectDraw depth fill or a Lock, not
 * a Direct3D Clear (doc 19 §34).
 *
 *   ZFILLTEST.EXE
 *
 * Crimson Skies resets its Z buffer every frame and depth-tests
 * GREATEREQUAL against it. With no blitter in the driver, an
 * IDirectDrawSurface7::Blt(DDBLT_DEPTHFILL) is the runtime's own write
 * into VRAM, which the host's depth buffer never sees, and the whole world
 * fails the test. Each case here clears or fills, draws one quad and reads
 * the back buffer through Lock (the driver reads the host's frame back
 * into VRAM):
 *
 *   A  Direct3D clears Z to 1.0, a depth fill of 0, a red quad at z 0.5,
 *      GREATEREQUAL: red, so the host must have taken the fill
 *   B  a depth fill of 0xffff, a green quad at z 0.5: black, so the fill
 *      is not a no-op that A passed by luck
 *   D  Z at 1.0, then Z written to 0 through a Lock (what Crimson Skies
 *      does), a yellow quad: yellow
 *
 * zfilltest.log ends with "zfilltest: N cases, M failed".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>
#include "../guestlog.h"

#define W 640
#define H 480

static FILE *logfile;
static int cases, failed;

static void logp(const char *fmt, ...)
{
    va_list ap;
    char buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (logfile) {
        fputs(buf, logfile);
        fflush(logfile);
    }
}

static void check(int ok, const char *what, unsigned got, unsigned want)
{
    cases++;
    if (!ok) failed++;
    logp("%s: %s (got %06x, want %06x)\n", ok ? "PASS" : "FAIL", what, got, want);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static int enum_hal;
static HRESULT CALLBACK enum_dev(char *desc, char *name, D3DDEVICEDESC7 *d, void *ctx)
{
    (void)desc;
    (void)name;
    (void)ctx;
    if (!memcmp(&d->deviceGUID, &IID_IDirect3DHALDevice, sizeof(GUID))) enum_hal = 1;
    return D3DENUMRET_OK;
}

static DDPIXELFORMAT zfmt;
static HRESULT CALLBACK enum_z(DDPIXELFORMAT *pf, void *ctx)
{
    (void)ctx;
    if (!zfmt.dwSize && pf->dwZBufferBitDepth == 16) zfmt = *pf;
    return D3DENUMRET_OK;
}

static HRESULT fill(LPDIRECTDRAWSURFACE7 z, RECT *r, DWORD value)
{
    DDBLTFX fx;

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillDepth = value;
    return z->lpVtbl->Blt(z, r, NULL, NULL, DDBLT_DEPTHFILL | DDBLT_WAIT, &fx);
}

/* one quad over x0..x1, the whole height, at depth 0.5, GREATEREQUAL, no Z write */
static HRESULT quad(LPDIRECT3DDEVICE7 dev, float x0, float x1, D3DCOLOR c)
{
    D3DTLVERTEX v[4];
    HRESULT hr;
    int i;

    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; i++) {
        v[i].sx = (i & 1) ? x1 : x0;
        v[i].sy = (i & 2) ? (float)H : 0.0f;
        v[i].sz = 0.5f;
        v[i].rhw = 1.0f;
        v[i].color = c;
        v[i].specular = 0xff000000;
    }
    hr = dev->lpVtbl->BeginScene(dev);
    if (FAILED(hr)) return hr;
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZFUNC, D3DCMP_GREATEREQUAL);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    hr = dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, v, 4, 0);
    dev->lpVtbl->EndScene(dev);
    return hr;
}

static unsigned pixel(LPDIRECTDRAWSURFACE7 back, int x, int y)
{
    DDSURFACEDESC2 sd;
    unsigned c = 0xdeadbe;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (SUCCEEDED(back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) {
        c = *(unsigned *)((unsigned char *)sd.lpSurface + y * sd.lPitch + x * 4) & 0xffffff;
        back->lpVtbl->Unlock(back, NULL);
    }
    return c;
}

static int near_(unsigned a, unsigned b)
{
    int i;

    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff);
        if (d < -2 || d > 2) return 0;
    }
    return 1;
}

static void run(HWND hwnd)
{
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL, z = NULL;
    LPDIRECT3D7 d3d = NULL;
    LPDIRECT3DDEVICE7 dev = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    D3DVIEWPORT7 vp;
    unsigned c;
    HRESULT hr;

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx %08lx\n", hr); failed++; return; }
    hr = dd->lpVtbl->QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr)) { logp("IDirect3D7 %08lx\n", hr); failed++; goto out; }
    d3d->lpVtbl->EnumDevices(d3d, enum_dev, NULL);
    if (!enum_hal) { logp("no HAL device\n"); failed++; goto out; }
    dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    hr = dd->lpVtbl->SetDisplayMode(dd, W, H, 32, 0, 0);
    if (FAILED(hr)) { logp("SetDisplayMode %08lx\n", hr); failed++; goto out; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    if (FAILED(hr)) { logp("CreateSurface(flip chain) %08lx\n", hr); failed++; goto out; }
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
    d3d->lpVtbl->EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, enum_z, NULL);
    if (!zfmt.dwSize || !back) { logp("no 16-bit Z format / back buffer\n"); failed++; goto out; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = W;
    sd.dwHeight = H;
    sd.ddpfPixelFormat = zfmt;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &z, NULL);
    if (FAILED(hr)) { logp("CreateSurface(Z) %08lx\n", hr); failed++; goto out; }
    back->lpVtbl->AddAttachedSurface(back, z);
    hr = d3d->lpVtbl->CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev);
    if (FAILED(hr)) { logp("CreateDevice(HAL) %08lx\n", hr); failed++; goto out; }
    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = W; vp.dwHeight = H; vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    dev->lpVtbl->SetViewport(dev, &vp);

    /* A: the host knows Z = 1.0 from a Clear; only the fill can make it 0 */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    hr = fill(z, NULL, 0);
    logp("Blt(DEPTHFILL 0) %08lx\n", hr);
    quad(dev, 0.0f, (float)W, 0xffff0000);
    c = pixel(back, W / 2, H / 2);
    check(near_(c, 0xff0000), "A: depth fill 0, a quad at z 0.5 passes GREATEREQUAL", c, 0xff0000);

    /* B: a fill of the far end makes the same quad fail */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    hr = fill(z, NULL, 0xffff);
    logp("Blt(DEPTHFILL ffff) %08lx\n", hr);
    quad(dev, 0.0f, (float)W, 0xff00ff00);
    c = pixel(back, W / 2, H / 2);
    check(near_(c, 0), "B: depth fill ffff, the quad fails GREATEREQUAL", c, 0);

    /* No case C, a fill of one rectangle. The runtime does that depth fill
     * itself through a Lock of the whole buffer, so the driver sees a Z
     * buffer that is no longer one value, and passing it to the host would
     * take a depth-image upload the executor does not have (doc 19 §34).
     * The left half of such a fill fails here. */

    /* D: what Crimson Skies does. Lock the Z buffer, write 0 everywhere,
     * Unlock; the driver finds one value and makes it the host's */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = z->lpVtbl->Lock(z, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    logp("Lock(Z) %08lx pitch %ld\n", hr, sd.lPitch);
    if (SUCCEEDED(hr)) {
        int y;

        for (y = 0; y < H; y++) memset((unsigned char *)sd.lpSurface + y * sd.lPitch, 0, W * 2);
        z->lpVtbl->Unlock(z, NULL);
    }
    quad(dev, 0.0f, (float)W, 0xffffff00);
    c = pixel(back, W / 2, H / 2);
    check(near_(c, 0xffff00), "D: Z written to 0 through a Lock, the quad passes", c, 0xffff00);

out:
    if (dev) dev->lpVtbl->Release(dev);
    if (z) z->lpVtbl->Release(z);
    if (prim) prim->lpVtbl->Release(prim);
    if (d3d) d3d->lpVtbl->Release(d3d);
    if (dd) {
        dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
}

int main(void)
{
    WNDCLASSA wc;
    HWND hwnd;

    logfile = guest_log_open("ZFILLTEST.LOG", "w");
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "zfilltest";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "zfilltest", "zfilltest", WS_POPUP, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    ShowCursor(FALSE);
    run(hwnd);
    ShowCursor(TRUE);
    DestroyWindow(hwnd);
    logp("zfilltest: %d cases, %d failed\n", cases, failed);
    if (logfile) fclose(logfile);
    return failed != 0;
}

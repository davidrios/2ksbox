/*
 * D9CTEST.EXE: the resources d3d9.dll creates on the driver, one line per
 * call with its HRESULT (track M16, step 5). Written for the cases Wine's
 * device.c and visual.c found failing on Win98 and passing on XP, so that
 * one short process shows them with the driver's log whole:
 *   textures of a non-power-of-two size, mipmapped, in every pool;
 *   cube textures of edge 3 and 4, volume textures (Wine's 2x4x8 of 4
 *   levels in system memory among them);
 *   a render target cleared and read back through GetRenderTargetData in
 *   A8R8G8B8, A16B16G16R16F and A32B32G32R32F (the first texel and the
 *   pitch, against the clear colour);
 *   a full-screen device at 640x480 (last, since it changes the mode).
 * `-readback` runs the readback part alone (the driver logs only the first
 * few dozen surfaces of a process). `-modechange` replays Wine's
 * test_wndproc's first case: a device window apart from the focus window,
 * a mode change under the full-screen device, focus to the desktop and
 * back (messages pumped, as the test's flush_events), Reset, then new
 * full-screen devices; `-modechange -nolock` with Win98's foreground lock
 * off first.
 * Log: C:\2KSBOX\D9CTEST.LOG. Runs on XP too, where every line is the
 * answer to compare with.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d9.h>
#include "../guestlog.h"

static FILE *out;

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

static const struct {
    D3DPOOL pool;
    const char *name;
} pools[] = {
    { D3DPOOL_DEFAULT, "DEFAULT" },
    { D3DPOOL_MANAGED, "MANAGED" },
    { D3DPOOL_SYSTEMMEM, "SYSTEMMEM" },
    { D3DPOOL_SCRATCH, "SCRATCH" },
};

/* A full-screen device of 640x480 on the focus window */
static HRESULT fullscreen2(IDirect3D9 *d3d, HWND focus, HWND wnd, IDirect3DDevice9 **dev);

static HRESULT fullscreen(IDirect3D9 *d3d, HWND wnd, IDirect3DDevice9 **dev)
{
    return fullscreen2(d3d, wnd, wnd, dev);
}

/* the same with a device window other than the focus window, as Wine's tests */
static HRESULT fullscreen2(IDirect3D9 *d3d, HWND focus, HWND wnd, IDirect3DDevice9 **dev)
{
    D3DPRESENT_PARAMETERS pp;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    return IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, focus, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, dev);
}

/* Wine's flush_events: messages for 200 ms (d3d9 learns of activation
 * through the focus window's own) */
static void pump(void)
{
    DWORD end = GetTickCount() + 200;
    MSG msg;

    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}

static HRESULT reset640(IDirect3DDevice9 *dev, HWND wnd)
{
    D3DPRESENT_PARAMETERS pp;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    return IDirect3DDevice9_Reset(dev, &pp);
}

/* Wine's test_wndproc in short: the mode changed under a full-screen
 * device, focus dropped, the device released; then full-screen devices
 * again (on Win98 every one after it failed DDERR_HWNDALREADYSET) */
static void modechange(IDirect3D9 *d3d, HWND wnd)
{
    IDirect3DDevice9 *dev = NULL;
    HWND devwnd;
    DEVMODEA dm;
    HRESULT hr;
    LONG ret;
    unsigned i;

    devwnd = CreateWindowA("D9CTEST", "D9CTEST device", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL,
                           NULL, NULL);
    hr = fullscreen2(d3d, wnd, devwnd, &dev);
    say("modechange: CreateDevice fullscreen, device window apart from the focus window: 0x%08lx\n", hr);
    if (FAILED(hr))
        return;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = 1024;
    dm.dmPelsHeight = 768;
    pump();
    ret = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
    pump();
    say("modechange: ChangeDisplaySettings 1024x768: %ld\n", ret);
    say("modechange: TestCooperativeLevel: 0x%08lx\n", IDirect3DDevice9_TestCooperativeLevel(dev));
    SetForegroundWindow(GetDesktopWindow());
    pump();
    say("modechange: after focus loss: 0x%08lx\n", IDirect3DDevice9_TestCooperativeLevel(dev));
    /* the test's way back: minimize, restore, SetForegroundWindow twice */
    ShowWindow(wnd, SW_SHOWMINNOACTIVE);
    pump();
    ShowWindow(wnd, SW_RESTORE);
    SetForegroundWindow(wnd);
    pump();
    SetForegroundWindow(wnd);
    pump();
    say("modechange: foreground %s, after restore: 0x%08lx\n", GetForegroundWindow() == wnd ? "ours" : "not ours",
        IDirect3DDevice9_TestCooperativeLevel(dev));
    say("modechange: Reset: 0x%08lx\n", reset640(dev, devwnd));
    say("modechange: Release: %lu\n", IDirect3DDevice9_Release(dev));
    DestroyWindow(devwnd);
    dev = NULL;
    ret = ChangeDisplaySettingsA(NULL, 0);
    say("modechange: ChangeDisplaySettings back: %ld\n", ret);
    for (i = 0; i < 2; i++) {
        HWND w2 = CreateWindowA("D9CTEST", "D9CTEST 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, NULL,
                                NULL, NULL, NULL);

        SetForegroundWindow(wnd);
        hr = fullscreen2(d3d, wnd, w2, &dev);
        say("modechange: CreateDevice fullscreen, another device window: 0x%08lx\n", hr);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice9_Release(dev);
            dev = NULL;
        }
        /* Wine's test_wndproc makes a new focus window per case */
        SetForegroundWindow(w2);
        hr = fullscreen2(d3d, w2, w2, &dev);
        say("modechange: CreateDevice fullscreen, another focus window: 0x%08lx\n", hr);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice9_Release(dev);
            dev = NULL;
        }
        DestroyWindow(w2);
        hr = fullscreen(d3d, wnd, &dev);
        say("modechange: CreateDevice fullscreen again: 0x%08lx\n", hr);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice9_Release(dev);
            dev = NULL;
        }
    }
}

static void textures(IDirect3DDevice9 *dev)
{
    static const UINT sizes[] = { 10, 16 };
    IDirect3DTexture9 *t;
    IDirect3DCubeTexture9 *c;
    IDirect3DVolumeTexture9 *v;
    unsigned i, s, l;
    HRESULT hr;

    for (i = 0; i < 4; i++) {
        for (s = 0; s < 2; s++)
            for (l = 0; l <= 2; l++) {
                hr = IDirect3DDevice9_CreateTexture(dev, sizes[s], sizes[s], l, 0, D3DFMT_X8R8G8B8, pools[i].pool, &t,
                                                    NULL);
                say("CreateTexture %ux%u levels %u %s: 0x%08lx\n", sizes[s], sizes[s], l, pools[i].name, hr);
                if (SUCCEEDED(hr))
                    IDirect3DTexture9_Release(t);
            }
        for (s = 3; s <= 4; s++)
            for (l = 0; l <= 1; l++) {
                hr = IDirect3DDevice9_CreateCubeTexture(dev, s, l, 0, D3DFMT_X8R8G8B8, pools[i].pool, &c, NULL);
                say("CreateCubeTexture edge %u levels %u %s: 0x%08lx\n", s, l, pools[i].name, hr);
                if (SUCCEEDED(hr))
                    IDirect3DCubeTexture9_Release(c);
            }
        hr = IDirect3DDevice9_CreateVolumeTexture(dev, 2, 4, 8, 4, 0, D3DFMT_X8R8G8B8, pools[i].pool, &v, NULL);
        say("CreateVolumeTexture 2x4x8 levels 4 %s: 0x%08lx\n", pools[i].name, hr);
        if (SUCCEEDED(hr))
            IDirect3DVolumeTexture9_Release(v);
        hr = IDirect3DDevice9_CreateVolumeTexture(dev, 8, 8, 8, 1, 0, D3DFMT_X8R8G8B8, pools[i].pool, &v, NULL);
        say("CreateVolumeTexture 8x8x8 levels 1 %s: 0x%08lx\n", pools[i].name, hr);
        if (SUCCEEDED(hr))
            IDirect3DVolumeTexture9_Release(v);
        hr = IDirect3DDevice9_CreateVolumeTexture(dev, 8, 8, 8, 0, 0, D3DFMT_A8R8G8B8, pools[i].pool, &v, NULL);
        say("CreateVolumeTexture 8x8x8 A8R8G8B8 levels 0 %s: 0x%08lx\n", pools[i].name, hr);
        if (SUCCEEDED(hr))
            IDirect3DVolumeTexture9_Release(v);
    }
}

static void readback(IDirect3DDevice9 *dev, D3DFORMAT fmt, const char *name, unsigned bytes)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL, *old = NULL;
    D3DLOCKED_RECT lr;
    const unsigned char *p;
    HRESULT hr;
    unsigned i;

    hr = IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    say("CreateRenderTarget 64x64 %s: 0x%08lx\n", name, hr);
    if (FAILED(hr))
        return;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, fmt, D3DPOOL_SYSTEMMEM, &sys, NULL);
    say("CreateOffscreenPlainSurface 64x64 %s SYSTEMMEM: 0x%08lx\n", name, hr);
    if (FAILED(hr))
        goto done;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &old);
    hr = IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    say("SetRenderTarget: 0x%08lx\n", hr);
    hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff4080c0, 1.0f, 0);
    say("Clear 0xff4080c0: 0x%08lx\n", hr);
    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    say("GetRenderTargetData: 0x%08lx\n", hr);
    hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    say("LockRect: 0x%08lx pitch %ld\n", hr, SUCCEEDED(hr) ? (long)lr.Pitch : 0L);
    if (SUCCEEDED(hr)) {
        p = lr.pBits;
        say("  texel (0,0):");
        for (i = 0; i < bytes; i++)
            say(" %02x", p[i]);
        p = (const unsigned char *)lr.pBits + 33 * lr.Pitch + 17 * bytes;
        say("\n  texel (17,33):");
        for (i = 0; i < bytes; i++)
            say(" %02x", p[i]);
        say("\n");
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (old) {
        IDirect3DDevice9_SetRenderTarget(dev, 0, old);
        IDirect3DSurface9_Release(old);
    }
done:
    if (sys)
        IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(rt);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *dev = NULL;
    WNDCLASSA wc = { 0 };
    HMODULE lib;
    HRESULT hr;
    HWND wnd;
    IDirect3D9 *d3d;
    unsigned i;

    out = guest_log_open("D9CTEST.LOG", "w");
    if (!out)
        out = stdout;
    lib = LoadLibraryA("d3d9.dll");
    create = lib ? (void *)GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) {
        say("D9CTEST: no Direct3D 9\n");
        return 1;
    }
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "D9CTEST";
    RegisterClassA(&wc);
    wnd = CreateWindowA("D9CTEST", "D9CTEST", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, NULL,
                        NULL);

    if (argc >= 3 && !strcmp(argv[2], "-nolock")) {
        /* Win98's foreground lock off, as for a user at the machine */
        BOOL r = SystemParametersInfoA(0x2001 /* SPI_SETFOREGROUNDLOCKTIMEOUT */, 0, (PVOID)0, SPIF_SENDCHANGE);
        say("foreground lock timeout 0: %s\n", r ? "set" : "refused");
    }
    if (argc >= 2 && !strcmp(argv[1], "-modechange")) {
        modechange(d3d, wnd);
        IDirect3D9_Release(d3d);
        DestroyWindow(wnd);
        say("D9CTEST: done\n");
        return 0;
    }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    say("CreateDevice windowed: 0x%08lx\n", hr);
    if (SUCCEEDED(hr)) {
        if (argc < 2 || strcmp(argv[1], "-readback"))
            textures(dev);
        readback(dev, D3DFMT_A8R8G8B8, "A8R8G8B8", 4);
        readback(dev, D3DFMT_A16B16G16R16F, "A16B16G16R16F", 8);
        readback(dev, D3DFMT_A32B32G32R32F, "A32B32G32R32F", 16);
        IDirect3DDevice9_Release(dev);
        dev = NULL;
    }

    for (i = 0; i < 3 && (argc < 2 || strcmp(argv[1], "-readback")); i++) {
        memset(&pp, 0, sizeof pp);
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.BackBufferFormat = i == 1 ? D3DFMT_X8R8G8B8 : D3DFMT_A8R8G8B8;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = wnd;
        pp.Windowed = FALSE;
        pp.EnableAutoDepthStencil = i != 2;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        say("CreateDevice fullscreen 640x480 %s %s: 0x%08lx\n", i == 1 ? "X8R8G8B8" : "A8R8G8B8",
            i == 2 ? "no depth" : "D24S8", hr);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice9_Release(dev);
            dev = NULL;
        }
    }
    IDirect3D9_Release(d3d);
    DestroyWindow(wnd);
    say("D9CTEST: done\n");
    return 0;
}

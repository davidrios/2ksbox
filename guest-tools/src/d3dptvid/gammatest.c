/*
 * gammatest.c — a gamma ramp through XP's own d3d8.dll on our driver: the
 * DX8 face claims D3DCAPS2_FULLSCREENGAMMA, and SetGammaRamp reaches the
 * adapter's GAMMA block through GDI's DrvIcmSetDeviceGammaRamp (register
 * set v5). A probe over d3d8probe.h: without the cap the last line says
 * "not offered". Otherwise a full-screen 640 x 480 device (32 bpp when it
 * can be had: the mode the adapter otherwise shows straight from VRAM)
 * clears to mid grey and holds two ramps, 20 s each: blue at three
 * quarters (red and green unchanged: XP refuses a ramp too far from a
 * straight line), then the identity.
 *
 * The probe cannot see the result itself: a ramp is applied where the
 * adapter makes the picture, as a RAMDAC is, not in VRAM, so GetFrontBuffer
 * reads the pixels as drawn. `tools/xp-driver-test.sh <image> gamma` takes a
 * screendump while each ramp is held (waiting for the adapter's own `gamma
 * ramp on` / `off` lines) and checks the centre pixel: 80 80 60, then
 * 80 80 80. What the probe checks is the runtime's side: GetGammaRamp
 * returns the ramp set.
 *
 *   GAMMATEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

#ifndef D3DSGR_NO_CALIBRATION
#define D3DSGR_NO_CALIBRATION 0x0     /* d3d8types.h; mingw's headers lack it (D3DSGR_CALIBRATE is 1) */
#endif

#define SW 640
#define SH 480

static void grey_frame(void)
{
    IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff808080, 1.0f, 0);
    IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
    pump();
}

/* keep the frame up for the harness's screendump */
static void hold(int secs)
{
    int i;
    for (i = 0; i < secs * 4; i++) {
        pump();
        Sleep(250);
    }
}

int main(void)
{
    D3DPRESENT_PARAMETERS pp;
    D3DGAMMARAMP ramp, back;
    D3DFORMAT fmt;
    HRESULT hr;
    int i;

    if (!probe_open("gammatest")) return 1;
    logp("gammatest: Caps2 0x%08lx (FULLSCREENGAMMA %s, CANCALIBRATEGAMMA %s)\n", (unsigned long)caps.Caps2,
         (caps.Caps2 & D3DCAPS2_FULLSCREENGAMMA) ? "yes" : "no", (caps.Caps2 & D3DCAPS2_CANCALIBRATEGAMMA) ? "yes" : "no");
    if (!(caps.Caps2 & D3DCAPS2_FULLSCREENGAMMA)) {
        return probe_close("no D3DCAPS2_FULLSCREENGAMMA");
    }
    fmt = SUCCEEDED(IDirect3D8_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, FALSE))
              ? D3DFMT_X8R8G8B8 : mode.Format;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = SW;
    pp.BackBufferHeight = SH;
    pp.BackBufferFormat = fmt;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = FALSE;
    SetForegroundWindow(hwnd);
    pump();
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    logp("gammatest: CreateDevice (full screen %dx%d, format %d) 0x%08lx\n", SW, SH, (int)fmt, (unsigned long)hr);
    if (FAILED(hr)) {
        dev = NULL;
        fail_case("the full-screen device");
        return probe_close(NULL);
    }
    grey_frame();

    for (i = 0; i < 256; i++) {
        ramp.red[i] = ramp.green[i] = (WORD)(i * 257);
        ramp.blue[i] = (WORD)(i * 257 * 3 / 4);
    }
    IDirect3DDevice8_SetGammaRamp(dev, D3DSGR_NO_CALIBRATION, &ramp);
    IDirect3DDevice8_GetGammaRamp(dev, &back);
    verdict("GetGammaRamp returns the ramp set", memcmp(&ramp, &back, sizeof ramp) == 0);
    grey_frame();
    logp("gammatest: holding blue at 3/4 (128 -> %04x %04x %04x) for 20 s\n", ramp.red[128], ramp.green[128], ramp.blue[128]);
    hold(20);

    for (i = 0; i < 256; i++) {
        ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)(i * 257);
    }
    IDirect3DDevice8_SetGammaRamp(dev, D3DSGR_NO_CALIBRATION, &ramp);
    grey_frame();
    logp("gammatest: holding the identity ramp for 20 s\n");
    hold(20);
    return probe_close(NULL);
}

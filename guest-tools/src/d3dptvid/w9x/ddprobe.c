/*
 * ddprobe.c — the smallest thing that makes DirectDraw initialise, so
 * that the 9x driver's DirectDraw half is exercised at all (doc 19 §2).
 *
 * The `DCICOMMAND` escapes a 16-bit display driver publishes its HAL
 * through — and therefore the ring-3 HAL DLL itself — are reached only
 * when something calls `DirectDrawCreate`. A Win98 desktop on its own
 * never does, so `tools/win98-driver-test.sh` stages this and names it
 * in WIN.INI's `run=`, which starts it once the shell is up.
 *
 * It draws nothing and shows nothing. The evidence it produces is in the
 * QEMU log, written by the driver's two halves through the adapter's
 * DEBUG register while this runs: `d3dpt9dd:` lines from the 16-bit side
 * as it answers the escapes, and `d3dpthal:` lines from the DLL once
 * DirectDraw has loaded it into *this* process. Its own findings go to
 * C:\DDPROBE.LOG for the harness to read back with mtools.
 *
 * What it asks, and why each question is here: caps say whether the
 * runtime associated our HAL with this device at all (a HAL it did not
 * take reads back as DDCAPS_NOHARDWARE and nothing else); the primary
 * surface says whether the runtime believes the display is ours (a
 * locked primary whose address is inside the adapter's VRAM is the
 * proof); and an explicitly video-memory offscreen surface says whether
 * the heap in DDHALINFO is being allocated from — it is the one request
 * the HEL cannot satisfy at all, so its HRESULT is a yes/no answer with
 * nothing in between.
 *
 * Build: guest-tools/build-driver9x.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

static FILE *log_file;

static void logf_(const char *fmt, ...)
{
    va_list ap;

    if (!log_file) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);           /* the guest may be powered off at any moment */
}

/* Everything a surface can say about where it lives. The pointer matters
 * as much as the caps: the runtime will happily report VIDEOMEMORY for a
 * surface it put in its own emulated heap, and the address is what tells
 * the two apart — a real one is inside the adapter's frame buffer. */
static void describe(const char *what, LPDIRECTDRAWSURFACE s)
{
    DDSURFACEDESC sd;
    HRESULT hr;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_GetSurfaceDesc(s, &sd);
    logf_("  %s GetSurfaceDesc -> 0x%08lx", what, (unsigned long)hr);
    logf_("  %s %lux%lu pitch %ld caps 0x%08lx", what,
          (unsigned long)sd.dwWidth, (unsigned long)sd.dwHeight,
          (long)sd.lPitch, (unsigned long)sd.ddsCaps.dwCaps);

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_Lock(s, NULL, &sd, DDLOCK_WAIT, NULL);
    logf_("  %s Lock -> 0x%08lx  lpSurface %p  pitch %ld", what,
          (unsigned long)hr, sd.lpSurface, (long)sd.lPitch);
    if (SUCCEEDED(hr)) {
        IDirectDrawSurface_Unlock(s, NULL);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE surf = NULL;
    DDSURFACEDESC sd;
    DDCAPS hal, hel;
    HRESULT hr;

    (void)inst; (void)prev; (void)cmd; (void)show;
    log_file = fopen("C:\\DDPROBE.LOG", "w");
    logf_("ddprobe: start");

    hr = DirectDrawCreate(NULL, &dd, NULL);
    logf_("DirectDrawCreate -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr) || !dd) {
        logf_("ddprobe: no DirectDraw object, nothing more to ask");
        if (log_file) fclose(log_file);
        return 1;
    }

    /* The question this whole probe exists to answer: did the runtime
     * take our HAL, or is it doing everything in its own HEL? A HAL with
     * no callbacks still reports caps here — what says the 32-bit DLL is
     * in play is dwCaps having the bits its callbacks imply, and what
     * says the runtime never took the HAL at all is DDCAPS_NOHARDWARE
     * (0x02000000) standing alone. */
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    hr = IDirectDraw_GetCaps(dd, &hal, &hel);
    logf_("GetCaps -> 0x%08lx", (unsigned long)hr);
    logf_("  HAL dwCaps      0x%08lx", (unsigned long)hal.dwCaps);
    logf_("  HAL dwCaps2     0x%08lx", (unsigned long)hal.dwCaps2);
    logf_("  HAL vidmem      %lu total, %lu free",
          (unsigned long)hal.dwVidMemTotal, (unsigned long)hal.dwVidMemFree);
    logf_("  HEL dwCaps      0x%08lx", (unsigned long)hel.dwCaps);

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = IDirectDraw_GetDisplayMode(dd, &sd);
    logf_("GetDisplayMode -> 0x%08lx  %lux%lux%lu pitch %ld", (unsigned long)hr,
          (unsigned long)sd.dwWidth, (unsigned long)sd.dwHeight,
          (unsigned long)sd.ddpfPixelFormat.dwRGBBitCount, (long)sd.lPitch);

    /* The one callback the DLL publishes so far. A HAL that is really
     * being called returns from this after a frame rather than at once,
     * and the adapter's frame counter is what it waited on. Asked twice,
     * before and after the cooperative level is set: the runtime answers
     * some calls out of its own state before a driver ever sees them,
     * and which side of that line this one falls on is the question. */
    hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logf_("WaitForVerticalBlank (no coop level) -> 0x%08lx", (unsigned long)hr);

    /* DDSCL_NORMAL on the desktop window: enough to own a primary
     * surface, and it changes no mode and shows no window, which is what
     * a probe that runs from WIN.INI's `run=` has to promise. */
    hr = IDirectDraw_SetCooperativeLevel(dd, GetDesktopWindow(), DDSCL_NORMAL);
    logf_("SetCooperativeLevel -> 0x%08lx", (unsigned long)hr);

    hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logf_("WaitForVerticalBlank -> 0x%08lx", (unsigned long)hr);
    hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKEND, NULL);
    logf_("WaitForVerticalBlank (end) -> 0x%08lx", (unsigned long)hr);

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw_CreateSurface(dd, &sd, &surf, NULL);
    logf_("CreateSurface(primary) -> 0x%08lx", (unsigned long)hr);
    if (SUCCEEDED(hr) && surf) {
        describe("primary", surf);
        IDirectDrawSurface_Release(surf);
        surf = NULL;
    }

    /* The question the HEL cannot answer yes to. DDSCAPS_VIDEOMEMORY
     * without DDSCAPS_SYSTEMMEMORY means "out of the driver's heap or
     * not at all", so DDERR_OUTOFVIDEOMEMORY here is the runtime saying
     * it has no heap of ours to allocate from. */
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.dwWidth = 64;
    sd.dwHeight = 64;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    hr = IDirectDraw_CreateSurface(dd, &sd, &surf, NULL);
    logf_("CreateSurface(64x64 vidmem) -> 0x%08lx", (unsigned long)hr);
    if (SUCCEEDED(hr) && surf) {
        describe("offscreen", surf);
        IDirectDrawSurface_Release(surf);
        surf = NULL;
    }

    IDirectDraw_Release(dd);
    logf_("ddprobe: done");
    if (log_file) fclose(log_file);
    return 0;
}

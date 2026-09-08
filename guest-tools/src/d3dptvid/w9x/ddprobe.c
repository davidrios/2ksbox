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

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    LPDIRECTDRAW dd = NULL;
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
     * in play is dwCaps having the bits its callbacks imply. */
    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    hr = IDirectDraw_GetCaps(dd, &hal, &hel);
    logf_("GetCaps -> 0x%08lx", (unsigned long)hr);
    logf_("  HAL dwCaps      0x%08lx", (unsigned long)hal.dwCaps);
    logf_("  HAL dwCaps2     0x%08lx", (unsigned long)hal.dwCaps2);
    logf_("  HAL vidmem      %lu total, %lu free",
          (unsigned long)hal.dwVidMemTotal, (unsigned long)hal.dwVidMemFree);
    logf_("  HEL dwCaps      0x%08lx", (unsigned long)hel.dwCaps);

    /* The one callback the DLL publishes so far. A HAL that is really
     * being called returns from this after a frame rather than at once,
     * and the adapter's frame counter is what it waited on. */
    hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logf_("WaitForVerticalBlank -> 0x%08lx", (unsigned long)hr);
    hr = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logf_("WaitForVerticalBlank -> 0x%08lx", (unsigned long)hr);

    IDirectDraw_Release(dd);
    logf_("ddprobe: done");
    if (log_file) fclose(log_file);
    return 0;
}

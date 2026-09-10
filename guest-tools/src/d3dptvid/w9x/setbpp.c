/*
 * setbpp.c — change the desktop's colour depth (and optionally its size)
 * from a batch file, so a headless run can bisect on it.
 *
 * There is no other lever for this on 9x from outside the guest. The
 * display driver's INF resets the mode only when PnP actually reinstalls
 * the device, and on a machine already bound to the driver a reinstall
 * changes nothing — `CURRENT` survives and the desktop comes back at the
 * depth it was (doc 19 §27, measured). The registry lives in SYSTEM.DAT,
 * which nothing here can edit offline. `ChangeDisplaySettings` from inside
 * the guest is what works, and this is the smallest thing that calls it.
 *
 *   SETBPP 32          — 32 bpp, current resolution
 *   SETBPP 16 800 600  — 16 bpp at 800x600
 *
 * It writes C:\SETBPP.LOG rather than to the console, because the harness
 * starts it from WIN.INI's `run=` where nothing is watching stdout, and
 * prints the depth before and after so a run that silently did nothing is
 * distinguishable from one that worked.
 *
 * Build: guest-tools/build-driver9x.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int screen_bpp(void)
{
    HDC dc = GetDC(NULL);
    int b = dc ? GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES) : 0;

    if (dc) ReleaseDC(NULL, dc);
    return b;
}

int main(int argc, char **argv)
{
    FILE *f = fopen("C:\\SETBPP.LOG", "w");
    DEVMODE dm;
    LONG rc;

    if (argc < 2) {
        if (f) { fprintf(f, "setbpp: usage: SETBPP <bpp> [width height]\n"); fclose(f); }
        return 2;
    }
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmBitsPerPel = atoi(argv[1]);
    dm.dmFields = DM_BITSPERPEL;
    if (argc >= 4) {
        dm.dmPelsWidth = atoi(argv[2]);
        dm.dmPelsHeight = atoi(argv[3]);
        dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    }
    if (f) fprintf(f, "setbpp: before %d bpp, asking for %ld\n", screen_bpp(), (long)dm.dmBitsPerPel);

    rc = ChangeDisplaySettings(&dm, 0);

    /* Give the driver's ReEnable and the shell's repaint a moment: a game
     * started in the same batch line otherwise creates its surfaces against
     * the mode that is on its way out. */
    Sleep(2500);
    if (f) {
        fprintf(f, "setbpp: ChangeDisplaySettings -> %ld (%s)\n", (long)rc,
                rc == DISP_CHANGE_SUCCESSFUL ? "ok" :
                rc == DISP_CHANGE_BADMODE ? "the driver has no such mode" :
                rc == DISP_CHANGE_RESTART ? "wants a restart" : "failed");
        fprintf(f, "setbpp: after %d bpp\n", screen_bpp());
        fclose(f);
    }
    return rc == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}

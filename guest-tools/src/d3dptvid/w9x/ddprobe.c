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
 * `DDPROBE <w> <h> <bpp> [sys]` adds a mode test after all that (2026-09-10):
 * an exclusive SetDisplayMode to the given mode, a flipping primary — with
 * DDSCAPS_SYSTEMMEMORY on it when `sys` is given, which is how Carmageddon
 * asks and which keeps the whole chain in the runtime's emulation layer
 * where the HAL never sees it — a palette at 8 bpp, five frames drawn and
 * flipped, RestoreDisplayMode; every HRESULT in the log. It is what found
 * the PDEVICE overrun of doc 19 §30 once the game had pointed at it.
 *
 * Build: guest-tools/build-driver9x.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *log_file;

/* One open-append-close per line, not an fflush: a flush hands the bytes to
 * the FAT driver, but the directory entry's size is written when the file
 * is closed — and a blue screen a few calls later leaves a 0-byte
 * DDPROBE.LOG on the disk (twice, 2026-09-10). Closing every time is what
 * makes the last line before the crash the one the harness reads back. */
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
    fclose(log_file);
    log_file = fopen("C:\\DDPROBE.LOG", "a");
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

    (void)prev; (void)show;
    log_file = fopen("C:\\DDPROBE.LOG", "w");
    logf_("ddprobe: start");

    /* **The runtime's own test, run here.** DirectDraw loads the 32-bit
     * HAL and calls DriverInit in DDHELP.EXE, and then validates the
     * callbacks it published with `IsBadCodePtr` — in *this* process. So
     * the question that decides everything is whether this process has
     * the DLL at all, and at which address; `GetModuleHandle` answers it
     * without loading anything, and `LoadLibrary` says where the system
     * put it (on 9x a module has one base for the whole machine). */
    {
        HMODULE m = GetModuleHandleA("d3dpt9hl.dll");
        HMODULE l;

        logf_("GetModuleHandle(d3dpt9hl.dll) -> %p", m);
        l = LoadLibraryA("d3dpt9hl.dll");
        logf_("LoadLibrary(d3dpt9hl.dll)     -> %p", l);
        if (l) {
            FARPROC f = GetProcAddress(l, "DriverInit");
            logf_("  DriverInit %p  IsBadCodePtr %d", f, f ? IsBadCodePtr(f) : -1);
            FreeLibrary(l);
        }
    }

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

    /* Flip chain test: exclusive fullscreen mode allows creating a complex
     * flipping primary surface and exercising Flip32 and its vblank pacing. */
    {
        HWND hwnd = CreateWindowA("STATIC", "ddprobe", WS_POPUP, 0, 0, 100, 100, NULL, NULL, inst, NULL);
        hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        logf_("SetCooperativeLevel(exclusive) -> 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            LPDIRECTDRAWSURFACE prim = NULL;
            LPDIRECTDRAWSURFACE back = NULL;

            memset(&sd, 0, sizeof(sd));
            sd.dwSize = sizeof(sd);
            sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
            sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            sd.dwBackBufferCount = 1;
            hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL);
            logf_("CreateSurface(flip chain) -> 0x%08lx", (unsigned long)hr);
            if (SUCCEEDED(hr) && prim) {
                DDSCAPS caps;
                describe("flipping primary", prim);

                memset(&caps, 0, sizeof(caps));
                caps.dwCaps = DDSCAPS_BACKBUFFER;
                hr = IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back);
                logf_("GetAttachedSurface(back) -> 0x%08lx", (unsigned long)hr);
                if (SUCCEEDED(hr) && back) {
                    int frame;
                    describe("back buffer", back);
                    for (frame = 0; frame < 5; frame++) {
                        DWORD t0 = GetTickCount();
                        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
                        DWORD t1 = GetTickCount();
                        logf_("  Flip %d -> 0x%08lx  dt %lu ms", frame, (unsigned long)hr, (unsigned long)(t1 - t0));
                    }
                    IDirectDrawSurface_Release(back);
                }
                IDirectDrawSurface_Release(prim);
            }
            IDirectDraw_SetCooperativeLevel(dd, GetDesktopWindow(), DDSCL_NORMAL);
        }
        if (hwnd) {
            DestroyWindow(hwnd);
        }
    }

    /* **A mode of the caller's choosing, the way a game asks for one.**
     * `DDPROBE <w> <h> <bpp>` is Carmageddon's opening (2026-09-10): an
     * exclusive full-screen SetDisplayMode to 320x200x8, a flipping
     * primary, a palette on it, a frame drawn and flipped, then
     * RestoreDisplayMode — each HRESULT logged, and the run paused with
     * the frame up so a screendump can see what the adapter scans out.
     * On this driver the game's request ended in a fatal exception in
     * KERNEL32 and no surface was ever created; this asks the same
     * questions without the game's own code in the way, and a step that
     * fails here names the driver's half of it. */
    if (cmd && cmd[0]) {
        unsigned w = 0, h = 0, bpp = 0;
        char extra[16] = "";

        /* A fourth word, `sys`, asks for the flip chain the way Carmageddon
         * does: DDSCAPS_SYSTEMMEMORY on a flipping primary. The runtime
         * keeps such a chain in its own emulation layer and the HAL never
         * hears of the surfaces, so a fault on that path is one the driver
         * log cannot show. */
        if (sscanf(cmd, "%u %u %u", &w, &h, &bpp) == 3 && w && h && bpp) {
            HWND hwnd = CreateWindowA("STATIC", "ddprobe mode", WS_POPUP | WS_VISIBLE,
                                      0, 0, 100, 100, NULL, NULL, inst, NULL);
            LPDIRECTDRAWSURFACE prim = NULL;
            LPDIRECTDRAWSURFACE back = NULL;
            LPDIRECTDRAWPALETTE pal = NULL;
            /* The words after the mode (2026-09-10, the Carmageddon black
             * screen): `sys` (above), `modex` = the game's own cooperative
             * level, DDSCL_ALLOWMODEX | DDSCL_ALLOWREBOOT on top of exclusive
             * full-screen — with it DirectDraw may answer a 320x200 request
             * with its *own* Mode X or VGA mode 13h, switching the display
             * driver out entirely; `vga` asks for that outright
             * (IDirectDraw2::SetDisplayMode with DDSDM_STANDARDVGAMODE);
             * `hold<N>` keeps the last frame up N seconds for screendumps. */
            DWORD coop = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN;
            int want_sys = 0, want_vga = 0, hold = 4;
            char args[128], *tok;

            strncpy(args, cmd, sizeof(args) - 1);
            args[sizeof(args) - 1] = 0;
            for (tok = strtok(args, " "); tok; tok = strtok(NULL, " ")) {
                if (strcmp(tok, "sys") == 0) want_sys = 1;
                else if (strcmp(tok, "modex") == 0) coop |= DDSCL_ALLOWMODEX | DDSCL_ALLOWREBOOT;
                else if (strcmp(tok, "vga") == 0) { want_vga = 1; coop |= DDSCL_ALLOWMODEX; }
                else if (strncmp(tok, "hold", 4) == 0) hold = atoi(tok + 4);
            }
            if (want_sys) strcpy(extra, "sys");

            logf_("mode test: %ux%ux%u  coop 0x%lx%s%s hold %d", w, h, bpp,
                  (unsigned long)coop, want_sys ? " sys" : "", want_vga ? " vga" : "", hold);
            hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, coop);
            logf_("  SetCooperativeLevel(exclusive) -> 0x%08lx", (unsigned long)hr);
            if (want_vga) {
                LPDIRECTDRAW2 dd2 = NULL;

                hr = IDirectDraw_QueryInterface(dd, &IID_IDirectDraw2, (void **)&dd2);
                logf_("  QueryInterface(IDirectDraw2) -> 0x%08lx", (unsigned long)hr);
                if (SUCCEEDED(hr) && dd2) {
                    hr = IDirectDraw2_SetDisplayMode(dd2, w, h, bpp, 0, DDSDM_STANDARDVGAMODE);
                    logf_("  SetDisplayMode(STANDARDVGAMODE) -> 0x%08lx", (unsigned long)hr);
                    IDirectDraw2_Release(dd2);
                }
            } else {
                hr = IDirectDraw_SetDisplayMode(dd, w, h, bpp);
                logf_("  SetDisplayMode -> 0x%08lx", (unsigned long)hr);
            }
            memset(&sd, 0, sizeof(sd));
            sd.dwSize = sizeof(sd);
            if (SUCCEEDED(IDirectDraw_GetDisplayMode(dd, &sd)))
                logf_("  GetDisplayMode: %lux%lux%lu pitch %ld caps 0x%08lx (MODEX 0x200000, STDVGA 0x40000000)",
                      (unsigned long)sd.dwWidth, (unsigned long)sd.dwHeight,
                      (unsigned long)sd.ddpfPixelFormat.dwRGBBitCount, (long)sd.lPitch,
                      (unsigned long)sd.ddsCaps.dwCaps);
            if (SUCCEEDED(hr)) {
                memset(&sd, 0, sizeof(sd));
                sd.dwSize = sizeof(sd);
                sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
                sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
                if (strcmp(extra, "sys") == 0) sd.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
                sd.dwBackBufferCount = 1;
                hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL);
                logf_("  CreateSurface(flip chain%s) -> 0x%08lx",
                      (sd.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? ", system memory" : "",
                      (unsigned long)hr);
            }
            if (SUCCEEDED(hr) && prim) {
                DDSCAPS caps;

                describe("  flipping primary", prim);
                if (bpp == 8) {
                    PALETTEENTRY pe[256];
                    int i;

                    for (i = 0; i < 256; i++) {
                        pe[i].peRed = (BYTE)i; pe[i].peGreen = (BYTE)(255 - i);
                        pe[i].peBlue = (BYTE)(i * 2); pe[i].peFlags = 0;
                    }
                    hr = IDirectDraw_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL);
                    logf_("  CreatePalette -> 0x%08lx", (unsigned long)hr);
                    if (SUCCEEDED(hr) && pal) {
                        hr = IDirectDrawSurface_SetPalette(prim, pal);
                        logf_("  SetPalette(primary) -> 0x%08lx", (unsigned long)hr);
                    }
                }
                memset(&caps, 0, sizeof(caps));
                caps.dwCaps = DDSCAPS_BACKBUFFER;
                hr = IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back);
                logf_("  GetAttachedSurface(back) -> 0x%08lx", (unsigned long)hr);
                if (SUCCEEDED(hr) && back) {
                    int frame;

                    describe("  back buffer", back);
                    for (frame = 0; frame < 5; frame++) {
                        DWORD t0, t1;

                        memset(&sd, 0, sizeof(sd));
                        sd.dwSize = sizeof(sd);
                        hr = IDirectDrawSurface_Lock(back, NULL, &sd, DDLOCK_WAIT, NULL);
                        logf_("    Lock %d -> 0x%08lx", frame, (unsigned long)hr);
                        if (SUCCEEDED(hr) && sd.lpSurface) {
                            BYTE *row = (BYTE *)sd.lpSurface;
                            unsigned y, x, bytepp = (bpp + 7) / 8;

                            for (y = 0; y < h; y++, row += sd.lPitch)
                                for (x = 0; x < w * bytepp; x++)
                                    row[x] = (BYTE)((x / bytepp) + y + frame * 16);
                            hr = IDirectDrawSurface_Unlock(back, NULL);
                            logf_("    Unlock %d -> 0x%08lx", frame, (unsigned long)hr);
                        }
                        t0 = GetTickCount();
                        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
                        t1 = GetTickCount();
                        logf_("    Lock/draw/Flip %d -> 0x%08lx  dt %lu ms", frame,
                              (unsigned long)hr, (unsigned long)(t1 - t0));
                    }
                    logf_("    holding %d s", hold);
                    Sleep(hold * 1000); /* leave the frame up for a screendump */
                    logf_("    releasing back...");
                    IDirectDrawSurface_Release(back);
                    logf_("    released back");
                }
                if (pal) { logf_("    releasing pal..."); IDirectDrawPalette_Release(pal); logf_("    released pal"); }
                logf_("    releasing prim...");
                IDirectDrawSurface_Release(prim);
                logf_("    released prim");
            }
            logf_("    RestoreDisplayMode...");
            hr = IDirectDraw_RestoreDisplayMode(dd);
            logf_("  RestoreDisplayMode -> 0x%08lx", (unsigned long)hr);
            hr = IDirectDraw_SetCooperativeLevel(dd, GetDesktopWindow(), DDSCL_NORMAL);
            logf_("  SetCooperativeLevel(normal) -> 0x%08lx", (unsigned long)hr);
            if (hwnd) DestroyWindow(hwnd);
            Sleep(2000);
        } else {
            logf_("mode test: arguments not understood: %s", cmd);
        }
    }

    IDirectDraw_Release(dd);
    logf_("ddprobe: done");
    if (log_file) fclose(log_file);
    return 0;
}

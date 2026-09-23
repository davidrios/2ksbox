/*
 * devcaps.c: every DirectDraw device on the machine and what each one's
 * Direct3D says it can do, for diffing ours against another driver's
 * (doc 19 §34). A title that decides a feature from caps does so before
 * any call a driver can log, so the only way to find the value it read is
 * to dump them all, on a device where the title works and on ours.
 *
 * For the primary and every secondary (a Voodoo 2 is one) it logs DDCAPS
 * (HAL), GetAvailableVidMem for video memory and for textures, and every
 * Direct3D device's D3DDEVICEDESC7, by field name and as a hex dump so a
 * field nobody thought of still shows up in a diff. It has no window, so
 * WIN.INI's run= starts it. It writes C:\2KSBOX\DEVCAPS.LOG
 * (tools/win98-game-test.sh STAGE= + PULL=).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>
#include "../../guestlog.h"

static FILE *g_log;

/* Closed and reopened after every line. If a driver hangs or faults
 * mid-enumeration, a file never closed keeps a FAT directory entry that
 * says 0 bytes. */
static void out(const char *fmt, ...)
{
    va_list ap;

    g_log = guest_log_open("DEVCAPS.LOG", "at");
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fclose(g_log);
}

static void hex(const char *what, const void *p, size_t n)
{
    const unsigned char *b = p;
    size_t i;

    for (i = 0; i < n; i += 32) {
        size_t j;

        out("  %s +%03x:", what, (unsigned)i);
        for (j = i; j < i + 32 && j < n; j += 4) out(" %08lx", *(const unsigned long *)(b + j));
        out("\n");
    }
}

static HRESULT CALLBACK enum_dev(char *desc, char *name, D3DDEVICEDESC7 *d, void *ctx)
{
    (void)ctx;
    out(" d3d device: %s (%s)\n", name, desc);
    out("  devcaps %08lx tricaps: misc %08lx raster %08lx zcmp %08lx src %08lx dst %08lx alphacmp %08lx\n",
        d->dwDevCaps, d->dpcTriCaps.dwMiscCaps, d->dpcTriCaps.dwRasterCaps, d->dpcTriCaps.dwZCmpCaps,
        d->dpcTriCaps.dwSrcBlendCaps, d->dpcTriCaps.dwDestBlendCaps, d->dpcTriCaps.dwAlphaCmpCaps);
    out("  shade %08lx texture %08lx filter %08lx blend %08lx address %08lx stipple %lux%lu\n",
        d->dpcTriCaps.dwShadeCaps, d->dpcTriCaps.dwTextureCaps, d->dpcTriCaps.dwTextureFilterCaps,
        d->dpcTriCaps.dwTextureBlendCaps, d->dpcTriCaps.dwTextureAddressCaps, d->dpcTriCaps.dwStippleWidth,
        d->dpcTriCaps.dwStippleHeight);
    out("  render depths %08lx z depths %08lx\n", d->dwDeviceRenderBitDepth, d->dwDeviceZBufferBitDepth);
    out("  tex %lux%lu..%lux%lu repeat %lu aspect %lu aniso %lu\n", d->dwMinTextureWidth, d->dwMinTextureHeight,
        d->dwMaxTextureWidth, d->dwMaxTextureHeight, d->dwMaxTextureRepeat, d->dwMaxTextureAspectRatio,
        d->dwMaxAnisotropy);
    out("  guard band %g,%g..%g,%g extents adjust %g stencil %08lx fvf %08lx texop %08lx stages %u simtex %u\n",
        d->dvGuardBandLeft, d->dvGuardBandTop, d->dvGuardBandRight, d->dvGuardBandBottom, d->dvExtentsAdjust,
        d->dwStencilCaps, d->dwFVFCaps, d->dwTextureOpCaps, d->wMaxTextureBlendStages, d->wMaxSimultaneousTextures);
    out("  lights %lu vtxproc %08lx clip planes %u blend matrices %u max w %g\n", d->dwMaxActiveLights,
        d->dwVertexProcessingCaps, d->wMaxUserClipPlanes, d->wMaxVertexBlendMatrices, d->dvMaxVertexW);
    hex("desc", d, sizeof(*d));
    return D3DENUMRET_OK;
}

static void vidmem(IDirectDraw7 *dd, const char *what, DWORD caps)
{
    DDSCAPS2 c;
    DWORD total = 0, avail = 0;
    HRESULT hr;

    memset(&c, 0, sizeof(c));
    c.dwCaps = caps;
    hr = IDirectDraw7_GetAvailableVidMem(dd, &c, &total, &avail);
    out(" GetAvailableVidMem(%s) -> %08lx total %lu KiB free %lu KiB\n", what, (unsigned long)hr, total >> 10, avail >> 10);
}

static BOOL WINAPI enum_dd(GUID *guid, char *desc, char *name, void *ctx, HMONITOR mon)
{
    IDirectDraw7 *dd = NULL;
    IDirect3D7 *d3d = NULL;
    DDCAPS hal, hel;
    HRESULT hr;

    (void)ctx;
    (void)mon;
    out("== directdraw device: %s (%s)%s\n", desc, name, guid ? "" : " [primary]");
    hr = DirectDrawCreateEx(guid, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !dd) {
        out(" DirectDrawCreateEx -> %08lx\n", (unsigned long)hr);
        return TRUE;
    }
    memset(&hal, 0, sizeof(hal));
    hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel));
    hel.dwSize = sizeof(hel);
    hr = IDirectDraw7_GetCaps(dd, &hal, &hel);
    out(" GetCaps -> %08lx caps %08lx caps2 %08lx ckey %08lx fx %08lx palette %08lx vidmem total %lu KiB free %lu KiB\n",
        (unsigned long)hr, hal.dwCaps, hal.dwCaps2, hal.dwCKeyCaps, hal.dwFXCaps, hal.dwPalCaps,
        hal.dwVidMemTotal >> 10, hal.dwVidMemFree >> 10);
    out(" ddscaps %08lx %08lx %08lx %08lx\n", hal.ddsCaps.dwCaps, hal.ddsCaps.dwCaps2, hal.ddsCaps.dwCaps3,
        hal.ddsCaps.dwCaps4);
    hex("ddcaps", &hal, sizeof(hal));
    vidmem(dd, "VIDEOMEMORY", DDSCAPS_VIDEOMEMORY);
    vidmem(dd, "TEXTURE", DDSCAPS_TEXTURE);
    vidmem(dd, "TEXTURE|VIDEOMEMORY", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY);
    vidmem(dd, "TEXTURE|LOCALVIDMEM", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
    vidmem(dd, "TEXTURE|NONLOCALVIDMEM", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_NONLOCALVIDMEM);
    hr = IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    if (SUCCEEDED(hr) && d3d) {
        IDirect3D7_EnumDevices(d3d, enum_dev, NULL);
        IDirect3D7_Release(d3d);
    } else {
        out(" no IDirect3D7 (%08lx)\n", (unsigned long)hr);
    }
    IDirectDraw7_Release(dd);
    return TRUE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst;
    (void)prev;
    (void)cmd;
    (void)show;
    g_log = guest_log_open("DEVCAPS.LOG", "wt");
    if (!g_log) return 1;
    out("devcaps: D3DDEVICEDESC7 is %u bytes, DDCAPS %u\n", (unsigned)sizeof(D3DDEVICEDESC7), (unsigned)sizeof(DDCAPS));
    /* NONDISPLAYDEVICES because a 3D-only card (the Voodoo 2) is neither
     * of the other two, and without it the enumeration stops at the
     * primary. */
    DirectDrawEnumerateExA(enum_dd, NULL, DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES |
                                              DDENUM_NONDISPLAYDEVICES);
    out("devcaps: done\n");
    fclose(g_log);
    return 0;
}

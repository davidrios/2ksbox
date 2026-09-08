/*
 * d3dpt9dd.c — the DirectDraw half of the Win98/Me display driver (doc 19
 * §2, M10 step 3): the `DCICOMMAND` escapes through which a 16-bit `.drv`
 * publishes a 32-bit HAL, and the `DDHALINFO_t` it builds.
 *
 * None of the actual HAL is here. On 9x the DirectDraw/Direct3D HAL is a
 * ring-3 Win32 DLL loaded into the game's own process (`d3dpt9hl.dll`,
 * the half that links our OS-independent core); this file's whole job is
 * to tell DirectDraw that the DLL exists, hand it the linear address of
 * the block the two halves share, and then copy back the callbacks the
 * DLL published into the tables DirectDraw wants.
 *
 * The four escapes, in the order they arrive:
 *
 *   DDVERSIONINFO         which DirectDraw HAL version we speak
 *   DDNEWCALLBACKFNS      DirectDraw hands us its lpSetInfo
 *   DDGET32BITDRIVERNAME  we answer with the DLL, its entry point and
 *                         the shared block's linear address
 *   DDCREATEDRIVEROBJECT  we build DDHALINFO_t and call lpSetInfo
 *
 * Build: guest-tools/build-driver9x.sh (Open Watcom, 16-bit, no CRT).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <ddrawi.h>

#include "d3dpt9x.h"
#include "d3dpt9hal.h"
#include "../../../../d3dpt/d3dpt_fb.h"

/* ------------------------------------------------------------------ DPMI */

/* Allocating the shared block is the one thing this file needs that the
 * rest of the driver does not: memory at a *linear* address, because the
 * 32-bit DLL has no idea what a selector is. DPMI gives both halves of
 * that — a block and a selector we point at it — and on 9x a block this
 * size comes out of the shared arena above 2 GiB, which is mapped into
 * every process, so the one address is valid wherever the DLL is loaded. */

extern WORD DPMI_AllocLDTDesc(WORD count);
#pragma aux DPMI_AllocLDTDesc = \
    "xor    ax, ax"             \
    "int    31h"                \
    "jnc    ok"                 \
    "xor    ax, ax"             \
    "ok:"                       \
    parm [cx];

extern DWORD DPMI_AllocMemBlk(DWORD size);
#pragma aux DPMI_AllocMemBlk =  \
    "xchg   cx, bx"             \
    "mov    ax, 501h"           \
    "int    31h"                \
    "jnc    ok"                 \
    "xor    bx, bx"             \
    "xor    cx, cx"             \
    "ok:"                       \
    "mov    dx, bx"             \
    "mov    ax, cx"             \
    parm [cx bx] modify [ax di si];

extern void DPMI_SetSegBase(WORD sel, DWORD base);
#pragma aux DPMI_SetSegBase =   \
    "mov    ax, 7"              \
    "int    31h"                \
    parm [bx] [cx dx];

extern void DPMI_SetSegLimit(WORD sel, DWORD limit);
#pragma aux DPMI_SetSegLimit =  \
    "mov    ax, 8"              \
    "int    31h"                \
    parm [bx] [cx dx];

/* ------------------------------------------------- the shared block */

static d3dpt_hal9 __far *pHal;          /* through our own selector */
static DWORD dwHalLinear;               /* the same block, as the DLL sees it */
static LPDDHAL_SETINFO lpSetInfo;       /* DirectDraw's, from DDNEWCALLBACKFNS */

/* The tables live in the shared block (see d3dpt9hal.h): these are just
 * the far pointers at them, and the linear addresses DDHALINFO carries. */
#define HALFIELD(type, field) ((type __far *)&pHal->field[0])
#define HALLINEAR(field)      (dwHalLinear + (DWORD)((BYTE __far *)&pHal->field[0] - (BYTE __far *)pHal))

/* each structure has to fit the slot the shared header reserved for it */
typedef char d3dpt_fits[
    (sizeof(DDHALINFO_t) <= sizeof(((d3dpt_hal9 *)0)->halinfo) &&
     sizeof(DDHAL_DDCALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_dd) &&
     sizeof(DDHAL_DDSURFACECALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_surf) &&
     sizeof(DDHAL_DDPALETTECALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_pal) &&
     sizeof(DDHALMODEINFO_t) <= sizeof(((d3dpt_hal9 *)0)->modeinfo) &&
     sizeof(VIDMEM_t) <= sizeof(((d3dpt_hal9 *)0)->heap)) ? 1 : -1];

/* The block is allocated once and never freed: DirectDraw hands its
 * linear address to every process that loads the DLL, and the display
 * driver outlives all of them. */
static BOOL HalBlock(void)
{
    WORD sel;

    if (pHal != 0) {
        return TRUE;
    }
    sel = DPMI_AllocLDTDesc(1);
    if (sel == 0) {
        dbg_str("d3dpt9dd: no LDT descriptor");
        return FALSE;
    }
    dwHalLinear = DPMI_AllocMemBlk((DWORD)sizeof(d3dpt_hal9));
    if (dwHalLinear == 0) {
        dbg_str("d3dpt9dd: no shared block");
        return FALSE;
    }
    DPMI_SetSegBase(sel, dwHalLinear);
    DPMI_SetSegLimit(sel, (DWORD)sizeof(d3dpt_hal9) - 1);
    pHal = (d3dpt_hal9 __far *)((DWORD)sel << 16);

    ZeroFar(pHal, sizeof(d3dpt_hal9));
    pHal->magic = D3DPT_HAL9_MAGIC;
    pHal->version = D3DPT_HAL9_VERSION;
    pHal->size = sizeof(d3dpt_hal9);
    /* The adapter, as the mini-VDD mapped it. Straight from the VxD, not
     * from DPMI: these selectors are GDT ones the mini-VDD built, and
     * DPMI's "get segment base" only knows the LDT — it answered with
     * junk and the DLL was handed a register page at 0x28d7
     * (2026-09-07). */
    pHal->regs_linear = dwRegsLin;
    pHal->vram_linear = dwVramLin;
    pHal->vram_size = dwVramSize;
    HalMode();                          /* the DLL is loaded next and reads it */
    dbg_val("d3dpt9dd: shared block at", dwHalLinear);
    dbg_val("d3dpt9dd: regs linear", pHal->regs_linear);
    dbg_val("d3dpt9dd: vram linear", pHal->vram_linear);
    return TRUE;
}

/* the mode, filled when the block is made and refreshed at every
 * DDCREATEDRIVEROBJECT: DirectDraw asks again after every mode change,
 * and the DLL reads it from here */
static void HalMode(void)
{
    pHal->width = wScrX;
    pHal->height = wScrY;
    pHal->bpp = wBpp;
    pHal->pitch = dwPitch;
}

/* ------------------------------------------------------- DDHALINFO_t */

static void BuildPixelFormat(DDPIXELFORMAT_t __far *pf)
{
    ZeroFar(pf, sizeof(DDPIXELFORMAT_t));
    pf->dwSize = sizeof(DDPIXELFORMAT_t);
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = wBpp;
    if (wBpp == 8) {
        pf->dwFlags |= DDPF_PALETTEINDEXED8;
        return;
    }
    if (wBpp == 16) {
        pf->dwRBitMask = 0xf800; pf->dwGBitMask = 0x07e0; pf->dwBBitMask = 0x001f;
    } else {
        pf->dwRBitMask = 0x00ff0000ul; pf->dwGBitMask = 0x0000ff00ul; pf->dwBBitMask = 0x000000fful;
    }
}

/* Copy the callbacks the DLL published into the tables DirectDraw reads,
 * and set the matching flag for each. A zero in cb32 simply means the
 * DLL does not implement that one and DirectDraw's own HEL does it. */
static void BuildCallbacks(void)
{
    DDHAL_DDCALLBACKS_t __far *pcbDD = HALFIELD(DDHAL_DDCALLBACKS_t, cb_dd);
    DDHAL_DDSURFACECALLBACKS_t __far *pcbSurf = HALFIELD(DDHAL_DDSURFACECALLBACKS_t, cb_surf);
    DDHAL_DDPALETTECALLBACKS_t __far *pcbPal = HALFIELD(DDHAL_DDPALETTECALLBACKS_t, cb_pal);
#define cbDD (*pcbDD)
#define cbSurf (*pcbSurf)
#define cbPal (*pcbPal)

    ZeroFar(&cbDD, sizeof(cbDD));
    ZeroFar(&cbSurf, sizeof(cbSurf));
    ZeroFar(&cbPal, sizeof(cbPal));
    cbDD.dwSize = sizeof(cbDD);
    cbSurf.dwSize = sizeof(cbSurf);
    cbPal.dwSize = sizeof(cbPal);

#define CB(tab, member, field, flag)                                    \
    if (pHal->cb32.field) {                                             \
        *(DWORD *)&(tab).member = pHal->cb32.field;                     \
        (tab).dwFlags |= (flag);                                        \
    }
    CB(cbDD, DestroyDriver, DestroyDriver, DDHAL_CB32_DESTROYDRIVER)
    CB(cbDD, CreateSurface, CreateSurface, DDHAL_CB32_CREATESURFACE)
    CB(cbDD, SetColorKey, SetColorKey, DDHAL_CB32_SETCOLORKEY)
    CB(cbDD, SetMode, SetMode, DDHAL_CB32_SETMODE)
    CB(cbDD, WaitForVerticalBlank, WaitForVerticalBlank, DDHAL_CB32_WAITFORVERTICALBLANK)
    CB(cbDD, CanCreateSurface, CanCreateSurface, DDHAL_CB32_CANCREATESURFACE)
    CB(cbDD, CreatePalette, CreatePalette, DDHAL_CB32_CREATEPALETTE)
    CB(cbDD, GetScanLine, GetScanLine, DDHAL_CB32_GETSCANLINE)

    CB(cbSurf, DestroySurface, DestroySurface, DDHAL_SURFCB32_DESTROYSURFACE)
    CB(cbSurf, Flip, Flip, DDHAL_SURFCB32_FLIP)
    CB(cbSurf, SetClipList, SetClipList, DDHAL_SURFCB32_SETCLIPLIST)
    CB(cbSurf, Lock, Lock, DDHAL_SURFCB32_LOCK)
    CB(cbSurf, Unlock, Unlock, DDHAL_SURFCB32_UNLOCK)
    CB(cbSurf, Blt, Blt, DDHAL_SURFCB32_BLT)
    CB(cbSurf, SetColorKey, SetColorKeySurface, DDHAL_SURFCB32_SETCOLORKEY)
    CB(cbSurf, AddAttachedSurface, AddAttachedSurface, DDHAL_SURFCB32_ADDATTACHEDSURFACE)
    CB(cbSurf, GetBltStatus, GetBltStatus, DDHAL_SURFCB32_GETBLTSTATUS)
    CB(cbSurf, GetFlipStatus, GetFlipStatus, DDHAL_SURFCB32_GETFLIPSTATUS)
    CB(cbSurf, UpdateOverlay, UpdateOverlay, DDHAL_SURFCB32_UPDATEOVERLAY)
    CB(cbSurf, SetOverlayPosition, SetOverlayPosition, DDHAL_SURFCB32_SETOVERLAYPOSITION)
    CB(cbSurf, SetPalette, SetPalette, DDHAL_SURFCB32_SETPALETTE)
#undef CB
    dbg_val("d3dpt9dd:   dd callbacks", cbDD.dwFlags);
    dbg_val(" surface callbacks", cbSurf.dwFlags);
    dbg_str("");
#undef cbDD
#undef cbSurf
#undef cbPal
}

static BOOL BuildHalInfo(void)
{
    DDHALINFO_t __far *hi = HALFIELD(DDHALINFO_t, halinfo);
    DDHALMODEINFO_t __far *mi = HALFIELD(DDHALMODEINFO_t, modeinfo);
    VIDMEM_t __far *hp = HALFIELD(VIDMEM_t, heap);
    DWORD start;

    ZeroFar(hi, sizeof(*hi));
    ZeroFar(mi, sizeof(*mi));
    ZeroFar(hp, sizeof(*hp));

    mi->dwWidth = wScrX;
    mi->dwHeight = wScrY;
    mi->lPitch = dwPitch;
    mi->dwBPP = wBpp;
    mi->wFlags = (wBpp == 8) ? DDMODEINFO_PALETTIZED : 0;
    mi->wRefreshRate = 0;
    /* DDHALMODEINFO carries the masks themselves, not a DDPIXELFORMAT */
    if (wBpp == 16) {
        mi->dwRBitMask = 0xf800; mi->dwGBitMask = 0x07e0; mi->dwBBitMask = 0x001f;
    } else if (wBpp > 8) {
        mi->dwRBitMask = 0x00ff0000ul; mi->dwGBitMask = 0x0000ff00ul; mi->dwBBitMask = 0x000000fful;
    }

    hi->dwSize = sizeof(*hi);
    /* Far pointers, not linear addresses: DDHALINFO is handed to the
     * *16-bit* runtime and it is the one that walks these. What the
     * 32-bit half needs is the cb32 entries, which reach it another way.
     * (Passing linear addresses here instead made DDHAL_SetInfo refuse
     * the HALINFO outright, 2026-09-07.) The structures still live in the
     * shared block so that the DLL can read them too, by offset from the
     * block's linear base. */
    hi->lpDDCallbacks = (LPDDHAL_DDCALLBACKS)HALFIELD(DDHAL_DDCALLBACKS_t, cb_dd);
    hi->lpDDSurfaceCallbacks = (LPDDHAL_DDSURFACECALLBACKS)HALFIELD(DDHAL_DDSURFACECALLBACKS_t, cb_surf);
    hi->lpDDPaletteCallbacks = (LPDDHAL_DDPALETTECALLBACKS)HALFIELD(DDHAL_DDPALETTECALLBACKS_t, cb_pal);
    hi->lpModeInfo = (LPDDHALMODEINFO)HALFIELD(DDHALMODEINFO_t, modeinfo);
    hi->dwNumModes = 1;
    hi->dwModeIndex = 0;

    hi->vmiData.fpPrimary = pHal->vram_linear;
    hi->vmiData.dwFlags = 0;
    hi->vmiData.dwDisplayWidth = wScrX;
    hi->vmiData.dwDisplayHeight = wScrY;
    hi->vmiData.lDisplayPitch = dwPitch;
    BuildPixelFormat(&hi->vmiData.ddpfDisplay);
    hi->vmiData.dwOffscreenAlign = 64;
    hi->vmiData.dwZBufferAlign = 64;
    hi->vmiData.dwOverlayAlign = 64;
    hi->vmiData.dwAlphaAlign = 64;
    hi->vmiData.dwTextureAlign = 64;

    /* One linear heap: everything behind the visible frame, up to where
     * the hardware cursor's image and the command window begin. A HAL
     * with no heap is a HAL DirectDraw has nowhere to allocate from.
     * `ddsCaps` here is what the memory *cannot* be used for, which is
     * why it is left empty.
     *
     * MulW, not `*`: no CRT here, so a 32-bit multiply has no helper. The
     * pitch of every mode the adapter offers fits a WORD (1600x32bpp is
     * 6400 bytes). */
    start = (MulW((WORD)dwPitch, wScrY) + 4095ul) & ~4095ul;
    hp->dwFlags = VIDMEM_ISLINEAR;
    hp->fpStart = pHal->vram_linear + start;
    hp->fpEnd = pHal->vram_linear + pHal->vram_size - D3DPT_FB_CURSOR_BYTES - 1;
    hi->vmiData.dwNumHeaps = 1;
    hi->vmiData.pvmList = (LPVIDMEM)HALFIELD(VIDMEM_t, heap);

    hi->ddCaps.dwSize = sizeof(DDCORECAPS_t);
    /* DDCAPS_GDI is normal on 9x and fatal on NT (doc 19 §5): here it
     * says the primary is the same memory GDI draws into, which it is. */
    hi->ddCaps.dwCaps = DDCAPS_GDI | DDCAPS_BLTQUEUE;
    hi->ddCaps.dwCaps2 = DDCAPS2_CERTIFIED;
    hi->ddCaps.dwVidMemTotal = pHal->vram_size - start;
    hi->ddCaps.dwVidMemFree = pHal->vram_size - start;
    hi->ddCaps.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE |
                                DDSCAPS_FLIP | DDSCAPS_VIDEOMEMORY;

    hi->dwMonitorFrequency = 0;
    hi->dwFlags = DDHALINFO_MODEXILLEGAL;
    /* the module DirectDraw loaded our 32-bit callbacks out of, and the
     * driver's own PDEVICE */
    hi->hInstance = pHal->dll_hinstance;
    hi->lpPDevice = (LPVOID)lpDriverPDevice;
    return TRUE;
}

/* --------------------------------------------------- the four escapes */

/* DDGET32BITDRIVERNAME: the DLL DirectDraw should load into the calling
 * process, the entry point to call in it, and the context value that
 * entry point is given — our shared block's linear address. */
BOOL DDGet32BitDriverName(DD32BITDRIVERDATA_t __far *dd32)
{
    static const char szDll[] = D3DPT_HAL_DLL;
    static const char szEntry[] = D3DPT_HAL_ENTRY;
    WORD i;

    if (!HalBlock()) {
        return FALSE;
    }
    ZeroFar(dd32, sizeof(DD32BITDRIVERDATA_t));
    for (i = 0; szDll[i] != 0; i++) dd32->szName[i] = szDll[i];
    for (i = 0; szEntry[i] != 0; i++) dd32->szEntryPoint[i] = szEntry[i];
    dd32->dwContext = dwHalLinear;
    return TRUE;
}

/* DDNEWCALLBACKFNS: DirectDraw's own function table; the only thing we
 * keep out of it is lpSetInfo, which is how a HAL is handed over. */
BOOL DDNewCallbackFns(DCICMD_t __far *lpCmd)
{
    LPDDHALDDRAWFNS pfns = (LPDDHALDDRAWFNS)lpCmd->dwParam1;

    if (pfns == 0) {
        return FALSE;
    }
    lpSetInfo = pfns->lpSetInfo;
    return TRUE;
}

/* DDVERSIONINFO */
void DDGetVersion(DDVERSIONDATA_t __far *lpVer)
{
    ZeroFar(lpVer, sizeof(DDVERSIONDATA_t));
    lpVer->dwHALVersion = DD_RUNTIME_VERSION;
}

/* DDCREATEDRIVEROBJECT: build the HALINFO and hand it over. By now the
 * DLL has been loaded and its DriverInit has filled cb32 — that is the
 * order DirectDraw uses, and the reason the callbacks are copied here
 * rather than when the name was asked for. */
BOOL DDCreateDriverObject(void)
{
    if (lpSetInfo == 0) {
        dbg_str("d3dpt9dd: no lpSetInfo");
        return FALSE;
    }
    if (!HalBlock()) {
        return FALSE;
    }
    HalMode();
    if (!pHal->dll_ready) {
        /* Not fatal: DirectDraw still gets a HAL, it just has no
         * callbacks and does everything in its own HEL. Worth a line,
         * because it means the DLL was not found or refused the block. */
        dbg_str("d3dpt9dd: the 32-bit HAL did not report in");
    }
    BuildCallbacks();
    if (!BuildHalInfo()) {
        return FALSE;
    }
    dbg_val("d3dpt9dd: hal for mode", ((DWORD)wScrX << 16) | wScrY);
    dbg_val("d3dpt9dd:   the DLL read magic", pHal->dll_reg_magic);
    dbg_val(" version", pHal->dll_reg_version);
    dbg_str("");
    dbg_val("d3dpt9dd:   hinstance", pHal->dll_hinstance);
    dbg_str("");
    /* The runtime's answer matters: it is the only place it says whether
     * it took the HAL, and a HAL it did not take looks exactly like one
     * that was never offered. */
    if (!lpSetInfo(HALFIELD(DDHALINFO_t, halinfo), FALSE)) {
        dbg_str("d3dpt9dd: DirectDraw refused the HALINFO");
        return FALSE;
    }
    dbg_str("d3dpt9dd: DirectDraw took the HAL");
    return TRUE;
}

/* What DDCREATEDRIVEROBJECT returns: the module DirectDraw loaded the
 * 32-bit callbacks out of. */
DWORD DDHinstance(void)
{
    return pHal ? pHal->dll_hinstance : 0;
}

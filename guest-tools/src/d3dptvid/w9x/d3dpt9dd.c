/*
 * d3dpt9dd.c: the DirectDraw half of the Win98/Me display driver (doc 19
 * §2). It answers the `DCICOMMAND` escapes through which a 16-bit `.drv`
 * publishes a 32-bit HAL, and builds the `DDHALINFO_t`.
 *
 * None of the HAL itself is here. On 9x the DirectDraw/Direct3D HAL is a
 * ring-3 Win32 DLL loaded into the game's own process (`d3dpt9hl.dll`,
 * which links our OS-independent core). This file tells DirectDraw that
 * the DLL exists, hands it the linear address of the block the two halves
 * share, and copies the callbacks the DLL published into the tables
 * DirectDraw wants.
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

/* The shared block needs a linear address, because the 32-bit DLL cannot
 * use a selector. DPMI gives a block and a selector we point at it. On 9x
 * a block this size comes out of the shared arena above 2 GiB, which is
 * mapped into every process, so the one address is valid wherever the DLL
 * is loaded. */

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

static void HalMode(void);
static WORD wHalUnreachable;            /* said once, not once per call */

/* `lar` on a selector sets ZF when the descriptor exists and this ring may
 * read it. It is the only check possible on `lpSetInfo` before calling it.
 * A far call into an unloaded module faults silently and takes the
 * caller's program with it (doc 19 §40). */
extern WORD SelOk(WORD sel);
#pragma aux SelOk =     \
    "lar   ax, ax"      \
    "mov   ax, 0"       \
    "jnz   gone"        \
    "inc   ax"          \
    "gone:"             \
    parm [ax] value [ax] modify [ax];

/* the selector half of the far pointer (offset first, selector second) */
#define SEL_OF(fp) (*((WORD *)&(fp) + 1))

/* The bisection knob, read off the adapter (see D9F_* in d3dpt9x.h). Read
 * on every use rather than cached, because the escapes arrive from more
 * than one place and no init this file owns runs before all of them. */
#define DDF() RegGet(D3DPT_FB_REG_DDFLAGS)

/* The tables live in the shared block (see d3dpt9hal.h). These are the
 * far pointers to them and the linear addresses DDHALINFO carries. */
#define HALFIELD(type, field) ((type __far *)&pHal->field[0])
#define HALLINEAR(field)      (dwHalLinear + (DWORD)((BYTE __far *)&pHal->field[0] - (BYTE __far *)pHal))

/* each structure has to fit the slot the shared header reserved for it */
typedef char d3dpt_fits[
    (sizeof(DDHALINFO_t) <= sizeof(((d3dpt_hal9 *)0)->halinfo) &&
     sizeof(DDHAL_DDCALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_dd) &&
     sizeof(DDHAL_DDSURFACECALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_surf) &&
     sizeof(DDHAL_DDPALETTECALLBACKS_t) <= sizeof(((d3dpt_hal9 *)0)->cb_pal) &&
     sizeof(DDHALMODEINFO_t) * D3DPT_HAL9_MAX_MODES <= sizeof(((d3dpt_hal9 *)0)->modeinfo) &&
     sizeof(VIDMEM_t) <= sizeof(((d3dpt_hal9 *)0)->heap)) ? 1 : -1];

/* The block is allocated once and never freed. DirectDraw hands its
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
    /* The adapter as the mini-VDD mapped it, straight from the VxD. DPMI
     * cannot answer this, because these are GDT selectors the mini-VDD
     * built and DPMI's "get segment base" only knows the LDT (it returns
     * junk, such as a register page at 0x28d7). */
    pHal->regs_linear = dwRegsLin;
    pHal->vram_linear = dwVramLin;
    pHal->vram_size = dwVramSize;
    HalMode();                          /* the DLL is loaded next and reads it */
    dbg_val("d3dpt9dd: shared block at", dwHalLinear);
    dbg_val("d3dpt9dd: regs linear", pHal->regs_linear);
    dbg_val("d3dpt9dd: vram linear", pHal->vram_linear);
    return TRUE;
}

/* The mode, filled when the block is made and refreshed at every
 * DDCREATEDRIVEROBJECT, which DirectDraw sends again after every mode
 * change. The DLL reads it from here. */
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
 * and set the matching flag for each. A zero in cb32 means the DLL does
 * not implement that one and DirectDraw's own HEL does it. */
/* Whether a callback the DLL published is worth handing to DirectDraw.
 *
 * The addresses are DDHELP.EXE's, and the runtime checks them in the
 * game's process (doc 19 §22). DirectDraw loads the 32-bit HAL and calls
 * `DriverInit` in `DDHELP.EXE`, once for the machine. Every application
 * then validates the stored HALINFO in its own address space,
 * `IsBadCodePtr` on every entry a flag claims, and refuses the whole
 * driver object if one fails. A module in the private arena gets a
 * different address in each process (DDHELP had it at 0x00b50000 and
 * ddprobe's own LoadLibrary at 0x00ca0000), so its entries are bad
 * pointers everywhere else and the HAL is thrown away, video memory heap
 * and mode list included.
 *
 * The DLL is based in the shared arena above 2 GiB (doc 19 §23), where one
 * address means the same thing in every process. This is the safety net
 * if it ever lands below: the callbacks are withheld and the log says so,
 * because a DirectDraw that draws by itself into our video memory is
 * worth more than one that draws into its own. */
static BOOL HalReachable(DWORD fn)
{
    if (fn == 0) {
        return FALSE;
    }
    if (fn < 0x80000000ul) {
        if (!wHalUnreachable) {
            wHalUnreachable = 1;
            dbg_val("d3dpt9dd: the 32-bit HAL is at", fn);
            dbg_str(", below the shared arena: callbacks withheld");
        }
        return FALSE;
    }
    return TRUE;
}

static void BuildCallbacks(void)
{
    DDHAL_DDCALLBACKS_t __far *pcbDD = HALFIELD(DDHAL_DDCALLBACKS_t, cb_dd);
    DDHAL_DDSURFACECALLBACKS_t __far *pcbSurf = HALFIELD(DDHAL_DDSURFACECALLBACKS_t, cb_surf);
    DDHAL_DDPALETTECALLBACKS_t __far *pcbPal = HALFIELD(DDHAL_DDPALETTECALLBACKS_t, cb_pal);
    DDHAL_DDEXEBUFCALLBACKS_t __far *pcbExeBuf = HALFIELD(DDHAL_DDEXEBUFCALLBACKS_t, cb_exebuf);
#define cbDD (*pcbDD)
#define cbSurf (*pcbSurf)
#define cbPal (*pcbPal)
#define cbExeBuf (*pcbExeBuf)

    ZeroFar(&cbDD, sizeof(cbDD));
    ZeroFar(&cbSurf, sizeof(cbSurf));
    ZeroFar(&cbPal, sizeof(cbPal));
    cbDD.dwSize = sizeof(cbDD);
    cbSurf.dwSize = sizeof(cbSurf);
    cbPal.dwSize = sizeof(cbPal);

/* `DWORD __far *`, and the `__far` matters. The tables live in the shared
 * block, so `&(tab).member` is a far pointer. This file is compiled in the
 * small model, where a plain `DWORD *` is near, so `*(DWORD *)&...`
 * silently truncates it to its offset and stores through DS into the
 * driver's own data segment. The slot itself stays zero.
 *
 * From outside that is invisible. The flags land, because
 * `(tab).dwFlags |= flag` goes through the far struct, so the driver's log
 * and DirectDraw both say the callbacks are published. DirectDraw's
 * HALINFO validator agrees, because it only `IsBadCodePtr`s non-zero
 * entries. The HAL is accepted with a table of nulls and every call goes
 * to the runtime's own HEL. The cast is needed because the member is a
 * 16-bit far function pointer and the value is a 32-bit flat address. */
/* A callback is published only if it can be reached (`HalReachable`
 * above). */
#define CB(tab, member, field, flag)                                    \
    if (HalReachable(pHal->cb32.field)) {                               \
        *(DWORD __far *)&(tab).member = pHal->cb32.field;               \
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

    ZeroFar(&cbExeBuf, sizeof(cbExeBuf));
    cbExeBuf.dwSize = sizeof(cbExeBuf);
    CB(cbExeBuf, CanCreateExecuteBuffer, CanCreateExecuteBuffer, DDHAL_EXEBUFCB32_CANCREATEEXEBUF)
    CB(cbExeBuf, CreateExecuteBuffer, CreateExecuteBuffer, DDHAL_EXEBUFCB32_CREATEEXEBUF)
    CB(cbExeBuf, DestroyExecuteBuffer, DestroyExecuteBuffer, DDHAL_EXEBUFCB32_DESTROYEXEBUF)
    CB(cbExeBuf, LockExecuteBuffer, LockExecuteBuffer, DDHAL_EXEBUFCB32_LOCKEXEBUF)
    CB(cbExeBuf, UnlockExecuteBuffer, UnlockExecuteBuffer, DDHAL_EXEBUFCB32_UNLOCKEXEBUF)
#undef CB
    dbg_val("d3dpt9dd:   dd callbacks", cbDD.dwFlags);
    dbg_val(" surface callbacks", cbSurf.dwFlags);
    dbg_val(" exebuf callbacks", pcbExeBuf->dwFlags);
    dbg_str("");
    /* Read the table back through the far pointer rather than trust the
     * store. The runtime `IsBadCodePtr`s every entry a flag claims and
     * refuses the whole HALINFO if one is bad, so what is in the slots is
     * what matters. */
    dbg_val("d3dpt9dd:   cbDD size", (DWORD)sizeof(cbDD));
    dbg_val(" destroy", *(DWORD __far *)&cbDD.DestroyDriver);
    dbg_val(" cansurf", *(DWORD __far *)&cbDD.CanCreateSurface);
    dbg_val(" vblank", *(DWORD __far *)&cbDD.WaitForVerticalBlank);
    dbg_val(" exebuf cancreate", *(DWORD __far *)&pcbExeBuf->CanCreateExecuteBuffer);
    dbg_str("");
#undef cbDD
#undef cbSurf
#undef cbPal
#undef cbExeBuf
}

static BOOL BuildHalInfo(void)
{
    static const struct {
        WORD w, h;
    } s_res[] = {
        { 640,  480 },
        { 800,  600 },
        { 1024, 768 },
        { 1280, 1024 },
        { 400,  300 },
        { 512,  384 },
        /* No 320x200 and no 320x240 (doc 19 §30). Those two are
         * DirectDraw's own. With DDSCL_ALLOWMODEX the runtime answers a
         * 320x200 request with Mode X / VGA mode 13h on the VGA core,
         * programs the DAC and copies the flip chain into VGA memory
         * itself. That is the only way a system-memory flipping primary
         * (Carmageddon's, the SDK's Mode X recipe) is ever displayed. As a
         * driver mode the same request got a real 320x200 linear mode with
         * a system-memory primary that nothing presents: a black screen
         * with the right palette. */
    };
    static const WORD s_bpp[] = { 16, 32, 8 };

    DDHALINFO_t __far *hi = HALFIELD(DDHALINFO_t, halinfo);
    DDHALMODEINFO_t __far *mi = HALFIELD(DDHALMODEINFO_t, modeinfo);
    VIDMEM_t __far *hp = HALFIELD(VIDMEM_t, heap);
    DWORD start, min_start, end;
    WORD n = 0, cur_idx = 0xffff;
    WORD i, j;

    ZeroFar(hi, sizeof(*hi));
    ZeroFar(mi, sizeof(DDHALMODEINFO_t) * D3DPT_HAL9_MAX_MODES);
    ZeroFar(hp, sizeof(*hp));

    for (i = 0; i < sizeof(s_res) / sizeof(s_res[0]); i++) {
        for (j = 0; j < sizeof(s_bpp) / sizeof(s_bpp[0]); j++) {
            WORD w = s_res[i].w;
            WORD h = s_res[i].h;
            WORD bpp = s_bpp[j];
            DWORD pitch = D3DPT9X_PITCH(w, bpp);
            DWORD need = MulW((WORD)pitch, h);
            if (need <= pHal->vram_size && n < D3DPT_HAL9_MAX_MODES) {
                DDHALMODEINFO_t __far *m = &mi[n];
                m->dwWidth = w;
                m->dwHeight = h;
                m->lPitch = pitch;
                m->dwBPP = bpp;
                m->wFlags = (bpp == 8) ? DDMODEINFO_PALETTIZED : 0;
                m->wRefreshRate = 0;
                if (bpp == 16) {
                    m->dwRBitMask = 0xf800;
                    m->dwGBitMask = 0x07e0;
                    m->dwBBitMask = 0x001f;
                } else if (bpp > 8) {
                    m->dwRBitMask = 0x00ff0000ul;
                    m->dwGBitMask = 0x0000ff00ul;
                    m->dwBBitMask = 0x000000fful;
                }
                m->dwAlphaBitMask = 0;
                if (w == wScrX && h == wScrY && bpp == wBpp) {
                    cur_idx = n;
                }
                n++;
            }
        }
    }

    if (cur_idx == 0xffff && n < D3DPT_HAL9_MAX_MODES) {
        DDHALMODEINFO_t __far *m = &mi[n];
        m->dwWidth = wScrX;
        m->dwHeight = wScrY;
        m->lPitch = dwPitch;
        m->dwBPP = wBpp;
        m->wFlags = (wBpp == 8) ? DDMODEINFO_PALETTIZED : 0;
        m->wRefreshRate = 0;
        if (wBpp == 16) {
            m->dwRBitMask = 0xf800;
            m->dwGBitMask = 0x07e0;
            m->dwBBitMask = 0x001f;
        } else if (wBpp > 8) {
            m->dwRBitMask = 0x00ff0000ul;
            m->dwGBitMask = 0x0000ff00ul;
            m->dwBBitMask = 0x000000fful;
        }
        m->dwAlphaBitMask = 0;
        cur_idx = n;
        n++;
    }

    hi->dwSize = sizeof(*hi);
    /* Far pointers, not linear addresses. DDHALINFO is handed to the
     * 16-bit runtime, which walks these, and DDHAL_SetInfo refuses a
     * HALINFO with linear addresses here. The 32-bit half gets the cb32
     * entries another way. The structures still live in the shared block
     * so the DLL can read them too, by offset from the block's linear
     * base. */
    hi->lpDDCallbacks = (LPDDHAL_DDCALLBACKS)HALFIELD(DDHAL_DDCALLBACKS_t, cb_dd);
    hi->lpDDSurfaceCallbacks = (LPDDHAL_DDSURFACECALLBACKS)HALFIELD(DDHAL_DDSURFACECALLBACKS_t, cb_surf);
    hi->lpDDPaletteCallbacks = (LPDDHAL_DDPALETTECALLBACKS)HALFIELD(DDHAL_DDPALETTECALLBACKS_t, cb_pal);
    hi->lpModeInfo = (LPDDHALMODEINFO)HALFIELD(DDHALMODEINFO_t, modeinfo);
    hi->dwNumModes = n;
    hi->dwModeIndex = (cur_idx != 0xffff) ? cur_idx : 0;
    dbg_val("d3dpt9dd: num modes", (DWORD)n);
    dbg_val("d3dpt9dd: cur mode idx", (DWORD)hi->dwModeIndex);

    hi->vmiData.fpPrimary = pHal->vram_linear;
    hi->vmiData.dwFlags = 0;
    hi->vmiData.dwDisplayWidth = wScrX;
    hi->vmiData.dwDisplayHeight = wScrY;
    hi->vmiData.lDisplayPitch = dwPitch;
    BuildPixelFormat(&hi->vmiData.ddpfDisplay);
    hi->vmiData.dwOffscreenAlign = D3DPT9X_PITCH_ALIGN;  /* = every mode's pitch rounding */
    hi->vmiData.dwZBufferAlign = 64;
    hi->vmiData.dwOverlayAlign = 64;
    hi->vmiData.dwAlphaAlign = 64;
    hi->vmiData.dwTextureAlign = 64;

    /* One linear heap: everything behind the visible frame, up to where
     * the hardware cursor's image and the command window begin. Without a
     * heap DirectDraw has nowhere to allocate from. `ddsCaps` here is what
     * the memory cannot be used for, so it stays empty.
     *
     * MulW, not `*`, because with no CRT a 32-bit multiply has no helper. The
     * pitch of every mode the adapter offers fits a WORD (1600x32bpp is
     * 6400 bytes). */
    start = (MulW((WORD)dwPitch, wScrY) + 4095ul) & ~4095ul;
    min_start = 8ul * 1024ul * 1024ul;
    if (start < min_start) start = min_start;

    /* Where the heap ends is the adapter's answer, not arithmetic. The top
     * of VRAM is not free. The command window the Direct3D half encodes
     * batches into sits at `D3DPT_FB_REG_CMD_OFFSET` (64 MB of a 128 MB
     * aperture), and the cursor sprite's image sits immediately below it.
     * Ending the heap at `vram_size - CURSOR_BYTES` made it 64 MB too long,
     * overlapping the command window, so a DirectDraw surface and the batch
     * ring could share memory. `core/`'s `dd_heap_end()` is the same
     * calculation on NT. This layer is 16-bit and cannot call that flat
     * 32-bit code, so it reads the same register.
     *
     * Read values like this from the one authority. Re-deriving them here
     * went wrong three times: `DDSCAPS_EXECUTEBUFFER`, the command window
     * (doc 19 §25) and the end of the heap. */
    end = RegGet(D3DPT_FB_REG_CMD_OFFSET);
    if (!end || end > pHal->vram_size) end = pHal->vram_size;
    end -= D3DPT_FB_CURSOR_BYTES;
    hp->dwFlags = VIDMEM_ISLINEAR;
    hp->fpStart = pHal->vram_linear + start;
    hp->fpEnd = pHal->vram_linear + end - 1;
    hi->vmiData.dwNumHeaps = 1;
    hi->vmiData.pvmList = (LPVIDMEM)HALFIELD(VIDMEM_t, heap);

    hi->ddCaps.dwSize = sizeof(DDCORECAPS_t);
    /* DDCAPS_GDI is normal on 9x and fatal on NT (doc 19 §5). Here it says
     * the primary is the same memory GDI draws into, which it is. */
    /* No DDCAPS_BLTDEPTHFILL. Claimed alone it does not route a depth fill
     * to Blt32. The runtime still does it itself through a Lock of the Z
     * buffer, which is where the HAL sees it (Unlock32, doc 19 §34).
     * DDCAPS_BLT would route it, but needs SRCCOPY and a real blitter
     * behind it (doc 19 §21). */
    hi->ddCaps.dwCaps = DDCAPS_GDI | DDCAPS_BLTQUEUE;
    /* Never DDCAPS2_CERTIFIED (doc 19 §21). "Certified" is something the
     * runtime grants, not something a driver claims, and DirectDraw's
     * HALINFO validator refuses a driver that claims it (`testb
     * $1,0x68(%ebx); jne fail` on `ddCaps.dwCaps2`). The refusal is
     * invisible from here. The 16-bit `DDHAL_SetInfo` stores the HALINFO
     * and returns TRUE, and the 32-bit half throws the driver object away
     * afterwards. The driver sees "DirectDraw took the HAL" and every
     * application sees dwCaps DDCAPS_NOHARDWARE, no video memory and no
     * callback ever entered. `D9F_CERTIFIED` puts it back as a repro.
     *
     * DDCAPS2_WIDESURFACES, as on NT. Without it 9x DirectDraw puts no
     * surface wider than the primary in video memory, so at 640x480 every
     * 1024-wide texture stayed in system memory and d3d8.dll bound nothing
     * in its place: 3DMark2001 SE's Nature sky drawn white (doc 19 §37). */
    hi->ddCaps.dwCaps2 = DDCAPS2_WIDESURFACES | ((DDF() & D9F_CERTIFIED) ? DDCAPS2_CERTIFIED : 0);
    hi->ddCaps.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE |
                                DDSCAPS_FLIP | DDSCAPS_VIDEOMEMORY;
    /* The size of the heap above, not of the aperture. A total that counts
     * the command window is memory the driver cannot hand out. */
    hi->ddCaps.dwVidMemTotal = end - start;
    hi->ddCaps.dwVidMemFree = end - start;

    hi->dwMonitorFrequency = 0;
    /* DDHALINFO_ISPRIMARYDISPLAY, as the reference driver sets. It becomes
     * DDRAWI_DISPLAYDRV on the runtime's side. It is set because it is
     * true. Removing it changes nothing (doc 19 §21). */
    /* Not DDHALINFO_MODEXILLEGAL. The runtime's Mode X and VGA mode 13h
     * run on the adapter's VGA core after GDI disables this driver
     * (VDD_DISPLAY_DRIVER_DISABLING, then the runtime programs the VGA
     * registers itself), and they are what a 320x200 game gets (see the
     * mode table). The reference driver leaves it clear too. */
    hi->dwFlags = DDHALINFO_ISPRIMARYDISPLAY;
    /* the module DirectDraw loaded our 32-bit callbacks out of, and the
     * driver's own PDEVICE */
    hi->hInstance = pHal->dll_hinstance;
    hi->lpPDevice = (LPVOID)lpDriverPDevice;

    if (HalReachable(pHal->cb32.GetDriverInfo)) {
        *(DWORD __far *)&hi->GetDriverInfo = pHal->cb32.GetDriverInfo;
        hi->dwFlags |= DDHALINFO_GETDRIVERINFOSET | DDHALINFO_GETDRIVERINFO2;
    }
    hi->lpDDExeBufCallbacks = (LPDDHAL_DDEXEBUFCALLBACKS)HALFIELD(DDHAL_DDEXEBUFCALLBACKS_t, cb_exebuf);

    /* The Direct3D half, all of it or none. None when the DLL published no
     * D3D (a host with no executor, `no-exec=on`, doc 19 §40), where this
     * driver is DirectDraw only and the guest's WineD3D does Direct3D. The
     * FourCC list belongs to it: those are the texture formats the
     * executor decodes, and the NT driver offers them only with Direct3D
     * too (`p->core.d3d ? 6 : 0`). */
    if (HalReachable(pHal->d3dhal_global) && HalReachable(pHal->d3dhal_callbacks)) {
        pHal->fourcc[0] = 0x31545844;      /* 'DXT1' */
        pHal->fourcc[1] = 0x33545844;      /* 'DXT3' */
        pHal->fourcc[2] = 0x35545844;      /* 'DXT5' */
        pHal->fourcc[3] = 0x32545844;      /* 'DXT2' */
        pHal->fourcc[4] = 0x34545844;      /* 'DXT4' */
        pHal->fourcc[5] = 63;              /* D3DFMT_Q8W8V8U8: d3d8.dll creates it as this FOURCC (core_caps.c) */
        hi->lpdwFourCC = (LPDWORD)HALFIELD(DWORD, fourcc);
        hi->ddCaps.dwNumFourCCCodes = 6;

        *(DWORD __far *)&hi->lpD3DGlobalDriverData = pHal->d3dhal_global;
        *(DWORD __far *)&hi->lpD3DHALCallbacks = pHal->d3dhal_callbacks;
        hi->ddCaps.dwCaps |= DDCAPS_3D | DDCAPS_COLORKEY;
        hi->ddCaps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        hi->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
        hi->ddCaps.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    }

    dbg_val("d3dpt9dd:   d3d global", *(DWORD __far *)&hi->lpD3DGlobalDriverData);
    dbg_val(" cb", *(DWORD __far *)&hi->lpD3DHALCallbacks);
    dbg_val(" exebuf", (DWORD)hi->lpDDExeBufCallbacks);
    dbg_val(" fourcc", (DWORD)hi->lpdwFourCC);
    dbg_str("");

    dbg_val("d3dpt9dd:   ddflags", DDF());
    dbg_str("");
    dbg_val("d3dpt9dd:   halinfo flags", hi->dwFlags);
    dbg_val(" caps", hi->ddCaps.dwCaps);
    dbg_str("");
    dbg_val("d3dpt9dd:   heap", hp->fpStart);
    dbg_val("..", hp->fpEnd);
    dbg_str("");
    return TRUE;
}

/* --------------------------------------------------- the four escapes */

/* DDGET32BITDRIVERNAME: the DLL DirectDraw should load into the calling
 * process, the entry point to call in it, and the context value that
 * entry point is given (our shared block's linear address). */
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

/* DDNEWCALLBACKFNS: DirectDraw's own function table. We keep only
 * lpSetInfo, which is how a HAL is handed over. */
BOOL DDNewCallbackFns(DCICMD_t __far *lpCmd)
{
    LPDDHALDDRAWFNS pfns = (LPDDHALDDRAWFNS)lpCmd->dwParam1;

    /* An empty table means DirectDraw is taking its entry point back. We
     * keep a far pointer into DDRAW16, which is loaded only while some
     * process has DirectDraw open. The next mode set calls it again from
     * `Enable` (DDCreateDriverObject(1)), so a pointer kept past the escape
     * that withdrew it is a general protection fault in whatever program
     * changed the mode. */
    if (pfns == 0) {
        dbg_str("d3dpt9dd: DirectDraw withdrew its callbacks");
        lpSetInfo = 0;
        return FALSE;
    }
    lpSetInfo = pfns->lpSetInfo;
    return TRUE;
}

/* DDVERSIONINFO */
void DDGetVersion(DDVERSIONDATA_t __far *lpVer)
{
    ZeroFar(lpVer, sizeof(DDVERSIONDATA_t));
    /* The reference driver's answer. DirectX 6.1 takes 0x700 as well as
     * 0x100 (both measured). */
    lpVer->dwHALVersion = DD_RUNTIME_VERSION;
}

/* DDCREATEDRIVEROBJECT: build the HALINFO and hand it over. DirectDraw
 * loads the DLL and runs its DriverInit, which fills cb32, before this
 * escape, so the callbacks are copied here rather than when the name was
 * asked for. */
BOOL DDCreateDriverObject(BOOL bReset)
{
    if (lpSetInfo != 0 && !SelOk(SEL_OF(lpSetInfo))) {
        dbg_val("d3dpt9dd: DirectDraw's entry point is gone, selector", (DWORD)SEL_OF(lpSetInfo));
        dbg_str("");
        lpSetInfo = 0;
    }
    if (lpSetInfo == 0) {
        if (bReset) {
            if (pHal != 0) HalMode();
            return TRUE;
        }
        dbg_str("d3dpt9dd: no lpSetInfo");
        return FALSE;
    }
    if (!HalBlock()) {
        return FALSE;
    }
    HalMode();
    if (!pHal->dll_ready) {
        /* Not fatal. DirectDraw still gets a HAL with no callbacks and
         * does everything in its own HEL. Logged, because it means the
         * DLL was not found or refused the block. */
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
    /* Log the runtime's answer. It is the only place it says whether it
     * took the HAL, and a HAL it did not take looks exactly like one that
     * was never offered. */
    if (!lpSetInfo(HALFIELD(DDHALINFO_t, halinfo), bReset)) {
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

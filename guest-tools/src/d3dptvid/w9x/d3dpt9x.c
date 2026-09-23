/*
 * d3dpt9x.c: the Windows 98/Me display driver for the d3dpt-vga adapter
 * (doc 19, ADR-012 / M10). The 16-bit half, a DIB Engine mini display
 * driver running in ring 3, built with Open Watcom.
 *
 * This is the 9x counterpart of the XP driver's "framebuf" shape (doc 15,
 * M7a). On NT the miniport enumerates modes and win32k loads a kernel DLL
 * that hands GDI an engine bitmap over VRAM. On 9x this file is a 16-bit
 * NE module GDI loads as DISPLAY, and every drawing export jumps straight
 * to the DIB Engine (dibthunk.asm). The driver itself gets the adapter
 * from the mini-VDD, programs the mode registers and describes the frame
 * buffer to the DIB Engine, so GDI draws directly into guest VRAM with no
 * copy.
 *
 * This half cannot work alone (doc 19 §11). Windows unmaps the adapter's
 * PCI base addresses within half a minute of boot if nothing claimed its
 * resources. On 9x the ring-0 mini-VDD claims the device and hands the
 * frame buffer to this driver.
 *
 * The pointer is the adapter's own cursor sprite rather than the DIB
 * Engine's software one, because an Engine pointer lives in the frame
 * buffer and every full-screen DirectDraw title writes over it (see
 * "hardware cursor" below). The mode list comes from the INF rather than
 * from the adapter (doc 19 §6).
 *
 * Debug output goes to the adapter's DEBUG register and so into the QEMU
 * log, as the XP driver's does. There is no COM port and no debugger.
 *
 * Build: guest-tools/build-driver9x.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* `SetCursor` names two different functions. The Win16 API (win16.h) takes
 * an HCURSOR and returns the previous one. The display driver entry this
 * file exports at ordinal 102 takes a CURSORSHAPE and returns nothing, and
 * it is the one GDI calls. So the API's declaration is hidden while the
 * headers go by, the same trick as `ValidateMode` below. */
#define SetCursor SetCursor_the_win16_api
#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <minivdd.h>
/* valmode.h declares ValidateMode without `__loadds`, and Open Watcom takes
 * a function's attributes from its first declaration. The definition below
 * would then compile without the DS prologue every other export has, and
 * its read of `wRegsSel` would go through whatever DS the display applet's
 * thunk left. That garbage becomes a selector, and Display Settings dies in
 * a GPF when its Settings tab asks about the first mode (doc 19 §18). Hiding
 * the header's prototype makes ours the first declaration. */
#define ValidateMode ValidateMode_as_the_ddk_declares_it
#include <valmode.h>
#undef ValidateMode

#include <ddrawi.h>
#undef SetCursor

#include "d3dpt9x.h"
#include "d3dpt9v.h"
#include "../../../../d3dpt/d3dpt_fb.h"
#include "../d3dpt_esc.h"

/* Pretend we have a 208 by 156 mm screen, as every driver of the era does. */
#define DISPLAY_HORZ_MM     208
#define DISPLAY_VERT_MM     156
#define DISPLAY_SIZE_EN     325
#define DISPLAY_SIZE_TWP    2340

WORD  wScrX = 640, wScrY = 480, wBpp = 32, wDpi = 96, wPalettized = 0;
WORD  OurVMHandle = 0;
DWORD VDDEntryPoint = 0;

DWORD dwVramSize = 0;
DWORD dwVramLin = 0, dwRegsLin = 0;     /* what the ring-3 HAL needs (d3dpt9v.h) */
WORD  wRegsSel = 0, wVramSel = 0;
DWORD dwPitch = 0;

static WORD wCursorHW;                  /* set by AdapterFind; see below */
void CursorHide(void);

LPDIBENGINE lpDriverPDevice = 0;
WORD wEnabled = 0;
RGBQUAD FAR *lpColorTable = 0;

static BYTE bReEnabling = 0;
static WORD wDDLines = 0;       /* the first DCICOMMAND escapes, logged */
static WORD wDIBPdevSize = 0;

/* ------------------------------------------------------- no CRT, no helpers */

/* The driver links no C runtime (the XP one uses kcrt.c for the same), so
 * the two helpers the compiler would otherwise pull in come from here: a
 * 16x16 -> 32 bit multiply (MulW in d3dpt9x.h, which keeps __U4M out of
 * the object) and a far memset. */
void ZeroFar(void __far *p, WORD n)
{
    BYTE __far *q = p;
    while (n--) *q++ = 0;
}

/* ------------------------------------------------------------ DPMI, ports */

/* The mini-VDD claims the device, maps VRAM and the register page, and
 * hands us selectors onto both. Doing it here does not work, because
 * Windows takes the base addresses away from a device nothing claimed
 * (doc 19 §11). */
static WORD CallVDD_Register(void);
#pragma aux CallVDD_Register =      \
    ".386"                          \
    "mov    eax, 83h"               \
    "movzx  ebx, word ptr [OurVMHandle]" \
    "call   dword ptr [VDDEntryPoint]"   \
    "jc     vdd_fail"               \
    "mov    word ptr [wRegsSel], ax"     \
    "mov    word ptr [wVramSel], dx"     \
    "mov    dword ptr [dwVramSize], ecx" \
    "mov    dword ptr [dwVramLin], esi"  \
    "mov    dword ptr [dwRegsLin], edi"  \
    "mov    ax, 1"                  \
    "jmp    vdd_done"               \
    "vdd_fail:"                     \
    "xor    ax, ax"                 \
    "vdd_done:"                     \
    value [ax] modify [bx cx dx si di];

/* The main VDD's own entries, which every 9x display driver calls around a
 * mode change. They go to the VDD itself, not to our mini-VDD (which hooks
 * only REGISTER_DISPLAY_DRIVER), because the VDD is what has to be told
 * that this VM is no longer showing VGA. */
static void CallVDD_Simple(WORD fn);
#pragma aux CallVDD_Simple =        \
    ".386"                          \
    "push   eax"                    \
    "push   ebx"                    \
    "movzx  eax, ax"                \
    "movzx  ebx, word ptr [OurVMHandle]" \
    "call   dword ptr [VDDEntryPoint]"   \
    "pop    ebx"                    \
    "pop    eax"                    \
    parm [ax] modify [cx dx si di];

/* VDD_DRIVER_REGISTER wants the size of the frame buffer in use (pitch
 * times height, in ECX) and a far pointer to the routine the VDD calls to
 * put the desktop mode back after a full-screen DOS box. */
static DWORD CallVDD_DriverRegister(WORD fn, WORD pitch, WORD height,
                                    void __far *restore);
#pragma aux CallVDD_DriverRegister =\
    ".386"                          \
    "movzx  eax, ax"                \
    "movzx  edx, dx"                \
    "mul    edx"                    \
    "mov    ecx, eax"               \
    "movzx  eax, bx"                \
    "movzx  ebx, word ptr [OurVMHandle]" \
    "xor    edx, edx"               \
    "call   dword ptr [VDDEntryPoint]"   \
    "mov    edx, eax"               \
    "shr    edx, 16"                \
    parm [bx] [ax] [dx] [es di] modify [bx cx];

/* ------------------------------------------------------------ the adapter */

/* The adapter's registers are 32 bits wide and the device accepts nothing
 * else (its register BAR is `valid.min_access_size = 4`). A 16-bit compiler
 * turns `*(DWORD __far *)p` into two word accesses, which QEMU answers with
 * zero while silently dropping the writes (doc 19 §14). So both directions
 * are one 32-bit access, written out by hand, and the module is .386
 * throughout. The XP driver is 32-bit code and never had this problem. */
static DWORD RegGetSel(WORD sel, WORD off);
#pragma aux RegGetSel =     \
    ".386"                  \
    "push   es"             \
    "mov    es, cx"         \
    "mov    eax, es:[bx]"   \
    "mov    edx, eax"       \
    "shr    edx, 16"        \
    "pop    es"             \
    parm [cx] [bx] value [dx ax] modify [ax dx];

static void RegPutSel(WORD sel, WORD off, DWORD val);
#pragma aux RegPutSel =     \
    ".386"                  \
    "push   es"             \
    "mov    es, cx"         \
    "shl    edx, 16"        \
    "mov    dx, ax"         \
    "mov    es:[bx], edx"   \
    "pop    es"             \
    parm [cx] [bx] [dx ax] modify [dx];

DWORD RegGet(WORD off)
{
    return RegGetSel(wRegsSel, off);
}

void RegPut(WORD off, DWORD val)
{
    RegPutSel(wRegsSel, off, val);
}

/* Two debug channels. The adapter's DEBUG register reaches the QEMU log as
 * the XP driver's does (doc 15), but only once the register page is mapped,
 * and the early failures happen before that. So every line also goes to
 * port 0xE9, QEMU's debug console (`-debugcon file:...`), which works from
 * the first instruction. */
static void OutE9(BYTE c);
#pragma aux OutE9 = "out 0E9h, al" parm [al];

static void dbg_ch(BYTE c)
{
    OutE9(c);
    if (wRegsSel) RegPut(D3DPT_FB_REG_DEBUG, (DWORD)c);
}

void dbg_str(const char *s)
{
    while (*s) dbg_ch((BYTE)*s++);
    dbg_ch('\n');
}

void dbg_val(const char *tag, DWORD v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    while (*tag) dbg_ch((BYTE)*tag++);
    dbg_ch('=');
    for (i = 28; i >= 0; i -= 4) dbg_ch((BYTE)hex[(v >> i) & 0xf]);
    dbg_ch('\n');
}

/* Ask the mini-VDD for the adapter. Everything the driver knows about the
 * hardware arrives here. */
BOOL AdapterFind(void)
{
    if (wRegsSel) return TRUE;          /* already asked */
    if (!VDDEntryPoint) { dbg_str("d3dpt9x: no VDD"); return FALSE; }

    /* Carry means no, and so does a zero selector. The main VDD answers this
     * function itself when no mini-VDD claimed it, and then the registers
     * hold whatever its own dispatch left in them. */
    if (!CallVDD_Register() || !wRegsSel || !wVramSel || !dwVramSize) {
        dbg_str("d3dpt9x: the mini-VDD has no adapter for us");
        wRegsSel = wVramSel = 0;
        return FALSE;
    }
    dbg_val("d3dpt9x: regs sel", wRegsSel);
    dbg_val("d3dpt9x: vram sel", wVramSel);
    dbg_val("d3dpt9x: vram", dwVramSize);
    dbg_val("d3dpt9x: regs lin", dwRegsLin);
    dbg_val("d3dpt9x: vram lin", dwVramLin);

    /* A newer register set only adds registers (d3dpt_fb.h), so it is
     * accepted. Only an older one, missing registers this build uses, is
     * refused. Exact matching made a v4 driver on a v5 adapter die at boot
     * with "Windows protection error". */
    if (RegGet(D3DPT_FB_REG_MAGIC) != D3DPT_FB_MAGIC ||
        RegGet(D3DPT_FB_REG_VERSION) < D3DPT_FB_VERSION) {
        dbg_val("d3dpt9x: magic", RegGet(D3DPT_FB_REG_MAGIC));
        dbg_val("d3dpt9x: version", RegGet(D3DPT_FB_REG_VERSION));
        dbg_str("d3dpt9x: not our adapter, or a register set older than this driver: refusing it");
        wRegsSel = wVramSel = 0;
        return FALSE;
    }
    /* The v4 cursor sprite, if this adapter has one. With it the pointer
     * never enters the frame buffer (see "hardware cursor" below). */
    wCursorHW = (RegGet(D3DPT_FB_REG_CAPS) & D3DPT_FB_CAP_CURSOR) ? 1 : 0;
    dbg_val("d3dpt9x: hardware cursor", wCursorHW);
    dbg_str("d3dpt9x: adapter found");
    return TRUE;
}

/* Is this a mode the adapter can show? */
static BOOL ModeOk(WORD x, WORD y, WORD bpp)
{
    DWORD caps, need;

    if (!wRegsSel) return FALSE;
    caps = RegGet(D3DPT_FB_REG_CAPS);
    if (bpp == 8 && !(caps & D3DPT_FB_CAP_BPP8)) return FALSE;
    if (bpp == 16 && !(caps & D3DPT_FB_CAP_BPP16)) return FALSE;
    if (bpp == 32 && !(caps & D3DPT_FB_CAP_BPP32)) return FALSE;
    if (bpp != 8 && bpp != 16 && bpp != 32) return FALSE;
    if (x < 320 || y < 200) return FALSE;

    need = MulW((WORD)D3DPT9X_PITCH(x, bpp), y);
    return need <= dwVramSize;
}

/* Paint the visible frame buffer black. GDI paints the desktop over it at
 * once, but VRAM at power-on holds whatever the device left there, and a
 * driver that does not clear shows it as a band of garbage above a desktop
 * that has not arrived yet. Written 32 bits at a time through
 * the VRAM selector, the same hand-written access the registers need. */
/* Not `rep stosd`. In a 16-bit code segment a string instruction indexes
 * with DI and counts with CX whatever the operand size, so the clear would
 * stop after 64 KB, in the first 25 lines of the screen. An explicit 32-bit
 * base register makes the assembler emit the address-size prefix the whole
 * frame buffer needs. */
static void ClearScreen(WORD sel, DWORD bytes);
#pragma aux ClearScreen =           \
    ".386"                          \
    "push   es"                     \
    "shl    edx, 16"                \
    "mov    dx, ax"                 \
    "shr    edx, 2"                 \
    "mov    ecx, edx"               \
    "mov    es, bx"                 \
    "xor    edx, edx"               \
    "xor    eax, eax"               \
    "jecxz  clr_done"               \
    "clr_next:"                     \
    "mov    es:[edx], eax"          \
    "add    edx, 4"                 \
    "dec    ecx"                    \
    "jnz    clr_next"               \
    "clr_done:"                     \
    "pop    es"                     \
    parm [bx] [dx ax] modify [ax bx cx dx];

/* The VDD calls this one back, so DS is not ours on entry. */
void __far RestoreDesktopMode(void);
#pragma aux RestoreDesktopMode loadds;

int PhysicalEnable(void)
{
    if (!AdapterFind()) return 0;
    if (!ModeOk(wScrX, wScrY, wBpp)) {
        dbg_str("d3dpt9x: refused mode");
        return 0;
    }

    dwPitch = D3DPT9X_PITCH(wScrX, wBpp);

    /* The VDD virtualises the VGA for every VM and has to be told before
     * and after the hardware stops being one. */
    if (VDDEntryPoint) CallVDD_Simple(VDD_PRE_MODE_CHANGE);

    RegPut(D3DPT_FB_REG_ENABLE, 0);
    RegPut(D3DPT_FB_REG_WIDTH,  wScrX);
    RegPut(D3DPT_FB_REG_HEIGHT, wScrY);
    RegPut(D3DPT_FB_REG_BPP,    wBpp);
    RegPut(D3DPT_FB_REG_PITCH,  dwPitch);
    RegPut(D3DPT_FB_REG_OFFSET, 0);
    RegPut(D3DPT_FB_REG_ENABLE, 1);

    if (!RegGet(D3DPT_FB_REG_ENABLE)) {
        dbg_str("d3dpt9x: the device refused the mode");
        return 0;
    }
    dbg_val("d3dpt9x: mode w", wScrX);
    dbg_val("d3dpt9x: mode h", wScrY);
    dbg_val("d3dpt9x: mode bpp", wBpp);

    /* The VDD has to know a hi-res driver owns the screen. Until this call
     * it believes the VM still shows the VGA it virtualises, and it runs the
     * screen switch, the DOS box and USER's repaint path on that footing.
     * `RestoreDesktopMode` is how it asks for the mode back afterwards. The
     * reference driver does not treat a failure here as fatal, so it is only
     * logged. */
    if (VDDEntryPoint) {
        DWORD rc = CallVDD_DriverRegister(VDD_DRIVER_REGISTER, (WORD)dwPitch,
                                          wScrY, RestoreDesktopMode);
        if (rc == VDD_DRIVER_REGISTER) dbg_str("d3dpt9x: VDD declined the register");
        else                           dbg_val("d3dpt9x: VDD vram used", rc);

        CallVDD_Simple(VDD_POST_MODE_CHANGE);
        CallVDD_Simple(VDD_SAVE_DRIVER_STATE);
    }

    if (wVramSel) ClearScreen(wVramSel, MulW(wScrY, (WORD)dwPitch));
    return 1;
}

/* The VDD calls this to put the desktop back after a full-screen DOS box.
 * It programs the mode registers again, with none of the one-time work. */
void __far RestoreDesktopMode(void)
{
    dbg_str("d3dpt9x: RestoreDesktopMode");
    /* the Engine may draw again (SwitchToBgnd set this) */
    if (lpDriverPDevice) lpDriverPDevice->deFlags &= ~BUSY;
    if (!wRegsSel) return;
    RegPut(D3DPT_FB_REG_ENABLE, 0);
    RegPut(D3DPT_FB_REG_WIDTH,  wScrX);
    RegPut(D3DPT_FB_REG_HEIGHT, wScrY);
    RegPut(D3DPT_FB_REG_BPP,    wBpp);
    RegPut(D3DPT_FB_REG_PITCH,  dwPitch);
    RegPut(D3DPT_FB_REG_OFFSET, 0);
    RegPut(D3DPT_FB_REG_ENABLE, 1);
}

/* ------------------------------------------------------- the screen switch
 *
 * When a DOS box closes, nobody repaints the desktop unless the display
 * driver asks (doc 19 §29). The four mini-VDD calls and `RestoreDesktopMode`
 * put the adapter back (doc 19 §26). The desktop comes back through an
 * older channel. The VDD raises INT 2Fh AX=4001h in the Windows VM when
 * the screen is about to be taken away and AX=4002h when it is back, and
 * every 9x display driver hooks that vector (the DDK's sample and vmdisp9x
 * included). On the way out the hook marks the DIB Engine's PDEVICE BUSY,
 * so GDI stops writing into a frame buffer that is now the DOS program's
 * VGA memory (on this adapter they are the same bytes from offset 0). On
 * the way back it restores the mode if it was lost and calls USER's
 * undocumented screen-repaint entry, ordinal 275, which is where every
 * window's WM_PAINT after a full-screen session comes from.
 *
 * Without the hook the return from Blood looked like a hang. The linear
 * mode came back, but the screen showed Blood's last frame tiled across an
 * 800x600x16 desktop indefinitely, with Windows idle behind it and nothing
 * asking to redraw.
 *
 * The handler is in dibthunk.asm. It calls these with DS already DGROUP,
 * on whatever stack the VDD's notification arrived on. */
extern void __far __cdecl SWHook(void);
extern void __cdecl SetOldInt2Fh(WORD alias, void __far *vec);
extern void __far * __cdecl GetOldInt2Fh(void);
UINT WINAPI AllocCStoDSAlias(UINT selCode);     /* KERNEL.170; imported in the .lnk */

static void __far *DOSGetIntVec(BYTE n);
#pragma aux DOSGetIntVec =  \
    "mov    ah, 35h"        \
    "int    21h"            \
    parm [al] value [es bx];

static void DOSSetIntVec(BYTE n, void __far *v);
#pragma aux DOSSetIntVec =  \
    "mov    ah, 25h"        \
    "push   ds"             \
    "push   es"             \
    "pop    ds"             \
    "int    21h"            \
    "pop    ds"             \
    parm [al] [es dx];

typedef void (WINAPI *REPAINTPROC)(void);
#define USER_REPAINT_ORDINAL 275

static REPAINTPROC RepaintFunc = 0;
static WORD wInt2FHooked = 0;
static WORD wNoRepaint = 0;         /* USER asked, through UserRepaintDisable */
static WORD wPaintPending = 0;      /* a repaint that arrived while it had */

static void RepaintScreen(void)
{
    if (!RepaintFunc) return;
    if (wNoRepaint) wPaintPending = 1;
    else            RepaintFunc();
}

/* The linear mode goes off here too, for the switch nobody announces
 * (doc 19 §41). The mini-VDD turns it off at `PRE_HIRES_TO_VGA` and at
 * `DISPLAY_DRIVER_DISABLING`, which covers a DOS box, a Mode X game and the
 * shutdown. Windows' own monitor power-down sends only this notification.
 * With ENABLE still on, the adapter kept scanning out the desktop's last
 * frame while the VDD wrote VGA planes at VRAM offset 0, the top of the
 * frame buffer. The result was a frozen desktop with a coloured band across
 * the first 20 rows, indistinguishable from a hang until a keypress. The
 * way back is unchanged: `SwitchToFgnd` calls `RestoreDesktopMode`, which
 * programs the mode and ENABLE again. In the announced switches the
 * mini-VDD has already turned the mode off, so this write is a no-op.
 *
 * Measured with `PWRPROBE.EXE`, which requests the same power-down the idle
 * time-out does. Without this write the linear mode stays on for the whole
 * blank (`switched out` at 13:19:28.83, `linear mode off` only at the
 * restore, 13:19:34.50). With it the mode is gone 69 µs after the
 * notification. */
void __cdecl SwitchToBgnd(void)
{
    dbg_str("d3dpt9x: switched out");
    if (lpDriverPDevice) lpDriverPDevice->deFlags |= BUSY;
    if (wRegsSel) RegPut(D3DPT_FB_REG_ENABLE, 0);
}

void __cdecl SwitchToFgnd(void)
{
    dbg_str("d3dpt9x: switched in");
    if (lpDriverPDevice && (lpDriverPDevice->deFlags & BUSY)) RestoreDesktopMode();
    RepaintScreen();
}

static void HookInt2Fh(void)
{
    WORD alias;

    if (wInt2FHooked) return;
    if (!RepaintFunc)
        RepaintFunc = (REPAINTPROC)GetProcAddress(GetModuleHandle("USER"),
                                                  MAKEINTRESOURCE(USER_REPAINT_ORDINAL));
    if (!RepaintFunc) dbg_str("d3dpt9x: USER has no repaint entry");

    /* the saved vector is in our code segment: write it through an alias */
    alias = AllocCStoDSAlias((WORD)((DWORD)(void __far *)SWHook >> 16));
    if (!alias) { dbg_str("d3dpt9x: no alias for the code segment"); return; }
    SetOldInt2Fh(alias, DOSGetIntVec(0x2f));
    FreeSelector(alias);
    DOSSetIntVec(0x2f, (void __far *)SWHook);
    wInt2FHooked = 1;
    dbg_str("d3dpt9x: screen-switch hook in");
}

static void UnhookInt2Fh(void)
{
    if (!wInt2FHooked) return;
    DOSSetIntVec(0x2f, GetOldInt2Fh());
    wInt2FHooked = 0;
}

void PhysicalDisable(void)
{
    /* The device composites the sprite outside the frame buffer, so it
     * would otherwise keep hovering over whatever comes next: a VGA text
     * screen, a fatal-exception message, the shutdown. */
    CursorHide();
    /* The linear mode goes off before the VDD is told (doc 19 §35).
     * Unregistering is when the VDD starts putting the VGA back, with its
     * planes at VRAM offset 0, the top of our frame buffer. With ENABLE
     * still on, a display refresh in those few ms took them into the last
     * linear frame, which the adapter then held, and Windows' shutdown came
     * up as the desktop with a green band across the top. The mini-VDD turns
     * the mode off at DISPLAY_DRIVER_DISABLING too; this keeps the same
     * order for a VDD that does not send it. */
    if (wRegsSel) RegPut(D3DPT_FB_REG_ENABLE, 0);
    if (VDDEntryPoint) CallVDD_Simple(VDD_DRIVER_UNREGISTER);
}

/* ------------------------------------------------------- the display config */

/* The main VDD answers VDD_GET_DISPLAY_CONFIG with the mode the registry
 * holds; SYSTEM.INI is the fallback, as on every 9x display driver. */
static DWORD CallVDD(WORD fn, WORD size, LPVOID p);
#pragma aux CallVDD =               \
    ".386"                          \
    "movzx  eax, ax"                \
    "movzx  ecx, cx"                \
    "movzx  ebx, OurVMHandle"       \
    "movzx  edi, di"                \
    "call   dword ptr VDDEntryPoint"\
    "mov    edx, eax"               \
    "shr    edx, 16"                \
    parm [ax] [cx] [es di] modify [bx];

void ReadDisplayConfig(void)
{
    DISPLAYINFO di;
    WORD x, y, bpp;
    DWORD rc;

    wDpi = GetPrivateProfileInt("display", "dpi", 96, "system.ini");
    x    = GetPrivateProfileInt("display", "x_resolution", 0, "system.ini");
    y    = GetPrivateProfileInt("display", "y_resolution", 0, "system.ini");
    bpp  = GetPrivateProfileInt("display", "bpp", 0, "system.ini");

    if (VDDEntryPoint) {
        rc = CallVDD(VDD_GET_DISPLAY_CONFIG, sizeof(di), &di);
        if (rc != VDD_GET_DISPLAY_CONFIG && !rc) {
            x = di.diXRes;
            y = di.diYRes;
            bpp = di.diBpp;
            wDpi = di.diDPI ? di.diDPI : wDpi;
        }
    }

    if (x && y) { wScrX = x; wScrY = y; }
    if (bpp)    wBpp = bpp;
    if (wBpp != 8 && wBpp != 16 && wBpp != 32) wBpp = 32;
    wPalettized = (wBpp == 8) ? 1 : 0;
}

/* ------------------------------------------------------- the 8 bpp default
 *
 * Windows' own 256-colour palette: the twenty static system colours at the
 * two ends of the table, a 6-6-6 colour cube in the middle and a grey ramp
 * in what is left. An 8 bpp mode shows it between coming up and the first
 * palette GDI realises, and `SetPalette` replaces it entry by entry from
 * then on. Without it the palette would be whatever bytes were already
 * there, not black. */
static const BYTE bSysColours[20][3] = {     /* r, g, b */
    { 0x00, 0x00, 0x00 }, { 0x80, 0x00, 0x00 }, { 0x00, 0x80, 0x00 },
    { 0x80, 0x80, 0x00 }, { 0x00, 0x00, 0x80 }, { 0x80, 0x00, 0x80 },
    { 0x00, 0x80, 0x80 }, { 0xc0, 0xc0, 0xc0 }, { 0xc0, 0xdc, 0xc0 },
    { 0xa6, 0xca, 0xf0 },
    { 0xff, 0xfb, 0xf0 }, { 0xa0, 0xa0, 0xa4 }, { 0x80, 0x80, 0x80 },
    { 0xff, 0x00, 0x00 }, { 0x00, 0xff, 0x00 }, { 0xff, 0xff, 0x00 },
    { 0x00, 0x00, 0xff }, { 0xff, 0x00, 0xff }, { 0x00, 0xff, 0xff },
    { 0xff, 0xff, 0xff }
};

static void DefaultColourTable(RGBQUAD FAR *ct)
{
    static const BYTE bCube[6] = { 0x00, 0x33, 0x66, 0x99, 0xcc, 0xff };
    WORD i, r, g, b, n;

    for (i = 0; i < 10; i++) {
        ct[i].rgbRed           = bSysColours[i][0];
        ct[i].rgbGreen         = bSysColours[i][1];
        ct[i].rgbBlue          = bSysColours[i][2];
        ct[i].rgbReserved      = 0;
        ct[246 + i].rgbRed     = bSysColours[10 + i][0];
        ct[246 + i].rgbGreen   = bSysColours[10 + i][1];
        ct[246 + i].rgbBlue    = bSysColours[10 + i][2];
        ct[246 + i].rgbReserved = 0;
    }
    n = 10;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                ct[n].rgbRed      = bCube[r];
                ct[n].rgbGreen    = bCube[g];
                ct[n].rgbBlue     = bCube[b];
                ct[n].rgbReserved = 0;
                n++;
            }
        }
    }
    for (; n < 246; n++) {              /* the twenty spare: a grey ramp */
        BYTE v = (BYTE)((n - 226) * 12 + 8);
        ct[n].rgbRed = ct[n].rgbGreen = ct[n].rgbBlue = v;
        ct[n].rgbReserved = 0;
    }
}

/* ------------------------------------------------------ hardware cursor
 *
 * The pointer must not live in the frame buffer (doc 19 §26). The DIB
 * Engine draws its cursor into VRAM and keeps the pixels it covered in a
 * save-under, and the `BeginAccess` / `EndAccess` pair below lifts it out
 * of the way before GDI writes there. DirectDraw does not go through GDI.
 * A game that locks the primary, blits to it or flips a chain writes the
 * frame buffer with the Engine's cursor still in it, the save-under goes
 * stale, and the next mouse move stamps that stale block back onto the
 * screen. The DirectDraw HAL cannot prevent it, because it is a flat 32-bit
 * DLL and the Engine's exclusion pair is 16-bit code it cannot call.
 *
 * So the pointer leaves the frame buffer, as on the XP driver (doc 15, "The
 * hardware cursor", register set v4). The shape is converted to a8r8g8b8 in
 * the VRAM the DirectDraw heap stops short of, and the device hands it to
 * the host as a cursor sprite. Nothing composites it into the frame, so
 * neither GDI, DirectDraw nor a mode change can corrupt it.
 *
 * Two consequences. A screendump shows no pointer, as on XP, because there
 * is none in the frame buffer. And on an adapter with no v4 cursor
 * (`D3DPT_FB_CAP_CURSOR` clear) all three entries below fall back to the
 * Engine's software pointer, which is why dibeng's `...CursorExt` entries
 * are still imported.
 */

/* What GDI hands `SetCursor` on 9x: a header, then `cy` rows of AND mask,
 * then `cy` rows of XOR mask, each row `cbWidth` bytes. Win16 `int` is 16
 * bits. Declared here because the headers in ../ddk9x carry only what the
 * DIB Engine needs. */
typedef struct {
    short xHotSpot, yHotSpot;
    short cx, cy;
    short cbWidth;
    BYTE  Planes, BitsPixel;
} D3DPT_CURSORSHAPE;

static WORD wCursorSet = 0;     /* the sprite has a shape and is shown */
static WORD wCursorSoft = 0;    /* this one shape went to the Engine instead */

/* One 32-bit write into VRAM at an offset that does not fit a WORD. The
 * sprite's image sits near the top of a 128 MB aperture, and in 16-bit code
 * a string instruction indexes with DI and a default operand is a word. So
 * the offset is an explicit 32-bit base register, the same hand-written
 * access as RegGetSel and ClearScreen above. */
static void VramPut(WORD sel, DWORD off, DWORD val);
#pragma aux VramPut =       \
    ".386"                  \
    "push   es"             \
    "mov    es, si"         \
    "shl    ecx, 16"        \
    "mov    cx, bx"         \
    "shl    edx, 16"        \
    "mov    dx, ax"         \
    "mov    es:[ecx], edx"  \
    "pop    es"             \
    parm [si] [cx bx] [dx ax] modify [cx dx];

/* Where the sprite's image goes: immediately below the command window,
 * where core/'s `cursor_offset()` puts it on NT and where the DirectDraw
 * heap this driver publishes stops. Read from the register, never derived.
 * The adapter is the one authority on its own layout, and deriving it made
 * the two layers drift apart twice (doc 19 §25, §26). */
static DWORD CursorOffset(void)
{
    DWORD end = wRegsSel ? RegGet(D3DPT_FB_REG_CMD_OFFSET) : 0;

    if (!end || end > dwVramSize) end = dwVramSize;
    return end - D3DPT_FB_CURSOR_BYTES;
}

void CursorHide(void)
{
    if (wCursorHW && wRegsSel) RegPut(D3DPT_FB_REG_CURSOR_ENABLE, 0);
    wCursorSet = 0;
}

/* Hand this shape to the Engine, taking the sprite off the screen first so
 * that only one pointer is ever drawn. */
static void CursorToEngine(LPVOID lpCursor)
{
    CursorHide();
    wCursorSoft = 1;
    DIB_SetCursorExt(lpCursor, lpDriverPDevice);
}

VOID WINAPI __loadds SetCursor(LPVOID lpCursor)
{
    D3DPT_CURSORSHAPE FAR *cs = lpCursor;
    BYTE FAR *bits;
    DWORD off;
    WORD x, y, w, h, stride;

    if (!wCursorHW) {
        DIB_SetCursorExt(lpCursor, lpDriverPDevice);
        return;
    }
    if (!cs) {                          /* no shape: the pointer goes away */
        if (wCursorSoft) {
            DIB_SetCursorExt(NULL, lpDriverPDevice);
            wCursorSoft = 0;
        }
        CursorHide();
        return;
    }

    w = (WORD)cs->cx;
    h = (WORD)cs->cy;
    stride = (WORD)cs->cbWidth;
    /* Monochrome only. `C1_COLORCURSOR` asks Windows for a colour pointer,
     * and this driver stops claiming it while the sprite is in use, so every
     * pointer Windows hands over is 1 bpp. A colour one would arrive in the
     * screen's own format and need a converter per bpp.
     *
     * Anything the sprite cannot carry goes to the Engine rather than being
     * dropped. A pointer that vanishes is worse than one that can be drawn
     * over, and this way there is always exactly one on the screen. */
    if (!w || !h || w > D3DPT_FB_CURSOR_MAX || h > D3DPT_FB_CURSOR_MAX ||
        cs->Planes != 1 || cs->BitsPixel != 1 || !stride) {
        CursorToEngine(lpCursor);
        return;
    }
    if (wCursorSoft) {                  /* the Engine had the last one */
        DIB_SetCursorExt(NULL, lpDriverPDevice);
        wCursorSoft = 0;
    }

    bits = (BYTE FAR *)(cs + 1);
    off = CursorOffset();
    for (y = 0; y < h; y++) {
        /* MulW, not `*`. There is no CRT, so a 32-bit multiply would need
         * __U4M and the link would fail. */
        BYTE FAR *arow = bits + MulW(y, stride);
        BYTE FAR *xrow = bits + MulW((WORD)(h + y), stride);
        for (x = 0; x < w; x++) {
            WORD a  = (WORD)((arow[x >> 3] >> (7 - (x & 7))) & 1);
            WORD xo = (WORD)((xrow[x >> 3] >> (7 - (x & 7))) & 1);
            /* AND 1 / XOR 0 is the screen showing through. AND 0 is black or
             * white by XOR. AND 1 / XOR 1 asks for the screen inverted, which
             * a sprite cannot do, so it is black, the same approximation as
             * on NT. */
            DWORD c = (a && !xo) ? 0ul : (xo && !a) ? 0xfffffffful : 0xff000000ul;
            VramPut(wVramSel, off + ((MulW(y, w) + x) << 2), c);
        }
    }

    RegPut(D3DPT_FB_REG_CURSOR_ADDR, off);
    RegPut(D3DPT_FB_REG_CURSOR_W, w);
    RegPut(D3DPT_FB_REG_CURSOR_H, h);
    RegPut(D3DPT_FB_REG_CURSOR_HOT_X, (DWORD)(WORD)cs->xHotSpot);
    RegPut(D3DPT_FB_REG_CURSOR_HOT_Y, (DWORD)(WORD)cs->yHotSpot);
    RegPut(D3DPT_FB_REG_CURSOR_DEFINE, 1);
    RegPut(D3DPT_FB_REG_CURSOR_ENABLE, 1);
    wCursorSet = 1;
}

VOID WINAPI __loadds MoveCursor(WORD absX, WORD absY)
{
    if (!wCursorHW || wCursorSoft) {
        DIB_MoveCursorExt(absX, absY, lpDriverPDevice);
        return;
    }
    if (!wCursorSet || !wRegsSel) return;
    RegPut(D3DPT_FB_REG_CURSOR_X, (DWORD)(long)(short)absX);
    RegPut(D3DPT_FB_REG_CURSOR_Y, (DWORD)(long)(short)absY);
    RegPut(D3DPT_FB_REG_CURSOR_ENABLE, 1);
}

VOID WINAPI __loadds CheckCursor(void)
{
    /* The Engine's software pointer has to be redrawn whenever something
     * has drawn over it. A sprite the device composites never has. */
    if (!wCursorHW || wCursorSoft) DIB_CheckCursorExt(lpDriverPDevice);
}

/* ------------------------------------------------------------ GDI: Enable */

/* GDI's software cursor lives in the DIB Engine. These are the
 * surface-access callbacks it uses to exclude the cursor while it draws. */
static VOID WINAPI __loadds BeginAccess(LPPDEVICE lpDevice, WORD l, WORD t,
                                        WORD r, WORD b, WORD flags)
{
    DIB_BeginAccess(lpDevice, l, t, r, b, flags);
}

static VOID WINAPI __loadds EndAccess(LPPDEVICE lpDevice, WORD flags)
{
    DIB_EndAccess(lpDevice, flags);
}

/* An undocumented USER callback: which font resource to use at this DPI. */
DWORD WINAPI __loadds GetDriverResourceID(WORD wResID, LPSTR lpResType)
{
    if (wResID == OBJ_FONT && wDpi != 96) return 2003;
    return wResID;
}

/* DISPLAY.500: USER says whether the driver may ask for repaints now. A
 * repaint that arrives while it may not is delivered when it may again. */
BOOL WINAPI __loadds UserRepaintDisable(BOOL bDisable)
{
    wNoRepaint = bDisable ? 1 : 0;
    if (!bDisable && wPaintPending) {
        wPaintPending = 0;
        RepaintScreen();
    }
    return TRUE;
}

/* GDI calls Enable twice: once with an odd style to fill GDIINFO, once to
 * bring the hardware up and build the PDEVICE. */
UINT WINAPI __loadds Enable(LPVOID lpDevice, UINT style, LPSTR lpDeviceType,
                            LPSTR lpOutputFile, LPVOID lpStuff)
{
    if (!(style & 1)) {
        LPDIBENGINE  lpEng = lpDevice;
        LPBITMAPINFO lpInfo;

        dbg_str("d3dpt9x: Enable (hardware)");
        lpDriverPDevice = lpDevice;
        if (!PhysicalEnable()) return 0;
        if (!bReEnabling) {
            /* the VGA core is not ours any more: stop trapping its I/O */
            _asm { mov ax, STOP_IO_TRAP
                   int 2Fh }
        }

        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);

        lpInfo = (LPVOID)((LPBYTE)lpDevice + wDIBPdevSize);
        ZeroFar(&lpInfo->bmiHeader, sizeof(lpInfo->bmiHeader));
        lpInfo->bmiHeader.biSize     = sizeof(lpInfo->bmiHeader);
        lpInfo->bmiHeader.biWidth    = wScrX;
        lpInfo->bmiHeader.biHeight   = wScrY;
        lpInfo->bmiHeader.biPlanes   = 1;
        lpInfo->bmiHeader.biBitCount = wBpp;

        /* Describe guest VRAM to the DIB Engine and let it draw there, so
         * the bits GDI writes are the bits the device scans out. */
        lpEng->deType         = TYPE_DIBENG;
        /* OFFSCREEN says the surface is a window on a larger VRAM the
         * driver manages. FIVE6FIVE tells the Engine a 16 bpp mode is 5-6-5
         * rather than 5-5-5. */
        lpEng->deFlags        = MINIDRIVER | VRAM | OFFSCREEN |
                                (wBpp == 16 ? FIVE6FIVE : 0) |
                                (wBpp == 8 ? PALETTIZED : 0);
        lpEng->deWidth        = wScrX;
        lpEng->deHeight       = wScrY;
        lpEng->deWidthBytes   = (WORD)dwPitch;
        lpEng->deDeltaScan    = dwPitch;
        lpEng->dePlanes       = 1;
        lpEng->deBitsPixel    = (BYTE)((wBpp + 7) & 0xf8);
        lpEng->deReserved1    = 0;
        lpEng->delpPDevice    = 0;
        lpEng->deBitsOffset   = 0;
        lpEng->deBitsSelector = wVramSel;
        lpEng->deBitmapInfo   = lpInfo;
        lpEng->deVersion      = VER_DIBENG;
        lpEng->deBeginAccess  = BeginAccess;
        lpEng->deEndAccess    = EndAccess;

        if (wBpp == 8) {
            WORD i;
            RGBQUAD FAR *ct = (RGBQUAD FAR *)((LPBYTE)lpInfo + sizeof(BITMAPINFOHEADER));

            /* Fill the table before reading it. This colour table is ours.
             * It sits past the DIB Engine's PDEVICE, in the bytes
             * `dpDEVICEsize` was grown by, and nothing has written it yet.
             * Programming the adapter's palette from it unfilled gave 256
             * entries of whatever the allocation held, so an 8 bpp mode came
             * up in arbitrary colours until something realised a palette.
             * Anything that drew without one stayed wrong, like a Windows
             * message box over a full-screen 8 bpp game with red greys. */
            DefaultColourTable(ct);
            for (i = 0; i < 256; i++) {
                DWORD c = ((DWORD)ct[i].rgbRed << 16) | ((DWORD)ct[i].rgbGreen << 8) | (DWORD)ct[i].rgbBlue;
                RegPut((WORD)(D3DPT_FB_REG_PALETTE + 4 * i), c);
            }
        }

        /* Notify DirectDraw of mode changes so primary surface pitch and format update */
        DDCreateDriverObject(1);

        wEnabled = 1;
        HookInt2Fh();
        dbg_str("d3dpt9x: enabled");
        return 1;
    } else {
        LPGDIINFO lpInfo = lpDevice;

        dbg_str("d3dpt9x: Enable (GDIINFO)");
        AdapterFind();
        ReadDisplayConfig();
        ZeroFar(lpInfo, sizeof(GDIINFO));
        /* the DIB Engine fills the curve/line/polygon capabilities */
        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);

        lpInfo->dpVersion    = DRV_VERSION;
        lpInfo->dpTechnology = DT_RASDISPLAY;
        lpInfo->dpHorzSize   = DISPLAY_HORZ_MM;
        lpInfo->dpVertSize   = DISPLAY_VERT_MM;
        lpInfo->dpPlanes     = 1;
        lpInfo->dpNumFonts   = 0;
        lpInfo->dpHorzRes    = wScrX;
        lpInfo->dpVertRes    = wScrY;

        lpInfo->dpMLoWin.xcoord = DISPLAY_HORZ_MM * 10;
        lpInfo->dpMLoWin.ycoord = DISPLAY_VERT_MM * 10;
        lpInfo->dpMLoVpt.xcoord = wScrX;
        lpInfo->dpMLoVpt.ycoord = -wScrY;
        lpInfo->dpMHiWin.xcoord = DISPLAY_HORZ_MM * 100;
        lpInfo->dpMHiWin.ycoord = DISPLAY_VERT_MM * 100;
        lpInfo->dpMHiVpt.xcoord = wScrX;
        lpInfo->dpMHiVpt.ycoord = -wScrY;
        lpInfo->dpELoWin.xcoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpELoWin.ycoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpELoVpt.xcoord = wScrX;
        lpInfo->dpELoVpt.ycoord = -wScrX;
        lpInfo->dpEHiVpt.xcoord = -wScrX / 2;
        lpInfo->dpEHiVpt.ycoord = wScrX / 2;
        lpInfo->dpTwpWin.xcoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpWin.ycoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpVpt.xcoord = -wScrX / 2;
        lpInfo->dpTwpVpt.ycoord = wScrX / 2;

        lpInfo->dpLogPixelsX = wDpi;
        lpInfo->dpLogPixelsY = wDpi;
        lpInfo->dpBitsPixel  = (wBpp + 7) & 0xfff8;
        lpInfo->dpDCManage   = DC_IgnoreDFNP;
        /* `C1_COLORCURSOR` asks Windows for a colour pointer, and the cursor
         * sprite carries monochrome shapes only. Claim it only on an adapter
         * with no sprite, where the Engine's software pointer (which handles
         * colour) is in use. */
        lpInfo->dpCaps1     |= C1_REINIT_ABLE | C1_BYTE_PACKED | C1_GLYPH_INDEX;
        if (!wCursorHW) lpInfo->dpCaps1 |= C1_COLORCURSOR;
        lpInfo->dpText      |= TC_CP_STROKE | TC_RA_ABLE;

        wDIBPdevSize = lpInfo->dpDEVICEsize;
        lpInfo->dpNumBrushes  = -1;
        lpInfo->dpNumPens     = -1;
        if (wBpp == 8) {
            lpInfo->dpNumColors   = 20;
            lpInfo->dpNumPalReg   = 256;
            lpInfo->dpPalReserved = 20;
            lpInfo->dpColorRes    = 18;
            lpInfo->dpRaster     |= RC_PALETTE;
        } else {
            lpInfo->dpNumColors   = -1;
            lpInfo->dpNumPalReg   = 0;
            lpInfo->dpPalReserved = 0;
            lpInfo->dpColorRes    = 0;
        }
        lpInfo->dpRaster     |= RC_DIBTODEV;
        /* Always room for the colour table, whatever this mode's depth
         * (doc 19 §30). GDI allocates the display's PDEVICE once, from the
         * size given here at boot, and `ReEnable` reuses that block for
         * every later mode change. A desktop enabled at 16 bpp that a game
         * switched to 320x200x8 had its 256 RGBQUADs written 1 KB past the
         * end of the block. The GDI-heap overrun surfaced far from here: a
         * fatal exception in KERNEL32 as Carmageddon set its mode, or page
         * faults in the VMM called from VTDAPI and VSB16, or a System VM
         * spinning in V86 mode. Sized for 8 bpp always, the block fits
         * every mode. */
        lpInfo->dpDEVICEsize += sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD);

        return sizeof(GDIINFO);
    }
}

/* Called to change resolution without a reboot. */
UINT WINAPI __loadds ReEnable(LPVOID lpDevice, LPGDIINFO lpInfo)
{
    /* The mode in force, put back when the hardware half refuses the new
     * one. The desktop stays where it was, and the next DOS box's
     * RestoreDesktopMode does not program the refused mode at the old
     * pitch. */
    WORD x = wScrX, y = wScrY, bpp = wBpp, pal = wPalettized;
    UINT rc;

    ReadDisplayConfig();
    bReEnabling = 1;
    rc = Enable(lpDevice, 0, NULL, NULL, NULL);
    if (rc) Enable(lpInfo, 1, NULL, NULL, NULL);
    else { wScrX = x; wScrY = y; wBpp = bpp; wPalettized = pal; }
    bReEnabling = 0;
    return rc;
}

VOID WINAPI __loadds Disable(LPPDEVICE lpDevice)
{
    if (wEnabled) {
        UnhookInt2Fh();
        DIB_Disable(lpDevice);
        PhysicalDisable();
        wEnabled = 0;
    }
}

typedef struct {
    BYTE r, g, b, flags;
} D3DPT_PALENTRY;

VOID WINAPI __loadds SetPalette(WORD nStartIndex, WORD nNumEntries,
                                D3DPT_PALENTRY FAR *lpPalette)
{
    WORD i;

    DIB_SetPaletteExt(nStartIndex, nNumEntries, lpPalette, lpDriverPDevice);

    if (wBpp == 8 && wRegsSel && lpPalette) {
        if (nStartIndex >= 256) return;
        if (nNumEntries > 256 - nStartIndex) nNumEntries = 256 - nStartIndex;
        for (i = 0; i < nNumEntries; i++) {
            D3DPT_PALENTRY FAR *p = &lpPalette[i];
            DWORD c = ((DWORD)p->r << 16) | ((DWORD)p->g << 8) | (DWORD)p->b;
            RegPut((WORD)(D3DPT_FB_REG_PALETTE + 4 * (nStartIndex + i)), c);
        }
    }
}

/* ValidateMode (ordinal 700). GDI asks before it switches. */
UINT WINAPI __loadds ValidateMode(DISPVALMODE FAR *lpMode)
{
    UINT rc;
    if (!AdapterFind()) rc = VALMODE_NO_WRONGDRV;
    else if (!ModeOk((WORD)lpMode->dvmXRes, (WORD)lpMode->dvmYRes, (WORD)lpMode->dvmBpp))
        rc = VALMODE_NO_NOMEM;
    else rc = VALMODE_YES;
    dbg_val("d3dpt9x: ValidateMode x", lpMode->dvmXRes);
    dbg_val("d3dpt9x: ValidateMode y", lpMode->dvmYRes);
    dbg_val("d3dpt9x: ValidateMode bpp", lpMode->dvmBpp);
    dbg_val("d3dpt9x: ValidateMode ->", rc);
    return rc;
}

/* ---------------------------------------------------------------- Control */

#define QUERYESCSUPPORT 8
/* DCICOMMAND itself comes from gdidefs.h */

/* Which escapes this driver is asked about, each logged once (doc 19 §41).
 * A display driver can be asked for DPMS through `SETPOWERMANAGEMENT`
 * (6148, 0x1804), as the NT miniport is through `HwSetPowerState`, and
 * answering it looked like a way to stop Windows taking the screen away
 * for a blank. `PWRPROBE.EXE` showed that on a monitor power-down Windows
 * asks about escapes 0xc01 and 0x27 only, never 0x1804, and takes the
 * screen away regardless. */
static WORD wEscAsked[16];
static WORD wEscSeen = 0;

static int EscNoted(WORD code)
{
    WORD i;
    for (i = 0; i < wEscSeen; i++) if (wEscAsked[i] == code) return 1;
    return 0;
}

/* the DirectDraw sub-commands of DCICOMMAND (doc 19 §2) */
#define DDCREATEDRIVEROBJECT    10
#define DDGET32BITDRIVERNAME    11
#define DDNEWCALLBACKFNS        12
#define DDVERSIONINFO           13

LONG WINAPI __loadds Control(LPVOID lpDevice, UINT function,
                             LPVOID lpInput, LPVOID lpOutput)
{
    /* Only the escapes. GDI sends a handful of other functions at startup
     * and this counter is not for them. */
    if (function == DCICOMMAND && wDDLines < 24) {
        dbg_val("d3dpt9x: Control fn", function);
        dbg_str("");
    }
    if (function == QUERYESCSUPPORT) {
        WORD code = *(WORD FAR *)lpInput;
        /* Log each escape Windows asks about once (see wEscAsked above).
         * GDI asks them all at start-up. */
        if (wEscSeen < 16 && !EscNoted(code)) {
            wEscAsked[wEscSeen++] = code;
            dbg_val("d3dpt9x: QUERYESCSUPPORT", code);
            dbg_str("");
        }
        if (code == QUERYESCSUPPORT) return 1;
        /* Ours (doc 19 §43). A ring-3 program cannot ask this any other
         * way, since the register page is not its to map and DirectDraw is
         * what it is deciding about. */
        if (code == D3DPT_ESC_HOSTINFO) return 1;
        if (code == DCICOMMAND) {
            /* The answer is the HAL version, not "yes". DirectDraw reads
             * this return value to decide what the driver is. A plain 1
             * means DCI only, and DirectDraw then sends one
             * DCICREATEPRIMARYSURFACE and never asks for DDVERSIONINFO or
             * DDGET32BITDRIVERNAME. The only symptom is a HAL with dwCaps
             * 0x02000000 and no video memory. */
            if (wDDLines < 24) {
                wDDLines++;
                dbg_val("d3dpt9x: QUERYESCSUPPORT(DCICOMMAND) ->", DD_HAL_VERSION);
                dbg_str("");
            }
            return DD_HAL_VERSION;
        }
        /* everything else the DIB Engine answers for us */
    }
    /* What the host can do, for a program that has to decide before it
     * loads DirectDraw. It returns the adapter's own `D3D_STATUS`, which is
     * `D3DPT_STATUS_NO_EXEC` on a host with no executor or one started with
     * `no-exec=on`. `D3DPRE.EXE` asks this at login and switches the
     * machine's DirectDraw to WineD3D's, or back (doc 19 §43). */
    if (function == D3DPT_ESC_HOSTINFO && lpOutput != 0) {
        D3DPT_ESC_HOSTINFO_T FAR *hi = (D3DPT_ESC_HOSTINFO_T FAR *)lpOutput;

        hi->magic = D3DPT_ESC_HOSTINFO_MAGIC;
        hi->size = sizeof(*hi);
        hi->d3d_status = RegGet(D3DPT_FB_REG_D3D_STATUS);
        hi->fb_version = RegGet(D3DPT_FB_REG_VERSION);
        dbg_val("d3dpt9x: host info escape, d3d status", hi->d3d_status);
        dbg_str("");
        return 1;
    }
    if (function == DCICOMMAND && lpInput != 0) {
        DCICMD_t FAR *cmd = (DCICMD_t FAR *)lpInput;

        /* Always log the first few. A driver that never hears this escape
         * looks exactly like one whose answers were wrong, and the whole
         * DirectDraw half depends on it arriving. */
        if (wDDLines < 24) {
            wDDLines++;
            dbg_val("d3dpt9x: DCICOMMAND version", cmd->dwVersion);
            dbg_val(" command", cmd->dwCommand);
            dbg_str("");
        }

        /* Only the DirectDraw version of this escape is ours. The DCI one
         * (dwVersion == DCI_VERSION) belongs to the DIB Engine, and so does
         * anything from a newer runtime. Handing those back rather than
         * failing them keeps DirectDraw working (the reference driver
         * learned this). */
        if (cmd->dwVersion != DD_VERSION) {
            return DIB_Control(lpDevice, function, lpInput, lpOutput);
        }
        switch (cmd->dwCommand) {
        case DDCREATEDRIVEROBJECT:
            if (!DDCreateDriverObject(0)) return 0;
            *(DWORD FAR *)lpOutput = DDHinstance();
            return 1;
        case DDGET32BITDRIVERNAME:
            return DDGet32BitDriverName((struct DD32BITDRIVERDATA FAR *)lpOutput) ? 1 : 0;
        case DDNEWCALLBACKFNS:
            return DDNewCallbackFns((struct DCICMD FAR *)lpInput) ? 1 : 0;
        case DDVERSIONINFO:
            DDGetVersion((struct DDVERSIONDATA FAR *)lpOutput);
            return 1;
        default:
            dbg_val("d3dpt9x: unknown DD escape", cmd->dwCommand);
            return 0;
        }
    }
    return DIB_Control(lpDevice, function, lpInput, lpOutput);
}

/* -------------------------------------------------------------- DriverInit */

/* Int 2Fh AX=1684h: the entry point of a virtual device (the main VDD). */
#define VDD_ID 10
static void __far *int2F_GetEP(WORD ax, WORD bx);
#pragma aux int2F_GetEP =   \
    "int    2Fh"            \
    parm [ax] [bx] value [es di];

static WORD int2F_GetVMID(WORD ax);
#pragma aux int2F_GetVMID = \
    "int    2Fh"            \
    parm [ax] value [bx];

UINT WINAPI GlobalSmartPageLock(HGLOBAL hglb);

/* The code segment's selector, for the page lock below (doc 19 §13). The
 * idiom every 9x driver uses, `extern char __based(__segname("_TEXT"))
 * *pText`, puts the reference in a second segment also called `_TEXT`, of
 * class FAR_DATA, which stays empty in a driver this small. wlink drops the
 * empty segment from the NE segment table but keeps the relocation, and
 * KERNEL silently refuses to load a module whose relocation names a missing
 * segment. CS is the same selector and needs no relocation.
 * build-driver9x.sh fails the build if a relocation points past the
 * segment table. */
static WORD GetCS(void);
#pragma aux GetCS = "mov ax, cs" value [ax];

#pragma aux DriverInit parm [cx] [di] [es si]

UINT FAR DriverInit(UINT cbHeap, UINT hModule, LPSTR lpCmdLine)
{
    /* The code segment must not move while we are on the hardware. */
    dbg_str("d3dpt9x: DriverInit entered");

    /* Nothing here may fail. GDI treats a zero from DriverInit as "no
     * driver" and silently falls back to VGA, which looks the same as the
     * module never loading. The adapter is found on the first Enable
     * instead, where a failure is visible. */
    GlobalSmartPageLock((HGLOBAL)GetCS());
    VDDEntryPoint = (DWORD)int2F_GetEP(0x1684, VDD_ID);
    OurVMHandle   = int2F_GetVMID(0x1683);

    /* Ask the mini-VDD for the adapter here as well as in Enable. This is
     * the earliest point at which the driver's state can be seen from
     * outside, because the answer is logged in ring 0, where port 0xE9 and
     * the DEBUG register both work. */
    AdapterFind();
    dbg_val("d3dpt9x: cs", GetCS());
    dbg_str("d3dpt9x: DriverInit done");
    return 1;
}

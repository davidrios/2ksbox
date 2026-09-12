/*
 * d3dptvxd.c — the Win98/Me mini-VDD for the d3dpt-vga adapter (doc 19,
 * ADR-012 / M10). Ring 0, a dynamically loadable VxD, built with Open
 * Watcom's 32-bit compiler.
 *
 * Why this exists, and why before the display driver: on 9x the ring-0
 * half owns the adapter. Windows unmaps a PCI device's base addresses
 * within half a minute of boot when nothing claims them, so a 16-bit
 * display driver that maps the BARs itself finds zeros and GDI falls back
 * to VGA — measured, doc 19 §11. This VxD is what claims the device:
 *
 *   - it finds the adapter on the bus and, if Windows has already taken
 *     the base addresses away, programs them back and re-enables memory
 *     decoding;
 *   - it maps VRAM and the register page into ring-0 linear space;
 *   - it registers itself in the main VDD's mini-VDD dispatch table, so
 *     the display driver's VDD_REGISTER_DISPLAY_DRIVER_INFO call reaches
 *     us and we can hand back 16-bit selectors onto both — the display
 *     driver never touches PCI configuration space again.
 *
 * Debug output goes to port 0xE9 (QEMU's `-debugcon`) and, once the
 * register page is mapped, to the adapter's DEBUG register and so into the
 * QEMU log — the same two channels the .drv uses, and in ring 0 both
 * actually work.
 *
 * Build: guest-tools/build-driver9x.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include "vmm.h"
#include "d3dpt9v.h"
#include "../../../../d3dpt/d3dpt_fb.h"

/* Everything — code, data and constants — goes into one segment of class
 * CODE, so the module links to a single LE object and the DDB below lands
 * at its offset 0. That is where the VMM's loader expects a VxD's
 * descriptor block: a real driver's entry table names object 1 offset 0
 * (checked against the guest's own FXMEMMAP.VXD), while letting the DDB
 * fall into _DATA gives a 16-bit entry into the second object and the VxD
 * silently does not load. vmdisp9x's source carries the same warning. */
#pragma data_seg("_LTEXT", "CODE")
#pragma code_seg("_LTEXT", "CODE")
#pragma const_seg("_LTEXT", "CODE")

void VXD_control(void);
void __stdcall Device_Init_proc(DWORD VM);

DDB VXD_DDB = {
    NULL,                       /* DDB_Next, VMM's */
    DDK_VERSION,
    D3DPT_VXD_ID,
    D3DPT_VXD_MAJOR, D3DPT_VXD_MINOR,
    0,                          /* flags */
    D3DPT_VXD_NAME,
    VDD_Init_Order,             /* with the VDD: we are its mini-VDD */
    (DWORD)VXD_control,
    NULL, NULL,                 /* no V86 / PM API entry: the VDD calls us */
    NULL, NULL,
    NULL,                       /* reference data */
    NULL, 0,                    /* no service table */
    NULL,                       /* no Win32 service table */
    'Prev',
    sizeof(DDB),
    'Rsv1', 'Rsv2', 'Rsv3',
};

/* ------------------------------------------------------------ VMM services */

static DWORD __declspec(naked) __cdecl _PageReserve(ULONG page, ULONG npages, ULONG flags)
{
    VMMJmp(_PageReserve);
}

static DWORD __declspec(naked) __cdecl _PageCommitPhys(ULONG page, ULONG npages,
                                                       ULONG physpg, ULONG flags)
{
    VMMJmp(_PageCommitPhys);
}



static DWORD __declspec(naked) __cdecl _CopyPageTable(ULONG lpn, ULONG npages,
                                                      DWORD *buf, ULONG flags)
{
    VMMJmp(_CopyPageTable);
}

static void __declspec(naked) __cdecl BuildDesc_(ULONG base, ULONG limit, ULONG type,
                                                 ULONG size, ULONG flags)
{
    VMMJmp(_BuildDescriptorDWORDs);
}

static void __declspec(naked) __cdecl AllocGDT_(ULONG hi, ULONG lo, ULONG flags)
{
    VMMJmp(_Allocate_GDT_Selector);
}



void dbg_str(const char *s);
void dbg_val(const char *tag, DWORD v);

/* Map a physical range of the adapter where **ring 3 can also reach it**.
 *
 * `_MapPhysToLinear` is the obvious call and it is the wrong one here: it
 * maps into the system arena, whose pages are supervisor-only, so a 16-bit
 * display driver holding a selector onto one reads zeros and its writes go
 * nowhere — no fault, no diagnostic, just a register set that looks like a
 * different device (doc 19 Section 14). The shared arena with PC_USER is
 * the mapping both halves can use, and one mapping serves both: ring 0 may
 * read a user page.
 *
 * PR_FIXED / PC_FIXED because the adapter's memory is not swappable, and
 * PC_INCR because the physical pages are consecutive. */
/* What the page tables actually say about a mapping. */
static void dbg_pte(const char *tag, DWORD lin)
{
    static DWORD pte;

    pte = 0;
    if (_CopyPageTable(lin >> 12, 1, &pte, 0)) dbg_val(tag, pte);
    else dbg_str("d3dptvxd: no page table for that address");
}

static DWORD MapDevicePhys(DWORD phys, DWORD bytes)
{
    DWORD pages = (bytes + 0xfffuL) >> 12;
    DWORD lin;

    lin = _PageReserve(PR_SHARED, pages, PR_FIXED);
    if (!lin || lin == 0xffffffffuL) { dbg_str("d3dptvxd: no shared arena"); return 0; }

    /* PC_USER is the whole point of the shared-arena mapping, PC_WRITEABLE
     * goes with it, and PC_INCR is what makes the physical pages advance —
     * without it the whole 128 MB of VRAM aliases onto one page and the
     * desktop is drawn 4 KB at a time on top of itself.
     *
     * **This VMM refuses PC_PRESENT here**, silently, returning zero with
     * everything else correct; it refuses PC_FIXED and PC_STATIC the same
     * way. There is no fallback on purpose: a commit without PC_INCR would
     * succeed and give an aliased mapping, which is worse than no mapping
     * because it looks like it worked (doc 19 Section 14). */
    if (!_PageCommitPhys(lin >> 12, pages, phys >> 12,
                         PC_USER | PC_WRITEABLE | PC_INCR)) {
        dbg_val("d3dptvxd: cannot commit phys", phys);
        return 0;
    }

    /* First and last page, because an aliased mapping is invisible from
     * anywhere else: the same value twice means PC_INCR did not take. */
    dbg_pte("d3dptvxd: pte 0", lin);
    dbg_pte("d3dptvxd: pte last", lin + ((pages - 1) << 12));
    return lin;
}

/* A 16-bit data selector onto a linear range, handed to the display
 * driver so it can address VRAM and the registers with a far pointer. */
static WORD MakeSelector(DWORD linear, DWORD bytes)
{
    DWORD hi = 0, lo = 0, sel = 0;

    BuildDesc_(linear, (bytes + 0xfff) >> 12, D3DPT_SEL_TYPE, 0x80, 0);
    _asm {
        mov [hi], edx
        mov [lo], eax
    }
    AllocGDT_(hi, lo, 0);
    _asm mov [sel], eax
    return (WORD)sel;
}

/* --------------------------------------------------------------- debugging */

static void OutE9(BYTE c);
#pragma aux OutE9 = "out 0E9h, al" parm [al];

DWORD dwRegsLin = 0, dwVramLin = 0, dwVramSize = 0;
DWORD dwRegsPhys = 0, dwVramPhys = 0;

static void dbg_ch(BYTE c)
{
    OutE9(c);
    if (dwRegsLin) *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_DEBUG) = c;
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

/* ---------------------------------------------------------- PCI, ring 0 */

static DWORD PciRead(DWORD addr);
#pragma aux PciRead =           \
    "mov    dx, 0CF8h"          \
    "out    dx, eax"            \
    "mov    dx, 0CFCh"          \
    "in     eax, dx"            \
    parm [eax] value [eax] modify [dx];

static void PciWrite(DWORD addr, DWORD val);
#pragma aux PciWrite =          \
    "mov    dx, 0CF8h"          \
    "out    dx, eax"            \
    "mov    dx, 0CFCh"          \
    "mov    eax, ebx"           \
    "out    dx, eax"            \
    parm [eax] [ebx] modify [dx eax];

#define CFG(dev, off) (0x80000000uL | ((DWORD)(dev) << 11) | ((off) & 0xfc))

/* Find the adapter and make sure it is decoding memory. Windows may have
 * stripped the base addresses already (doc 19 §11), in which case we put
 * back what we were told at the first sighting — the BIOS's assignment,
 * which nothing else is using. */
static BOOL AdapterClaim(void)
{
    DWORD dev, id, cmd;

    for (dev = 0; dev < 32; ++dev) {
        id = PciRead(CFG(dev, 0));
        if (id == ((DWORD)D3DPT_FB_PCI_DEVICE << 16 | D3DPT_FB_PCI_VENDOR))
            break;
    }
    if (dev == 32) { dbg_str("d3dptvxd: adapter not on the bus"); return FALSE; }

    if (!dwVramPhys) {
        dwVramPhys = PciRead(CFG(dev, 0x10)) & 0xfffffff0uL;
        dwRegsPhys = PciRead(CFG(dev, 0x14)) & 0xfffffff0uL;
        dbg_val("d3dptvxd: bar0", dwVramPhys);
        dbg_val("d3dptvxd: bar1", dwRegsPhys);
    }
    if (!dwVramPhys || !dwRegsPhys) {
        dbg_str("d3dptvxd: the adapter has no base addresses");
        return FALSE;
    }

    /* put them back if they have been taken away, and decode memory */
    if ((PciRead(CFG(dev, 0x10)) & 0xfffffff0uL) != dwVramPhys)
        PciWrite(CFG(dev, 0x10), dwVramPhys | 0x8);     /* prefetchable */
    if ((PciRead(CFG(dev, 0x14)) & 0xfffffff0uL) != dwRegsPhys)
        PciWrite(CFG(dev, 0x14), dwRegsPhys);
    cmd = PciRead(CFG(dev, 0x04));
    if (!(cmd & 0x2)) PciWrite(CFG(dev, 0x04), cmd | 0x2);
    return TRUE;
}

static BOOL AdapterMap(void)
{
    if (!AdapterClaim()) return FALSE;

    if (!dwRegsLin) {
        dwRegsLin = MapDevicePhys(dwRegsPhys, D3DPT_FB_REGS_SIZE);
        if (!dwRegsLin) return FALSE;
    }
    if (*(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_MAGIC) != D3DPT_FB_MAGIC) {
        dbg_str("d3dptvxd: not our adapter after all");
        dwRegsLin = 0;
        return FALSE;
    }
    /* at least our version: a newer register set only adds (d3dpt_fb.h) */
    if (*(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_VERSION) < D3DPT_FB_VERSION) {
        dbg_str("d3dptvxd: register set older than this driver, refusing the device");
        dwRegsLin = 0;
        return FALSE;
    }
    dwVramSize = *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_VRAM_SIZE);

    if (!dwVramLin) {
        dwVramLin = MapDevicePhys(dwVramPhys, dwVramSize);
        if (!dwVramLin) return FALSE;
    }
    dbg_val("d3dptvxd: vram", dwVramSize);
    return TRUE;
}

/* ------------------------------------------------- the mini-VDD dispatch */

static DWORD *DispatchTable = 0;
static DWORD DispatchTableLength = 0;

static void VDD_Get_Mini_Dispatch_Table(void)
{
    VxDCall(VDD, Get_Mini_Dispatch_Table);
    _asm mov [DispatchTable], edi
    _asm mov [DispatchTableLength], ecx
}

static WORD wVramSel = 0, wRegsSel = 0;

/*
 * REGISTER_DISPLAY_DRIVER (mini-VDD function 0), reached from the display
 * driver's VDD_REGISTER_DISPLAY_DRIVER_INFO. We answer with what the
 * driver cannot get for itself:
 *   EAX = the register page's selector
 *   EDX = VRAM's selector
 *   ECX = VRAM's size in bytes
 *   ESI = VRAM's ring-0 linear address (for whoever comes next: the
 *         DirectDraw heap and the ring-3 HAL both want it)
 */
static void __stdcall register_display_driver_proc(DWORD vm, PCRS_32 state)
{
    if (!AdapterMap()) {
        state->Client_EAX = 0;
        state->Client_EDX = 0;
        state->Client_ECX = 0;
        state->Client_ESI = 0;
        state->Client_EDI = 0;
        state->Client_EFlags |= 0x1;    /* carry: nothing to give */
        return;
    }

    if (!wRegsSel) wRegsSel = MakeSelector(dwRegsLin, D3DPT_FB_REGS_SIZE);
    if (!wVramSel) wVramSel = MakeSelector(dwVramLin, dwVramSize);

    dbg_val("d3dptvxd: regs lin", dwRegsLin);
    dbg_val("d3dptvxd: vram lin", dwVramLin);
    dbg_val("d3dptvxd: regs sel", wRegsSel);
    dbg_val("d3dptvxd: vram sel", wVramSel);

    state->Client_EAX = wRegsSel;
    state->Client_EDX = wVramSel;
    state->Client_ECX = dwVramSize;
    state->Client_ESI = dwVramLin;
    state->Client_EDI = dwRegsLin;
    state->Client_EFlags &= 0xfffffffeuL;
    dbg_str("d3dptvxd: display driver registered");
}

/* ------------------------------------------------- the screen switch
 *
 * **A DOS box takes the screen away, and somebody has to give it back.**
 * When a VM goes full-screen the main VDD switches the adapter from the
 * display driver's hi-res mode to VGA and lets the DOS program program the
 * VGA registers itself. Our adapter is a VGA *and* a linear frame buffer,
 * and while `D3DPT_FB_REG_ENABLE` is set the device scans out the linear
 * one — so the DOS program writes VGA memory at A0000 and nothing of it
 * reaches the screen, which goes on showing the frozen desktop. Every
 * full-screen DOS game "glitches out", whatever mode it asks for.
 *
 * The main VDD tells the mini-VDD when this is about to happen and when it
 * is over; nothing else in the system knows about the register. The way
 * back is the display driver's `RestoreDesktopMode`, which the VDD calls
 * through the callback registered with `VDD_DRIVER_REGISTER` and which
 * writes every mode register including ENABLE — so `POST_VGA_TO_HIRES`
 * only has to make sure, and the device keeps width/height/bpp/pitch across
 * an ENABLE of 0 anyway.
 *
 * **Measured 2026-09-09**, Alt+Enter on a DOS box and back: all four are
 * called, in order, with the display driver's callback in the middle —
 *
 *     d3dptvxd: hi-res -> VGA
 *     d3dptvxd: hi-res -> VGA done
 *     d3dptvxd: VGA -> hi-res
 *     d3dpt9x: RestoreDesktopMode
 *     d3dptvxd: VGA -> hi-res done
 *
 * — and with the ENABLE write in `PRE_HIRES_TO_VGA`, Blood renders
 * full-screen at 640x480 through the device's VGA core for the whole of its
 * attract demo, then the desktop comes back. Which of these the VDD calls
 * for a given kind of switch is not something the DDK headers say, so they
 * all log; that log is what the round trip above was read off.
 *
 * One thing to know if this ever looks wrong again: judging it from an
 * *interim* read of the log is how it gets misread. Half way through that
 * run only the first two lines existed and the screen was still the frozen
 * desktop — the DOS box had not left its prompt yet — which reads exactly
 * like a one-way switch into a black screen. Wait for the run to end. */
static void __stdcall hires_to_vga_proc(void)
{
    dbg_str("d3dptvxd: hi-res -> VGA");
    if (dwRegsLin) *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_ENABLE) = 0;
}

static void __stdcall post_hires_to_vga_proc(void)
{
    dbg_str("d3dptvxd: hi-res -> VGA done");
}

static void __stdcall pre_vga_to_hires_proc(void)
{
    dbg_str("d3dptvxd: VGA -> hi-res");
}

static void __stdcall vga_to_hires_proc(void)
{
    dbg_str("d3dptvxd: VGA -> hi-res done");
    if (dwRegsLin) *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_ENABLE) = 1;
}

/* ------------------------------------------------- the blue screen
 *
 * **A blue screen is a message screen, and the VDD draws it itself.** A
 * fatal exception, a "Windows protection error", the Ctrl+Alt+Del screen,
 * "It is now safe to turn off your computer": the VMM enters *message
 * mode* and the main VDD programs VGA text mode directly — no int 10h, no
 * display driver drawing. With ENABLE left on the device went on scanning
 * out the frozen desktop, and every blue screen this driver produced in its
 * first three days was invisible: doc 19 §15 is the archaeology of reading
 * them out of VRAM afterwards, and the user's report that "BSODs don't
 * show up" (2026-09-09) is the same thing seen from the chair.
 *
 * Measured with tools/win98-bsod-test.sh (a VxD of ours faulting at load):
 * a fatal exception in the Windows VM takes the ordinary road — the INT 2Fh
 * notification to the display driver, then PRE_HIRES_TO_VGA above — so the
 * hook above is what puts that one on the screen. SAVE_MESSAGE_MODE_STATE
 * is the DDK's other door, for message screens the VDD puts up without a VM
 * switch; this VMM calls it once at boot, before the mode is set, and would
 * call it for those. ENABLE off here too: harmless at boot, right whenever
 * it is used for what its name says. The way back after "press any key to
 * continue" is RestoreDesktopMode, as after any switch. */
static void __stdcall save_message_mode_proc(void)
{
    dbg_str("d3dptvxd: message mode: VGA text");
    if (dwRegsLin) *(volatile DWORD *)(dwRegsLin + D3DPT_FB_REG_ENABLE) = 0;
}

/* The notifications that are only logged, the first four of each — enough
 * to read a sequence off, not enough to fill the log on a machine that
 * switches VMs all day (SAVE/RESTORE_REGISTERS run at every VM switch). */
static BYTE seen[64] = {0};

static void __stdcall note_proc(DWORD fn)
{
    if (fn < 64 && seen[fn]++ < 4) dbg_val("d3dptvxd: vdd fn", fn);
}

/* The VDD calls a dispatch entry with EBX = VM and EBP = client registers.
 * These want neither, so each thunk is a register-preserving call with
 * carry clear on the way out ("handled, no objection").
 *
 * Written out four times rather than from a macro: Open Watcom's inline
 * assembler does not take the callee's name through a macro parameter — it
 * assembles a call to nothing and the compiler then warns that the function
 * is "defined, but not referenced", which is the only sign you get. */
static void __declspec(naked) hires_to_vga_entry(void)
{
    _asm {
        pushad
        call hires_to_vga_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) post_hires_to_vga_entry(void)
{
    _asm {
        pushad
        call post_hires_to_vga_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) pre_vga_to_hires_entry(void)
{
    _asm {
        pushad
        call pre_vga_to_hires_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) vga_to_hires_entry(void)
{
    _asm {
        pushad
        call vga_to_hires_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) save_message_mode_entry(void)
{
    _asm {
        pushad
        call save_message_mode_proc
        popad
        clc
        retn
    }
}

/* One logging thunk per entry, each pushing its own number (note_proc is
 * __stdcall, so it pops it). Thirteen copies rather than a macro, for the
 * reason above: the inline assembler takes no macro parameter, not even a
 * literal — `push n` through one is "Invalid instruction operands". */
static void __declspec(naked) note_8_entry(void)
{
    _asm {
        pushad
        push 8
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_9_entry(void)
{
    _asm {
        pushad
        push 9
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_11_entry(void)
{
    _asm {
        pushad
        push 11
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_12_entry(void)
{
    _asm {
        pushad
        push 12
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_13_entry(void)
{
    _asm {
        pushad
        push 13
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_14_entry(void)
{
    _asm {
        pushad
        push 14
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_15_entry(void)
{
    _asm {
        pushad
        push 15
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_26_entry(void)
{
    _asm {
        pushad
        push 26
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_28_entry(void)
{
    _asm {
        pushad
        push 28
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_29_entry(void)
{
    _asm {
        pushad
        push 29
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_39_entry(void)
{
    _asm {
        pushad
        push 39
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_40_entry(void)
{
    _asm {
        pushad
        push 40
        call note_proc
        popad
        clc
        retn
    }
}

static void __declspec(naked) note_46_entry(void)
{
    _asm {
        pushad
        push 46
        call note_proc
        popad
        clc
        retn
    }
}

/* The main VDD calls a dispatch entry with EBX = VM, EBP = client
 * registers, and expects the flags left alone; this is the thunk that
 * turns that into a C call. */
static void __declspec(naked) register_display_driver_entry(void)
{
    _asm {
        pushad
        push ebp
        push ebx
        call register_display_driver_proc
        popad
        retn
    }
}

/* ------------------------------------------------------------ control */

void __stdcall Device_Init_proc(DWORD VM)
{
    dbg_str("d3dptvxd: Device_Init");

    if (!AdapterMap()) {
        dbg_str("d3dptvxd: no adapter, staying out of the way");
        return;
    }

    VDD_Get_Mini_Dispatch_Table();
    dbg_val("d3dptvxd: dispatch entries", DispatchTableLength);
    if (DispatchTable && DispatchTableLength > VDD_SAVE_FORCED_PLANAR_STATE) {
        DispatchTable[VDD_REGISTER_DISPLAY_DRIVER] = (DWORD)register_display_driver_entry;
        DispatchTable[VDD_PRE_HIRES_TO_VGA]  = (DWORD)hires_to_vga_entry;
        DispatchTable[VDD_POST_HIRES_TO_VGA] = (DWORD)post_hires_to_vga_entry;
        DispatchTable[VDD_PRE_VGA_TO_HIRES]  = (DWORD)pre_vga_to_hires_entry;
        DispatchTable[VDD_POST_VGA_TO_HIRES] = (DWORD)vga_to_hires_entry;
        DispatchTable[VDD_SAVE_MESSAGE_MODE_STATE] = (DWORD)save_message_mode_entry;
        DispatchTable[VDD_SAVE_REGISTERS]    = (DWORD)note_8_entry;
        DispatchTable[VDD_RESTORE_REGISTERS] = (DWORD)note_9_entry;
        DispatchTable[VDD_ACCESS_VGA_MEMORY_MODE]    = (DWORD)note_11_entry;
        DispatchTable[VDD_ACCESS_LINEAR_MEMORY_MODE] = (DWORD)note_12_entry;
        DispatchTable[VDD_ENABLE_TRAPS]      = (DWORD)note_13_entry;
        DispatchTable[VDD_DISABLE_TRAPS]     = (DWORD)note_14_entry;
        DispatchTable[VDD_MAKE_HARDWARE_NOT_BUSY]    = (DWORD)note_15_entry;
        DispatchTable[VDD_DISPLAY_DRIVER_DISABLING]  = (DWORD)note_26_entry;
        DispatchTable[VDD_PRE_CRTC_MODE_CHANGE]      = (DWORD)note_28_entry;
        DispatchTable[VDD_POST_CRTC_MODE_CHANGE]     = (DWORD)note_29_entry;
        DispatchTable[VDD_PRE_HIRES_SAVE_RESTORE]    = (DWORD)note_39_entry;
        DispatchTable[VDD_POST_HIRES_SAVE_RESTORE]   = (DWORD)note_40_entry;
        DispatchTable[VDD_SAVE_FORCED_PLANAR_STATE]  = (DWORD)note_46_entry;
    } else
        dbg_str("d3dptvxd: the VDD's dispatch table is not the shape we expect");

    dbg_str("d3dptvxd: ready");
}

/* Every control message: carry clear means "handled, no objection". */
void __declspec(naked) VXD_control(void)
{
    _asm {
        cmp eax, Sys_Critical_Init
        jz  ctl_ok
        cmp eax, Device_Init
        jz  ctl_init
        cmp eax, Sys_Dynamic_Device_Init
        jz  ctl_init
        jmp ctl_ok

      ctl_init:
        push ebx            /* the VM handle */
        call Device_Init_proc

      ctl_ok:
        clc
        ret
    }
}

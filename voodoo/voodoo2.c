/*
 * voodoo2.c -- the 3dfx Voodoo 2 as a QEMU PCI device (doc 21, M14).
 *
 * The chip is 86Box's emulation, vendored verbatim under 86box/ and built
 * against the shim of 86Box's platform headers under shim/ (voodoo_shim.c
 * is the shim's other half). This file is what QEMU sees: a PCI function
 * 121a:0002 with one 16 MiB memory BAR whose accesses go to 86Box's
 * register/LFB/texture handlers, the configuration bytes 86Box owns
 * (initEnable at 0x40) forwarded to its handlers, and the display half --
 * a Voodoo 1/2 is a pass-through card, so when the guest sets fbiInit0's
 * VGA_PASS bit the guest console stops being drawn by the VGA device and
 * every frame the Voodoo's display timer completes is copied into it.
 * Screendumps, the VNC fallback and the player's own surface path all see
 * that frame the way they see a VGA one.
 *
 *   -device voodoo2[,fbmem=2|4][,texmem=2|4][,threads=1|2|4]
 *                  [,bilinear=on|off][,dither-sub=on|off][,filter=on|off]
 *                  [,recompiler=on|off][,ramfifo=on|off]
 *
 * ramfifo (on by default): the command-FIFO ring is plain RAM to the guest,
 * so Glide's packet stream is ordinary stores instead of one MMIO trap per
 * dword -- and under TCG a trap mid-block is a cpu_io_recompile, which was
 * most of the vCPU's time in Quake II (doc 21 §9). The chip learns what was
 * written at the guest's next access to anything else on the card; see
 * voodoo2_fifo_sync().
 *
 * Threads: the guest's MMIO writes run on the vCPU thread with the BQL; the
 * display timer on the main loop with the BQL (86Box has both on its CPU
 * thread, which is the invariant that matters); the FIFO thread and the
 * render threads are 86Box's own and touch nothing of QEMU's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/memalign.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qom/object.h"

#include "shim/86box/86box.h"
#include "shim/86box/device.h"
#include "shim/86box/mem.h"
#include "shim/86box/pci.h"
#include "shim/86box/plat.h"
#include "shim/86box/thread.h"
#include "shim/86box/timer.h"
#include "shim/86box/video.h"
#include "shim/86box/vid_svga.h"
#include "86box/vid_voodoo_common.h"
#include "86box/vid_voodoo_fifo.h"
#include "voodoo_shim.h"

/* 86box/vid_voodoo_regs.h's offsets (the header itself does not compile
 * outside 86Box's own files) */
#define SST_cmdFifoBaseAddr 0x1e0
#define SST_cmdFifoRdPtr    0x1e8
#define SST_cmdFifoDepth    0x1f4
#define SST_fbiInit7        0x24c

/* vid_voodoo.c (no upstream header declares these three) */
void   *voodoo_init(const device_t *info);
void    voodoo_close(void *priv);
uint8_t voodoo_pci_read(int func, int addr, int len, void *priv);
void    voodoo_pci_write(int func, int addr, int len, uint8_t val, void *priv);

#define TYPE_VOODOO2 "voodoo2"
OBJECT_DECLARE_SIMPLE_TYPE(Voodoo2State, VOODOO2)

#define VOODOO2_PCI_VENDOR 0x121a
#define VOODOO2_PCI_DEVICE 0x0002
#define VOODOO2_BAR_SIZE   (16 * MiB)
#define VOODOO2_STATS_MS   5000
#define VOODOO2_BLANK_MS   2000   /* a guest that never swaps still shows */

struct Voodoo2State {
    PCIDevice parent_obj;

    MemoryRegion  mmio;
    voodoo_set_t *set;
    voodoo_t     *v;

    /* display */
    QemuConsole    *con;
    DisplaySurface *surface;    /* ours, while the console shows it */
    int             override;
    bool            no_console_warned;
    uint32_t        frames;
    /* scan-outs of a buffer other than the last one: the game's frames as
     * the monitor shows them (at most the refresh rate). v->frame_count
     * cannot say this -- 86Box counts only swaps that wait for a retrace */
    uint32_t        shown;
    uint32_t        shown_front;
    /* black until the guest's first swap after the monitor changes hands or
     * size: what the frame buffer holds then is the driver's memory test
     * or the last mode's lines at the new pitch (a gray pattern), which a
     * real monitor never showed because it was re-locking to the timings */
    bool            blank;
    bool            blank_swapped;  /* seen; the next frame is all new lines */
    uint32_t        blank_front;
    int             blank_frames;
    int64_t         blank_since;
    int             shown_w, shown_h;

    /* the 5 s activity line */
    QEMUTimer *stats;
    uint32_t   last_frames;
    uint32_t   last_shown;
    int        last_tris;
    int        last_wr;
    int        last_rd;
    int        last_tex;
    unsigned   last_fatals;
    uint32_t   fifo_off_writes;   /* FIFO-window packets decoded as registers */
    uint32_t   last_fifo_off;
    bool       fifo_off_warned;

    /* the command FIFO in RAM (ramfifo=on) */
    uint8_t     *fb_86box;       /* 86Box's own fb_mem, given back at close */
    MemoryRegion fb_ram;         /* the frame buffer as RAM, never mapped */
    MemoryRegion fifo_win;       /* an alias of the ring at BAR 0x200000 */
    bool         fifo_mapped;
    bool         fifo_broken;    /* a packet not followed: MMIO for good */
    bool         fifo_stray_warned;
    uint32_t     fifo_base;      /* frame-buffer addresses of the ring */
    uint32_t     fifo_size;
    uint32_t     fifo_parse;     /* the next header nobody has counted */
    uint32_t     fifo_poisoned;  /* consumed words poisoned up to here */
    uint32_t     fifo_words, last_fifo_words;
    uint32_t     fifo_syncs, last_fifo_syncs;
    /* register-window accesses by register (addr & 0x3fc) since the last
     * line: a guest that spins on one register names it here */
    uint32_t   rd_hist[256];
    uint32_t   wr_hist[256];
    uint32_t   cfg_hist[64];    /* configuration-space reads, by dword */
    /* the last accesses, for a fatal(): what the guest was doing */
    struct {
        uint32_t addr;
        uint32_t val;
        uint8_t  size;
        uint8_t  write;
    } ring[64];
    uint32_t   ring_n;

    /* properties */
    uint32_t fbmem_mb;
    uint32_t texmem_mb;
    uint32_t threads;
    bool     bilinear;
    bool     dithersub;
    bool     filter;
    bool     recompiler;
    bool     ramfifo;
};

/* ------------------------------------------------------------------ MMIO */

static bool     voodoo2_trace;          /* VOODOO2_TRACE=1 in the environment */
static uint32_t voodoo2_trace_status;   /* status reads since the last other line */

static inline void
voodoo2_note(Voodoo2State *s, hwaddr addr, uint64_t val, unsigned size, bool write)
{
    unsigned i = s->ring_n++ % ARRAY_SIZE(s->ring);

    s->ring[i].addr  = (uint32_t) addr;
    s->ring[i].val   = (uint32_t) val;
    s->ring[i].size  = size;
    s->ring[i].write = write;
    if (voodoo2_trace && addr < 0x400000) {
        /* every register- and command-FIFO-window access, the status polls
         * (thousands between two real accesses) as one count */
        if (!write && (addr & 0x3fc) == 0 && !(addr & 0x200000)) {
            voodoo2_trace_status++;
            return;
        }
        if (voodoo2_trace_status) {
            fprintf(stderr, "voodoo2:   (%u status reads)\n", voodoo2_trace_status);
            voodoo2_trace_status = 0;
        }
        fprintf(stderr, "voodoo2: %s %06x %08x (initEnable %08x%s)\n", write ? "wr" : "rd",
                (unsigned) addr, (unsigned) val, s->v->initEnable,
                s->v->cmdfifo_enabled ? ", fifo on" : "");
    }
}

/* 86Box's fatal(): the state a bug report needs, before the abort */
static void
voodoo2_on_fatal(void *opaque)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;
    unsigned      n = MIN(s->ring_n, ARRAY_SIZE(s->ring));

    fprintf(stderr, "voodoo2: initEnable %08x fbiInit0 %08x fbiInit7 %08x "
            "cmdfifo %s base %08x end %08x rp %08x depth wr %u rd %u; "
            "%dx%d, %s\n",
            v->initEnable, v->fbiInit0, v->fbiInit7,
            v->cmdfifo_enabled ? "on" : "off", v->cmdfifo_base, v->cmdfifo_end,
            v->cmdfifo_rp, v->cmdfifo_depth_wr, v->cmdfifo_depth_rd,
            v->h_disp, v->v_disp, s->override ? "monitor" : "no monitor");
    fprintf(stderr, "voodoo2: the last %u accesses, oldest first:\n", n);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (s->ring_n - n + k) % ARRAY_SIZE(s->ring);

        fprintf(stderr, "voodoo2:   %s %u %06x %08x\n",
                s->ring[i].write ? "wr" : "rd", s->ring[i].size,
                s->ring[i].addr, s->ring[i].val);
    }
}

/* -------------------------------------------------- the command FIFO in RAM
 *
 * With the ring as RAM the device sees none of the guest's packet writes,
 * and 3dfx's Glide tells the chip nothing either: it leaves hole counting
 * on (cvg/init/util.c), i.e. the chip executes whatever it has seen written
 * contiguously from its read pointer. So the device finds out itself, at
 * the guest's next access to anything else on the card -- a status poll, a
 * register, the LFB. The guest has finished its stores by then, and Glide
 * writes a packet whole before it touches the card again, so every header
 * found from the last one counted on is a packet ready to run: its words
 * go to 86Box's consumer as the depth the per-dword writes used to add.
 *
 * Where the guest has not written yet is told by a poison header: a word
 * the consumer has taken is set to 0xffffffff (packet type 7, which does
 * not exist) before the guest can learn that its slot is free -- the only
 * way it learns that is reading cmdFifoRdPtr, and that read is answered
 * here, after the poisoning, with the pointer it poisoned up to. A packet
 * this cannot follow (a JSR, AGP, a Banshee type) is warned about once and
 * the window goes back to MMIO; ramfifo=off is the A/B.
 */
#define VOODOO2_FIFO_WIN     0x200000
#define VOODOO2_FIFO_WIN_MAX 0x40000    /* the window decodes addr & 0x3fffc */
#define VOODOO2_FIFO_POISON  0xffffffffu

static inline uint32_t *
voodoo2_fifo_word(voodoo_t *v, uint32_t a)
{
    return (uint32_t *) &v->fb_mem[a & v->fb_mask];
}

/* the words a packet takes, header included, exactly as 86Box's consumer
 * takes them (vid_voodoo_fifo.c); 0 for one this does not follow */
static uint32_t
voodoo2_packet_words(uint32_t h)
{
    uint32_t pv;
    uint32_t n;

    switch (h & 7) {
    case 0:
        switch ((h >> 3) & 7) {
        case 0:     /* NOP */
        case 3:     /* JMP to the frame buffer: the caller follows it */
            return 1;
        default:    /* JSR/RET (a subroutine elsewhere), JMP AGP, reserved */
            return 0;
        }
    case 1:
        return 1 + (h >> 16);
    case 2:
        return 1 + ctpop32(h >> 3);
    case 3:
        pv = 2;                                         /* x, y */
        if (h & (1 << 10)) {
            pv += (h & (1u << 28)) ? 1 : 3;             /* packed ARGB or RGB */
        }
        if ((h & (1 << 11)) && !(h & (1u << 28))) {
            pv++;                                       /* alpha */
        }
        pv += !!(h & (1 << 12)) + !!(h & (1 << 13)) + !!(h & (1 << 14));
        pv += 2 * !!(h & (1 << 15));                    /* s0, t0 */
        pv += !!(h & (1 << 16));                        /* w1 */
        pv += 2 * !!(h & (1 << 17));                    /* s1, t1 */
        return 1 + ((h >> 6) & 0xf) * pv + ((h >> 29) & 7);
    case 4:
        return 1 + ctpop32((h >> 15) & 0x3fff) + ((h >> 29) & 7);
    case 5:
        n = (h >> 3) & 0x7ffff;
        return 2 + (n ? n : 1);
    default:
        return 0;
    }
}

static void voodoo2_fifo_map(Voodoo2State *s);

static void
voodoo2_fifo_break(Voodoo2State *s, uint32_t a, uint32_t h, const char *why)
{
    s->fifo_broken = true;
    warn_report("voodoo2: command FIFO back to MMIO: %s (header %08x at %08x, "
                "ring %08x+%x); ramfifo=off is the A/B", why, h, a,
                s->fifo_base, s->fifo_size);
    voodoo2_fifo_map(s);
}

/* count every whole packet the guest has written since the last call */
static void
voodoo2_fifo_sync(Voodoo2State *s)
{
    voodoo_t *v     = s->v;
    uint32_t  end   = s->fifo_base + s->fifo_size;
    uint32_t  a     = s->fifo_parse;
    uint32_t  words = 0;

    if (a < s->fifo_base || a >= end) {
        return;     /* the read pointer is not in the ring yet */
    }
    s->fifo_syncs++;
    while (words < s->fifo_size / 4) {
        uint32_t h = *voodoo2_fifo_word(v, a);
        uint32_t n;

        if (h == VOODOO2_FIFO_POISON) {
            break;
        }
        n = voodoo2_packet_words(h);
        if (!n) {
            voodoo2_fifo_break(s, a, h, "a packet it does not follow");
            break;
        }
        if ((h & 0x3f) == 0x18) {       /* JMP */
            uint32_t to = (h >> 4) & 0xfffffc;

            if (to < s->fifo_base || to >= end) {
                voodoo2_fifo_break(s, a, h, "a jump out of the ring");
                break;
            }
            words++;
            a = to;
            continue;
        }
        if (a + 4 * n > end) {
            voodoo2_fifo_break(s, a, h, "a packet across the ring's end");
            break;
        }
        words += n;
        a     += 4 * n;
    }
    s->fifo_parse = a;
    if (words) {
        smp_wmb();      /* the words before the depth that says they are there */
        v->cmdfifo_depth_wr += words;
        s->fifo_words       += words;
        voodoo_wake_fifo_thread_now(v);
    }
}

/* the guest asks where the chip is: poison what it has taken since the last
 * time, then tell -- the guest never writes past what it was told */
static uint32_t
voodoo2_fifo_rdptr(Voodoo2State *s)
{
    voodoo_t *v   = s->v;
    uint32_t  rp  = qatomic_read(&v->cmdfifo_rp);
    uint32_t  end = s->fifo_base + s->fifo_size;
    uint32_t  a   = s->fifo_poisoned;

    if (rp < s->fifo_base || rp >= end || a < s->fifo_base || a >= end) {
        return rp;
    }
    for (uint32_t k = 0; a != rp && k < s->fifo_size / 4; k++) {
        *voodoo2_fifo_word(v, a) = VOODOO2_FIFO_POISON;
        a += 4;
        if (a >= end) {
            a = s->fifo_base;
        }
    }
    s->fifo_poisoned = rp;
    return rp;
}

/* a fresh ring: all poison, counted and poisoned from the read pointer */
static void
voodoo2_fifo_restart(Voodoo2State *s)
{
    voodoo_t *v = s->v;

    for (uint32_t a = s->fifo_base; a < s->fifo_base + s->fifo_size; a += 4) {
        *voodoo2_fifo_word(v, a) = VOODOO2_FIFO_POISON;
    }
    s->fifo_parse    = v->cmdfifo_rp;
    s->fifo_poisoned = v->cmdfifo_rp;
}

/* map the ring as RAM when the FIFO is on and the ring fits the window, and
 * back to MMIO when either stops being so */
static void
voodoo2_fifo_map(Voodoo2State *s)
{
    voodoo_t *v    = s->v;
    uint32_t  base = v->cmdfifo_base;
    uint32_t  size = v->cmdfifo_end + 0x1000 - base;
    bool      want = s->ramfifo && s->fb_86box && !s->fifo_broken &&
                     v->cmdfifo_enabled && !v->cmdfifo_in_agp &&
                     v->cmdfifo_end >= base && size <= VOODOO2_FIFO_WIN_MAX &&
                     base + size <= v->fb_mask + 1;

    if (want == s->fifo_mapped &&
        (!want || (base == s->fifo_base && size == s->fifo_size))) {
        return;
    }
    memory_region_transaction_begin();
    if (want) {
        s->fifo_base = base;
        s->fifo_size = size;
        voodoo2_fifo_restart(s);
        memory_region_set_alias_offset(&s->fifo_win, base);
        memory_region_set_size(&s->fifo_win, size);
    }
    memory_region_set_enabled(&s->fifo_win, want);
    memory_region_transaction_commit();
    s->fifo_mapped = want;
    info_report("voodoo2: command FIFO %s (ring %08x+%x)",
                want ? "in RAM" : "through MMIO", base, size);
}

/* after a trapped write: the FIFO's own registers may have moved the ring */
static void
voodoo2_fifo_after_write(Voodoo2State *s, hwaddr addr)
{
    if (addr >= 0x400000 || !s->ramfifo || !s->fb_86box) {
        return;
    }
    if (addr & VOODOO2_FIFO_WIN) {
        if (s->fifo_mapped && !s->fifo_stray_warned) {
            s->fifo_stray_warned = true;
            warn_report("voodoo2: a command-FIFO write at %06x, outside the "
                        "ring mapped as RAM (%08x+%x)", (unsigned) addr,
                        s->fifo_base, s->fifo_size);
        }
        return;
    }
    switch (addr & 0x3fc) {
    case SST_cmdFifoRdPtr:
    case SST_cmdFifoDepth:
        if (s->fifo_mapped) {
            voodoo2_fifo_restart(s);
        }
        /* fall through */
    case SST_cmdFifoBaseAddr:
    case SST_fbiInit7:
        voodoo2_fifo_map(s);
        break;
    default:
        break;
    }
}

static uint64_t
voodoo2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    uint64_t      val;

    if (addr < 0x400000) {
        s->rd_hist[(addr >> 2) & 0xff]++;
    }
    if (s->fifo_mapped) {
        voodoo2_fifo_sync(s);
        if (size == 4 && addr < VOODOO2_FIFO_WIN &&
            (addr & 0x3fc) == SST_cmdFifoRdPtr && s->fifo_mapped) {
            val = voodoo2_fifo_rdptr(s);
            voodoo2_note(s, addr, val, size, false);
            return val;
        }
    }
    switch (size) {
    case 4:
        val = v->mapping.read_l((uint32_t) addr, v->mapping.priv);
        break;
    case 2:
        val = v->mapping.read_w((uint32_t) addr, v->mapping.priv);
        break;
    default:
        /* the card has no byte lane: 86Box registers no byte handler */
        val = 0xff;
        break;
    }
    voodoo2_note(s, addr, val, size, false);
    return val;
}

static void
voodoo2_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    if (s->fifo_mapped) {
        /* what the guest put in the ring comes before this write */
        voodoo2_fifo_sync(s);
    }
    if (addr < 0x400000) {
        s->wr_hist[(addr >> 2) & 0xff]++;
    }
    voodoo2_note(s, addr, val, size, true);
    if ((addr & 0x200000) && addr < 0x400000 && !v->cmdfifo_enabled &&
        (addr & 0x1fffff) >= 0x100) {
        /* THE teardown bug (2026-09-12): a command-FIFO packet written to
         * the 0x200000 window while the FIFO is off. With the FIFO off that
         * window is the legacy register map, so 86Box decodes each packet
         * dword as the register at bits 9:2 -- garbage into videoDimensions
         * (v_disp -> 0, the display timer breaks and Glide's vsync wait
         * hangs), triangleCMD (garbage geometry keeps the card busy), and
         * fbiInit7 itself (a dword with bit 8 set spuriously turns the FIFO
         * back on). It happens because 3dfx's Glide, on a window reopen,
         * keeps streaming to the FIFO ring while sst1InitRegisters has just
         * reset fbiInit7 to its default (FIFO off) and nothing re-enabled
         * it. Not dropped yet -- the fix (match the chip, or drop) is the
         * open question; this names it. */
        s->fifo_off_writes++;
        if (!s->fifo_off_warned) {
            s->fifo_off_warned = true;
            warn_report("voodoo2: command-FIFO packet %08x to the window at "
                        "%06x with the FIFO off -> decoded as register %03x "
                        "(Glide streaming to a reset FIFO: the teardown hang)",
                        (unsigned) val, (unsigned) addr, (unsigned) (addr & 0x3fc));
        }
    }
    if (addr < 0x400000 && (addr & 0x3fc) == 0x214 &&
        !((addr & 0x200000) && v->cmdfifo_enabled)) {
        /* fbiInit1 bit 23, scanline interleaving: this device is one card
         * with no partner, so the bit is not writable -- 86Box's display
         * timer takes SLI at its word and draws the odd lines from a second
         * card that does not exist (a NULL, 2026-09-12: 3dfx's Glide on a
         * grSstWinClose/grSstWinOpen pushes its reopen's register writes
         * through the command-FIFO transport with the FIFO off, and the
         * stream walks the register file, fbiInit1 included). A real single
         * board with the bit set shows half its lines; this one ignores it. */
        val &= ~(1u << 23);
    }
    switch (size) {
    case 4:
        v->mapping.write_l((uint32_t) addr, (uint32_t) val, v->mapping.priv);
        break;
    case 2:
        v->mapping.write_w((uint32_t) addr, (uint16_t) val, v->mapping.priv);
        break;
    default:
        break;
    }
    voodoo2_fifo_after_write(s, addr);
}

static const MemoryRegionOps voodoo2_mmio_ops = {
    .read       = voodoo2_mmio_read,
    .write      = voodoo2_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* a vector store to the LFB is split into dwords; a byte is answered
     * the way the hardware answers it (nothing) */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------- configuration space
 *
 * The PCI core owns the header (vendor, class, the BAR, the command
 * register). 86Box's handlers own 0x40-0x43, initEnable, whose byte 1 also
 * carries the "I am a Voodoo 2" strap the 3dfx driver reads; the command
 * register and the BAR's top byte are echoed so the card's own notion of
 * being enabled and mapped stays in step.
 */
/* 0x54, siProcess: the Voodoo 2's silicon-process monitor. Glide's
 * sst1InitMeasureSiProcess (cvg/init/util.c) loads a PCI-clock countdown
 * into bits 27:16, sets RUN (bit 28), polls the register until that field
 * reads zero and then takes the ring-oscillator count out of bits 15:0.
 * QEMU's configuration space keeps whatever a guest writes past the 64-byte
 * header, so the loaded count read back for ever and 3dfx's glide2x.dll spun
 * there on every grSstWinOpen (2026-09-12: GLIDETEST and Diablo II's video
 * test "froze"). 86Box answers 0 to the whole register, which ends the loop
 * with a count of 0 ("a very slow process": the shorter clock delay); here
 * the countdown is over the moment RUN is read back, and the count is a
 * production board's, well past Glide's 5000 threshold. 0x40-0x43 is
 * initEnable, 86Box's. */
#define VOODOO2_CFG_SIPROCESS  0x54
#define SIPROCESS_OSC_CNTR     0x0000ffffu
#define SIPROCESS_PCI_CNTR     0x0fff0000u
#define SIPROCESS_RUN          (1u << 28)
#define SIPROCESS_OSC_COUNT    8000u

static uint32_t
voodoo2_siprocess(PCIDevice *dev)
{
    uint32_t si = pci_get_long(dev->config + VOODOO2_CFG_SIPROCESS);

    if (si & SIPROCESS_RUN) {
        return (si & ~(SIPROCESS_PCI_CNTR | SIPROCESS_OSC_CNTR)) | SIPROCESS_OSC_COUNT;
    }
    return si & ~SIPROCESS_OSC_CNTR;   /* held in reset: nothing counted */
}

static uint32_t
voodoo2_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    Voodoo2State *s   = VOODOO2(dev);
    uint32_t      val = pci_default_read_config(dev, addr, len);

    s->cfg_hist[(addr >> 2) & 63]++;
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;
        int      b = -1;

        if (a >= 0x40 && a <= 0x43) {
            b = voodoo_pci_read(0, (int) a, 1, s->v);
        } else if (a >= VOODOO2_CFG_SIPROCESS && a < VOODOO2_CFG_SIPROCESS + 4) {
            b = (voodoo2_siprocess(dev) >> (8 * (a - VOODOO2_CFG_SIPROCESS))) & 0xff;
        }
        if (b >= 0) {
            val &= ~(0xffu << (8 * i));
            val |= (uint32_t) b << (8 * i);
        }
    }
    return val;
}

static void
voodoo2_config_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
    Voodoo2State *s = VOODOO2(dev);

    pci_default_write_config(dev, addr, val, len);
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;

        if (a == PCI_COMMAND || a == 0x13 || (a >= 0x40 && a <= 0x43)) {
            voodoo_pci_write(0, (int) a, 1, (val >> (8 * i)) & 0xff, s->v);
        }
    }
    if (addr <= 0x43 && addr + len > 0x40) {
        info_report("voodoo2: initEnable <= %08x", s->v->initEnable);
    }
}

/* --------------------------------------------------------------- display */

static QemuConsole *
voodoo2_console(Voodoo2State *s)
{
    if (!s->con) {
        s->con = qemu_console_lookup_by_index(0);
    }
    if (!s->con && !s->no_console_warned) {
        s->no_console_warned = true;
        warn_report("voodoo2: no VGA console to pass through (-vga none "
                    "and no display adapter?): frames go nowhere");
    }
    return s->con;
}

/* fbiInit0 VGA_PASS: vCPU thread, BQL held */
static void
voodoo2_set_override(void *opaque, int on)
{
    Voodoo2State *s   = opaque;
    QemuConsole  *con = voodoo2_console(s);

    on = (on != 0);
    if (on == s->override) {
        return;
    }
    s->override = on;
    s->shown_w  = 0;
    s->shown_h  = 0;
    if (!con) {
        return;
    }
    graphic_hw_passthrough(con, on);
    if (!on) {
        /* the VGA draws again: make it start from a full frame, into a
         * surface of its own (ours is freed when it replaces it) */
        s->surface = NULL;
        graphic_hw_invalidate(con);
        graphic_hw_update(con);
    }
    info_report("voodoo2: display %s", on ? "on (VGA pass-through)" : "off (VGA back)");
}

/* end of a frame with dirty lines: main loop (display timer), BQL held */
static void
voodoo2_present(void *opaque, const bitmap_t *frame, int w, int h)
{
    Voodoo2State   *s   = opaque;
    QemuConsole    *con = voodoo2_console(s);
    DisplaySurface *cur;
    uint8_t        *dst;
    int             stride;

    if (!con || !s->override) {
        return;
    }
    if (w <= 0 || h <= 0 || w > frame->w || h > frame->h) {
        return;
    }
    if (w != s->shown_w || h != s->shown_h) {
        /* the card just took the monitor, or its mode changed */
        s->shown_w       = w;
        s->shown_h       = h;
        s->blank         = true;
        s->blank_swapped = false;
        s->blank_front   = s->v->front_offset;
        s->blank_frames  = s->v->frame_count;
        s->blank_since   = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    }
    if (s->blank) {
        int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->blank_since;

        if (s->blank_swapped || ms >= VOODOO2_BLANK_MS) {
            /* a swap marks every line dirty, so the frame after the one it
             * landed in is all the new buffer's */
            s->blank = false;
            info_report("voodoo2: %dx%d shown after %" PRId64 " ms of black (%s)",
                        w, h, ms, s->blank_swapped ? "first swap" : "no swap");
        } else if (s->v->front_offset != s->blank_front ||
                   s->v->frame_count != s->blank_frames) {
            s->blank_swapped = true;
        }
        if (s->blank) {
            /* redraw every line next frame: a frame is only presented when
             * lines are dirty, and a guest that swaps once and then draws
             * nothing would otherwise leave the console black for good */
            memset(s->v->dirty_line, 1, sizeof(s->v->dirty_line));
        }
    }
    cur = qemu_console_surface(con);
    if (cur != s->surface || !cur ||
        surface_width(cur) != w || surface_height(cur) != h) {
        s->surface = qemu_create_displaysurface(w, h);
        dpy_gfx_replace_surface(con, s->surface);
    }
    dst    = surface_data(s->surface);
    stride = surface_stride(s->surface);
    for (int y = 0; y < h; y++) {
        if (s->blank) {
            memset(dst + (size_t) y * stride, 0, (size_t) w * 4);
        } else {
            memcpy(dst + (size_t) y * stride, frame->line[y], (size_t) w * 4);
        }
    }
    dpy_gfx_update_full(con);
    s->frames++;
    if (s->v->front_offset != s->shown_front) {
        s->shown_front = s->v->front_offset;
        s->shown++;
    }
}

/* ----------------------------------------------------------------- stats */

/* the four most-hit registers of a histogram, as " 0x000:N ..." */
static void
voodoo2_top_regs(uint32_t *hist, int count, char *buf, size_t len)
{
    size_t n = 0;

    buf[0] = 0;
    for (int k = 0; k < 4; k++) {
        int best = -1;

        for (int i = 0; i < count; i++) {
            if (hist[i] && (best < 0 || hist[i] > hist[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(buf + n, len - n, " 0x%03x:%u", best << 2, hist[best]);
        hist[best] = 0;
        if (n >= len) {
            break;
        }
    }
    memset(hist, 0, count * sizeof(*hist));
}

static void
voodoo2_stats(void *opaque)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;
    uint32_t      frames = s->frames - s->last_frames;
    int           tris   = v->tri_count - s->last_tris;
    int           wr     = v->wr_count - s->last_wr;
    int           rd     = v->rd_count - s->last_rd;
    int           tex    = v->tex_count - s->last_tex;

    if (frames || tris || wr || rd || voodoo_shim_fatals != s->last_fatals ||
        s->fifo_off_writes != s->last_fifo_off) {
        char rds[64], wrs[64], cfg[64], ref[48] = "", busy[96] = "", ram[64] = "";
        int  written = v->cmd_written + v->cmd_written_fifo + v->cmd_written_fifo_2;
        int  outstanding = written - v->cmd_read;
        int  is_busy = outstanding ||
            (v->cmdfifo_depth_rd != v->cmdfifo_depth_wr) || v->cmdfifo_in_sub ||
            v->voodoo_busy ||
            RENDER_VOODOO_BUSY(v, 0) ||
            (v->render_threads >= 2 && RENDER_VOODOO_BUSY(v, 1)) ||
            (v->render_threads == 4 && (RENDER_VOODOO_BUSY(v, 2) || RENDER_VOODOO_BUSY(v, 3)));

        /* the guest is polling status and the card is busy: name what the
         * status register's busy bit is reading, so a spin says why */
        if (is_busy && rd > 100000 && s->rd_hist[0] * 4 > (uint32_t) rd) {
            snprintf(busy, sizeof(busy),
                     "; busy: %d cmds outstanding (wr %d rd %d), fifo depth %u/%u%s%s%s",
                     outstanding, written, v->cmd_read,
                     (unsigned) v->cmdfifo_depth_rd, (unsigned) v->cmdfifo_depth_wr,
                     v->voodoo_busy ? ", voodoo_busy" : "",
                     RENDER_VOODOO_BUSY(v, 0) ? ", render0" : "",
                     (v->render_threads >= 2 && RENDER_VOODOO_BUSY(v, 1)) ? ", render1" : "");
        }

        voodoo2_top_regs(s->rd_hist, 256, rds, sizeof(rds));
        voodoo2_top_regs(s->wr_hist, 256, wrs, sizeof(wrs));
        voodoo2_top_regs(s->cfg_hist, 64, cfg, sizeof(cfg));
        if (voodoo_shim_fatals != s->last_fatals || s->fifo_off_writes != s->last_fifo_off) {
            snprintf(ref, sizeof(ref), "; %u refused, %u fifo-off",
                     voodoo_shim_fatals - s->last_fatals,
                     s->fifo_off_writes - s->last_fifo_off);
        }
        s->last_fifo_off = s->fifo_off_writes;
        if (s->fifo_mapped || s->fifo_words != s->last_fifo_words) {
            snprintf(ram, sizeof(ram), "; FIFO in RAM: %u words in %u syncs",
                     s->fifo_words - s->last_fifo_words,
                     s->fifo_syncs - s->last_fifo_syncs);
        }
        s->last_fifo_words = s->fifo_words;
        s->last_fifo_syncs = s->fifo_syncs;
        /* frames = presents (a scan-out with any dirty line); new = those
         * showing a buffer the last one did not, i.e. the game's frame rate */
        info_report("voodoo2: %dx%d %s: %u frames (%u new), %d triangles, "
                    "%d writes (%d texture), %d reads in %.1f s; regs read%s; "
                    "written%s; config read%s%s%s%s",
                    v->h_disp, v->v_disp, s->override ? "on" : "off",
                    frames, s->shown - s->last_shown, tris, wr, tex, rd,
                    VOODOO2_STATS_MS / 1000.0,
                    rds[0] ? rds : " none", wrs[0] ? wrs : " none",
                    cfg[0] ? cfg : " none", ref, busy, ram);
    }
    s->last_fatals = voodoo_shim_fatals;
    s->last_frames = s->frames;
    s->last_shown  = s->shown;
    s->last_tris   = v->tri_count;
    s->last_wr     = v->wr_count;
    s->last_rd     = v->rd_count;
    s->last_tex    = v->tex_count;
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VOODOO2_STATS_MS);
}

/* ---------------------------------------------------------------- device */

static bool voodoo2_instantiated;

static void
voodoo2_realize(PCIDevice *dev, Error **errp)
{
    Voodoo2State   *s = VOODOO2(dev);
    VoodooShimHooks hooks = {
        .opaque       = s,
        .set_override = voodoo2_set_override,
        .on_fatal     = voodoo2_on_fatal,
        .present      = voodoo2_present,
    };

    if (voodoo2_instantiated) {
        /* 86Box's primary-SVGA hook and the shim's monitor are one per
         * process; SLI is not modelled (fbiInit7 has no second card) */
        error_setg(errp, "voodoo2: only one Voodoo 2 per machine");
        return;
    }
    if (s->fbmem_mb != 2 && s->fbmem_mb != 4) {
        error_setg(errp, "voodoo2: fbmem must be 2 or 4 (MB)");
        return;
    }
    if (s->texmem_mb != 2 && s->texmem_mb != 4) {
        error_setg(errp, "voodoo2: texmem must be 2 or 4 (MB, per TMU)");
        return;
    }
    if (s->threads != 1 && s->threads != 2 && s->threads != 4) {
        error_setg(errp, "voodoo2: threads must be 1, 2 or 4");
        return;
    }
    voodoo2_instantiated = true;

    voodoo2_trace = getenv("VOODOO2_TRACE") && *getenv("VOODOO2_TRACE") == '1';
    voodoo_shim_init(&hooks);
    voodoo_shim_set_config("bilinear", s->bilinear);
    voodoo_shim_set_config("dithersub", s->dithersub);
    voodoo_shim_set_config("dacfilter", s->filter);
    voodoo_shim_set_config("texture_memory", (int) s->texmem_mb);
    voodoo_shim_set_config("framebuffer_memory", (int) s->fbmem_mb);
    voodoo_shim_set_config("render_threads", (int) s->threads);
    voodoo_shim_set_config("recompiler", s->recompiler);
    voodoo_shim_set_config("sli", 0);

    s->set = voodoo_init(NULL);
    s->v   = s->set->voodoos[0];
    /* the per-scanline display timer, coalesced (shim/86box/timer.h) */
    s->v->timer.slack_ns = 1000000;

    memory_region_init_io(&s->mmio, OBJECT(s), &voodoo2_mmio_ops, s,
                          "voodoo2.mmio", VOODOO2_BAR_SIZE);
    if (s->ramfifo) {
        /* the frame buffer page-aligned, so a slice of it can be RAM to the
         * guest (86Box's own is a calloc, 16 bytes past a page, which a KVM
         * memory slot refuses); nothing has used 86Box's yet, and it goes
         * back before voodoo_close() frees it */
        uint8_t *fb = qemu_memalign(qemu_real_host_page_size(), 4 * MiB);

        memset(fb, 0, 4 * MiB);
        s->fb_86box  = s->v->fb_mem;
        s->v->fb_mem = fb;
        memory_region_init_ram_ptr(&s->fb_ram, OBJECT(s), "voodoo2.fb",
                                   4 * MiB, fb);
        memory_region_init_alias(&s->fifo_win, OBJECT(s), "voodoo2.cmdfifo",
                                 &s->fb_ram, 0, 0x1000);
        memory_region_set_enabled(&s->fifo_win, false);
        memory_region_add_subregion_overlap(&s->mmio, VOODOO2_FIFO_WIN,
                                            &s->fifo_win, 1);
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    /* initEnable: writable (the read is answered by 86Box's handler) */
    for (int a = 0x40; a <= 0x43; a++) {
        dev->wmask[a] = 0xff;
    }
    /* a reference board: no subsystem IDs (QEMU's default is Red Hat's) */
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, 0);

    s->stats = timer_new_ms(QEMU_CLOCK_VIRTUAL, voodoo2_stats, s);
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VOODOO2_STATS_MS);

    info_report("voodoo2: %u MB frame buffer, %u MB per TMU, %u render "
                "thread%s, recompiler %s",
                s->fbmem_mb, s->texmem_mb, s->threads, s->threads > 1 ? "s" : "",
                s->recompiler ? "on" : "off");
}

static void
voodoo2_exit(PCIDevice *dev)
{
    Voodoo2State *s = VOODOO2(dev);

    if (s->stats) {
        timer_free(s->stats);
        s->stats = NULL;
    }
    if (s->set) {
        voodoo2_set_override(s, 0);
        if (s->fb_86box) {
            qemu_vfree(s->v->fb_mem);
            s->v->fb_mem = s->fb_86box;
            s->fb_86box  = NULL;
        }
        voodoo_close(s->set);
        s->set = NULL;
        s->v   = NULL;
    }
    voodoo2_instantiated = false;
}

/* A system reset: 86Box has no reset entry for the card (the driver
 * re-initialises it), but the BAR is gone and the monitor must go back to
 * the VGA, or a rebooting guest stares at the last frame. */
static void
voodoo2_reset(DeviceState *dev)
{
    Voodoo2State *s = VOODOO2(dev);
    voodoo_t     *v = s->v;

    if (!v) {
        return;
    }
    thread_wait_mutex(v->force_blit_mutex);
    v->can_blit         = 0;
    v->force_blit_count = 0;
    thread_release_mutex(v->force_blit_mutex);
    v->fbiInit0        = 0;
    v->fbiInit7        = 0;
    v->cmdfifo_enabled = 0;
    v->initEnable      = 0;
    v->pci_enable      = 0;
    v->memBaseAddr     = 0;
    voodoo2_set_override(s, 0);
    s->fifo_broken = false;
    voodoo2_fifo_map(s);
}

static Property voodoo2_properties[] = {
    DEFINE_PROP_UINT32("fbmem", Voodoo2State, fbmem_mb, 4),
    DEFINE_PROP_UINT32("texmem", Voodoo2State, texmem_mb, 4),
    DEFINE_PROP_UINT32("threads", Voodoo2State, threads, 2),
    DEFINE_PROP_BOOL("bilinear", Voodoo2State, bilinear, true),
    DEFINE_PROP_BOOL("dither-sub", Voodoo2State, dithersub, true),
    DEFINE_PROP_BOOL("filter", Voodoo2State, filter, false),
    DEFINE_PROP_BOOL("recompiler", Voodoo2State, recompiler, true),
    DEFINE_PROP_BOOL("ramfifo", Voodoo2State, ramfifo, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void
voodoo2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);

    k->realize      = voodoo2_realize;
    k->exit         = voodoo2_exit;
    k->config_read  = voodoo2_config_read;
    k->config_write = voodoo2_config_write;
    k->vendor_id    = VOODOO2_PCI_VENDOR;
    k->device_id    = VOODOO2_PCI_DEVICE;
    k->revision     = 0x02;
    k->class_id     = PCI_CLASS_MULTIMEDIA_VIDEO;
    dc->desc        = "3dfx Voodoo 2 (86Box's emulation)";
    dc->hotpluggable = false;
    device_class_set_legacy_reset(dc, voodoo2_reset);
    device_class_set_props(dc, voodoo2_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo voodoo2_info = {
    .name          = TYPE_VOODOO2,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Voodoo2State),
    .class_init    = voodoo2_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void
voodoo2_register_types(void)
{
    type_register_static(&voodoo2_info);
}

type_init(voodoo2_register_types)

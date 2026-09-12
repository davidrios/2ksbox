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
 *                  [,recompiler=on|off]
 *
 * Threads: the guest's MMIO writes run on the vCPU thread with the BQL; the
 * display timer on the main loop with the BQL (86Box has both on its CPU
 * thread, which is the invariant that matters); the FIFO thread and the
 * render threads are 86Box's own and touch nothing of QEMU's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
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
#include "voodoo_shim.h"

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

    /* the 5 s activity line */
    QEMUTimer *stats;
    uint32_t   last_frames;
    int        last_tris;
    int        last_wr;
    int        last_rd;
    int        last_tex;
    unsigned   last_fatals;
    uint32_t   fifo_off_writes;   /* FIFO-window packets decoded as registers */
    uint32_t   last_fifo_off;
    bool       fifo_off_warned;
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

static uint64_t
voodoo2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    uint64_t      val;

    if (addr < 0x400000) {
        s->rd_hist[(addr >> 2) & 0xff]++;
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
    cur = qemu_console_surface(con);
    if (cur != s->surface || !cur ||
        surface_width(cur) != w || surface_height(cur) != h) {
        s->surface = qemu_create_displaysurface(w, h);
        dpy_gfx_replace_surface(con, s->surface);
    }
    dst    = surface_data(s->surface);
    stride = surface_stride(s->surface);
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t) y * stride, frame->line[y], (size_t) w * 4);
    }
    dpy_gfx_update_full(con);
    s->frames++;
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
        char rds[64], wrs[64], cfg[64], ref[48] = "", busy[96] = "";
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
        info_report("voodoo2: %dx%d %s: %u frames, %d triangles, %d writes "
                    "(%d texture), %d reads in %.1f s; regs read%s; written%s; "
                    "config read%s%s%s",
                    v->h_disp, v->v_disp, s->override ? "on" : "off",
                    frames, tris, wr, tex, rd, VOODOO2_STATS_MS / 1000.0,
                    rds[0] ? rds : " none", wrs[0] ? wrs : " none",
                    cfg[0] ? cfg : " none", ref, busy);
    }
    s->last_fatals = voodoo_shim_fatals;
    s->last_frames = s->frames;
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
}

static Property voodoo2_properties[] = {
    DEFINE_PROP_UINT32("fbmem", Voodoo2State, fbmem_mb, 4),
    DEFINE_PROP_UINT32("texmem", Voodoo2State, texmem_mb, 4),
    DEFINE_PROP_UINT32("threads", Voodoo2State, threads, 2),
    DEFINE_PROP_BOOL("bilinear", Voodoo2State, bilinear, true),
    DEFINE_PROP_BOOL("dither-sub", Voodoo2State, dithersub, true),
    DEFINE_PROP_BOOL("filter", Voodoo2State, filter, false),
    DEFINE_PROP_BOOL("recompiler", Voodoo2State, recompiler, true),
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

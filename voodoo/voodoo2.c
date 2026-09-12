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

static uint64_t
voodoo2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    switch (size) {
    case 4:
        return v->mapping.read_l((uint32_t) addr, v->mapping.priv);
    case 2:
        return v->mapping.read_w((uint32_t) addr, v->mapping.priv);
    default:
        /* the card has no byte lane: 86Box registers no byte handler */
        return 0xff;
    }
}

static void
voodoo2_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

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
static uint32_t
voodoo2_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    Voodoo2State *s   = VOODOO2(dev);
    uint32_t      val = pci_default_read_config(dev, addr, len);

    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;

        if (a >= 0x40 && a <= 0x43) {
            val &= ~(0xffu << (8 * i));
            val |= (uint32_t) voodoo_pci_read(0, (int) a, 1, s->v) << (8 * i);
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

    if (frames || tris || wr || rd) {
        info_report("voodoo2: %dx%d %s: %u frames, %d triangles, %d writes "
                    "(%d texture), %d reads in %.1f s",
                    v->h_disp, v->v_disp, s->override ? "on" : "off",
                    frames, tris, wr, tex, rd, VOODOO2_STATS_MS / 1000.0);
    }
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

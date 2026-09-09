/*
 * opl3 — the Yamaha YMF262 (OPL3) as a machine of the era had it (doc 20 §5).
 *
 * Overlaid into hw/audio/ by scripts/prepare-qemu.sh and instantiated by
 * patch 25. The synthesis is libsynth's (Nuked-OPL3); this file is the
 * ports, the clock and the audio voice.
 *
 * Why not QEMU's own `adlib`: that device is an OPL2 (YM3812) on the
 * 1990s MAME core, and a Sound Blaster 16 has an OPL3 — a game that
 * writes the second register file gets four-operator instruments there
 * and silence here. It also mirrors the chip at the Sound Blaster's own
 * base (2x0-2x3), which is where an SB-aware driver looks; `sbbase=0`
 * leaves the machine with a bare AdLib at 0x388 alone.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "audio/audio.h"
#include "qom/object.h"

#include "libsynth.h"

#define TYPE_OPL3 "opl3"
OBJECT_DECLARE_SIMPLE_TYPE(Opl3State, OPL3)

/* stereo signed 16-bit: what the engine renders and the voice takes */
#define OPL3_FRAME_BYTES 4
/* one render block; the mixer asks for far less than this per tick */
#define OPL3_BUF_BYTES   8192

struct Opl3State {
    ISADevice parent_obj;

    QEMUSoundCard card;
    SWVoiceOut *voice;
    libsynth_opl *chip;

    uint32_t port;      /* the AdLib pair, 0x388 */
    uint32_t sbbase;    /* the Sound Blaster mirror, 0x220, or 0 for none */
    uint32_t freq;      /* voice rate; the chip's own by default */

    /* Guest time already handed to the chip's timers. A detection
     * routine reads the status register in a delay loop, so the timers
     * have to move with the guest and not with the audio callback. */
    int64_t last_ns;
    int64_t rem_ns;     /* the sub-microsecond remainder, never dropped */

    /* Nothing is rendered until the guest touches a port: an idle
     * machine should not spend a core on a chip nobody programmed. */
    bool active;

    /* What the last render produced and the mixer has not taken yet. A
     * short write must not cost the frames that were already made. */
    uint8_t buf[OPL3_BUF_BYTES];
    int pending;
    int pos;

    PortioList port_list;
    PortioList sb_port_list;
};

/* Advance the chip's timers to now. */
static void opl3_sync(Opl3State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delta = now - s->last_ns + s->rem_ns;

    if (delta < 0) {
        delta = 0;
    }
    s->last_ns = now;
    s->rem_ns = delta % 1000;
    libsynth_opl_advance(s->chip, delta / 1000);
}

/* The four ports are the two register files: even = address, odd = data,
 * the upper pair being the OPL3 extension. Both the 0x388 pair and the
 * Sound Blaster mirror land here, and `nport & 3` is all that separates
 * them because the mirror is the same chip at another address. */
static void opl3_write(void *opaque, uint32_t nport, uint32_t val)
{
    Opl3State *s = opaque;
    int a = nport & 3;

    if (!s->active) {
        s->active = true;
        AUD_set_active_out(s->voice, 1);
    }
    opl3_sync(s);
    if (a & 1) {
        libsynth_opl_data(s->chip, a >> 1, val);
    } else {
        libsynth_opl_address(s->chip, a >> 1, val);
    }
}

static uint32_t opl3_read(void *opaque, uint32_t nport)
{
    Opl3State *s = opaque;

    /* Every address in the block reads the status register — which is
     * what the detection sequence reads, and it must reflect the timers
     * as of *now*, not as of the last audio tick. */
    opl3_sync(s);
    return libsynth_opl_status(s->chip);
}

static const MemoryRegionPortio opl3_portio_list[] = {
    { 0, 4, 1, .read = opl3_read, .write = opl3_write },
    PORTIO_END_OF_LIST(),
};

static void opl3_callback(void *opaque, int free)
{
    Opl3State *s = opaque;

    if (!s->active) {
        return;
    }
    opl3_sync(s);
    while (free >= OPL3_FRAME_BYTES) {
        int wrote;

        if (s->pending == 0) {
            int want = MIN(free, OPL3_BUF_BYTES);
            int frames = want / OPL3_FRAME_BYTES;

            libsynth_opl_render(s->chip, (int16_t *)s->buf, frames);
            s->pending = frames * OPL3_FRAME_BYTES;
            s->pos = 0;
        }
        wrote = AUD_write(s->voice, s->buf + s->pos, s->pending);
        if (wrote <= 0) {
            return;         /* keep what is left for the next tick */
        }
        s->pos += wrote;
        s->pending -= wrote;
        free -= wrote;
    }
}

static void opl3_realizefn(DeviceState *dev, Error **errp)
{
    Opl3State *s = OPL3(dev);
    struct audsettings as;

    if (libsynth_api_version() != LIBSYNTH_API_VERSION) {
        error_setg(errp, "libsynth is API version %u, this build wants %u"
                   " — rebuild it (scripts/configure-qemu.sh)",
                   libsynth_api_version(), LIBSYNTH_API_VERSION);
        return;
    }
    if (!AUD_register_card(TYPE_OPL3, &s->card, errp)) {
        return;
    }
    s->chip = libsynth_opl_new(s->freq);
    if (!s->chip) {
        error_setg(errp, "libsynth_opl_new(%u) failed", s->freq);
        return;
    }

    as.freq = s->freq;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.endianness = AUDIO_HOST_ENDIANNESS;
    s->voice = AUD_open_out(&s->card, s->voice, TYPE_OPL3, s, opl3_callback, &as);
    if (!s->voice) {
        error_setg(errp, "opl3: opening the audio voice failed");
        return;
    }

    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    portio_list_init(&s->port_list, OBJECT(s), opl3_portio_list, s, TYPE_OPL3);
    portio_list_add(&s->port_list, isa_address_space_io(&s->parent_obj), s->port);
    if (s->sbbase) {
        portio_list_init(&s->sb_port_list, OBJECT(s), opl3_portio_list, s,
                         "opl3-sb");
        portio_list_add(&s->sb_port_list, isa_address_space_io(&s->parent_obj),
                        s->sbbase);
    }
}

static void opl3_unrealizefn(DeviceState *dev)
{
    Opl3State *s = OPL3(dev);

    if (s->chip) {
        libsynth_opl_free(s->chip);
        s->chip = NULL;
    }
    AUD_remove_card(&s->card);
}

static Property opl3_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Opl3State, card),
    DEFINE_PROP_UINT32("iobase", Opl3State, port,   0x388),
    /* The Sound Blaster mirror. 0 turns it off, which is a bare AdLib. */
    DEFINE_PROP_UINT32("sbbase", Opl3State, sbbase, 0),
    /* The chip's own rate: at anything else the core resamples, and
     * QEMU's mixer would then be the second resampler in the path. */
    DEFINE_PROP_UINT32("freq",   Opl3State, freq,   LIBSYNTH_OPL_NATIVE_RATE),
    DEFINE_PROP_END_OF_LIST(),
};

static void opl3_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = opl3_realizefn;
    dc->unrealize = opl3_unrealizefn;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Yamaha YMF262 (OPL3)";
    device_class_set_props(dc, opl3_properties);
    /* No vmstate: a snapshot taken mid-note comes back with the chip at
     * power-on, like the cdimage medium's fields (patch 51). */
}

static const TypeInfo opl3_info = {
    .name          = TYPE_OPL3,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Opl3State),
    .class_init    = opl3_class_initfn,
};

static void opl3_register_types(void)
{
    type_register_static(&opl3_info);
}

type_init(opl3_register_types)

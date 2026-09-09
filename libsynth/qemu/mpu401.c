/*
 * mpu401 — the MIDI port of the era, and a synthesizer behind it
 * (doc 20 §5). QEMU has no MPU-401 of any kind, so a game offering
 * "General MIDI" or "Roland MT-32" in its setup program has had nothing
 * to talk to here.
 *
 * Overlaid into hw/audio/ by scripts/prepare-qemu.sh, instantiated by
 * patch 25. What plays the stream is libsynth's (a SoundFont bank or a
 * CM-32L); this file is the two ports, the ACK queue and the voice.
 *
 * Scope, deliberately: **UART mode**. Command 0x3F, and reset (0xFF)
 * answering 0xFE, is what DOS games, Windows 9x's own "MPU-401
 * Compatible" driver and XP's alike use, and it is the whole of the
 * hardware most cards ever implemented. The intelligent mode's
 * conductor — tracks, timing, its own metronome — is answered with an
 * ACK and nothing else; no software of the period this project is for
 * depends on it.
 *
 * There is no MIDI *in*: nothing here produces data for the guest to
 * read except the ACKs, so the status register only ever reports one
 * waiting when an ACK is, and the IRQ follows that. A real module's
 * replies would arrive the same way (doc 20 §8.2).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/isa/isa.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "audio/audio.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "libsynth.h"

#define TYPE_MPU401 "mpu401"
OBJECT_DECLARE_SIMPLE_TYPE(Mpu401State, MPU401)

#define MPU_FRAME_BYTES 4
#define MPU_BUF_BYTES   8192
#define MPU_QUEUE_SIZE  16

/* Status register (base+1, read). The two bits that mean anything are
 * active low, as they are on the hardware: 0x80 clear = a byte is
 * waiting to be read, 0x40 clear = the port will take a byte. The rest
 * read high, which is what DOSBox reports and what every driver
 * tolerates. */
#define MPU_STATUS_IDLE     0x3F
#define MPU_STATUS_NO_DATA  0x80

#define MPU_CMD_RESET       0xFF
#define MPU_CMD_UART        0x3F
#define MPU_ACK             0xFE

struct Mpu401State {
    ISADevice parent_obj;

    QEMUSoundCard card;
    SWVoiceOut *voice;
    libsynth_midi *midi;
    qemu_irq irq_line;

    uint32_t port;
    uint32_t irq;
    uint32_t gain;
    char *synth;        /* "none" | "gm" | "mt32" */
    char *soundfont;    /* synth=gm */
    char *romdir;       /* synth=mt32 */

    bool uart;
    uint8_t queue[MPU_QUEUE_SIZE];
    int queue_used;
    int queue_pos;

    bool active;
    uint8_t buf[MPU_BUF_BYTES];
    int pending;
    int pos;

    /* What the guest has been sending, reported every 5 s while it is
     * sending anything (`d3dpt-vga: N page flips in 5.0 s`'s habit).
     * The question this answers is the first one to ask of a game that
     * is silent: did it write to the port at all? */
    int64_t report_ns;
    unsigned bytes;
    unsigned notes;
    uint16_t channels;

    PortioList port_list;
};

static void mpu401_irq(Mpu401State *s)
{
    if (s->irq_line) {
        qemu_set_irq(s->irq_line, s->queue_used > 0);
    }
}

/* Queue a byte for the guest to read. The queue only ever holds ACKs,
 * so overflowing it means the guest stopped reading — drop rather than
 * grow, exactly as the hardware's own eight bytes do. */
static void mpu401_queue(Mpu401State *s, uint8_t byte)
{
    if (s->queue_used >= MPU_QUEUE_SIZE) {
        return;
    }
    s->queue[(s->queue_pos + s->queue_used) % MPU_QUEUE_SIZE] = byte;
    s->queue_used++;
    mpu401_irq(s);
}

static void mpu401_reset_port(Mpu401State *s)
{
    s->uart = false;
    s->queue_used = 0;
    s->queue_pos = 0;
    mpu401_irq(s);
    libsynth_midi_reset(s->midi);
}

static void mpu401_count(Mpu401State *s, uint8_t byte);

static uint32_t mpu401_read(void *opaque, uint32_t nport)
{
    Mpu401State *s = opaque;
    uint8_t ret;

    if (nport & 1) {
        return MPU_STATUS_IDLE | (s->queue_used ? 0 : MPU_STATUS_NO_DATA);
    }
    if (s->queue_used == 0) {
        /* Nothing to hand over: an undriven bus reads high. */
        return 0xFF;
    }
    ret = s->queue[s->queue_pos];
    s->queue_pos = (s->queue_pos + 1) % MPU_QUEUE_SIZE;
    s->queue_used--;
    mpu401_irq(s);
    return ret;
}

static void mpu401_write(void *opaque, uint32_t nport, uint32_t val)
{
    Mpu401State *s = opaque;

    if (nport & 1) {
        switch (val & 0xFF) {
        case MPU_CMD_RESET:
            /* Answered in both modes, and it leaves UART mode: this is
             * how a driver finds the port at all — write 0xFF, read
             * 0xFE back. */
            mpu401_reset_port(s);
            mpu401_queue(s, MPU_ACK);
            break;
        case MPU_CMD_UART:
            s->uart = true;
            mpu401_queue(s, MPU_ACK);
            break;
        default:
            /* An intelligent-mode command. Acknowledged so a driver
             * probing the port does not conclude the hardware is
             * broken, and otherwise not implemented. */
            if (!s->uart) {
                mpu401_queue(s, MPU_ACK);
            }
            break;
        }
        return;
    }

    if (!s->active) {
        s->active = true;
        if (s->voice) {
            AUD_set_active_out(s->voice, 1);
        }
    }
    /* The stream, byte by byte. Data written before the guest asked for
     * UART mode is played too: a driver that skips the handshake is
     * commoner than one that means something else by it. */
    libsynth_midi_write(s->midi, val & 0xFF);
    mpu401_count(s, val & 0xFF);
}

/* A note-on with a non-zero velocity is what "the guest is playing
 * music" looks like from here. Running status is why this counts a
 * status byte's channel rather than pairing bytes: the count is a sign
 * of life, not a transcript — libsynth's parser is the transcript. */
static void mpu401_count(Mpu401State *s, uint8_t byte)
{
    int64_t now;

    s->bytes++;
    if ((byte & 0xF0) == 0x90) {
        s->notes++;
        s->channels |= 1 << (byte & 0x0F);
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->report_ns == 0) {
        s->report_ns = now;
        return;
    }
    if (now - s->report_ns < 5 * NANOSECONDS_PER_SECOND) {
        return;
    }
    if (s->bytes) {
        unsigned n = 0;

        for (int i = 0; i < 16; i++) {
            n += (s->channels >> i) & 1;
        }
        info_report("mpu401: %u bytes, %u note-ons on %u channel%s in 5.0 s",
                    s->bytes, s->notes, n, n == 1 ? "" : "s");
    }
    s->report_ns = now;
    s->bytes = 0;
    s->notes = 0;
    s->channels = 0;
}

static const MemoryRegionPortio mpu401_portio_list[] = {
    { 0, 2, 1, .read = mpu401_read, .write = mpu401_write },
    PORTIO_END_OF_LIST(),
};

static void mpu401_callback(void *opaque, int free)
{
    Mpu401State *s = opaque;

    if (!s->active) {
        return;
    }
    while (free >= MPU_FRAME_BYTES) {
        int wrote;

        if (s->pending == 0) {
            int want = MIN(free, MPU_BUF_BYTES);
            int frames = want / MPU_FRAME_BYTES;

            libsynth_midi_render(s->midi, (int16_t *)s->buf, frames);
            s->pending = frames * MPU_FRAME_BYTES;
            s->pos = 0;
        }
        wrote = AUD_write(s->voice, s->buf + s->pos, s->pending);
        if (wrote <= 0) {
            return;
        }
        s->pos += wrote;
        s->pending -= wrote;
        free -= wrote;
    }
}

/* The General MIDI bank we ship, relative to a source tree — the last
 * candidate below, and the same shape as the Glide wrapper's
 * `build/glide/libglide2x.so` (patch 33): a build tree needs no
 * environment at all, and a package's player sets the variable. */
#define LIBSYNTH_SF2_IN_TREE "soundfonts/TimGM6mb.sf2"

/* `synth=` and the file or directory it needs.
 *
 * The bank is searched for the way QEMU searches for every other
 * companion of ours: the property, then LIBSYNTH_SF2 — which is what a
 * packaged player sets (player/src/companions.rs) — then the copy in a
 * checkout. So a machine that simply says `synth=gm` starts everywhere
 * this project is run from, and a bundle never has to carry an absolute
 * path into someone else's install. */
static int mpu401_engine(Mpu401State *s, const char **arg, Error **errp)
{
    const char *name = s->synth && s->synth[0] ? s->synth : "none";

    *arg = NULL;
    if (!strcmp(name, "none")) {
        return LIBSYNTH_MIDI_NONE;
    }
    if (!strcmp(name, "gm")) {
        *arg = s->soundfont && s->soundfont[0] ? s->soundfont
                                               : getenv("LIBSYNTH_SF2");
        if (!*arg && g_file_test(LIBSYNTH_SF2_IN_TREE, G_FILE_TEST_IS_REGULAR)) {
            *arg = LIBSYNTH_SF2_IN_TREE;
        }
        if (!*arg) {
            error_setg(errp, "mpu401: synth=gm found no SoundFont — give it"
                       " soundfont=<file>, or LIBSYNTH_SF2 in the"
                       " environment (a packaged player sets that itself)");
            return -1;
        }
        return LIBSYNTH_MIDI_GM;
    }
    if (!strcmp(name, "mt32")) {
        *arg = s->romdir && s->romdir[0] ? s->romdir
                                         : getenv("LIBSYNTH_MT32_ROMS");
        if (!*arg) {
            error_setg(errp, "mpu401: synth=mt32 needs romdir=<directory>"
                       " holding your own Roland CM-32L ROMs"
                       " (or LIBSYNTH_MT32_ROMS in the environment)");
            return -1;
        }
        return LIBSYNTH_MIDI_MT32;
    }
    error_setg(errp, "mpu401: synth=%s is not one of none, gm, mt32", name);
    return -1;
}

static void mpu401_realizefn(DeviceState *dev, Error **errp)
{
    Mpu401State *s = MPU401(dev);
    ISADevice *isa = ISA_DEVICE(dev);
    struct audsettings as;
    char err[256] = "";
    const char *arg;
    uint32_t rate;
    int kind;

    if (libsynth_api_version() != LIBSYNTH_API_VERSION) {
        error_setg(errp, "libsynth is API version %u, this build wants %u"
                   " — rebuild it (scripts/configure-qemu.sh)",
                   libsynth_api_version(), LIBSYNTH_API_VERSION);
        return;
    }
    kind = mpu401_engine(s, &arg, errp);
    if (kind < 0) {
        return;
    }
    s->midi = libsynth_midi_new(kind, arg, err, sizeof(err));
    if (!s->midi) {
        /* A machine that was asked for music and silently got none is
         * the failure this refuses to be. */
        error_setg(errp, "mpu401: %s", err[0] ? err : "the synthesizer failed to open");
        return;
    }
    libsynth_midi_gain(s->midi, s->gain);

    rate = libsynth_midi_rate(s->midi);
    if (rate) {
        as.freq = rate;
        as.nchannels = 2;
        as.fmt = AUDIO_FORMAT_S16;
        as.endianness = AUDIO_HOST_ENDIANNESS;
        /* Both failures below leave the device unrealized, so unrealize
         * will not run: the synthesizer is given back here. */
        if (!AUD_register_card(TYPE_MPU401, &s->card, errp)) {
            libsynth_midi_free(s->midi);
            s->midi = NULL;
            return;
        }
        s->voice = AUD_open_out(&s->card, s->voice, TYPE_MPU401, s,
                                mpu401_callback, &as);
        if (!s->voice) {
            libsynth_midi_free(s->midi);
            s->midi = NULL;
            AUD_remove_card(&s->card);
            error_setg(errp, "mpu401: opening the audio voice failed");
            return;
        }
    }

    if (s->irq <= 15) {
        s->irq_line = isa_get_irq(isa, s->irq);
    }
    mpu401_reset_port(s);
    portio_list_init(&s->port_list, OBJECT(s), mpu401_portio_list, s,
                     TYPE_MPU401);
    portio_list_add(&s->port_list, isa_address_space_io(isa), s->port);
}

static void mpu401_unrealizefn(DeviceState *dev)
{
    Mpu401State *s = MPU401(dev);

    if (s->midi) {
        libsynth_midi_free(s->midi);
        s->midi = NULL;
    }
    if (s->voice) {
        AUD_remove_card(&s->card);
    }
}

static Property mpu401_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Mpu401State, card),
    DEFINE_PROP_UINT32("iobase", Mpu401State, port, 0x330),
    /* The MPU-401's own line is IRQ 2/9. Anything above 15 is "no
     * interrupt", which is enough for output-only use — every DOS game
     * of the period polls the status register instead. */
    DEFINE_PROP_UINT32("irq",    Mpu401State, irq,  9),
    DEFINE_PROP_UINT32("gain",   Mpu401State, gain, 100),
    DEFINE_PROP_STRING("synth",     Mpu401State, synth),
    DEFINE_PROP_STRING("soundfont", Mpu401State, soundfont),
    DEFINE_PROP_STRING("romdir",    Mpu401State, romdir),
    DEFINE_PROP_END_OF_LIST(),
};

static void mpu401_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mpu401_realizefn;
    dc->unrealize = mpu401_unrealizefn;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Roland MPU-401 MIDI interface (UART mode)";
    device_class_set_props(dc, mpu401_properties);
    /* No vmstate, like opl3 and the cdimage medium's fields: a snapshot
     * resumes with the port reset and nothing sounding. */
}

static const TypeInfo mpu401_info = {
    .name          = TYPE_MPU401,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Mpu401State),
    .class_init    = mpu401_class_initfn,
};

static void mpu401_register_types(void)
{
    type_register_static(&mpu401_info);
}

type_init(mpu401_register_types)

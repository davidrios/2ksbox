/*
 * libsynth — the C API of the music engines (doc 20 §3).
 *
 * The one header for the Rust crate (libsynth/src/capi.rs implements it),
 * QEMU's OPL3 device (libsynth/qemu/opl3.c) and its MPU-401 device
 * (libsynth/qemu/mpu401.c), both overlaid into hw/audio/ by
 * scripts/prepare-qemu.sh. Bump LIBSYNTH_API_VERSION on any change; each
 * device refuses a mismatch at realize time rather than at the first note.
 *
 * Threading: a handle is *not* internally locked. Both devices are driven
 * from QEMU with the BQL held — port writes from the vCPU thread, render
 * from the audio timer in the main loop — which is the serialization.
 * Nothing here blocks, allocates on the render path, or unwinds into C:
 * every entry point catches a panic and degrades to silence.
 */
#ifndef LIBSYNTH_H
#define LIBSYNTH_H
#include <stddef.h>
#include <stdint.h>

#define LIBSYNTH_API_VERSION 1

uint32_t libsynth_api_version(void);

/* ---------------------------------------------------------------- OPL3 */

/* The YMF262's own sample rate. A voice opened at this rate is the chip
 * with no resampler in front of it; any other rate makes the core
 * resample (linear, as every OPL implementation does). */
#define LIBSYNTH_OPL_NATIVE_RATE 49716u

typedef struct libsynth_opl libsynth_opl;      /* opaque */

libsynth_opl *libsynth_opl_new(uint32_t rate);
void libsynth_opl_free(libsynth_opl *o);

/* The two register files: 0 = the OPL2-compatible one (ports 388/389 and
 * a Sound Blaster's 2x0/2x1), 1 = the OPL3 extension (38A/38B, 2x2/2x3). */
void libsynth_opl_address(libsynth_opl *o, int bank, uint8_t addr);
void libsynth_opl_data(libsynth_opl *o, int bank, uint8_t val);

/* The status register: the two timer flags and the IRQ bit. Detection
 * routines write the timer registers, wait, and read this twice — so the
 * device must have called libsynth_opl_advance() for the wait to pass. */
uint8_t libsynth_opl_status(libsynth_opl *o);

/* Advance the timers by `usec` of guest time. */
void libsynth_opl_advance(libsynth_opl *o, uint32_t usec);

/* Render `frames` stereo frames, interleaved L,R (2 * frames int16s). */
void libsynth_opl_render(libsynth_opl *o, int16_t *out, size_t frames);

/* ---------------------------------------------------------------- MIDI */

/* What plays the MPU-401's byte stream. */
enum {
    LIBSYNTH_MIDI_NONE = 0,  /* the port exists and swallows everything */
    LIBSYNTH_MIDI_GM   = 1,  /* SoundFont General MIDI; arg = a .sf2 file */
    LIBSYNTH_MIDI_MT32 = 2,  /* Roland CM-32L; arg = a directory of ROMs */
};

typedef struct libsynth_midi libsynth_midi;    /* opaque */

/* Open one. On failure returns NULL and writes a sentence into `err`:
 * the device fails to realize with it, because a machine that was asked
 * for music and silently got none is the bug this avoids. */
libsynth_midi *libsynth_midi_new(int kind, const char *arg,
                                 char *err, size_t errlen);
void libsynth_midi_free(libsynth_midi *m);

/* The engine's own rate: the voice is opened at it and QEMU's mixer does
 * the conversion (48000 for the SoundFont synth, 32000 for the CM-32L,
 * which is the rate the hardware runs at). 0 = renders nothing. */
uint32_t libsynth_midi_rate(libsynth_midi *m);

/* One byte of the UART stream, exactly as the guest wrote it: running
 * status, System Exclusive and interleaved real-time bytes are the
 * parser's problem, not the device's. */
void libsynth_midi_write(libsynth_midi *m, uint8_t byte);

/* All notes off, controllers reset, parser back to no running status —
 * what an MPU-401 reset command means downstream of the port. */
void libsynth_midi_reset(libsynth_midi *m);

/* Output level, per cent of the engine's own full scale (100 = as the
 * bank or the module was mastered). Music and the sound card meet in
 * QEMU's mixer and only this side has a trim. The CM-32L caps at 100 —
 * that is its own master volume, the front-panel knob. */
void libsynth_midi_gain(libsynth_midi *m, uint32_t percent);

/* Render `frames` stereo frames, interleaved L,R. */
void libsynth_midi_render(libsynth_midi *m, int16_t *out, size_t frames);

#endif /* LIBSYNTH_H */

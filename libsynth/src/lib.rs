//! libsynth — the music engines behind the OPL3 and MPU-401 devices
//! (doc 20).
//!
//! Three of them, and the difference between them is the whole point:
//! `opl` is the FM chip a Sound Blaster carries, driven register by
//! register from the guest's own port writes; `gm` and `mt32` sit behind
//! an MPU-401 port and are handed a MIDI byte stream instead, which
//! `midi` turns back into messages. All three are computation over guest
//! bytes — no host device, no system library, no thread of their own — so
//! they link into QEMU the way libdisc does (doc 20 §2) and render into
//! the same audiodev as the sound card, on the same clock.
//!
//! The C API in `capi` is what the two QEMU devices call; `synthx` (the
//! `libsynth` check in `scripts/test.sh`) drives the same engines through
//! the same boundary without a guest.

#[allow(non_camel_case_types)]
pub mod capi;
pub mod gm;
pub mod midi;
pub mod mt32;
pub mod opl;

/// One MIDI engine, whatever is behind the port. The MPU-401 device holds
/// a `Box<dyn Voice>` and never knows which.
pub trait Voice {
    /// The rate it renders at. The device opens its audio voice at this
    /// rate and lets QEMU's mixer resample, rather than resampling twice.
    fn rate(&self) -> u32;
    /// A channel or system-common message, already parsed.
    fn message(&mut self, status: u8, d1: u8, d2: u8);
    /// A complete System Exclusive message, `F0 … F7` included.
    fn sysex(&mut self, data: &[u8]);
    /// Render `out.len() / 2` frames, interleaved L,R.
    fn render(&mut self, out: &mut [i16]);
    /// Silence everything and go back to power-on defaults.
    fn reset(&mut self);
    /// Output level, as a percentage of the engine's own full scale.
    /// Music and digital audio come out of different devices here and
    /// meet in QEMU's mixer, so one of them needs a trim, and it is this
    /// one: the guest has a mixer for its sound card and none for a
    /// synthesizer nobody in 1997 could turn down from software.
    fn gain(&mut self, percent: u32);
}

/// Clip a rendered float sample to the int16 the audiodev takes. The
/// engines are free to overshoot — a SoundFont bank with 24 voices on
/// does — and wrapping there is the loudest possible artefact.
#[inline]
pub(crate) fn clip(v: f32) -> i16 {
    // 32767.0, not 32768.0: the positive full scale is what a converter
    // can actually reach, and rounding to 32768 wraps to -32768.
    let s = v * 32767.0;
    if s >= 32767.0 {
        32767
    } else if s <= -32768.0 {
        -32768
    } else {
        s as i16
    }
}

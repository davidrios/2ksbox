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

/// A capture of what a guest wrote to a music device, turned on by an
/// environment variable in QEMU's own environment (the player and QEMU
/// are one process, so the player's environment is this one):
/// `LIBSYNTH_MIDI_LOG` for the MIDI port's byte stream,
/// `LIBSYNTH_OPL_LOG` for the FM chip's register writes.
///
/// It exists because the engines cannot be argued with from the outside:
/// music that goes wrong part-way through is either a stream that already
/// said so — a channel driven to silence, a note nothing ever released,
/// a byte the parser could attach to nothing — or it is ours, and the
/// only way to tell is to take the guest's own writes away and play them
/// again without a guest (`synthx midilog` / `opllog`, `synthx play`).
///
/// One line per write, `<microseconds since the device opened> <what>`.
/// The clock is the host's, deliberately: the engines render on the
/// host's audio callback, so this is the timeline the music was heard on
/// rather than the one the guest believes in. Flushed every line — a
/// guest is very often stopped by having its power cut, and a buffered
/// tail is exactly the part worth reading.
pub(crate) struct Log {
    out: std::fs::File,
    start: std::time::Instant,
    written: usize,
}

/// Past this the capture stops and says so. Neither device carries more
/// than a few KB/s, so this is hours of music; a file that reaches it is
/// a session left running, not a score.
const LOG_MAX: usize = 32 << 20;

impl Log {
    /// Open the capture `var` names, if it names one. `what` and `shape`
    /// are the header line, which is also how `synthx` tells the two
    /// kinds of capture apart.
    pub(crate) fn open(var: &str, what: &str, shape: &str) -> Option<Log> {
        use std::io::Write;
        use std::sync::atomic::{AtomicBool, Ordering};
        static STARTED: AtomicBool = AtomicBool::new(false);

        let path = std::env::var_os(var)?;
        // The first device of the process truncates and the rest append,
        // so a machine with two of them writes one file rather than each
        // erasing the other's — and a second run still starts empty.
        let first = !STARTED.swap(true, Ordering::Relaxed);
        let mut out = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .append(!first)
            .truncate(first)
            .open(&path)
            .map_err(|e| eprintln!("libsynth: {}: {e}", std::path::Path::new(&path).display()))
            .ok()?;
        let _ = writeln!(out, "# libsynth {what} log: <microseconds> {shape}");
        Some(Log {
            out,
            start: std::time::Instant::now(),
            written: 0,
        })
    }

    pub(crate) fn put(&mut self, what: &str) {
        use std::io::Write;
        if self.written >= LOG_MAX {
            return;
        }
        let us = self.start.elapsed().as_micros();
        let line = format!("{us} {what}\n");
        self.written += line.len();
        let _ = self.out.write_all(line.as_bytes());
        if self.written >= LOG_MAX {
            let _ = self.out.write_all(b"# truncated\n");
        }
    }
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

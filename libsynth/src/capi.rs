//! The C API (`libsynth/libsynth.h`, doc 20 §3): `#[no_mangle] extern "C"`
//! functions over the engines, for the two QEMU devices that own them.
//!
//! Every body runs under `catch_unwind`. A panic in a synthesizer must
//! cost the guest its music, never QEMU its process: a render that
//! panics leaves the block silent and the VM running.

use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;

use crate::midi::{Event, Parser};
use crate::{gm, midi, mt32, opl, Voice};

pub const LIBSYNTH_API_VERSION: u32 = 1;

pub const LIBSYNTH_MIDI_NONE: i32 = 0;
pub const LIBSYNTH_MIDI_GM: i32 = 1;
pub const LIBSYNTH_MIDI_MT32: i32 = 2;

#[no_mangle]
pub extern "C" fn libsynth_api_version() -> u32 {
    LIBSYNTH_API_VERSION
}

/// Write a message into the caller's error buffer, NUL-terminated and
/// truncated rather than refused.
fn set_err(err: *mut c_char, errlen: usize, msg: &str) {
    if err.is_null() || errlen == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(errlen - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
}

fn c_str<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/* ---------------------------------------------------------------- OPL3 */

#[no_mangle]
pub extern "C" fn libsynth_opl_new(rate: u32) -> *mut opl::Opl {
    match catch_unwind(|| Box::into_raw(Box::new(opl::Opl::new(rate)))) {
        Ok(p) => p,
        Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn libsynth_opl_free(o: *mut opl::Opl) {
    if o.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(o) })));
}

#[no_mangle]
pub extern "C" fn libsynth_opl_address(o: *mut opl::Opl, bank: i32, addr: u8) {
    let Some(o) = (unsafe { o.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| o.address(bank, addr)));
}

#[no_mangle]
pub extern "C" fn libsynth_opl_data(o: *mut opl::Opl, bank: i32, val: u8) {
    let Some(o) = (unsafe { o.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| o.data(bank, val)));
}

#[no_mangle]
pub extern "C" fn libsynth_opl_status(o: *mut opl::Opl) -> u8 {
    let Some(o) = (unsafe { o.as_mut() }) else { return 0 };
    catch_unwind(AssertUnwindSafe(|| o.status())).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn libsynth_opl_advance(o: *mut opl::Opl, usec: u32) {
    let Some(o) = (unsafe { o.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| o.advance(usec)));
}

#[no_mangle]
pub extern "C" fn libsynth_opl_render(o: *mut opl::Opl, out: *mut i16, frames: usize) {
    if out.is_null() || frames == 0 {
        return;
    }
    let Some(o) = (unsafe { o.as_mut() }) else { return };
    let buf = unsafe { std::slice::from_raw_parts_mut(out, frames * 2) };
    if catch_unwind(AssertUnwindSafe(|| o.render(buf))).is_err() {
        buf.fill(0);
    }
}

/* ---------------------------------------------------------------- MIDI */

/// A port and whatever is behind it: the byte-stream parser, which is
/// the same whichever engine plays the result, and the engine.
pub struct Midi {
    parser: Parser,
    voice: Option<Box<dyn Voice>>,
}

#[no_mangle]
pub extern "C" fn libsynth_midi_new(
    kind: i32,
    arg: *const c_char,
    err: *mut c_char,
    errlen: usize,
) -> *mut Midi {
    let arg = c_str(arg).map(PathBuf::from);
    let built = catch_unwind(AssertUnwindSafe(|| -> Result<Midi, String> {
        let voice: Option<Box<dyn Voice>> = match kind {
            LIBSYNTH_MIDI_NONE => None,
            LIBSYNTH_MIDI_GM => {
                let path = arg.ok_or("no SoundFont given (synth=gm needs soundfont=<file>)")?;
                Some(Box::new(gm::Gm::new(&path)?))
            }
            LIBSYNTH_MIDI_MT32 => {
                let path = arg.ok_or("no ROM directory given (synth=mt32 needs romdir=<dir>)")?;
                Some(Box::new(mt32::Mt32::new(&path)?))
            }
            other => return Err(format!("unknown MIDI synth {other}")),
        };
        Ok(Midi {
            parser: Parser::new(),
            voice,
        })
    }));
    match built {
        Ok(Ok(m)) => Box::into_raw(Box::new(m)),
        Ok(Err(msg)) => {
            set_err(err, errlen, &msg);
            std::ptr::null_mut()
        }
        Err(_) => {
            set_err(err, errlen, "the MIDI synthesizer panicked while opening");
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn libsynth_midi_free(m: *mut Midi) {
    if m.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(m) })));
}

#[no_mangle]
pub extern "C" fn libsynth_midi_rate(m: *mut Midi) -> u32 {
    let Some(m) = (unsafe { m.as_mut() }) else { return 0 };
    m.voice.as_ref().map(|v| v.rate()).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn libsynth_midi_write(m: *mut Midi, byte: u8) {
    let Some(m) = (unsafe { m.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| m.write(byte)));
}

#[no_mangle]
pub extern "C" fn libsynth_midi_reset(m: *mut Midi) {
    let Some(m) = (unsafe { m.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| {
        m.parser.reset();
        if let Some(v) = m.voice.as_mut() {
            v.reset();
        }
    }));
}

/// Output level, per cent of the engine's full scale. The CM-32L caps
/// at its own 100; the SoundFont synth takes more, for a bank that was
/// mastered quietly.
#[no_mangle]
pub extern "C" fn libsynth_midi_gain(m: *mut Midi, percent: u32) {
    let Some(m) = (unsafe { m.as_mut() }) else { return };
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Some(v) = m.voice.as_mut() {
            v.gain(percent);
        }
    }));
}

#[no_mangle]
pub extern "C" fn libsynth_midi_render(m: *mut Midi, out: *mut i16, frames: usize) {
    if out.is_null() || frames == 0 {
        return;
    }
    let Some(m) = (unsafe { m.as_mut() }) else { return };
    let buf = unsafe { std::slice::from_raw_parts_mut(out, frames * 2) };
    let rendered = catch_unwind(AssertUnwindSafe(|| match m.voice.as_mut() {
        Some(v) => {
            v.render(buf);
            true
        }
        None => false,
    }));
    if !matches!(rendered, Ok(true)) {
        buf.fill(0);
    }
}

impl Midi {
    /// One byte of the stream: parse it, and hand whatever it completed
    /// to the engine. Public so `synthx` drives the same path the device
    /// does rather than a private one of its own.
    pub fn write(&mut self, byte: u8) {
        let Some(event) = self.parser.write(byte) else {
            return;
        };
        let Some(voice) = self.voice.as_mut() else {
            return;
        };
        match event {
            Event::Message(status, d1, d2) => voice.message(status, d1, d2),
            Event::Sysex(data) => voice.sysex(&data),
        }
    }

    /// For `synthx`: build one without going through the C boundary.
    pub fn new(voice: Option<Box<dyn Voice>>) -> Midi {
        Midi {
            parser: midi::Parser::new(),
            voice,
        }
    }

    pub fn rate(&self) -> u32 {
        self.voice.as_ref().map(|v| v.rate()).unwrap_or(0)
    }

    pub fn render(&mut self, out: &mut [i16]) {
        match self.voice.as_mut() {
            Some(v) => v.render(out),
            None => out.fill(0),
        }
    }
}

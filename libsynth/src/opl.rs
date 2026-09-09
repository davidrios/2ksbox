//! The FM chip: a YMF262 (OPL3) behind the ports a game writes to.
//!
//! This is not a MIDI engine — nothing above it parses anything. The
//! guest writes a register address and a value, exactly as it would to
//! the chip on a Sound Blaster 16, and the sound is whatever those
//! registers make. It is the one music path of the era that needs no
//! file, no ROM and no driver: an AdLib-aware game finds it by writing
//! the timer registers and reading the status back, which is why the
//! timers below matter as much as the operators do.

use nuked_opl3::{Opl3Device, OplRegisterFile};

/// The chip's own sample rate. Opening the voice here means the core
/// resamples nothing and QEMU's mixer does the one conversion there is.
pub const NATIVE_RATE: u32 = 49716;

pub struct Opl {
    dev: Opl3Device,
    rate: u32,
}

impl Opl {
    pub fn new(rate: u32) -> Opl {
        let rate = if rate == 0 { NATIVE_RATE } else { rate };
        Opl {
            dev: Opl3Device::new(rate),
            rate,
        }
    }

    pub fn rate(&self) -> u32 {
        self.rate
    }

    fn file(bank: i32) -> OplRegisterFile {
        if bank == 0 {
            OplRegisterFile::Primary
        } else {
            OplRegisterFile::Secondary
        }
    }

    /// Latch a register address. An OPL2-only game writes bank 0 all its
    /// life and never learns the chip has a second file.
    pub fn address(&mut self, bank: i32, addr: u8) {
        let _ = self.dev.write_address(addr, Self::file(bank));
    }

    /// Write the value for the latched address. Unbuffered: the guest's
    /// own writes already carry the timing the chip would have seen.
    pub fn data(&mut self, bank: i32, val: u8) {
        let _ = self.dev.write_data(val, Self::file(bank), false);
    }

    /// The status register: timer flags plus the IRQ bit.
    pub fn status(&mut self) -> u8 {
        self.dev.read_status()
    }

    /// Advance the two timers by `usec` of guest time. A detection
    /// routine sets timer 1 to expire in 80 µs and reads the status back
    /// in a delay loop, so a chip whose timers never move is a chip no
    /// game finds.
    pub fn advance(&mut self, usec: u32) {
        self.dev.run(usec as f64);
    }

    /// Render `out.len() / 2` frames, interleaved L,R.
    pub fn render(&mut self, out: &mut [i16]) {
        if out.len() < 2 {
            return;
        }
        let _ = self.dev.generate_samples(out);
    }
}

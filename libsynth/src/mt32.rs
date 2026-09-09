//! The Roland MT-32 family, through moont's CM-32L.
//!
//! A 1990 game that says "Roland" on its setup screen is not asking for
//! General MIDI: it drives an MT-32's own parts and often uploads its
//! own timbres over System Exclusive first. Nothing a SoundFont bank
//! does resembles that, which is why this is a second engine and not a
//! second bank.
//!
//! **The ROMs are the user's own.** Roland's control and PCM ROMs are
//! not redistributable and none of this repository carries them: the
//! device is pointed at a directory and the two files are recognised by
//! their sizes (64 KiB control, 1 MiB PCM), whatever their names, because
//! every dump in circulation names them differently.

use std::path::Path;

use moont::cm32l;
use moont::{Frame, Synth};

use crate::Voice;

/// The hardware's own rate. Not a choice: the CM-32L's converter runs at
/// 32 kHz and the emulation is sample-accurate against Munt at it.
pub const RATE: u32 = moont::SAMPLE_RATE;

pub struct Mt32 {
    dev: cm32l::Device,
    buf: Vec<Frame>,
}

impl Mt32 {
    /// Open the ROMs in `dir`. The error is the sentence the device
    /// fails to realize with.
    pub fn new(dir: &Path) -> Result<Mt32, String> {
        let (control, pcm) = find_roms(dir)?;
        let rom = cm32l::Rom::new(&control, &pcm)
            .map_err(|e| format!("MT-32 ROMs in {}: {e}", dir.display()))?;
        Ok(Mt32 {
            dev: cm32l::Device::new(rom),
            buf: Vec::new(),
        })
    }
}

/// The PCM ROM of an *original* MT-32 — half the CM-32L's. Not a size we
/// can use (the engine is a CM-32L), but much the likeliest thing to
/// find in a directory someone points at this, so it is worth saying so
/// rather than reporting that nothing was found.
const MT32_PCM_SIZE: usize = 512 * 1024;

/// The two ROM images in a directory, by size rather than by name: a
/// control ROM is exactly 64 KiB and a PCM ROM exactly 1 MiB, and the
/// dumps people have are called CM32L_CONTROL.ROM, cm32l_ctrl.rom,
/// ctrl_cm32l_1_02.rom and half a dozen other things.
fn find_roms(dir: &Path) -> Result<(Vec<u8>, Vec<u8>), String> {
    let entries = std::fs::read_dir(dir)
        .map_err(|e| format!("MT-32 ROM directory {}: {e}", dir.display()))?;
    let (mut control, mut pcm) = (None, None);
    let mut mt32_pcm = false;
    for entry in entries.flatten() {
        let path = entry.path();
        let len = match entry.metadata() {
            Ok(m) if m.is_file() => m.len() as usize,
            _ => continue,
        };
        let slot = match len {
            cm32l::CONTROL_SIZE => &mut control,
            cm32l::PCM_SIZE => &mut pcm,
            MT32_PCM_SIZE => {
                mt32_pcm = true;
                continue;
            }
            _ => continue,
        };
        if slot.is_none() {
            *slot = Some(path);
        }
    }
    let read = |what: &str, want: usize, p: Option<std::path::PathBuf>| -> Result<Vec<u8>, String> {
        let p = p.ok_or_else(|| {
            let mut msg = format!(
                "no CM-32L {what} ROM in {} (looking for a {want} byte file)",
                dir.display()
            );
            // The one wrong answer worth naming: an original MT-32's PCM
            // ROM is half the size, and this engine is a CM-32L. A game
            // written for an MT-32 plays on a CM-32L — the module is a
            // superset — but the ROMs are not interchangeable.
            if mt32_pcm && want == cm32l::PCM_SIZE {
                msg.push_str(concat!(
                    ". There is a 524288 byte one here, which is an original ",
                    "MT-32's: this emulates the CM-32L, whose PCM ROM is 1 MiB ",
                    "— a CM-32L, CM-64 or LAPC-I dump is what it wants",
                ));
            }
            msg
        })?;
        std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))
    };
    Ok((
        read("control", cm32l::CONTROL_SIZE, control)?,
        read("PCM", cm32l::PCM_SIZE, pcm)?,
    ))
}

impl Voice for Mt32 {
    fn rate(&self) -> u32 {
        RATE
    }

    fn message(&mut self, status: u8, d1: u8, d2: u8) {
        if status < 0x80 || status >= 0xF0 {
            return;
        }
        // The packing moont takes is the packing a UART delivers:
        // status, then data, low byte first.
        self.dev
            .play_msg(status as u32 | (d1 as u32) << 8 | (d2 as u32) << 16);
    }

    fn sysex(&mut self, data: &[u8]) {
        // Whole message, F0 … F7 included — this is where a game uploads
        // the timbres its music is written for, so dropping these is
        // dropping the instruments.
        self.dev.play_sysex(data);
    }

    fn render(&mut self, out: &mut [i16]) {
        let frames = out.len() / 2;
        if frames == 0 {
            return;
        }
        if self.buf.len() < frames {
            self.buf.resize(frames, Frame(0, 0));
        }
        self.dev.render(&mut self.buf[..frames]);
        for (i, frame) in out.chunks_exact_mut(2).enumerate() {
            frame[0] = self.buf[i].0;
            frame[1] = self.buf[i].1;
        }
    }

    fn reset(&mut self) {
        let _ = self.dev.apply_command(moont::ControlCommand::Reset);
    }

    fn gain(&mut self, percent: u32) {
        // The hardware's own master volume, which is what the module's
        // front-panel knob moved: 0-100, and the reference for it.
        let _ = self.dev.apply_command(moont::ControlCommand::SetMasterVolume {
            volume: percent.min(100) as u8,
        });
    }
}

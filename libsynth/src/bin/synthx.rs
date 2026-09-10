//! synthx — the libsynth exerciser (doc 20 §6).
//!
//!   synthx selftest <outdir>        the engines through the C API, PASS/FAIL/SKIP per check,
//!                                   one .wav per case in <outdir> (listen to what failed)
//!   synthx bank <file.sf2>          a note rendered through that bank: what the file is worth
//!   synthx opl <out.wav> [seconds]  the FM tone the selftest programs, to hear by hand
//!   synthx wavlevel <file.wav>      how loud a wav is, and for how much of its length: what a
//!                                   *music* check asks, since a score is not one frequency
//!   synthx wavtone <file.wav> <hz>  a wav really holds that note: what the `music` check asks
//!                                   of a wav QEMU's own audiodev recorded, with the guest's
//!                                   ports written by the monitor rather than by us
//!   synthx midilog <file>           what a guest actually sent the MIDI port, captured by
//!                                   `LIBSYNTH_MIDI_LOG=<file>`: the channels second by second,
//!                                   what silenced each one, notes left held, bytes the parser
//!                                   could attach to nothing
//!   synthx play <file> <out.wav>    the same capture played again with no guest, at the timing
//!                                   it was written with — if the music breaks here it is ours,
//!                                   and if it does not the guest stopped sending it
//!
//! `--sf2 <file>` (or `LIBSYNTH_SF2`) is the SoundFont the General MIDI
//! checks play through; without either it is the bank the packages ship,
//! `soundfonts/TimGM6mb.sf2`, so the check covers the file a user will
//! actually hear rather than a fixture written for the occasion.
//! `--roms <dir>` (or `LIBSYNTH_MT32_ROMS`) points the MT-32 checks at a
//! directory holding the two Roland ROMs; without it they SKIP, because
//! nothing here ships them.
//!
//! The exit code is 1 on any FAIL. A SKIP is not a failure: a machine
//! with no MT-32 ROMs still has to pass everything else.

use std::path::{Path, PathBuf};

use libsynth::capi;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut positional: Vec<String> = Vec::new();
    let mut roms: Option<PathBuf> = std::env::var_os("LIBSYNTH_MT32_ROMS").map(PathBuf::from);
    let mut sf2: Option<PathBuf> = std::env::var_os("LIBSYNTH_SF2").map(PathBuf::from);
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--roms" if i + 1 < args.len() => {
                roms = Some(PathBuf::from(&args[i + 1]));
                i += 1;
            }
            "--sf2" if i + 1 < args.len() => {
                sf2 = Some(PathBuf::from(&args[i + 1]));
                i += 1;
            }
            other => positional.push(other.to_string()),
        }
        i += 1;
    }
    let verb = positional.first().map(String::as_str).unwrap_or("");
    let rc = match verb {
        "selftest" => selftest(
            Path::new(positional.get(1).map(String::as_str).unwrap_or("build/test/libsynth")),
            &sf2.unwrap_or_else(shipped_bank),
            roms.as_deref(),
        ),
        "bank" => match positional.get(1) {
            Some(p) => bank_info(Path::new(p)),
            None => usage(),
        },
        "wavlevel" => match positional.get(1) {
            Some(p) => match read_wav(Path::new(p)) {
                Ok((rate, pcm)) => {
                    let (level, peak, busy) = level(&pcm, rate);
                    // A score is not a tone, so this is all a wav can
                    // honestly be asked: is there sound in it, and for
                    // how much of its length. Half a second of audible
                    // signal is more than a click and less than a tune.
                    let verdict = if busy >= 0.5 { "PASS" } else { "FAIL" };
                    println!(
                        "{verdict} wavlevel         {p}: {:.4} RMS, peak {:.3}, audible for {:.1} s",
                        level, peak, busy
                    );
                    i32::from(verdict == "FAIL")
                }
                Err(e) => {
                    println!("FAIL wavlevel         {p}: {e}");
                    1
                }
            },
            None => usage(),
        },
        "wavtone" => match (positional.get(1), positional.get(2)) {
            (Some(p), Some(hz)) => {
                let freq: f32 = hz.parse().unwrap_or(0.0);
                match read_wav(Path::new(p)) {
                    Ok((rate, pcm)) => match tone_at(&pcm, rate, freq) {
                        Ok(note) => {
                            println!("PASS wavtone          {p}: {note}");
                            0
                        }
                        Err(why) => {
                            println!("FAIL wavtone          {p}: {why}");
                            1
                        }
                    },
                    Err(e) => {
                        println!("FAIL wavtone          {p}: {e}");
                        1
                    }
                }
            }
            _ => usage(),
        },
        "midilog" => match positional.get(1) {
            Some(p) => midilog(Path::new(p)),
            None => usage(),
        },
        "play" => match (positional.get(1), positional.get(2)) {
            (Some(log), Some(out)) => play(
                Path::new(log),
                Path::new(out),
                &sf2.unwrap_or_else(shipped_bank),
                roms.as_deref(),
            ),
            _ => usage(),
        },
        "opl" => match positional.get(1) {
            Some(p) => {
                let secs: f32 = positional.get(2).and_then(|s| s.parse().ok()).unwrap_or(2.0);
                let pcm = opl_tone(secs);
                write_wav(Path::new(p), libsynth::opl::NATIVE_RATE, &pcm);
                println!("{p}: {:.1} s at {} Hz", secs, libsynth::opl::NATIVE_RATE);
                0
            }
            None => usage(),
        },
        _ => usage(),
    };
    std::process::exit(rc);
}

fn usage() -> i32 {
    eprintln!("usage: synthx selftest <outdir> [--sf2 <file>] [--roms <dir>] | bank <in.sf2> | opl <out.wav> [seconds] | wavtone <in.wav> <hz> | wavlevel <in.wav> | midilog <log> | play <log> <out.wav> [--roms <dir>]");
    2
}

/* ------------------------------------------------------------ the checks */

struct Report {
    failed: u32,
}

impl Report {
    fn case(&mut self, name: &str, result: Result<String, String>) {
        match result {
            Ok(note) => println!("PASS {name:<16} {note}"),
            Err(why) => {
                println!("FAIL {name:<16} {why}");
                self.failed += 1;
            }
        }
    }
    fn skip(&mut self, name: &str, why: &str) {
        println!("SKIP {name:<16} {why}");
    }
}

/// The bank the packages ship, in the checkout this binary was built
/// from — `launcher_core::paths::checkout`'s rule, which synthx cannot
/// call because it links no launcher code.
fn shipped_bank() -> PathBuf {
    Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../soundfonts")).join("TimGM6mb.sf2")
}

fn selftest(dir: &Path, bank: &Path, roms: Option<&Path>) -> i32 {
    if let Err(e) = std::fs::create_dir_all(dir) {
        eprintln!("{}: {e}", dir.display());
        return 1;
    }
    let mut r = Report { failed: 0 };

    r.case("opl-detect", opl_detect());

    let fm = opl_tone(1.0);
    write_wav(&dir.join("opl-tone.wav"), libsynth::opl::NATIVE_RATE, &fm);
    r.case("opl-tone", tone_at(&fm, libsynth::opl::NATIVE_RATE, 440.0));

    // The bank the packages ship, not a fixture written for the check:
    // a bank that stopped loading — a truncated file in the package, a
    // parser that lost a chunk — is exactly the failure worth catching,
    // and a synthetic one would never see it.
    if bank.is_file() {
        r.case("gm-tone", gm_tone(bank, dir));
        r.case("gm-running-status", gm_running_status(bank, dir));
    } else {
        r.case("gm-tone", Err(format!("{}: no such SoundFont", bank.display())));
    }

    match roms {
        Some(dir_roms) if dir_roms.is_dir() => r.case("mt32-tone", mt32_tone(dir_roms, dir)),
        Some(dir_roms) => r.skip("mt32-tone", &format!("{} is not a directory", dir_roms.display())),
        None => r.skip("mt32-tone", "no --roms / LIBSYNTH_MT32_ROMS: the Roland ROMs are the user's own"),
    }

    if r.failed > 0 {
        println!("synthx: {} failed", r.failed);
        1
    } else {
        println!("synthx: all checks passed");
        0
    }
}

/// The sequence every AdLib-aware game runs before it will play a note:
/// mask and reset the flags, read 0, start timer 1, wait, read the timer
/// flag and the IRQ bit, reset again. A chip whose timers do not move is
/// a chip no game finds, however well it synthesizes.
fn opl_detect() -> Result<String, String> {
    let o = capi::libsynth_opl_new(libsynth::opl::NATIVE_RATE);
    if o.is_null() {
        return Err("libsynth_opl_new returned NULL".into());
    }
    let write = |reg: u8, val: u8| {
        capi::libsynth_opl_address(o, 0, reg);
        capi::libsynth_opl_data(o, 0, val);
    };
    write(0x04, 0x60); // mask both timer flags
    write(0x04, 0x80); // reset them
    let idle = capi::libsynth_opl_status(o);
    write(0x02, 0xFF); // timer 1 expires after one 80 µs tick
    write(0x04, 0x21); // unmask timer 1, start it
    capi::libsynth_opl_advance(o, 800); // the game's delay loop
    let fired = capi::libsynth_opl_status(o);
    write(0x04, 0x60);
    write(0x04, 0x80);
    let cleared = capi::libsynth_opl_status(o);
    capi::libsynth_opl_free(o);
    if idle != 0x00 {
        return Err(format!("status was {idle:#04x} before the timer started, wanted 0x00"));
    }
    if fired & 0xC0 != 0xC0 {
        return Err(format!("status was {fired:#04x} after timer 1 expired, wanted bits 0xC0"));
    }
    if cleared != 0x00 {
        return Err(format!("status was {cleared:#04x} after the flags were reset, wanted 0x00"));
    }
    Ok("status 0x00 → 0xC0 → 0x00 across the detection sequence".into())
}

/// A 440 Hz note on the FM chip, programmed the way an AdLib driver
/// does: one channel, two operators, additive, key on.
fn opl_tone(seconds: f32) -> Vec<i16> {
    let o = capi::libsynth_opl_new(libsynth::opl::NATIVE_RATE);
    let write = |bank: i32, reg: u8, val: u8| {
        capi::libsynth_opl_address(o, bank, reg);
        capi::libsynth_opl_data(o, bank, val);
    };
    write(1, 0x05, 0x01); // OPL3 mode: the second register file, and stereo
    write(0, 0x01, 0x20); // waveform select enabled
    for op in [0x00u8, 0x03] {
        write(0, 0x20 + op, 0x01); // multiplier 1, no tremolo/vibrato
        write(0, 0x40 + op, 0x00); // full output level
        write(0, 0x60 + op, 0xF0); // fastest attack, slow decay
        write(0, 0x80 + op, 0x77); // sustain high, medium release
        write(0, 0xE0 + op, 0x00); // sine
    }
    write(0, 0xC0, 0x31); // both channels, additive
    // 440 Hz = fnum * 49716 / 2^(20 - block), block 4 → fnum 580.
    write(0, 0xA0, 580u16 as u8);
    write(0, 0xB0, 0x20 | (4 << 2) | (580 >> 8) as u8); // key on
    let frames = (libsynth::opl::NATIVE_RATE as f32 * seconds) as usize;
    let mut pcm = vec![0i16; frames * 2];
    capi::libsynth_opl_render(o, pcm.as_mut_ptr(), frames);
    capi::libsynth_opl_free(o);
    pcm
}

/// A note through the SoundFont engine, byte by byte through the port —
/// the same path the guest's MPU-401 writes take.
fn gm_tone(bank: &Path, dir: &Path) -> Result<String, String> {
    let m = midi_new(capi::LIBSYNTH_MIDI_GM, bank)?;
    for b in [0xC0, 0x00] {
        capi::libsynth_midi_write(m, b); // program 0
    }
    for b in [0x90, 69, 100] {
        capi::libsynth_midi_write(m, b); // A4, the 440 Hz the bank is built on
    }
    let rate = capi::libsynth_midi_rate(m);
    let pcm = render(m, rate, 1.0);
    write_wav(&dir.join("gm-tone.wav"), rate, &pcm);
    capi::libsynth_midi_free(m);
    tone_at(&pcm, rate, 440.0)
}

/// The two things that make a real stream harder than a test one:
/// running status (the note-off arrives as two bytes with no status of
/// its own) and a real-time byte dropped in the middle of a message.
/// Both are proved by the sound: the note must stop.
fn gm_running_status(bank: &Path, dir: &Path) -> Result<String, String> {
    let m = midi_new(capi::LIBSYNTH_MIDI_GM, bank)?;
    let rate = capi::libsynth_midi_rate(m);
    for b in [0x90, 69, 0xFE, 100] {
        // 0xFE is active sensing, between the key and the velocity
        capi::libsynth_midi_write(m, b);
    }
    let sounding = render(m, rate, 0.25);
    for b in [69, 0] {
        capi::libsynth_midi_write(m, b); // running status: note-on, velocity 0
    }
    let after = render(m, rate, 1.0);
    write_wav(&dir.join("gm-running-status.wav"), rate, &[sounding.clone(), after.clone()].concat());
    capi::libsynth_midi_free(m);
    tone_at(&sounding, rate, 440.0)?;
    // The tail: what is left a second after the note was released.
    let tail = &after[after.len() / 2..];
    let level = rms(tail);
    if level > 0.01 {
        return Err(format!("still sounding at {level:.4} RMS a second after the running-status note-off"));
    }
    Ok(format!("note through an interleaved 0xFE, silent ({level:.5} RMS) after a running-status note-off"))
}

/// The MT-32, on a machine that has the ROMs. Its own default mapping
/// puts part 1 on MIDI channel 2, so this is not channel 1 by mistake.
fn mt32_tone(roms: &Path, dir: &Path) -> Result<String, String> {
    let m = midi_new(capi::LIBSYNTH_MIDI_MT32, roms)?;
    let rate = capi::libsynth_midi_rate(m);
    if rate != libsynth::mt32::RATE {
        return Err(format!("rate {rate}, wanted the hardware's {}", libsynth::mt32::RATE));
    }
    for b in [0x91, 69, 100] {
        capi::libsynth_midi_write(m, b);
    }
    let pcm = render(m, rate, 1.5);
    write_wav(&dir.join("mt32-tone.wav"), rate, &pcm);
    capi::libsynth_midi_free(m);
    tone_at(&pcm, rate, 440.0)
}

fn midi_new(kind: i32, arg: &Path) -> Result<*mut capi::Midi, String> {
    let c = std::ffi::CString::new(arg.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
    let mut err = [0i8; 256];
    let m = capi::libsynth_midi_new(kind, c.as_ptr(), err.as_mut_ptr() as *mut _, err.len());
    if m.is_null() {
        let msg: Vec<u8> = err.iter().take_while(|&&b| b != 0).map(|&b| b as u8).collect();
        return Err(String::from_utf8_lossy(&msg).into_owned());
    }
    Ok(m)
}

fn render(m: *mut capi::Midi, rate: u32, seconds: f32) -> Vec<i16> {
    let frames = (rate as f32 * seconds) as usize;
    let mut pcm = vec![0i16; frames * 2];
    // In blocks, the way the audio callback asks for it — a synthesizer
    // that only works when handed the whole note at once is not one.
    let block = 512;
    let mut done = 0;
    while done < frames {
        let n = block.min(frames - done);
        capi::libsynth_midi_render(m, pcm[done * 2..].as_mut_ptr(), n);
        done += n;
    }
    pcm
}

/* -------------------------------------------------------- measuring it */

fn rms(pcm: &[i16]) -> f32 {
    if pcm.is_empty() {
        return 0.0;
    }
    let sum: f64 = pcm.iter().map(|&s| { let v = s as f64 / 32768.0; v * v }).sum();
    (sum / pcm.len() as f64).sqrt() as f32
}

/// The energy at `freq`, by Goertzel over the left channel.
fn goertzel(pcm: &[i16], rate: u32, freq: f32) -> f32 {
    let n = pcm.len() / 2;
    if n == 0 {
        return 0.0;
    }
    let w = 2.0 * std::f64::consts::PI * freq as f64 / rate as f64;
    let coeff = 2.0 * w.cos();
    let (mut s1, mut s2) = (0.0f64, 0.0f64);
    for i in 0..n {
        let x = pcm[i * 2] as f64 / 32768.0;
        let s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    ((s1 * s1 + s2 * s2 - coeff * s1 * s2).max(0.0).sqrt() / n as f64) as f32
}

/// Overall level, peak, and how many seconds of the file are audible —
/// the three numbers that say whether a *piece of music* is in there.
/// "Audible" is measured in 100 ms blocks, so a quiet passage inside a
/// score does not end the count and a single click does not start one.
fn level(pcm: &[i16], rate: u32) -> (f32, f32, f32) {
    let peak = pcm.iter().map(|&s| (s as f32 / 32768.0).abs()).fold(0.0f32, f32::max);
    let block = (rate as usize / 10).max(1) * 2;
    let audible = pcm
        .chunks(block)
        .filter(|c| rms(c) > 0.002)
        .count();
    (rms(pcm), peak, audible as f32 / 10.0)
}

/// The pitch test every tone check shares: the note must be *there*
/// (audible at all) and must be *that* note — twice the energy of two
/// frequencies that are neither its harmonics nor its subharmonics, so
/// a timbre with a strong overtone still passes and a chip playing the
/// wrong note does not.
fn tone_at(pcm: &[i16], rate: u32, freq: f32) -> Result<String, String> {
    let level = rms(pcm);
    if level < 0.005 {
        return Err(format!("silent ({level:.5} RMS)"));
    }
    let want = goertzel(pcm, rate, freq);
    let decoys = [freq * 1.26, freq * 0.63]; // a third up, a sixth down
    let worst = decoys.iter().fold(0.0f32, |acc, &f| acc.max(goertzel(pcm, rate, f)));
    if want < worst * 2.0 {
        return Err(format!(
            "{freq:.0} Hz is not the note: {want:.5} against {worst:.5} at a neighbour ({level:.4} RMS)"
        ));
    }
    Ok(format!("{freq:.0} Hz at {want:.4}, {:.0}× its neighbours, {level:.4} RMS", want / worst.max(1e-9)))
}

/// A 16-bit PCM wav back in, for the checks that measure what QEMU's own
/// `wav` audiodev recorded. Only the two chunks that backend writes, in
/// the order it writes them.
fn read_wav(path: &Path) -> Result<(u32, Vec<i16>), String> {
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("not a RIFF/WAVE file".into());
    }
    let u32at = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    let u16at = |o: usize| u16::from_le_bytes([bytes[o], bytes[o + 1]]);
    let (mut pos, mut rate, mut channels, mut bits) = (12usize, 0u32, 0u16, 0u16);
    while pos + 8 <= bytes.len() {
        let id = &bytes[pos..pos + 4];
        let size = u32at(pos + 4) as usize;
        let body = pos + 8;
        if id == b"fmt " && body + 16 <= bytes.len() {
            channels = u16at(body + 2);
            rate = u32at(body + 4);
            bits = u16at(body + 14);
        } else if id == b"data" {
            let end = (body + size).min(bytes.len());
            if bits != 16 || channels == 0 {
                return Err(format!("{bits}-bit, {channels} channels: not 16-bit PCM"));
            }
            let mut pcm: Vec<i16> = bytes[body..end]
                .chunks_exact(2)
                .map(|c| i16::from_le_bytes([c[0], c[1]]))
                .collect();
            // Everything downstream measures the left channel of a
            // stereo stream; a mono recording is doubled rather than
            // special-cased.
            if channels == 1 {
                pcm = pcm.into_iter().flat_map(|s| [s, s]).collect();
            }
            return Ok((rate, pcm));
        }
        pos = body + size + (size & 1);
    }
    Err("no data chunk".into())
}

fn write_wav(path: &Path, rate: u32, pcm: &[i16]) {
    let bytes = pcm.len() * 2;
    let mut out: Vec<u8> = Vec::with_capacity(44 + bytes);
    out.extend(b"RIFF");
    out.extend(((36 + bytes) as u32).to_le_bytes());
    out.extend(b"WAVEfmt ");
    out.extend(16u32.to_le_bytes());
    out.extend(1u16.to_le_bytes()); // PCM
    out.extend(2u16.to_le_bytes()); // stereo
    out.extend(rate.to_le_bytes());
    out.extend((rate * 4).to_le_bytes());
    out.extend(4u16.to_le_bytes());
    out.extend(16u16.to_le_bytes());
    out.extend(b"data");
    out.extend((bytes as u32).to_le_bytes());
    for &s in pcm {
        out.extend(s.to_le_bytes());
    }
    if let Err(e) = std::fs::write(path, out) {
        eprintln!("{}: {e}", path.display());
    }
}

fn bank_info(path: &Path) -> i32 {
    let m = match midi_new(capi::LIBSYNTH_MIDI_GM, path) {
        Ok(m) => m,
        Err(e) => {
            println!("FAIL bank             {e}");
            return 1;
        }
    };
    let rate = capi::libsynth_midi_rate(m);
    for b in [0xC0, 0x00, 0x90, 69, 100] {
        capi::libsynth_midi_write(m, b);
    }
    let pcm = render(m, rate, 1.0);
    capi::libsynth_midi_free(m);
    match tone_at(&pcm, rate, 440.0) {
        Ok(note) => {
            println!("PASS bank             {}: {note}", path.display());
            0
        }
        Err(why) => {
            println!("FAIL bank             {}: {why}", path.display());
            1
        }
    }
}

/* ------------------------------------------------- a captured guest stream */

/// One line of a `LIBSYNTH_MIDI_LOG` capture.
enum Entry {
    Byte(u8),
    Reset,
}

/// Read a capture: `<microseconds> <hex>` a line, `R` where the guest
/// reset the port, `#` comments.
fn read_log(path: &Path) -> Result<Vec<(u64, Entry)>, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut out = Vec::new();
    for (n, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (us, what) = line
            .split_once(' ')
            .ok_or_else(|| format!("{}:{}: not `<microseconds> <byte>`", path.display(), n + 1))?;
        let us: u64 = us
            .parse()
            .map_err(|_| format!("{}:{}: {us} is not a timestamp", path.display(), n + 1))?;
        let entry = if what == "R" {
            Entry::Reset
        } else {
            Entry::Byte(
                u8::from_str_radix(what, 16)
                    .map_err(|_| format!("{}:{}: {what} is not a byte", path.display(), n + 1))?,
            )
        };
        out.push((us, entry));
    }
    Ok(out)
}

/// What one MIDI channel did, for the report.
#[derive(Default, Clone)]
struct Chan {
    notes: u32,
    /// Note-ons per second, so a channel that stops is a column that
    /// stops rather than a number that has to be believed.
    per_sec: Vec<u32>,
    /// (bank, program) as the guest selected them, in order.
    patches: Vec<(u32, u8)>,
    bank_msb: u8,
    bank_lsb: u8,
    volume: u8,
    expression: u8,
    /// Keys currently down, and when each went down.
    held: std::collections::HashMap<u8, u64>,
    /// Times the channel was silenced from the outside, and by what.
    silenced: Vec<(u64, String)>,
}

/// The report a capture is taken for: which channels sounded, when each
/// stopped, and what stopped it.
///
/// The one question it exists to answer is where the music went. A
/// channel whose note-ons stop is the guest's doing — Windows, the
/// driver, the program — and nothing here can put them back. A channel
/// still being sent notes and not heard is ours, and then the same file
/// goes through `synthx play` to hear it with no guest in the way.
fn midilog(path: &Path) -> i32 {
    let entries = match read_log(path) {
        Ok(e) => e,
        Err(why) => {
            println!("FAIL midilog          {why}");
            return 1;
        }
    };
    let mut parser = libsynth::midi::Parser::new();
    let mut chans: Vec<Chan> = vec![Chan::default(); 16];
    let last_us = entries.last().map(|(us, _)| *us).unwrap_or(0);
    let secs = (last_us / 1_000_000) as usize + 1;
    for c in chans.iter_mut() {
        // What a channel is worth before anyone says otherwise: the
        // General MIDI defaults, so a report of 100/127 means nobody
        // touched them rather than that they were set.
        c.volume = 100;
        c.expression = 127;
        c.per_sec = vec![0; secs];
    }
    let mut sysex: Vec<(u64, Vec<u8>)> = Vec::new();
    let mut resets: Vec<u64> = Vec::new();
    let mut bytes = 0u64;
    let mut dropped = 0u64;
    // The most notes down at once, anywhere in the capture. This is the
    // measurement behind the commonest way for music to start right and
    // then thin out: past the engine's voice count every new note takes
    // one from a note that is still sounding, and what survives is
    // whatever was started last.
    let mut peak_held = 0usize;
    let mut peak_us = 0u64;

    for (us, entry) in &entries {
        let us = *us;
        let byte = match entry {
            Entry::Reset => {
                resets.push(us);
                dropped += parser.dropped();
                parser.reset();
                for c in chans.iter_mut() {
                    c.held.clear();
                }
                continue;
            }
            Entry::Byte(b) => *b,
        };
        bytes += 1;
        let Some(event) = parser.write(byte) else {
            continue;
        };
        let (status, d1, d2) = match event {
            libsynth::midi::Event::Sysex(data) => {
                sysex.push((us, data));
                continue;
            }
            libsynth::midi::Event::Message(s, a, b) => (s, a, b),
        };
        if status >= 0xF0 {
            continue;
        }
        let c = &mut chans[(status & 0x0F) as usize];
        match status & 0xF0 {
            0x90 if d2 > 0 => {
                c.notes += 1;
                c.per_sec[(us / 1_000_000) as usize] += 1;
                c.held.insert(d1, us);
                let down: usize = chans.iter().map(|c| c.held.len()).sum();
                if down > peak_held {
                    peak_held = down;
                    peak_us = us;
                }
            }
            0x80 | 0x90 => {
                c.held.remove(&d1);
            }
            0xC0 => {
                let bank = ((c.bank_msb as u32) << 7) | c.bank_lsb as u32;
                if c.patches.last() != Some(&(bank, d1)) {
                    c.patches.push((bank, d1));
                }
            }
            0xB0 => match d1 {
                0x00 => c.bank_msb = d2,
                0x20 => c.bank_lsb = d2,
                0x07 => {
                    c.volume = d2;
                    if d2 == 0 {
                        c.silenced.push((us, "volume (CC 7) to 0".into()));
                    }
                }
                0x0B => {
                    c.expression = d2;
                    if d2 == 0 {
                        c.silenced.push((us, "expression (CC 11) to 0".into()));
                    }
                }
                0x78 => {
                    c.held.clear();
                    c.silenced.push((us, "all sound off (CC 120)".into()));
                }
                0x7B => {
                    c.held.clear();
                    c.silenced.push((us, "all notes off (CC 123)".into()));
                }
                _ => {}
            },
            _ => {}
        }
    }
    dropped += parser.dropped();

    println!(
        "{}: {bytes} bytes over {:.1} s, {} port reset{}",
        path.display(),
        last_us as f64 / 1e6,
        resets.len(),
        if resets.len() == 1 { "" } else { "s" }
    );
    for (us, data) in &sysex {
        let hex: Vec<String> = data.iter().take(16).map(|b| format!("{b:02x}")).collect();
        println!(
            "  {:8.3} s  sysex, {} bytes: {}{}",
            *us as f64 / 1e6,
            data.len(),
            hex.join(" "),
            if data.len() > 16 { " …" } else { "" }
        );
    }
    println!();
    println!("  ch  note-ons  patches (bank:program)      vol  expr  held at end");
    for (i, c) in chans.iter().enumerate() {
        if c.notes == 0 && c.patches.is_empty() {
            continue;
        }
        let mut patches: String = c
            .patches
            .iter()
            .take(4)
            .map(|(b, p)| format!("{b}:{p}"))
            .collect::<Vec<String>>()
            .join(" ");
        if c.patches.len() > 4 {
            patches.push_str(" …");
        }
        println!(
            "  {:2}  {:8}  {:26}  {:3}  {:4}  {}",
            i + 1,
            c.notes,
            patches,
            c.volume,
            c.expression,
            c.held.len()
        );
    }

    // The columns. A channel that goes quiet while the others play on is
    // the whole diagnosis, and it is visible here and nowhere else.
    println!();
    println!("  note-ons per second, a column a second, a row a channel:");
    for (i, c) in chans.iter().enumerate() {
        if c.notes == 0 {
            continue;
        }
        let row: String = c
            .per_sec
            .iter()
            .map(|&n| match n {
                0 => '.',
                1..=9 => (b'0' + n as u8) as char,
                _ => '#',
            })
            .collect();
        println!("  {:2} |{row}|", i + 1);
    }

    let mut trouble = false;
    println!();
    for (i, c) in chans.iter().enumerate() {
        for (us, why) in &c.silenced {
            println!("  ch {:2} silenced at {:8.3} s by {why}", i + 1, *us as f64 / 1e6);
        }
        if !c.held.is_empty() {
            let oldest = c.held.values().min().copied().unwrap_or(0);
            println!(
                "  ch {:2} left {} note{} held, the oldest since {:.3} s",
                i + 1,
                c.held.len(),
                if c.held.len() == 1 { "" } else { "s" },
                oldest as f64 / 1e6
            );
            trouble = true;
        }
    }
    if dropped > 0 {
        println!("  {dropped} byte(s) the parser could attach to nothing: the stream and the parser disagree about where a message starts");
        trouble = true;
    }
    println!(
        "  at most {peak_held} note{} down at once (at {:.3} s) against the {} voices this engine has{}",
        if peak_held == 1 { "" } else { "s" },
        peak_us as f64 / 1e6,
        libsynth::gm::POLYPHONY,
        if peak_held >= libsynth::gm::POLYPHONY {
            " — past that every new note takes one from a note still sounding"
        } else {
            ""
        }
    );
    if peak_held >= libsynth::gm::POLYPHONY {
        trouble = true;
    }
    if !trouble {
        println!("  nothing held, nothing dropped: every note this guest sent was released");
    }
    i32::from(trouble)
}

/// Play a capture back with no guest, at the timing it was written with.
fn play(log: &Path, out: &Path, bank: &Path, roms: Option<&Path>) -> i32 {
    let entries = match read_log(log) {
        Ok(e) => e,
        Err(why) => {
            println!("FAIL play             {why}");
            return 1;
        }
    };
    let (kind, arg) = match roms {
        Some(r) => (capi::LIBSYNTH_MIDI_MT32, r),
        None => (capi::LIBSYNTH_MIDI_GM, bank),
    };
    let m = match midi_new(kind, arg) {
        Ok(m) => m,
        Err(why) => {
            println!("FAIL play             {why}");
            return 1;
        }
    };
    let rate = capi::libsynth_midi_rate(m);
    let mut pcm: Vec<i16> = Vec::new();
    let mut done = 0usize;
    // Rendered in blocks up to each byte's own timestamp, which is what
    // puts the guest's timing back: the engine hears the stream spaced
    // as the guest spaced it, not as fast as the file can be read.
    let until = |pcm: &mut Vec<i16>, done: &mut usize, frames: usize| {
        while *done < frames {
            let n = (frames - *done).min(512);
            pcm.resize((*done + n) * 2, 0);
            capi::libsynth_midi_render(m, pcm[*done * 2..].as_mut_ptr(), n);
            *done += n;
        }
    };
    for (us, entry) in &entries {
        until(&mut pcm, &mut done, (*us * rate as u64 / 1_000_000) as usize);
        match entry {
            Entry::Byte(b) => capi::libsynth_midi_write(m, *b),
            Entry::Reset => capi::libsynth_midi_reset(m),
        }
    }
    // Two seconds past the last byte: the release tail, and room for a
    // note nothing ever turned off to be heard still ringing.
    let tail = done + 2 * rate as usize;
    until(&mut pcm, &mut done, tail);
    capi::libsynth_midi_free(m);
    write_wav(out, rate, &pcm);
    let (rms, peak, busy) = level(&pcm, rate);
    println!(
        "{}: {:.1} s at {rate} Hz, {rms:.4} RMS, peak {peak:.3}, audible for {busy:.1} s",
        out.display(),
        done as f32 / rate as f32
    );
    0
}

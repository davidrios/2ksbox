//! Host audio output: an SPSC byte ring fed by QEMU's `embed` audiodev
//! (S16LE interleaved at the rate we tell QEMU) drained by a cpal stream.
//!
//! The producer drains the guest at the guest's own pace and holds the
//! ring's minimum at the cushion (`embed/embedaudio.c` says why it never
//! hurries); this side starts a stream only once the ring holds one of the
//! device's periods plus that cushion, and after an underrun waits for it
//! again, so a start or a stall costs one clean gap instead of a sputter.
//!
//! Two knobs make what the host hears inspectable without a speaker:
//! `PLAYER_AUDIO_NULL=<frames>` replaces the device by a thread that drains
//! the ring like a DAC with that period (`1` = 1024 frames, what PipeWire
//! hands a client by default) at 48 kHz, and `PLAYER_AUDIO_TAP=<file.wav>`
//! records exactly what the consumer handed the device, silence it padded
//! included — the file `tools/audio-glitch-test.py` counts clicks in.

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::io::{Seek, SeekFrom, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::{Duration, Instant};

pub const RING_BYTES: usize = 1 << 16; // 64 KiB ≈ 340 ms of 48 kHz stereo s16

/// The cushion QEMU keeps in the ring under the device's own pull (ms): how
/// late its main loop may be before the host hears a gap, and the latency
/// on top of the device's period. `PLAYER_AUDIO_MS`, 40 by default.
pub fn cushion_ms() -> u32 {
    std::env::var("PLAYER_AUDIO_MS")
        .ok()
        .and_then(|v| v.parse::<u32>().ok())
        .filter(|&ms| ms > 0)
        .unwrap_or(40)
}

pub struct Ring {
    pub buf: Box<[u8]>,
    pub wr: AtomicU32, // written by QEMU
    pub rd: AtomicU32, // written by us
}

impl Ring {
    pub fn new() -> Arc<Ring> {
        Arc::new(Ring {
            buf: vec![0u8; RING_BYTES].into_boxed_slice(),
            wr: AtomicU32::new(0),
            rd: AtomicU32::new(0),
        })
    }
    fn available(&self) -> usize {
        let wr = self.wr.load(Ordering::Acquire) as usize;
        let rd = self.rd.load(Ordering::Relaxed) as usize;
        (wr.wrapping_sub(rd)) & (RING_BYTES - 1)
    }
    /// Pop up to `out.len()` s16 samples; pads with silence when starved.
    fn pop_s16(&self, out: &mut [i16]) -> usize {
        let mut rd = self.rd.load(Ordering::Relaxed) as usize;
        let avail = self.available() / 2;
        let n = avail.min(out.len());
        for s in out.iter_mut().take(n) {
            *s = i16::from_le_bytes([self.buf[rd], self.buf[(rd + 1) & (RING_BYTES - 1)]]);
            rd = (rd + 2) & (RING_BYTES - 1);
        }
        self.rd.store(rd as u32, Ordering::Release);
        for s in out.iter_mut().skip(n) {
            *s = 0;
        }
        n
    }
    /// Discard `bytes` (whole frames) from the read side.
    fn skip(&self, bytes: usize) {
        let rd = self.rd.load(Ordering::Relaxed) as usize;
        let bytes = bytes.min(self.available()) & !3;
        self.rd
            .store(((rd + bytes) & (RING_BYTES - 1)) as u32, Ordering::Release);
    }
}

/// The host's side of the ring: what every device callback runs.
struct Consumer {
    ring: Arc<Ring>,
    tap: Option<mpsc::Sender<Vec<i16>>>,
    /// Bytes the ring must hold, on top of a callback's own, before a stream
    /// starts.
    cushion: usize,
    /// Bytes of stall the ring may soak up before the backlog is skipped.
    slack: usize,
    bytes_per_ms: usize,
    /// The device's callback size, frames.
    frames: usize,
    /// A stream is flowing; false while one is waited for.
    playing: bool,
    /// When the ring last ran dry mid-callback.
    dry_at: Option<Instant>,
    /// Since the last report: streams that ran dry and came back, the
    /// silence padded, the backlog skipped.
    underruns: u32,
    padded: usize,
    skipped: usize,
    last_report: Instant,
}

impl Consumer {
    fn new(ring: Arc<Ring>, rate: u32) -> Consumer {
        let bytes_per_ms = rate as usize * 4 / 1000;
        Consumer {
            ring,
            tap: tap(rate),
            cushion: cushion_ms() as usize * bytes_per_ms,
            slack: 100 * bytes_per_ms,
            bytes_per_ms,
            frames: 0,
            playing: false,
            dry_at: None,
            underruns: 0,
            padded: 0,
            skipped: 0,
            last_report: Instant::now(),
        }
    }

    fn fill(&mut self, out: &mut [i16]) {
        if self.frames != out.len() / 2 {
            // how chunky the device drains the ring: the first thing to know
            // about a host that crackles
            self.frames = out.len() / 2;
            eprintln!("[audio] device asks for {} frames a callback", self.frames);
        }
        let want = out.len() * 2;
        let avail = self.ring.available();
        if !self.playing {
            if avail >= want + self.cushion {
                self.playing = true;
                if self
                    .dry_at
                    .take()
                    .is_some_and(|t| t.elapsed() < Duration::from_secs(1))
                {
                    self.underruns += 1;
                }
            }
        } else if avail > 2 * want + self.cushion + self.slack {
            // The device stalled, or started late, and the ring holds more
            // than it will ever need: one skip back to where a stream starts
            // rather than that much latency for the rest of the session.
            let excess = avail - (want + self.cushion);
            self.ring.skip(excess);
            self.skipped += excess;
        }
        if self.playing {
            let n = self.ring.pop_s16(out);
            if n < out.len() {
                self.playing = false;
                self.padded += (out.len() - n) / 2;
                self.dry_at = Some(Instant::now());
            }
        } else {
            out.fill(0);
        }
        if let Some(tx) = &self.tap {
            let _ = tx.send(out.to_vec());
        }
        if self.last_report.elapsed() >= Duration::from_secs(5) {
            if self.underruns > 0 || self.skipped > 0 {
                eprintln!(
                    "[audio] {} underruns ({} frames of silence), {} ms skipped",
                    self.underruns,
                    self.padded,
                    self.skipped / self.bytes_per_ms
                );
            }
            self.underruns = 0;
            self.padded = 0;
            self.skipped = 0;
            self.last_report = Instant::now();
        }
    }
}

/// `PLAYER_AUDIO_TAP=<file.wav>`: a writer thread, so the device callback
/// only hands over a buffer. The header's sizes are rewritten every second
/// because a player is as often killed as it is closed.
fn tap(rate: u32) -> Option<mpsc::Sender<Vec<i16>>> {
    let path = std::env::var("PLAYER_AUDIO_TAP").ok()?;
    let mut f = match std::fs::File::create(&path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("[audio] PLAYER_AUDIO_TAP {path}: {e}");
            return None;
        }
    };
    eprintln!("[audio] tap: {path}");
    let (tx, rx) = mpsc::channel::<Vec<i16>>();
    std::thread::spawn(move || {
        let header = |data: u32| {
            let mut h = Vec::with_capacity(44);
            h.extend_from_slice(b"RIFF");
            h.extend_from_slice(&(36 + data).to_le_bytes());
            h.extend_from_slice(b"WAVEfmt ");
            h.extend_from_slice(&16u32.to_le_bytes());
            h.extend_from_slice(&1u16.to_le_bytes()); // PCM
            h.extend_from_slice(&2u16.to_le_bytes());
            h.extend_from_slice(&rate.to_le_bytes());
            h.extend_from_slice(&(rate * 4).to_le_bytes());
            h.extend_from_slice(&4u16.to_le_bytes());
            h.extend_from_slice(&16u16.to_le_bytes());
            h.extend_from_slice(b"data");
            h.extend_from_slice(&data.to_le_bytes());
            h
        };
        let _ = f.write_all(&header(0));
        let mut data = 0u32;
        let mut last = Instant::now();
        let mut bytes = Vec::new();
        while let Ok(buf) = rx.recv() {
            bytes.clear();
            for s in buf {
                bytes.extend_from_slice(&s.to_le_bytes());
            }
            if f.write_all(&bytes).is_err() {
                return;
            }
            data += bytes.len() as u32;
            if last.elapsed() >= Duration::from_secs(1) {
                let _ = f.seek(SeekFrom::Start(0));
                let _ = f.write_all(&header(data));
                let _ = f.seek(SeekFrom::End(0));
                last = Instant::now();
            }
        }
    });
    Some(tx)
}

pub struct Output {
    _stream: Option<cpal::Stream>,
    pub sample_rate: u32,
}

/// Open the default output device. Returns the rate/channels QEMU must be
/// configured with (we don't resample: `-audiodev embed,out.frequency=<rate>`).
pub fn start(ring: Arc<Ring>) -> Option<Output> {
    if let Ok(v) = std::env::var("PLAYER_AUDIO_NULL") {
        // headless: a thread drains the ring like a DAC with this period
        let period: usize = match v.parse() {
            Ok(n) if n > 1 => n,
            _ => 1024,
        };
        let rate = 48000;
        eprintln!("[audio] PLAYER_AUDIO_NULL: no device, a {period}-frame DAC at {rate} Hz");
        let mut consumer = Consumer::new(ring, rate);
        std::thread::spawn(move || {
            let mut buf = vec![0i16; period * 2];
            let step = Duration::from_secs_f64(period as f64 / rate as f64);
            let mut next = Instant::now() + step;
            loop {
                let now = Instant::now();
                if next > now {
                    std::thread::sleep(next - now);
                }
                consumer.fill(&mut buf);
                next += step;
            }
        });
        return Some(Output {
            _stream: None,
            sample_rate: rate,
        });
    }
    let host = cpal::default_host();
    let device = host.default_output_device()?;
    let default = device.default_output_config().ok()?;
    let sample_rate = default.sample_rate();
    let channels: u16 = 2;
    let config = cpal::StreamConfig {
        channels,
        sample_rate,
        buffer_size: cpal::BufferSize::Default,
    };
    let mut consumer = Consumer::new(ring, sample_rate);
    let stream = match default.sample_format() {
        cpal::SampleFormat::I16 => device
            .build_output_stream(
                config,
                move |data: &mut [i16], _| consumer.fill(data),
                |e| eprintln!("[audio] stream error: {e}"),
                None,
            )
            .ok()?,
        _ => {
            let mut tmp: Vec<i16> = Vec::new();
            device
                .build_output_stream(
                    config,
                    move |data: &mut [f32], _| {
                        tmp.resize(data.len(), 0);
                        consumer.fill(&mut tmp);
                        for (d, s) in data.iter_mut().zip(&tmp) {
                            *d = *s as f32 / 32768.0;
                        }
                    },
                    |e| eprintln!("[audio] stream error: {e}"),
                    None,
                )
                .ok()?
        }
    };
    stream.play().ok()?;
    eprintln!(
        "[audio] output {} Hz, {} ch, {:?}",
        sample_rate,
        channels,
        default.sample_format()
    );
    Some(Output {
        _stream: Some(stream),
        sample_rate,
    })
}

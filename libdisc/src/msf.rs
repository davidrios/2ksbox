//! MSF (minute:second:frame) ↔ LBA addressing.
//!
//! CD addressing: 75 frames per second, 60 seconds per minute, and a 150
//! frame (2 second) offset between the start of the program area (MSF
//! 00:02:00) and LBA 0. MMC commands use both; protection code cares that we
//! get the edges right. Checked by `discx selftest`'s `msf` check (no unit
//! tests: CLAUDE.md policy).

/// Frames (sectors) per second on CD.
pub const FRAMES_PER_SECOND: u32 = 75;
/// LBA 0 sits at MSF 00:02:00.
pub const MSF_OFFSET: i32 = 150;
/// The last sector an MSF can name: 99:59:74, one frame short of 100
/// minutes. Three BCD bytes have no room past it, so a disc longer than
/// this has sectors the TOC, the subchannel and every raw sector header
/// cannot address at all. `isodir` refuses to build one (a folder disc
/// is a CD-ROM: `atapi_disc_get_configuration` says so to the guest).
pub const MAX_LBA: i32 = (99 * 60 + 59) * FRAMES_PER_SECOND as i32 + 74 - MSF_OFFSET;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Msf {
    pub m: u8,
    pub s: u8,
    pub f: u8,
}

impl Msf {
    /// Absolute MSF → LBA. `00:02:00` → 0. Values below 2 seconds map to
    /// negative LBAs (lead-in addressing), which is intentional.
    pub fn to_lba(self) -> i32 {
        (self.m as i32 * 60 + self.s as i32) * FRAMES_PER_SECOND as i32 + self.f as i32 - MSF_OFFSET
    }

    /// LBA → absolute MSF, saturating at both ends of the addressable
    /// range rather than failing: this runs inside QEMU behind a C ABI
    /// (`capi.rs` turns a panic into `LIBDISC_EIO`), so an out-of-range
    /// address would surface as an I/O error on whatever command
    /// happened to convert one — the lead-out of a TOC, a sector header
    /// — long after the disc that cannot be addressed was accepted. A
    /// guest reads by LBA; the openers refuse an over-long disc up front
    /// (`isodir`, [`MAX_LBA`]), and what is left here is arithmetic that
    /// cannot bring the machine down.
    pub fn from_lba(lba: i32) -> Msf {
        let abs = (lba.clamp(-MSF_OFFSET, MAX_LBA) + MSF_OFFSET) as u32;
        let m = abs / (60 * FRAMES_PER_SECOND);
        let s = (abs / FRAMES_PER_SECOND) % 60;
        let f = abs % FRAMES_PER_SECOND;
        Msf {
            m: m as u8,
            s: s as u8,
            f: f as u8,
        }
    }
}

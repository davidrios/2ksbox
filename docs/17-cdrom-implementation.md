# 17. CD-ROM backend: implementation (M5, doc 05)

How the drive emulation of doc 05 is built: the `libdisc` disc model,
the image formats, error correction and subchannel, the C API, the MMC
reply layouts, the QEMU block driver and ATAPI patches, CD-DA, the
tests, what the protections we own actually read, and a host folder
served as a disc (§8). Doc 05 has the problem and the acceptance table;
`docs/tracks/m5-cdrom-backend.md` and `m5-dirdisc.md` are the track
records; the disc shelf is doc 07 and patch 52. The QEMU side was
written against the pinned tree (v9.2.4); function names are exact.

## 1. Shape

```
image on disk            libdisc (Rust crate, staticlib, C API in libdisc/libdisc.h)
 .cue+.bin / .ccd+.img+.sub    disc model: sessions, tracks, indices, per-sector kind,
 / .iso / .mds+.mdf            raw ⇄ cooked synthesis, EDC/ECC, subchannel Q, MMC responders
 / isodir:<folder>
        │                                  │
        ▼                                  ▼
 qemu/block/cdimage.c  ── a QEMU *format* block driver (+ the isodir protocol driver):
   cooked 2048-byte view through bdrv_co_preadv (qemu-img, SeaBIOS El Torito,
   blockdev-change-medium, eject all keep working), plus
   cdimage_disc(bs) → the libdisc handle for whoever wants the raw model
        │
        ▼
 qemu/hw/ide/atapi.c ── patch 51: every command asks `cdimage_disc(blk_bs(s->blk))`;
   NULL → stock QEMU, byte for byte (plain ISO on the `raw` driver);
   non-NULL → sector reads, TOC, subchannel, capabilities, CD-DA from the model
        │
        ▼
 guest: an ordinary IDE/ATAPI CD-ROM (in-box drivers, protection drivers unmodified)
```

Decisions (consistent with doc 05 and ADR-004; do not reopen):

1. **Integration is a block driver, not a device property.** `-cdrom
   game.cue` probes to `cdimage`; `-drive
   file=game.cue,format=cdimage,media=cdrom` is the explicit form. The
   drive stays QEMU's `ide-cd`; a medium swap is QMP
   `blockdev-change-medium`.
2. **The MMC reply bytes are computed in Rust.** `atapi.c` copies
   buffers and drives the IDE state machine; libdisc builds TOCs,
   subchannel replies and READ CD layouts, so the host-side exerciser
   tests the exact bytes a guest sees.
3. **Reads are synchronous** (`pread` inside libdisc). A guest reads at
   most 128 KiB per command; no AIO. If stalls ever show, mmap inside
   libdisc; the C API does not change.
4. **Copy-protection fidelity comes from modelling the drive**, not from
   lists of bad sectors: every data sector's EDC/ECC is checked, what
   the parity can locate is corrected as a drive's decoder does, and the
   rest fails as on a drive (§2.5, §2.6c). No annotation file, nothing
   patched or bypassed.
5. **A plain `.iso` stays on QEMU's `raw` driver** (probe score 0), so
   the stock path stays bit-identical and remains the regression
   baseline. `format=cdimage` on an `.iso` is allowed, and the tests
   compare the two paths.
6. **Subchannel is stored deinterleaved** (P..W, 12 bytes each, the
   CloneCD `.sub` layout); the MMC interleaved form is produced only in
   the READ CD responder.

## 2. The libdisc crate

`libdisc/` is a workspace member (`crate-type = ["rlib", "staticlib"]`)
with **no dependencies** — keep it that way; the staticlib is linked
into QEMU.

| File | Contents |
|---|---|
| `src/lib.rs` | the model (§2.1), `Disc::open` dispatching on extension, content, or a directory |
| `src/msf.rs` | `Msf` ⇄ LBA (LBA 0 = MSF 00:02:00) |
| `src/cue.rs`, `ccd.rs`, `mds.rs`, `iso.rs` | the image formats (§2.3–2.4b) |
| `src/isodir.rs` | a host folder as an ISO 9660 + Joliet volume (§8) |
| `src/sector.rs` | raw ⇄ cooked: sync, header, subheader, EDC placement, sector-kind detection |
| `src/ecc.rs` | EDC and RSPC P/Q parity, verification and correction (§2.5) |
| `src/subq.rs` | Q-channel synthesis and CRC-16, P channel (§2.6) |
| `src/mmc.rs` | READ TOC 0/1/2, READ SUB-CHANNEL, READ CD, READ DISC INFORMATION (§4) |
| `src/capi.rs` | the `extern "C"` functions of `libdisc/libdisc.h` (§3) |
| `src/bin/discx.rs` | the exerciser and diagnosis tool (§6.1) |
| `qemu/cdimage.c`, `cdimage.h` | the block drivers, overlaid into QEMU by `prepare-qemu.sh` (§5) |
| `libdisc.h` | the one C header, overlaid next to them |

### 2.1 Model

```rust
pub struct Disc {
    pub sessions: Vec<Session>,          // 1-based, ascending
    pub mcn: Option<[u8; 13]>,           // CATALOG
    pub raw_toc: Option<Vec<TocEntry>>,  // CCD/MDS entries verbatim; None = synthesize
    files: Vec<Payload>,                 // path, length, mtime; opened on demand
    blobs: Vec<Vec<u8>>,                 // generated sectors (isodir metadata)
    fds: Mutex<Vec<(usize, Arc<File>)>>, // 8-entry MRU handle cache
}
pub struct Session { pub number: u8, pub tracks: Vec<Track>, pub leadout_lba: i32 }
pub struct Track {
    pub number: u8,                      // 1..=99, unique across sessions
    pub mode: TrackMode,                 // Audio, Mode1, Mode2 (form per sector), Mode2Form1/2, Mode2Formless
    pub control: u8,                     // audio 0x0 (|1 pre-emphasis, |2 DCP, |8 4ch), data 0x4
    pub isrc: Option<[u8; 12]>,
    pub indices: Vec<(u8, i32)>,         // (index, absolute LBA), index 1 always present
    pub start_lba: i32,                  // index 1
    pub end_lba: i32,                    // exclusive
    pub extents: Vec<Extent>,            // contiguous coverage of [first index .. end_lba)
}
pub struct Extent { pub lba: i32, pub count: u32, pub source: Source, pub sub: Option<SubSource> }
pub enum Source {
    File { file: usize, offset: u64, layout: Layout, swap: bool, eof_pad: bool },
    Mem { blob: usize, offset: u64 },    // isodir's descriptors, path tables, directories
    Silence,                             // audio pre/postgap
    ZeroData,                            // data pre/postgap, valid EDC/ECC
}
pub enum Layout { Cooked2048, Mode2_2336, Raw2352, Raw2352Sub96 }
pub enum SubSource { File { file: usize, offset: u64 } } // 96 B/sector, deinterleaved
```

Rules:

- LBAs are `i32`; the program area starts at LBA 0 (MSF 00:02:00). The
  readable range is `[0, last session lead-out)`; anything else is
  `Err(Range)`. libdisc never invents lead-in data, and track 1's
  mandatory 150-sector pregap is never in a file and never addressed.
- The last session's lead-out is `sector_count()`, READ CAPACITY and
  TOC point A2.
- **A *CD* ends at `msf::MAX_LBA`** (449,849 — MSF 99:59:74, ~878 MiB):
  three BCD bytes cannot address past it. A *disc* does not: past an
  80-minute CD (`CD_MAX_SECTORS` in `atapi.c`, 360,000 sectors) the
  drive reports a DVD-ROM profile (patch 53) and nothing asks for an
  MSF, so the model goes on to a dual-layer DVD-9, 4,173,824 sectors.
  `Msf::from_lba` saturates rather than asserting — libdisc runs in
  QEMU, where `capi.rs` turns a panic into `LIBDISC_EIO` on whatever
  command converted the address — so **the opener is where a disc past
  any real medium is refused** (§8).
- Payload files are held as path, length and mtime and opened on demand
  into the MRU cache; every open re-`stat`s, and a changed file is
  `Error::Medium`, a read error as from a damaged disc, never a torn
  read. `eof_pad` lets an extent's last sector run past its file's end;
  only `isodir` sets it, so a short image file stays an error. A host
  read failure is logged on stderr (`libdisc: …`, the first 32), since
  a guest sees only a sense code.
- `extent_at` is a binary search.
- Sector reads go through `read_raw(lba) -> [u8; 2352]` (stored, or
  synthesized from cooked per §2.5) and `read_sub(lba) -> [u8; 96]`
  (stored, or synthesized per §2.6). `read_cooked(lba)` is raw → kind
  detection → L-EC → user data, or `Err(Medium)` / `Err(Mode)` (audio,
  gap).

### 2.2 Byte conventions

- **Header MSF and Q-channel times are BCD** (`0x21` = 21). MMC TOC and
  subchannel *replies* carry binary MSF or a 32-bit big-endian LBA.
- A sector header's absolute MSF is the MSF of `lba + 150`.
- CD-DA: 16-bit little-endian, left then right, 588 stereo frames per
  sector. `FILE … MOTOROLA` is big-endian (swap). `FILE … WAVE`: parse
  the RIFF chunks and use the `data` chunk's offset (not a fixed 44);
  anything but 44100 Hz / 16-bit / stereo is refused.
- Raw sectors in dumps are **descrambled** already; we never scramble.
- Sync: `00 FF×10 00`. Header: `MIN SEC FRAME MODE` (BCD ×3, then 1 or
  2). Mode 2 subheader: bytes 16..24; form 2 = bit 5 of byte 18.

### 2.3 Cue sheets (`cue.rs`)

Accepted (case-insensitive keywords, quoted or bare names, `REM`
ignored, CRLF or LF):

```
CATALOG 1234567890123
FILE "name" BINARY|MOTOROLA|WAVE
  TRACK nn AUDIO|MODE1/2048|MODE1/2352|MODE2/2336|MODE2/2352|CDI/2336|CDI/2352
    FLAGS [DCP] [4CH] [PRE] [SCMS]
    ISRC XXXXXXXXXXXX
    PREGAP mm:ss:ff
    INDEX nn mm:ss:ff
    POSTGAP mm:ss:ff
```

Where cue parsers go wrong:

- `INDEX` times are **relative to the start of the current FILE**,
  without the 150 offset. Absolute LBA = the running LBA at the file's
  start + the index frames.
- A track's sectors run from its lowest index to the next track's lowest
  index in the same file, or the file's end. File sectors = size ÷
  stride (2048 / 2336 / 2352; a remainder is an error naming the file).
- `PREGAP` sectors are **not in the file**: synthesized before index 01
  as index 00 (silence, or zero data with valid EDC/ECC). `POSTGAP`
  likewise after the last sector. Absolute LBAs advance across both.
- Several `FILE`s concatenate; the running LBA carries over.
- Track 1 starts at LBA 0; `INDEX 00 00:00:00` + `INDEX 01 00:02:00`
  (pregap in the file) is accepted as is.
- Control: AUDIO 0x0, +0x1 `PRE`, +0x2 `DCP`, +0x8 `4CH`; data 0x4.
- Payloads resolve relative to the cue, then case-insensitively (dumps
  made on Windows), then fail naming the path tried.
- Single session; a declared second session is named in the error.

### 2.4 CloneCD (`ccd.rs`)

`.ccd` is an INI file:

```
[CloneCD]  Version=3
[Disc]     TocEntries=n  Sessions=n  DataTracksScrambled=0  CDTextLength=0  CATALOG=…
[Session n] PreGapMode=n PreGapSubC=n
[Entry n]  Session= Point= ADR= Control= TrackNo= AMin= ASec= AFrame= ALBA= Zero= PMin= PSec= PFrame= PLBA=
[TRACK n]  MODE=0|1|2  INDEX 0=lba  INDEX 1=lba  ISRC=…
```

- `.img`: 2352 bytes per sector from LBA 0; `.sub` (optional): 96 bytes
  per sector, deinterleaved. `DataTracksScrambled=1` is refused.
- Tracks come from `[Entry]` records with `Point` 1..99 (`PLBA` = index
  1, `Control`), `[TRACK n]` adds `INDEX 0` and the mode; session
  lead-outs from `Point=0xA2`. One `Session` per distinct `Session=`.
- **Every `[Entry]` is kept verbatim** in `raw_toc` and replayed by READ
  TOC format 2 (§4.1): the fidelity SecuROM-era checks want.

### 2.4b MDS/MDF (`mds.rs`)

Alcohol 120% / Daemon Tools: a `MEDIA DESCRIPTOR` header (version 1.x),
24-byte session blocks at the offset in header byte 0x50, 80-byte track
blocks (mode, subchannel flag `0x08` = 96 bytes interleaved appended,
ADR/control, point, MSF, PMSF, extra-block offset, sector size
2048/2352/2448, start sector, 64-bit start offset, footer with the
`.mdf` name) and extra blocks (pregap, length). Every block with a point
becomes a raw TOC entry. Modes: `0xA9` audio, `0xAA` Mode 1, `0xAB` Mode
2, `0xAC`/`0xAD` form 1/2, and **`0xEC`, Alcohol's mixed Mode 2 (XA)**,
form per sector — read as Mode 2, not audio (NFS Porsche Unleashed's
MDS once came out as one unmountable 281,279-sector CD-DA track). An MDS
whose mode byte contradicts its TOC control bits is refused rather than
misread.

**Layout rule, checked against a RAW+SUB dump's own Q frames:** the
`.mdf` holds each track from `start_sector` (index 1) for `length`
sectors at `start_offset`; the `pregap` sectors are *not* in the file
and are synthesized as index 0; track 1's pregap of 150 is never
addressed. DPM blocks (header byte 0x54) are ignored until a title
needs timing.

### 2.5 Raw ⇄ cooked and L-EC (`sector.rs`, `ecc.rs`)

Raw layouts (2352 bytes): Mode 1 `sync[12] header[4] data[2048] edc[4]
zero[8] p[172] q[104]`; Mode 2 form 1 `sync header subheader[8]
data[2048] edc[4] p[172] q[104]`; form 2 `sync header subheader
data[2324] edc[4]` (EDC optional, may be zero).

**EDC**: CRC-32, polynomial x^32+x^31+x^16+x^15+x^2+x+1, LSB-first
(table from reflected `0xD8018001`), init 0, no final XOR, stored
little-endian. Coverage: Mode 1 bytes 0..2064; form 1 16..2072; form 2
16..2348.

**ECC (RSPC)** over GF(2^8), primitive polynomial `0x11D`, on the
2064-byte area bytes 12..2076 (for Mode 2 the header is taken as four
zero bytes). The standard two-pass loop (Neill Corlett's ECM; libmirage
does the same); each pass writes two parity bytes per major, at
`dst[major]` and `dst[major + major_count]`:

| Pass | major_count | minor_count | major_mult | minor_inc | writes to |
|---|---|---|---|---|---|
| P | 86 | 24 | 2 | 86 | sector[2076..2248] |
| Q | 52 | 43 | 86 | 88 | sector[2248..2352] |

For major `m`: `index = (m >> 1) * major_mult + (m & 1)`, `a = b = 0`;
`minor_count` times: `t = src[index]`, `index += minor_inc` (wrapping at
2064, or 2064 + 172 for Q, which reads the P parity too), `a ^= t; b ^=
t; a = gf_mul2(a)`; then `a = ecc_b_lut[a ^ b]`, `dst[m] = a`, `dst[m +
major_count] = a ^ b`. The tables: `ecc_f_lut[i] = 2·i` in GF,
`ecc_b_lut[ecc_f_lut[i] ^ i] = i`.

`verify_mode1` / `verify_mode2f1` recompute and compare: `Ok`,
`EdcMismatch`, `EccMismatch`, or `NoSync` (no sync or a wrong mode byte
in a data track — an audio-format tail, or the zero filler a dumper
writes for an unreadable sector, whose all-zero EDC and parity would
otherwise verify).

**A cooked read is decided by the EDC and repaired by the parity only
when the EDC fails.**

1. **The EDC says whether the delivered bytes are intact**
   (`ecc::edc_ok`). It is a CRC-32 over exactly what a cooked read hands
   over, so when it holds, any disagreement is in parity fields the guest
   never sees: the sector is delivered and the parity not even computed.
   The sync is checked first. A real dump settled this: a Warcraft 3
   disc with 92 L-EC failures, every one EDC-clean, whose in-game video
   stopped midway while we refused sectors that were provably right.
2. **A wrong EDC gives the decoder work** (`ecc::correct`). Each P and Q
   codeword's two parity symbols locate one wrong symbol (a single error
   of magnitude `s0` at position `i` gives `s1/s0 = alpha^(m+1-i)`).
   Correct one per codeword, alternate the passes, up to four rounds;
   **the EDC is the verdict here too**: a sector is accepted, and written
   back, only when its CRC-32 then holds, so a mis-correction cannot
   pass. On the selftest disc a burst of up to 96 bytes is recovered
   (the interleave spreads it over codewords); 128 bytes, a filled body
   or a zeroed sector are not.

Only what the decoder cannot fix is `Err(Medium)`. `sector_info`'s `lec`
still reports whether the sector verifies *as stored*, which is what
`discx scan` counts. `LIBDISC_NO_CORRECT=1` turns both steps off (every
L-EC failure a medium error) for an A/B.

This is the drive's behaviour, not a bypass: a real drive's L-EC
hardware corrects before it hands data over, and a protection band
survives it because its sectors are damaged far past one symbol per
codeword (DiscImageCreator writes the body as `0x55`) and their EDC is
wrong. Without correction an ordinary dump with a few imperfect sectors
gives a guest hard errors where the disc gives data. What a *raw* read
delivers is §4.3.

C2 pointers: a bit for every byte whose recomputed parity disagrees —
approximate, enough for checks that count errors.

Synthesis (cooked image → raw request): sync + BCD header + data + EDC
+ zero + P + Q per sector on demand (~10 µs; no cache).

### 2.6 Subchannel synthesis (`subq.rs`)

With no `.sub`, `read_sub(lba)` returns: P all `0xFF` inside index 00
and for the 150 sectors before a track's index 01 (the pause flag), else
`0x00`; Q as below; R..W zero. A `.sub` wins for every sector it covers;
sectors past a truncated one are synthesized.

Q, ADR 1 (position):

```
byte 0     control<<4 | adr(1)
byte 1     TNO   BCD track number
byte 2     INDEX BCD (00 in pregap)
byte 3-5   MIN SEC FRAME  BCD, track-relative; in index 00 it counts DOWN
           (to 00:00:01 at the last pregap sector on both real discs measured)
byte 6     ZERO
byte 7-9   AMIN ASEC AFRAME  BCD absolute (lba + 150)
byte 10-11 CRC-16 big-endian: CCITT 0x1021, init 0, over bytes 0..10, inverted
```

With an MCN or ISRC, ADR 2 goes at `lba % 100 == 98` and ADR 3 at `==
99` (MCN: 13 BCD digits high nibble first in bytes 1..7, byte 8 zero,
AFRAME in byte 9; ISRC: 12 characters 6-bit packed per Red Book in bytes
1..8, AFRAME in byte 9).

**Synthesizing an undeclared pregap is a guess, and it stays one.**
Where a descriptor gives a track no index 00, the disc may be any of
three things and nothing in the descriptor tells them apart (the MDS
`pregap` field is 0 for every such track on both Alcohol dumps we have;
measured with `discx subscan`):

| Disc | P before the next track | Q there |
|---|---|---|
| AoE Gold | `0xFF` (pause) | previous track, index 01, counting up |
| Moto Racer | `0xFF` (pause) | next track, index 00, counting down over 149 sectors |
| Settlers 3 | `0x00` | previous track, index 01, counting up |

We synthesize the first: it reproduces AoE's subchannel exactly (277,626
of 277,626 ADR 1 frames) and costs ~0.7 % of frames on a
Moto-Racer-shaped disc; the second convention traded 1,633 wrong frames
on one disc for 1,866 and 1,650 on two others. MCN placement is the same
kind of guess: Rayman 2, the one disc here with MCN frames (3,307),
places them elsewhere (99.0 % agreement). The residual reaches no game —
AoE Gold and Moto Racer both play their soundtracks in-game — and
nothing met so far reads Q (§2.6b). Anything that fingerprints
subchannel wants a dump that carries it (CCD `.sub`, MDS 2448), which is
replayed verbatim.

### 2.6b The negative control, and what the protections read

A check that ran and was satisfied looks exactly like one that never
ran. `discx repair` builds the disc that tells them apart: a copy in
which every L-EC-failing sector verifies, the user data exactly as
dumped, the difference confined to bytes 2064–2351 of those sectors
(§6.1). The control copies live under `oldstuff/clean/` (FIFA 2002: 584
sectors repaired; Settlers 3 CD01: 547, leaving the 150 sync-less
run-out sectors a drive fails too).

**Both titles run from their repaired copies** (user, 2026-09-05), so
SafeDisc 2.x and ProtectCD are inconclusive in doc 05, not passing. Both
binaries are genuinely wrapped (`7z` reads the qcow2 directly):
`fifa2002.exe` has SafeDisc 2's `stxt371` / `stxt774` sections and
`BoG_` marker, `S3.EXE` ProtectCD's `.ficken` section. FIFA 2002 does
ask for the CD with an empty drive, but a presence check would do that
too.

**The ATAPI traces** (`-trace ide_atapi_cmd_packet -trace
ide_atapi_cmd_error`, which also works through the player's QEMU
arguments), FIFA 2002 from its `.mds` on one image:

| Run | CD on | Display | Outcome | Reads | Multi-sector | Max LBA | Band reads |
|---|---|---|---|---|---|---|---|
| bare QEMU | ide0 slave | cirrus | *"insira o CD"* | 488 | 1 | 13272 | 0 |
| bare QEMU `-nodefaults` | ide0 slave | cirrus | *"insira o CD"* | 506 | 1 | 13272 | 0 |
| bare QEMU | ide.1 | cirrus | no dialog, then a crash | 505 | — | — | 0 |
| player | ide.1 | d3dpt-vga | **runs** | 781 | 281 | 250230 | **0** |

- SafeDisc 2's probe is an anchor `READ(10)` at LBA 800 and one
  pseudo-random single sector (~1300–9900), repeated (13 pairs in the
  passing launch): a timing-shaped pattern, not an error-pattern check.
  **It never reads a band sector**, which is why the repaired disc
  passes.
- Settlers 3 CD01 (`.ccd`, in the player) reaches its menu in 1196
  reads: none above LBA 191776, below the band at 195539, and one READ
  SUB-CHANNEL. Neither its data anomaly nor its Q anomaly is read, and
  the `.cue` (synthesized, clean Q) and `.ccd` (the disc's own Q) behave
  the same.
- **The check refuses when the CD shares the boot disk's IDE channel**
  (`-drive media=cdrom` lands at ide0 slave) and passes on `ide.1`,
  where the player and the launcher put it. A timing check perturbed by
  the shared channel fits, but is a hypothesis; the correlation is
  measured. Run protected titles with the CD on its own channel.
- **Attribute every sense reply to its drive before calling it a bug.**
  The `GET CONFIGURATION` errors in the first trace came from QEMU's
  *empty* default CD drive, which has no model and answers from stock
  code (feature 0 only). Ours answers an unsupported starting feature
  with the header alone, as MMC wants. The one error our drive returns,
  MODE SENSE page 0x1b, is the correct reply to a page we do not
  implement.

So for SafeDisc 2 and ProtectCD the L-EC band is not what the check
reads. §2.5 is still the right drive behaviour, and `atapi-guest`
proves the errors are delivered; the scheme that depends on them is
§2.6c.

### 2.6c The protection that does read the band

**Crimson Skies**, SafeDisc 1.50.020 (the original loader,
`CRIMSON2.EXE` in the user's `claude98` install: `BoG_ *90.0&!!` at
0x29250 with version dwords `(1, 50, 20)`, the Secdrv strings,
`CRIMSON.ICD` beside it). Its dump `C_SKIES.cue` (Aaru 5.4.1) carries
**579 sectors from LBA 807 to 10018**, all with no sync pattern at all
(not `0x55` fill, and not merely scrambled). So a SafeDisc 1.x disc can
carry a band; which ones do is for `discx scan` to say (§6.x).

The loader does three rounds of an anchor `READ(10)` at LBA 800 and one
pseudo-random sector in ~1400..10000 read **raw** (`READ CD`, byte 9 =
`0xF8`). With the stored bytes delivered, every round stopped on the
first probe that hit a band sector, and the loader refused with
*"Cannot locate the CD-ROM"*. With a raw read of an unreadable sector
answered `03/11/05` — one variable — the loader does REQUEST SENSE,
carries on probing, decrypts `CRIMSON.ICD` and reaches the game's
"Select Video Device" dialog. It is the one check seen to fail and then
pass on the drive model alone. NFS Porsche Unleashed (SafeDisc 1.x, 0
L-EC failures over 281,279 sectors) works: the band is the difference.

The user's own install runs a no-CD `CRIMSON.EXE` that never reads the
disc; its rendering problems are the display driver's (doc 19), not
this.

**The raw rule** (`mmc::read_cd_sector`), which replaced "a raw request
delivers the bytes as dumped":

- user data selected, EDC/ECC not → a cooked read: verify, correct,
  `Err(Medium)` for the rest;
- user data and EDC/ECC, **no C2 field** → the stored bytes, but only
  if the sector is readable at all (the same `verify_or_correct` as the
  cooked path); an unreadable one is `Err(Medium)`, as a drive answers;
- **C2 error flags asked for** (byte 9 bits 1–2) → the stored bytes
  regardless, with C2 marking the untrusted ones: how a dumping tool gets
  an unreadable sector out of a real drive.

`discx selftest`'s `lec` case carries both halves.

## 3. The C API (`libdisc/libdisc.h`)

One hand-written header for the crate, the block driver and `atapi.c`,
like `d3dpt/d3dpt_proto.h`. Bump `LIBDISC_API_VERSION` (1) on any
change; `cdimage_open` refuses a mismatch. Every function is
thread-safe on one handle: the layout is immutable and the payload
handle cache is behind a lock. `libdisc_open` also takes a directory
(§8).

| Call | Does |
|---|---|
| `libdisc_api_version`, `libdisc_probe(head, len, filename)` | 0..100 confidence: `.cue` that parses, `.ccd` with `[CloneCD]`, `.mds` → 100; `.iso` → 0 (stays on `raw`) |
| `libdisc_open(path, err, errlen)` / `libdisc_close` | NULL and a message on failure |
| `libdisc_sector_count`, `_session_count`, `_track_count`, `_track_info`, `_sector_info` | the layout; `LibdiscTrackInfo` has number, session, control, mode, `start_lba` (index 1), `pregap_lba`, `end_lba`; `LibdiscSectorInfo` kind, track, index, `lec` as stored |
| `libdisc_read_cooked` / `_read_raw` / `_read_sub` | 2048 L-EC-checked bytes / 2352 / 96 deinterleaved |
| `libdisc_mmc_read_toc`, `_read_subchannel`, `_read_disc_information` | a reply into `out`, its length or an error; the caller truncates to the allocation length |
| `libdisc_mmc_read_cd_length` / `_read_cd_sector` | bytes per sector for a READ CD CDB (or `EINVAL`), then one call per sector |

| Code | Meaning | Sense |
|---|---|---|
| `LIBDISC_ERANGE` −1 | LBA outside `[0, lead-out)` | 05/21/00 |
| `LIBDISC_EMEDIUM` −2 | unreadable sector (§2.5, §4.3), or a changed payload | 03/11/05 |
| `LIBDISC_EMODE` −3 | wrong sector kind for the request | 05/64/00 |
| `LIBDISC_EINVAL` −4 | bad parameter or CDB field | 05/24/00 |
| `LIBDISC_EIO` −5 | host file read failed, or a caught panic | 04/xx |

`capi.rs` holds a `Box<Disc>` behind the opaque pointer, fills `err` as
NUL-terminated ASCII, and wraps every body in `catch_unwind`: a panic
never crosses the boundary.

## 4. MMC reply layouts (`mmc.rs`)

MMC-3 (T10 1363-D), which Win9x/XP's `cdrom.sys` and period protection
drivers assume. Multi-byte fields big-endian; `msf=1` → `0, M, S, F`
binary, `msf=0` → 32-bit LBA.

### 4.1 READ TOC/PMA/ATIP (0x43)

A 2-byte data length (excluding itself), then:

- **Format 0** (TOC): first and last track, then an 8-byte descriptor
  per track from `start` (0 or 1 = all, 0xAA = lead-out only, past the
  last track → EINVAL): `reserved, ADR<<4|control, track, reserved,
  time`; then track 0xAA, control 0x14 if the last track is data else
  0x10, at the last lead-out. Multisession lists every session's
  tracks. Against QEMU's own `cdrom_read_toc` (kept for the no-disc
  path) the one deliberate difference is the lead-out's control: ours
  `0x14`, as a real drive reports for a data disc, QEMU's `0x16`.
- **Format 1** (multisession): first and last session, one descriptor
  for the first track of the last session.
- **Format 2** (raw TOC): first and last session, then 11-byte
  descriptors `session, ADR<<4|control, TNO 0, POINT, MIN, SEC, FRAME,
  ZERO, PMIN, PSEC, PFRAME`; per session `A0` (PMIN first track, PSEC
  disc type: 0x20 CD-ROM XA when the session's first track is Mode 2,
  else 0x00), `A1` (last track), `A2` (lead-out), then each track at its
  index 1. Multisession adds `B0` (ADR 5) after the last `A2`: the last
  lead-out + 150 and 0x4C:0x2C:0x00, as drives report for closed discs.
  **Always MSF**, whatever the `msf` bit. With `raw_toc` present its
  entries are emitted verbatim.
- Formats 3–5 → EINVAL.

### 4.2 READ SUB-CHANNEL (0x42)

`out[0] = 0`, `out[1]` = audio status (0x11 playing, 0x12 paused, 0x13
completed, 0x14 error, 0x15 none), `out[2..4]` = data length; `subq=0`
→ the header only.

- **Format 1** (position): `1, ADR<<4|control, track, index, absolute
  time, relative time` from the Q of the position (§5.4); relative time
  counts down in index 0.
- **Format 2** (MCN): `2, 0, 0, 0, MCVal<<7`, 13 ASCII digits + NUL.
- **Format 3** (ISRC): `3, ADR<<4|control, track, 0, TCVal<<7`, 12
  ASCII + NUL + 0.

### 4.3 READ CD (0xBE) and READ CD MSF (0xB9)

Expected sector type `(byte1 >> 2) & 7`: 0 any, 1 CD-DA, 2 Mode 1, 3
Mode 2 formless, 4 form 1, 5 form 2. Byte 9: sync 0x80, header 0x60
(01 header, 10 subheader, 11 both), user data 0x10, EDC/ECC 0x08, C2
0x06 (01 = 294 bytes, 10 = 296 with the block error byte). Byte 10 & 7:
subchannel 0 none, 1 raw 96 (interleaved), 2 Q 16 bytes, 4 R–W 96.

`read_cd_length` sums the selected fields for the type per MMC-3's
table (Mode 1 `0xF8` = 2352, `0x10` = 2048; CD-DA any combination =
2352); C2 adds 294 or 296, subchannel 96 or 16; illegal combinations
are EINVAL, a sector of the wrong type EMODE (type 0 takes anything).
The fill copies sync, header, subheader, data, EDC/ECC, C2, subchannel,
in that order. MSF form: start inclusive, end exclusive.

**L-EC applies to a raw read too**, by the raw rule in §2.6c: only a
request that asks for C2 flags gets an unreadable sector's bytes.

### 4.4 READ DISC INFORMATION (0x51)

34 bytes: length 32, status 0x0E (complete, last session complete),
first track 1, sessions, first and last track of the last session,
lead-in and lead-out `0xFFFFFFFF`, disc type by the A0 rule. The stock
`cmd_read_disc_information` stays for the no-disc path.

## 5. The QEMU side

### 5.1 `block/cdimage.c` (in the repo: `libdisc/qemu/cdimage.c`)

A read-only format driver on `block/bochs.c`'s model:

- **probe** is `libdisc_probe`; **open** takes the `file` child as bochs
  does (the `.cue` itself becomes `bs->file`), refuses read-write
  (`bdrv_apply_auto_read_only`) and an API-version mismatch, and calls
  `libdisc_open` on the child's filename;
- **getlength** is `sector_count × 2048`, `request_alignment` 2048;
- **co_preadv** reads per sector through `libdisc_read_cooked`; any
  libdisc error is `-EIO`;
- **`cdimage_disc(bs)`**, exported through `include/block/cdimage.h`,
  returns the handle for `atapi.c`.

**The `isodir` protocol driver lives in the same file** (§8): the same
state and read path, no `file` child, and `isodir:/path` instead of a
probe, because a directory can be neither probed nor a `file` child
(QEMU's own precedent is vvfat's `fat:`). `bdrv_parse_filename` strips
the prefix. It needs no QEMU patch.

**The block layer puts a `raw` format node above a protocol driver it
resolved from a prefix**, so `cdimage_disc()` walks down through format
nodes (after `bdrv_skip_filters`). When it fails to, nothing breaks
visibly: the guest still reads files, and only the model's answers go
missing (the TOC, READ CD, a bad sector's sense). `CDIMAGE_TRACE=1`
prints every packet, reply and sense **only** when the model was found,
which the `dirdisc` check uses as its proof.

`atapi.c` calls `cdimage_disc` under the BQL on every command; a medium
change replaces `blk_bs(s->blk)`, so the handle is never cached in
IDEState.

### 5.2 Build: patch `50-cdimage-block-driver`

- A meson option `libdisc_dir` (the directory holding `liblibdisc.a`),
  the `libdisc` dependency with the staticlib's native libraries per
  platform, `CONFIG_CDIMAGE`, and `cdimage.c` in `block_ss`.
  `atapi.c`'s disc paths are under `#ifdef CONFIG_CDIMAGE`.
- `scripts/configure-qemu.sh` runs `cargo build --release -p libdisc`
  first (no cycle: the crate has no QEMU dependency) and passes
  `-Dlibdisc_dir=target/release`. `prepare-qemu.sh` overlays
  `cdimage.c` into `qemu/block/`, and `cdimage.h` and `libdisc.h` into
  `qemu/include/block/`, before the patch loop.
- **meson does not track the staticlib**: `cc.find_library` makes it
  no target's input, so after a change under `libdisc/` a plain `ninja`
  keeps the old code in `qemu-system-i386`, `qemu-img` and the embed
  library (seen as `qemu-img` calling an MDS "not a disc image libdisc
  reads"). `scripts/build-libdisc.sh` runs cargo and deletes those
  targets so ninja relinks them; `scripts/build.sh` builds libdisc
  before QEMU links it.
- The staticlib is linked into `qemu-system-i386` and
  `libqemu-embed-i386`. The player is Rust too, so two copies of `std`
  share the process; on macOS the export list hides libdisc's symbols,
  and on Linux the copies merely interpose identical code. QEMU's own
  Rust support is not enabled; cargo builds the crate outside meson.

### 5.3 ATAPI: patch `51-atapi-disc-model`

Files: `hw/ide/atapi.c`; `hw/ide/ide-internal.h` (ASC 0x11 unrecovered
read error, 0x64 illegal mode for this track, and
`ide_atapi_cmd_error_ascq`); `include/hw/ide/ide-dev.h` (new IDEState
fields, **not** in `vmstate_ide_drive`: migration and snapshots with a
disc attached are unsupported); `hw/ide/ide-dev.c` (`audiodev` on
`ide-cd`, §5.4).

```c
bool     atapi_disc_read;          /* this transfer is served by libdisc, not blk */
uint8_t  atapi_rc_type, atapi_rc_b9, atapi_rc_b10;  /* READ CD parameters; READ(10) = 0, 0x10, 0 */
uint32_t atapi_last_lba;           /* the head, for READ SUB-CHANNEL when not playing */
uint8_t  atapi_audio_status;       /* 0x11 playing … 0x15 none */
uint32_t atapi_play_lba, atapi_play_end;
uint8_t  atapi_audio_port[4], atapi_audio_vol[4];   /* mode page 0x0E */
QEMUSoundCard *atapi_card; SWVoiceOut *atapi_voice; QEMUTimer *atapi_play_timer;
```

Every handler asks `atapi_disc(s)` itself. New table entries, all
`CHECK_READY` (the audio ones `NONDATA` too): READ SUB-CHANNEL 0x42,
PLAY AUDIO 0x45 / 0x47 / 0x48 / 0xA5, PAUSE/RESUME 0x4B, STOP PLAY/SCAN
0x4E, MODE SELECT(10) 0x55, READ CD MSF 0xB9; START STOP UNIT 0x1B stops
audio too. With no disc the audio commands are no-ops leaving status
0x15, READ SUB-CHANNEL reports 0x15 at position 0, READ CD MSF behaves
like READ CD.

**The transfer path.** The disc path generalises `cd_sector_size` to
whatever `read_cd_length` returns (≤ 2744 bytes; at least 47 sectors fit
the 131,076-byte `io_buffer`).

- **PIO**: `cd_read_sector` returns 1 when it filled the sector
  synchronously (the disc path) and 0 when it started an async block
  read; `ide_atapi_cmd_reply_end` loops on 1 instead of recursing (a
  128 KiB transfer would recurse 64 deep). `cd_read_sector_sync` has the
  same branch.
- **DMA**: `ide_atapi_disc_read_dma_cb` mirrors the stock callback,
  fills each chunk synchronously and re-enters through a bottom half
  (`replay_bh_schedule_oneshot_event`), keeping the stack flat and
  completion asynchronous. `aiocb` stays NULL; `ide_atapi_dma_restart`
  is guarded.
- **Errors mid-transfer** end the command with the §3 sense after the
  sectors already sent, as a drive aborts on the failing sector.
- `cmd_read` (READ 10/12) checks `sector_info` over the range first: an
  audio or gap sector is 05/64/00.
- TOC, sub-channel and disc information come from the responders.
- `GET CONFIGURATION` with a disc: the CD-ROM profile with features
  0x001E (CD read, C2) and 0x0103 (CD audio); past 80 minutes the
  DVD-ROM profile current with CD-ROM listed and DVD Read 0x001F (patch
  53). Mode page 0x2A: a 48× CD-DA drive (speed 8467, C2, ISRC, UPC,
  R–W, separate volume and mute, 256 levels, 128 KiB buffer; the DVD
  read bit past 80 minutes); page 0x0E the audio ports. MODE SELECT
  parses page 0x0E only and errors on malformed lengths (05/26/00).
- INQUIRY's product string follows `-device ide-cd,model=…`.
- `ide_cd_change_cb` stops audio and resets the status and the head.

Implementation rules:

- **A new PIO end-transfer function must be listed** in `hw/ide/core.c`'s
  `ide_is_pio_out` and `transfer_end_table`, or QEMU aborts on the first
  data word. A static one in `atapi.c` cannot be, hence the exported
  `ide_atapi_data_out_done` (MODE SELECT's data-out).
- **No speed model.** Page 0x2A claims 48× with a disc (the stock no-disc
  path says 4×), `SET CD SPEED` is a no-op, and data arrives as fast as
  the host reads it (~580 MB/s over a whole disc under TCG). QEMU's
  block throttle (`throttling.bps-read=`) reaches only the `raw` driver:
  `atapi_disc_read_sector` reads from libdisc, not a BlockBackend, so an
  `.iso` throttled to 600 KB/s measures 666 KB/s in the guest and the
  same data as a `.cue` 10,705 KB/s (`THROTTLE=` in
  `tools/cd-rate-guest-test.py`). What the guest reads does not depend on
  the rate: 34 ways of reading one range (PIO and DMA, request sizes,
  byte-count limits, 2048 and 2352, paced 1×/4×/16×) agree with each
  other and with the host's checksum on both drivers. A `speed=`
  property both drivers honour waits for a title that paces itself on
  CD reads.

### 5.4 CD-DA

`DEFINE_AUDIO_PROPERTIES` on `ide-cd` (the card lives in `IDEDrive`).
Without an `audiodev` no voice is opened and a `QEMUTimer` advances the
play position at 75 sectors per second, so polling games still see
tracks complete. With one: `AUD_open_out` at 44100 Hz stereo S16, active
while playing.

`cd_audio_callback` reads audio sectors with `libdisc_read_raw`, routes
them through page 0x0E (port 0 = left: channel mask 1 L, 2 R, 3 mix,
scaled by `vol/255`; port 1 = right), `AUD_write`s them and advances;
at the end the status is 0x13. A data sector inside a play range ends it
with 0x14. **A host read error plays as silence** (patch 55): a
transient failure off a network share used to stop a game's music for
good, where a real drive plays through a bad audio sector as a dropout.

Commands: PLAY AUDIO(10) start LBA + 2-byte length; (12) 4-byte length;
MSF start bytes 3..5, end 6..8, exclusive, `FF:FF:FF` = the disc's end;
TRACK/INDEX through `libdisc_track_info`; PAUSE/RESUME byte 8 bit 0;
STOP → 0x15. A play starting on a data sector is 05/64/00; `start ==
end` completes at once.

**Three commands stop a play, and each family sends only one of them**
(`tools/cdaudio-guest-test.sh`, which runs `CDTEST.EXE` under
`CDIMAGE_TRACE=1`):

- **XP** brackets every play with `PAUSE, SEEK, PAUSE, PLAY` and stops
  with **START STOP UNIT** `1b 00 00 00 00`, never 0x4E.
- **Win98's `mcicda`** sends one PLAY AUDIO MSF and two SEEKs for a whole
  play / pause / stop session — no 0x4E, 0x1B or 0x4B. **On 9x a seek is
  the stop**: MCI seeks to where playback should end and reports
  "stopped" on its own authority, so a drive that treats the seek as
  advisory plays the disc out behind a stopped MCI (11 MB of wav for a
  4-second play). A SEEK therefore ends playback and moves the head
  (patch 54).
- A **data read** does not stop audio, although real drives abandon
  playback for one: Win98's CDFS re-reads the volume descriptors several
  times a second during a play (532 READ(10)s in one session).

READ SUB-CHANNEL's position is `play_lba` while playing, paused or
completed, else the head (`atapi_last_lba`): where a play ended, a seek
was sent, or the last sector read, whichever was latest. With only the
last *read*, XP answered "track 01, 00:00:17" — its own volume
descriptor — after a stop.

The launcher and player attach the drive explicitly, on its own channel
and with the embed audiodev:

```
-drive if=none,id=cd0,media=cdrom[,file=<medium>]
-device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0[,shelf=<file>]
```

`ide1-cd0` is the QMP id a medium change addresses
(`launcher-core/src/control.rs`, forced so a tray the guest locked
still changes).

## 6. Tests

Integration only; the catalogue with how to run each is
`docs/testing.md`.

### 6.1 Host: `discx` (`libdisc/src/bin/discx.rs`)

Built by `cargo build --release -p libdisc`.

- **`discx selftest <dir>`** (the `libdisc` check) writes synthetic
  discs and checks them **through the C API** — the boundary
  `cdimage.c` uses: `mixed.cue/.bin` (a MODE1/2352 track of 2000
  pseudo-random sectors, an audio track with index 00 in the file and a
  1 kHz tone, an audio track with `PREGAP`, `CATALOG`, `ISRC`), the same
  disc as CloneCD with a synthesized `.sub`, a MODE1/2048 cooked copy,
  and `plain.iso`. Cases: `msf`; `raw-synth` (synthesized raw equals
  stored raw, cooked equals cooked); `lec` (an unreadable and a
  repairable sector, cooked and raw, both halves of the raw rule);
  `repair`; `edges` (range ends, EMODE on the wrong type); `subq-synth`
  (synthesized Q equals the `.sub`, CRC on every frame); `toc` (formats
  0–2 identical across the three images, format 0 on the ISO as QEMU
  lays it out); `ccd`; `read-cd-length` (the MMC-3 table);
  `read-cd-fill`; `dirdisc` (§8); `panic-safety` (a corrupt cue is an
  error string, never an abort).
- **`discx info` / `dump <image> <what>`** print the layout and any
  responder's bytes (`toc 0 1`, `subq 1000`, `readcd 16 0 0xf8 1`): the
  oracle `atapi-guest` compares against.
- **`discx scan <image>`** L-EC-checks every sector of a real dump and
  splits the failures the way a guest meets them: read anyway (EDC
  intact), read after the decoder repairs them, or unreadable, listing
  the last. On a protection band every failure must be unreadable.
  **`subscan`** does the same for stored subchannel (Q CRC failures,
  clustering, how often `subq::synthesize` reproduces the disc's
  frames).
- **`discx repair <image> <outdir>`** writes the negative control
  (§2.6b): every L-EC-failing sector's EDC, reserved gap and ECC
  regenerated over the dumped user data (SafeDisc's `0x55` stays), each
  payload and descriptor copied by basename; sync-less sectors (run-out,
  which a drive fails too) left alone. Checked by selftest's `repair`:
  the sector reads, its data is still the corrupted data, no byte
  outside the parity fields moved.
- **`discx convert <in.iso> <out.cue> [--audio tone.wav …]`**: an ISO
  as MODE1/2352 cue/bin with synthesized EDC/ECC and appended audio
  tracks — how guest test discs are made from the guest-tools ISO.
- **`discx export <image> <out.iso>`**: the cooked view written out, how
  a folder disc is checked by readers that are not ours (§8).
- **`discx mktree <dir>`**: the folder-disc fixture tree (§8).

The `cdimage` check has `qemu-img info` report `"format": "cdimage"`
and `sector_count × 2048` on the selftest's cue.

**An independent EDC/ECC oracle.** Neill Corlett's `ecm` (ecm-tools
1.03, public domain; `pacman -S ecm-tools`, or `ecm.c` + `common.h` +
`banner.h` from github.com/alucryd/ecm-tools and `gcc -O2 -o ecm
ecm.c`) strips a sector's EDC/ECC only when its own regeneration
reproduces them, so `ecm mixed.bin x.ecm` reporting `Mode 1
sectors.......... 2000` proves `ecc.rs` byte-exact (1999 on `lec.bin`,
whose flipped sector does not). Its "Mode 2 form 1 sectors … 151" are
the all-zero audio pregap (ecm's false positive on zero blocks). Re-run
it after any change to `ecc.rs`.

### 6.2 Guest, DOS: `tools/atapi-guest-test.py`

The `x87-guest-test.py` harness (NASM `.COM`, FreeDOS floppy, COM1 out,
`-cpu pentium3`). The program drives the secondary channel
(0x170/0x376, `-cdrom` is its master) by PIO PACKET commands and dumps
every reply and every REQUEST SENSE: INQUIRY, READ TOC 0/1/2, READ CD
across track boundaries, READ(10) of the unreadable sector (03/11/05),
the repairable one and their neighbours, READ SUB-CHANNEL 1/2/3, PLAY
AUDIO MSF with the position advancing, PAUSE, both stops (STOP PLAY/SCAN
and START STOP UNIT, each followed by status 0x15), GET CONFIGURATION,
MODE SENSE 0x2A / 0x0E and MODE SELECT 0x0E read back — at byte-count
limits 512 and 65534. Every reply must equal `discx dump` of the same
request. Then the shelf (patch 52). The `atapi-guest` check;
`ATAPI_READ_ERROR=1` is `atapi-read-error` (patch 55).

### 6.3 Guest, Windows

- **Copying a disc through the OS driver**: `tools/xp-cdimage-test.sh`
  boots XP with a converted guest-tools disc (or any `.cue` / `.ccd` /
  `.mds` / `.iso` / `isodir:`), copies it through `cdrom.sys` and
  compares every file (the `guest-cdimage` and `guest-dirdisc` checks).
  Win98 has `tools/dirdisc-guest-test.sh`.
- **`CDTEST.EXE`** (`guest-tools/src/cdtest.c`, `TESTS\` on the ISO):
  MCI `open cdaudio`, the track count, `play cd from 2 to 3`, positions
  for 3 s, `C:\2KSBOX\CDTEST.LOG`. With `CDTEST=`, `xp-cdimage-test.sh`
  records the drive's audiodev to a wav, which must hold the 1 kHz tone,
  and `status cd mode` right after `stop cd` must not say `playing`.
  MCI answers from the state it commanded, so the drive and the wav are
  the verdict, not MCI; `tools/cdaudio-guest-test.sh` adds the trace
  (§5.4).
- **Still wanted** (doc 09): ATAPI traces from the rig's real drive while
  a protected title checks its disc (an XP SPTI logger; a filter driver
  is out of scope), and CloneCD dumps with subchannel for SecuROM.

### 6.x Which dumps carry a protection signal

`discx scan` over the real dumps (outside the repo, in
`/mnt/data2/david/Downloads/oldstuff` on the Linux box):

| Dump | Protection (from the disc's files) | What `discx scan` finds |
|---|---|---|
| `fifa2002/` (DIC and Alcohol sets) | SafeDisc 2.x (`00000001.TMP`, `DRVMGT.DLL`, `SECDRV.SYS`) | 584 sectors from LBA 811, identical in both sets |
| `AOM_D1.ccd` | SafeDisc 2.x | 580 weak sectors, LBA 825–12000: valid sync and header, `0x55` fill, wrong EDC |
| `AOM_D2.ccd` | none | clean |
| `the settlers 3/CD01.ccd` / `.cue` | VOB ProtectCD (`.ficken` in `S3.EXE`) | 697 failures, identical in both |
| `CD02` | none | only its run-out, 234254–234403 |
| The Sims (CCD), `rayman2/` (redump) | SafeDisc 1.x | 0 failures |
| `nfs_porsche/*.mds` | SafeDisc 1.x | 0 failures over 281,279 sectors |
| Crimson Skies (`C_SKIES.cue`) | SafeDisc 1.50.020 | 579 sync-less sectors, LBA 807–10018 (§2.6c) |

Settlers CD01's 697: the 538-sector band at LBA 195539–196076 (corrupt
in the data and in Q's relative timing), nine scattered singles past it
with the EDC wrong too, and 150 sync-less run-out sectors at
219692–219841 before track 02. The protection is on disc 1 only.

What the table teaches:

- **SafeDisc 2.x writes a band; 1.x discs differ.** Crimson Skies has
  one, The Sims, Rayman 2 and Porsche none: `discx scan` is the only way
  to know, and a 1.x title may be an L-EC fixture.
- **DiscImageCreator / redump sets carry the band.** `/sf` replaces a
  bad sector's user data with `0x55` and keeps its header and wrong
  parity ("N unmatch sector is replaced at 0x55 except header" in
  `*.img_EccEdc.txt`), so the sector still fails and still reaches the
  guest as an error.
- **The two FIFA 2002 sets differ outside the band.** DIC logs 64
  sectors it could not descramble (LBA 135084–135086, 161089, 223875,
  224045) that fail as `-EIO` and break an install; the Alcohol set read
  them cleanly. **Run the `.mds`**, and keep the DIC `.cue` as the
  verification fixture: its bad-sector list provably equals the dumper's
  log.
- **Real protection is a band** — hundreds of sectors against hundreds
  of thousands clean. Whole-disc failure is a format problem, most
  likely a scrambled image: `ccd.rs` refuses `DataTracksScrambled=1`,
  but a bare `.scm` from DIC's pipeline carries no flag to catch.

## 7. Milestones

| Step | Delivers | State |
|---|---|---|
| M5a | cue/bin and ISO model, EDC/ECC, Q synthesis, C API, `discx`, the driver, patches 50/51's data path, TOC, READ CD, subchannel | done |
| M5b | CD-DA (play, pause, stop, position, pages 0x0E / 0x2A, `audiodev`), swaps over QMP | done; Win98's CD Player by ear not recorded |
| M5c | CCD + `.sub` replay, the raw TOC verbatim, a SecuROM title | replay done; no SecuROM title tested |
| M5d | SafeDisc: L-EC against a real dump | done (§2.6c): Crimson Skies passes once an unreadable raw read fails; guarded by selftest's `lec` and `atapi-guest` |
| M5e | MDS/MDF (+ DPM), CHD, a timing profile if StarForce needs one | MDS done without DPM; CHD and timing open |
| M5f | the disc shelf in the launcher and the guest (with M6) | done (patch 52, doc 07) |
| M5g | a host folder as a disc (§8) | done |

Open items — re-measuring the protected dumps on the rig since the
EDC-first correction, SecuROM, multisession, CHD, a speed model — are in
`docs/tracks/m5-cdrom-backend.md`.

## 8. Folder discs: `isodir`

`isodir:/path/to/folder` serves a host directory as a read-only
ISO 9660 + Joliet volume, generated lazily inside libdisc: no image file
written, nothing copied, no `xorriso` at run time. It is small because a
`Disc` was never tied to an image file: `isodir.rs` is a layout builder
that emits one `Source::File` extent per host file (`Cooked2048`, with
sync, header and EDC/ECC synthesized as for an `.iso`) and `Source::Mem`
extents for the metadata. `read_cooked` / `read_raw`, the responders,
patch 51, `qemu-img` and the shelf are unchanged, and `libdisc_open` on
a directory is the whole interface (`LIBDISC_API_VERSION` stays 1).

### Decisions

1. **Read-only.** Files leave a guest through a FAT scratch disk, never
   vvfat read-write.
2. **Snapshotted at insert**, like a burned disc. Host edits appear on
   the next eject and insert (the shelf's LOAD, whose UNIT ATTENTION
   makes the guest's CD file system drop its cache); a file changed
   under a mounted disc reads as `EMEDIUM`, never torn (§2.1).
3. **Generated lazily, never written out.** `discx export` writing a
   real ISO is the test oracle, not the mechanism.
4. **Joliet for Windows and ISO 9660 level 1 (8.3) for DOS** in one
   volume, both trees pointing at the same extents. No Rock Ridge: no
   guest reads it.
5. **A protocol prefix, not a probe** (§5.1). Everything that inspects a
   medium string must understand the prefix, not only what passes it
   on: the shelf's C side strips it before `access()`ing the host path
   (`cdshelf_host_path()`), and once listed every folder as `[missing on
   the host]`. Patch 52's LOAD passes the name with no format and the
   block layer resolves the prefix itself.
6. **No QEMU patch**: the driver is in our overlay file `cdimage.c`.
7. **Not bootable.** No El Torito; a boot image named in the folder and
   a catalogue at a fixed LBA would add it without a redesign.

### Layout

One Mode 1 data track, LBA 0 to the lead-out:

| LBA | Contents |
|---|---|
| 0–15 | system area, zero |
| 16 | Primary Volume Descriptor (type 1, `CD001`, version 1) |
| 17 | Joliet Supplementary VD (type 2, escape `%/E`, UCS-2 level 3) |
| 18 | Volume Descriptor Set Terminator (type 255) |
| 19… | L and M path tables, primary then Joliet |
| … | directory records: the primary tree, then the Joliet tree |
| … | file extents, 2048-aligned, one contiguous run per file |
| last | 150 zero sectors (`TAIL_PAD`: guest drivers read ahead past the last extent, and every real ISO has it) |

Rules that are easy to get wrong:

- **Both-endian fields.** A directory record's extent LBA and length are
  8 bytes each, LE then BE; the PVD's path-table pointers are four
  4-byte fields (L, optional L, M, optional M), the optional ones 0.
- **Dates.** Directory records carry 7 binary bytes (year − 1900 … GMT
  offset in 15-minute units) from the file's mtime; the PVD's volume
  dates are the 17-byte digit form.
- **Every directory starts with `.` and `..`** (identifiers `0x00`,
  `0x01`); the root's `..` is the root. A record never straddles a
  sector: pad to the next.
- **Path tables** are ordered by level, parent number, identifier; the
  root is 1 and a parent's number is ≤ its child's. Joliet has its own
  pair of the same shape.
- **Primary names**: level 1, `A–Z 0–9 _`, 8.3, files `;1`,
  directories no extension or version, anything else `_`, collisions
  `~1`, `~2`… decided from a **sorted** listing (otherwise which name
  gets `~1` depends on readdir order and two runs differ). This is the
  tree MSCDEX reads, and why level 2 is not used.
- **Joliet names**: UCS-2 BE, ≤ 64 characters, `* / : ; ? \` and
  controls → `_`, `;1` appended. A name that is not UTF-8 is skipped,
  not mangled.
- **Deterministic order**: directories breadth-first, entries by primary
  identifier, files laid out in the same walk; two opens of an unchanged
  tree are byte-identical.
- **An empty file** has length 0 and owns no sectors, but gets the first
  file extent's LBA rather than 0 (an extent inside the system area is
  the kind of thing a reader drops).
- **Volume space size** = every sector including the system area and
  the tail padding = the lead-out = READ CAPACITY.

### Limits (`isodir.rs` constants)

- **Refused**, with a message naming the path: a file of 4 GiB or more
  (single extent only; multi-extent files are a Windows-version
  minefield), nesting deeper than 30, a symlink loop, and a tree larger
  than **4,173,824 sectors (a dual-layer DVD-9, 8.1 GiB)**. The size is
  measured before anything is laid out, and the message carries both
  sizes; it reaches the shelf's error line through `cdimage.c`'s
  `error_setg`.
- **Warned, then carried on**: deeper than 8 levels (ISO 9660's limit;
  Windows reads the Joliet tree, MSCDEX may not), a file of 2 GiB or
  more (dicey on Win98), more than 65,535 directories. Symlinks are
  followed for files and directories; sockets, FIFOs and devices are
  skipped with a warning.
- **CD or DVD.** Up to an 80-minute CD the medium is a CD; above that
  the drive reports a DVD-ROM (patch 53; `READ DVD STRUCTURE` needed
  nothing), so the CD's MSF ceiling (~878 MiB) no longer bounds a
  folder. Win98 and XP both read marker files at 703 MiB, 878 MiB,
  2 GiB, 4 GiB and 7.8 GiB of one folder disc (`BIG=1
  tools/dirdisc-guest-test.sh`).

**Validate where the disc is built, and make the arithmetic after it
unable to fail.** A 34 GiB folder once got past the builder and
panicked in MSF on the TOC's lead-out, which reached the guest as
`LIBDISC_EIO` on an unrelated command. The size is now refused up front
and `Msf::from_lba` saturates; the `msf` selftest pins both ends of the
clamp.

### Model additions (`libdisc/src/lib.rs`)

- `Source::Mem`: cooked blocks from an in-memory blob — descriptors,
  path tables, directory records; a few hundred KB for tens of thousands
  of files.
- `eof_pad` on `Source::File` (§2.1).
- Lazy payload handles behind the 8-entry MRU cache: eager opens of
  5,000 files pass macOS's default limit of 256 descriptors. Every open
  re-`stat`s (§2.1); the builder still opens each file once, so a
  missing file fails at open time.
- `extent_at` as a binary search: a linear scan is a comparison per file
  per sector read.

### Tests

- `discx selftest`'s `dirdisc` case: a hostile tree (an empty file, 300
  entries in one directory, 9 levels, accents, spaces, `*`, colliding
  8.3 forms, a 64-character name), every refusal, the stale-file
  `EMEDIUM` case, and a byte-identical second open. `discx mktree <dir>`
  writes the fixture tree (plus files of exactly one sector and one
  sector plus a byte, 3 MB of pseudo-random data and an empty
  directory).
- The `dirdisc` check: xorriso extracts the exported volume identical;
  bsdtar with Joliet off reads the 8.3 tree, including which of the two
  colliding names holds which contents; `qemu-img` names `isodir` and
  converts byte-identically to `discx export`; SeaBIOS probing the
  drive under `CDIMAGE_TRACE=1` proves `cdimage_disc()` found the model
  through the `raw` node, against a plain `.iso` as the control.
- `guest-dirdisc` (XP copies a folder through `cdrom.sys`), `dirshelf`
  (the launcher's verbs), `tools/dirdisc-guest-test.sh` for Win98.
- The stale-file rule is checked host-side only: in a guest it would
  race XP's read-ahead. There is no MSCDEX leg: the FreeDOS floppy the
  DOS tools use carries no CD driver, and bsdtar with Joliet off stands
  in for it.

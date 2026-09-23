# 5. CD-ROM backend: raw images and copy protection

The problem the CD-ROM backend solves, the shape of the answer and the
acceptance table, one row per protection scheme. How it is built — the
disc model, the formats, the C API, the MMC byte layouts, the QEMU
driver and patches, CD-DA and the tests — is doc 17; the track record is
`docs/tracks/m5-cdrom-backend.md`, and a host folder as a disc is doc 17
§8.

## Problem

QEMU's CD-ROM emulation is ISO-shaped: one data track of cooked
2048-byte sectors. Everything a 1996–2005 game disc relies on beyond that
is thrown away:

- **Multi-track layouts and CD-DA**: Red Book audio played through ATAPI
  audio commands, the in-game music of a large share of Win9x titles.
- **Raw 2352-byte sectors**: mixed-mode discs, Mode 2 forms.
- **Subchannel data (P–W, especially Q)**, read by SecuROM.
- **Deliberate defects**: a disc's unreadable sectors must fail *at the
  right LBAs*; a drive that reads everything cleanly fails such a check.
- **Raw TOC and session layout**: multisession and protection
  fingerprinting.
- **Data position measurement (DPM)**: StarForce-class checks time
  sector positions; images that carry DPM data (MDS) can satisfy them.

The goal: mount a raw dump of a disc you own and have the *unmodified*
protection code in the guest pass, because the virtual drive is
indistinguishable from a period drive with that disc in it. The DRM runs
and succeeds; nothing is patched, stripped or bypassed, and no-CD or
crack functionality is out of scope.

## Prior art

- **CDEmu / libmirage** (Linux, GPL-2.0+) proves the approach against
  real protection drivers: every relevant format, a disc modelled as
  tracks, sectors and subchannel, and whatever a format lacks generated
  on the fly.
- **86Box and DOSBox-X** implement cue/bin, CD-DA and some raw commands
  in their own drives: a reference for ATAPI behaviour under real Win9x
  drivers.
- QEMU has none of it.

## Design

```
image (cue/bin, ccd/img/sub, mds/mdf, iso; chd later; or a host folder)
  → libdisc: sessions, tracks, indices, raw sectors, subchannel
  → the cdimage block driver (cooked view for the block layer)
  → hw/ide/atapi.c: the MMC command surface answered from the model
```

- **libdisc is Rust** (ADR-004): a crate with a C API, linked into QEMU
  as a staticlib. libmirage is the behavioural reference, not a
  dependency. The MMC reply bytes are built in Rust; the C in `atapi.c`
  moves buffers and drives the IDE state machine.
- **The QEMU side is a format block driver**, so `-cdrom game.cue`
  probes to it and a medium swap stays QMP `blockdev-change-medium`. A
  plain `.iso` stays on QEMU's `raw` driver, byte for byte as before.
- **Protection fidelity comes from modelling the drive**, not from lists
  of bad sectors: every data sector is checked against its EDC/ECC and
  what a drive could not correct fails as it would (doc 17 §2.5, §2.6c).
- **Missing data is synthesized** as hardware would produce it:
  subchannel Q from the TOC when there is no `.sub`, EDC/ECC for a
  cooked image. Protection data is only as good as the dump: SafeDisc
  needs one that recorded the bad sectors, SecuROM one with subchannel,
  StarForce DPM.
- Format priority: cue/bin, CCD (img + sub), MDS/MDF (landed early,
  DPM ignored), then CHD.

### The command surface

Beyond stock QEMU: `READ CD` / `READ CD MSF` for every sector type with
subchannel and C2 error pointers; `READ SUB-CHANNEL`; `READ TOC` formats
0–2 including the raw TOC and multisession; the audio commands with mode
page 0x0E routing, played into QEMU's audio as the drive's analogue
output; the right sense codes for unreadable sectors; `GET
CONFIGURATION` and mode page 0x2A describing a period drive. There is
no seek or read timing model: data arrives as fast as the host reads it
(doc 17 §5.3), and one waits for a check that needs it.

### What the guest sees

- Win98 and XP see an ordinary IDE/ATAPI CD-ROM with their in-box
  drivers; protection drivers (`secdrv.sys` and the like) run
  unmodified.
- Discs are mounted, ejected and swapped at run time from a per-machine
  disc shelf, in the launcher and from inside the guest (`CDSHELF`,
  patch 52; doc 07).
- A **host folder** can go in the drive too (`isodir:/path`, doc 17 §8):
  a read-only ISO 9660 + Joliet volume generated lazily and snapshotted
  when the tray closes, a CD while it fits on one and a DVD-ROM up to a
  dual-layer DVD-9 above that. It hands a guest a pile of files with no
  image to burn and no network stack.

## Acceptance tests (M5 exit criteria)

Dumps of discs we own, one title per scheme. A check only counts once it
has been seen to fail: the `discx repair` copy of a dump (its bad
sectors made good, nothing else changed) is the negative control (doc 17
§2.6b).

| Scheme | Expectation | Result |
|---|---|---|
| Mixed-mode + CD-DA | installs; in-game CD audio plays, tracks and seeks right | **Pass.** Age of Empires Gold and Moto Racer play their soundtracks in-game on XP, in the player, from their `.mds` (user, 2026-09-05) |
| SafeDisc 1.x | launch check passes from a raw dump whose weak sectors fail to read, as on a drive | **Pass**, and the one check seen to fail and then pass on the drive model alone. Crimson Skies (1.50.020) reads its 579-sector band raw and refused the disc until a raw read of an unreadable sector failed with `03/11/05`; then it launches. NFS Porsche Unleashed (1.x, no band) works. Doc 17 §2.6c |
| SafeDisc 2.x | launch check passes from a raw dump with its error sectors | **Inconclusive: the title runs, the check never reads the band.** FIFA 2002 reaches its menus from the dump and from the repaired copy alike; a trace of a passing launch has 0 of 781 reads in the 584-sector band. Doc 17 §2.6b |
| VOB ProtectCD | launch check passes from a dump with the data and Q anomalies | **Inconclusive, the same way.** Settlers 3 plays from `.ccd`, `.cue` and the repaired copy; its launch reads nothing above LBA 191776, below the band, and one READ SUB-CHANNEL. Doc 17 §2.6b |
| SecuROM (early, 4.x) | launch check passes from a dump with subchannel | Not tested. Early needs a `.sub` (replay exists); 4.x needs DPM, which `mds.rs` ignores |
| StarForce (stretch) | a documented result with a DPM-carrying MDS | Not started |
| Multisession | both sessions visible, TOC right | Not tested |
| Multi-disc title | the guest sees the change and the game takes the new disc | **Pass.** Settlers 3's campaign asked for CD2 and took it (user, 2026-09-05) |

Command-level regressions are guarded host-side by `discx selftest`,
whose checks call the same C API QEMU does, and in a guest by
`atapi-guest`, whose every reply must equal `discx dump` (doc 17 §6).
Still wanted: ATAPI traces from the reference rig's real drive while a
protected title checks its disc (doc 09), as fixtures of the command
sequences protections actually send.

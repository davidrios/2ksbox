# Track: M5 — the CD-ROM backend (docs 05 and 17)

This track covers raw optical-drive emulation:

- the `libdisc` Rust crate: the disc model, image formats, EDC/ECC,
  subchannel and the MMC responders;
- the `cdimage` QEMU block driver over libdisc's C API;
- the ATAPI patch that serves TOC, raw sectors, subchannel and CD-DA from
  the model.

The work ran from 2026-09-04 to 2026-09-09 and is all on `main` (the
branch was deleted on 2026-09-06). This record keeps the scope, the test
loop, the traps that are specific to this code, and what stayed open.

The design and every finding live elsewhere:

- **Doc 05:** the problem statement and the acceptance table, one row per
  protection.
- **Doc 17:** the spec, including:
  - the byte layouts and formats;
  - the L-EC rule (EDC decides, parity repairs), §2.5;
  - subchannel synthesis and the undeclared-pregap decision, §2.6;
  - the negative control and what the protections actually read,
    §2.6b–c;
  - which dumps carry a protection signal, §6.x.
- **`docs/tracks/m5-dirdisc.md`:** M5g, a host folder as a disc.
- **Doc 07:** the disc shelf (patch 52, M6).

## Scope and files

- `libdisc/`:
  - `src/`: the model in `lib.rs`; `cue.rs`, `ccd.rs`, `mds.rs`, `iso.rs`,
    `sector.rs`, `ecc.rs`, `subq.rs`, `msf.rs`, `mmc.rs` and `capi.rs`;
  - `src/bin/discx.rs`;
  - the C header `libdisc/libdisc.h` (bump `LIBDISC_API_VERSION` on any
    change);
  - the block driver `libdisc/qemu/cdimage.{c,h}`, which
    `prepare-qemu.sh` overlays into the QEMU tree.
- QEMU patches, with rows in `patches/qemu/README.md`:
  - `50-cdimage-block-driver`: meson and `CONFIG_CDIMAGE`;
  - `51-atapi-disc-model`: `hw/ide/`;
  - `53-atapi-dvd-profile`: M5g;
  - `54-atapi-audio-seek-stop`: Win9x's seek-as-stop, doc 17 §5.4;
  - `55-atapi-audio-read-error`.

  Numbers 56–59 are reserved for this track. Patch 52, the shelf, is
  shared with M6.
- Guest program: `guest-tools/src/cdtest.c` (`CDTEST.EXE`, MCI CD audio).
- Tools:
  - `tools/atapi-guest-test.py`
  - `tools/cd-rate-guest-test.py`
  - `tools/xp-cdimage-test.sh`
  - `tools/cdaudio-guest-test.sh`
  - `tools/cdshelf-guest-test.sh`
  - `tools/read-error-inject.c`
- Shared: `scripts/prepare-qemu.sh` and `configure-qemu.sh` (the overlay
  and `-Dlibdisc_dir`), `scripts/test.sh`, and `player/`'s `-drive` /
  `ide-cd,audiodev=` lines.

## Test loop

```sh
scripts/build-libdisc.sh                    # cargo + relink QEMU (see traps)
target/release/discx selftest build/test/disc
scripts/test.sh all                         # before every commit
```

Checks in `scripts/test.sh`:

| Check | What it runs |
|---|---|
| `libdisc` | `discx selftest`, through the C API |
| `cdimage` | `qemu-img` / `qemu-io` on the selftest images |
| `atapi-guest` | a DOS program driving the drive by PIO: every reply equals `discx dump`, a sector that is unreadable and one that is repairable, audio, the shelf |
| `atapi-read-error` | unreadable audio sectors play as silence (Linux) |
| `guest-cdimage` | XP copies a converted disc through cdrom.sys; `CDTEST.EXE` plays the tone into the drive's wav |

Local-only tools (details in `docs/testing.md`):

- `tools/cd-rate-guest-test.py`: asks whether what is read depends on the
  read rate; it does not;
- `tools/cdaudio-guest-test.sh`: how each Windows family stops a drive;
- `tools/cdshelf-guest-test.sh`.

`discx scan`, `subscan` and `repair` diagnose a real dump (doc 17 §6.1).
`CDIMAGE_TRACE=1` prints every packet, reply and sense of the disc path.
`LIBDISC_NO_CORRECT=1` turns off L-EC correction for the A/B test.

The real dumps are not in the repo. On the Linux box they are in
`/mnt/data2/david/Downloads/oldstuff`, and the `discx repair` control
copies are under its `clean/`.

## Traps

- **meson does not track `liblibdisc.a`.** After a change under `libdisc/`,
  a plain `ninja` keeps the old code in QEMU, `qemu-img` and the embed
  library. `scripts/build-libdisc.sh` relinks them, and `scripts/build.sh`
  does the same.
- **Register new PIO end-transfer functions.** A new one must be added to
  `hw/ide/core.c`'s `ide_is_pio_out` / `transfer_end_table`. If it is not,
  QEMU aborts on the first data word.
- **A plain `.iso` must keep probing to `raw`.** The XP guest stage and
  every recorded number were taken on that path. The new IDEState fields
  have no vmstate, so snapshots with a `cdimage` medium are unsupported.
- **Watch a check fail before believing it passes.** Three protections
  "passed" before anyone saw one refuse a disc. The clue was that the
  `discx repair` copies passed too. The trace then showed that SafeDisc 2
  and ProtectCD never read their band (doc 17 §2.6b). Only Crimson Skies'
  SafeDisc 1.50.020 does (§2.6c).
- **Put a protected title's disc on its own IDE channel** (`ide.1`, as the
  player does). SafeDisc 2 refused FIFA 2002 when the CD was the boot
  disk's slave.
- **Attribute a sense reply to its device before calling it a bug.**
  QEMU's default *empty* CD drive answers `GET CONFIGURATION` with errors
  that look like ours.
- **Run FIFA 2002 from its `.mds`, not the DIC `.cue`.** The DIC dump has
  64 sectors outside the band that it could not descramble, and they break
  the install (doc 17 §6.x).
- **A DOS program doing bus-master DMA must set PCI_COMMAND_MASTER.**
  Without it the engine reports a clean finish and writes nothing.

## What stayed open

- **Re-measure the protected dumps on the rig.** Since the EDC-first
  correction (doc 17 §2.5), check that every L-EC failure on FIFA 2002, AoM
  disc 1 and Settlers 3 CD01 lands in `discx scan`'s *unreadable* column,
  and that FIFA 2002 still installs and reaches its menus.
- **Protections:**
  - FIFA 2002 never reached a match; the suspect is the display path, not
    the disc.
  - Age of Mythology has not been run as a second SafeDisc 2.x title.
  - SecuROM needs DPM in `mds.rs` (M5e) and an owned dump.
  - Multisession has not been done.
- **CHD (M5e).** v5 with `cdlz`/`cdzl` hunks, which needs zlib, LZMA and
  FLAC. The choice between a pure-Rust decode and `chdman` at import time
  is still open.
- **Speed model.** The drive delivers as fast as the host reads, and QEMU's
  block throttle reaches only the raw driver. Testing whether a title that
  paces itself on CD reads minds this needs a `speed=` property on
  `ide-cd` that both drivers honour.
- **Not recorded:**
  - Win98's CD Player playing track 2 by ear in the player;
  - a medium swap while audio plays.

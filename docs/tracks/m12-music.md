# Track: M12 — music (doc 20)

The handoff for a session working on what a guest plays *music* on: the
OPL3 and MPU-401 devices, the three engines behind them (`libsynth/`),
and the two pickers that put them on a machine. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then doc 20 (the design), then doc 06 §"Audio" for what
each family's hardware is supposed to be.

On `main` — no branch. The work touches no file another live track owns
(M4 and M7 are display and Direct3D; M10 is the 9x display driver), and
its QEMU patch number (25) sits in the gap between the CPU patches and
the 3dfx ones.

## Scope and files (this track owns them)

- `libsynth/` — the crate: `src/opl.rs`, `src/gm.rs`, `src/mt32.rs`,
  `src/midi.rs` (the byte-stream state machine), `src/capi.rs`,
  `libsynth.h`, and `src/bin/synthx.rs` (the exerciser and the
  `libsynth` check).
- `libsynth/qemu/` — `opl3.c` and `mpu401.c`, overlaid into `hw/audio/`
  by `scripts/prepare-qemu.sh`.
- `patches/qemu/25-opl3-mpu401-devices.patch` — the meson/Kconfig wiring
  and `-Dlibsynth_dir`, mirroring patch 50's for libdisc.
- `soundfonts/` — the shipped General MIDI bank and its provenance.
- In `launcher-core/src/bundle.rs`: `Sound`, `Music`, their `*_choices`,
  the defaults and the arguments. The machine form's two rows in
  `wizard.rs` and both front ends' views of them.
- `scripts/test.sh`: the `libsynth` and `music` checks.
- `tools/midi-guest-test.py` — the guest end-to-end.

## State

**Stages 1–4 landed 2026-09-09; a machine has music.**

- **The engines.** `libsynth` builds as an rlib and a staticlib;
  `synthx selftest` passes the OPL3 detection sequence, an FM note
  measured at 440 Hz, the shipped bank through the MIDI byte-stream
  path, and a running-status note-off with a real-time byte inside the
  note-on. The MT-32 case SKIPs without ROMs — it has never been run
  here, which is the one engine still unproven (see below).
- **The devices**, behind **patch 60** (not 25: a number below 50 would
  have had to fight patch 50's `meson.build` hunks for context).
  `-device opl3[,sbbase=]` and `-device mpu401,synth=gm|mt32`, both
  overlaid from `libsynth/qemu/`, both accepted by our own
  `qemu-system-i386`, and both *sounding* into a wav.
- **The pickers.** `bundle::Sound` / `bundle::Music` with doc 20 §6's
  per-family choices and defaults, in `launcher-core` and drawn by both
  front ends, the C API (`lc_wizard_sound_*`, `lc_wizard_music_*`) and
  `launcherx --music`. The `music` check covers all of it.
- **Packaging.** `soundfonts/TimGM6mb.sf2` into all four packages, found
  through `LIBSYNTH_SF2` by `player/src/companions.rs`; the Linux
  packager asks the staged player where the bank is, as it does for the
  Glide wrapper. The three new crates are in the Flatpak's
  `cargo-sources.json` — added by hand from `Cargo.lock`'s checksums
  (verified against the downloaded `.crate` files), because
  `scripts/gen-flatpak-cargo-sources.sh` does not run on macOS
  (`sha256sum` usage differs); a Linux session should re-run the
  generator and confirm it produces the same three entries.

- **The guest end-to-end** (`tools/midi-guest-test.py`, the `midi-guest`
  check): a DOS program under TCG runs the AdLib detection sequence
  (`OPL status 00 c0 00`), plays 440 Hz on the OPL3, then resets an
  MPU-401 (`fe fe` — both ACKs), puts it in UART mode and plays A4; both
  notes are in the wav QEMU recorded. 11 s for the pair.

**The MT-32 is unverified, by decision (2026-09-09).** No one here has
CM-32L ROMs and the user is not going to get any, so `mt32-tone` will go
on SKIPping and the engine's *sound* has never been heard. What is
proved is everything around it: the option, the form's refusal without a
ROM directory, the size-based ROM finder and its message for an original
MT-32's half-size PCM ROM. `moont` claims sample accuracy against Munt
and is taken at its word until someone with a dump runs
`synthx selftest --roms <dir>` — which is a minute's work and is the
first thing to do if a title sounds wrong on it. Do **not** treat this
as a task waiting to be done here.

- **A real game, 2026-09-09** (`tools/duke-guest-test.py`): Duke Nukem
  3D (Atomic Edition, the user's own disc) plays its score through our
  MPU-401 — 300-450 note-ons per 5 s across 5 to 8 MIDI channels — from
  a run that starts with nothing: the DOS build is copied off the disc,
  a FAT disk is made, the game's own SETUP.EXE is driven for a config,
  and the disc goes back in the drive because the game checks for it.
  The OPL3 plays the same game's music too (2466 register writes, 516
  key-ons in 5 s), but only when SETUP launches the game itself; from a
  batch file the game says "Couldn't find selected sound card" whatever
  the config says, which is the game's own business — the same OPL3
  answers the `midi-guest` battery and the `music` check from a standing
  start. Both devices now print what the guest is doing to them every
  5 s, which is what made all of this diagnosable.
- **The MIDI port has no interrupt line** (2026-09-09, doc 20 §5.1). It
  shipped on the hardware's own IRQ 2/9 and that is where QEMU's PIIX4
  puts the ACPI SCI, so on an ACPI Win98 — every machine the launcher
  installs — the ACK a driver's reset queues was an interrupt no handler
  could acknowledge. The line is held until the guest reads the data
  port, exactly as the hardware holds it, so the handler was re-entered
  on every `IRET`: 234 nested `INT 0x59`, the ring-0 stack walked off
  its end, #PF → #DF → triple fault, and QEMU reset the machine —
  Windows rebooting in front of the user. Found from a user report and
  reproduced headless on the user's own `win98-2` machine with Duke's
  own `SETUP.EXE` (Choose Music Card → General Midi → 0x330 → **Test
  Music Card**), A/B'd against `irq=255`, which plays the theme song
  instead. `duke-guest` could never have caught it: DOS leaves IRQ 9
  masked. The `music` check now writes the reset from the monitor and
  requires the slave PIC to have nothing pending. Fixing it also
  uncovered that `libsynth/qemu` was missing from `scripts/build.sh`'s
  `qemu-prepare` stamp, so edits to either device were silently not
  rebuilt.

## Next steps

1. **Win98 in front of it.** Whether "MPU-401 Compatible" from Add New
   Hardware really drives the port. (The other half of this item —
   whether the default IRQ 9 collides with the ACPI SCI — was answered
   on 2026-09-09: it does, catastrophically. See State above; the device
   now has no interrupt line at all.)
2. **Duke's FM entry from a batch file**, if anyone cares: `MusicDevice
   = 2` with `MidiPort = 0x388` and `BLASTER` exported still gets
   "Couldn't find selected sound card" unless SETUP.EXE starts the game.
   A curiosity, not a blocker — and possibly QEMU's `sb16` rather than
   our OPL3, since that game's *Sound Blaster* music entry refuses on
   every path tried.
3. **The host MIDI port** (doc 20 §8.1), the first thing deliberately
   outside these stages.

## Rules

- The engines never learn about QEMU and the devices never learn about
  MIDI: the parser is `libsynth`'s, the ports are the devices'. A change
  that needs both is a change to the C API, and that bumps
  `LIBSYNTH_API_VERSION`.
- `soundfonts/TimGM6mb.sf2` is a *default*, not a fixture: the
  `libsynth` check plays the shipped file on purpose, so a package that
  ships a broken one fails a check rather than a user's evening.
- Nothing of Roland's is ever added to this repository.

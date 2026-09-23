# Track: M12 — music (doc 20)

What a guest plays *music* on: the OPL3 and MPU-401 devices, the three
engines behind them (`libsynth/`), and the machine form's sound-card and
music pickers. The design is doc 20; what each family's audio hardware
is, doc 06's Audio rows. Read `docs/00-status.md` first for the global
picture and the track rules. Work happens on `main`.

## State

Every planned stage has landed; what is left is optional (doc 20 §8).

- **The engines**: `nuked-opl3`, `rustysynth` (General MIDI from a
  SoundFont) and `moont` (a CM-32L), pure Rust, as an rlib and a
  staticlib linked into QEMU the way `libdisc` is (doc 20 §1–§3).
- **The devices**: `-device opl3[,sbbase=]` and
  `-device mpu401,synth=gm|mt32` (patch 60), sounding into a wav. The
  MPU-401 has **no interrupt line** unless `irq=` asks for one: its
  hardware IRQ 9 is PIIX4's ACPI SCI, and a queued ACK there rebooted
  Win98 (doc 20 §5.1).
- **The pickers**: `bundle::Sound` / `bundle::Music` with per-family
  choices and defaults (doc 20 §6), in `launcher-core`, the Qt form, the
  C API (`lc_wizard_sound_*`, `lc_wizard_music_*`) and `launcherx
  --music`.
- **Packaging**: `soundfonts/TimGM6mb.sf2` in every package, found
  through `LIBSYNTH_SF2` by `player/src/companions.rs`.
- **Beside it, in QEMU's own SB16**: the interrupt line a driver can
  acknowledge (patch 25, doc 20 §5.2), the mixer volumes applied (patch
  61), and wave-device names DirectX 9 can take (`SETUP /I 5`, doc 20
  §5.3).
- **In guests**: Duke Nukem 3D plays its score through the MPU-401 and
  the OPL3 (`tools/duke-guest-test.py`); Win98 plays MIDI through the
  port once "MPU-401 Compatible" is added from Add New Hardware (doc 20
  §6).
- **The MT-32 is unverified, by decision (2026-09-09)**: nobody here has
  CM-32L ROMs and the user will not get any. Everything around the
  engine is proved (the option, the form's refusal without a ROM
  directory, the ROM finder and its message for an original MT-32's
  half-size PCM ROM); `moont` is taken at its word. Someone with a dump
  runs `synthx selftest --roms <dir>` — the first thing to do if a title
  sounds wrong on it. Not a task waiting to be done here.

## Scope and files

- `libsynth/`: `src/opl.rs`, `src/gm.rs`, `src/mt32.rs`, `src/midi.rs`
  (the byte-stream parser), `src/capi.rs`, `libsynth.h`, and
  `src/bin/synthx.rs`.
- `libsynth/qemu/opl3.c` and `mpu401.c`, overlaid into `hw/audio/` by
  `scripts/prepare-qemu.sh`; `patches/qemu/60-opl3-mpu401-devices.patch`
  wires them into meson/Kconfig with `-Dlibsynth_dir`.
- `soundfonts/` — the shipped bank and its provenance.
- `launcher-core/src/bundle.rs` (`Sound`, `Music`, their `*_choices`,
  defaults and arguments) and the form's two rows in `wizard.rs`.
- Checks: `libsynth`, `music`, `sb-mixer`, `sb16-irq` (host stage) and
  `midi-guest` (guest stage) in `scripts/test.sh`;
  `tools/midi-guest-test.py`, `tools/duke-guest-test.py`.

## Build and test loop

```sh
cargo build --release -p libsynth          # synthx; configure-qemu.sh links the staticlib into QEMU
target/release/synthx selftest build/synth # the engines alone (the libsynth check)
scripts/test.sh host                       # libsynth, music, sb-mixer, sb16-irq
tools/midi-guest-test.py                   # both devices from a DOS guest, judged by QEMU's wav
tools/duke-guest-test.py                   # a real game, off the user's own disc (local only)
```

Music that starts right and then goes wrong: capture what the guest
wrote with `LIBSYNTH_MIDI_LOG=<file>` / `LIBSYNTH_OPL_LOG=<file>` in the
player's environment, then `synthx midilog`, `opllog` and `play` with no
guest (doc 20 §7.2). Both devices print what the guest did to them every
5 s in the QEMU log (§7.1). Every tool: `docs/testing.md`.

## Rules

- The engines never learn about QEMU and the devices never learn about
  MIDI: the parser is `libsynth`'s, the ports are the devices'. A change
  that needs both changes the C API and bumps `LIBSYNTH_API_VERSION`.
- `soundfonts/TimGM6mb.sf2` is a default, not a fixture: the `libsynth`
  check plays the shipped file on purpose, so a package with a broken
  bank fails a check.
- Nothing of Roland's is ever added to this repository.

## Next steps

1. **A host MIDI port** (doc 20 §8.1), the first thing deliberately
   outside the stages: the stream out to real hardware or the host's own
   synth, player-side, with an embed API bump.
2. **Duke's FM music from a batch file**, if anyone cares: with
   `MusicDevice = 2` it says "Couldn't find selected sound card" unless
   its own `SETUP.EXE` starts the game. A curiosity, possibly QEMU's
   `sb16` rather than our OPL3.

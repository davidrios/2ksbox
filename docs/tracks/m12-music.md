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

- **Stage 1 done (2026-09-09): the engines.** `libsynth` builds as an
  rlib and a staticlib; `synthx selftest` passes the OPL3 detection
  sequence, an FM note measured at 440 Hz, the shipped bank through the
  MIDI byte-stream path, and a running-status note-off with a real-time
  byte inside the note-on. MT-32 SKIPs without ROMs.
- Stages 2–5 (the devices and the patch; the pickers; packaging; the
  guest test) are the ordered next steps below.

## Next steps

1. **The devices.** `libsynth/qemu/opl3.c` and `mpu401.c`, patch 25, the
   `-Dlibsynth_dir` build wiring in `scripts/configure-qemu.sh`, the
   overlay in `prepare-qemu.sh`. Done when our `qemu-system-i386`
   accepts `-device opl3,audiodev=…` and `-device mpu401,synth=gm,…`
   and a `-audiodev wav` run has sound in the file.
2. **The pickers.** `bundle::Sound` / `bundle::Music`, the per-family
   choices and defaults of doc 20 §6, the arguments, the wizard rows in
   both front ends, the `music` check in `scripts/test.sh`, and the
   `capi` smoke's expectations.
3. **Packaging.** The bank into `share/2ksbox/soundfonts/`, the
   `LIBSYNTH_SF2` fallback in `player/src/companions.rs` (the
   `QEMU_GLIDE_LIB` pattern), and the four packagers' checks.
4. **The guest end-to-end.** `tools/midi-guest-test.py`: a DOS program
   playing an AdLib note and an MPU-401 melody, checked in the wav.
5. **The host MIDI port** (doc 20 §8.1), which is the first thing that
   is deliberately outside the stages above.

## Rules

- The engines never learn about QEMU and the devices never learn about
  MIDI: the parser is `libsynth`'s, the ports are the devices'. A change
  that needs both is a change to the C API, and that bumps
  `LIBSYNTH_API_VERSION`.
- `soundfonts/TimGM6mb.sf2` is a *default*, not a fixture: the
  `libsynth` check plays the shipped file on purpose, so a package that
  ships a broken one fails a check rather than a user's evening.
- Nothing of Roland's is ever added to this repository.

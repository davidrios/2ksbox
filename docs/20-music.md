# 20. Music: FM, General MIDI and the MT-32 (M12)

A guest here has had *sound* since M0 — SB16 or AC'97 digital audio into
the player's ring (patch 20) — and no **music**. The two are not the same
thing on a machine of this era and the difference is not a detail:

- A DOS game's sound effects are digitised samples through the Sound
  Blaster's DMA channel. Its *music* is a score the game plays on a
  synthesizer: FM registers on the AdLib/SB16's OPL chip, or MIDI
  messages out of an MPU-401 port to whatever module is plugged into it,
  or GF1 voices on a Gravis Ultrasound.
- QEMU gives us the first and none of the second. `sb16.c` has no OPL at
  all (a real SB16 carries a YMF262 at 0x388 — QEMU's card does not), the
  separate `adlib` device is an OPL**2** on the 1990s MAME core, and
  **there is no MPU-401 device in QEMU**. So a game that finds a Sound
  Blaster and asks it for music gets silence, and a game offering
  "Roland MT-32" in its setup has nothing to talk to.

This track gives a machine the music hardware to go with its sound card,
and the synthesizers behind it:

| The guest sees | Played by | Needs |
|---|---|---|
| an OPL3 at 0x388 (and at the SB base) | Nuked-OPL3, the cycle-accurate YMF262 | nothing |
| an MPU-401 UART at 0x330 | a SoundFont General MIDI synthesizer | a `.sf2` bank (one ships) |
| the same MPU-401 | a Roland CM-32L (the MT-32 family) | the user's own Roland ROMs |
| a Gravis Ultrasound at 0x240 | QEMU's own `gus`, which was always there | the guest's GUS drivers |

## 1. Shape

```
guest port writes ─► hw/audio/opl3.c   ─┐
                                        ├─► libsynth (Rust staticlib) ─► AUD voice ─► QEMU mixer ─► embed audiodev ─► player
guest MIDI bytes ─► hw/audio/mpu401.c ─┘                                                   ▲
                                                                     sb16 / AC97 / gus ────┘
```

**The engines live inside QEMU, not in the player.** That is the one
structural decision in this doc and it follows the `libdisc` precedent
(doc 17 §5.2, patch 50): a Rust staticlib with a C API, linked into
`qemu-system-i386` and `libqemu-embed-i386` alike, driven by a device
model. The alternative — MIDI bytes out through the embed API and a
synthesizer in the player beside the audio output — was rejected for
three reasons:

1. **One clock.** Music and sound effects have to stay in step with each
   other and with the guest. Rendered in a device, they go through
   QEMU's own mixer against the same virtual clock as the SB16's DMA,
   and the cushion the `embed` audiodev keeps (patch 20) covers all of
   it at once. Rendered in the player, music would have a second clock
   and a second buffer, and every stall would slide one against the
   other.
2. **It is testable headless.** `-audiodev wav` captures what the guest
   plays, so a scripted run can *hear* a DOS program's music without a
   host audio device — the same way `tools/cdaudio-guest-test.sh`
   already checks CD-DA by looking at the wav. A player-side synthesizer
   would be invisible to every test we have.
3. **It costs the packages nothing.** All three engines are pure Rust
   (`nuked-opl3`, `rustysynth`, `moont`) with no system library behind
   them, so linking them adds no `.so` to the closure the macOS app and
   the Flatpak have to carry — which a FluidSynth (glib) or a munt (C++,
   CMake) would have.

A **host MIDI port** — sending the stream to a real MT-32, an SC-55 or
the operating system's own synth — is deliberately *not* here. It is a
host device, so it belongs on the player's side of the embed API next to
the audio output, and it is the one thing on this list that a headless
test cannot check. §8 has the plan.

## 2. The engines, and why these

| Engine | Crate | Licence | Why |
|---|---|---|---|
| OPL3 | `nuked-opl3` | LGPL-2.1+ | The Nuked core is a register-level model of the YMF262 taken from a die scan; it is what every serious DOS emulator moved to. Its `Opl3Device` layer carries the two timers and the status register, which is the half QEMU's `adlib` lacks and every AdLib *detection* routine reads. |
| General MIDI | `rustysynth` | MIT | A SoundFont 2 synthesizer in pure Rust: renders at any rate, allocates nothing on the render path after the first block. |
| MT-32 / CM-32L | `moont` | LGPL-2.1+ | A Rust port of Munt that states, and tests, sample accuracy against it. The alternative was Munt itself as a C++ submodule with a build stage per platform. |

All three are GPL-2.0 compatible, which the workspace licence
(`GPL-2.0-only`, ADR-009) requires.

## 3. The C API

`libsynth/libsynth.h` is the one header, shared by the crate
(`libsynth/src/capi.rs`) and the two devices, exactly as `libdisc.h` is
shared by the block driver and `atapi.c`. `LIBSYNTH_API_VERSION` is
checked at realize time by both devices — a stale staticlib is otherwise
a guest that plays nothing and says nothing.

Two handle types, both **unlocked by design**: QEMU drives them with the
BQL held from both sides (port writes on the vCPU thread, render from the
audio timer in the main loop), and that is the serialization. Nothing in
the API blocks or allocates on the render path, and every entry point
catches a panic — a synthesizer that falls over costs the guest its
music, never QEMU its process.

The MIDI half takes the **byte stream, one byte at a time, exactly as the
guest wrote it**: running status, System Exclusive split across as many
writes as the game likes, and real-time bytes dropped in the middle of
either. `libsynth/src/midi.rs` is that state machine, and it is shared by
both synthesizers — the MPU-401 device knows nothing about MIDI beyond
"a byte arrived".

## 4. The bank, and the ROMs

**A General MIDI synthesizer is a bank of recorded instruments**, and
which bank it is decides how the music sounds far more than the engine
does. One ships: `soundfonts/TimGM6mb.sf2`, 5.7 MB, GPL-2, the bank
MuseScore 1.x shipped and Debian packages. `soundfonts/README.md` has
the provenance, the hash and why this one rather than the larger and
better-sounding GeneralUser GS (whose author does not vouch for where
every sample came from — a fine thing to point a user at, a poor thing
to put inside a GPL package). Any `.sf2` can be picked instead, and the
machine form says so.

**Where the bank comes from is not the bundle's business.** A machine
that simply says `synth=gm` is the normal case, and the device finds the
file the way QEMU finds every other companion of ours (the Glide
wrapper's search, patch 33): the `soundfont=` property, then
`LIBSYNTH_SF2` — which a packaged player sets to its own copy
(`player/src/companions.rs`) — then `soundfonts/TimGM6mb.sf2` in a
checkout. So the same machine file works in a checkout, in a package and
on someone else's install, and a bundle only ever names a bank the user
chose themselves.

**The MT-32 ROMs are the user's own.** An MT-32 is a sampler: the LA
synthesis engine is emulated, but the *sounds* are two Roland ROM chips
— a 64 KiB control ROM (the firmware, the timbre and parameter tables)
and a 1 MiB PCM ROM (the waveforms) — and neither is redistributable.
Nothing here carries them. The device is pointed at a *directory* and
the two images are found **by size rather than by name**, because every
dump in circulation names them differently (`CM32L_CONTROL.ROM`,
`cm32l_ctrl.rom`, `ctrl_cm32l_1_02.rom`…). Without them the option is
offered but refuses to start the machine, with a sentence saying what is
missing — never a machine that boots and is silent.

**It is a CM-32L, and that decides which dump works.** `moont` emulates
the CM-32L: the MT-32's superset, with the 33 extra PCM samples the
later machines added, and what a CM-64 or an LAPC-I has inside it. A
game written for an MT-32 plays on it — that is what the hardware was
for — but the ROMs are not interchangeable: an *original* MT-32's PCM
ROM is 512 KiB, half the size, and is refused with a sentence that says
so rather than "nothing found". Someone who has only MT-32 dumps needs
CM-32L ones, or the engine would have to become Munt itself (doc 20 §2
took that trade deliberately).

## 5. The devices

Both are ours, overlaid from `libsynth/qemu/` into `hw/audio/` by
`scripts/prepare-qemu.sh`, and instantiated by patch 25 like every other
device we add.

### `-device opl3`

An ISA device with the YMF262's register pair at **0x388–0x38B** and, when
`sbbase=` says so, the mirror a Sound Blaster puts at **2x0–2x3** — which
is where an SB-aware game looks, and the reason this is not simply
QEMU's `adlib` with a better core. Address and data are the chip's two
register files (bank 0 is the OPL2-compatible one). Reads return the
status register, which is where the two timers show up: the detection
sequence every AdLib driver runs is *write timer 1, wait, read the flag
back*, and a chip whose timers do not tick is a chip no game finds. The
voice is opened at the chip's own 49716 Hz and QEMU's mixer does the one
conversion there is.

### `-device mpu401`

An ISA device at **0x330–0x331**: data port, and a status/command port
whose two flags are "ready to take a byte" and "a byte is waiting".
UART mode (command `0x3F`) is what everything of the era uses — DOS
games, and Windows 9x's own MPU-401 driver — and the reset command
(`0xFF`) answers `0xFE` the way the hardware does, which is how a game
decides the port is there at all. There is no MIDI *in*: nothing here
generates data for the guest to read, so the status register never says
one is waiting and the IRQ is never raised. Its `synth=` property picks
the engine, `soundfont=` / `romdir=` feeds it, and `gain=` trims it
against the sound card in the same mixer.

## 6. What a machine offers

Two pickers in the machine form (doc 07), both `launcher-core`'s
(ADR-014), both following the display-adapter picker's rule: a family
offers what it has a real question about, and the **first entry is its
default**.

**Sound card** — the digital audio device, `bundle::Sound`:

| Family | Offers | Default |
|---|---|---|
| Win98 | SB16 (with the OPL3), AC'97, Gravis Ultrasound, none | SB16 — Windows has the driver in the box, and a DOS box inside 98 finds the card it expects |
| DOS | SB16, Gravis Ultrasound, AdLib only, none | SB16 |
| XP | AC'97, SB16, none | AC'97 — unchanged from what every XP machine already has |
| Other | ES1370, AC'97, none | ES1370 — unchanged (doc 06: the card BeOS and a period Linux both drive in the box) |

**Music** — what is behind the MPU-401 port, `bundle::Music`:

| Entry | What it is |
|---|---|
| General MIDI (SoundFont) | the shipped bank, or the user's own |
| Roland MT-32 / CM-32L | the user's ROMs |
| None | no MPU-401 device at all — not a port that swallows notes, which is worse than no port: a game would pick it and play to nobody |

**Win98 and DOS start on General MIDI; XP and `Other` start on None.**
The first two have no synthesizer of their own — 98's MIDI output is the
FM chip and a DOS machine has nothing else at all — so the port is what
makes their music sound like music. XP ships a wavetable synthesizer
with the operating system, and `Other` is the family we add no drivers
to, so a port neither would use by default is hardware for nothing; both
offer it one pick away, which is how an old game gets a real MT-32 under
XP.

The FM chip is **not** in that picker: it comes with the card that had
one, exactly as the hardware did. Picking SB16 or AdLib puts an OPL3 on
the machine; picking AC'97 or the ES1370 does not. A game therefore
finds both an FM chip and a MIDI port on a period DOS machine and picks
whichever its setup program offers, which is the point.

**Changing either is a hardware change** to an installed guest, and the
form says so in the same orange as the display adapter: a Windows guest
re-detects a sound card that moved or changed, and a DOS game's setup
has to be run again.

The guest side is the user's, and doc 06 says what it needs: a DOS box
wants `BLASTER=A220 I5 D1 H5 P330 T6` for the MPU-401 to be found, and
an `ULTRASND` line matching what the machine gives the card (QEMU's `gus`
defaults are port 0x240, IRQ 7, DMA 3) plus Gravis's own drivers; Windows 98
finds the SB16 and the AC'97 itself but wants "MPU-401 Compatible" added
by hand from Add New Hardware before its MIDI output goes anywhere.

## 7. Tests

Integration and end-to-end only, as the policy requires.

| Check | What it proves |
|---|---|
| `libsynth` (`synthx selftest`) | the three engines through the **C API the devices use**: the AdLib detection sequence (status 0x00 → 0xC0 → 0x00 across a timer), a 440 Hz FM note measured by Goertzel against its neighbours, the same note through the **shipped bank** (so a truncated or unreadable bank in a package fails here), a running-status note-off with a real-time byte wedged inside the note-on, and the CM-32L when ROMs are given |
| `music` (`scripts/test.sh`) | the two pickers from a checkbox to a real QEMU: each family offers what doc 06 says, the first entry is what a new machine gets, an entry a family does not offer is refused rather than written, the FM chip follows the card, and our own `qemu-system-i386` accepts every combination |
| `midi-guest` (`tools/midi-guest-test.py`) | the whole chain with a guest in it: a DOS program runs the AdLib detection sequence at the ports, plays 440 Hz on the OPL3, then resets an MPU-401, puts it in UART mode and plays A4 through it — and the **wav QEMU recorded** is what is checked, not the program's own opinion. Two boots, one per device: both are asked the same question and one file with two notes in it cannot answer it twice. ~11 s in the guest stage |

## 8. Not here (and the order to add it)

1. **A host MIDI port** — the stream out to real hardware or the host's
   own synth (CoreMIDI / ALSA / WinMM through `midir`). Player-side, an
   embed API bump for the byte sink, and a port list in the machine
   form. This is the "real MIDI" case for someone who owns a module.
2. **MIDI in.** Nothing of the era needs it and it is what the IRQ and
   the status register's second flag exist for; the device is written so
   that adding it is a queue and a `qemu_irq`.
3. **A wavetable header on the sound card** (the SB16's daughterboard
   connector) rather than a separate MPU-401. Same engines, different
   port — worth it only if a game turns up that insists.
4. **The MT-32 through moont's GM mapping** (`GmDevice`), for General
   MIDI music played on MT-32 timbres, which is what a few 1994 titles
   assume.

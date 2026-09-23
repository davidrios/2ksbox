# Track: M13 — gamepads

A real gamepad in a guest, three ways: a USB HID pad (path A), a
gameport at 0x201 (path B, the only one that reaches DOS), or the pad
mapped onto keys (path C, every guest). The track is done: all three
paths are built, both guest devices have been driven by a real
PlayStation 5 DualSense (XP and Windows 98 SE's Game Controllers panel;
`PADTEST.COM` in a Win98 DOS box), and each device is under a check in
the suite's guest stage. There is no separate design doc, so this file
holds the design. Where the neighbours are: doc 03 §"Input path" for
the grab model (a pad sits beside it and never touches it), doc 07 for
the machine form's row, `docs/development.md` §"Gamepads" for the player's
flags and `PLAYER_PAD_SCRIPT`, doc 06 for the per-family defaults,
`patches/qemu/README.md` for patches 26 and 27.

## Scope and files

- `gamepad/` — the abstract pad, a crate shared by the player and
  `launcher-core` (as `shader-chain` is): `Control` (20 controls, face
  buttons named by *position* so a binding does not move between an Xbox
  pad and a DualShock), `Shaping` (deadzone rescale, press thresholds),
  `Binding` + `default_key_bindings`.
- `gamepad/qemu/dev-gamepad.c` + `usb-gamepad.h` — `usb-gamepad`,
  overlaid into `hw/usb/`, built by patch 26 under `CONFIG_USB_HID`.
- `gamepad/qemu/gameport.{c,h}` — `gameport`, overlaid into `hw/input/`,
  built by patch 27 under its own `CONFIG_GAMEPORT`.
- `player/src/pad.rs` — the host end: `gilrs` and the scripted source,
  shaping, the key map, `--pads`, `--pad-sweep`.
- `embed/libqemu_embed.h` — `qemu_embed_pad_state` /
  `qemu_embed_pad_present` (embed API v8).
- `launcher-core/src/bundle.rs` — `bundle::Pad`, `pad_choices`,
  `default_pad`; `wizard.rs` (`choose_pad`, `pad_notes`, `pad_warning`);
  `player::pad_args`.
- Guest probes: `guest-tools/src/padtest.asm` (`TESTS\PADTEST.COM`),
  `guest-tools/src/padwin.c` (`TESTS\PADWIN.EXE`).
- Tests: the `pad` check, `tools/hid-descriptor-check.py`,
  `tools/pad-guest-test.py`.
- `packaging/flatpak/com._2ksbox.Launcher.yml`: `--device=input`.

## The design

### The host end

`gilrs` reads the pad on every host (evdev, XInput/DirectInput,
IOKit/GameController), with hot-plug, and `PLAYER_PAD_SCRIPT` replays a
scripted pad on the guest's frame clock — the piece that makes every
path testable, since no machine running `scripts/test.sh` has a
controller. The player polls on the **UI thread**, once per published
guest frame: on macOS a HID source wants the process's run loop and the
QEMU thread has none. `player --pads` reports what this host can read,
from the binary that has to read it — the way to tell "no controller"
from "no permission" in a sandbox.

The *choices* (which device, the bindings) are launcher-core's per
ADR-014, but the shared model is a crate of its own: the player cannot
depend on launcher-core without carrying the machine library.

A control never seen counts as **centred, not unknown**; otherwise a
resting stick's first report is a spurious change, which matters once an
axis is a position.

### Path A — `usb-gamepad`

Two analog sticks (X/Y, Z/Rz), an 8-way hat **with a null state**
(without it a released hat reads north) and twelve buttons in a six-byte
report, on the `-usb` controller every seamless-mouse machine already
has. Its own file, not a fourth kind in `dev-hid.c`, because every
machine's `usb-tablet` runs on that code.

- **Absolute state, not events**: one call carries the whole pad, so a
  dropped update is corrected by the next, where a dropped event would
  hold a button for ever. The device NAKs the interrupt endpoint while
  nothing changed, so an idle pad never wakes the guest.
- **Fed out of band**: the embed shim calls `usb_gamepad_set_state()`
  from the input bottom half under the BQL. A joystick event class in
  QEMU's input core was the plan and the wrong model — that core is built
  around consoles and pointer semantics, and a pad has no console. The
  cost: not upstreamable as it stands; the event class is the version to
  write for upstream.
- A second `usb-gamepad` is refused at realize: the host drives one pad.
- `-usb` comes with the pad even when the seamless mouse is off.

| Guest | What it needs |
|---|---|
| XP | nothing: inbox `hidusb.sys` binds on first start |
| Win98 SE | its own HID driver, but the New Hardware wizard asks for the Windows 98 source files the first time (the CD or the CAB folder) |
| Windows Me | untried |
| Win98 FE | untried; its USB stack may want the USB supplement |
| DOS | no USB stack — path B |

### Path B — the gameport

Four RC one-shots and four buttons: a write to 0x201 starts the timers,
a read returns bits 0–3 set while each axis is charging and bits 4–7
clear while a button is held. Pulse width is `24.2 µs + 0.011 µs × R`
over a 0–100 kΩ pot, so an axis byte of 0x00 is 24 µs and 0xff 1124 µs.

- **No timer, and exact**: the write records
  `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` and one deadline per axis, a
  read compares.
- A standalone ISA device at 0x200–0x207 (every card with a port decoded
  all eight), not part of a sound card, so it does not appear and vanish
  with `bundle::Sound`.
- **The d-pad drives the first stick's axes to their ends**
  (`gameport_set_state`; the stick wins while the d-pad is centred). A
  Gravis GamePad has no pots — its directions switch the one-shots
  between extremes — so a DOS game cannot read a d-pad any other way.
- Buttons 1–4 are the four face buttons in `gamepad::HID_BUTTONS` order,
  the same numbers as on path A. The port has nowhere for the rest or a
  hat.
- The same `qemu_embed_pad_state` feeds it; the shim calls both devices'
  entry points and the absent one returns at once.
- An idle port reads **0xf0**; an absent one reads 0xff off the open bus,
  which is how a game decides there is no joystick.

**Pacing is what makes the counts mean anything.** A game counts loop
iterations until a bit clears, so the count depends on the guest's
speed. On the DOS family's `-icount shift=7,align=on` (doc 06, "Why a
rate control, and what it costs") a stick's ends and centre count
12 / 265 / 571 against true pulse ratios of 1 : 23.8 : 46.4, steady to
~6 %. Unpaced, the counts are ten times higher and an axis
nobody touches spans 2.25x — a game with a fixed timeout sees a stick
jammed at one end.

**Win9x gets no gameport driver, by decision (2026-09-10).** The port is
not Plug and Play, so a *Windows* game would need "Standard Game Port"
through Add New Hardware and calibration — but on 98 the USB pad already
reaches both APIs such a game calls: DirectInput, and winmm
(`joyGetPosEx` over VJOYD, whose generic "Microsoft PC-joystick driver"
surfaces the HID pad), sample for sample. The port stays offered on
Win98 for DOS boxes, which read 0x201 directly. XP is not offered it
(nothing enumerates a non-PnP port there), nor is Other.

### Path C — keys

`KeyMap` in `player/src/pad.rs` turns the pad into the key presses the
player already sends (d-pad and left stick are the arrows by default).
It recomputes the wanted scancode set every poll and diffs it against
what is held: a diff cannot drift into a key stuck down in the guest,
and a set union handles keys with two holders. Releases go before
presses in one `input_flush`, so a stick swung across centre never
leaves both arrows down. Focus loss lifts keys through the map. No
analog anything, and a game that asks for a joystick still finds none.

### The launcher model

`bundle::Pad` is `none` / `usb` / `gameport` / `keys`, one choice per
machine; `pad_choices` lists what a family offers, first entry is the
default, and **`none` is first everywhere** — a machine nobody asked for
a pad on should not grow a device in its Device Manager.

| Family | Offered |
|---|---|
| XP, Other | `none`, `usb`, `keys` |
| Win98 | `none`, `usb`, `gameport`, `keys` |
| DOS | `none`, `gameport`, `keys` |

An offered device a family cannot use is refused rather than written; a
`pad` value no build knows falls back. The form warns per device: a USB
pad Windows finds by itself, the gameport needs Add New Hardware and
calibration. `launcher_core::player::pad_args` writes `--pad <name>`.

## Tests

- **`pad`** (host stage): the model to a real QEMU per family — no device
  for `none`, `-usb` with `usb` even without the seamless mouse, no USB
  controller for the gameport, refusals, device swaps — then the port
  through the human monitor (f0 idle, ff armed with the VM stopped, f0 a
  second later), the second-device refusals, `player --pad-sweep` for the
  key map, and `tools/hid-descriptor-check.py`, which the check also
  feeds two mutated descriptors that must fail.
- **`pad-guest`** (guest stage): FreeDOS under the **player** reading the
  gameport with `PADTEST.COM`, every axis judged against the undriven
  axis's own spread; `UNTHROTTLED=1` is the control.
- **`pad-guest-xp`** and **`pad-guest-98`**: `PADWIN.EXE` in a Windows
  guest, DirectInput and winmm columns required to agree sample by
  sample. `pad-guest-98` names a launcher machine (`WIN98_PAD_MACHINE`,
  default `claude98`) whose Windows has bound the pad once, and skips
  where there is none.

All of them run the player, so they skip without a display. Commands and
knobs: `docs/testing.md`.

## Traps

- **No `usb-tablet` beside the pad on Win98**: with it, DirectInput
  enumerates no joystick at all (same image, A/B; XP does not care; the
  mechanism is unknown). `pad-guest-test.py` passes `-usb -device
  usb-gamepad` and nothing else.
- **PADWIN, not PADTEST**: both DOS and cmd resolve a bare name to the
  `.COM` first.
- The Flatpak needs `--device=input` (`--device=dri` does not cover
  `/dev/input`); run `player --pads` inside the sandbox to check.
- `gilrs` links libudev on Linux; the KDE SDK and Platform 6.10 carry it.
  A host without it builds `--no-default-features` and gets the scripted
  pad only.
- `gilrs` on Windows is XInput-first (four pads); a DualShock needs its
  DirectInput fallback.
- macOS: USB HID pads need no entitlement under the hardened runtime; a
  Bluetooth controller may prompt.
- Patches 26/27 and the embed API move together with the library; after
  pulling, rebuild the embed library before the player links.

## What stayed open

- **Path C with a real controller.** Only `PLAYER_PAD_SCRIPT` has driven
  the key map; its guest test was dropped by decision (more harness than
  the path is worth — `--pad-sweep` covers the map, the shared keys, the
  ordering and the scancodes). A wrong binding there looks like a broken
  game, not a broken pad.
- Windows 98 first edition and Windows Me with the USB pad: untried.

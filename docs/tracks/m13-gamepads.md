# Track: M13 — gamepads

The handoff for a session working on getting a real gamepad into a guest.
Read `docs/00-status.md` first for the global picture and the track
rules, then this file, then doc 03 §"Input" for the grab model the pad
has to sit beside and doc 07 §"Input" for what the front ends show.

Opened 2026-09-09. Nothing is implemented — this doc is the design for
all three guest-facing paths, written before any of them is built,
because which one to build first is a product decision and the three do
not share a guest end.

Gamepads were a post-v1 candidate in doc 08 until this track opened.

## The starting position: QEMU has nothing

Every guest-facing path below is new code in our queue. This was checked,
not assumed:

- **No gameport.** Nothing in `qemu/hw/` answers port 0x201. `sb16.c`
  models the DSP and mixer only; the gameport that sits at 0x200–0x207 on
  a real Sound Blaster is not there.
- **No gamepad HID.** `qemu/include/hw/input/hid.h:6` defines exactly
  three kinds — `HID_MOUSE`, `HID_TABLET`, `HID_KEYBOARD` — and
  `hw/input/hid.c` has a poll function for each. `hw/usb/dev-hid.c` has
  descriptor sets for those same three.
  (`hw/input/stellaris_gamepad.c` is buttons wired to GPIO on an ARM
  board. It is not a PC device and shares no code with anything here.)
- **No joystick event class in the input core.** `InputEventKind` is
  key / btn / rel / abs / mtt. There is no axis event a joystick could
  ride, so a new device cannot simply register a `QemuInputHandler` the
  way `hid_mouse_handler` does — the event class has to be added first,
  or the device fed out of band.

So the question is not "which QEMU device do we turn on", it is "which
device do we write".

## Scope and files (this track would own them)

- `player/src/pad.rs` — the host end: `gilrs` → deadzone/curve → the
  embed input queue. Plus `PLAYER_PAD_SCRIPT` (below).
- `embed/libqemu_embed.{h,c}` — API **v8**: `qemu_embed_pad_axis` /
  `_btn` / `_hat`, enqueued from any thread and drained by the existing
  `qemu_embed_input_flush`, exactly like `qemu_embed_key`. Header
  `QEMU_EMBED_API_VERSION` (currently 7, `embed/libqemu_embed.h:138`)
  and `qemu-embed`'s `API_VERSION` (`qemu-embed/src/lib.rs:11`) move
  together.
- `patches/qemu/26-usb-gamepad.patch` — path A.
- `patches/qemu/27-gameport.patch` — path B.
  (25 is M12's OPL3/MPU-401 patch; 26–27 are free and sit in the input
  gap below the 3dfx band at 30.)
- In `launcher-core/src/bundle.rs`: `Pad`, `pad_choices`, the per-family
  defaults and the arguments. The machine form's row in `wizard.rs` and
  both front ends' views of it. **Conflict warning:** M12 is editing
  this same file for `Sound` / `Music`. Sequence, or rebase.
- `guest-tools/src/padtest.c` and `padtest.asm` — the guest probes.
- `scripts/test.sh`: the `pad` check. `tools/pad-guest-test.sh`.
- `packaging/flatpak/com._2ksbox.Launcher.yml` — one line, see Traps.

## Step 0 — the host end, and the thing that makes this testable

Before any guest path, two pieces both of them need.

**`gilrs` in the player.** One Rust crate over evdev (Linux), XInput and
DirectInput (Windows) and IOKit/GameController (macOS), so all three
hosts are one code path and the rule about Rust wherever possible
holds. It reports hot-plug, which the pad model needs: a controller
switched on mid-game must appear without restarting the machine.
Polling rides the existing winit loop — the player is already
publish-driven and wakes on the QEMU refresh tick.

**`PLAYER_PAD_SCRIPT=<file>` — a synthetic pad.** This is the piece to
build first, and the reason is the testing policy: every check in this
track is an integration or end-to-end test, and no machine that runs
`scripts/test.sh` has a physical controller plugged into it. A recorded
sequence of axis/button events, replayed on the guest's own clock,
is what lets a headless run drive a pad at all. Without it path A and
path B can only ever be tested by hand, which means they rot. Everything
below assumes it exists.

The deadzone, the axis curve and the binding table are **launcher-core's**,
not the player's — ADR-014 is explicit that a default which follows the
family does not live in a front end, and a pad binding is exactly that.

## Path A — a USB HID gamepad

A new `usb-gamepad`, modelled on `hw/usb/dev-hid.c`. The descriptor,
endpoint and polling plumbing there is all reusable; what is new is a
report descriptor and a report packer.

- Report descriptor: Generic Desktop / Gamepad, four 8-bit axes (X, Y, Z,
  Rz — two sticks), a hat switch (usage 0x39, logical 0–7 with a null
  state) and 12 buttons. Byte-aligned, 8-byte interrupt IN, `bInterval`
  10 ms — the same shape as `usb-tablet`'s endpoint.
- A `HID_GAMEPAD` kind in `hw/input/hid.c` with `hid_gamepad_poll()`
  packing the report out of a new state struct.
- Feeding it. Two options, and the choice matters beyond this track:
  **(a)** add a joystick event class to `qapi/ui.json` and the input core,
  so the device registers a normal `QemuInputHandler` and the embed shim
  sends events the way it sends keys. More work, touches QAPI, and it is
  the upstream-shaped version — doc 08's post-v1 list already names an
  upstreaming campaign. **(b)** have the embed shim write the device's
  state directly through a small exported entry point. Less invasive,
  and a dead end if we ever offer this upstream. Recommend (a).

**What the guest needs: nothing.**

| Guest | Result |
|---|---|
| XP | inbox `hidusb.sys` / `hidclass.sys`; DirectInput 8 and `joy.cpl` see it on first plug |
| Win98 SE, Me | HID class present; DirectInput enumerates it |
| Win98 FE | USB stack is weak pre-SE; expect to need the USB supplement |
| DOS | nothing — no USB stack at all |

The controller itself is proven: `-usb` (`piix3-usb-uhci`, USB 1.1) with
`usb-tablet` on it is what every seamless-mouse machine already boots
(`launcher-core/src/bundle.rs:953`), on both Windows families, under TCG.
An 8-byte interrupt transfer every 10 ms is far below what that path
already carries.

One bundle change: `-usb` is added today **only when seamless mouse is
on**. A machine with a pad and no seamless mouse needs the controller
anyway, so the condition becomes "either".

This is the best value per unit of work for the era 2ksbox is mostly
used in, and it is the recommended first guest path.

## Path B — the gameport at 0x201

The period-correct one, and the only thing that reaches DOS.

The hardware is four RC one-shots and four buttons. A write to 0x201
starts the timers; a read returns bits 0–3 set while each axis is still
charging and bits 4–7 clear while each button is held. Pulse width is
t = 24.2 µs + 0.011 × R µs over a 0–100 kΩ pot, so 24 µs at one extreme
to about 1124 µs at the other, roughly 600 µs centred. A game writes,
then counts loop iterations until the bit clears, and the count is the
axis.

The model needs no timers: record `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`
and the four deadlines on the write, compare against them on each read.
A few lines, and exact.

**The risk, and why it is smaller than it looks.** A guest that counts a
busy-wait loop is measuring our one-shot against its own execution rate,
and under plain TCG that rate is nothing like a period CPU — a game with a
fixed timeout count can give up before the pulse ends. But the DOS family
already runs `-icount shift=N,align=on` for its processor picker
(`docs/tracks/m6-launcher.md`, `tools/dos-guest-test.py`), and `align=on`
is the whole thing: the guest's loop and the one-shot deadline are then
paced by one clock at a chosen rate. The throttled DOS machine is the
mitigation, not a complication. What must be checked early is a machine
with the throttle off.

Where it lives: on real hardware, on the sound card. Simplest here is a
standalone ISA device claiming 0x200–0x207, so it is independent of which
card the family picked and the bundle names it directly.

| Guest | Result |
|---|---|
| DOS | reads 0x201 directly — this path or nothing |
| Win9x | "Standard Game Port" + `VJOYD.VXD` / `MSANALOG.DRV`; not PnP, so Add New Hardware, then calibrate in `joy.cpl` |
| XP | `gameenum.sys` exists but Microsoft was already retiring analog sticks; not the XP answer |

Limits are the hardware's: two axes and two buttons per port (four and
four with a Y-cable), and a calibration step the user has to do once per
guest. An analog stick's centre drifts; that is period-accurate and still
annoying.

## Path C — map the pad onto keys and the mouse

No guest device. `player/src/pad.rs` turns buttons and stick directions
into the key and mouse events the player already sends, against a
binding table from launcher-core.

Works on **every** guest — DOS, Win98 FE, XP, the Other family — with no
QEMU patch and no guest install, and it is roughly a day. It is also what
a lot of people actually want for a DOS platformer, where the game only
ever read the keyboard.

The honest limit, which the wizard should say plainly: there is no analog
anything, and a game that enumerates DirectInput or reads 0x201 still
finds no controller. It is a mapping, not a gamepad.

## The launcher model

`bundle::Pad` — `none` / `usb` / `gameport` / `keys` — with
`pad_choices` per family and first entry winning, the way
`bundle::video_choices` works:

| Family | Offered, in order |
|---|---|
| XP | `usb`, `keys`, `none` |
| Win98 | `usb`, `gameport`, `keys`, `none` |
| DOS | `gameport`, `keys`, `none` — no `usb` |
| Other | `usb`, `keys`, `none` |

Win98's first entry is a judgement to make with a real 98 SE image in
front of you: HID works on SE and Me, and the FE case is the doubt.

Picking `gameport` under an installed 9x guest is a hardware change — the
guest will not find it without Add New Hardware, and then wants
calibrating. The wizard says that in orange, the way the display-adapter
picker already does for a changed adapter.

## Testing

Integration and end-to-end only, per the policy.

- **`pad` check in `scripts/test.sh`** — the model to a real QEMU, built
  like `pointer_check()` (`scripts/test.sh:698`): `launcherx --new` per
  family, `--print-args`, and our own `qemu-system-i386` accepting every
  line. It must also prove that `none` emits no device, that `usb` brings
  `-usb` even with seamless mouse off, and that `gameport` is refused on
  a family that does not offer it — the same shape as the
  `display-adapter` check, where an adapter a family lacks a driver for
  is refused rather than written.
- **`guest-tools/src/padtest.c`** — DirectInput enumeration plus axis and
  button readout on XP and 98, `padtest.log`. The sibling of
  `DRIVER\DITEST.EXE`, which already does this for the keyboard.
- **`guest-tools/src/padtest.asm`** — a DOS `.COM` reading 0x201 and
  printing the four counts and the button bits.
- **`tools/pad-guest-test.sh <image> [xp|win98|dos]`** — headless, the
  player driving a scripted pad through `PLAYER_PAD_SCRIPT` while the
  guest probe reports over COM1. Overlay only, never the user's image.
  Local only, not in `scripts/test.sh`.

## Next steps, in order

1. **Step 0** — `gilrs` in the player, the launcher-core binding model,
   and `PLAYER_PAD_SCRIPT`. Nothing else is testable without it.
2. **Path C** — the key mapping. Smallest, reaches every guest, and it
   exercises the whole host end and the launcher model against a guest
   that needs no new device.
3. **Path A** — `usb-gamepad`, with the joystick event class in the input
   core. The main event: XP and 98 SE with nothing to install.
4. **Path B** — the gameport, for DOS and the 9x analog stack. Last
   because it is the one with a timing model to get wrong.

Path C is deliberately not a throwaway: it stays as a `keys` choice for
DOS games that never read a joystick and for Win98 FE.

## Traps

- **The Flatpak sees no input devices.** `finish-args` is
  `--device=kvm` and `--device=dri` and nothing else
  (`packaging/flatpak/com._2ksbox.Launcher.yml:32`). A gamepad needs
  `--device=input`. One line, invisible until someone plugs a pad into
  the Flatpak and nothing happens.
- **The embed API version.** Header and the `qemu-embed` crate move
  together, and every machine rebuilds the library before the player
  links, or it is `undefined symbol _qemu_embed_…`.
- **`bundle.rs` is contended.** M12 is adding `Sound` and `Music` to the
  same file. Whoever is second rebases.
- **gilrs on Windows is XInput-first**: four pads, and a DualShock needs
  its DirectInput fallback. Worth knowing before someone reports "my
  controller works in other games".
- **macOS**: USB HID pads need no entitlement under the hardened runtime,
  but a Bluetooth controller can prompt. `package-macos.sh` already asks
  the staged app a list of questions; this belongs on it.
- **The grab model.** A pad is not the mouse and must not touch the grab
  state machine in doc 03 — a controller works whether or not the window
  has grabbed the pointer. Say so in doc 07's Input bullet, which
  currently ends "Gamepads → DirectInput research post-v1."

## Rules

Track rules are in `docs/00-status.md` §Tracks. Shared files
(`player/`, `launcher-core/src/bundle.rs`, `scripts/test.sh`,
`docs/00-status.md`) are edited minimally and the commit message says
`m13:`.

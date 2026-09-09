# Track: M13 — gamepads

The handoff for a session working on getting a real gamepad into a guest.
Read `docs/00-status.md` first for the global picture and the track
rules, then this file, then doc 03 §"Input" for the grab model the pad
has to sit beside and doc 07 §"Input" for what the front ends show.

Opened 2026-09-09 with the design for all three guest-facing paths,
written before any of them was built, because which one to build first is
a product decision and the three do not share a guest end.

**All four steps landed 2026-09-09**: step 0, path C, path A, and path B
the same day. A machine can now have a real USB HID gamepad (`-device
usb-gamepad`, patch 26) that XP, 98 SE and Me drive with nothing
installed; a gameport at 0x200-0x207 (`-device gameport`, patch 27),
which is the only path that reaches DOS and the one a **real DOS guest
has now driven** (`tools/pad-guest-test.py`); or the key mapping, on
everything. See "State" below for what is in, what the plan got wrong,
and what is still not proved against a real *Windows* guest.

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

## State

All of it is in — step 0, path C, path A and path B — and
`scripts/test.sh host` is green (34 passed, 0 failed). What exists, in the
order it was built:

- **`gamepad/`** — a new workspace crate holding the abstract pad:
  `Control` (20 controls, face buttons named by *position* so a binding
  does not move between an Xbox pad and a DualShock), `Shaping` (the
  deadzone rescale and the two press thresholds) and `Binding` +
  `default_key_bindings` for path C. Its own crate, shared the way
  `shader-chain` is, for the reason under "What the plan got wrong".
- **`player/src/pad.rs`** — the host end. Two sources behind one trait:
  `gilrs` (verified enumerating on Linux) and `PLAYER_PAD_SCRIPT`.
  Polled from `user_event`, on the **UI thread**, once per published
  guest frame — not the QEMU thread, because on macOS a HID source wants
  the process's run loop and the QEMU thread has none.
- **`player --pads`** — what this host can actually read, out of the
  binary that has to read it. The only place a build without the `gilrs`
  feature or a sandbox with no `/dev/input` reports itself.
- **`player --pad-sweep <frames>`** — the scripted pad with no window, no
  QEMU and no guest.
- **`bundle::Pad`** — `none` (the default on every family), `usb`,
  `gameport` and `keys` — with `pad_choices`, `default_pad`, the `pad` field,
  `Form::choose_pad` / `pad_notes` / `pad_warning`, and the ninth
  `--wizard-edit` argument. Every family starts on `none` for the reason
  networking does: a machine nobody asked for a pad on should not grow a
  device in its Device Manager.
- **The `pad` check** in `scripts/test.sh`.
- Packaging: `--device=input` in the Flatpak manifest, and
  `cargo-sources.json` regenerated (1109 → 1133 sources).

Verified rather than assumed: the KDE SDK **and** the Platform runtime
both carry `libudev.so.1` and `libudev.pc`, which `gilrs` needs on Linux
through `libudev-sys`; and `gilrs` cross-compiles clean for
`x86_64-pc-windows-gnu`, so the Windows package is not at risk.

Then **path C** (same day):

- **`KeyMap`** in `player/src/pad.rs` — the pad's pressed halves as key
  presses. It recomputes the wanted set of scancodes every poll and diffs
  it against what is held, rather than reacting to transitions. Two
  reasons, both load-bearing: a diff cannot drift into a key stuck down
  in the guest (which outlives the mistake and cannot be cleared from the
  host), and a *set union* handles the default map's shared keys — both
  the d-pad and the left stick drive the arrows, so `left` has two
  holders and must not be released when only the first lets go.
- Releases are emitted **before** presses in the same batch, and the
  batch is one `input_flush`. A stick swung across centre changes both
  halves in one poll; the other order leaves a guest that samples between
  the two calls holding both arrows.
- `lift_all_keys` (focus loss) now goes through the map rather than
  behind its back, or the map would still believe the key was down and
  never press it again.
- **`launcher_core::player::pad_args`** writes `--pad keys` from
  `bundle::Pad`, beside `shader_args` and for the same reason; `spawn`
  passes it, and `launcherx --print-player-args` shows what a bundle
  resolves to without spawning anything.

Then **path A** (same day) — the device QEMU did not have:

- **`gamepad/qemu/dev-gamepad.c`** — `usb-gamepad`, overlaid into
  `hw/usb/` by `prepare-qemu.sh` and built by **patch 26** under
  `CONFIG_USB_HID`. Two analog sticks as X/Y and Z/Rz, an 8-way hat *with
  a null state* and twelve buttons, in a six-byte report. Its own file
  and not a fourth kind in `dev-hid.c`, because every machine here
  already has a `usb-tablet` on that code.
- **Absolute state, not events** (`usb_gamepad_set_state`, embed API
  **v8**). The host always knows the whole pad, so sending all of it
  cannot desync — a dropped update is corrected by the next one, where a
  dropped event leaves a button held forever. The device compares against
  what it holds and NAKs the interrupt endpoint while nothing has
  changed, so a motionless pad never wakes the guest.
- A second `-device usb-gamepad` is **refused at realize**, not silently
  ignored: the host drives one pad, and two would leave whichever the
  lookup found first as the only live one.
- **`tools/hid-descriptor-check.py`** parses the shipped descriptor bytes
  and checks the collections balance, that the input items total exactly
  `GAMEPAD_REPORT_LEN`, and that the hat has its null state. None of that
  is readable from this side at run time, and each failure produces a
  device that enumerates, appears in `joy.cpl` and is wrong. The `pad`
  check also mutates two copies and requires the checker to complain, so
  the check cannot pass by having stopped working.
- **Both front ends** gained the row — the Qt one that ships and the egui
  one ADR-015 keeps maintained. Worth saying plainly because step 0 and
  path C did *not*: the setting existed in the model and was reachable
  only by editing `machine.toml` by hand. It is a picker in both now.

And **path B** (same day) — the port QEMU never had:

- **`gamepad/qemu/gameport.c`** — `gameport`, overlaid into `hw/input/`
  and built by **patch 27** under a `CONFIG_GAMEPORT` of its own
  (`default y / depends on ISA_BUS`). A standalone ISA device and not a
  member of one sound card, because the family's card is a separate
  choice (`bundle::Sound`) and a joystick should not appear and vanish
  with it. All eight ports 0x200-0x207 answer, as every card that carried
  a gameport decoded them.
- **No timer, and it is exact**: the write records `qemu_clock_get_ns(
  QEMU_CLOCK_VIRTUAL)` plus one deadline per axis, and a read compares
  against them. `t = 24.2 us + 0.011 x R us` over a 0-100 kohm pot, so an
  axis byte of 0x00 is 24 us and 0xff is 1124 us.
- **The same `qemu_embed_pad_state` feeds it**, beside the usb-gamepad —
  no embed API bump, because it is one more consumer of the same bytes and
  a machine has one device or the other (`bundle::Pad` is a single
  choice). The shim calls both entry points unconditionally and the absent
  one returns at once.
- **The d-pad drives the first stick's axes to their ends**, in the
  device. That is not a convenience mapping: a Gravis GamePad has no pots
  at all — its directions switch the one-shots between the two extremes —
  so a DOS game cannot tell a stick from a d-pad and has no other way to
  read one. Without it the control every era game is played with would do
  nothing on the only path that reaches DOS.
- **Buttons 1-4 are the four face buttons**, in `gamepad::HID_BUTTONS`
  order, so a control is the same number on both guest paths. The port has
  nowhere to put the other eight, or a hat, and that is the hardware's
  limit rather than a simplification.
- **`bundle::Pad::Gameport`**, offered to **DOS** (which cannot have path
  A at all) and **Win98** (which has both stacks), and to neither XP nor
  `Other` — a non-PnP port has nothing to enumerate it on XP and no driver
  step this project can name on an unknown OS. The wizard's warning is
  per-device now, because the true sentence differs: Windows finds a USB
  pad by itself, and does *not* find this one — Add New Hardware, then
  calibrate.
- **`guest-tools/src/padtest.asm` → `PADTEST.COM`** on the ISO, and
  **`tools/pad-guest-test.py`**, which is the proof (below).

### What a real DOS guest reads

`tools/pad-guest-test.py`, 2026-09-09, FreeDOS under the **player** (it
has to be the player: the pad reaches the guest through the embed library,
and a bare `qemu-system-i386` has a gameport nothing ever moves).
`PLAYER_PAD_SCRIPT` stands in for a controller nobody has plugged in.

On the paced machine the family really uses (`-icount shift=7,align=on`):

| stick | pulse | counts |
|---|---|---|
| short end (0x00) | 24 us | 12-13 |
| centred (0x80) | 576 us | 265 |
| long end (0xff) | 1124 us | 571 |

265/12 = 22 against a true 23.8, and 571/265 = 2.15 against 1.95 — the
model is what the hardware is, measured through a guest's own counting
loop. The idle port reads **0xf0** (expired, nothing held), which is how a
game tells it apart from the 0xff an absent port gives off the open bus.

`UNTHROTTLED=1` is the control, and it is the thing to know about this
path: with the pacing off the same positions count **ten times higher**
(89..5828) and the *undriven* axis wanders 1125..2530 — a 2.25x spread on
a stick nobody is touching, because a count is loop iterations and the
host is deciding the guest's speed moment to moment. A game with a fixed
timeout sees a stick jammed at one end. The DOS family's `-icount
…,align=on` is the mitigation and it was already there for the processor
picker; the harness therefore judges every axis against the undriven
one's own spread rather than against a number chosen in advance, so the
same checks pass in both machines and say what changed.

### Not proved yet

**No *Windows* guest has seen any of this.** A DOS guest has now driven
path B end to end (above), which also proves the whole host chain —
`gilrs`-shaped source, shaping, `hid_state()`, the embed queue, the input
bottom half, the device — since path A shares every part of it but the
last. What is still checked *without* a guest is the rest: the key
mapping, the ordering, the shared keys, the scancodes, the HID report
packing, the descriptor bytes, and that a real `qemu-system-i386` attaches
the device to its bus (`info usb` says *2ksbox USB Gamepad*).

Three things are therefore still unwatched, and they are the Windows ones:

- **path A in a guest** — that Windows enumerates the HID pad, binds a
  driver and shows a working controller in `joy.cpl`. It rests on how the
  HID class is specified, not on having seen it. Wants
  `guest-tools/src/padtest.c`: DirectInput enumeration plus axis and
  button readout, the sibling of `DRIVER\DITEST.EXE`.
- **path B on 9x** — "Standard Game Port" through Add New Hardware, then
  calibration in `joy.cpl`. The wizard tells someone to do this; nobody
  has done it here. `PADTEST.COM` runs in a Win98 DOS box unchanged and is
  the way to tell "the port is not there" from "the driver is not there".
- **path C in a guest** — keys arriving. Cheap once a Windows pad harness
  exists.

Also untried: a **real controller**. `gilrs` enumerates here (a foot
pedal is what is plugged into this box), so the button and axis mapping
is compiled and enumerated but never felt.

### What the plan got wrong

Two corrections, both worth keeping because the reasoning was wrong and
not just the detail.

1. **Embed API v8 does not belong in step 0.** The original scope put
   `qemu_embed_pad_axis` / `_btn` / `_hat` here. There is nothing to
   receive them: path C sends *keys* through the existing
   `qemu_embed_key`, and the pad functions have no consumer until path A
   or B has a device. Adding them now would have been dead API across a
   version bump that every machine must rebuild for. **The v8 bump moves
   to path A.**
2. **The model could not live in `launcher-core`.** ADR-014 says a
   binding and a deadzone are launcher-core's, and that is still true of
   the *choices* — but the player cannot depend on launcher-core without
   inverting the architecture: the player is the runtime, the launcher is
   the manager that spawns it, and the player would end up carrying the
   whole machine library. The precedent was already in the tree
   (`shader-chain`, shared by both), so the shared half became the
   `gamepad` crate and the deciding half stayed in `launcher-core` as
   `bundle::Pad`.

3. **The joystick event class was the wrong way in.** The plan
   recommended adding one to QEMU's input core (`qapi/ui.json`,
   `ui/input.c`) so the device could register a `QemuInputHandler` like
   `hid_mouse_handler`, on the grounds that it is the upstream-shaped
   version. It is not just more code — it is the wrong *model*. QEMU's
   input core is built around consoles and pointer/keyboard semantics
   (`qemu_input_is_absolute(con)`, handler activation, per-console
   routing), and a gamepad has no console affinity at all; forcing it
   through would have meant describing the device as something it is
   not. Path A instead exports `usb_gamepad_set_state()` and the embed
   shim calls it from the input bottom half, under the same BQL the rest
   of the queue drains under. The cost is honest: this is **not**
   upstreamable as it stands, and if the device is ever offered upstream
   the event class is the version to write.

A fourth thing the plan simply did not foresee: **a control not yet seen
counts as centred, not as unknown.** The obvious version emits a "change"
the first time a resting stick reports itself, because the map has no
entry for it. Harmless for a key and not harmless at all once path B
makes an axis a position; the `pad` check pins it.

## Scope and files (this track owns them)

- `gamepad/` — the abstract pad, shared by the player and (from path C)
  `launcher-core`. ✅
- `player/src/pad.rs` — the host end: `gilrs` and the scripted source,
  the shaping, `--pads` and `--pad-sweep`. ✅
- `embed/libqemu_embed.{h,c}` — API **v8**: `qemu_embed_pad_axis` /
  `_btn` / `_hat`, enqueued from any thread and drained by the existing
  `qemu_embed_input_flush`, exactly like `qemu_embed_key`. Header
  `QEMU_EMBED_API_VERSION` (currently 7, `embed/libqemu_embed.h:138`)
  and `qemu-embed`'s `API_VERSION` (`qemu-embed/src/lib.rs:11`) move
  together. **Path A, not step 0** — see above.
- `patches/qemu/26-usb-gamepad.patch` — path A.
- `patches/qemu/27-gameport.patch` — path B.
  (M12's OPL3/MPU-401 patch landed as **60**, not the 25 this doc first
  guessed at; 26–27 are free and sit in the input
  gap below the 3dfx band at 30.)
- In `launcher-core/src/bundle.rs`: `Pad`, `pad_choices`, the per-family
  defaults and the arguments. The machine form's row in `wizard.rs` and
  both front ends' views of it. **Conflict warning:** M12 is editing
  this same file for `Sound` / `Music`. Sequence, or rebase.
- `gamepad/qemu/gameport.{c,h}` — path B's device. ✅
- `guest-tools/src/padtest.asm` — the DOS probe. ✅ (`padtest.c`, the
  DirectInput one, is still unwritten.)
- `scripts/test.sh`: the `pad` check. ✅ `tools/pad-guest-test.py` (a
  `.py`, not the `.sh` this doc first named: it wants the FreeDOS floppy
  `tools/x87-guest-test.py` fetches and it parses counts). ✅
- `packaging/flatpak/com._2ksbox.Launcher.yml` — one line, see Traps.

## Step 0 — the host end, and the thing that makes this testable  ✅

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

## Path A — a USB HID gamepad  ✅

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

## Path B — the gameport at 0x201  ✅

The period-correct one, and the only thing that reaches DOS. **Landed
2026-09-09** (patch 27, `gamepad/qemu/gameport.c`); a real DOS guest reads
it, and the counts are in "What a real DOS guest reads" above. What
follows is the design, which is what was built.

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

**Measured, 2026-09-09.** Both, through a guest's own counting loop. Paced,
a stick's three positions come back as 12 / 265 / 571 counts against true
pulse ratios of 1 : 23.8 : 46.4 — right, and steady to about 6 %.
Unpaced, the same positions count ten times higher *and the undriven axis
alone spans 2.25x*, so a game reading a stick nobody is touching sees it
move. The risk is real and the throttle is what answers it; the harness
measures the noise instead of assuming it (see above).

Where it lives: on real hardware, on the sound card. Here it is a
standalone ISA device claiming 0x200–0x207, so it is independent of which
card the family picked and the bundle names it directly. Built under its
own `CONFIG_GAMEPORT` (`default y / depends on ISA_BUS`).

One thing the design did not have, and the port needs: **the d-pad has to
drive the first two axes**. A Gravis GamePad's directions switch the
one-shots between their extremes — there are no pots in one at all — so a
DOS game cannot tell a d-pad from a stick and has no other way to read
one. It happens in `gameport_set_state`, where the port's own shape
belongs, and the stick wins whenever the d-pad is centred.

| Guest | Result |
|---|---|
| DOS | reads 0x201 directly — this path or nothing |
| Win9x | "Standard Game Port" + `VJOYD.VXD` / `MSANALOG.DRV`; not PnP, so Add New Hardware, then calibrate in `joy.cpl` |
| XP | `gameenum.sys` exists but Microsoft was already retiring analog sticks; not the XP answer |

Limits are the hardware's: two axes and two buttons per port (four and
four with a Y-cable), and a calibration step the user has to do once per
guest. An analog stick's centre drifts; that is period-accurate and still
annoying.

## Path C — map the pad onto keys and the mouse  ✅ (the keys half)

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

| Family | Offered, in order | Built |
|---|---|---|
| XP | `usb`, `keys`, `none` | `none`, `usb`, `keys` |
| Win98 | `usb`, `gameport`, `keys`, `none` | `none`, `usb`, `gameport`, `keys` |
| DOS | `gameport`, `keys`, `none` — no `usb` | `none`, `gameport`, `keys` |
| Other | `usb`, `keys`, `none` | `none`, `usb`, `keys` |

The plan's first column and what was built differ in one way, and it is
deliberate: **`none` is first, so it is every family's default**, for the
reason the "State" section gives — a machine nobody asked for a pad on
should not grow a device in its Device Manager. The plan's ordering was
written before that decision and is kept here to show the change.

Win98's *first offered device* is the HID pad, which is a judgement to
make with a real 98 SE image in front of you: HID works on SE and Me, and
the FE case is the doubt. `Other` is offered no gameport — the guest is an
OS this project cannot name a driver step for — and XP none either, since
nothing enumerates a non-PnP port there.

Picking `gameport` under an installed 9x guest is a hardware change — the
guest will not find it without Add New Hardware, and then wants
calibrating. The wizard says that in orange, the way the display-adapter
picker already does for a changed adapter.

## Testing

Integration and end-to-end only, per the policy.

- **`pad` check in `scripts/test.sh`** ✅ — the model to a real QEMU,
  built like `pointer_check()`: `launcherx --new` per family,
  `--print-args`, and our own `qemu-system-i386` accepting every line.
  `none` emits no device, `usb` brings `-usb` even with the seamless mouse
  off, the gameport brings *no* USB controller, a family that is not
  offered a device refuses it rather than writing it (`usb` on DOS,
  `gameport` on XP), switching Win98 from one device to the other takes
  the first away, and a `pad` value no build knows falls back instead of
  refusing the machine. Then the port itself, through the human monitor —
  which is the only way to read 0x201 with no guest: **f0 idle, ff armed
  with the VM stopped** (the virtual clock does not move, so it is exact
  rather than a race against a 576 us pulse) **and f0 a second later**. A
  second `gameport` is refused like a second `usb-gamepad`.
- **`guest-tools/src/padtest.asm`** ✅ → `PADTEST.COM` on the ISO: a DOS
  `.COM` that arms 0x201 and counts the four one-shots down with
  interrupts off, printing the counts and the button nibble to COM1 *and*
  the screen — so it is both the harness's evidence and something to run
  by hand in a DOS box (including a Win98 one, where it separates "no
  port" from "no driver").
- **`tools/pad-guest-test.py`** ✅ — the DOS half, and the only test in the
  track that runs the **player** (the pad reaches a guest through the
  embed library; a bare QEMU has a gameport nothing moves). A scripted pad
  poses one control per axis so the checks need no clock alignment, and
  every axis is judged against the *undriven* axis's own spread.
  `UNTHROTTLED=1` is the control. Local only, not in `scripts/test.sh`.
- **`guest-tools/src/padtest.c`** — still to write: DirectInput
  enumeration plus axis and button readout on XP and 98, `padtest.log`.
  The sibling of `DRIVER\DITEST.EXE`, which already does this for the
  keyboard. With it, the Windows half of the harness (`--pad usb` and
  `--pad keys` in a real guest, from an image overlay).

## Next steps, in order

1. ~~**Step 0** — `gilrs` in the player, the binding model, and
   `PLAYER_PAD_SCRIPT`.~~ **Done 2026-09-09.**
2. ~~**Path C** — the key mapping.~~ **Done 2026-09-09**, except the
   guest proof — see "Not proved yet".
3. ~~**Path A** — `usb-gamepad`.~~ **Done 2026-09-09**, except the guest
   proof — see "Not proved yet". (The joystick event class in QEMU's
   input core was *not* the way; see the third correction below.)
4. ~~**Path B** — the gameport, for DOS and the 9x analog stack.~~ **Done
   2026-09-09**, and a DOS guest reads it (patch 27,
   `tools/pad-guest-test.py`). The timing model that was the reason to
   leave it last is measured in both machines, paced and not.

What is left, in order:

5. **`guest-tools/src/padtest.c` and the Windows half of
   `tools/pad-guest-test.py`** — DirectInput enumeration in a real XP and
   Win98 guest, from an image overlay. This is the whole of "Not proved
   yet" for paths A and C, and it is now the only thing between this track
   and done.
6. **Path B on Win98**, by hand once: "Standard Game Port" through Add New
   Hardware, then calibrate in `joy.cpl`. The wizard tells someone to do
   this and nobody here has. `PADTEST.COM` in a Win98 DOS box is the A/B
   that separates a missing port from a missing driver.
7. **A real controller**, on all three paths, once one is plugged into a
   machine that runs this.

Path C is deliberately not a throwaway: it stays as a `keys` choice for
DOS games that never read a joystick and for Win98 FE.

## Traps

- ~~**The Flatpak sees no input devices.**~~ Fixed in step 0:
  `--device=input` is in `finish-args`. It was invisible until someone
  plugged a pad into the Flatpak and nothing happened, which is why
  `player --pads` exists — run it *inside* the sandbox to tell "no
  controller" from "no permission".
- **`gilrs` links libudev on Linux** (through `libudev-sys`), so the
  player has a system dependency it did not have before. Checked in step
  0: `org.kde.Sdk` 6.10 has `libudev.pc` and `org.kde.Platform` 6.10 has
  `libudev.so.1`, so the Flatpak builds and runs. A host or sandbox
  without it builds `--no-default-features` and gets the scripted pad
  only, which the feature exists for.
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

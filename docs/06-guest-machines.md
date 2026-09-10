# 6. Guest machines: Win98 and XP reference configs

The frontend ships four "machine families" with tested defaults. These are the
reference definitions the guided creation flow instantiates; users supply
their own OS media and licenses. Three of them are machines this project is
actually built around — Win98, XP, DOS — and the fourth, **Other**, is the
catch-all for an era OS that is none of those (BeOS, a period Linux, OS/2):
standard hardware, and none of ours.

## Windows 98 SE machine

Modeled as a ~1998–2000 consumer PC.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX + PIIX) | period-correct chipset, best-tested with 9x |
| CPU model | `pentium3` (TCG) / host-masked (KVM) | avoids CPUID features 9x mishandles; sidesteps the fast-CPU Win9x bugs (e.g. the >2.1 GHz-class IOS/NDIS crashes). **Floor is pentium3 (SSE1)**: our guest-tools wrappers are built `-march=pentium3` (upstream builds them x86-64-v2 and expects `-cpu host`/`max`) |
| RAM | 256 MB default, **≤ 512 MB hard cap** | 9x VCache breaks above ~512 MB without patches |
| Video | **`-vga cirrus` (the default) or `-vga none -device d3dpt-vga` + our driver (doc 19)** — a choice since 2026-09-07 (`bundle::Video`) | ours is the whole display path: the mode table, the desktop straight from VRAM, the paced page flips, Direct3D through the driver. The Cirrus is Windows' in-box 2D driver and where this family **starts** (2026-09-07): the 9x driver of ours is much newer than XP's, so a new 98 machine comes up on the driver Windows already has and is moved to ours deliberately. The standard VGA is not offered on either Windows family |
| Audio | **SB16 + its OPL3 (the default), or AC'97, or a Gravis Ultrasound, or none** — a choice since 2026-09-09 (`bundle::Sound`, doc 20) | the SB16 is what Windows has a driver for in the box and what a DOS box inside 98 expects, and since 2026-09-09 it carries the FM chip a real one had (QEMU's `sb16` has none, so 98's own MIDI output had nothing to play on). The AC'97 sounds better and needs the guest-tools driver; the Gravis needs Gravis's own |
| Music | **an MPU-401 at 0x330 with a General MIDI synthesizer (the default), a Roland CM-32L, or nothing** (`bundle::Music`, doc 20) | 98 has no wavetable synthesizer of its own — its MIDI output is the FM chip — so this is what makes a game's music sound like anything. Windows finds the port after "MPU-401 Compatible" is added from Add New Hardware |
| Net | PCnet (AMD), **off on a new machine** | driver in-box on 98. The card is what the wizard's networking checkbox gives the machine; since 2026-09-07 a new machine of every family starts without one (doc 07, `bundle::default_network`) — an unpatched guest is not put on a network before anyone asks |
| Storage | IDE HDD (qcow2) + our ATAPI CD | period-correct; no VirtIO for 9x |
| Input | PS/2 mouse + kbd; USB tablet optional | the bundle's `seamless_mouse` (doc 07), on by default here: the tablet is absolute, so nothing is grabbed; off leaves the PS/2 relative mode games want (see doc 03) |
| Gamepad | **off on a new machine**; a USB HID pad, the gameport at 0x201, or the key mapping (`bundle::Pad`, M13) | the one family offered both devices, because it is the one with both stacks: 98 SE binds its in-box HID driver to the USB pad (user-confirmed 2026-09-09 — it asks for the Windows 98 source files the first time, not for anything of ours), and the gameport is what a DOS box under it and a 1995 title want — a real pad reads correctly through `PADTEST.COM` in a Win98 DOS box (2026-09-10), though nobody has yet installed "Standard Game Port" so that *Windows* sees a joystick. The port is **not** Plug and Play — Add New Hardware, then calibrate — and the wizard says so |
| Floppy | enabled | driver/utility sneakernet, boot disks |

Known QEMU-side traps (tracked in `patches/qemu/README.md`): qemu-3dfx 3D
activates only for a frontend that registers a context provider — the
player does (patch 30, `embed/embedfx.c`); a bare `qemu-system-i386` has
none since SDL was dropped (2026-09-07) and refuses pass-through cleanly;
9.2.4 TCG needs
the upstream LSS fix (issue 2987) or Win98 faults with exception 0D on first
boot; TCG also faults RUNDLL32 in Display Properties since 7.2 (issue 1964,
still open as of 2026-09 — cosmetic, the OS survives; KVM/WHPX unaffected).

Guest install notes (docs shipped with the app): install from user's CD image;
apply guest-tools ISO (SoftGPU, 3dfx wrappers, AC'97, unofficial fixes the
user opts into). **The install must come out ACPI.** A PnP-BIOS install
leaves the PCI bus un-enumerated — "Plug and Play BIOS" with a yellow ! and
no PCI hot-adds ever detected (USB tablet, AC'97, NIC). Known quirks to
document: DOS-compatibility-mode storage regressions.

### Why a plain SETUP used to install PnP-BIOS (2026-09-06)

Setup's `DetectACPIBIOS` (`sysdetmg.dll`, PRECOPY1.CAB) decides from the
**legacy BIOS date at F000:FFF5**, against the `ACPICheckDate` its own
`machine.inf` writes:

```
; machine.inf, [BaseWinOptions] → ACPI_BASE
;   "Add the date after which ACPI GoodBiosList will not be used"
HKLM,Software\Microsoft\Windows\CurrentVersion\Detect,ACPICheckDate,,"12/01/99"
```

A BIOS at least that new is believed. An older one is believed only if it
matches `BIOSINFO.INF`'s `[GoodACPIBios]` — four 1998 machines (Compaq
Armada 19 and Capone, Intel Atlanta, Toshiba Santa Clara) named by their
ACPI OEM ids. SeaBIOS reports **06/23/99**, five months short, and QEMU's
tables say OEM `BOCHS `, creator `BXPC` — so no match, and setup fell back
to PnP-BIOS. (`BadACPIBios`, all Dell/Toshiba laptops, never matched us
either; the `ASL Compiler version < 1.0` gate in the same function only
applies to tables whose creator id is `MSFT`, so it never fired on ours.)

`SETUP /p j` forces it — it sets the `ACPIOption` value the same function
reads — but a launcher cannot type that for someone at a DOS prompt. So
**`scripts/prepare-qemu.sh` stamps the firmware's date to `12/31/99`**
instead (every `pc-bios/bios*.bin`, eight ASCII bytes ending three from the
end of each image; 1999 and not a 2000s date because the comparison is on a
two-digit year). Nothing else reads the field — the per-machine quirks in
`BIOSINFO.INF` that key on `date=` all want an exact 1994–96 day, and a
guest's clock comes from the RTC. The `bios-date` check in
`scripts/test.sh` asks a running QEMU what the guest reads there.

The alternative, kept in reserve: `-machine pc,x-oem-id=COMPAQ,x-oem-table-id="CAPONE  "`
makes us match `[CompaqCapone]` (its rules want only FACP OEM revision ≥ 1
and RSDT creator revision ≥ 0, both hardcoded to 1 by
`hw/acpi/aml-build.c`), which needs no firmware change at all but puts a
vendor's name on every table.

**Confirmed by an install, 2026-09-07** (the user, on the Win98 SE CD this
was read out of): with the stamped date a plain `D:\WIN98\SETUP` comes out
ACPI. `/p j` is no longer needed anywhere — it stays only as the thing that
explains an image installed before the stamp, which is repaired through
Device Manager (PnP-BIOS→PCI Bus, `docs/build-macos.md`) rather than
reinstalled.

## Windows XP machine

Modeled as a ~2002–2005 PC.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX) | best compat with XP-era drivers; q35 unnecessary |
| CPU | `pentium3`/host-class with sane flags | XP handles more, keep TCG features modest |
| RAM | 512 MB–1 GB default | period-typical, snappy |
| Video | **`-vga none -device d3dpt-vga` + our driver on both — XP since 2026-09-04 (doc 15), Win98 since 2026-09-07 (doc 19); Win98 3D also has qemu-3dfx — or `-vga cirrus`, a choice in the wizard since 2026-09-07 (`bundle::Video`)** | d3dpt-vga: the host's mode table (640×480…1600×1200, 16/32 bpp, 60/75/85 Hz), desktop straight from VRAM; before the driver is installed it is a standard VGA (vga.sys, 800×600×4). Cirrus: XP inbox driver for 2D (up to 1024×768×16 / 800×600×24; std VGA has **no** XP driver, which is why it is not on offer here). Wrappers for 3D do not depend on the VGA device — but the *driver's* own Direct3D (the M7c HAL) goes with the driver |
| Audio | **AC'97 (the default) or the SB16 + its OPL3, or none** (`bundle::Sound`, doc 20) | unchanged as a default: XP's in-box AC'97 driver. The SB16 is there for a DOS-era title that wants one |
| Music | **nothing by default**; an MPU-401 with General MIDI or a CM-32L on request (`bundle::Music`, doc 20) | the one family that ships a wavetable synthesizer with the operating system, so a port it would not use by default is hardware for nothing. A real MT-32 for an old game is one pick away |
| Net | RTL8139, **off on a new machine** | in-box XP driver; the checkbox gives it, and a new machine starts without one (doc 07) |
| Storage | IDE + our ATAPI CD | AHCI needs F6 drivers; not worth it |
| Input | PS/2 + USB tablet toggle | same grab semantics as 98 (`seamless_mouse`, on by default) |
| Gamepad | **off on a new machine**; a USB HID pad or the key mapping (`bundle::Pad`, M13) | XP binds `hidusb.sys` to the pad on the first start after it is added and shows it to DirectInput and `joy.cpl`, with nothing to install (user-confirmed 2026-09-09). No gameport offered: nothing enumerates a non-PnP port here, and Microsoft was already retiring analog sticks |

Notes: SP3 recommended; activation is the user's affair with their own
license (volume/retail as they possess) — the project ships nothing related
to it. TSC/HAL: uniprocessor ACPI HAL default; SMP under TCG is a measured
decision later (MTTCG helps, XP-era games rarely do).

## DOS machine (added 2026-09-06)

Modeled as a ~1994 PC: the same i440FX board, the SB16 the Win98 row
already carries "for DOS boxes/games", and nothing else.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX + PIIX) | same board as the other two; DOS cares about the ISA devices, not the chipset |
| CPU model | `pentium3` | deliberately *not* changed: what makes a machine feel like a 486 is the rate, not the CPUID string, and one variable at a time. Revisit if a real title is found that dislikes the model |
| **CPU rate** | **`cpu_speed`, default 486DX2-66** | the field that makes this a DOS machine at all — see below |
| RAM | 64 MB (4–256) | DOS uses the first megabyte; the rest is XMS for a mid-90s extender. 64 MB is generous for the era and inside what MS-DOS 6.22's own HIMEM.SYS manages |
| Video | **The standard VGA (`-vga std`, the default) or the Cirrus GD5446 (`-vga cirrus`)** — a choice since 2026-09-09 (`bundle::Video`) | the Bochs adapter's VBE 2.0 and linear frame buffer are the fuller of the two VESA BIOSes a DOS title can find, and this family's default (2026-09-09, user decision: it is what a DOS machine was meant to have, where the hardcoded line it replaced said `cirrus`). The one family where the adapter is **not** a driver question: a DOS title programs the registers itself, so what changes is *which VESA BIOS it finds*. The Cirrus is the other half of an A/B nothing else can settle — it was an open question until a game rendered wrongly on it and there was no way to change the adapter at all. Nothing is installed either way. `d3dpt-vga` is not offered: there is no DOS driver for it |
| Audio | **SB16 + its OPL3 (the default), a Gravis Ultrasound, an AdLib alone, or none** (`bundle::Sound`, doc 20) | the SB16 is what DOS software knows how to talk to, and its `BLASTER=A220 I5 D1 H5 P330 T6` names the MIDI port as well. The Gravis is the card the games written for one sound best on; the bare AdLib is the 1990 machine |
| Music | **an MPU-401 at 0x330 with a General MIDI synthesizer (the default), a Roland CM-32L, or nothing** (`bundle::Music`, doc 20) | what a game's setup screen means by "General MIDI", "MPU-401" or "Roland". Nothing else on a DOS machine plays a score: the FM chip is the fallback, not the point |
| Net | none | DOS reaches a network only through a packet driver the user installs by hand; an unused card is one more device to enumerate. The one family that has never had one by default — since 2026-09-07 the others start without one too, for a different reason (doc 07) |
| Input | PS/2 mouse + kbd, **no USB tablet** (`seamless_mouse = false`) | a DOS mouse driver talks to the PS/2 controller; a tablet would leave the guest with no pointer at all. The player takes the pointer on a click and Ctrl+Alt+G gives it back |
| Gamepad | **off on a new machine**; the gameport at 0x201 or the key mapping (`bundle::Pad`, M13, patch 27) | the one family with no USB stack, so the port is the only controller it can have — and it is the one a DOS game knows how to read, by arming four one-shots and counting until each bit falls. That count depends on how fast the guest runs, which is the other reason this family is paced (`-icount …,align=on`): unpaced, an axis nobody is touching wanders by half |
| Storage | IDE HDD + our ATAPI CD | the CD-ROM model (doc 17) and the disc shelf both already speak DOS: `CDSHELF.COM` is a DOS program |
| Floppy | `floppy` + `boot` on the machine | a DOS machine usually boots from one |

**No 3D of any kind.** The Glide wrapper for DOS is `GLIDE2X.OVL`, which
needs Open Watcom and which we do not build; OpenGL and Direct3D
pass-through are Windows DLLs. A DOS machine is 2D, the CRT shader chain
and the real CD-ROM model — which is a coherent story, and a much smaller
one than the Windows families'.

### Why a rate control, and what it costs

Software of the era calibrates a delay loop against the CPU it finds and
then trusts the answer forever, so on a fast machine it does not merely
run quickly — it runs *wrong*. Our TCG runs a DOS guest at ~660 M
instructions/s on the Linux box, which is Pentium III territory; KVM is
far beyond that.

QEMU's only rate control is `-icount`, whose `shift` gives one
instruction per 2^shift ns, so the offered processors are powers of two
by construction (`bundle::CpuSpeed`). Two consequences, both surfaced in
the wizard rather than discovered later:

- **`align=on` is the whole thing.** `-icount shift=N` alone only makes
  the guest's clock a function of instructions retired — the guest
  believes it is slow while the host runs it as fast as it likes.
  Measured 2026-09-06: a run meant to be throttled finished in *less*
  wall-clock time than the unthrottled one, because the guest's idle
  waits collapse with it.
- **A throttled machine is emulated.** `-icount` and KVM cannot coexist,
  so `effective_accel()` returns TCG whenever a processor is chosen.

Measured in a real FreeDOS guest, 200 M instructions of loop
(`tools/dos-guest-test.py`, Linux box, 2026-09-06):

| `cpu_speed` | asks for | measured | loop |
|---|---|---|---|
| `unthrottled` | — | 663 M/s | 0.30 s |
| `pentium-133` | 125 M/s | (a ceiling, see below) | |
| `486dx2-66` | 31.25 M/s | **31.3 M/s** | 6.39 s |
| `386dx-33` | 7.8 M/s | **7.8 M/s** | 25.63 s |

The cap is exact where it matters. Above ~30 M/s the alignment only
corrects a guest that has fallen *behind*, so the fast settings are a
ceiling the host may overshoot — which is why the era settings a 1993
game wants are the accurate ones. Boot time barely moves (2.4 → 3.5 s):
booting is mostly waiting, and waiting is not instructions.

## Other machine (added 2026-09-07)

For an era OS that is neither Windows nor DOS. Nothing here is tested
against a specific guest — that is the point of the family — so every
choice is the one with the widest chance of having had a driver in the
box on a nineties system, and nothing of ours is on the machine at all.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX + PIIX) | the same board as the other three |
| CPU model | `pentium3` | as everywhere else; no reason for this family to differ |
| CPU rate | unthrottled | these are OSes that read the clock, not DOS software counting a delay loop |
| RAM | 512 MB (16–3072) | no reference machine to inherit from, so the range is the machine's own limits: a 1995 kernel at the bottom, XP's 32-bit ceiling at the top. BeOS R5 is the one guest with a lower limit of its own (1 GB), which the wizard *says* above that rather than enforces |
| Video | **`-vga std` or `-vga cirrus`, chosen in the wizard** (default std) | the standard VGA is the Bochs adapter with VBE 2.0 and a linear frame buffer — what a period VESA driver wants, what a modern Linux binds `bochs-drm` to, and the one a guest with no native driver can always fall back on. The Cirrus is a chip that really existed, so an era guest is likelier to have a *native* driver for it (BeOS R5 and XFree86 both ship one). **Neither is `d3dpt-vga`**: our adapter needs our display driver, which exists for Windows only (docs 15, 19), so a BeOS or Linux guest on it would have no display at all |
| Audio | **ES1370** (Ensoniq AudioPCI) — the default; the AC'97 or nothing are the other picks (`bundle::Sound`, doc 20) | the PCI sound card of the period both these guests drive in the box — BeOS ships an `ensoniq` add-on, Linux has `snd-ens1370` — where AC'97 needs a driver an era install may not have. No FM chip on either, and no MIDI port by default: this is the family we add no drivers to |
| Net | RTL8139, **off on a new machine** | in-box on BeOS R5 and on Linux since 2.2 (`8139too`); the checkbox gives it, and a new machine starts without one (doc 07) |
| Storage | IDE HDD + our ATAPI CD | as everywhere; the CD-ROM model (doc 17) is a drive, not a driver |
| Input | PS/2 mouse + kbd, **no USB tablet** (`seamless_mouse = false`) | an absolute pointer needs the guest's USB HID stack *and* its windowing system to agree it is absolute, which an era XFree86 (an explicit input section) and BeOS do not do unconfigured — and unlike the Windows families there is no guest-tools install that would fix it. The checkbox turns it on for a guest that does handle it |
| Gamepad | **off on a new machine**; a USB HID pad or the key mapping (`bundle::Pad`, M13) | the pad is a HID device an era Linux or BeOS drives from its own USB stack. No gameport: it would work on a guest whose driver can be told an address, and this project cannot name that step for an OS it does not know |
| Acceleration | Automatic | none of these has Win9x's fast-CPU bugs, and nothing here is tuned for them either: take the host's speed when it is there |

**No 3D of any kind, and no guest tools.** The Direct3D pass-through
(doc 14), the Glide wrapper's guest half (doc 12 §5) and the display
driver (docs 15, 19) are all Windows components; `SETUP.EXE` on the
guest-tools ISO is a Win32 console program. This family is 2D, the CRT
shader chain and the real CD-ROM model — the same story the DOS family
has, on a guest modern enough to want PCI cards.

The PCI addresses are pinned (`rtl8139` at `0x03`, `ES1370` at `0x04`,
with the adapter taking `0x02` — measured, and the same for `-vga std`,
`-vga cirrus` and `d3dpt-vga` alike) for the reason the Windows families
pin theirs: turning networking off, or changing the adapter, would
otherwise slide the sound card up into the NIC's slot, and a card that
moves is a hardware change an installed guest re-detects. The
`family-other` check in `scripts/test.sh` holds all of this — the
standard VGA, the absent card, the sound card at its own address, the
RTL8139 arriving at `0x03` when the box is ticked, the absent tablet and
the sound card staying put when the NIC goes again — and ends by having
our own `qemu-system-i386` accept the line.

## The display adapter (added 2026-09-07)

All four families offer a choice of adapter, `bundle::Video`, written
into the bundle as `video` (DOS since 2026-09-09). The list is per
family, and **the first entry is that family's default**
(`bundle::video_choices`):

| Family | Offers | Default |
|---|---|---|
| XP | `d3dpt` (our adapter + our driver) / `cirrus` (Windows' in-box driver) | `d3dpt` |
| Win98 | `cirrus` / `d3dpt` | `cirrus` |
| Other | `std` (Bochs VGA, VBE 2.0) / `cirrus` | `std` |
| DOS | `std` (Bochs VGA, VBE 2.0) / `cirrus` (period VESA BIOS) | `std` |

The choice exists because there are two honest answers and nothing here
can pick between them. On Windows, ours is what the whole display path is
built on — the mode table, the desktop straight from VRAM, the page flips
that pace a game (doc 15's flip chain), the Direct3D DDI — and the Cirrus
is the right answer for a machine whose driver is not installed yet, for
A/B'ing a title that misbehaves on ours, and for the test tools that
still exercise the in-box driver. On `Other` there is no driver of ours
at all and only the person installing the guest knows which standard
adapter it has a driver for.

DOS is the exception to all of that, and it was the last family to get
the picker (2026-09-09) because it looked like it needed nothing to
pick between: a DOS title asks no operating system for a driver, it
programs the adapter itself. What it *does* ask is the VESA BIOS, and
the two are not the same BIOS — the Cirrus's is of the period, the Bochs
adapter's is VBE 2.0 with a linear frame buffer. When a game draws
wrongly in a mode, which of the two it found is a variable, and until
this there was no way to change it short of editing the bundle by hand.
So the DOS row is one pick with no consequences either side: nothing is
installed for a DOS adapter, and the machine boots the same on both.

Its default is the **standard VGA** (2026-09-09, user decision), which is
also what a DOS machine was meant to have all along: the family shipped
with `-vga cirrus` hardcoded into its arguments from the day it landed
(`8a0cfce`), which was the slip this row corrects. The Cirrus belongs to
`Other`, where a guest wants a chip a *native* driver was written for.
Existing DOS machines carry no `video` field and so move to the standard
VGA on their next start; nothing is installed for them to lose, though a
game that has been through its own setup may want that run again.

The two Windows families therefore **start at opposite ends of the same
pair**. XP starts on ours: the driver has been the whole display path
there since 2026-09-04 and every game the M4 and M7 tracks were built on
runs through it. Win98 starts on the Cirrus (2026-09-07, user decision):
ours runs there too (doc 19, M10) and is one pick away, but that driver
is a day old against XP's, so a machine the wizard makes comes up on the
driver Windows already has in the box.

Two rules make the field safe to hand-write:

- **An adapter a family does not offer falls back to that family's
  default** (`Machine::effective_video`), and the wizard refuses it
  outright (`Form::choose_video`). `video = "std"` on an XP machine would
  otherwise leave the guest with no display driver at all — there is none
  for the Bochs adapter on XP — which is not something a stray bundle
  field should be able to do.
- **Every adapter lands at PCI `0x02`** — measured, `-vga std`,
  `-vga cirrus` and `d3dpt-vga` alike — so the cards pinned below it do
  not move when it changes under an installed guest.

On the three families that have drivers, changing it *is* a hardware
change to a guest that is already installed: it finds an unknown adapter,
comes up in plain VGA and wants a driver before the desktop is back. The
wizard says so, in orange, but only while editing a machine whose adapter
has actually been changed (`Form::video_warning`). **DOS is told
something else there**, because that sentence is not true of it: the
machine simply boots, and the only thing that can be stale is a *game's*
own setup, which may have written down a video mode the other adapter
does not offer.

The `display-adapter` check in `scripts/test.sh` holds the whole table:
each family's default, the switch away from it and back (a different
direction on each Windows family), our adapter being *gone* rather than
sitting beside it, the NIC staying at `0x03`, the standard VGA refused on
Windows, our own adapter refused on DOS, and our own
`qemu-system-i386` accepting every combination. The **family switch**
itself — every untouched field moving to the new family's default, a
picked one surviving unless the new family has no such entry, and
"Default" putting a field back to following the family — is the
`capi` check, which holds a live form rather than a saved bundle.

## Performance expectations (set honestly in-app)

| Host | Win98 | XP |
|---|---|---|
| Linux/Windows x86 (KVM/WHPX) | vastly faster than period hardware | near-native |
| Apple Silicon (TCG) | comfortably faster than a period PC | usable; vs. the rig's P4 1.7 (M1 Air, `reference/benchmarks/`): boots as fast, integer 1.3–2× faster (7-Zip 0.996/1.511 vs 0.742/0.776 GIPS). x87 FP reached 104 % of the real P4 with patch 06 (Super PI 1M 1:57 vs 2:02; 1:25.3 with patch 14 W^X tracking; down from 9:49 unpatched). SSE/SIMD inline TCG (patches 11/12) matches or exceeds the real P4 on key kernels. State both in-app. |

The XP-on-Apple-Silicon row was the major architectural risk in the guest story;
it was benchmarked in milestone M1 and optimized through M8/M9. Baselines come
from the reference rig (P4 + GeForce 6200, doc 09): expectations are stated as
a percentage of that real machine's benchmark scores, not adjectives.

## Snapshots and storage

- qcow2 with named snapshots ("fresh install", "drivers installed",
  "pre-game-X") surfaced in the frontend — the retro workflow is
  reinstall-heavy and snapshots are the killer convenience.
- Machine definitions are declarative files (TOML/JSON) in the machine
  library; the player process translates to QEMU config. No user-visible
  QEMU command lines anywhere.

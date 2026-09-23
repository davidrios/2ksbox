# 6. Guest machines: the four families

The launcher ships four machine families with tested defaults; users
supply their own OS media and licences. Win98, XP and DOS are the
machines this project is built around; **Other** is the catch-all for an
era OS that is none of those (BeOS, a period Linux, OS/2) — standard
hardware and nothing of ours. The defaults live in
`launcher-core/src/bundle.rs` (`video_choices`, `sound_choices`,
`music_choices`, `pad_choices`, `default_*`): every choice list's first
entry is the family's default. The machine form is doc 07; the sound and
music devices doc 20; the display drivers docs 15 and 19; the Voodoo 2
doc 21.

**Common to all four:** i440FX + PIIX (`-machine pc`), `-cpu pentium3`
(with the form's optimization switches as its properties), IDE disk
(qcow2) and our ATAPI CD (doc 17), **no network card on a new machine**
(nor on a bundle that does not say: an unpatched guest is not put on a
network before anyone asks; `-nic none`, since QEMU otherwise adds an
e1000 of its own), **no gamepad** until one is picked (`bundle::Pad`,
M13), and a Voodoo 2 checkbox, off (`voodoo2,addr=0x05`). Every display
adapter lands at PCI `0x02` and the NIC and sound card are pinned below it
(`0x03`, `0x04`), so turning networking off or changing the adapter never
slides a card into another's slot — a card that moves is a hardware
change an installed guest re-detects.

## Windows 98 SE

Modeled as a ~1998–2000 consumer PC.

| Component | Default (alternatives) | Why |
|---|---|---|
| Machine | `pc,hpet=off` | 98 has no driver for an HPET (`PNP0103`) and never uses one; it showed as an Unknown Device (the `hpet` check). fw_cfg (`QEMU0002`) has no driver either, but its `_STA` hides it |
| Accel | TCG | under KVM `~/vms/win98.qcow2` loses Explorer at startup (*SHELL32.DLL is linked to missing export SHLWAPI.DLL:GetFileAttributesA*), so there is no Start menu |
| CPU | `pentium3` | avoids CPUID features and fast-CPU bugs 9x mishandles; the floor, since our guest wrappers are built `-march=pentium3` |
| RAM | 256 MB (32–512) | 9x VCACHE sizing overflows much above 512 MB |
| Video | `d3dpt-vga` + our driver (`cirrus`) | ours is the whole display path (doc 19): mode table, desktop from VRAM, paced page flips, Direct3D through the driver. Default since 2026-09-16 (user decision); the Cirrus is Windows' in-box driver and the A/B |
| Sound | SB16 + OPL3 (AC'97, Gravis Ultrasound, none) | the SB16 is in the box and what a DOS box inside 98 expects. The AC'97 needs a driver 98 lacks, not yet on the guest-tools ISO; the Gravis needs Gravis's own |
| Music | MPU-401 at 0x330, General MIDI (CM-32L, none) | 98 has no wavetable synth; its MIDI output is the FM chip. Windows finds the port after "MPU-401 Compatible" is added in Add New Hardware |
| Net | PCnet when on | in-box driver |
| Pointer | USB tablet (`seamless_mouse`) | absolute, nothing grabbed; off leaves PS/2 relative mode for mouselook (doc 03) |
| Gamepad | none (USB HID pad, gameport, key mapping) | the one family offered both devices (below) |
| Floppy | enabled | drivers, utilities, boot disks |

**Gamepads.** 98 SE binds its in-box HID driver to the USB pad (it asks
for the Windows 98 source files the first time), and a Windows game gets
it through DirectInput **and** winmm's `joyGetPosEx` over VJOYD — measured
by the `pad-guest-98` check — so "Standard Game Port" buys a Windows game
nothing and M13 does not install it. The gameport at 0x201 is for a DOS
box under 98 and 1995 titles; it is not Plug and Play (Add New Hardware,
then calibrate), which the form says.

**QEMU-side traps** (`patches/qemu/README.md`): 9.2.4's TCG needs the
upstream LSS fix (patch 01, issue 2987) or 98 faults with exception 0D
on first boot; TCG also faults RUNDLL32 in Display Properties since 7.2
(issue 1964, cosmetic, the OS survives; KVM/WHPX unaffected); qemu-3dfx's
3D needs a context provider, which the player registers and a bare
`qemu-system-i386` does not (doc 12).

**Guest setup:** install from the user's CD image, then the guest-tools
ISO (`SETUP.EXE`, `guest-tools/README.md`). Known quirk to document:
DOS-compatibility-mode storage regressions.

### The install must come out ACPI

A PnP-BIOS install leaves the PCI bus un-enumerated — "Plug and Play
BIOS" with a yellow ! and no PCI hot-add ever seen (no USB tablet, AC'97
or NIC). Setup's `DetectACPIBIOS` (`sysdetmg.dll`, PRECOPY1.CAB) decides
from the **legacy BIOS date at F000:FFF5** against the `ACPICheckDate`
its own `machine.inf` writes:

```
; machine.inf, [BaseWinOptions] → ACPI_BASE
HKLM,Software\Microsoft\Windows\CurrentVersion\Detect,ACPICheckDate,,"12/01/99"
```

A BIOS at least that new is believed; an older one only if it matches
`BIOSINFO.INF`'s `[GoodACPIBios]` (four 1998 machines, by ACPI OEM id).
SeaBIOS says **06/23/99** and QEMU's tables say `BOCHS `/`BXPC`, so setup
fell back to PnP-BIOS. (`BadACPIBios` never matched us, and the `ASL
Compiler version < 1.0` gate applies only to `MSFT`-created tables.)

`SETUP /p j` forces ACPI, but a launcher cannot type that at a DOS
prompt, so **`scripts/prepare-qemu.sh` stamps every `pc-bios/bios*.bin`
to `12/31/99`** (eight ASCII bytes ending three from the end of each
image; 1999 because the comparison is on a two-digit year). Nothing else
reads the field — `BIOSINFO.INF`'s `date=` quirks all want exact 1994–96
days, and the guest's clock comes from the RTC. The `bios-date` check
asks a running QEMU what a guest reads there. A plain `SETUP` from the
98 SE CD comes out ACPI (user-confirmed 2026-09-07). An image installed
before the stamp is repaired through Device Manager (PnP-BIOS → PCI Bus,
`docs/build-macos.md`), not reinstalled.

Kept in reserve: `-machine pc,x-oem-id=COMPAQ,x-oem-table-id="CAPONE  "`
matches `[CompaqCapone]` (its rules want FACP OEM revision ≥ 1 and RSDT
creator revision ≥ 0, both hardcoded to 1 in `hw/acpi/aml-build.c`) with
no firmware change, at the price of a vendor's name on every table.

## Windows XP

Modeled as a ~2002–2005 PC.

| Component | Default (alternatives) | Why |
|---|---|---|
| Machine | `pc` | best compatibility with XP-era drivers; q35 unnecessary |
| Accel | Automatic (KVM or WHPX with TCG behind it; TCG on macOS) | prefer an era CPU model for games: KVM `-cpu host` breaks Max Payne's level loading |
| RAM | 512 MB (64–3072) | the top is the practical 32-bit limit, below where PCI space eats RAM |
| Video | `d3dpt-vga` + our driver (`cirrus`) | doc 15: the host's mode table (640×480…1600×1200, 16/32 bpp, 60/75/85 Hz), desktop from VRAM, Direct3D 7/8 through the driver. Before the driver is installed it is plain VGA (vga.sys, 800×600×4). The Cirrus is XP's in-box 2D driver (up to 1024×768×16). The standard VGA has **no** XP driver and is not offered |
| Sound | AC'97 (SB16 + OPL3, none) | XP's in-box AC'97 driver; the SB16 for a DOS-era title |
| Music | none (General MIDI, CM-32L) | XP ships a wavetable synth; a port it would not use is hardware for nothing |
| Net | RTL8139 when on | in-box driver |
| Pointer | USB tablet (`seamless_mouse`) | as on 98 |
| Gamepad | none (USB HID pad, key mapping) | XP binds `hidusb.sys` on the first start and shows the pad to DirectInput and `joy.cpl`, nothing to install. No gameport: nothing enumerates a non-PnP port here |

SP3 recommended; activation is the user's affair with their own licence.
Uniprocessor ACPI HAL; SMP under TCG is a measured decision for later.

## DOS

Modeled as a ~1994 PC: the same board, a Sound Blaster, and nothing else.

| Component | Default (alternatives) | Why |
|---|---|---|
| CPU model | `pentium3` | deliberately unchanged: what makes a machine feel like a 486 is the rate, not the CPUID string |
| **CPU rate** | **486DX2-66** (`cpu_speed`) | what makes this a DOS machine at all (below) |
| Accel | TCG | forced: `-icount` and KVM cannot coexist |
| RAM | 64 MB (4–256) | the rest of the first megabyte is XMS for a mid-90s extender; inside what MS-DOS 6.22's HIMEM.SYS manages |
| Video | `std` (`cirrus`) | not a driver question: a DOS title programs the adapter itself, so what changes is **which VESA BIOS it finds** — the Bochs adapter's VBE 2.0 with a linear frame buffer (the default since 2026-09-09, user decision) or the Cirrus's of the period. The one A/B for a game whose modes come out wrong. `d3dpt-vga` is not offered: there is no DOS driver for it |
| Sound | SB16 + OPL3 (Gravis Ultrasound, AdLib alone, none) | the SB16 is what DOS software talks to (`BLASTER=A220 I5 D1 H5 P330 T6` names the MIDI port too); the Gravis for the games written for one; the bare AdLib is the 1990 machine |
| Music | MPU-401 at 0x330, General MIDI (CM-32L, none) | what a setup screen means by "General MIDI", "MPU-401" or "Roland" |
| Net | none | DOS networks only through a packet driver installed by hand |
| Pointer | PS/2 only (`seamless_mouse = false`) | a DOS mouse driver talks to the PS/2 controller; with a tablet it would find nothing. A click grabs, Ctrl+Alt+G releases |
| Gamepad | none (gameport at 0x201, key mapping) | no USB stack. A DOS game reads the port by arming four one-shots and counting, so the count depends on the guest's speed — the other reason this family is paced: unpaced, an untouched axis wanders by half |
| Boot | floppy + `boot` on the machine | a DOS machine usually boots from one |

**3D.** No OpenGL or Direct3D (those are Windows DLLs). A Glide 2 game can
run through qemu-3dfx's `GLIDE2X.OVL` put next to it (doc 12 §5; nothing
has tried it on this family yet), and the Voodoo 2 checkbox gives a DOS
game a real card for its own Glide (doc 21).

### Why a rate control, and what it costs

Software of the era calibrates a delay loop against the CPU it finds and
trusts the answer forever, so on a fast machine it runs *wrong*, not just
quickly. Our TCG runs a DOS guest at ~660 M instructions/s on the Linux
box — Pentium III territory; KVM is far beyond.

QEMU's only rate control is `-icount`, whose `shift` gives one
instruction per 2^shift ns, so the offered processors are powers of two
(`bundle::CpuSpeed`).

- **`align=on` is the whole thing.** `-icount shift=N` alone only makes
  the guest's clock a function of instructions retired: the guest
  believes it is slow while the host runs it as fast as it likes. A run
  meant to be throttled finished in *less* wall time than the unthrottled
  one, because the guest's idle waits collapsed too. The machine line is
  `-icount shift=N,align=on`.
- **A throttled machine is emulated.** `effective_accel()` returns TCG
  whenever a processor is chosen.

Measured in FreeDOS, 200 M instructions of loop (`tools/dos-guest-test.py`):

| `cpu_speed` | asks for | measured | loop |
|---|---|---|---|
| `unthrottled` | — | 663 M/s | 0.30 s |
| `pentium-133` | 125 M/s | a ceiling (below) | |
| `486dx2-66` | 31.25 M/s | **31.3 M/s** | 6.39 s |
| `386dx-33` | 7.8 M/s | **7.8 M/s** | 25.63 s |

Above ~30 M/s alignment only corrects a guest that has fallen *behind*,
so the fast settings are a ceiling the host may overshoot; the era
settings a 1993 game wants are the accurate ones. Boot time barely moves
(2.4 → 3.5 s): booting is mostly waiting.

### DOS games in a Win98 DOS box: the timer is Windows' business

A DOS game whose clock misbehaves only in a Win98 DOS box is not chased
(user decision, 2026-09-11): DOS games run on pure DOS — the DOS family,
or 98's "Restart in MS-DOS mode" — where `TESTS\QCLOCK.COM` reads 100 %
since patch 34. Inside a DOS box about 17 of 18 ticks still read one
period backward: the IRQ reaches the machine on time, so the lag is
between VTD/VPICD taking the interrupt and the VM's reflected INT 8
updating 0040:006C, while VTD's trapped counter reads are already
current. DOS Quake there runs steadily fast. (Before patch 34 the DOS box
got about 12 ticks a second and Windows repaid the shortfall in bursts of
up to 153 ticks — the "momentary speed-up".) Left untried on purpose:
measuring the VM's tick lag, `[386Enh] TrapTimerPorts=Off`, and the rate
VTD programs the PIT to.

## Other

For an era OS that is neither Windows nor DOS. Nothing is tested against
a specific guest — that is the point — so every choice is the one likeliest
to have had an in-box driver on a nineties system.

| Component | Default (alternatives) | Why |
|---|---|---|
| CPU rate | unthrottled | these OSes read the clock rather than count a delay loop |
| Accel | Automatic | none has Win9x's fast-CPU bugs |
| RAM | 512 MB (16–3072) | the machine's own limits; BeOS R5's lower 1 GB ceiling is said by the form, not enforced |
| Video | `std` (`cirrus`) | the Bochs VGA with VBE 2.0 and a linear frame buffer: what a period VESA driver wants and modern Linux's `bochs-drm` binds. The Cirrus is a real chip, so an era guest may have a *native* driver (BeOS R5, XFree86). Not `d3dpt-vga`: its driver is Windows-only |
| Sound | ES1370 (AC'97, none) | the period PCI card both BeOS (`ensoniq`) and Linux (`snd-ens1370`) drive in the box; no FM chip, no MIDI port |
| Net | RTL8139 when on | in-box on BeOS R5 and Linux since 2.2 (`8139too`) |
| Pointer | PS/2 only | an absolute USB pointer needs the guest's HID stack *and* its windowing system to agree, which an era XFree86 and BeOS do not unconfigured, and no guest-tools install can fix it; the checkbox turns it on |
| Gamepad | none (USB HID pad, key mapping) | driven by the guest's own USB stack; no gameport, whose address this project cannot tell an unknown OS |

No 3D and no guest tools: the Direct3D pass-through, the Glide guest half
and our display drivers are Windows components, and `SETUP.EXE` is a Win32
console program. The `family-other` check holds this family's machine line.

## The display adapter

All four families offer a choice (`bundle::Video`, the bundle's `video`):

| Family | Offers | Default |
|---|---|---|
| XP | `d3dpt` (our adapter + driver) / `cirrus` (in-box driver) | `d3dpt` |
| Win98 | `d3dpt` / `cirrus` | `d3dpt` (since 2026-09-16) |
| Other | `std` (Bochs VGA, VBE 2.0) / `cirrus` | `std` |
| DOS | `std` / `cirrus` (period VESA BIOS) | `std` (since 2026-09-09) |

On Windows ours is what the whole display path is built on; the Cirrus is
for a machine whose driver is not installed yet, for A/B-ing a title that
misbehaves on ours, and for the test tools that exercise the in-box
driver. On Other only the person installing knows which standard adapter
the guest has a driver for.

- **An adapter a family does not offer falls back to its default**
  (`Machine::effective_video`) and the form refuses it
  (`Form::choose_video`): `video = "std"` on XP would leave the guest with
  no display driver at all.
- **Changing it under an installed Windows or Other guest is a hardware
  change**: an unknown adapter, plain VGA, a driver wanted before the
  desktop is back. The form says so in orange, only when the adapter was
  actually changed (`Form::video_warning`). **DOS is told something
  else**: the machine just boots, and only a game's own setup, which may
  have recorded a mode the other adapter lacks, can be stale. A DOS
  bundle with no `video` field (written before the picker, when DOS was
  hardcoded to the Cirrus) moves to `std` on its next start.
- **`d3d9 = "auto" | "dxvk" | "system"`** rides on our adapter and is
  about the *host*: the Direct3D 9 library the executor runs on (ADR-007's
  second amendment, doc 14). `auto` (the default, and what an older bundle
  means) is DXVK, or on a Windows host below DXVK's Vulkan 1.3 floor that
  host's own d3d9, resolved by the launcher's Vulkan probe. It is read
  only where the machine has our adapter, kept when the machine moves off
  it, and reaches QEMU as `-device d3dpt-vga,d3d9=…` (the `d3d9` check).

The `display-adapter` check holds the table (defaults, switches both
ways, our adapter *gone* rather than beside the Cirrus, the NIC at
`0x03`, refusals, our QEMU accepting every line); the `capi` check holds
the family switch (untouched fields follow the new family's default, a
picked one survives unless the new family lacks it, "Default" puts a
field back to following the family). `docs/testing.md` lists both.

## Performance expectations (stated honestly in-app)

| Host | Win98 | XP |
|---|---|---|
| Linux/Windows x86 (KVM/WHPX) | vastly faster than period hardware | near-native |
| Apple Silicon (TCG) | comfortably faster than a period PC | usable: against the rig's P4 1.7 (M1 Air, `reference/benchmarks/`) it boots as fast, integer is 1.3–2× faster (7-Zip 0.996/1.511 vs 0.742/0.776 GIPS), x87 reaches the P4 with patch 06 (Super PI 1M 1:57 vs 2:02; 1:25.3 with patch 14; 9:49 unpatched), and SSE/SIMD inline TCG (patches 11/12) matches or beats it on key kernels |

Expectations are percentages of the reference rig's (doc 09) benchmark
scores, not adjectives. Doc 22 has the full evaluation.

## Snapshots and storage

- qcow2 with named snapshots ("fresh install", "drivers installed",
  "pre-game-X") surfaced in the launcher — the retro workflow is
  reinstall-heavy.
- Machines are declarative bundles (`machine.toml`) in the library; the
  launcher turns them into QEMU arguments (`launcherx --print-args`). No
  user-visible QEMU command line, except the form's "Extra QEMU
  arguments" field for testing.

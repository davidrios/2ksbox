# 6. Guest machines: Win98 and XP reference configs

The frontend ships two "machine families" with tested defaults. These are the
reference definitions the guided creation flow instantiates; users supply
their own OS media and licenses.

## Windows 98 SE machine

Modeled as a ~1998–2000 consumer PC.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX + PIIX) | period-correct chipset, best-tested with 9x |
| CPU model | `pentium3` (TCG) / host-masked (KVM) | avoids CPUID features 9x mishandles; sidesteps the fast-CPU Win9x bugs (e.g. the >2.1 GHz-class IOS/NDIS crashes). **Floor is pentium3 (SSE1)**: our guest-tools wrappers are built `-march=pentium3` (upstream builds them x86-64-v2 and expects `-cpu host`/`max`) |
| RAM | 256 MB default, **≤ 512 MB hard cap** | 9x VCache breaks above ~512 MB without patches |
| Video | QEMU std VGA (bochs) | SoftGPU's primary target; clean mode behavior for our pipeline |
| Audio | SB16 (DOS-mode compat) + AC'97 | SB16 for DOS boxes/games, AC'97 driver in guest tools |
| Net | PCnet (AMD) | driver in-box on 98 |
| Storage | IDE HDD (qcow2) + our ATAPI CD | period-correct; no VirtIO for 9x |
| Input | PS/2 mouse + kbd; USB tablet optional | the bundle's `seamless_mouse` (doc 07), on by default here: the tablet is absolute, so nothing is grabbed; off leaves the PS/2 relative mode games want (see doc 03) |
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
| Video | **XP: `-vga none -device d3dpt-vga` + our driver (doc 15, since 2026-09-04)**; Win98: Cirrus GD5446 (`-vga cirrus`) + qemu-3dfx | d3dpt-vga: the host's mode table (640×480…1600×1200, 16/32 bpp, 60/75/85 Hz), desktop straight from VRAM; before the driver is installed it is a standard VGA (vga.sys, 800×600×4). Cirrus: XP inbox driver for 2D (up to 1024×768×16 / 800×600×24; std VGA has **no** XP driver). Wrappers for 3D do not depend on the VGA device |
| Audio | AC'97 (fallback: emulated HDA) | XP AC'97 driver in guest tools |
| Net | RTL8139 | in-box XP driver |
| Storage | IDE + our ATAPI CD | AHCI needs F6 drivers; not worth it |
| Input | PS/2 + USB tablet toggle | same grab semantics as 98 (`seamless_mouse`, on by default) |

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
| Video | Cirrus GD5446 (`-vga cirrus`) | a real VGA/VESA BIOS of the period. `-vga std`'s Bochs VBE 2.0 with a linear framebuffer is arguably better for late VESA titles — an open question, not a decision |
| Audio | SB16 | what DOS software knows how to talk to |
| Net | none | DOS reaches a network only through a packet driver the user installs by hand; an unused card is one more device to enumerate |
| Input | PS/2 mouse + kbd, **no USB tablet** (`seamless_mouse = false`) | a DOS mouse driver talks to the PS/2 controller; a tablet would leave the guest with no pointer at all. The player takes the pointer on a click and Ctrl+Alt+G gives it back |
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

## Performance expectations (set honestly in-app)

| Host | Win98 | XP |
|---|---|---|
| Linux/Windows x86 (KVM/WHPX) | vastly faster than period hardware | near-native |
| Apple Silicon (TCG) | comfortably faster than a period PC | usable; vs. the rig's P4 1.7 (M1 Air, 2026-09-02, `reference/benchmarks/`): boots as fast, integer 1.3–2× faster (7-Zip 0.996/1.511 vs 0.742/0.776 GIPS), x87 FP at 31 % with patch 05 (Super PI 1M 6:33 vs 2:02; 9:49 = 21 % before it — Pentium II→III class). State both in-app. |

The XP-on-Apple-Silicon row is the one architectural risk in the guest story;
it gets benchmarked in milestone M1, not discovered in M4. Baselines come from
the reference rig (P4 + GeForce 6200, doc 09): expectations are stated as a
percentage of that real machine's benchmark scores, not adjectives.

## Snapshots and storage

- qcow2 with named snapshots ("fresh install", "drivers installed",
  "pre-game-X") surfaced in the frontend — the retro workflow is
  reinstall-heavy and snapshots are the killer convenience.
- Machine definitions are declarative files (TOML/JSON) in the machine
  library; the player process translates to QEMU config. No user-visible
  QEMU command lines anywhere.

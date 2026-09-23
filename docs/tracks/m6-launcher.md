# Track: M6 — the launcher

The record of the launcher track: the machine library, the settings
form, the disc shelf, snapshots, shader profiles and the packages. M6
itself is done (doc 08); the launcher keeps changing, and this is where a
session working on it starts. **The design lives in doc 07** (player vs.
launcher, the core/front-end split, every form field and why, the
install layout, the Qt traps); the decisions are ADR-009 (licence),
ADR-011 (the names), ADR-014 (one core), ADR-015 (Qt ships) and ADR-017
(egui deleted) in doc 10. Every tool below is described in
`docs/testing.md`.

## Scope and files

| Owned | What it is |
|---|---|
| `launcher-core/` | everything the launcher decides: the bundle (`bundle.rs`, `machine.toml`), library, disc shelf, shader profiles and sources, paths, spawning the player, QMP control, snapshots, preview, clone, host GPU probe, and one model per window (`machines`, `wizard`, `shelf`, `snaps`, `editor`, `firstrun`) with the sentences it shows; `cli.rs` is every toolkit-free debug verb, `src/bin/launcherx.rs` their binary |
| `launcher-qt/` | the shipped front end (Qt 6 / QML over cxx-qt), installed as `2ksbox`; its own cargo workspace |
| `launcher-capi/` | the same models as a C ABI (`include/launcher_core.h`); `examples/smoke.c` is a test |
| `packaging/`, `scripts/package-{linux,flatpak,macos,windows}.sh`, `scripts/gen-flatpak-cargo-sources.sh`, `scripts/gen-icons.sh` | the packages and their self-checks |

Shared, edit minimally and say so in the commit: `shader-chain/` (the
librashader chain, linked into the player *and* the launcher — rebuild
and retest both), `player/build.rs`'s rpath, the root `Cargo.toml`,
`scripts/test.sh`. `guest-tools/src/cdshelf.{c,asm}` and patch 52 are
the in-guest half of the shelf (guest-tools README, patch README).

The egui front end `launcher/` was deleted on 2026-09-13 (ADR-017);
don't bring it back, and don't cite its `--diag-*-frame` or
`--pick-file` verbs — they went with it.

## Building

```sh
cargo build --release            # launcher-core + launcherx (root workspace)
cargo check --release --workspace  # keeps launcher-capi compiling
scripts/build.sh qt              # launcher-qt; skipped on a host without Qt 6
```

`launcher-qt` is outside the root workspace so a plain `cargo build`
never needs Qt 6 (the Mac, CI, the Flatpak). It needs `qt6-base` and
`qt6-declarative`; `cxx-qt-build` finds them through `qmake6`, no CMake.
A dependency change in either lock file means
`scripts/gen-flatpak-cargo-sources.sh`, or the offline Flatpak build
breaks.

## Test loop

No clicking and no unit tests. Three layers, all in `scripts/test.sh
host`:

- **The models, without a toolkit**: `launcherx` verbs (`--new`,
  `--wizard-new`, `--wizard-edit <bundle> [fields…]` with `-` keeping a
  field, `--print-args`, `--print-player-args`, `--discs`, `--boot-disc`,
  `--snapshots [--live]`, `--insert-disc`, `--clone`, `--first-run`,
  `--default-profiles`, `--preview-shader`, `--browse-start`,
  `--optimizations`, `--host-check`, `--paths`, `--diagnose`, …;
  `cli.rs` has the usage). `launcher-qt` answers the same verbs from the
  same code. Checks: `optimizations`, `pointer`, `extra-args`,
  `display-adapter`, `d3d9`, `voodoo2`, `music`, `pad`, `hpet`,
  `family-other`, `host-check`, `shelforder`, `dirshelf`, `clone`,
  `shader-defaults`, `preview-anim`, and `capi` (the C smoke over the
  same models). Most end with our own `qemu-system-i386` accepting the
  exact line `--print-args` wrote.
- **The real windows, headless**: `QT_QPA_PLATFORM=offscreen` with
  `LAUNCHER_QT_SCREEN=<screen>` (`wizard`, `wizardscroll`, `optall`,
  `create`, `closebox`, `clone`, `adddisc`, `pickdisc`, `discs`,
  `snapshots`, `profiles`, `saveprofile`, `firstrun`, `escfocus`,
  `editor`; cases in `launcher-qt/qml/Main.qml`) and
  `LAUNCHER_QT_ARG=<its argument>`. Without `LAUNCHER_QT_SHOT` the screen
  runs its script, prints what the window *shows* and quits — no GPU, no
  session. With `LAUNCHER_QT_SHOT=<png>` (and `LAUNCHER_QT_DELAY=<ms>`) it
  also grabs a picture, which needs a real session and a GPU nothing
  else is holding (a running player makes the grab hang). Checks:
  `qt-wizard` (every family's fields as shown vs. the model, every page
  fits the window), `qt-close`, `qt-esc`, `qt-snapshots`, `qt-profile`,
  `qt-shelf`, `qt-firstrun`, `qt-clone`.
- **The packages**: the `package` check runs `package-linux.sh --no-tar`
  (or `package-macos.sh` on a Mac), which asks the *staged* launcher and
  player with a scrubbed environment where every companion resolves, and
  opens a real window offscreen for a PNG. `icons` is
  `gen-icons.sh --check`. `scripts/package-flatpak.sh` runs its own
  in-sandbox checks and is not in the suite.

Beyond the suite: `tools/dos-guest-test.py` drives `launcherx` for the DOS
family end to end, and `tools/cdshelf-guest-test.sh <image> xp` is the
in-guest shelf on a real XP.

## Rules for working on the launcher

- **A decision goes in `launcher-core`, never in QML** — defaults that
  follow the family, labels, notes, which rows apply. The C smoke and the
  Qt window must not be able to disagree (ADR-014).
- **Test against a scratch library.** `LAUNCHER_LIBRARY_DIR`,
  `LAUNCHER_DISC_LIBRARY`, `LAUNCHER_SHADER_PROFILES_DIR` (and
  `LAUNCHER_SHADERS_DIR`, `LAUNCHER_BROWSE_MEMORY`) move everything the
  launcher writes; `create`, `adddisc`, `clone` and the verbs write for
  real. `LAUNCHER_PLAYER_BIN`, `LAUNCHER_QEMU_IMG_BIN`,
  `LAUNCHER_PC_BIOS_DIR` and `LAUNCHER_GUEST_TOOLS_ISO` override the
  companions.
- **Ask the window, not the model, for a Qt bug.** Every Qt-only bug so
  far (a spin box clamped to 32 MB, a name wiped by a combo box, a dialog
  that answered itself, a list nobody published) had a correct model; a
  check that asks the model passes on the broken build. A probe types
  with `insert` (a JS assignment unbinds the field and hides the bug) and
  presses a dialog by emitting its `accepted`/`rejected` signal, never by
  calling the like-named method. The retained-mode rules behind these
  are doc 07's "What is in the core" and "Five Qt traps".
- **Verify with a real QEMU.** `--print-args` is not the machine: the
  NIC that QEMU adds when none is asked for, and a sound card sliding
  into the NIC's PCI slot, were only visible through `query-pci` on a
  running binary.
- **Sentences in a window are short and plain** (user decision
  2026-09-16, doc 07); the *why* stays in comments and docs.
- The launcher never stops a running machine: a killed guest leaves a
  dirty FAT.

## Open

- **The preview's CPU readback** (doc 07, "What the front end still
  owns"): a `QQuickRhiItem` importing the Vulkan image instead of a temp
  BMP per frame. The one place the Qt build is worse than egui was.
- **Grid thumbnails** (a machine's last frame) and **bundle
  import/export**, both in doc 07's launcher list, are not built.
- **The player's own overlay** (pause, snapshot, disc swap, doc 07's
  Player section); today these live in the launcher only.
- **Packaging:** Flathub (metainfo screenshots need hosting on
  2ksbox.com; the manifest's sources as git rather than a local
  directory), the **AppImage** the user asked for after the Flatpak
  (2026-09-05, not started), and a Windows installer beside the zip.
  Windows live control (AF_UNIX) has not run on a real PC — M11's item.
- **Clone cannot be cancelled** once copying: `std::fs::copy` keeps the
  kernel's fast paths (reflinks, `copy_file_range`) and cannot stop
  mid-file, so Cancel is off while a copy runs.
- **`CDSHELF.EXE` on Win98**: the user has used it by hand; a scripted
  `tools/cdshelf-guest-test.sh <image> win98` pass has never been
  recorded (the first image tried had a broken shell).
- A shader-pack release and a docs site built from these documents were
  in the original M6 plan; neither needs code, and neither is started.

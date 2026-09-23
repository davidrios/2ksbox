# launcher-qt, the launcher

The front end the product ships (ADR-015), installed by every package as
`2ksbox`: the machine grid, the machine form, the disc shelf, snapshots,
and the shader profile manager with its live preview, in Qt 6 / QML
through [cxx-qt](https://github.com/KDAB/cxx-qt).

Nothing here decides anything. Every rule and every sentence a window
shows is in `launcher-core` (ADR-014); this crate is the Qt view over it.
The design is doc 07 (`docs/07-frontend.md`). The rules for working on
the launcher, the scratch-library variables and the test loop are in
`docs/tracks/m6-launcher.md`; the checks are in `docs/testing.md`.

## Building

It needs Qt 6's `qt6-base` and `qt6-declarative` development files and
nothing else. There is no CMake step: `cxx-qt-build` finds Qt through
`qmake6` and drives `moc` and `qmltyperegistrar` itself.

```sh
scripts/build.sh qt   # the stage that builds it, in the default set
cd launcher-qt && cargo build
```

The crate is its own cargo workspace, so the root `cargo build` never
needs Qt 6. A host without it builds everything else and rolls no
package; `scripts/build.sh` says so and `scripts/test.sh` skips the
`package` check.

## Running it

`create`, `adddisc`, `clone` and the debug verbs write for real, so
point the launcher at scratch copies first (the full list is in the M6
track doc, "Rules for working on the launcher"):

```sh
export LAUNCHER_LIBRARY_DIR=/tmp/lib
export LAUNCHER_DISC_LIBRARY=/tmp/discs.toml
export LAUNCHER_SHADER_PROFILES_DIR=/tmp/profiles
./target/debug/launcher-qt
```

`launcher-qt` answers every `launcher_core::cli` verb exactly as
`launcherx` does:

```sh
./target/debug/launcher-qt --paths
# prints "animated" or "still": whether the editor would keep redrawing
# this preset; PREVIEW_FRAME=<n> picks the frame the editor takes from a
# clock
./target/debug/launcher-qt --preview-shader <preset.slangp> <image> <out.png>
```

The real windows also run with no clicking, through
`LAUNCHER_QT_SCREEN=`, `LAUNCHER_QT_ARG=`, `LAUNCHER_QT_SHOT=` and
`LAUNCHER_QT_DELAY=` (M6 track doc, "Test loop"; the cases are in
`qml/Main.qml`):

```sh
# no shot: the case runs, prints what the window shows, and quits
QT_QPA_PLATFORM=offscreen LAUNCHER_QT_SCREEN=wizard LAUNCHER_QT_ARG=win98 \
  ./target/release/launcher-qt
# [diag] wizard memory: shown 256, model 256, range 32..512

LAUNCHER_QT_SHOT=/tmp/editor.png LAUNCHER_QT_SCREEN=editor \
LAUNCHER_QT_ARG="/path/crt-aperture.slangp;/path/frame.png" \
  ./target/debug/launcher-qt
```

The first form needs no GPU and no session; the `qt-*` checks read it. A
grab that never completes means another process, typically a running
player, holds the GPU.

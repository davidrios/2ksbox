# launcher-qt, the launcher

The front end the product ships (ADR-015). It has the machine grid, the
machine form, the disc shelf, snapshots, and the shader profile manager
with its live preview, in Qt 6 / QML through
[cxx-qt](https://github.com/KDAB/cxx-qt). Every package installs this
binary as `2ksbox`.

Nothing here decides anything. Every rule and every sentence a window
shows is in `launcher-core` (ADR-014), and this crate is the Qt view
over it. The design is doc 07 (`docs/07-frontend.md`). The rules for
working on the launcher, the scratch-library variables and the full
test loop are in `docs/tracks/m6-launcher.md`, and the checks are in
`docs/testing.md`.

## Building

It needs Qt 6's `qt6-base` and `qt6-declarative` development files and
nothing else. There is no CMake step: `cxx-qt-build` finds Qt through
`qmake6` and drives `moc` and `qmltyperegistrar` itself.

```sh
scripts/build.sh qt   # the stage that builds it, in the default set
cd launcher-qt && cargo build
```

The crate is its own cargo workspace, so the root `cargo build` never
needs Qt 6 (a Mac without it, CI, a sandbox). Such a host builds
everything else and can roll no package. `scripts/build.sh` says so in
its summary and `scripts/test.sh` skips the `package` check.

## Running it

`create`, `adddisc`, `clone` and the debug verbs write for real, so
point the launcher's paths at scratch copies first (the full variable
list is in the M6 track doc, "Rules for working on the launcher"):

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
`LAUNCHER_QT_DELAY=` (the M6 track doc, "Test loop"; the cases are in
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

The first form needs no GPU and no session, and it is what the `qt-*`
checks read. A grab that never completes means another process holds
the GPU, typically a running player. Take it again with nothing else on
the GPU.

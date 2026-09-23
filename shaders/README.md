# shaders

Our own CRT presets. The preset collection is libretro's slang-shaders,
the `third_party/slang-shaders` submodule; only our `.slangp` parameter
files (and any custom pass) live here. The display pipeline is doc 03;
shader profiles are doc 07.

- `syncmaster-753dfx.slangp` is the reference rig's monitor (doc 09), a
  17" Samsung DynaFlat with a delta dot trio at ≈0.20 mm. Its values come
  from the tube's geometry, not yet calibrated against photographs; the
  file says where each value came from.

```sh
player --shader shaders/syncmaster-753dfx.slangp --mode-sweep <dir>   # every mode in the table
player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- <qemu args>
```

`crt-geom`, `crt-easymode`, `crt-royale` and `crt-aperture` are worth
trying too. The player's flags are in `docs/development.md`.

A machine normally gets its shader through a **profile**, a preset plus
parameter overrides, one file each under the data directory's
`shader-profiles/` (`LAUNCHER_SHADER_PROFILES_DIR` moves it;
`launcher-core/src/shader_library.rs`, `shader_profile.rs`). The machine
form picks one by name, and the launcher turns it into the player's
`--shader` / `--shader-params` at launch. A hand-written `machine.toml`
can name a preset in its `shader` field instead. Without the submodule,
or in a package built without `--with-shaders`, the profile manager
downloads the collection into the data directory (`shader_source.rs`).

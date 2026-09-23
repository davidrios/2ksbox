# shaders

Our own CRT presets. The preset collection itself is libretro's
slang-shaders, the `third_party/slang-shaders` submodule; only `.slangp`
parameter files of ours (and any custom pass) live here. The display
pipeline is doc 03; shader profiles are doc 07.

- `syncmaster-753dfx.slangp` — the reference rig's monitor (doc 09), a
  17" Samsung DynaFlat with a delta dot trio at ≈0.20 mm. Derived from
  the tube's geometry, not yet calibrated against photographs of it; the
  file says where each value came from.

```sh
player --shader shaders/syncmaster-753dfx.slangp --mode-sweep <dir>   # every mode in the table
player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- <qemu args>
```

(`crt-geom`, `crt-easymode`, `crt-royale` and `crt-aperture` are worth
trying too; the player's flags are in `docs/development.md`.)

A machine normally gets its shader through a **profile**: a preset plus
parameter overrides, one file per profile under the data directory's
`shader-profiles/` (`LAUNCHER_SHADER_PROFILES_DIR` moves it;
`launcher-core/src/shader_library.rs`, `shader_profile.rs`), picked by
name in the machine form and turned into the player's `--shader` /
`--shader-params` at launch. A hand-written `machine.toml` can name a
preset in its `shader` field instead. Without the submodule, or in a
package built without `--with-shaders`, the profile manager downloads
the collection into the data directory (`shader_source.rs`).

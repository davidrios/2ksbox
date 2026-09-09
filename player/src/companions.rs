//! Where an *installed* player's optional companions are, and how QEMU is
//! told about them.
//!
//! Three of the things a guest can use are `dlopen`ed by QEMU itself,
//! late, by a search that starts at `build/…` — the checkout this binary
//! was built from (`hw/3dfx/glide2x_impl.c`, `d3dpt/hw/d3dpt_exec_load.c`).
//! A package has no checkout, so each of them names an environment
//! variable as its first candidate, and this is what fills those in:
//!
//! * `QEMU_GLIDE_LIB`  — our OpenGLide build (doc 12 §5).
//! * `D3DPT_EXEC_LIB`  — the Direct3D executor (doc 14).
//! * `D3DPT_DXVK_LIB`  — the DXVK `d3d9` the executor runs on, which it
//!   `dlopen`s in turn and which is not named like the others.
//! * `VK_DRIVER_FILES` — the Vulkan driver the executor needs. Stock macOS
//!   has no Vulkan at all, so a redistributable app carries a loader and
//!   an ICD of its own; on Linux the system's driver is the right one and
//!   nothing is set.
//! * `LIBSYNTH_SF2` — the General MIDI bank the `mpu401` device plays
//!   through (doc 20 §4). Not a `dlopen`, but the same problem: a
//!   machine says `synth=gm` and the file that answers it lives wherever
//!   this build was installed, which is not something to freeze into
//!   every machine's bundle. **Set in a checkout too**, unlike the four
//!   above, because there QEMU has no search of its own to fall back on
//!   — a library is found beside the binary, a SoundFont is not.
//!
//! Only ever *when the caller left them unset*: a developer running the
//! packaged player with `D3DPT_EXEC_LIB=` pointing at a fresh build is
//! doing that deliberately, and an A/B that the package silently
//! overrode would be worse than useless. Everything missing is simply not
//! set — QEMU already reports each absence in its own words ("d3dpt:
//! libd3dpt_exec not found … Direct3D pass-through off").
//!
//! The prefix rule is `launcher_core::paths`', deliberately duplicated
//! rather than depended on: the player links no launcher code, and the
//! rule is two `stat`s.

use std::path::{Path, PathBuf};

/// The install prefix this player is running under, or `None` in a
/// checkout. `share/2ksbox` is the marker, as it is for the launcher —
/// inside a macOS `.app` that makes the prefix `Contents`, whose
/// `MacOS/` plays the part `bin/` plays elsewhere.
fn install_prefix() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    if cfg!(windows) {
        let dir = exe.parent()?;
        return dir.join("pc-bios").is_dir().then(|| dir.to_path_buf());
    }
    let prefix = exe.parent()?.parent()?;
    prefix.join("share").join("2ksbox").is_dir().then(|| prefix.to_path_buf())
}

/// A packaged file, flat on Windows and under a Unix prefix's
/// `lib`/`share` otherwise (`launcher_core::paths::in_prefix`).
fn in_prefix(prefix: &Path, installed: &str) -> PathBuf {
    if !cfg!(windows) {
        return prefix.join(installed);
    }
    for lead in ["share/2ksbox/", "lib/2ksbox/", "libexec/2ksbox/", "bin/"] {
        if let Some(rest) = installed.strip_prefix(lead) {
            return prefix.join(rest);
        }
    }
    prefix.join(installed)
}

/// The bank the packages ship (doc 20 §4, `soundfonts/README.md`). One
/// name in one place: the packagers stage this file and this is what
/// names it to QEMU.
pub const SOUNDFONT: &str = "TimGM6mb.sf2";

fn set_if_unset_and_present(var: &str, path: PathBuf) {
    if std::env::var_os(var).is_some() || !path.exists() {
        return;
    }
    // SAFETY: main() calls this before any thread exists — the event loop
    // and QEMU's own thread are both started after it returns.
    unsafe { std::env::set_var(var, path) };
}

/// The names `--companions` prints, in the order this module sets them.
const VARS: [(&str, &str); 5] = [
    ("glide", "QEMU_GLIDE_LIB"),
    ("d3dpt-exec", "D3DPT_EXEC_LIB"),
    ("dxvk", "D3DPT_DXVK_LIB"),
    ("vulkan-icd", "VK_DRIVER_FILES"),
    ("soundfont", "LIBSYNTH_SF2"),
];

/// What `announce` resolved, one line each — the answer to "did this
/// package ship the thing, and is the copy it found its own?". Called
/// after `announce`, so a name with a path is either the package's file or
/// the caller's own override, and a name without one is a companion this
/// build has none of. `scripts/package-linux.sh` asks the *staged* player
/// this instead of restating the layout, which is what catches a rule that
/// moved on one side only.
pub fn report() {
    match install_prefix() {
        Some(prefix) => println!("prefix         {}", prefix.display()),
        None => println!("prefix         (a checkout: QEMU's own searches find build/…)"),
    }
    for (name, var) in VARS {
        match std::env::var_os(var) {
            Some(value) => println!("{name:<14} {}", Path::new(&value).display()),
            None => println!("{name:<14} (not shipped)"),
        }
    }
}

/// Point QEMU's own `dlopen` searches at the package. A no-op in a
/// checkout, where those searches already find `build/…`.
pub fn announce() {
    // The bank first, because it is the one companion that also has to
    // be found in a checkout: `soundfonts/` in the source tree, the
    // package's own copy otherwise.
    match install_prefix() {
        Some(prefix) => set_if_unset_and_present(
            "LIBSYNTH_SF2",
            in_prefix(&prefix, &format!("share/2ksbox/soundfonts/{SOUNDFONT}")),
        ),
        None => set_if_unset_and_present(
            "LIBSYNTH_SF2",
            Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../soundfonts")).join(SOUNDFONT),
        ),
    }
    let Some(prefix) = install_prefix() else { return };
    let dylib = |stem: &str| {
        let ext = if cfg!(target_os = "macos") {
            "dylib"
        } else if cfg!(windows) {
            "dll"
        } else {
            "so"
        };
        let name = if cfg!(windows) { format!("{stem}.{ext}") } else { format!("lib{stem}.{ext}") };
        in_prefix(&prefix, &format!("lib/2ksbox/{name}"))
    };
    set_if_unset_and_present("QEMU_GLIDE_LIB", dylib("glide2x"));
    set_if_unset_and_present("D3DPT_EXEC_LIB", dylib("d3dpt_exec"));
    // DXVK's own soname, which carries its major version rather than the
    // plain name the other three have.
    let dxvk = if cfg!(target_os = "macos") {
        "lib/2ksbox/libdxvk_d3d9.0.dylib"
    } else if cfg!(windows) {
        "bin/d3d9.dll"
    } else {
        "lib/2ksbox/libdxvk_d3d9.so.0"
    };
    set_if_unset_and_present("D3DPT_DXVK_LIB", in_prefix(&prefix, dxvk));
    if cfg!(target_os = "macos") && std::env::var_os("VK_ICD_FILENAMES").is_none() {
        set_if_unset_and_present(
            "VK_DRIVER_FILES",
            in_prefix(&prefix, "share/2ksbox/vulkan/icd.d/driver.json"),
        );
    }
}

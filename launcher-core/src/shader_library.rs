//! The shader profile library: a directory of `<slug>.toml` files, one per
//! profile — flat, unlike the machine library's per-bundle subdirectories
//! (`library.rs`), since a profile has no disk image or disc shelf beside
//! it to keep together. Same "plain, documented directory, no database"
//! stance (doc 07).

use crate::shader_profile::ShaderProfile;
use std::path::{Path, PathBuf};

/// The default profile directory: the platform data dir plus
/// `shader-profiles`, alongside `library::default_dir`'s `machines`.
/// `LAUNCHER_SHADER_PROFILES_DIR` overrides it.
pub fn default_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_SHADER_PROFILES_DIR") {
        return dir.into();
    }
    crate::paths::data_dir().map(|d| d.join("shader-profiles")).unwrap_or_else(|| PathBuf::from("shader-profiles"))
}

pub struct ProfileEntry {
    pub path: PathBuf,
    pub profile: ShaderProfile,
}

/// The profile id a machine's `shader_profile` field stores: a `.toml`
/// file's bare stem (e.g. `trinitron-warm`), matching `path`'s own name so
/// looking one up by id (`find`, below) is a plain filename join.
pub fn id_of(path: &Path) -> String {
    path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

/// An id from a profile name: lowercase, non-alphanumerics collapsed to
/// `-`, deduplicated against what's already in `dir` — same scheme as
/// `library::slug`, just against `<candidate>.toml` files instead of
/// bundle subdirectories.
fn slug(dir: &Path, name: &str) -> String {
    let mut base: String = name
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while base.contains("--") {
        base = base.replace("--", "-");
    }
    let base = base.trim_matches('-');
    let base = if base.is_empty() { "profile" } else { base };
    let mut candidate = base.to_string();
    let mut n = 2;
    while dir.join(format!("{candidate}.toml")).exists() {
        candidate = format!("{base}-{n}");
        n += 1;
    }
    candidate
}

/// Create a new profile under `dir`, returning its file path.
pub fn create(dir: &Path, name: String, preset: PathBuf) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    let path = dir.join(format!("{}.toml", slug(dir, &name)));
    ShaderProfile::new(name, preset).save(&path)?;
    Ok(path)
}

/// The starter profiles (`shader_source::DEFAULT_PROFILES`) against the
/// collection at `presets_dir`, returning the names actually written.
///
/// Two things it will not do, both of which would turn a helpful gesture
/// into a mess someone has to clean up. It never writes a **second**
/// profile under a name the library already has — `create`'s slug
/// deduplication would happily make `crt-aperture-2`, so re-running this
/// (a second download, a `--default-profiles` by hand) has to be a
/// no-op rather than a slow-motion duplication. And it skips a preset
/// the collection doesn't actually contain, since a profile naming a
/// missing `.slangp` is only a parse error deferred to whoever opens it.
pub fn create_defaults(dir: &Path, presets_dir: &Path) -> Vec<String> {
    let existing: Vec<String> = scan(dir).into_iter().map(|e| e.profile.name).collect();
    let mut added = Vec::new();
    for (name, rel) in crate::shader_source::DEFAULT_PROFILES {
        if existing.iter().any(|have| have == name) {
            continue;
        }
        let preset = presets_dir.join(rel);
        if !preset.is_file() {
            eprintln!("[shader-library] no {} in the collection; skipping the {name} profile", preset.display());
            continue;
        }
        // Absolute, whatever `presets_dir` was: a profile is read by the
        // *player*, which is started from wherever the launcher happens
        // to have been, and a relative preset would resolve against that
        // instead of against the collection.
        let preset = std::path::absolute(&preset).unwrap_or(preset);
        match create(dir, (*name).to_string(), preset) {
            Ok(_) => added.push((*name).to_string()),
            Err(e) => eprintln!("[shader-library] creating the {name} profile: {e}"),
        }
    }
    added
}

/// Every `*.toml` directly under `dir`. A file that fails to parse is
/// skipped with a stderr line, not fatal — matches `library::scan`.
pub fn scan(dir: &Path) -> Vec<ProfileEntry> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut entries: Vec<ProfileEntry> = read
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "toml"))
        .filter_map(|path| match ShaderProfile::load(&path) {
            Ok(profile) => Some(ProfileEntry { path, profile }),
            Err(err) => {
                eprintln!("[shader-library] skipping {}: {err}", path.display());
                None
            }
        })
        .collect();
    entries.sort_by(|a, b| a.profile.name.cmp(&b.profile.name));
    entries
}

/// Look up a profile by id (a machine's `shader_profile` value) under
/// `dir`. `None` covers both "no such profile" and "unreadable" — a
/// dangling reference (the profile was deleted after a machine picked it)
/// falls back to no shader override rather than failing the machine.
pub fn find(dir: &Path, id: &str) -> Option<ShaderProfile> {
    ShaderProfile::load(&dir.join(format!("{id}.toml"))).ok()
}

pub fn delete(path: &Path) -> std::io::Result<()> {
    std::fs::remove_file(path)
}

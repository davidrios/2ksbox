//! The part of "Browse…" that is not a dialog.
//!
//! Neither front end's file dialog is here: the egui build has to bring
//! one (`rfd` — egui draws pixels and nothing else), Qt ships its own
//! (`QtQuick.Dialogs`' `FileDialog`), and both end up on the same three
//! backends anyway — the XDG portal on Linux, `NSOpenPanel` on macOS,
//! `IFileDialog` on Windows. What *is* here is the decision they were
//! separately getting right: which extensions a field offers, and which
//! directory the dialog opens in. A `.slangp` lives somewhere nobody
//! would navigate to by hand, so getting that wrong is the difference
//! between a working button and a dialog on the user's home directory.

use std::path::{Path, PathBuf};

/// One extension filter for a dialog (e.g. `("Disk images", &["qcow2"])`).
/// A plain pair rather than a toolkit type: each front end turns it into
/// whatever its own dialog wants — `rfd::FileDialog::add_filter` here,
/// a `"Disk images (*.qcow2)"` string for Qt's `nameFilters` there.
pub type Filter<'a> = (&'a str, &'a [&'a str]);

/// The extensions a dialog is actually handed: every one in lower *and*
/// upper case. The constants are written lower case, and on Linux every
/// backend matches the glob case-sensitively — the XDG portal, GTK and
/// Qt's own dialog alike — so a `GAME.CUE` burnt by a DOS-era tool was
/// hidden from the disc shelf's "Browse…" (2026-09-11, user-reported).
/// Both spellings rather than a `*.[cC][uU][eE]` class, because Windows'
/// and macOS's dialogs take no classes, and a backend that case-folds
/// the globs itself would mangle one; a mixed-case `.Cue` is the one
/// spelling this misses.
pub fn extensions(filter: Filter) -> Vec<String> {
    let mut out = Vec::new();
    for e in filter.1 {
        for v in [e.to_ascii_lowercase(), e.to_ascii_uppercase()] {
            if !out.contains(&v) {
                out.push(v);
            }
        }
    }
    out
}

/// A Qt-style `"Disk images (*.qcow2 *.QCOW2 *.img *.IMG)"` name filter.
/// Qt's `FileDialog` takes those, so the same constants drive both
/// dialogs instead of the QML repeating the extension lists by hand.
pub fn name_filter(filter: Filter) -> String {
    let globs: Vec<String> = extensions(filter).iter().map(|e| format!("*.{e}")).collect();
    format!("{} ({})", filter.0, globs.join(" "))
}

/// The directory a path field's "Browse…" should open in: the value's own
/// directory if it names one (a file inside it, or the directory itself),
/// `None` (the OS default — the platform picker's own last-used location,
/// or an initial default) if the field is empty or names a bare filename.
///
/// A first attempt handed the dialog the *file* path instead of the
/// directory containing it, which breaks it; `cli`'s `--pick-file` verb
/// exercises this exact function for that reason.
pub fn start_dir(value: &str) -> Option<PathBuf> {
    if value.is_empty() {
        return None;
    }
    let path = Path::new(value);
    if path.is_dir() {
        return Some(path.to_path_buf());
    }
    path.parent().filter(|p| !p.as_os_str().is_empty()).map(|p| p.to_path_buf())
}

/// Where "Browse…" actually opens: the field's own value if it points
/// somewhere (`start_dir`), else the caller's suggestion for an empty
/// field (the preset collection, for the shader editor's preset field),
/// else the OS default. Its own function so `cli`'s `--browse-start`
/// verb can check the choice without popping a modal dialog only a human
/// could answer.
///
/// **Only the shader editor's preset field passes an `empty_dir`**, and
/// deliberately: a `.slangp` lives in a checkout's `third_party/` or a
/// downloaded copy under the platform data directory, and neither is
/// somewhere a person would navigate to by hand. A disk image, an
/// install ISO, a floppy, a disc or a screenshot are all files the user
/// already knows where they put, so those fields want the platform
/// picker's own last-used location, which is what `None` asks for. The
/// asymmetry looks like a bug from the outside — two "Browse…" buttons
/// in one window opening in different places (2026-09-06, user-reported)
/// — so it is written down here rather than inferred from call sites.
pub fn browse_start(value: &str, empty_dir: Option<&Path>) -> Option<PathBuf> {
    start_dir(value).or_else(|| empty_dir.map(|d| d.to_path_buf()))
}

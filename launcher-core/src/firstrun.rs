//! The first-run shader offer: the one question a brand-new launcher
//! asks before it shows anything else.
//!
//! A launcher with no preset collection is a launcher whose machines all
//! look like a flat window — the CRT chain is the point of the thing
//! (doc 03), and `shader_source` already knows how to fetch libretro's
//! collection. What was missing was ever *offering* to: the download
//! button lives two windows deep, on the profile manager's preset row,
//! which is exactly where someone who has never opened the profile
//! manager will not find it.
//!
//! So this is the same download, asked as a question on the way up, and
//! answered once. Two rules make "once" mean once:
//!
//! * The question is only asked when there is **no collection at all**
//!   (`shader_source::presets_dir`), which is already false in a source
//!   checkout — the submodule is a collection — and in any package that
//!   ships one.
//! * Answering it either way writes a marker (`first-run.txt` in the
//!   profile directory), so declining is not re-asked on every start.
//!   The marker sits beside the profiles rather than beside the presets
//!   because a successful download *replaces* the preset directory by a
//!   rename (`shader_source::fetch`) and would take the marker with it.
//!
//! And a "yes" is worth something the moment it finishes: the starter
//! profiles (`shader_source::DEFAULT_PROFILES`) are written against the
//! collection that just landed, so the first machine someone creates has
//! a CRT to pick rather than a preset picker and four hundred `.slangp`
//! files to guess from.

use crate::shader_library;
use crate::shader_source::{self, Download, Status};
use std::path::{Path, PathBuf};

/// The dialog's title, here rather than in a front end for the same
/// reason its sentences are: two views of one question.
pub const TITLE: &str = "Download shader presets?";

/// What the dialog is showing right now. `Idle` is "no dialog" — the
/// answer for every start after the first, and for every launcher that
/// already has a collection.
pub enum Step {
    Idle,
    /// The question is up: `question()`, with a confirm and a cancel.
    Asking,
    /// Megabytes fetched so far. There is no total (see `shader_source`).
    Downloading(f64),
    /// The download failed; the message, with a retry beside it.
    Failed(String),
    /// It worked: what arrived and which profiles were added, for the
    /// one line the dialog shows before it is dismissed.
    Done(String),
}

/// The offer's whole state machine. Built by `check`, polled by `state`
/// — from a repaint (egui) or a timer (Qt), the same way
/// `editor::Presets` is.
#[derive(Default)]
pub struct FirstRun {
    profiles_dir: PathBuf,
    asking: bool,
    download: Option<Download>,
    /// The finished download's sentence, or its failure — kept until the
    /// user dismisses it, so neither flashes past.
    outcome: Option<Result<String, String>>,
}

impl FirstRun {
    /// Decide whether to ask, given where profiles live
    /// (`shader_library::default_dir()` for both front ends). Cheap: a
    /// `stat` for the marker and, only when it is absent, `presets_dir`'s
    /// two-level walk.
    pub fn check(profiles_dir: PathBuf) -> FirstRun {
        let asking = !marker(&profiles_dir).exists() && shader_source::presets_dir().is_none();
        FirstRun { profiles_dir, asking, ..FirstRun::default() }
    }

    /// A model that will never ask — what a front end holds when the
    /// question has no place (a diagnostic run, a headless frame grab).
    pub fn silent() -> FirstRun {
        FirstRun::default()
    }

    /// Whether anything is on screen. A front end shows its dialog on
    /// this and reads `state` for what goes in it.
    pub fn open(&self) -> bool {
        self.asking || self.download.is_some() || self.outcome.is_some()
    }

    /// Whether a download is running — the cue to keep a timer going,
    /// matching `editor::Presets::download_active`.
    pub fn busy(&self) -> bool {
        self.download.is_some()
    }

    /// The question itself. It names the size and the destination for
    /// the same reason the profile manager's row does: 50 MB on a phone
    /// tether is a decision, and a download that lands somewhere unnamed
    /// is one nobody can undo.
    pub fn question(&self) -> String {
        format!(
            "There are no CRT shader presets on this machine yet — they are what makes a \
             machine look like the monitor it was played on.\n\n\
             Download libretro's slang-shaders ({size}) into {dir}?\n\n\
             {count} ready-made profiles are added with them: {names}.",
            size = shader_source::DOWNLOAD_SIZE,
            dir = shader_source::install_dir().display(),
            count = shader_source::DEFAULT_PROFILES.len(),
            names = shader_source::default_profile_names(),
        )
    }

    /// The confirm button's words, size included — the same phrasing the
    /// profile manager's own button uses.
    pub fn confirm_label(&self) -> String {
        format!("Download ({})", shader_source::DOWNLOAD_SIZE)
    }

    /// The cancel button's words. "Not now" rather than "Cancel": the
    /// offer does not come back, and the profile manager's button is
    /// where it is taken up later.
    pub fn cancel_label(&self) -> &'static str {
        "Not now"
    }

    /// Yes: remember the question was answered and start the download.
    pub fn accept(&mut self) {
        self.asking = false;
        self.answered("accepted");
        self.start();
    }

    /// No: remember it and say nothing more, this start or any other.
    pub fn decline(&mut self) {
        self.asking = false;
        self.outcome = None;
        self.answered("declined");
    }

    /// The "Try again" on a failed download.
    pub fn retry(&mut self) {
        self.outcome = None;
        self.start();
    }

    /// Close whatever is on screen — the OK on the finished line, and
    /// the way out of a failure that is not being retried.
    pub fn dismiss(&mut self) {
        self.asking = false;
        self.download = None;
        self.outcome = None;
    }

    /// What to draw, turning a finished download into the starter
    /// profiles on the way past. Safe to call as often as a front end
    /// likes.
    pub fn state(&mut self) -> Step {
        if let Some(download) = &self.download {
            match download.status() {
                Status::Running(bytes) => return Step::Downloading(bytes as f64 / 1_000_000.0),
                Status::Done(dir) => {
                    self.download = None;
                    self.outcome = Some(Ok(self.install_defaults(&dir)));
                }
                Status::Failed(err) => {
                    self.download = None;
                    self.outcome = Some(Err(err));
                }
            }
        }
        match &self.outcome {
            Some(Ok(done)) => Step::Done(done.clone()),
            Some(Err(err)) => Step::Failed(err.clone()),
            None if self.asking => Step::Asking,
            None => Step::Idle,
        }
    }

    fn start(&mut self) {
        self.download = Some(Download::start(shader_source::install_dir()));
    }

    /// The starter profiles against the collection that just landed, and
    /// the sentence naming them. A profile whose name is already taken is
    /// left alone (`shader_library::create_defaults`), so this can never
    /// be what duplicates somebody's library.
    fn install_defaults(&self, presets: &Path) -> String {
        let added = shader_library::create_defaults(&self.profiles_dir, presets);
        let mut line = format!("Shader presets installed in {}.", presets.display());
        if !added.is_empty() {
            line.push_str(&format!(" Added {} profiles: {}.", added.len(), added.join(", ")));
        }
        line
    }

    /// Write the marker. A failure here is a warning and nothing else:
    /// the worst it costs is being asked again next start, which is a
    /// great deal better than refusing to go on.
    fn answered(&self, how: &str) {
        if self.profiles_dir.as_os_str().is_empty() {
            return;
        }
        let path = marker(&self.profiles_dir);
        let body = format!(
            "The first-run shader-preset offer was {how}.\n\
             Delete this file to be asked again the next time {name} starts.\n",
            name = crate::paths::NAME,
        );
        let wrote = std::fs::create_dir_all(&self.profiles_dir).and_then(|()| std::fs::write(&path, body));
        if let Err(e) = wrote {
            eprintln!("[first-run] {}: {e}", path.display());
        }
    }
}

/// The marker file: plain text in the profile directory, so somebody who
/// finds it can read what it is and delete it (doc 07's "a plain,
/// documented directory, no database").
pub fn marker(profiles_dir: &Path) -> PathBuf {
    profiles_dir.join("first-run.txt")
}

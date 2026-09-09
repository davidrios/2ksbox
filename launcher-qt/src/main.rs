//! The companion launcher (doc 07) on Qt 6 / QML through cxx-qt, one of
//! the project's two maintained front ends — the other is `launcher/`
//! (egui). Both are views over the **same crate**: `launcher-core` holds
//! the bundle format, the machine library, the disc shelf, the snapshot
//! state machine, the wizard's form, the shader profile editor, the
//! preview's render path and every debug verb that needs no toolkit.
//!
//! So what is in this crate is Qt, and only Qt: a `QObject` per window
//! whose properties are a *projection* of a core model, a
//! `QAbstractListModel` wherever a core model has rows, and the views in
//! `qml/`. Nothing here decides anything about machines, discs or
//! shaders. When the two builds used to disagree — the wizard's
//! networking checkbox, the words under it, whether a new shader profile
//! kept its parameter overrides — it was always because a rule had been
//! written twice, and there is nowhere left to write one twice.
//!
//! It is deliberately **not** in the root workspace: `Cargo.toml`
//! declares its own, so `cargo build` at the repository root never
//! starts needing Qt 6 development files on the Mac, in CI or in the
//! Flatpak. Build it from this directory; that is the whole build
//! command, no CMake (`cxx-qt-build` finds Qt through `qmake6` and
//! drives `moc`/`qmltyperegistrar` itself).
//!
//! **The trap every bridge here is written around:** cxx-qt stores a
//! Q_PROPERTY *in* the Rust struct, and the generated setter is
//!
//! ```ignore
//! if self.field == value { return; }   // no binding loops
//! self.as_mut().rust_mut().field = value;
//! self.field_changed();
//! ```
//!
//! so writing a property's field directly and then "publishing" it with
//! its setter emits **nothing** — the setter sees the value already
//! there. Every window in the port opened once and then stopped
//! reacting because of exactly that. Keeping the state in a core model
//! *beside* the properties is what makes this safe by construction: a
//! `publish` reads the model and writes every property through its
//! setter, and the property fields are never assigned anywhere else.

// A windowed program on Windows, like the egui build: a console-subsystem
// binary opens a black terminal on every double-click. The debug verbs
// below still print — `main` borrows the console it was launched from
// when there is one (`launcher_core::console`).
#![cfg_attr(windows, windows_subsystem = "windows")]

mod preview;

mod qt {
    pub mod diag;
    pub mod discs;
    pub mod firstrun;
    pub mod machines;
    pub mod shaders;
    pub mod snaps;
    pub mod wizard;
}

use cxx_qt_lib::{QGuiApplication, QQmlApplicationEngine, QString, QUrl};

// `src/appearance.cpp`: the Quick Controls style and the colour scheme,
// neither of which cxx-qt-lib binds.
extern "C" {
    fn launcher_qt_choose_style();
    fn launcher_qt_set_scheme(scheme: i32);
    fn launcher_qt_appearance() -> *const std::ffi::c_char;
}

/// Where `build.rs`'s QML module lands in the binary's resource tree:
/// `qrc:/qt/qml/` + the module URI with `.` as `/`.
const QML_MAIN: &str = "qrc:/qt/qml/com/_2ksbox/launcher/qml/Main.qml";

unsafe extern "C" {
    /// `src/window_icon.cpp`: `QGuiApplication::setWindowIcon` on a
    /// `QIcon` built from these PNG bytes.
    fn twoksbox_set_window_icon(png: *const u8, len: i32);
}

fn main() {
    // First of all: a windowed program on Windows has no stderr, so
    // anything that goes wrong on the way to the first window would
    // otherwise be a process that vanishes without a word. The log's
    // milestones say how far this got — and *no* log at all says it
    // never reached `main`, which is its own answer
    // (`launcher_core::fatal`).
    launcher_core::fatal::install("qt");
    // Debug verbs first, before a GUI exists. They are
    // `launcher_core::cli`'s, so this binary answers every one the egui
    // launcher does, identically — `--paths`, `--discs`, `--snapshots`,
    // `--wizard-new`, `--preview-shader` and the rest — rather than the
    // two it used to reimplement. (`--pick-file` is the one exception:
    // it pops `rfd`'s dialog, and this build's file dialog is QML's.)
    let mut args = std::env::args().skip(1);
    let verb = args.next();
    if verb.is_some() {
        // Anything on the command line answers in text, so this is where
        // a Windows build goes looking for a console to print into.
        launcher_core::console::attach_parent();
    }
    if let Some(verb) = verb.as_deref() {
        if let Some(code) = launcher_core::cli::run(verb, &mut args) {
            std::process::exit(code);
        }
        // `--diag-frame` is not a verb: the headless screenshot path is
        // driven by environment variables (`qt/diag.rs`).
        if verb.starts_with("--") && verb != "--diag-frame" {
            eprintln!("unknown option {verb}");
            std::process::exit(2);
        }
    }

    // Before the application exists, because a style cannot be chosen
    // after one has been used. Windows and macOS keep their own; the
    // rest get Fusion instead of Basic (`src/appearance.cpp`).
    unsafe { launcher_qt_choose_style() };
    launcher_core::fatal::note("QGuiApplication");
    let mut app = QGuiApplication::new();
    // The window's own identity, the two halves the egui build sets with
    // `with_app_id` and `with_icon`: the desktop-entry name a Wayland
    // compositor matches a window to its launcher (and its icon) by, and
    // the picture itself for every window system that takes one instead.
    // The PNG is one of the sizes `scripts/gen-icons.sh` derives from the
    // icon the packages install, so a window and the applications menu
    // cannot show different pictures.
    QGuiApplication::set_desktop_file_name(&QString::from(launcher_core::paths::APP_ID));
    let png = include_bytes!("../../packaging/icon/2ksbox-256.png");
    // SAFETY: a pointer and a length into a `'static` slice, read and
    // copied into a QImage before the call returns.
    unsafe { twoksbox_set_window_icon(png.as_ptr(), png.len() as i32) };
    // Light, whatever the desktop is set to: the Quick Controls style
    // paints its controls light and only the surfaces around them come
    // from the palette, so a dark system palette gets you half a theme
    // (`src/appearance.cpp`). `LAUNCHER_QT_SCHEME=system` hands the
    // desktop's own palette back, `=dark` forces the other one.
    unsafe {
        launcher_qt_set_scheme(match std::env::var("LAUNCHER_QT_SCHEME").as_deref() {
            Ok("system") => 0,
            Ok("dark") => 2,
            _ => 1,
        })
    };
    // What actually took, not what was asked for: the style a report
    // came from is the first thing to know when a window is the wrong
    // colour.
    launcher_core::fatal::note(&unsafe {
        std::ffi::CStr::from_ptr(launcher_qt_appearance())
    }.to_string_lossy());
    launcher_core::fatal::note("QML engine");
    let mut engine = QQmlApplicationEngine::new();
    if let Some(mut engine) = engine.as_mut() {
        // A QML error otherwise leaves the engine with no root object and
        // the process sitting in an event loop with no window at all —
        // which looks exactly like a hang.
        engine
            .as_mut()
            .on_object_creation_failed(|_, url| {
                launcher_core::fatal::fatal(&format!("{url} failed to load"));
                std::process::exit(1);
            })
            .release();
        launcher_core::fatal::note(&format!("loading {QML_MAIN}"));
        engine.load(&QUrl::from(QML_MAIN));
    }
    launcher_core::fatal::note("the event loop");
    if let Some(app) = app.as_mut() {
        app.exec();
    }
}

/// A `QString` from anything `Display`, which is most of what the bridges
/// hand to QML (paths, errors, formatted labels).
pub fn qs(s: impl std::fmt::Display) -> QString {
    QString::from(&s.to_string())
}

/// A `QString` from an `Option<&str>`, empty for `None` — how a model's
/// `status()`/`error()` reaches a property.
pub fn qs_opt(s: Option<&str>) -> QString {
    s.map(QString::from).unwrap_or_default()
}

//! The first-run shader offer (`launcher_core::firstrun`), as one
//! QObject over the shared model.
//!
//! Nothing here decides anything: whether to ask at all, the words at
//! every step, when the download is finished and which starter profiles
//! it earned are all the core model's. This is the projection QML binds
//! to — a `step` naming which answers are possible, and the two strings
//! that go with it, which `FirstRunDialog.qml` hands to a real
//! `MessageDialog` as its `text` and `informativeText`.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        /// Whether the offer is live at all — false for every start
        /// after the first, and for any launcher that came with a preset
        /// collection. `busy` is the download specifically, which is the
        /// one step that is not a dialog.
        #[qproperty(bool, open)]
        #[qproperty(bool, busy)]
        /// Which answers are possible: "asking", "running", "failed",
        /// "done", or "" for nothing at all. `FirstRunDialog.qml` turns
        /// it into the dialog's standard buttons.
        #[qproperty(QString, step)]
        /// The words, from the model at every step: the dialog's `text`
        /// and `informativeText`, and the header's line while the
        /// download runs.
        #[qproperty(QString, title)]
        #[qproperty(QString, headline)]
        #[qproperty(QString, detail)]
        type FirstRun = super::FirstRunRust;

        /// Yes — remember the answer and start the download.
        #[qinvokable]
        fn accept(self: Pin<&mut FirstRun>);

        /// No — remember that too, so this is asked exactly once.
        #[qinvokable]
        fn decline(self: Pin<&mut FirstRun>);

        /// "Try again", after a download that failed.
        #[qinvokable]
        fn retry(self: Pin<&mut FirstRun>);

        /// Put the dialog away (OK, or Close on a failure).
        #[qinvokable]
        fn dismiss(self: Pin<&mut FirstRun>);

        /// Poll a running download, from a QML `Timer` — the Qt build's
        /// equivalent of the egui build repainting (`qt/shaders.rs`'s
        /// `poll_download` says more about why the interval is stated
        /// out loud here rather than being a frame rate).
        #[qinvokable]
        fn poll(self: Pin<&mut FirstRun>);
    }

    impl cxx_qt::Initialize for FirstRun {}
}

use crate::qs;
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use launcher_core::firstrun::{self, Step};
use launcher_core::shader_library;
use std::pin::Pin;

#[derive(Default)]
pub struct FirstRunRust {
    open: bool,
    busy: bool,
    step: QString,
    title: QString,
    headline: QString,
    detail: QString,

    /// The model. Everything above is a projection of it.
    model: firstrun::FirstRun,
}

/// The question is decided **once, at construction**, and the properties
/// published from there: QML shows the dialog on `open`, so the answer
/// has to exist before the first frame does.
impl cxx_qt::Initialize for ffi::FirstRun {
    fn initialize(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model = if diag_screen_other_than_this() {
            // A headless probe drives one window and photographs it
            // (`qt/diag.rs`). A modal question over the top of that
            // would be in every grab and would swallow the probe's
            // clicks — on a checkout with no `slang-shaders` submodule,
            // which is the only place this can happen, it would look
            // like the window under test having failed. The `firstrun`
            // screen is the exception: that one is here to drive *this*.
            firstrun::FirstRun::silent()
        } else {
            firstrun::FirstRun::check(shader_library::default_dir())
        };
        self.publish();
    }
}

fn diag_screen_other_than_this() -> bool {
    matches!(std::env::var("LAUNCHER_QT_SCREEN"), Ok(screen) if !screen.is_empty() && screen != "firstrun")
}

impl ffi::FirstRun {
    fn accept(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.accept();
        self.publish();
    }

    fn decline(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.decline();
        self.publish();
    }

    fn retry(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.retry();
        self.publish();
    }

    fn dismiss(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.dismiss();
        self.publish();
    }

    fn poll(self: Pin<&mut Self>) {
        if !self.rust().model.busy() {
            return;
        }
        self.publish();
    }

    /// The model onto the properties — every one through its own setter,
    /// which is the rule the whole port is written around (`main.rs`).
    fn publish(mut self: Pin<&mut Self>) {
        let (open, busy, step, title, headline, detail);
        {
            // `state()` advances a finished download into the starter
            // profiles, so it needs `&mut`.
            let this = &mut *self.as_mut().rust_mut();
            let message = this.model.state();
            step = QString::from(match message.step {
                Step::Idle => "",
                Step::Asking => "asking",
                Step::Downloading => "running",
                Step::Failed => "failed",
                Step::Done => "done",
            });
            headline = qs(message.headline);
            detail = qs(message.detail);
            open = this.model.open();
            busy = this.model.busy();
            title = QString::from(firstrun::TITLE);
        }
        self.as_mut().set_title(title);
        self.as_mut().set_headline(headline);
        self.as_mut().set_detail(detail);
        self.as_mut().set_busy(busy);
        self.as_mut().set_open(open);
        // Last: `step` is what the dialog follows, so every word it will
        // show is already in place when it is told to open.
        self.as_mut().set_step(step);
    }
}

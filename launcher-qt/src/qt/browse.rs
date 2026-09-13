//! "Browse…" for QML's dialogs (`launcher_core::browse`), as one small
//! QObject every path field and folder button makes its own copy of.
//!
//! Nothing here decides anything: where a dialog opens is
//! `browse::browse_start`'s answer — the field's value, the caller's
//! suggestion, the directory the last dialog was browsing — and what it
//! remembers is `browse::remember`'s. This turns paths into the URLs
//! Qt's dialogs take and back, with `QUrl`'s own local-file rules rather
//! than a `file://` prefix glued on, which a Windows drive letter does
//! not survive.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qurl.h");
        type QUrl = cxx_qt_lib::QUrl;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        type Browse = super::BrowseRust;

        /// Where a dialog should open for a field showing `value`, with
        /// `empty_dir` as the suggestion for an empty field ("" for
        /// none). An empty URL is the OS default.
        #[qinvokable]
        fn start_url(self: &Browse, value: &QString, empty_dir: &QString) -> QUrl;

        /// The local path of a URL a dialog handed back.
        #[qinvokable]
        fn local_path(self: &Browse, url: &QUrl) -> QString;

        /// A path was picked in a dialog: the next one opens beside it.
        #[qinvokable]
        fn remember(self: &Browse, path: &QString);
    }
}

use crate::qs;
use cxx_qt_lib::{QString, QUrl};
use launcher_core::browse;
use std::path::{Path, PathBuf};

#[derive(Default)]
pub struct BrowseRust;

impl ffi::Browse {
    fn start_url(&self, value: &QString, empty_dir: &QString) -> QUrl {
        let empty_dir = empty_dir.to_string();
        let empty_dir = (!empty_dir.is_empty()).then(|| PathBuf::from(empty_dir));
        match browse::browse_start(&value.to_string(), empty_dir.as_deref()) {
            // An absolute path is `QUrl::fromLocalFile` to `fromUserInput`.
            Some(dir) => QUrl::from_user_input(&qs(dir.display()), &QString::default()),
            None => QUrl::default(),
        }
    }

    fn local_path(&self, url: &QUrl) -> QString {
        url.to_local_file().unwrap_or_default()
    }

    fn remember(&self, path: &QString) {
        let path = path.to_string();
        if !path.is_empty() {
            browse::remember(Path::new(&path));
        }
    }
}

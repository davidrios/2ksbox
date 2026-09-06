//! The smallest cxx-qt program, in three rungs (M11).
//!
//! `launcher-qt.exe` faults at address 0 before `main` on Windows -- on
//! wine and on a real machine alike -- so the question is which of
//! cxx-qt's layers puts a static initialiser there. Each feature adds
//! exactly one, and the rung that stops printing `qtmin: main` is the
//! answer:
//!
//!   (none)   Qt and cxx-qt-lib linked, no bridge
//!   bridge   one QObject: moc, generated C++, cxx-qt's own initialisers
//!   qml      a QML module: qmltyperegistrar, a resource, and the type
//!            registration that runs *before* main
//!
//! It says `qtmin: main` on stdout and again in `qtmin.log` beside the
//! executable, because a windows-subsystem program has no stdout -- and
//! this one is a console program precisely so that it does.

#[cfg(feature = "bridge")]
mod obj;

fn main() {
    // Both, because the whole question is whether anything of ours runs
    // at all: a file survives a double-click, stdout survives a pipe.
    let note = |what: &str| {
        println!("qtmin: {what}");
        if let Ok(exe) = std::env::current_exe() {
            use std::io::Write;
            if let Ok(mut f) =
                std::fs::OpenOptions::new().create(true).append(true).open(exe.with_extension("log"))
            {
                let _ = writeln!(f, "qtmin: {what}");
            }
        }
    };
    note("main");
    note(&format!(
        "rungs: bridge={} qml={}",
        cfg!(feature = "bridge"),
        cfg!(feature = "qml")
    ));

    // `--no-gui` stops here: on a machine with no display the question is
    // only ever "did it reach main", and a QGuiApplication would fail for
    // an unrelated reason.
    if std::env::args().any(|a| a == "--no-gui") {
        note("stopping before QGuiApplication (--no-gui)");
        return;
    }

    let mut app = cxx_qt_lib::QGuiApplication::new();
    note("QGuiApplication");
    #[cfg(feature = "qml")]
    {
        let mut engine = cxx_qt_lib::QQmlApplicationEngine::new();
        note("QQmlApplicationEngine");
        if let Some(engine) = engine.as_mut() {
            engine.load(&cxx_qt_lib::QUrl::from("qrc:/qt/qml/com/min/qml/Main.qml"));
        }
        note("loaded Main.qml");
        if let Some(app) = app.as_mut() {
            app.exec();
        }
    }
    #[cfg(not(feature = "qml"))]
    {
        note("no QML module in this rung; not entering the event loop");
        let _ = &mut app;
    }
    note("done");
}

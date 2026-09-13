//! `launcherx`: the launcher's toolkit-free verbs, with no toolkit
//! behind them.
//!
//! `launcher_core::cli` is where every verb that needs no GUI lives, so
//! that `launcher-qt` and this binary answer them identically (doc 07,
//! ADR-014). This is the caller nothing has to be installed to build:
//! `launcher-qt` needs Qt 6. `scripts/test.sh` drives
//! the launcher through this binary for exactly that reason — the suite
//! runs before every commit, on hosts with no Qt, and must not pay for
//! a front end to answer `--print-args`.
//!
//! It is a test and debugging tool, not a product: no packager installs
//! it (ADR-015 — the shipped command is `2ksbox`, which is
//! `launcher-qt`). What it deliberately cannot do is what *is* a
//! toolkit: the headless frame grabs of real windows, which are
//! `launcher-qt`'s (`QT_QPA_PLATFORM=offscreen`, doc 07).

fn main() {
    // No `fatal::install` here, unlike the front end: that exists
    // because a windowed program on Windows has no stderr and a panic on
    // the way to the window would vanish. This one is a console program
    // whose every run is somebody watching, and installing it would
    // append to the launcher's own crash log 60 times per test run.
    let mut args = std::env::args().skip(1);
    let Some(verb) = args.next() else {
        eprintln!("usage: launcherx <verb> [args]   (the launcher's toolkit-free verbs; see launcher-core/src/cli.rs)");
        std::process::exit(2);
    };
    match launcher_core::cli::run(&verb, &mut args) {
        Some(code) => std::process::exit(code),
        None => {
            eprintln!("launcherx: unknown verb {verb}");
            eprintln!("  (the headless window frame grabs are the front end's: `launcher-qt`)");
            std::process::exit(2);
        }
    }
}

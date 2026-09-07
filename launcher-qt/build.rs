//! cxx-qt's cargo-only build: no CMake, no Corrosion. `CxxQtBuilder`
//! finds Qt through `qmake6`, runs `moc` and `qmltyperegistrar` over the
//! QObjects the bridges declare, compiles the generated C++ and links it
//! into this binary — so `cargo build` is still the whole build command,
//! which is what keeps this comparable to the egui launcher.
//!
//! The QML files are compiled into the binary as a Qt resource, hence
//! the `qrc:/qt/qml/<uri as a path>/…` URL `main.rs` loads: an installed
//! launcher has no `qml/` directory beside it.

use cxx_qt_build::{CxxQtBuilder, QmlModule};

include!("../packaging/windows/win-icon.rs");

fn main() {
    // The picture Explorer draws on the launcher: this crate is the one
    // that becomes 2ksbox.exe in a Windows package that has Qt.
    embed_windows_icon();
    // `appearance.cpp` calls `QQuickStyle`, and it is compiled into the
    // generated archive that the linker reaches *after* the Qt import
    // libraries `qt_module` names. On ELF that is fine; a PE import
    // library only satisfies symbols that are already undefined when the
    // linker walks past it, so the Windows build needs it named again at
    // the end -- which is what a `rustc-link-arg` is (M11).
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        println!("cargo::rustc-link-arg=-lQt6QuickControls2");
    }
    CxxQtBuilder::new_qml_module(
        // The same reverse-DNS identity as the rest of the product
        // (ADR-011): `com._2ksbox.…`, the leading digit escaped because
        // neither a QML module URI nor a D-Bus name may start with one.
        QmlModule::new("com._2ksbox.launcher").qml_files([
            "qml/Main.qml",
            "qml/Disclosure.qml",
            "qml/PathField.qml",
            "qml/PresetCollection.qml",
            "qml/WizardWindow.qml",
            "qml/DiscShelfWindow.qml",
            "qml/SnapshotsWindow.qml",
            "qml/ShaderProfilesWindow.qml",
            "qml/ShaderEditorWindow.qml",
        ]),
    )
    // Windows: `std::call_once` in cxx-qt's own generated crate
    // initialiser reaches a `__once_proxy` in `libstdc++-6.dll` that
    // reads its argument out of a different emutls registry than this
    // binary writes it to, and calls the NULL it finds -- before `main`,
    // so the launcher never started at all on Windows (M11). This
    // defines the proxy locally, where the two halves agree; the file
    // compiles to nothing anywhere else.
    .cpp_file("src/once_proxy.cpp")
    // The window icon: one call into QGuiApplication that cxx-qt-lib
    // does not bind (it has QImage, not QIcon).
    .cpp_file("src/window_icon.cpp")
    // The headless probe's stand-in for the title bar's close button
    // (`src/close_event.cpp`): a close *event*, which nothing in
    // cxx-qt-lib can send.
    .cpp_file("src/close_event.cpp")
    // Which Quick Controls style, and which colour scheme
    // (`src/appearance.cpp`) — it calls `QQuickStyle`, so the module has
    // to be linked as well as the ones the QML imports pull in.
    .cpp_file("src/appearance.cpp")
    .qt_module("QuickControls2")
    .files([
        "src/qt/diag.rs",
        "src/qt/discs.rs",
        "src/qt/machines.rs",
        "src/qt/shaders.rs",
        "src/qt/snaps.rs",
        "src/qt/wizard.rs",
    ])
    .build();
}

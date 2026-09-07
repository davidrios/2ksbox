//! The reproducer's ladder (M11): the Qt front end faults before `main`
//! on Windows, and each feature adds exactly one cxx-qt layer, so the
//! rung that first stops printing "qtmin: main" names the layer.
fn main() {
    #[cfg(not(feature = "bridge"))]
    {
        // Rung 1: link Qt and cxx-qt-lib, no bridge at all -- so no moc,
        // no generated C++, no static initialisers of cxx-qt's own.
        cxx_qt_build::CxxQtBuilder::new().cpp_file("src/once_proxy.cpp").build();
    }
    #[cfg(all(feature = "bridge", not(feature = "qml")))]
    {
        // Rung 2: one QObject through a bridge. moc runs, generated C++
        // is compiled in, but nothing registers a QML type.
        cxx_qt_build::CxxQtBuilder::new()
            .file("src/obj.rs")
            .cpp_file("src/once_proxy.cpp")
            .build();
    }
    #[cfg(feature = "qml")]
    {
        // Rung 3: what launcher-qt does -- a QML module, so
        // qmltyperegistrar, a compiled-in resource, and the type
        // registration that runs from a static initialiser.
        cxx_qt_build::CxxQtBuilder::new_qml_module(
            cxx_qt_build::QmlModule::new("com.min").qml_files(["qml/Main.qml"]),
        )
        .file("src/obj.rs")
        .cpp_file("src/once_proxy.cpp")
        .build();
    }
}

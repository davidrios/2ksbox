// The application icon, inside the .exe.
//
// `include!`d by the build script of every crate that produces a Windows
// binary someone sees in Explorer (`launcher-qt`, `player`).
// An `include!` rather than a crate on purpose. A build-dependency would
// land in `Cargo.lock`, and the Flatpak's offline build declares every
// crate there with a checksum (`packaging/flatpak/cargo-sources.json`), so
// a Windows-only dependency would have to be vendored for a Linux build
// that never uses it.
//
// Explorer, the taskbar and Alt-Tab draw the *lowest-numbered* icon
// resource, so the .rc names it `1`. The resource script holds nothing
// else (no VERSIONINFO block; nobody maintains a version string yet).
//
// Cross-built from Linux, so mingw's `windres` turns the .rc into a COFF
// object and `rustc-link-arg-bins` hands it to the linker for every binary
// of the crate. A host with no windres gets a warning and a binary with no
// icon, not a failed build.
#[allow(dead_code)]
fn embed_windows_icon() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let ico = root.join("packaging/icon/2ksbox.ico");
    // The icon set is generated (`scripts/gen-icons.sh`) but checked in,
    // so this is a plain file read at build time on any machine.
    println!("cargo:rerun-if-changed={}", ico.display());
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    let rc = out.join("icon.rc");
    let obj = out.join("icon.o");
    // The path goes into the .rc quoted, and a backslash there is an
    // escape, so use forward slashes, which windres accepts everywhere.
    let path = ico.display().to_string().replace('\\', "/");
    if std::fs::write(&rc, format!("1 ICON \"{path}\"\n")).is_err() {
        println!("cargo:warning=could not write {}: the .exe gets no icon", rc.display());
        return;
    }
    // The target-prefixed name first: a Linux host has both `windres`
    // (its own binutils, which cannot write COFF for this target) and
    // `x86_64-w64-mingw32-windres`, and only the second is right.
    let target = std::env::var("TARGET").unwrap_or_default();
    let prefixed = format!("{}-windres", target.replace("-pc-windows-gnu", "-w64-mingw32"));
    for windres in [prefixed.as_str(), "windres"] {
        match std::process::Command::new(windres)
            .args(["-I", &root.display().to_string()])
            .arg(&rc)
            .args(["-O", "coff", "-o"])
            .arg(&obj)
            .status()
        {
            Ok(status) if status.success() => {
                println!("cargo:rustc-link-arg-bins={}", obj.display());
                return;
            }
            Ok(status) => {
                println!("cargo:warning={windres} failed ({status}): the .exe gets no icon");
                return;
            }
            // Not installed under this name: try the next one.
            Err(_) => continue,
        }
    }
    println!("cargo:warning=no windres for {target}: the .exe gets no icon");
}

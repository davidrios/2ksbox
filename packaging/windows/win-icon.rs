// The application icon, inside the .exe.
//
// `include!`d by the build script of every crate that produces a Windows
// binary someone sees in Explorer (`launcher`, `launcher-qt`, `player`).
// A shared file rather than three copies, and an `include!` rather than a
// crate, deliberately: a build-dependency would land in `Cargo.lock`, and
// the Flatpak's offline build declares every crate in that file with a
// checksum (`packaging/flatpak/cargo-sources.json`) — a dependency added
// for Windows would have to be vendored for a Linux build that never uses
// it.
//
// Windows takes the *lowest-numbered* icon resource in a binary as the
// one Explorer, the taskbar and Alt-Tab draw, so the .rc names it `1`.
// Nothing else is in the resource script: a VERSIONINFO block would be
// worth having one day, but it is a different question (a version string
// nobody is currently maintaining) and this one is only about the picture.
//
// Cross-built from Linux, so the compiler is mingw's `windres`, which
// turns the .rc into a COFF object; `rustc-link-arg-bins` hands that
// object to the linker for every binary of the crate. On a host that has
// no windres this prints a warning and links a binary with no icon rather
// than failing the build: a missing picture is not a reason a Windows
// build cannot happen at all.
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
    // escape — so forward slashes, which windres accepts everywhere.
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

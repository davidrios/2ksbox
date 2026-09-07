// The Windows icon, and nothing else: everything the launcher needs to
// find at runtime it finds relative to its own executable (doc 07's
// install layout), which needs no build script at all.
include!("../packaging/windows/win-icon.rs");

fn main() {
    embed_windows_icon();
}

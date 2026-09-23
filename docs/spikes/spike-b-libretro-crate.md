# Spike B: rust-libretro crate vs. own shim (superseded)

Void since ADR-005 (2026-09-02) dropped RetroArch/libretro for a
standalone Rust player, a decision not to be reopened. Desk research had
found that `rust-libretro-sys` and `libretro-core` both support hardware
rendering, and the plan was the `-sys` bindings under an export layer of
our own; nothing was built.

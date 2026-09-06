# qtmin — the smallest cxx-qt binary, in three rungs

Why this exists: `launcher-qt.exe` cross-built for Windows faults on a
call to address 0 **before `main`** — under wine and on a real Windows
machine alike (M11, `docs/tracks/m11-windows-host.md`). Nothing of the
launcher's own runs, so nothing the launcher does can be the cause, and
the question is which of cxx-qt's layers puts a static initialiser there.

Each cargo feature adds exactly one layer, and the rung that stops
printing `qtmin: main` is the answer:

| rung | `--features` | what it adds |
|---|---|---|
| 1 | *(none)* | Qt and `cxx-qt-lib` linked. No bridge, no moc, no generated C++ of our own |
| 2 | `bridge` | one QObject through a `#[cxx_qt::bridge]`: moc, generated C++, cxx-qt's own initialisers |
| 3 | `qml` | a QML module: `qmltyperegistrar`, the `.qml` in a resource, and the type registration that runs before `main` |

```sh
scripts/win-cross.sh sh -c 'cd tools/qtmin && cargo build --release --target x86_64-pc-windows-gnu'
cp tools/qtmin/target/x86_64-pc-windows-gnu/release/qtmin.exe \
   build/win/package/2ksbox-0.0.1-windows-x86_64-qt/          # for the Qt DLLs beside it
cd build/win/package/2ksbox-0.0.1-windows-x86_64-qt
env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all wine qtmin.exe --no-gui
```

`--no-gui` stops before `QGuiApplication`, because on a machine with no
display the only question is whether `main` was reached at all. It says
so on stdout *and* in `qtmin.log` beside the executable: this one is a
console program on purpose, so that both work.

## What it found (2026-09-06)

**Rung 1 already faults.** So it is not the QML registration, not the
bridge, and not a line of ours — linking `cxx-qt-lib` is enough.

The chain, read out of the crash with `wine winedbg` and `objdump`:

1. `_GLOBAL__sub_I_call_initializers.cpp` — a static initialiser in the
   C++ that `cxx-qt-build` generates — calls `cxx_qt_init_crate_cxx_qt`.
2. That uses `std::call_once`. On mingw-w64 `std::call_once` parks the
   callable in `std::__once_call`, a **`__thread` variable reached
   through emulated TLS**, and asks `pthread_once` to run
   `__once_proxy`.
3. `__once_proxy` lives in `libstdc++-6.dll`. The import is bound
   correctly — the thunk really does land in the DLL — and it reads
   `__once_call` back through emutls.
4. But emutls keys its storage per *libgcc*, and there are two: the exe
   links libgcc statically (rustc does this for `x86_64-pc-windows-gnu`
   and neither `-C link-self-contained=no` nor `-shared-libgcc` moves
   it), while `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s. So the
   proxy reads a slot the exe never wrote, finds `NULL`, and calls it.

Which is the `rip=0` with a return address inside `pthread_once` that
every crash dump of that binary shows.

The cure is to stop the C++ runtime spanning two modules for this call —
a statically linked `libstdc++` in the exe, or a toolchain whose
`std::call_once` does not use emutls (mingw's *win32* threads model uses
`InitOnceExecuteOnce`), or a cxx-qt that does not use `std::call_once` in
its crate initialiser. `-C link-arg=-static-libstdc++` on its own does
**not** do it: something on the link line still asks for the DLL, and the
exe keeps importing `__once_proxy`. That is where this got to.

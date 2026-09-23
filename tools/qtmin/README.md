# qtmin, the smallest cxx-qt binary in three rungs

A reproducer for the Windows build (M11, `docs/tracks/m11-windows-host.md`).
`launcher-qt.exe` cross-built for Windows used to fault on a call to
address 0 **before `main`**, under Wine and on a real PC alike. Nothing of
the launcher's own had run, so the question was which of cxx-qt's layers
puts a static initialiser there. Each cargo feature adds one layer; the
rung that stops printing `qtmin: main` names it.

| rung | `--features` | what it adds |
|---|---|---|
| 1 | *(none)* | Qt and `cxx-qt-lib` linked; no bridge, no moc, no generated C++ of ours |
| 2 | `bridge` | one QObject through a `#[cxx_qt::bridge]`: moc, generated C++, cxx-qt's initialisers |
| 3 | `qml` | a QML module: `qmltyperegistrar`, the `.qml` in a resource, type registration before `main` |

```sh
scripts/win-cross.sh sh -c 'cd tools/qtmin && cargo build --release --target x86_64-pc-windows-gnu'
# beside the Qt DLLs of a staged Windows package (scripts/package-windows.sh)
cp tools/qtmin/target/x86_64-pc-windows-gnu/release/qtmin.exe build/win/package/2ksbox-0.0.1-windows-x86_64/
cd build/win/package/2ksbox-0.0.1-windows-x86_64
env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all wine qtmin.exe --no-gui
```

`--no-gui` stops before `QGuiApplication`, since with no display the only
question is whether `main` was reached. It is a console program on
purpose, so the answer goes to stdout and to `qtmin.log` beside the
executable.

## The answer: two emutls registries

**Rung 1 already faulted**, so the cause was linking `cxx-qt-lib`, not
QML, the bridge or any code of ours. Read out of the crash with `wine
winedbg` and `objdump`:

1. A static initialiser in the C++ that `cxx-qt-build` generates
   (`_GLOBAL__sub_I_call_initializers.cpp`) calls
   `cxx_qt_init_crate_cxx_qt`, which uses `std::call_once`.
2. On mingw-w64, `std::call_once` parks the callable in
   `std::__once_call`, a `__thread` variable reached through **emulated
   TLS**, and asks `pthread_once` to run `__once_proxy`, which lives in
   `libstdc++-6.dll` and reads `__once_call` back through emutls.
3. emutls keys its storage per libgcc, and there are two: rustc links
   libgcc statically into the exe for `x86_64-pc-windows-gnu`, while
   `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s. The proxy reads a slot
   the exe never wrote, finds `NULL` and calls it. That is the `rip=0`
   with a return address inside `pthread_once` in every crash dump.

## The fix

`src/once_proxy.cpp`, carried by `launcher-qt` too
(`launcher-qt/src/once_proxy.cpp`):

```cpp
namespace std { extern __thread void (*__once_call)(); }
extern "C" void __once_proxy() { std::__once_call(); }
```

The linker prefers a local definition over an import, so the proxy that
runs reads `__once_call` through *this* module's emutls, the registry
`call_once` wrote to. It is libstdc++'s own implementation
(`src/c++11/mutex.cc`). With it all three rungs reach `main`, and the Qt
launcher passes every check of `package-windows.sh`.

What does **not** work:

- `-C link-arg=-static-libstdc++`. Something on the link line still asks
  for the DLL, and the exe keeps importing `__once_proxy`.
- `-C link-self-contained=no` and `-C link-arg=-shared-libgcc`. rustc
  links libgcc statically for this target regardless, so there are still
  two registries. The fix makes that harmless instead of fighting it.

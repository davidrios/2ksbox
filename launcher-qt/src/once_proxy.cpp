// `std::call_once` across a libstdc++ DLL boundary, defused.
//
// `tools/qtmin/README.md` has the whole diagnosis, and reproduces it. In short: cxx-qt's generated
// crate initialiser calls `std::call_once` from a static initialiser;
// mingw's `call_once` parks the callable in `std::__once_call`, a
// `__thread` variable reached through **emulated TLS**, and hands
// `pthread_once` the address of `__once_proxy` to run. `__once_proxy`
// lives in `libstdc++-6.dll`, and emutls keys its storage per *libgcc* —
// of which there are two here, the one linked statically into this
// binary (rustc does that for `x86_64-pc-windows-gnu`) and the one
// `libstdc++-6.dll` uses. So the proxy reads a slot this binary never
// wrote, finds NULL, and calls it.
//
// Defining `__once_proxy` here gives the linker a local definition to
// prefer over the DLL's import, and this one reads `__once_call` through
// *this* module's emutls — the same registry `call_once` wrote it to. It
// is the whole of libstdc++'s own implementation (`src/c++11/mutex.cc`):
// the proxy exists only to be a plain `void()` that `pthread_once` can
// take the address of.
//
// Windows/mingw only. Everywhere else the C++ runtime is one module and
// libstdc++'s own proxy is the right one.
//
// And only where libstdc++ exports the thread-local itself. GCC 16's mingw
// headers define `_GLIBCXX_NO_EXTERN_THREAD_LOCAL` ("Windows does not
// support exporting thread-local data"): `call_once` then reaches the
// callable through `std::__get_once_call()`, a function in the DLL, so it is
// written and read by the same module and the bug above cannot happen --
// and `std::__once_call` is not exported at all, so this definition would
// not link ("undefined reference to std::__once_call", MSYS2's GCC 16,
// 2026-09-17). Fedora's GCC 15, which builds the package, has the extern
// thread-local and needs the proxy.
#if defined(_WIN32) && defined(__MINGW32__)
#include <cstddef> // libstdc++'s configuration, os_defines.h included
#endif
#if defined(_WIN32) && defined(__MINGW32__) && !defined(_GLIBCXX_NO_EXTERN_THREAD_LOCAL)

namespace std {
extern __thread void (*__once_call)();
}

extern "C" void __once_proxy() { std::__once_call(); }

#endif

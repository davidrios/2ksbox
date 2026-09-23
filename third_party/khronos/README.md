# Khronos OpenGL headers (vendored)

`GL/glcorearb.h` and `KHR/khrplatform.h` from the Khronos OpenGL registry
(https://registry.khronos.org/OpenGL/), MIT-licensed (see the SPDX header
in each file), fetched 2026-09-02.

qemu-3dfx's `hw/mesa/mesagl_pfn.h` includes `<GL/glcorearb.h>`. Linux has
it from libglvnd / Mesa, but macOS has no `GL/` headers outside XQuartz,
which the Mac build does not need. `scripts/configure-qemu.sh` puts this
directory on the include path on every platform, so every build sees one
known header version.

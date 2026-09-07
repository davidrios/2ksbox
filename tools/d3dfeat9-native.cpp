/*
 * d3dfeat9-native: guest-tools/src/d3dfeat9.c, unmodified, as a native
 * program on DXVK's d3d9 (the D3D executor, ADR-007). Same options, same
 * log, same -dump BMPs → diff against reference/d3d with tools/bmpdiff.py.
 *
 * Build (macOS and Linux alike; the rpath finds the .dylib/.so in build/dxvk):
 *   c++ -std=c++17 -O2 -o build/d3dfeat9-native tools/d3dfeat9-native.cpp \
 *     -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *     -Ithird_party/dxvk/include/native/directx \
 *     -Lbuild/dxvk/src/d3d9 -ldxvk_d3d9 -Wl,-rpath,$PWD/build/dxvk/src/d3d9
 * Run (Linux: DXVK_WSI_DRIVER=Headless is enough; macOS env as for tools/dxvk-d3d9-test.cpp):
 *   DYLD_LIBRARY_PATH=/opt/homebrew/lib VK_ICD_FILENAMES=<icd> \
 *   DXVK_WSI_DRIVER=Headless build/d3dfeat9-native -frames 600 -dump 300 g9.bmp
 * There is no window (win32_headless.h): the run is off-screen and ends on
 * -frames, so it needs no display and no keyboard.
 */
#define COBJMACROS
#include "d3dgame-native/win32_headless.h"
#include <d3d9.h>
#include "../guest-tools/src/d3dfeat9.c"

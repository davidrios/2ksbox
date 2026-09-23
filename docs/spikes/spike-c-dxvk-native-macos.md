# Spike C: DXVK d3d9 native on macOS (doc 14 P0b), 2026-09-03

**What became of it.** Option 2 below. ADR-007 made DXVK the executor on
every platform, on KosmicKrisp on macOS 26+. The spike started
`patches/dxvk/01`–`05`; the headless WSI (patch 04) later replaced its
SDL2 one, and `scripts/configure-dxvk.sh` is the build. Hosts below
DXVK's Vulkan 1.3 floor run the same executor on Windows' own d3d9 or on
Wine on the host (doc 14). The rest is the record of the spike.

Question: can DXVK's d3d9 run natively on the Air (M1, macOS 15.7.9) over
MoltenVK, so that DXVK is the host executor of the paravirtual Direct3D
device on every platform? Measured against DXVK master (3.1.0, d7ac258,
submodule `third_party/dxvk`) and MoltenVK 1.4.2 (Homebrew, Vulkan 1.4
conformant on the M1).

## Result: not on MoltenVK as-is; yes on Mesa's KosmicKrisp, one line away

DXVK master refuses the device before rendering anything. Its required
feature list (`src/dxvk/dxvk_device_info.cpp`, `getFeatureList`, third
argument `true`) against what MoltenVK 1.4.2 reports on the M1
(`vulkaninfo` with `VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`):

| Required by DXVK | MoltenVK 1.4.2 | KosmicKrisp (Mesa main 69945fe) | What d3d9 needs it for |
|---|---|---|---|
| `geometryShader` | false (Metal has none) | absent (roadmap: "tessellation and geometry" in 2026) | nothing in d3d9; a D3D10+ feature DXVK core requires globally |
| `shaderCullDistance` | false | true | nothing in d3d9 (SM4 semantic) |
| `VK_EXT_depth_clip_enable` | not exposed | true | D3D depth-clip semantics; older DXVK mapped it onto `depthClamp` |
| `robustBufferAccess2` | false ([MoltenVK #2447](https://github.com/KhronosGroup/MoltenVK/issues/2447), open since 2025-02) | true | OOB vertex/constant fetches read zero (sloppy era games) |
| `nullDescriptor` | false | true | unbound resources; DXVK ≤ 1.10 bound dummy resources instead |

Everything else DXVK requires (Vulkan 1.3 core, robustness2 extension
presence, maintenance5/6, load_store_op_none, dual-source blend,
multi-viewport, multi-draw-indirect, BC textures, precise occlusion
queries) is present on both.

**KosmicKrisp** (LunarG's Mesa Vulkan-on-Metal-4 driver, Vulkan 1.4
conformant on Apple Silicon, prebuilt in the [LunarG macOS SDK
1.4.357](https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-357-0/))
advertises four of the five, and transform feedback. It **requires
macOS 26** (Metal 4), which every Apple Silicon Mac can take. With it,
DXVK d3d9 needs one patch: `geometryShader` required → optional (DXVK
only uses the flag to add the GS pipeline stage bit; d3d9 never creates
one).

## DXVK native builds on macOS

`meson setup … -Dnative_sdl2=enabled -Denable_d3d8/10/11/dxgi=false`
configures (run `git submodule update --init --recursive` inside
`third_party/dxvk`; Homebrew `glslang`, `vulkan-headers`, `sdl2`,
`meson`, `ninja`). The core (dxvk, dxbc-spirv, dxso, util, wsi) compiles
with Apple clang 17. Every failure is in the Linux-only native Windows
shim:

| File | Error | Fix size |
|---|---|---|
| `include/native/windows/*` | `LoadLibraryA`, `GetProcAddress`, `FreeLibrary`, `CloseHandle`, `CreateCompatibleDC`, `DeleteDC` undeclared (used by `wsi/sdl2`, `vulkan_loader.cpp`, `d3d9_util.cpp`, `d3d9_common_texture.cpp`, `d3d9_surface.cpp`) | dlopen/dlsym shims + GDI stubs, ~30 lines |
| `src/util/util_env.cpp:139` | `pthread_setname_np` takes one argument on macOS | 3 lines |

The 6 failing objects are portability, not architecture. We never use
DXVK's presenter or swapchain: the device renders to an off-screen
target the player imports (IOSurface/dma-buf, as the GL path does), so a
window-less WSI replaces SDL2 on every platform.

## Verified end to end (same day)

With `patches/dxvk/01–03` (macOS shim, geometry shaders optional,
portability enumeration) `libdxvk_d3d9.0.dylib` builds in `build/dxvk`
and `tools/dxvk-d3d9-test.cpp` (hidden SDL2 window, fixed-function lit
textured triangle, `GetRenderTargetData` → BMP) reaches the M1 through
MoltenVK and gets DXVK's refusal verbatim:

```
info:  Found device: Apple M1 (MoltenVK 0.2.2210)
info:    Skipping: Device does not support required feature 'shaderCullDistance'
```

That is the first missing feature in DXVK's list order. The same binary
is the acceptance test for KosmicKrisp, and so is
`tools/d3dgame9-native.cpp`, which compiles the unmodified reference
scene against DXVK through a small Win32 shim (then over SDL2, now the
window-less `tools/d3dgame-native/win32_headless.h`) and diffs against
`reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp`.

## Verified on KosmicKrisp (2026-09-03, Air on macOS 26.6.2)

LunarG SDK 1.4.357.1 (`vulkansdk-macos-1.4.357.1.zip`, headless install
into `~/VulkanSDK`; KosmicKrisp is the optional component
`com.lunarg.vulkan.kosmic`, added with the SDK's `MaintenanceTool`).
`vulkaninfo`: `Apple M1`, `DRIVER_ID_MESA_KOSMICKRISP` 26.2.99, Vulkan
1.4.359, conformance 1.4.3.2. It lacks exactly two required features:
`geometryShader` (patch 02, as predicted) and **`fillModeNonSolid`**
(not predicted: MoltenVK reports it, KosmicKrisp does not). Patch 05 makes
it optional and clamps D3D wireframe/point fill to solid on such a driver.
With both, `dxvk-d3d9-test` writes its BMP and `d3dgame9-native -frames
600 -dump 300` at 120 fps windowed matches the rig golden as closely as
RADV does (1095 vs 1089 pixels beyond tolerance 8, 16 beyond 32 on both,
HUD masked). ADR-007 stands; option 2 is the production path and option 1
is no longer needed.

## Options for the macOS executor

1. **DXVK + MoltenVK with a DXVK patch queue** (what Gcenx's DXVK-macOS
   1.10.x fork did for CrossOver): relax `geometryShader`,
   `shaderCullDistance`, `depthClipEnable` (fall back to depth clamp),
   restore dummy resources for `nullDescriptor`, accept core
   `robustBufferAccess` instead of `robustBufferAccess2`. Days to a
   couple of weeks; the dummy-resource patch is the only real code and
   fights DXVK's descriptor-heap design on every rebase. Works on
   macOS 15.
2. **DXVK + KosmicKrisp.** A one-line requirement patch; needs macOS 26
   on every Mac that runs the player. The driver is young (Metal 4 bugs on
   M1/M2 under macOS 26 with in-driver workarounds, "fixed in macOS 27")
   but Mesa people built it to run DXVK/Proton. Same patch queue
   discipline as our QEMU fork, near-zero maintenance.
3. **Implement the features in MoltenVK.** Geometry shaders and cull
   distance need a compute-emulation pass MoltenVK's SPIRV-Cross pipeline
   has no place for (and d3d9 does not need them);
   `robustBufferAccess2`/`nullDescriptor` need shader-side bounds checks
   Metal does not provide. Only depth-clip is cheap. Worse than option 1
   for the same result.
4. **Own "DXMetal" D3D9-on-Metal.** A rewrite of DXVK's d3d9 (~40 k
   lines: fixed-function T&L and texture-stage emulation, SM1–3
   translation, state, resources, lost-device protocol) plus 6 years of
   game-specific fidelity work; and Linux still needs DXVK, so two
   executors to keep bug-for-bug equal against the rig goldens. Months to
   a year before the first commercial title; last resort.

**Recommendation (as written before the KosmicKrisp run).** DXVK is the
executor everywhere. On macOS start with option 1, move to option 2 as
the production path; the P1 transport, the guest DLLs and the D3DGAME9
golden diff are identical under both. Do not write a native Metal
backend.

## Reproduce (as run then; the SDL2 WSI is no longer built)

```sh
brew install molten-vk vulkan-loader vulkan-headers vulkan-tools glslang sdl2 meson ninja
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
vulkaninfo | grep -E "geometryShader|shaderCullDistance|robustBufferAccess2|nullDescriptor|depth_clip"
cd third_party/dxvk && git submodule update --init --recursive
PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig meson setup ../../build/dxvk --buildtype release \
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
  -Dnative_sdl2=enabled -Dnative_glfw=disabled -Dnative_sdl3=disabled
ninja -C ../../build/dxvk -k 0      # 6 objects fail, all in the Windows shim
```

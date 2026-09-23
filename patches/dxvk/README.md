# DXVK patch queue

DXVK's d3d9 is the host executor's Direct3D 9 for the paravirtual
Direct3D device (doc 14, ADR-006/007) on Linux, macOS and Windows. Only
`d3d9` is built: `scripts/configure-dxvk.sh` → `build/dxvk`, and with
`--windows` → `build/win/dxvk` (shipped as `dxvk_d3d9.dll`). The macOS
Vulkan setup (KosmicKrisp, the SDK, the app's own loader) is
`docs/build-macos.md`.

`scripts/prepare-dxvk.sh` applies these patches in filename order to the
pinned submodule `third_party/dxvk` (3.1.0, master `d7ac258`), restoring
every tracked file a patch touches and re-applying the queue on each run.

| Patch | What / why | Drop when |
|---|---|---|
| `01-native-macos` | dxvk-native builds on macOS. The Windows shim (`util_win32_compat.h`) was `#if __unix__`, which Apple clang does not define; `getExePath` via `_NSGetExecutablePath`; one-argument `pthread_setname_np`; the `libvulkan.1.dylib` loader names; the d3d9 export list as an ld64 `-exported_symbols_list` (`d3d9.exports`, from `d3d9.sym`) | upstream accepts a macOS port |
| `02-geometry-shader-optional` | `geometryShader` required → optional. d3d9 never creates one and Vulkan-on-Metal drivers have none; DXVK uses the flag only for a stage mask. Must be required again if d3d10/11 are ever built | KosmicKrisp grows geometry shaders, or upstream scopes the requirement per API |
| `03-portability-enumeration` | enables `VK_KHR_portability_enumeration` when offered and sets `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, because the loader hides portability ICDs (MoltenVK, KosmicKrisp) from apps that do not opt in. Without it: "Failed to create Vulkan instance" (`VK_ERROR_INCOMPATIBLE_DRIVER`) | upstream opts in |
| `04-wsi-headless` | a window-less WSI driver (`src/wsi/headless`, `DXVK_WSI_DRIVER=Headless`) with one fake monitor with a D3D9-era mode list, no valid window, `createSurface` fails. A swapchain with a NULL window has no presenter and `Present` is a no-op, so the executor renders off-screen and reads the back buffer. It is the only WSI we build. The patch drops upstream's "SDL3, SDL2, or GLFW are required", and `configure-dxvk.sh` disables all three | never; upstream has no headless WSI |
| `05-fill-mode-non-solid-optional` | `fillModeNonSolid` required → optional; `BindRasterizerState` clamps wireframe and point fill to solid when the driver lacks it. KosmicKrisp does not expose it | KosmicKrisp exposes `fillModeNonSolid` |
| `06-vulkan-loader-beside-us` | on macOS, `@loader_path/libvulkan.1.dylib` (and the unversioned name) is tried before the bare names. macOS ships no Vulkan, the app carries its own loader and ICD beside the executor, and `DYLD_*` is stripped from a hardened, notarized process. Inert in a checkout | upstream has a macOS bundle story |
| `07-ff-bumpenvmap-luminance` | `D3DTOP_BUMPENVMAPLUMINANCE` in the fixed-function shader applies its luminance. The previous stage's value was read into a shadowing variable (the outer one stayed 0), and the luminance came from the environment map's texel instead of the bump map's. BUMPTEST drew full intensity where L = ½ was asked. `tools/d3dpt-dp2-test.cpp`'s luminance case fails without it | upstream fixes both lines |
| `08-wsi-headless-windows` | patch 04's headless WSI on Windows too, beside Win32 (still the default when `DXVK_WSI_DRIVER` is unset). The executor asks for `Headless` on every host, since none of its devices has a window | never, as 04 |
| `09-singleton-acquire-throw` | `Singleton<T>::acquire` (`util_singleton.h`, the holder of d3d9's process-wide `DxvkInstance`) counted a user *before* constructing the object. When the constructor threw (`DxvkInstance` on a host whose ICD reports no GPU, such as KosmicKrisp on macOS 15 where `vkEnumeratePhysicalDevices` fails, or with a loader and no ICD), the count stayed at one and the object null. The next `Direct3DCreate9` in the process handed `D3D9InterfaceEx` that null instance, which faulted reading its config. The executor names the same DXVK by full path and by leaf name, so its second candidate was that next call, and the community app on macOS 15 died at the adapter's realize instead of moving on to the Wine executor. It now counts after `new`, so a throw leaves nothing and the next call tries again. The executor also never asks a library twice (`d3dpt_exec.cpp`, `refused`), and the `exec-no-device` check in `scripts/test.sh` tests the built artefacts | upstream counts after constructing |

## Building and testing

`scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C
build/dxvk`; `scripts/build.sh` runs it. The oracles are
`tools/dxvk-d3d9-test.cpp` (build line in its header) and the native
reference scene; `scripts/test.sh` runs DXVK under
`DXVK_WSI_DRIVER=Headless` in the `d3dgame9-nat`, `d3dpt-exec` and
`d3dpt-dp2` checks.

On macOS the library `dlopen`s the loader by leaf name, so a run from a
checkout needs `DYLD_LIBRARY_PATH=/opt/homebrew/opt/vulkan-loader/lib`
(never all of `/opt/homebrew/lib`; see the gotcha in
`docs/00-status.md`) and `VK_ICD_FILENAMES` naming KosmicKrisp's ICD.
`test.sh` sets both. On KosmicKrisp the reference scene's frame 300 is 1095 pixels
beyond tolerance 8 against the rig golden, against 1089 on Linux RADV.
MoltenVK refuses (`Device does not support required feature
'shaderCullDistance'`), as expected.

## Regenerating

Run `prepare-dxvk.sh`, edit inside `third_party/dxvk`, then `git -C
third_party/dxvk diff -- <files>` (plain `a/` `b/` prefixes; `git add -N`
a new file first so it appears with `--- /dev/null`). Prove it from
pristine: `prepare-dxvk.sh` twice must succeed.

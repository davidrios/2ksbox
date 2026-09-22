/*
 * d3dpt_exec_load.c — dlopen libd3dpt_exec once for every d3dpt device
 * (see d3dpt_exec_load.h). A machine without the library (or without a
 * Vulkan device) boots normally; the devices report "no executor".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

/*
 * Windows spells the same three calls differently and has no dlfcn.h.
 * Kept as a local shim rather than glib's GModule because QEMU's glib
 * dependency does not carry gmodule-2.0 on every host, and this is the
 * only dynamic load in the tree.
 */
#ifdef _WIN32
#include <windows.h>
#define D3DPT_DLOPEN(p)     ((void *)LoadLibraryA(p))
#define D3DPT_DLSYM(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#define D3DPT_DLCLOSE(h)    FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define D3DPT_DLOPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define D3DPT_DLSYM(h, s)   dlsym((h), (s))
#define D3DPT_DLCLOSE(h)    dlclose(h)
#endif

static D3dptExecLib lib;
static bool tried;
static bool refused;

void d3dpt_exec_refuse(void)
{
    refused = true;
}

void d3dpt_exec_prefer(const char *which)
{
    if (!which || !*which) {
        return;
    }
    if (strcmp(which, "auto") && strcmp(which, "dxvk") && strcmp(which, "system")) {
        warn_report("d3dpt: d3d9=%s is not auto, dxvk or system; ignored", which);
        return;
    }
    g_setenv("D3DPT_D3D9", which, false);
#ifdef _WIN32
    /*
     * The executor is another module with another C runtime, and reads
     * the process environment; g_setenv reaches glib's own copy of it.
     * An explicit D3DPT_D3D9 in the environment still wins, as above.
     */
    if (!GetEnvironmentVariableA("D3DPT_D3D9", NULL, 0)) {
        SetEnvironmentVariableA("D3DPT_D3D9", which);
    }
#endif
}

const D3dptExecLib *d3dpt_exec_lib(void)
{
    const char *env = getenv("D3DPT_EXEC_LIB");
    const char *candidates[] = {
        env,
#if defined(__APPLE__)
        "build/d3dpt/libd3dpt_exec.dylib", "libd3dpt_exec.dylib",
#elif defined(_WIN32)
        /* Bare name last: LoadLibrary searches the executable's own
         * directory first, which is where the package puts it. */
        "build/win/d3dpt/d3dpt_exec.dll", "d3dpt_exec.dll",
#else
        "build/d3dpt/libd3dpt_exec.so", "libd3dpt_exec.so",
#endif
    };

    if (tried) {
        return lib.handle ? &lib : NULL;
    }
    tried = true;
    if (refused) {
        /* the same sentence the real floor prints, so a log from a run with
         * this flag reads like a log from such a host */
        warn_report("d3dpt: no-exec=on: no Vulkan 1.3 device on this host; "
                    "Direct3D pass-through off");
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        if (!candidates[i]) {
            continue;
        }
        lib.handle = D3DPT_DLOPEN(candidates[i]);
        if (lib.handle) {
            info_report("d3dpt: executor %s", candidates[i]);
            break;
        }
    }
    if (!lib.handle) {
        warn_report("d3dpt: libd3dpt_exec not found (D3DPT_EXEC_LIB); Direct3D pass-through off");
        return NULL;
    }
    lib.version = D3DPT_DLSYM(lib.handle, "d3dpt_exec_version");
    lib.create = D3DPT_DLSYM(lib.handle, "d3dpt_exec_create");
    lib.destroy = D3DPT_DLSYM(lib.handle, "d3dpt_exec_destroy");
    lib.attach = D3DPT_DLSYM(lib.handle, "d3dpt_exec_attach");
    lib.submit = D3DPT_DLSYM(lib.handle, "d3dpt_exec_submit");
    lib.set_vram = D3DPT_DLSYM(lib.handle, "d3dpt_exec_set_vram");
    if (!lib.version || !lib.create || !lib.destroy || !lib.attach || !lib.submit || !lib.set_vram ||
        lib.version() != D3DPT_PROTO_VERSION) {
        warn_report("d3dpt: executor library mismatch (protocol %u, need %u)",
                    lib.version ? lib.version() : 0, D3DPT_PROTO_VERSION);
        D3DPT_DLCLOSE(lib.handle);
        memset(&lib, 0, sizeof(lib));
        return NULL;
    }
    return &lib;
}

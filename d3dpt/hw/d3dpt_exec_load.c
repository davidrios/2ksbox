/*
 * d3dpt_exec_load.c: dlopen libd3dpt_exec once for every d3dpt device
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

void d3dpt_exec_prefer_backend(const char *which)
{
    if (!which || !*which) {
        return;
    }
    if (strcmp(which, "auto") && strcmp(which, "dxvk") && strcmp(which, "wine") && strcmp(which, "none")) {
        warn_report("d3dpt: exec=%s is not auto, dxvk, wine or none; ignored", which);
        return;
    }
    g_setenv("D3DPT_EXEC", which, false);
}

/* open one candidate, resolve it, and ask it whether it has a device */
static bool try_lib(const char *path, const char *kind)
{
    lib.handle = D3DPT_DLOPEN(path);
    if (!lib.handle) {
        return false;
    }
    lib.version = D3DPT_DLSYM(lib.handle, "d3dpt_exec_version");
    lib.create = D3DPT_DLSYM(lib.handle, "d3dpt_exec_create");
    lib.destroy = D3DPT_DLSYM(lib.handle, "d3dpt_exec_destroy");
    lib.attach = D3DPT_DLSYM(lib.handle, "d3dpt_exec_attach");
    lib.submit = D3DPT_DLSYM(lib.handle, "d3dpt_exec_submit");
    lib.set_vram = D3DPT_DLSYM(lib.handle, "d3dpt_exec_set_vram");
    lib.probe = D3DPT_DLSYM(lib.handle, "d3dpt_exec_probe");
    lib.shared_alloc = D3DPT_DLSYM(lib.handle, "d3dpt_exec_shared_alloc");
    lib.shared_map = D3DPT_DLSYM(lib.handle, "d3dpt_exec_shared_map");
    lib.name = path;
    if (!lib.version || !lib.create || !lib.destroy || !lib.attach || !lib.submit || !lib.set_vram ||
        lib.version() != D3DPT_PROTO_VERSION) {
        warn_report("d3dpt: %s: executor library mismatch (protocol %u, need %u)", path,
                    lib.version ? lib.version() : 0, D3DPT_PROTO_VERSION);
        D3DPT_DLCLOSE(lib.handle);
        memset(&lib, 0, sizeof(lib));
        return false;
    }
    if (lib.probe && !lib.probe()) {
        info_report("d3dpt: %s (%s): no device on this host", path, kind);
        /* Left mapped, never closed: the probe ran DXVK, which keeps
         * worker threads and its own statics, and unloading it under
         * them took QEMU down with a segfault at realize on every host
         * without Vulkan. A library that found no device is
         * a few megabytes of address space, not a problem. */
        memset(&lib, 0, sizeof(lib));
        return false;
    }
    info_report("d3dpt: executor %s (%s)", path, kind);
    return true;
}

const D3dptExecLib *d3dpt_exec_lib(void)
{
    /* the in-process executor (DXVK: a Vulkan 1.3 device) ... */
    const char *env = getenv("D3DPT_EXEC_LIB");
    const char *in_process[] = {
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
    /* ... and the one in another process, on Wine (M15): the same API,
     * the same protocol, taken when the first has no device */
    const char *renv = getenv("D3DPT_EXEC_REMOTE_LIB");
    const char *remote[] = {
        renv,
#if defined(__APPLE__)
        "build/d3dpt/libd3dpt_exec_remote.dylib", "libd3dpt_exec_remote.dylib",
#elif defined(_WIN32)
        /* a Windows host below the floor has its own Direct3D 9 (d3d9=system) */
#else
        "build/d3dpt/libd3dpt_exec_remote.so", "libd3dpt_exec_remote.so",
#endif
    };
    const char *pick = getenv("D3DPT_EXEC");
    bool want_dxvk = !pick || !strcmp(pick, "auto") || !strcmp(pick, "dxvk");
    bool want_wine = !pick || !strcmp(pick, "auto") || !strcmp(pick, "wine");

    if (tried) {
        return lib.handle ? &lib : NULL;
    }
    tried = true;
    if (pick && !strcmp(pick, "none")) {
        refused = true;
    }
    if (refused) {
        /* the same sentence the real floor prints, so a log from a run with
         * this flag reads like a log from such a host */
        warn_report("d3dpt: no-exec=on: no Vulkan 1.3 device on this host; "
                    "Direct3D pass-through off");
        return NULL;
    }
    if (want_dxvk) {
        for (size_t i = 0; i < ARRAY_SIZE(in_process); i++) {
            if (in_process[i] && try_lib(in_process[i], "in process")) {
                return &lib;
            }
        }
    }
    if (want_wine) {
        for (size_t i = 0; i < ARRAY_SIZE(remote); i++) {
            if (remote[i] && try_lib(remote[i], "another process, Wine")) {
                return &lib;
            }
        }
    }
    warn_report("d3dpt: no executor%s (D3DPT_EXEC_LIB / D3DPT_EXEC_REMOTE_LIB; D3DPT_EXEC=%s); "
                "Direct3D pass-through off", env || renv ? "" : " library found", pick ? pick : "auto");
    return NULL;
}

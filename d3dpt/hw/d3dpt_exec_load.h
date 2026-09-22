/*
 * d3dpt_exec_load.h — the executor library (libd3dpt_exec, d3dpt/exec)
 * as the QEMU devices see it: dlopened once per process, its entry
 * points resolved and its protocol version checked. Shared by the SysBus
 * Direct3D device (d3dpt_mm.c, doc 14) and the d3dpt-vga display adapter
 * (d3dpt_vga.c, doc 15 M7c); each creates its own executor instance.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_D3DPT_EXEC_LOAD_H
#define HW_D3DPT_EXEC_LOAD_H

#include <stdint.h>
#include "hw/d3dpt/d3dpt_exec.h"

typedef struct D3dptExecLib {
    void *handle;
    uint32_t (*version)(void);
    d3dpt_exec_t *(*create)(const d3dpt_exec_ops *ops);
    void (*destroy)(d3dpt_exec_t *x);
    void (*attach)(d3dpt_exec_t *x, int attach);
    uint32_t (*submit)(d3dpt_exec_t *x, void *shm, uint32_t shm_size);
    void (*set_vram)(d3dpt_exec_t *x, void *vram, uint32_t size);
} D3dptExecLib;

/* the library, or NULL if it is missing / speaks another protocol (warned
 * once; D3DPT_EXEC_LIB overrides the search) */
const D3dptExecLib *d3dpt_exec_lib(void);

/* Act as a host that has no usable executor, so every d3dpt device answers
 * D3DPT_STATUS_NO_EXEC and the guest driver takes the fallback a real such
 * host gives it (ADR-013: below the Vulkan 1.3 floor — pre-Broadwell Intel,
 * Kepler and older, AMD TeraScale, every Intel Mac and macOS before 26 —
 * WineD3D in the guest, which needs no Vulkan at all). Set by
 * -device d3dpt-vga,no-exec=on before anything calls d3dpt_exec_lib(); it
 * is the whole host's property, not one device's, which is why it lives
 * here and the adapter's property only sets it.
 *
 * It refuses **before the library is opened at all**, so it is not a way
 * to reach a particular backend: nothing reads d3dpt_exec_prefer's answer
 * on a refused host, neither DXVK nor the system Direct3D 9 is tried, and
 * the guest sees no pass-through. Since 2026-09-21 that is a *Linux and
 * macOS* host below the floor; a Windows one below it runs the system
 * Direct3D 9 (ADR-007's second amendment), and `d3d9=system` is how that
 * host is met from one that has Vulkan. So: `no-exec=on` for no 3D at
 * all, `d3d9=system` for the other rasteriser. */
void d3dpt_exec_refuse(void);

/* Which Direct3D 9 implementation the executor is to run on: "dxvk"
 * (ADR-007's, and the only one whose frames are held against the rig
 * goldens), "system" (Windows' own, the fallback for a host below the
 * Vulkan 1.3 floor — ADR-007's 2026-09-21 amendment) or "auto", which is
 * DXVK with the system one behind it. Like no-exec this is the whole
 * host's answer and not one device's, so it is set here, before anything
 * creates an executor, and read by the library out of the environment
 * (D3DPT_D3D9, which an explicit one in the environment wins). Set by
 * -device d3dpt-vga,d3d9=<which>; the launcher writes it from its own
 * Vulkan probe, which is the only thing that can tell a software Vulkan
 * device from a real one. */
void d3dpt_exec_prefer(const char *which);

#endif

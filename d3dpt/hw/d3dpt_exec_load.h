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
 * here and the adapter's property only sets it. */
void d3dpt_exec_refuse(void);

#endif

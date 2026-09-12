/*
 * voodoo_shim.h -- the seam between the QEMU device (voodoo2.c) and the
 * shim that stands in for 86Box's platform (voodoo_shim.c). Not seen by the
 * vendored 86Box sources.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_H
#define VOODOO_SHIM_H

#include "shim/86box/video.h"

typedef struct VoodooShimHooks {
    void *opaque;
    /* fbiInit0's VGA_PASS bit: the Voodoo takes the monitor (1) or gives it
     * back to the 2D adapter (0). vCPU thread, BQL held. */
    void (*set_override)(void *opaque, int on);
    /* a frame is complete in the monitor bitmap: rows 0..h-1, w pixels of
     * 0xXXRRGGBB each. Main loop (the display timer), BQL held. */
    void (*present)(void *opaque, const bitmap_t *frame, int w, int h);
} VoodooShimHooks;

/* Install the hooks and the monitor bitmap; before voodoo_init(). */
void voodoo_shim_init(const VoodooShimHooks *hooks);
/* What device_get_config_int(name) will answer; before voodoo_init(). */
void voodoo_shim_set_config(const char *name, int value);

#endif /* VOODOO_SHIM_H */

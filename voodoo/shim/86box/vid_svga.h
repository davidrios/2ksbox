/*
 * vid_svga.h -- 2ksbox's stand-in for 86Box's <86box/vid_svga.h>. A Voodoo
 * 1/2 is a pass-through card: the 2D adapter's signal goes through it and,
 * with fbiInit0's VGA_PASS bit set, the Voodoo drives the monitor instead.
 * 86Box models that as the Voodoo "overriding" the primary SVGA; here the
 * override puts the guest console into pass-through (the VGA device stops
 * updating it, ui/console.c's graphic_hw_passthrough) and the Voodoo's
 * frames go into that console's surface (voodoo2.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_VID_SVGA_H
#define VOODOO_SHIM_VID_SVGA_H

#include "video.h"

typedef struct svga_t {
    int        override;
    monitor_t *monitor;
} svga_t;

extern svga_t *svga_get_pri(void);
extern void    svga_set_override(svga_t *svga, int val);
extern void    svga_recalctimings(svga_t *svga);
extern void    svga_doblit(int wx, int wy, svga_t *svga);

#endif /* VOODOO_SHIM_VID_SVGA_H */

/*
 * video.h -- 2ksbox's stand-in for 86Box's <86box/video.h>. The Voodoo's
 * display timer converts each dirty scanline of the front buffer into the
 * monitor's 32-bit target bitmap (0xXXRRGGBB); voodoo2.c copies that bitmap
 * into the guest console's surface when the frame is done (svga_doblit).
 *
 * The bitmap is sized for the register file, not for a Voodoo 2: h_disp
 * and v_disp are 12-bit fields a guest can set to anything, and
 * voodoo_callback indexes line[] and the row by them unchecked, so the
 * shim allocates 4096 x 4224 lazily (mmap, untouched pages cost nothing)
 * rather than trusting the guest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_VIDEO_H
#define VOODOO_SHIM_VIDEO_H

#include <stdint.h>

#define VOODOO_SHIM_BITMAP_W 4096
#define VOODOO_SHIM_BITMAP_H 4224

typedef struct bitmap_t {
    int       w;
    int       h;
    uint32_t *dat;
    uint32_t *line[VOODOO_SHIM_BITMAP_H];
} bitmap_t;

typedef struct monitor_t {
    int       mon_xsize;
    int       mon_ysize;
    bitmap_t *target_buffer;
    int       mon_overscan_x;
    int       mon_overscan_y;
    int       mon_renderedframes;
} monitor_t;

extern monitor_t monitors[1];
extern int       monitor_index_global;
extern double    cpuclock;

extern void video_wait_for_buffer_monitor(int monitor_index);
extern void video_clamp_vram(uint64_t bios_flags, int *size);

#endif /* VOODOO_SHIM_VIDEO_H */

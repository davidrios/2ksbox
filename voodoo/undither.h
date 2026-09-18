/*
 * undither.h -- the Voodoo's ordered dither, undone at scanout (doc 21 §12).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_UNDITHER_H
#define VOODOO_UNDITHER_H

#include <stdint.h>

struct voodoo_t;

/* Write the front buffer into dst as 32-bit pixels (B | G << 8 | R << 16,
 * through the card's CLUT -- the same pixels the ordinary path writes), with
 * the rasterizer's dither reconstructed away.
 *
 * Returns 1 when it did. Returns 0 and touches nothing when it cannot, with
 * *why naming the reason for the caller's one-shot log line; the caller then
 * copies 86Box's own scanlines as before.
 */
int voodoo_undither_frame(struct voodoo_t *v, uint8_t *dst, int dst_stride,
                          int w, int h, const char **why);

#endif

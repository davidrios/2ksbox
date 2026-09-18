/*
 * voodoo_vbfilter.h -- the Voodoo 3 / Banshee post filter over a Voodoo 2
 * (voodoo_vbfilter.c, doc 21 §13). Patch 72 declares voodoo_filterline_vb
 * itself at the one call site in the vendored display timer; this header
 * is for the device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_VBFILTER_H
#define VOODOO_VBFILTER_H

#include "86box/vid_voodoo_common.h"

/* scrfilter: what the display timer does with each scanline */
#define VOODOO_FILTER_OFF   0
#define VOODOO_FILTER_CARD  1   /* the Voodoo 1/2's own, 86Box's v1/v2 */
#define VOODOO_FILTER_4X1   2   /* the Voodoo 3's, four taps along the line */
#define VOODOO_FILTER_2X2   3   /* the Voodoo 3's, this line and the next */

void voodoo_filterline_vb(voodoo_t *voodoo, uint8_t *fil, int column,
                          uint16_t *src, int line, int mode);

#endif /* VOODOO_VBFILTER_H */

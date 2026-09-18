/*
 * voodoo_vbfilter.c -- the Voodoo 3 / Banshee "22-bit" post filter, run
 * over a Voodoo 2's scanlines (doc 21 §13).
 *
 * A 3dfx card of this era draws into a 16-bit frame buffer and hides the
 * banding with an ordered dither; the RAMDAC then takes the dither back
 * out on the way to the monitor, which is what 3dfx sold as "22-bit
 * colour". 86Box emulates that filter three times over: one table for the
 * Voodoo Graphics, one for the Voodoo 2 (both in the vendored
 * vid_voodoo_display.c, and `filter=v2` is the Voodoo 2's own), and a
 * third, later and better-tuned pair for the Banshee and Voodoo 3 --
 * which lives in vid_voodoo_banshee.c, a file this port does not vendor
 * (a Banshee is a 2D+3D card with its own VGA core, voodoo/86box/UPSTREAM).
 *
 * So the two things the Voodoo 3's filter is made of are ported here,
 * from 86Box's vid_voodoo_banshee.c at the commit in
 * voodoo/86box/UPSTREAM, same licence (GPL-2.0-or-later; the filters are
 * leilei's):
 *
 *   - voodoo_generate_vb_filters(), verbatim but for the table storage --
 *     the vendored display.c already declares and calls it (for a Banshee),
 *     and the shim used to stub it out;
 *   - voodoo_filterline_vb(), which is banshee_render_line()'s filter
 *     inner loops with the overlay, the chroma key and the scaling taken
 *     out: what is left is the filter itself, in the shape the vendored
 *     voodoo_filterline_v1/v2 have, so patch 72 can pick it at the one
 *     call site in the display timer.
 *
 * Both of the Voodoo 3's modes are here. **4x1** is the one 3dfx's
 * marketing called the 22-bit post filter: four taps along the scanline,
 * three passes leftwards and one rightwards, over a line whose even rows
 * get the "purple line" lift first. **2x2** samples this line and the
 * next, box-filters each and blends the two, which is softer and costs a
 * second scanline -- under SLI that line is on the other board, and this
 * finds it there (doc 21 §12).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "shim/86box/86box.h"
#include "shim/86box/device.h"
#include "shim/86box/mem.h"
#include "shim/86box/plat.h"
#include "shim/86box/thread.h"
#include "shim/86box/timer.h"
#include "shim/86box/video.h"
#include "shim/86box/vid_svga.h"
#include "86box/vid_voodoo_common.h"
#include "voodoo_vbfilter.h"

/* 86Box's names for these, kept */
static uint8_t vb_filter_v1_rb[256][256];
static uint8_t vb_filter_v1_g[256][256];
static uint8_t vb_filter_bx_rb[256][256];
static uint8_t vb_filter_bx_g[256][256];

/* ------------------------------------------------------------------------
 * 86Box vid_voodoo_banshee.c, voodoo_generate_vb_filters(): verbatim.
 * The vendored display.c calls this from voodoo_threshold_check() for a
 * Banshee, and patch 72 calls it for a Voodoo 2 asked for this filter.
 */
void
voodoo_generate_vb_filters(voodoo_t *voodoo, int fcr, int fcg)
{
    float difference;
    float diffg;
    float thiscol;
    float thiscolg;
    float clr;
    float clg  = 0;
    float hack = 1.0f;
    // pre-clamping

    fcr *= hack;
    fcg *= hack;

    /* box prefilter */
    for (uint16_t g = 0; g < 256; g++) {     // pixel 1 - our target pixel we want to bleed into
        for (uint16_t h = 0; h < 256; h++) { // pixel 2 - our main pixel
            float avg;
            float avgdiff;

            difference = (float) (g - h);
            avg        = g;
            avgdiff    = avg - h;

            avgdiff = avgdiff * 0.75f;
            if (avgdiff < 0)
                avgdiff *= -1;
            if (difference < 0)
                difference *= -1;

            thiscol = thiscolg = g;

            if (h > g) {
                clr = clg = avgdiff;

                if (clr > fcr)
                    clr = fcr;
                if (clg > fcg)
                    clg = fcg;

                thiscol  = g;
                thiscolg = g;

                if (thiscol > g + fcr)
                    thiscol = g + fcr;
                if (thiscolg > g + fcg)
                    thiscolg = g + fcg;

                if (thiscol > g + difference)
                    thiscol = g + difference;
                if (thiscolg > g + difference)
                    thiscolg = g + difference;

                // hmm this might not be working out..
                int ugh = g - h;
                if (ugh < fcr)
                    thiscol = h;
                if (ugh < fcg)
                    thiscolg = h;
            }

            if (difference > fcr)
                thiscol = g;
            if (difference > fcg)
                thiscolg = g;

            // clamp
            if (thiscol < 0)
                thiscol = 0;
            if (thiscolg < 0)
                thiscolg = 0;

            if (thiscol > 255)
                thiscol = 255;
            if (thiscolg > 255)
                thiscolg = 255;

            vb_filter_bx_rb[g][h] = thiscol;
            vb_filter_bx_g[g][h]  = thiscolg;
        }
        float lined = g + 4;
        if (lined > 255)
            lined = 255;
        voodoo->purpleline[g][0] = lined;
        voodoo->purpleline[g][2] = lined;

        lined = g + 0;
        if (lined > 255)
            lined = 255;
        voodoo->purpleline[g][1] = lined;
    }

    /* 4x1 and 2x2 filter */
    for (uint16_t g = 0; g < 256; g++) {     // pixel 1
        for (uint16_t h = 0; h < 256; h++) { // pixel 2
            difference = (float) (h - g);
            diffg      = difference;

            thiscol = thiscolg = g;

            if (difference > fcr)
                difference = fcr;
            if (difference < -fcr)
                difference = -fcr;

            if (diffg > fcg)
                diffg = fcg;
            if (diffg < -fcg)
                diffg = -fcg;

            if ((difference < fcr) || (-difference > -fcr))
                thiscol = g + (difference / 2);
            if ((diffg < fcg) || (-diffg > -fcg))
                thiscolg = g + (diffg / 2);

            if (thiscol < 0)
                thiscol = 0;
            if (thiscol > 255)
                thiscol = 255;

            if (thiscolg < 0)
                thiscolg = 0;
            if (thiscolg > 255)
                thiscolg = 255;

            vb_filter_v1_rb[g][h] = thiscol;
            vb_filter_v1_g[g][h]  = thiscolg;
        }
    }
}

/* ------------------------------------------------------------------ lines
 *
 * fil[] is the shape the vendored filters produce and the display timer
 * reads: three bytes a pixel -- blue, green, red, each 0..255 -- which the
 * timer then puts through the CLUT. The 16-bit source is expanded the way
 * the vendored voodoo_filterline_v2 expands it (5 and 6 bits to 8 by a
 * shift, no rounding: the filter's thresholds are in those units).
 *
 * The work is done in locals and copied into fil[] at the end, because
 * every pass reads one pixel past the line's end (86Box's reads off the
 * end of its overlay buffer, where the next line's first pixel happens to
 * sit) and the caller's buffer is exactly h_disp pixels wide.
 */
#define VB_MAX_COLUMN 4096

static void
vb_sample(uint8_t *dst, const uint16_t *src, int column)
{
    for (int x = 0; x < column; x++) {
        dst[x * 3]     = (src[x] & 31) << 3;
        dst[x * 3 + 1] = ((src[x] >> 5) & 63) << 2;
        dst[x * 3 + 2] = ((src[x] >> 11) & 31) << 3;
    }
    /* the edge pixel, for the pass that reads x + 1 */
    dst[column * 3]     = dst[(column - 1) * 3];
    dst[column * 3 + 1] = dst[(column - 1) * 3 + 1];
    dst[column * 3 + 2] = dst[(column - 1) * 3 + 2];
}

/* The scanline under the one being drawn, for the 2x2 filter: the next
 * display line, which under SLI lives on the other board (doc 21 §12) --
 * the display timer reads the two boards' memories the same way. The last
 * line of the frame filters against itself. */
static const uint16_t *
vb_next_line(voodoo_t *voodoo, const uint16_t *src, int line)
{
    voodoo_t *v   = voodoo;
    int       row = line + 1;

    if (line + 1 >= voodoo->v_disp) {
        return src;
    }
    if (voodoo->fbiInit1 & (1 << 23)) {     /* scanline interleaving */
        int master_owns = (((voodoo->initEnable & (1 << 11)) ? 1 : 0) == ((line + 1) & 1));

        v   = master_owns ? voodoo->set->voodoos[0] : voodoo->set->voodoos[1];
        row = (line + 1) >> 1;
        if (!v) {
            return src;
        }
    }
    return (const uint16_t *) &v->fb_mem[(v->front_offset + row * v->row_width) & v->fb_mask];
}

/*
 * 86Box vid_voodoo_banshee.c, banshee_render_line()'s two filtered cases,
 * with the overlay sampling, the chroma key and the horizontal scaling
 * taken out.
 *
 * mode 1 = 4x1 (VIDPROCCFG_FILTER_MODE_DITHER_4X4 there),
 * mode 2 = 2x2 (VIDPROCCFG_FILTER_MODE_DITHER_2X2).
 */
void
voodoo_filterline_vb(voodoo_t *voodoo, uint8_t *fil, int column,
                     uint16_t *src, int line, int mode)
{
    uint8_t one[(VB_MAX_COLUMN + 1) * 3];
    uint8_t two[(VB_MAX_COLUMN + 1) * 3];

    if (column < 2 || column > VB_MAX_COLUMN) {
        if (column > 0 && column <= VB_MAX_COLUMN) {
            vb_sample(one, src, column);
            memcpy(fil, one, (size_t) column * 3);
        }
        return;
    }

    if (mode == 2) {
        /* this line and the next, each box-filtered against its own right
         * neighbour, then the two blended together */
        vb_sample(one, src, column);
        vb_sample(two, vb_next_line(voodoo, src, line), column);
        for (int x = 0; x < column; x++) {
            uint8_t soak[3];
            uint8_t soak2[3];

            soak[0]  = vb_filter_bx_rb[one[x * 3]][one[(x + 1) * 3]];
            soak[1]  = vb_filter_bx_g[one[x * 3 + 1]][one[(x + 1) * 3 + 1]];
            soak[2]  = vb_filter_bx_rb[one[x * 3 + 2]][one[(x + 1) * 3 + 2]];

            soak2[0] = vb_filter_bx_rb[two[x * 3]][two[(x + 1) * 3]];
            soak2[1] = vb_filter_bx_g[two[x * 3 + 1]][two[(x + 1) * 3 + 1]];
            soak2[2] = vb_filter_bx_rb[two[x * 3 + 2]][two[(x + 1) * 3 + 2]];

            fil[x * 3]     = vb_filter_v1_rb[soak[0]][soak2[0]];
            fil[x * 3 + 1] = vb_filter_v1_g[soak[1]][soak2[1]];
            fil[x * 3 + 2] = vb_filter_v1_rb[soak[2]][soak2[2]];
        }
        return;
    }

    /* 4x1: the 22-bit post filter. Three passes leftwards and one
     * rightwards, over a line whose even rows get the purple-line lift
     * first (86Box's `fil` and `fil3` are `one` and `two` here). */
    vb_sample(one, src, column);
    memcpy(two, one, (size_t) (column + 1) * 3);
    if (line % 2 == 0) {
        for (int x = 0; x < column; x++) {
            one[x * 3]     = voodoo->purpleline[one[x * 3]][0];
            one[x * 3 + 1] = voodoo->purpleline[one[x * 3 + 1]][1];
            one[x * 3 + 2] = voodoo->purpleline[one[x * 3 + 2]][2];
        }
    }
    for (int x = 1; x < column; x++) {
        two[x * 3]     = vb_filter_v1_rb[one[x * 3]][one[(x - 1) * 3]];
        two[x * 3 + 1] = vb_filter_v1_g[one[x * 3 + 1]][one[(x - 1) * 3 + 1]];
        two[x * 3 + 2] = vb_filter_v1_rb[one[x * 3 + 2]][one[(x - 1) * 3 + 2]];
    }
    for (int x = 1; x < column; x++) {
        one[x * 3]     = vb_filter_v1_rb[one[x * 3]][two[(x - 1) * 3]];
        one[x * 3 + 1] = vb_filter_v1_g[one[x * 3 + 1]][two[(x - 1) * 3 + 1]];
        one[x * 3 + 2] = vb_filter_v1_rb[one[x * 3 + 2]][two[(x - 1) * 3 + 2]];
    }
    for (int x = 1; x < column; x++) {
        two[x * 3]     = vb_filter_v1_rb[one[x * 3]][one[(x - 1) * 3]];
        two[x * 3 + 1] = vb_filter_v1_g[one[x * 3 + 1]][one[(x - 1) * 3 + 1]];
        two[x * 3 + 2] = vb_filter_v1_rb[one[x * 3 + 2]][one[(x - 1) * 3 + 2]];
    }
    for (int x = 0; x < column; x++) {
        fil[x * 3]     = vb_filter_v1_rb[one[x * 3]][two[(x + 1) * 3]];
        fil[x * 3 + 1] = vb_filter_v1_g[one[x * 3 + 1]][two[(x + 1) * 3 + 1]];
        fil[x * 3 + 2] = vb_filter_v1_rb[one[x * 3 + 2]][two[(x + 1) * 3 + 2]];
    }
}

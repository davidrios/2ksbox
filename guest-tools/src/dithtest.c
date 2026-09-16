/*
 * DITHTEST.EXE — what repeated alpha blending does to a 16-bit frame
 * buffer, from inside the guest (doc 21 §9).
 *
 * A Voodoo writes 16-bit pixels with an ordered dither, so a pixel read
 * back for blending is not the colour that was meant: it is that colour
 * plus this position's dither offset. The chip therefore *subtracts* the
 * dither on the way in (86Box calls it "dither subtraction",
 * `-device voodoo2,dither-sub=on|off`). 86Box's plain interpreter does
 * that; **neither of its recompilers does** — `dithersub` appears in
 * neither codegen — and the recompiler is the default
 * (`-device voodoo2,recompiler=on|off`). One blend hides the difference;
 * this is what makes it visible.
 *
 * The scene: a grey that lies between representable levels in every
 * channel (130,130,130 — 5 bits for red and blue, 6 for green, so every
 * one of them has to dither), drawn as a full-screen quad through the
 * pixel pipeline so that it really is dithered. Then eight columns blend
 * **that same grey onto itself** at alpha 0.5, 1, 2, 4 ... 128 times. The
 * ideal result of blending a colour onto itself is that colour, so every
 * column should stay the background's grey and the strip across the top,
 * which nothing blends over, is the reference. Per-pass error accumulates
 * instead: the more passes, the further the column drifts.
 *
 * Each column's centre pixel is read back through grLfbLock and printed
 * with its drift from the reference, so a run answers in numbers as well
 * as on the screen, and dithtest.log keeps them.
 *
 *   DITHTEST             640x480, alpha 128, the frame held 15 s
 *   DITHTEST -alpha 64   another blend weight
 *   DITHTEST -hold 30    hold the frame longer (a screendump wants it up)
 *   DITHTEST -res 8      another resolution (0-15, glidewnd.c's table)
 *
 * Built by guest-tools/build-wrappers.sh into TESTS\ on the guest ISO.
 * It is a Glide 2.x program like GLIDETEST.EXE, so it runs on whatever
 * GLIDE2X.DLL the machine has: 3dfx's own driver on a machine with the
 * emulated Voodoo 2, which is the point here.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk2_glide.h"

#define COLUMNS 8

static FILE *logfp;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fflush(logfp);
    }
}

/* the grey everything is made of: between two representable levels in all
 * three channels (red and blue step by 8, green by 4), so the write path
 * has to dither it and the read path has something to subtract */
#define GREY_R 130
#define GREY_G 130
#define GREY_B 130

static void quad(float x0, float y0, float x1, float y1, float a)
{
    GrVertex v[4];
    int      i;

    memset(v, 0, sizeof(v));
    v[0].x = x0; v[0].y = y0;
    v[1].x = x1; v[1].y = y0;
    v[2].x = x1; v[2].y = y1;
    v[3].x = x0; v[3].y = y1;
    for (i = 0; i < 4; i++) {
        v[i].r   = (float) GREY_R;
        v[i].g   = (float) GREY_G;
        v[i].b   = (float) GREY_B;
        v[i].a   = a;
        v[i].oow = 1.f;
    }
    grDrawTriangle(&v[0], &v[1], &v[2]);
    grDrawTriangle(&v[0], &v[2], &v[3]);
}

/* 0xRRGGBB of a pixel in the locked buffer */
static unsigned pix_rgb(const GrLfbInfo_t *info, int x, int y)
{
    const unsigned char *row = (const unsigned char *) info->lfbPtr
                             + (size_t) y * info->strideInBytes;
    unsigned short       p   = ((const unsigned short *) row)[x];
    unsigned             r   = (p >> 11) & 0x1f;
    unsigned             g   = (p >> 5) & 0x3f;
    unsigned             b   = p & 0x1f;

    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

static const struct tbl { int w, h; } tblRes[] = {
    { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 },
    { 640, 200 }, { 640, 350 }, { 640, 400 }, { 640, 480 },
    { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 },
    { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 }, { 400, 300 },
};

int main(int argc, char **argv)
{
    int               res = GR_RESOLUTION_640x480, hold = 15, alpha = 128;
    int               close_it = 0;   /* -close: 3dfx's Glide can wedge there */
    int               i, c, w, h, colw, top, bot;
    int               passes[COLUMNS];
    GrHwConfiguration hw;
    GrLfbInfo_t       info;
    char              version[80] = "";
    unsigned          ref;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-res") && i + 1 < argc) {
            res = atoi(argv[++i]) & 0xf;
        } else if (!strcmp(argv[i], "-hold") && i + 1 < argc) {
            hold = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-alpha") && i + 1 < argc) {
            alpha = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-close")) {
            close_it = 1;
        }
    }
    w      = tblRes[res].w;
    h      = tblRes[res].h;
    colw   = w / COLUMNS;
    top    = h / 6;             /* the reference strip: nothing blends there */
    bot    = h - 1;
    logfp  = fopen("dithtest.log", "w");
    for (i = 0; i < COLUMNS; i++) {
        passes[i] = 1 << i;     /* 1, 2, 4 ... 128 */
    }

    grGlideInit();
    grGlideGetVersion(version);
    say("dithtest: Glide %s, %dx%d, alpha %d, grey %d,%d,%d\n",
        version, w, h, alpha, GREY_R, GREY_G, GREY_B);
    if (!grSstQueryHardware(&hw)) {
        say("dithtest: no Glide hardware\n");
        return 1;
    }
    grSstSelect(0);
    if (!grSstWinOpen(0, res, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                      GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        say("dithtest: grSstWinOpen(%d) refused\n", res);
        return 1;
    }

    say("dithtest: window open\n");
    /* the colour comes from the vertices, the alpha too */
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    /* the background, written once through the pixel pipeline: dithered,
     * which is what the blends below then read back */
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO,
                         GR_BLEND_ONE, GR_BLEND_ZERO);
    grBufferClear(0x00000000, 0, 0);
    quad(0.f, 0.f, (float) w, (float) h, 255.f);

    say("dithtest: background drawn\n");
    /* and the blends: the same grey onto itself, N times per column */
    grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                         GR_BLEND_ONE, GR_BLEND_ZERO);
    for (c = 0; c < COLUMNS; c++) {
        float x0 = (float) (c * colw);
        float x1 = (float) ((c + 1) * colw);

        for (i = 0; i < passes[c]; i++) {
            quad(x0, (float) top, x1, (float) bot, (float) alpha);
        }
        say("dithtest: column %d, %d passes drawn\n", c, passes[c]);
    }
    grBufferSwap(0);
    say("dithtest: swapped\n");

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER,
                   GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE,
                   &info)) {
        say("dithtest: grLfbLock refused\n");
        grGlideShutdown();
        return 1;
    }
    say("dithtest: buffer locked\n");
    ref = pix_rgb(&info, w / 2, top / 2);    /* the unblended strip */
    say("dithtest: reference (no blending) %06x\n", ref);
    say("dithtest: passes   pixel    dR  dG  dB   (drift from the reference)\n");
    for (c = 0; c < COLUMNS; c++) {
        unsigned got = pix_rgb(&info, c * colw + colw / 2, (top + bot) / 2);
        int      dr  = (int) ((got >> 16) & 0xff) - (int) ((ref >> 16) & 0xff);
        int      dg  = (int) ((got >> 8) & 0xff) - (int) ((ref >> 8) & 0xff);
        int      db  = (int) (got & 0xff) - (int) (ref & 0xff);

        say("dithtest: %6d   %06x  %+3d %+3d %+3d\n", passes[c], got, dr, dg, db);
    }
    grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER);

    /* the log is finished before the frame is held: 3dfx's own Glide 2.x
     * can wedge in grSstWinClose (doc 21, the teardown bug), and a run that
     * hangs there must still leave its numbers on the disk */
    say("dithtest: done\n");
    if (logfp) {
        fclose(logfp);
        logfp = NULL;
    }
    if (hold > 0) {
        Sleep(hold * 1000);
    }
    if (close_it) {
        grSstWinClose();
        grGlideShutdown();
    }
    return 0;
}

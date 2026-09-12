/*
 * GLIDE3TEST.EXE — Glide 3 through the pass-through device, from inside the
 * guest (docs/tracks/m14-glide3.md).
 *
 * GLIDETEST's Glide 3 counterpart and the guest half of
 * `tools/glide-host-test 3`: the same scenes, through GLIDE3X.DLL in the
 * guest, the MMIO FIFO, hw/3dfx's dispatcher and the Glide 3 layer of our
 * OpenGLide build on the host. Every scene is read back through grLfbLock
 * and checked here, so a host that drew nothing cannot pass.
 *
 * What only a guest adds is the transport. The guest DLL copies each vertex
 * across by the layout it was told (grVertexLayout), GR_PARAM by GR_PARAM,
 * so a layout the two sides disagree about arrives as the wrong bytes; the
 * texture tables and sizes go through its own size arithmetic; and
 * grGetString is answered from strings the DLL copied out of the host at
 * grGlideInit.
 *
 * Cases:
 *   1. grGet and grGetString: one board, one TMU, a Glide 3 version string
 *   2. a game's own vertex (x, y, 1/w, packed ARGB) through grDrawVertexArray
 *   3. a fan, and a strip whose second half only exists if
 *      GR_TRIANGLE_STRIP_CONTINUE carried the last call's vertices over
 *   4. clip coordinates (x, y, w, colour in [0,1]) through the viewport
 *   5. a 32x16 texture in Glide 3's log2 LOD/aspect encoding, its four
 *      quadrants each a different colour, drawn and sampled
 *   6. grSstWinClose then grSstWinOpen: a game's mode switch
 *
 * Prints "glide3test: N cases, M failed" as its last line and writes
 * glide3test.log beside itself. Console program; exit code 0 = all passed.
 *
 *   GLIDE3TEST            640x480
 *   GLIDE3TEST -res 8     another resolution (0-15, glidewnd.c's table)
 *   GLIDE3TEST -hold 5    keep the last frame up for 5 s, to look at it
 *
 * Built by guest-tools/build-wrappers.sh into TESTS\ on the guest ISO;
 * needs GLIDE3X.DLL installed (SETUP.EXE's Glide component).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk2_glide.h"
#define GLIDE3_GUEST_PROTOTYPES
#include "glide3.h"

static FILE *logfp;
static int cases, failed;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fflush(logfp);
    }
}

/* 0xRRGGBB of a pixel read out of the locked buffer, whatever the depth */
static unsigned pix_rgb(const GrLfbInfo_t *info, int w, int x, int y)
{
    const unsigned char *row = (const unsigned char *)info->lfbPtr
                               + (size_t)y * info->strideInBytes;
    if (info->strideInBytes >= (FxU32)w * 4) {
        return ((const unsigned *)row)[x] & 0xffffffu;
    }
    {
        unsigned short p = ((const unsigned short *)row)[x];
        unsigned r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
        return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
    }
}

/* 5 % per channel: 565 quantisation, and the host may dither */
static int near_rgb(unsigned got, unsigned want)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int g = (got >> i) & 0xff, w = (want >> i) & 0xff;
        if (abs(g - w) > 13) {
            return 0;
        }
    }
    return 1;
}

struct probe { int x, y; unsigned want; const char *what; };

static void check(const char *name, int w, const struct probe *p, int n)
{
    GrLfbInfo_t info;
    int i, bad = 0;

    cases++;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER,
                   GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
        failed++;
        say("  %-10s FAIL grLfbLock refused\n", name);
        return;
    }
    for (i = 0; i < n; i++) {
        unsigned got = pix_rgb(&info, w, p[i].x, p[i].y);
        int ok = near_rgb(got, p[i].want);
        bad += !ok;
        say("  %-10s %-16s (%3d,%3d) %06x want %06x  %s\n", i ? "" : name,
            p[i].what, p[i].x, p[i].y, got, p[i].want, ok ? "ok" : "WRONG");
    }
    grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER);
    if (bad) {
        failed++;
    }
}

static void check_value(const char *name, int ok, const char *fmt, ...)
{
    va_list ap;
    char line[160];

    cases++;
    failed += !ok;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    say("  %-10s %s  %s\n", name, line, ok ? "ok" : "WRONG");
}

static void flat_shading(void)
{
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
}

/* the upper-left half, green on red, from a packed 16-byte vertex */
static void scene_layout(int w, int h)
{
    struct { float x, y, q; unsigned argb; } t[3];
    void *ptrs[3];
    int i;

    t[0].x = 0.f;              t[0].y = 0.f;
    t[1].x = (float)(w - 1);   t[1].y = 0.f;
    t[2].x = 0.f;              t[2].y = (float)(h - 1);
    for (i = 0; i < 3; i++) {
        t[i].q = 1.f;
        t[i].argb = 0xff00ff00u;
        ptrs[i] = &t[i];
    }
    grReset(GR_VERTEX_PARAMETER);
    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q, 8, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_PARGB, 12, GR_PARAM_ENABLE);
    grBufferClear(0x0000ff, 0, 0);
    grDrawVertexArray(GR_TRIANGLES, 3, ptrs);
}

int main(int argc, char **argv)
{
    int res = GR_RESOLUTION_640x480, hold = 0, i, w, h;
    static const struct { int w, h; } tblRes[] = {
        { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 },
        { 640, 200 }, { 640, 350 }, { 640, 400 }, { 640, 480 },
        { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 },
        { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 }, { 400, 300 },
    };
    GrContext_t ctx;
    FxI32 v[4];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-res") && i + 1 < argc) {
            res = atoi(argv[++i]) & 0xf;
        } else if (!strcmp(argv[i], "-hold") && i + 1 < argc) {
            hold = atoi(argv[++i]);
        }
    }
    w = tblRes[res].w;
    h = tblRes[res].h;
    logfp = fopen("glide3test.log", "w");

    /* 1. what the "hardware" says, before and after grGlideInit */
    v[0] = 0;
    if (grGet(GR_NUM_BOARDS, 4, v) != 4 || v[0] < 1) {
        say("glide3test: no Glide 3 hardware (grGet(GR_NUM_BOARDS) = %d) — is "
            "GLIDE3X.DLL the pass-through wrapper?\n", (int)v[0]);
        say("glide3test: 0 cases, 1 failed\n");
        return 1;
    }
    grGlideInit();
    {
        const char *ver = grGetString(GR_VERSION);
        const char *hw = grGetString(GR_HARDWARE);
        v[0] = v[1] = 0;
        grGet(GR_NUM_TMU, 4, v);
        grGet(GR_MAX_TEXTURE_SIZE, 4, v + 1);
        say("glide3test: Glide %s on \"%s\", resolution %d = %dx%d\n",
            ver ? ver : "?", hw ? hw : "?", res, w, h);
        check_value("grGet", ver && ver[0] == '3' && v[0] >= 1 && v[1] == 256,
                    "version %s, %d TMU, textures up to %d", ver ? ver : "?",
                    (int)v[0], (int)v[1]);
    }
    grSstSelect(0);
    ctx = g3SstWinOpen(0, res, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                       GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (!ctx) {
        say("glide3test: grSstWinOpen(%d) failed — the host refused a drawable\n", res);
        say("glide3test: %d cases, %d failed\n", cases, failed + 1);
        return 1;
    }
    flat_shading();

    /* 2. a game's own vertex */
    {
        struct probe p[] = {
            { 10, 10, 0x00ff00, "in triangle" },
            { 0, 0, 0xff0000, "outside" },
            { 0, 0, 0xff0000, "below hypotenuse" },
        };
        p[1].x = w - 10; p[1].y = h - 10;
        p[2].x = w / 8;  p[2].y = h - h / 8;
        scene_layout(w, h);
        grBufferSwap(0);
        check("layout", w, p, 3);
    }

    /* 3. a fan on the left, a strip continued across two calls on the right */
    {
        struct vtx { float x, y, r, g, b; };
        struct vtx fan[4], top[4], bottom[2];
        float mx = (float)(w / 2), my = (float)(h / 2), fw_ = (float)w, fh_ = (float)h;
        struct probe p[3];

        memset(fan, 0, sizeof(fan));
        fan[1].x = mx; fan[2].x = mx; fan[2].y = fh_; fan[3].y = fh_;
        for (i = 0; i < 4; i++) {
            fan[i].g = 255.f;
        }
        top[0].x = mx;  top[0].y = 0.f;
        top[1].x = fw_; top[1].y = 0.f;
        top[2].x = mx;  top[2].y = my;
        top[3].x = fw_; top[3].y = my;
        bottom[0].x = mx;  bottom[0].y = fh_;
        bottom[1].x = fw_; bottom[1].y = fh_;
        for (i = 0; i < 4; i++) {
            top[i].r = top[i].g = top[i].b = 255.f;
        }
        for (i = 0; i < 2; i++) {
            bottom[i].r = bottom[i].g = bottom[i].b = 255.f;
        }
        grReset(GR_VERTEX_PARAMETER);
        grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout(GR_PARAM_RGB, 8, GR_PARAM_ENABLE);
        grBufferClear(0xff0000, 0, 0);   /* blue */
        grDrawVertexArrayContiguous(GR_TRIANGLE_FAN, 4, fan, sizeof(fan[0]));
        grDrawVertexArrayContiguous(GR_TRIANGLE_STRIP, 4, top, sizeof(top[0]));
        grDrawVertexArrayContiguous(GR_TRIANGLE_STRIP_CONTINUE, 2, bottom,
                                    sizeof(bottom[0]));
        grBufferSwap(0);
        p[0].x = w / 4;          p[0].y = h / 2;          p[0].want = 0x00ff00; p[0].what = "fan";
        p[1].x = 3 * w / 4;      p[1].y = h / 4;          p[1].want = 0xffffff; p[1].what = "strip";
        p[2].x = w - w / 16;     p[2].y = h - h / 16;     p[2].want = 0xffffff; p[2].what = "continued";
        check("fan/strip", w, p, 3);
    }

    /* 4. clip coordinates, w = 2: the lower-right half in blue */
    {
        struct { float x, y, w, r, g, b; } t[3] = {
            { 2.f, -2.f, 2.f, 0.f, 0.f, 1.f },
            { 2.f, 2.f, 2.f, 0.f, 0.f, 1.f },
            { -2.f, 2.f, 2.f, 0.f, 0.f, 1.f },
        };
        struct probe p[4];
        grReset(GR_VERTEX_PARAMETER);
        grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout(GR_PARAM_W, 8, GR_PARAM_ENABLE);
        grVertexLayout(GR_PARAM_RGB, 12, GR_PARAM_ENABLE);
        grCoordinateSpace(GR_CLIP_COORDS);
        grViewport(0, 0, w, h);
        grBufferClear(0x0000ff, 0, 0);
        grDrawVertexArrayContiguous(GR_TRIANGLES, 3, t, sizeof(t[0]));
        grBufferSwap(0);
        grCoordinateSpace(GR_WINDOW_COORDS);
        p[0].x = w - 10;  p[0].y = h - 10;     p[0].want = 0x0000ff; p[0].what = "lower right";
        p[1].x = w / 2;   p[1].y = 5 * h / 6;  p[1].want = 0x0000ff; p[1].what = "inside";
        p[2].x = 10;      p[2].y = 10;         p[2].want = 0xff0000; p[2].what = "upper left";
        p[3].x = w / 2;   p[3].y = h / 5;      p[3].want = 0xff0000; p[3].what = "outside";
        check("clip", w, p, 4);
    }

    /* 5. a 32x16 texture in Glide 3's encoding: GR_LOD_LOG2_32 is Glide 2's
     *    GR_LOD_8 untranslated, and GR_ASPECT_LOG2_2x1 its 4x1 */
    {
        static unsigned short texels[16][32];
        GrTexInfo info;
        FxU32 need, start;
        int x, y;
        struct { float x, y, q, s, t; } q[4];
        struct probe p[4];

        for (y = 0; y < 16; y++) {
            for (x = 0; x < 32; x++) {
                texels[y][x] = y < 8 ? (x < 16 ? 0x001f : 0xffe0)
                                     : (x < 16 ? 0x07e0 : 0xf81f);
            }
        }
        info.smallLod = GR_LOD_LOG2_32;
        info.largeLod = GR_LOD_LOG2_32;
        info.aspectRatio = GR_ASPECT_LOG2_2x1;
        info.format = GR_TEXFMT_RGB_565;
        info.data = texels;
        need = grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &info);
        check_value("texmem", need == 1024, "a 32x16 565 texture needs %u bytes (want 1024)",
                    (unsigned)need);
        start = grTexMinAddress(GR_TMU0);
        grTexDownloadMipMap(GR_TMU0, start, GR_MIPMAPLEVELMASK_BOTH, &info);
        grTexSource(GR_TMU0, start, GR_MIPMAPLEVELMASK_BOTH, &info);
        grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED,
                        GR_TEXTUREFILTER_POINT_SAMPLED);
        grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
        grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);
        grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
        grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                       GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        memset(q, 0, sizeof(q));
        q[1].x = (float)w;                      q[1].s = 256.f;
        q[2].x = (float)w; q[2].y = (float)h;   q[2].s = 256.f; q[2].t = 128.f;
        q[3].y = (float)h;                      q[3].t = 128.f;
        for (i = 0; i < 4; i++) {
            q[i].q = 1.f;
        }
        grReset(GR_VERTEX_PARAMETER);
        grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout(GR_PARAM_Q, 8, GR_PARAM_ENABLE);
        grVertexLayout(GR_PARAM_ST0, 12, GR_PARAM_ENABLE);
        grBufferClear(0, 0, 0);
        grDrawVertexArrayContiguous(GR_TRIANGLE_FAN, 4, q, sizeof(q[0]));
        grBufferSwap(0);
        p[0].x = w / 4;     p[0].y = h / 4;     p[0].want = 0x0000ff; p[0].what = "top left";
        p[1].x = 3 * w / 4; p[1].y = h / 4;     p[1].want = 0xffff00; p[1].what = "top right";
        p[2].x = w / 4;     p[2].y = 3 * h / 4; p[2].want = 0x00ff00; p[2].what = "bottom left";
        p[3].x = 3 * w / 4; p[3].y = 3 * h / 4; p[3].want = 0xff00ff; p[3].what = "bottom right";
        check("texture", w, p, 4);
        flat_shading();
    }

    if (hold > 0) {
        Sleep(hold * 1000);
    }

    /* 6. a mode switch: close, open again, draw again */
    g3SstWinClose(ctx);
    ctx = g3SstWinOpen(0, res, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                       GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (!ctx) {
        cases++;
        failed++;
        say("  %-10s FAIL the second grSstWinOpen was refused\n", "reopen");
    } else {
        struct probe p[] = {
            { 10, 10, 0x00ff00, "in triangle" },
            { 0, 0, 0xff0000, "outside" },
        };
        p[1].x = w - 10; p[1].y = h - 10;
        flat_shading();
        scene_layout(w, h);
        grBufferSwap(0);
        check("reopen", w, p, 2);
        g3SstWinClose(ctx);
    }

    grGlideShutdown();
    say("glide3test: %d cases, %d failed\n", cases, failed);
    if (logfp) {
        fclose(logfp);
    }
    return failed ? 1 : 0;
}

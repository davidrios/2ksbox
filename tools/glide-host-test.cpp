/*
 * Glide pass-through end to end without a guest (doc 12 §5): the real
 * host-side wrapper (build/glide/libglide2x.so), loaded by hw/3dfx's own
 * dispatcher, opened through glidewnd.c's own handshake, rendering into
 * the embed library's window-less context, with the frame checked where
 * the player would receive it.
 *
 * What each stage proves:
 *
 *   1. init_glide2x()  — the wrapper is found (QEMU_GLIDE_LIB), its 121
 *      Glide entry points resolve undecorated, and patch 33 hands it the
 *      GlideHostOps table the embed provider returns.
 *   2. init_window() + stat_window() — glidewnd.c's resolution encoding
 *      and the provider's window_stat run the wrapper's real
 *      grSstWinOpen through cwnd_glide2x, on a context nobody has a
 *      window for. This is the exact sequence glidept_mm.c performs when
 *      a guest calls grSstWinOpen; only the MMIO decode is missing.
 *   3. a clear and a triangle through the wrapper, then grBufferSwap —
 *      on_3d_frame arrives at the frontend with the drawn pixels, so the
 *      whole path from a Glide call to the player's texture is closed.
 *   4. fini_window()/fini_glide2x() — the context is given back and the
 *      frontend is told 3D is over.
 *
 * `glide-host-test 3` asks the same of a glide3x.dll guest instead
 * (docs/tracks/m14-glide3.md): the dispatcher loads the wrapper as Glide 3,
 * which makes it prefer every wrap3x_ entry point, and every export of the
 * guest's glide3x.dll must then resolve to something. Then the scenes
 * GLIDE3TEST.EXE draws in a guest, each checked in the frame: a game's own
 * packed vertex through grVertexLayout, a fan and a strip continued across
 * two calls, clip coordinates through the viewport, a 32x16 texture whose
 * size is right only if the log2 LOD and aspect were translated, and the
 * Glide 3 state surviving grGlideGetState / grGlideSetState.
 *
 * C++ because the Glide SDK header is (sdk2_3dfx.h includes <cstdint>).
 * Linux only (EGL backend). Build & run from the repo root:
 *   c++ -O1 -std=c++17 -Iembed -Ithird_party/openglide -Iglidept -Iqemu/hw/3dfx \
 *      -o build/glide-host-test tools/glide-host-test.cpp \
 *      -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,$PWD/build/qemu -ldl \
 *   && QEMU_GLIDE_LIB=$PWD/build/glide/libglide2x.so build/glide-host-test [3]
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdk2_glide.h"   /* the wrapper's own ABI, from third_party/openglide */
#include "g2xfuncs.h"     /* FEnum_*, from the prepared qemu/hw/3dfx */
#include "glide3.h"       /* what Glide 3 added, from glidept/ */
#include "libqemu_embed.h"

/* hw/3dfx internals, exported by the embed library on Linux */
typedef struct {
    int activate;
    uint32_t *arg;
    uint32_t FEnum;
    uintptr_t GrContext;
} window_cb;
extern "C" {
int init_glide2x(const char *dllname);
void fini_glide2x(void);
void init_window(const int res, const char *title, void *opaque);
int stat_window(const int res, void *opaque);
void fini_window(void *opaque);
const void *glide_host_ops(void);
}

#define GR_RESOLUTION_640x480 0x7

static int actives, frames, fw, fh;
static uint32_t px_c, px_tl, px_br;
static uint32_t *last_frame;

/* GLIDE_TEST_BMP=<path> writes the frame out, for looking at a failure */
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t n = (uint32_t)w * h * 4, sz = n + 54;
    FILE *f = fopen(path, "wb");
    int y, x;

    if (!f) {
        return;
    }
    memcpy(hdr + 2, &sz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 32;
    memcpy(hdr + 34, &n, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = h - 1; y >= 0; y--) {          /* BMP rows run bottom-up */
        for (x = 0; x < w; x++) {
            uint32_t p = px[y * w + x] | 0xff000000u;
            fwrite(&p, 4, 1, f);
        }
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void on_3d_active(void *ud, bool on)
{
    actives++;
    printf("on_3d_active(%d)\n", on);
}

static void on_3d_frame(void *ud, const uint8_t *p, int w, int h, int stride)
{
    const uint32_t *row = (const uint32_t *)p;
    frames++;
    fw = w;
    fh = h;
    px_c = row[(h / 2) * (stride / 4) + (w / 2)];
    px_tl = row[10 * (stride / 4) + 10];
    px_br = row[(h - 10) * (stride / 4) + (w - 10)];
    last_frame = (uint32_t *)realloc(last_frame, (size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        memcpy(last_frame + (size_t)y * w, p + (size_t)y * stride, (size_t)w * 4);
    }
}

/* the wrapper's entry points, looked up the way hw/3dfx looks them up */
static void *wrapper;
static void *sym(const char *name)
{
    void *p = dlsym(wrapper, name);
    if (!p) {
        printf("  wrapper has no %s\n", name);
    }
    return p;
}

/* ...and for a glide3x.dll guest, where wrap3x_<name> comes first */
static void *sym3(const char *name)
{
    char w[128];
    snprintf(w, sizeof(w), "wrap3x_%s", name);
    void *p = dlsym(wrapper, w);
    return p ? p : sym(name);
}

/* 0xRRGGBB at (x, y) of the last frame, row 0 at the top */
static uint32_t pixel(int x, int y)
{
    return last_frame ? last_frame[(size_t)y * fw + x] & 0xffffff : 0;
}

/* 565 quantisation: 0xf8 is what a full channel comes back as */
static bool near_rgb(uint32_t got, uint32_t want)
{
    for (int i = 0; i < 24; i += 8) {
        int g = (got >> i) & 0xff, w = (want >> i) & 0xff;
        if (abs(g - w) > 13) {
            return false;
        }
    }
    return true;
}

struct probe { int x, y; uint32_t want; const char *what; };

static int g3_cases, g3_failed;

static void check_frame(const char *name, const probe *p, int n)
{
    int bad = 0;
    g3_cases++;
    for (int i = 0; i < n; i++) {
        uint32_t got = pixel(p[i].x, p[i].y);
        bool ok = near_rgb(got, p[i].want);
        bad += !ok;
        printf("  %-10s %-16s (%3d,%3d) %06x want %06x  %s\n", i ? "" : name,
               p[i].what, p[i].x, p[i].y, got, p[i].want, ok ? "ok" : "WRONG");
    }
    g3_failed += bad != 0;
}

static void check_value(const char *name, bool ok, const char *fmt, ...)
{
    va_list ap;
    g3_cases++;
    g3_failed += !ok;
    printf("  %-10s ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("  %s\n", ok ? "ok" : "WRONG");
}

/*
 * Every export of qemu-3dfx's guest glide3x.dll (its glide3x.def). The
 * guest DLL forwards each one to hw/3dfx, which calls whatever this lookup
 * finds — and blocks the call as a null pointer when it finds nothing, so a
 * name missing here is a function a Glide 3 game calls into the void.
 */
static const char *const glide3x_exports[] = {
    "grAADrawTriangle", "grAlphaBlendFunction", "grAlphaCombine",
    "grAlphaControlsITRGBLighting", "grAlphaTestFunction",
    "grAlphaTestReferenceValue", "grBufferClear", "grBufferSwap",
    "grCheckForRoom", "grChromakeyMode", "grChromakeyValue", "grClipWindow",
    "grColorCombine", "grColorMask", "grConstantColorValue",
    "grCoordinateSpace", "grCullMode", "grDepthBiasLevel",
    "grDepthBufferFunction", "grDepthBufferMode", "grDepthMask",
    "grDepthRange", "grDisable", "grDisableAllEffects", "grDitherMode",
    "grDrawLine", "grDrawPoint", "grDrawTriangle", "grDrawVertexArray",
    "grDrawVertexArrayContiguous", "grEnable", "grErrorSetCallback",
    "grFinish", "grFlush", "grFogColorValue", "grFogMode", "grFogTable",
    "grGet", "grGetProcAddress", "grGetString", "grGlideGetState",
    "grGlideGetVertexLayout", "grGlideInit", "grGlideSetState",
    "grGlideSetVertexLayout", "grGlideShutdown", "grLfbConstantAlpha",
    "grLfbConstantDepth", "grLfbLock", "grLfbReadRegion", "grLfbUnlock",
    "grLfbWriteColorFormat", "grLfbWriteColorSwizzle", "grLfbWriteRegion",
    "grLoadGammaTable", "grQueryResolutions", "grRenderBuffer", "grReset",
    "grSelectContext", "grSplash", "grSstConfigPipeline", "grSstOrigin",
    "grSstSelect", "grSstVidMode", "grSstWinClose", "grSstWinOpen",
    "grTexCalcMemRequired", "grTexClampMode", "grTexCombine",
    "grTexDetailControl", "grTexDownloadMipMap", "grTexDownloadMipMapLevel",
    "grTexDownloadMipMapLevelPartial", "grTexDownloadTable",
    "grTexDownloadTablePartial", "grTexFilterMode", "grTexLodBiasValue",
    "grTexMaxAddress", "grTexMinAddress", "grTexMipMapMode", "grTexMultibase",
    "grTexMultibaseAddress", "grTexNCCTable", "grTexSource",
    "grTexTextureMemRequired", "grVertexLayout", "grViewport",
    "gu3dfGetInfo", "gu3dfLoad", "guFogGenerateExp", "guFogGenerateExp2",
    "guFogGenerateLinear", "guFogTableIndexToW", "guGammaCorrectionRGB",
};

/* the names whose Glide 3 meaning differs, which must be ours */
static const char *const glide3x_rewritten[] = {
    "grGlideInit", "grSstWinOpen", "grSstWinClose", "grDrawPoint",
    "grDrawLine", "grDrawTriangle", "grAADrawTriangle", "grTexSource",
    "grTexDownloadMipMap", "grTexTextureMemRequired", "grTexCalcMemRequired",
    "grTexDownloadMipMapLevel", "grTexDownloadMipMapLevelPartial",
    "grTexDownloadTable", "grTexDownloadTablePartial", "grTexNCCTable",
    "grLfbWriteRegion", "grGlideGetState", "grGlideSetState",
    "gu3dfGetInfo", "gu3dfLoad",
};

static int run_glide3(const char *lib)
{
    window_cb disp = { };
    uint32_t arg[16] = { 0 };
    int stat, missing = 0;
    FxI32 v[4];

    if (init_glide2x("glide3x.dll") != 0) {
        printf("init_glide2x(glide3x.dll) failed: %s not loadable\n", lib);
        return 1;
    }
    printf("wrapper loaded as Glide 3: %s\n", lib);
    wrapper = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
    if (!wrapper) {
        printf("the wrapper is loaded but not by this name?\n");
        return 1;
    }

    /* 1. the guest DLL's whole surface has somewhere to go */
    for (const char *n : glide3x_exports) {
        char w[128];
        snprintf(w, sizeof(w), "wrap3x_%s", n);
        if (!dlsym(wrapper, w) && !dlsym(wrapper, n)) {
            printf("  no host implementation of %s\n", n);
            missing++;
        }
    }
    for (const char *n : glide3x_rewritten) {
        char w[128];
        snprintf(w, sizeof(w), "wrap3x_%s", n);
        if (!dlsym(wrapper, w)) {
            printf("  %s would be Glide 2's: no %s\n", n, w);
            missing++;
        }
    }
    check_value("exports", missing == 0, "%zu glide3x.dll entry points, %d unresolved",
                sizeof(glide3x_exports) / sizeof(*glide3x_exports), missing);

    auto grGet_ = (FxU32 (*)(FxU32, FxU32, FxI32 *))sym3("grGet");
    auto grGetString_ = (const char *(*)(FxU32))sym3("grGetString");
    auto grGlideInit_ = (void (*)(void))sym3("grGlideInit");
    auto grBufferClear_ = (void (*)(GrColor_t, GrAlpha_t, FxU32))sym3("grBufferClear");
    auto grBufferSwap_ = (void (*)(FxU32))sym3("grBufferSwap");
    auto grColorCombine_ =
        (void (*)(GrCombineFunction_t, GrCombineFactor_t, GrCombineLocal_t,
                  GrCombineOther_t, FxBool))sym3("grColorCombine");
    auto grVertexLayout_ = (void (*)(FxU32, FxI32, FxU32))sym3("grVertexLayout");
    auto grReset_ = (FxBool (*)(FxU32))sym3("grReset");
    auto grDrawVertexArray_ = (void (*)(FxU32, FxU32, void *))sym3("grDrawVertexArray");
    auto grDrawVertexArrayContiguous_ =
        (void (*)(FxU32, FxU32, void *, FxU32))sym3("grDrawVertexArrayContiguous");
    auto grCoordinateSpace_ = (void (*)(FxU32))sym3("grCoordinateSpace");
    auto grViewport_ = (void (*)(FxI32, FxI32, FxI32, FxI32))sym3("grViewport");
    auto grGlideGetState_ = (void (*)(void *))sym3("grGlideGetState");
    auto grGlideSetState_ = (void (*)(const void *))sym3("grGlideSetState");
    auto grTexMinAddress_ = (FxU32 (*)(GrChipID_t))sym3("grTexMinAddress");
    auto grTexTextureMemRequired_ = (FxU32 (*)(FxU32, GrTexInfo *))sym3("grTexTextureMemRequired");
    auto grTexCalcMemRequired_ =
        (FxU32 (*)(GrLOD_t, GrLOD_t, GrAspectRatio_t, GrTextureFormat_t))sym3("grTexCalcMemRequired");
    auto grTexDownloadMipMap_ =
        (void (*)(GrChipID_t, FxU32, FxU32, GrTexInfo *))sym3("grTexDownloadMipMap");
    auto grTexSource_ = (void (*)(GrChipID_t, FxU32, FxU32, GrTexInfo *))sym3("grTexSource");
    auto grTexFilterMode_ =
        (void (*)(GrChipID_t, GrTextureFilterMode_t, GrTextureFilterMode_t))sym3("grTexFilterMode");
    auto grTexClampMode_ =
        (void (*)(GrChipID_t, GrTextureClampMode_t, GrTextureClampMode_t))sym3("grTexClampMode");
    auto grTexMipMapMode_ = (void (*)(GrChipID_t, GrMipMapMode_t, FxBool))sym3("grTexMipMapMode");
    auto grTexCombine_ =
        (void (*)(GrChipID_t, GrCombineFunction_t, GrCombineFactor_t, GrCombineFunction_t,
                  GrCombineFactor_t, FxBool, FxBool))sym3("grTexCombine");
    if (!grGet_ || !grGetString_ || !grGlideInit_ || !grBufferClear_ || !grBufferSwap_
        || !grColorCombine_ || !grVertexLayout_ || !grReset_ || !grDrawVertexArray_
        || !grDrawVertexArrayContiguous_ || !grCoordinateSpace_ || !grViewport_
        || !grGlideGetState_ || !grGlideSetState_ || !grTexMinAddress_
        || !grTexTextureMemRequired_ || !grTexCalcMemRequired_ || !grTexDownloadMipMap_
        || !grTexSource_ || !grTexFilterMode_ || !grTexClampMode_ || !grTexMipMapMode_
        || !grTexCombine_) {
        return 1;
    }

    /* 2. grGet before grGlideInit: hw/3dfx lets it through, games ask */
    v[0] = 0;
    check_value("boards", grGet_(GR_NUM_BOARDS, 4, v) == 4 && v[0] == 1,
                "grGet(GR_NUM_BOARDS) before grGlideInit = %d", v[0]);
    grGlideInit_();
    {
        const char *ver = grGetString_(GR_VERSION);
        const char *hw = grGetString_(GR_HARDWARE);
        v[0] = v[1] = 0;
        grGet_(GR_NUM_TMU, 4, v);
        grGet_(GR_GLIDE_STATE_SIZE, 4, v + 1);
        check_value("strings", ver && ver[0] == '3' && hw && strlen(hw) < 16
                    && grGetString_(GR_EXTENSION) && v[0] == 1 && v[1] <= 312,
                    "version \"%s\" hardware \"%s\", %d TMU, state %d bytes",
                    ver ? ver : "(null)", hw ? hw : "(null)", v[0], v[1]);
    }

    /* 3. the window, through the dispatcher's table: grSstWinOpen is
     *    wrap3x_grSstWinOpen now, and it answers with a context */
    disp.FEnum = FEnum_grSstWinOpen;
    disp.arg = arg;
    arg[1] = GR_RESOLUTION_640x480;
    arg[2] = GR_REFRESH_60Hz;
    arg[3] = GR_COLORFORMAT_ABGR;
    arg[4] = GR_ORIGIN_UPPER_LEFT;
    arg[5] = 2;
    arg[6] = 1;
    init_window(GR_RESOLUTION_640x480, "Glide3x", &disp);
    stat = stat_window(GR_RESOLUTION_640x480, &disp);
    printf("stat_window -> %d, GrContext %p\n", stat, (void *)disp.GrContext);
    if (stat != 0 || !disp.GrContext) {
        printf("grSstWinOpen refused\n");
        return 1;
    }
    grColorCombine_(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                    GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    /* 4. a game's own vertex: x, y, 1/w, packed ARGB — sixteen bytes that
     *    are nothing like a GrVertex. The upper-left half, green on red. */
    {
        struct { float x, y, q; uint32_t argb; } t[3] = {
            { 0.f, 0.f, 1.f, 0xff00ff00u },
            { 639.f, 0.f, 1.f, 0xff00ff00u },
            { 0.f, 479.f, 1.f, 0xff00ff00u },
        };
        void *ptrs[3] = { &t[0], &t[1], &t[2] };
        grReset_(GR_VERTEX_PARAMETER);
        grVertexLayout_(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_Q, 8, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_PARGB, 12, GR_PARAM_ENABLE);
        grBufferClear_(0x0000ff, 0, 0);
        grDrawVertexArray_(GR_TRIANGLES, 3, ptrs);
        grBufferSwap_(0);
        const probe p[] = {
            { 10, 10, 0x00ff00, "in triangle" },
            { 630, 470, 0xff0000, "outside" },
            { 80, 420, 0xff0000, "below hypotenuse" },
        };
        check_frame("layout", p, 3);
    }

    /* 5. float colours, contiguous: a fan on the left half, and on the right
     *    a strip whose lower half only exists if GR_TRIANGLE_STRIP_CONTINUE
     *    carried the last two vertices over from the call before */
    {
        struct { float x, y, r, g, b; } fanv[4] = {
            { 0.f, 0.f, 0.f, 255.f, 0.f }, { 320.f, 0.f, 0.f, 255.f, 0.f },
            { 320.f, 480.f, 0.f, 255.f, 0.f }, { 0.f, 480.f, 0.f, 255.f, 0.f },
        };
        struct { float x, y, r, g, b; } top[4] = {
            { 320.f, 0.f, 255.f, 255.f, 255.f }, { 640.f, 0.f, 255.f, 255.f, 255.f },
            { 320.f, 240.f, 255.f, 255.f, 255.f }, { 640.f, 240.f, 255.f, 255.f, 255.f },
        };
        struct { float x, y, r, g, b; } bottom[2] = {
            { 320.f, 480.f, 255.f, 255.f, 255.f }, { 640.f, 480.f, 255.f, 255.f, 255.f },
        };
        grReset_(GR_VERTEX_PARAMETER);
        grVertexLayout_(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_RGB, 8, GR_PARAM_ENABLE);
        grBufferClear_(0xff0000, 0, 0);   /* blue */
        grDrawVertexArrayContiguous_(GR_TRIANGLE_FAN, 4, fanv, sizeof(fanv[0]));
        grDrawVertexArrayContiguous_(GR_TRIANGLE_STRIP, 4, top, sizeof(top[0]));
        grDrawVertexArrayContiguous_(GR_TRIANGLE_STRIP_CONTINUE, 2, bottom, sizeof(bottom[0]));
        grBufferSwap_(0);
        const probe p[] = {
            { 160, 240, 0x00ff00, "fan" },
            { 480, 120, 0xffffff, "strip" },
            { 600, 450, 0xffffff, "continued" },
        };
        check_frame("fan/strip", p, 3);
    }

    /* 6. clip coordinates: x, y, w and colours in [0,1], through the
     *    viewport — w = 2 everywhere, so a missing divide draws off-screen */
    {
        struct { float x, y, w, r, g, b; } t[3] = {
            { 2.f, -2.f, 2.f, 0.f, 0.f, 1.f },
            { 2.f, 2.f, 2.f, 0.f, 0.f, 1.f },
            { -2.f, 2.f, 2.f, 0.f, 0.f, 1.f },
        };
        grReset_(GR_VERTEX_PARAMETER);
        grVertexLayout_(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_W, 8, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_RGB, 12, GR_PARAM_ENABLE);
        grCoordinateSpace_(GR_CLIP_COORDS);
        grBufferClear_(0x0000ff, 0, 0);
        grDrawVertexArrayContiguous_(GR_TRIANGLES, 3, t, sizeof(t[0]));
        grBufferSwap_(0);
        grCoordinateSpace_(GR_WINDOW_COORDS);
        const probe p[] = {
            { 630, 470, 0x0000ff, "lower right" },
            { 320, 400, 0x0000ff, "inside" },
            { 10, 10, 0xff0000, "upper left" },
            { 320, 100, 0xff0000, "outside" },
        };
        check_frame("clip", p, 4);
    }

    /* 7. a 32x16 texture: GR_LOD_LOG2_32 and GR_ASPECT_LOG2_2x1 are 5 and 1,
     *    which untranslated are Glide 2's 8x8-texel GR_LOD_8 and 4x1 — the
     *    memory size gives it away before a pixel is drawn, the quadrants
     *    after. */
    {
        static uint16_t texels[16][32];
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 32; x++) {
                texels[y][x] = y < 8 ? (x < 16 ? 0x001f : 0xffe0)    /* blue, yellow */
                                     : (x < 16 ? 0x07e0 : 0xf81f);   /* green, magenta */
            }
        }
        GrTexInfo info;
        info.smallLod = GR_LOD_LOG2_32;
        info.largeLod = GR_LOD_LOG2_32;
        info.aspectRatio = GR_ASPECT_LOG2_2x1;
        info.format = GR_TEXFMT_RGB_565;
        info.data = texels;
        FxU32 need = grTexTextureMemRequired_(GR_MIPMAPLEVELMASK_BOTH, &info);
        FxU32 calc = grTexCalcMemRequired_(GR_LOD_LOG2_32, GR_LOD_LOG2_32,
                                           GR_ASPECT_LOG2_2x1, GR_TEXFMT_RGB_565);
        check_value("texmem", need == 1024 && calc == 1024,
                    "a 32x16 565 texture needs %u / %u bytes (want 1024)", need, calc);

        FxU32 start = grTexMinAddress_(GR_TMU0);
        grTexDownloadMipMap_(GR_TMU0, start, GR_MIPMAPLEVELMASK_BOTH, &info);
        grTexSource_(GR_TMU0, start, GR_MIPMAPLEVELMASK_BOTH, &info);
        grTexFilterMode_(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
        grTexClampMode_(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
        grTexMipMapMode_(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);
        grTexCombine_(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                      GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
        grColorCombine_(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                        GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        /* window coordinates: s/w and t/w in texels of a 256-wide map, so a
         * 2:1 texture spans s 0..256 and t 0..128 */
        struct { float x, y, q, s, t; } q[4] = {
            { 0.f, 0.f, 1.f, 0.f, 0.f }, { 640.f, 0.f, 1.f, 256.f, 0.f },
            { 640.f, 480.f, 1.f, 256.f, 128.f }, { 0.f, 480.f, 1.f, 0.f, 128.f },
        };
        grReset_(GR_VERTEX_PARAMETER);
        grVertexLayout_(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_Q, 8, GR_PARAM_ENABLE);
        grVertexLayout_(GR_PARAM_ST0, 12, GR_PARAM_ENABLE);
        grBufferClear_(0, 0, 0);
        grDrawVertexArrayContiguous_(GR_TRIANGLE_FAN, 4, q, sizeof(q[0]));
        grBufferSwap_(0);
        const probe p[] = {
            { 160, 120, 0x0000ff, "top left" },
            { 480, 120, 0xffff00, "top right" },
            { 160, 360, 0x00ff00, "bottom left" },
            { 480, 360, 0xff00ff, "bottom right" },
        };
        check_frame("texture", p, 4);
        grColorCombine_(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                        GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    }

    /* 8. the Glide 3 half of the state comes back with the Glide 2 half */
    {
        static uint8_t saved[312];
        grViewport_(10, 20, 300, 200);
        grGlideGetState_(saved);
        grViewport_(0, 0, 640, 480);
        grGlideSetState_(saved);
        memset(v, 0, sizeof(v));
        grGet_(GR_VIEWPORT, 16, v);
        check_value("state", v[0] == 10 && v[1] == 20 && v[2] == 300 && v[3] == 200,
                    "viewport after grGlideSetState %d,%d %dx%d (want 10,20 300x200)",
                    v[0], v[1], v[2], v[3]);
        grViewport_(0, 0, 640, 480);
    }

    if (getenv("GLIDE_TEST_BMP") && last_frame) {
        write_bmp(getenv("GLIDE_TEST_BMP"), last_frame, fw, fh);
    }

    /* 9. close the way processFRet does for a Glide 3 guest */
    disp.FEnum = FEnum_grSstWinClose3x;
    fini_window(&disp);
    fini_glide2x();

    printf("glide3: %d cases, %d failed; actives %d frames %d -> %s\n", g3_cases,
           g3_failed, actives, frames, (g3_failed == 0 && actives == 2) ? "OK" : "FAIL");
    fflush(stdout);
    return (g3_failed == 0 && actives == 2) ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* `glide-host-test [3] [bios dir]` */
    bool glide3 = argc > 1 && !strcmp(argv[1], "3");
    const char *bios = argc > 1 + glide3 ? argv[1 + glide3] : "qemu/pc-bios";
    const char *lib = getenv("QEMU_GLIDE_LIB");
    char *qargv[] = {
        "qemu-system-i386", "-machine", "pc", "-m", "32", "-net", "none",
        "-L", (char *)bios, "-nodefaults", "-vga", "std",
    };
    qemu_embed_display_cb cb = {
        .on_3d_active = on_3d_active,
        .on_3d_frame = on_3d_frame,
        /* readback only: the check is the pixels, not the transport, and
         * tools/embed-3d-test already guards the dma-buf ring */
    };
    window_cb disp = { };
    uint32_t arg[16] = { 0 };
    int ok = 1, stat;

    /* Unset is the interesting case: hw/3dfx's own candidate list has to
     * find the build tree by itself. We still need the path to reach the
     * wrapper's gr* directly below, and dlopen of an already-loaded library
     * returns the same handle, so use the same relative name it does. */
    if (!lib || !*lib) {
        lib = "build/glide/libglide2x.so";
        printf("QEMU_GLIDE_LIB unset: expecting hw/3dfx to find %s\n", lib);
    }
    if (qemu_embed_api_version() != QEMU_EMBED_API_VERSION) {
        printf("API version mismatch\n");
        return 1;
    }
    if (!qemu_embed_new(sizeof(qargv) / sizeof(qargv[0]), qargv, &cb, NULL)) {
        printf("qemu_embed_new failed\n");
        return 1;
    }
    /* BQL is held by this thread after init, as on a vCPU thread */

    /* 1. the dispatcher loads the wrapper and gives it our context */
    if (glide_host_ops() == NULL) {
        printf("the embed provider offers no host ops (no EGL?)\n");
        return 1;
    }
    if (glide3) {
        int rc = run_glide3(lib);
        /* one VM per process; cleanup is partial — exit without it */
        _exit(rc);
    }
    if (init_glide2x("glide2x.dll") != 0) {
        printf("init_glide2x failed: %s not loadable\n", lib);
        return 1;
    }
    printf("wrapper loaded: %s\n", lib);
    wrapper = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
    if (!wrapper) {
        printf("the wrapper is loaded but not by this name?\n");
        return 1;
    }

    auto grGlideInit_ = (void (*)(void))sym("grGlideInit");
    auto grSstSelect_ = (void (*)(int))sym("grSstSelect");
    auto grBufferClear_ =
        (void (*)(GrColor_t, GrAlpha_t, FxU32))sym("grBufferClear");
    auto grBufferSwap_ = (void (*)(int))sym("grBufferSwap");
    auto grDrawTriangle_ =
        (void (*)(const void *, const void *, const void *))sym("grDrawTriangle");
    auto grColorCombine_ =
        (void (*)(GrCombineFunction_t, GrCombineFactor_t, GrCombineLocal_t,
                  GrCombineOther_t, FxBool))sym("grColorCombine");
    if (!grGlideInit_ || !grSstSelect_ || !grBufferClear_ || !grBufferSwap_
        || !grDrawTriangle_ || !grColorCombine_) {
        return 1;
    }
    grGlideInit_();
    grSstSelect_(0);

    /* 2. glidewnd.c's own handshake: what glidept_mm.c does for a guest's
     *    grSstWinOpen, then the guest's poll of MMIO 0xFB8. */
    disp.FEnum = FEnum_grSstWinOpen;
    disp.arg = arg;
    arg[1] = GR_RESOLUTION_640x480;
    arg[2] = GR_REFRESH_60Hz;
    arg[3] = GR_COLORFORMAT_ABGR;
    arg[4] = GR_ORIGIN_UPPER_LEFT;
    arg[5] = 2;   /* double buffered */
    arg[6] = 1;   /* one aux buffer */
    init_window(GR_RESOLUTION_640x480, "Glide2x", &disp);
    stat = stat_window(GR_RESOLUTION_640x480, &disp);
    printf("stat_window -> %d (0 = the window is up), GrContext %p\n",
           stat, (void *)disp.GrContext);
    if (stat != 0 || !disp.GrContext) {
        printf("grSstWinOpen refused\n");
        return 1;
    }
    ok = ok && actives == 1;

    /* 3. draw through Glide and swap */
    grColorCombine_(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                    GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    /* 0x0000ff in ABGR is red; the clear colour is the frame's background */
    grBufferClear_(0x0000ff, 0, 0);
    /*
     * A green triangle over the *upper-left half* of the 640x480 Glide
     * window. Glide was opened with GR_ORIGIN_UPPER_LEFT, so this is the
     * orientation check as much as the drawing one: get the flip wrong
     * anywhere between OpenGLide, the FBO and the readback and the two
     * corners below simply swap. A centred shape would pass either way.
     */
    {
        GrVertex a = { }, b = { }, c = { };
        a.x = 0.f;   a.y = 0.f;
        b.x = 639.f; b.y = 0.f;
        c.x = 0.f;   c.y = 479.f;
        a.r = b.r = c.r = 0.f;
        a.g = b.g = c.g = 255.f;
        a.b = b.b = c.b = 0.f;
        a.a = b.a = c.a = 255.f;
        a.oow = b.oow = c.oow = 1.f;
        grDrawTriangle_(&a, &b, &c);
    }
    grBufferSwap_(0);
    printf("frame %d: %dx%d top-left %08x bottom-right %08x centre %08x\n",
           frames, fw, fh, px_tl, px_br, px_c);
    if (getenv("GLIDE_TEST_BMP") && last_frame) {
        write_bmp(getenv("GLIDE_TEST_BMP"), last_frame, fw, fh);
    }
    ok = ok && frames == 1 && fw == 640 && fh == 480
            && (px_tl & 0xffffff) == 0x00ff00     /* the triangle, top-left */
            && (px_br & 0xffffff) == 0xff0000;    /* the clear, bottom-right */

    /* a second swap with nothing drawn: the clear colour everywhere */
    grBufferClear_(0x0000ff, 0, 0);
    grBufferSwap_(0);
    ok = ok && frames == 2 && (px_tl & 0xffffff) == 0xff0000
            && (px_c & 0xffffff) == 0xff0000;

    /* 3b. an LFB write lock held across the swap. Carmageddon's 3dfx build
     *     locks the back buffer once and treats it as its frame buffer for
     *     the whole front end; upstream OpenGLide drew a write buffer only
     *     on the unlock, which never comes, and every frame was black
     *     (patches/openglide/05-lfb-locked-swap, 2026-09-10). Fill the
     *     locked buffer with 565 blue, swap without unlocking: blue. */
    auto grLfbLock_ = (FxBool (*)(GrLock_t, GrBuffer_t, GrLfbWriteMode_t,
                                  GrOriginLocation_t, FxBool, GrLfbInfo_t *))sym("grLfbLock");
    auto grLfbUnlock_ = (FxBool (*)(GrLock_t, GrBuffer_t))sym("grLfbUnlock");
    if (!grLfbLock_ || !grLfbUnlock_) {
        return 1;
    }
    {
        GrLfbInfo_t info = { };
        FxBool got;
        info.size = sizeof(info);
        got = grLfbLock_(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565,
                         GR_ORIGIN_UPPER_LEFT, FXFALSE, &info);
        printf("grLfbLock(write, back) -> %d ptr %p stride %u\n", got,
               info.lfbPtr, (unsigned)info.strideInBytes);
        ok = ok && got && info.lfbPtr && info.strideInBytes >= 1280;
        if (got && info.lfbPtr) {
            uint16_t *p = (uint16_t *)info.lfbPtr;
            for (int y = 0; y < 480; y++) {
                for (int x = 0; x < 640; x++) {
                    p[y * (info.strideInBytes / 2) + x] = 0x001f;   /* 565 blue */
                }
            }
        }
        grBufferSwap_(0);   /* still locked */
        printf("frame %d (LFB write lock held): centre %08x top-left %08x\n",
               frames, px_c, px_tl);
        ok = ok && frames == 3 && (px_c & 0xffffff) == 0x0000f8
                && (px_tl & 0xffffff) == 0x0000f8;
        grLfbUnlock_(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
    }

    /* 4. close, exactly as processFRet does for grSstWinClose */
    disp.FEnum = FEnum_grSstWinClose;
    fini_window(&disp);
    fini_glide2x();
    ok = ok && actives == 2;

    printf("actives %d frames %d -> %s\n", actives, frames, ok ? "OK" : "FAIL");
    fflush(stdout);
    /* one VM per process; cleanup is partial — exit without it */
    _exit(ok ? 0 : 1);
}

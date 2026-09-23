/*
 * glide2sdk.h: the part of the Glide 2.4 ABI that DITHTEST.EXE needs (the
 * types, the numeric values and the fifteen entry points it calls),
 * written from the public Glide 2.4 programming guide and reference. It
 * is neither 3dfx's header (its legend forbids copying it) nor OpenGLide's,
 * which left with the Glide pass-through (ADR-020). The layouts are the
 * Voodoo 2 SDK's with two TMUs, which is what 3dfx's own glide2x.dll on
 * the emulated card (doc 21) expects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLIDE2SDK_H
#define GLIDE2SDK_H

#include <stdint.h>

#define FX_CALL __stdcall

typedef uint8_t   FxU8;
typedef uint16_t  FxU16;
typedef uint32_t  FxU32;
typedef int32_t   FxI32;
typedef int       FxBool;
typedef uintptr_t FxU;

#define FXTRUE  1
#define FXFALSE 0

#define GLIDE_NUM_TMU 2
#define MAX_NUM_SST   4

typedef FxU32 GrColor_t;
typedef FxU8  GrAlpha_t;
typedef FxI32 GrCombineFunction_t;
typedef FxI32 GrCombineFactor_t;
typedef FxI32 GrCombineLocal_t;
typedef FxI32 GrCombineOther_t;
typedef FxI32 GrAlphaBlendFnc_t;
typedef FxI32 GrBuffer_t;
typedef FxI32 GrColorFormat_t;
typedef FxU32 GrLock_t;
typedef FxI32 GrLfbWriteMode_t;
typedef FxI32 GrOriginLocation_t;
typedef FxI32 GrScreenResolution_t;
typedef FxI32 GrScreenRefresh_t;
typedef int   GrSstType;

#define GR_COMBINE_FUNCTION_LOCAL     0x1
#define GR_COMBINE_FACTOR_NONE        0x0
#define GR_COMBINE_LOCAL_ITERATED     0x0
#define GR_COMBINE_OTHER_NONE         0x2

#define GR_BLEND_ZERO                 0x0
#define GR_BLEND_SRC_ALPHA            0x1
#define GR_BLEND_ONE                  0x4
#define GR_BLEND_ONE_MINUS_SRC_ALPHA  0x5

#define GR_BUFFER_FRONTBUFFER         0x0
#define GR_BUFFER_BACKBUFFER          0x1

#define GR_COLORFORMAT_ABGR           0x1

#define GR_LFB_READ_ONLY              0x00
#define GR_LFBWRITEMODE_ANY           0xFF
#define GR_ORIGIN_UPPER_LEFT          0x0

#define GR_REFRESH_60Hz               0x0
#define GR_RESOLUTION_640x480         0x7
#define GR_RESOLUTION_800x600         0x8

typedef struct {
    float sow, tow, oow;
} GrTmuVertex;

typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

typedef struct {
    int                size;
    void              *lfbPtr;
    FxU32              strideInBytes;
    GrLfbWriteMode_t   writeMode;
    GrOriginLocation_t origin;
} GrLfbInfo_t;

typedef struct {
    int tmuRev;
    int tmuRam;
} GrTMUConfig_t;

typedef struct {
    int           fbRam;
    int           fbiRev;
    int           nTexelfx;
    FxBool        sliDetect;
    GrTMUConfig_t tmuConfig[GLIDE_NUM_TMU];
} GrVoodooConfig_t;

typedef struct {
    int           fbRam;
    int           nTexelfx;
    GrTMUConfig_t tmuConfig;
} GrSst96Config_t;

typedef struct {
    int rev;
} GrAT3DConfig_t;

typedef struct {
    int num_sst;
    struct {
        GrSstType type;
        union {
            GrVoodooConfig_t VoodooConfig;
            GrSst96Config_t  SST96Config;
            GrAT3DConfig_t   AT3DConfig;
            GrVoodooConfig_t Voodoo2Config;
        } sstBoard;
    } SSTs[MAX_NUM_SST];
} GrHwConfiguration;

void   FX_CALL grGlideInit(void);
void   FX_CALL grGlideShutdown(void);
void   FX_CALL grGlideGetVersion(char version[80]);
FxBool FX_CALL grSstQueryHardware(GrHwConfiguration *hwconfig);
FxBool FX_CALL grSstSelect(int which_sst);
FxBool FX_CALL grSstWinOpen(FxU hWnd, GrScreenResolution_t screen_resolution,
                            GrScreenRefresh_t refresh_rate, GrColorFormat_t color_format,
                            GrOriginLocation_t origin_location, int nColBuffers,
                            int nAuxBuffers);
void   FX_CALL grSstWinClose(void);
void   FX_CALL grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU16 depth);
void   FX_CALL grBufferSwap(int swap_interval);
void   FX_CALL grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c);
void   FX_CALL grColorCombine(GrCombineFunction_t function, GrCombineFactor_t factor,
                              GrCombineLocal_t local, GrCombineOther_t other, FxBool invert);
void   FX_CALL grAlphaCombine(GrCombineFunction_t function, GrCombineFactor_t factor,
                              GrCombineLocal_t local, GrCombineOther_t other, FxBool invert);
void   FX_CALL grAlphaBlendFunction(GrAlphaBlendFnc_t rgb_sf, GrAlphaBlendFnc_t rgb_df,
                                    GrAlphaBlendFnc_t alpha_sf, GrAlphaBlendFnc_t alpha_df);
FxBool FX_CALL grLfbLock(GrLock_t type, GrBuffer_t buffer, GrLfbWriteMode_t writeMode,
                         GrOriginLocation_t origin, FxBool pixelPipeline, GrLfbInfo_t *info);
FxBool FX_CALL grLfbUnlock(GrLock_t type, GrBuffer_t buffer);

#endif /* GLIDE2SDK_H */

/*
 * Glide 3 on top of Glide 2: the part of the Glide 3.x ABI that
 * sdk2_glide.h (OpenGLide's copy of 3dfx's 2.x header) does not carry.
 *
 * Two readers, one file, so they cannot drift apart:
 *
 *   - glidept/host/glide3.cpp, the Glide 3 layer of our OpenGLide build
 *     (the host half of qemu-3dfx's glide3x.dll pass-through), which needs
 *     the constants;
 *   - guest-tools/src/glide3test.c, the in-guest test, which also needs
 *     the prototypes (define GLIDE3_GUEST_PROTOTYPES before including).
 *
 * Include sdk2_glide.h first: everything Glide 2 and Glide 3 share (the
 * combine units, buffers, LFB, texture formats, GrLfbInfo_t, GrTexInfo)
 * comes from there, and only what Glide 3 added or re-encoded is here.
 * sdk2_glide.h keeps its own Glide 3 *alpha* spellings behind GLIDE3, with
 * some values that the release changed; never define GLIDE3 next to this.
 *
 * The values are those of 3dfx's released Glide 3 source (the Voodoo 2
 * "cvg" tree of glide.sourceforge.net), which is this file's specification.
 * Its licence is not GPL-compatible, so it is read, never copied: nothing
 * below is its text, only the numbers an ABI consists of.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
 */
#ifndef GLIDEPT_GLIDE3_H
#define GLIDEPT_GLIDE3_H

/* grSstWinOpen's return. 32 bits in the guest; the host dispatcher keeps
 * it in a uintptr_t, and any non-zero value is a context. */
typedef unsigned long GrContext_t;

/*
 * Glide 3 re-encoded the level of detail and the aspect ratio as log2
 * values, running the other way from Glide 2's: GR_LOD_256 was 0 and is
 * now 8, GR_ASPECT_8x1 was 0 and is now 3. Same GrTexInfo layout, same
 * names of the functions that take them — which is why a Glide 3 texture
 * handed to a Glide 2 implementation untranslated is the wrong size rather
 * than an error.
 */
#define GR_LOD_LOG2_256         8
#define GR_LOD_LOG2_128         7
#define GR_LOD_LOG2_64          6
#define GR_LOD_LOG2_32          5
#define GR_LOD_LOG2_16          4
#define GR_LOD_LOG2_8           3
#define GR_LOD_LOG2_4           2
#define GR_LOD_LOG2_2           1
#define GR_LOD_LOG2_1           0

#define GR_ASPECT_LOG2_8x1      3
#define GR_ASPECT_LOG2_4x1      2
#define GR_ASPECT_LOG2_2x1      1
#define GR_ASPECT_LOG2_1x1      0
#define GR_ASPECT_LOG2_1x2      (-1)
#define GR_ASPECT_LOG2_1x4      (-2)
#define GR_ASPECT_LOG2_1x8      (-3)

/* the one Glide 2 value of each, for the translation */
#define G3_LOD_TO_G2(lod)       (GR_LOD_LOG2_256 - (lod))
#define G3_ASPECT_TO_G2(asp)    (GR_ASPECT_LOG2_8x1 - (asp))
#define G2_LOD_TO_G3(lod)       (GR_LOD_LOG2_256 - (lod))
#define G2_ASPECT_TO_G3(asp)    (GR_ASPECT_LOG2_8x1 - (asp))

/* grVertexLayout: the game describes its own vertex, by byte offset */
#define GR_PARAM_XY             0x01
#define GR_PARAM_Z              0x02
#define GR_PARAM_W              0x03
#define GR_PARAM_Q              0x04
#define GR_PARAM_FOG_EXT        0x05
#define GR_PARAM_A              0x10
#define GR_PARAM_RGB            0x20
#define GR_PARAM_PARGB          0x30
#define GR_PARAM_ST0            0x40
#define GR_PARAM_ST1            0x41
#define GR_PARAM_ST2            0x42
#define GR_PARAM_Q0             0x50
#define GR_PARAM_Q1             0x51
#define GR_PARAM_Q2             0x52
#define GR_PARAM_DISABLE        0x00
#define GR_PARAM_ENABLE         0x01

/* grDrawVertexArray / grDrawVertexArrayContiguous */
#define GR_POINTS                   0
#define GR_LINE_STRIP               1
#define GR_LINES                    2
#define GR_POLYGON                  3
#define GR_TRIANGLE_STRIP           4
#define GR_TRIANGLE_FAN             5
#define GR_TRIANGLES                6
#define GR_TRIANGLE_STRIP_CONTINUE  7
#define GR_TRIANGLE_FAN_CONTINUE    8

/* grCoordinateSpace */
#define GR_WINDOW_COORDS        0x00
#define GR_CLIP_COORDS          0x01

/* grEnable / grDisable */
#define GR_AA_ORDERED           0x01
#define GR_ALLOW_MIPMAP_DITHER  0x02
#define GR_PASSTHRU             0x03
#define GR_SHAMELESS_PLUG       0x04
#define GR_VIDEO_SMOOTHING      0x05

/* grGet */
#define GR_BITS_DEPTH                   0x01
#define GR_BITS_RGBA                    0x02
#define GR_FIFO_FULLNESS                0x03
#define GR_FOG_TABLE_ENTRIES            0x04
#define GR_GAMMA_TABLE_ENTRIES          0x05
#define GR_GLIDE_STATE_SIZE             0x06
#define GR_GLIDE_VERTEXLAYOUT_SIZE      0x07
#define GR_IS_BUSY                      0x08
#define GR_LFB_PIXEL_PIPE               0x09
#define GR_MAX_TEXTURE_SIZE             0x0a
#define GR_MAX_TEXTURE_ASPECT_RATIO     0x0b
#define GR_MEMORY_FB                    0x0c
#define GR_MEMORY_TMU                   0x0d
#define GR_MEMORY_UMA                   0x0e
#define GR_NUM_BOARDS                   0x0f
#define GR_NON_POWER_OF_TWO_TEXTURES    0x10
#define GR_NUM_FB                       0x11
#define GR_NUM_SWAP_HISTORY_BUFFER      0x12
#define GR_NUM_TMU                      0x13
#define GR_PENDING_BUFFERSWAPS          0x14
#define GR_REVISION_FB                  0x15
#define GR_REVISION_TMU                 0x16
#define GR_STATS_LINES                  0x17
#define GR_STATS_PIXELS_AFUNC_FAIL      0x18
#define GR_STATS_PIXELS_CHROMA_FAIL     0x19
#define GR_STATS_PIXELS_DEPTHFUNC_FAIL  0x1a
#define GR_STATS_PIXELS_IN              0x1b
#define GR_STATS_PIXELS_OUT             0x1c
#define GR_STATS_PIXELS                 0x1d
#define GR_STATS_POINTS                 0x1e
#define GR_STATS_TRIANGLES_IN           0x1f
#define GR_STATS_TRIANGLES_OUT          0x20
#define GR_STATS_TRIANGLES              0x21
#define GR_SWAP_HISTORY                 0x22
#define GR_SUPPORTS_PASSTHRU            0x23
#define GR_TEXTURE_ALIGN                0x24
#define GR_VIDEO_POSITION               0x25
#define GR_VIEWPORT                     0x26
#define GR_WDEPTH_MIN_MAX               0x27
#define GR_ZDEPTH_MIN_MAX               0x28
#define GR_VERTEX_PARAMETER             0x29
#define GR_BITS_GAMMA                   0x2a

/* grGetString */
#define GR_EXTENSION            0xa0
#define GR_HARDWARE             0xa1
#define GR_RENDERER             0xa2
#define GR_VENDOR               0xa3
#define GR_VERSION              0xa4

/* grQueryResolutions */
#define GR_QUERY_ANY            ((FxU32)(~0))
typedef struct {
    GrScreenResolution_t resolution;
    GrScreenRefresh_t    refresh;
    int                  numColorBuffers;
    int                  numAuxBuffers;
} GrResolution;

#ifdef GLIDE3_GUEST_PROTOTYPES
/*
 * A guest program linked against glide3x.dll's import library. The calls
 * Glide 3 added have names Glide 2 never used and are declared plainly.
 * The ones whose Glide 3 signature differs from sdk2_glide.h's Glide 2
 * declaration of the same name get a g3 prefix here, bound to the real
 * export by an asm label (the i386 stdcall name, argument bytes included).
 * The rest — grBufferClear, grTexSource, grLfbLock and the like — have the
 * same signature in both and are called through sdk2_glide.h, with Glide 3
 * values where the encoding changed (the LODs and aspect ratios above).
 */
FX_ENTRY GrContext_t FX_CALL
g3SstWinOpen(FxU32 hwnd, GrScreenResolution_t res, GrScreenRefresh_t ref,
             GrColorFormat_t cformat, GrOriginLocation_t origin,
             int nColBuffers, int nAuxBuffers) __asm__("_grSstWinOpen@28");
FX_ENTRY FxBool FX_CALL
g3SstWinClose(GrContext_t context) __asm__("_grSstWinClose@4");
FX_ENTRY void FX_CALL
g3DrawPoint(const void *pt) __asm__("_grDrawPoint@4");
FX_ENTRY void FX_CALL
g3DrawLine(const void *v1, const void *v2) __asm__("_grDrawLine@8");
FX_ENTRY void FX_CALL
g3DrawTriangle(const void *a, const void *b, const void *c)
    __asm__("_grDrawTriangle@12");
FX_ENTRY void FX_CALL
g3TexDownloadTable(GrTexTable_t type, void *data)
    __asm__("_grTexDownloadTable@8");
FX_ENTRY void FX_CALL
g3TexNCCTable(GrNCCTable_t table) __asm__("_grTexNCCTable@4");
FX_ENTRY FxBool FX_CALL
g3LfbWriteRegion(GrBuffer_t dst_buffer, FxU32 dst_x, FxU32 dst_y,
                 GrLfbSrcFmt_t src_format, FxU32 src_width, FxU32 src_height,
                 FxBool pixelPipeline, FxI32 src_stride, void *src_data)
    __asm__("_grLfbWriteRegion@36");

FX_ENTRY void FX_CALL grVertexLayout(FxU32 param, FxI32 offset, FxU32 mode);
FX_ENTRY void FX_CALL grDrawVertexArray(FxU32 mode, FxU32 count, void *pointers);
FX_ENTRY void FX_CALL grDrawVertexArrayContiguous(FxU32 mode, FxU32 count,
                                                  void *vertices, FxU32 stride);
FX_ENTRY FxU32 FX_CALL grGet(FxU32 pname, FxU32 plength, FxI32 *params);
FX_ENTRY const char * FX_CALL grGetString(FxU32 pname);
FX_ENTRY FxBool FX_CALL grReset(FxU32 what);
FX_ENTRY void FX_CALL grEnable(FxU32 mode);
FX_ENTRY void FX_CALL grDisable(FxU32 mode);
FX_ENTRY void FX_CALL grCoordinateSpace(FxU32 mode);
FX_ENTRY void FX_CALL grDepthRange(float n, float f);
FX_ENTRY void FX_CALL grViewport(FxI32 x, FxI32 y, FxI32 width, FxI32 height);
FX_ENTRY FxI32 FX_CALL grQueryResolutions(const GrResolution *resTemplate,
                                          GrResolution *output);
FX_ENTRY FxBool FX_CALL grSelectContext(GrContext_t context);
FX_ENTRY void FX_CALL grGlideGetVertexLayout(void *layout);
FX_ENTRY void FX_CALL grGlideSetVertexLayout(const void *layout);
FX_ENTRY void FX_CALL grFinish(void);
FX_ENTRY void FX_CALL grFlush(void);
#endif /* GLIDE3_GUEST_PROTOTYPES */

#endif /* GLIDEPT_GLIDE3_H */

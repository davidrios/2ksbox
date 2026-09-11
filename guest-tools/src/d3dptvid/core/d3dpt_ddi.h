/*
 * d3dpt_ddi.h — the Direct3D DDI types that are the *same* on every
 * Windows the driver runs on: the DirectX caps structures, the
 * DrawPrimitives2 command header and its token numbers, the DirectX 8
 * GetDriverInfo2 shapes and D3DCAPS8.
 *
 * They live here rather than in a per-OS DDK header because the core
 * (core/ sources) fills them and the core must include no DDK header of
 * either family (doc 19, "The split"). NT's d3dnthal.h and the 9x
 * d3dhal.h keep only what genuinely differs — the callback data
 * structures and the surface objects — and include this for the rest.
 * The layouts are the DDK's; the trailing underscore says the name is
 * ours, spelled out so that neither d3dtypes.h / d3dcaps.h nor the
 * user-mode windows.h has to be reachable from a kernel build.
 *
 * Needs <windef.h> and <ddraw.h> (for DDSURFACEDESC / DDPIXELFORMAT)
 * included first; the includer's DDK header does that.
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED (the ReactOS base it was split out
 * of); additions SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_DDI_H
#define D3DPT_DDI_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- the Direct3D types the DDI structures use (d3dtypes.h / d3dcaps.h) --- */

typedef float D3DVALUE;
typedef DWORD D3DCOLOR;
typedef struct _D3DRECT_ { LONG x1, y1, x2, y2; } D3DRECT_, *LPD3DRECT_;
typedef enum _D3DCOLORMODEL_ { D3DCOLOR_MONO_ = 1, D3DCOLOR_RGB_ = 2 } D3DCOLORMODEL_;

typedef struct _D3DTRANSFORMCAPS_ { DWORD dwSize; DWORD dwCaps; } D3DTRANSFORMCAPS_;
typedef struct _D3DLIGHTINGCAPS_ { DWORD dwSize; DWORD dwCaps; DWORD dwLightingModel; DWORD dwNumLights; } D3DLIGHTINGCAPS_;
typedef struct _D3DPRIMCAPS_ {
    DWORD dwSize;
    DWORD dwMiscCaps;
    DWORD dwRasterCaps;
    DWORD dwZCmpCaps;
    DWORD dwSrcBlendCaps;
    DWORD dwDestBlendCaps;
    DWORD dwAlphaCmpCaps;
    DWORD dwShadeCaps;
    DWORD dwTextureCaps;
    DWORD dwTextureFilterCaps;
    DWORD dwTextureBlendCaps;
    DWORD dwTextureAddressCaps;
    DWORD dwStippleWidth;
    DWORD dwStippleHeight;
} D3DPRIMCAPS_;

#define D3DDD_COLORMODEL              0x00000001
#define D3DDD_DEVCAPS                 0x00000002
#define D3DDD_TRANSFORMCAPS           0x00000004
#define D3DDD_LIGHTINGCAPS            0x00000008
#define D3DDD_BCLIPPING               0x00000010
#define D3DDD_LINECAPS                0x00000020
#define D3DDD_TRICAPS                 0x00000040
#define D3DDD_DEVICERENDERBITDEPTH    0x00000080
#define D3DDD_DEVICEZBUFFERBITDEPTH   0x00000100
#define D3DDD_MAXBUFFERSIZE           0x00000200
#define D3DDD_MAXVERTEXCOUNT          0x00000400

#define D3DDEVCAPS_FLOATTLVERTEX      0x00000001
#define D3DDEVCAPS_EXECUTESYSTEMMEMORY 0x00000010
#define D3DDEVCAPS_TLVERTEXSYSTEMMEMORY 0x00000040
#define D3DDEVCAPS_TEXTUREVIDEOMEMORY 0x00000200
#define D3DDEVCAPS_DRAWPRIMTLVERTEX   0x00000400
#define D3DDEVCAPS_CANRENDERAFTERFLIP 0x00000800
#define D3DDEVCAPS_DRAWPRIMITIVES2    0x00002000
#define D3DDEVCAPS_DRAWPRIMITIVES2EX  0x00008000
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT 0x00010000
#define D3DDEVCAPS_HWRASTERIZATION    0x00080000

#define D3DPMISCCAPS_MASKZ            0x00000002
#define D3DPMISCCAPS_CULLNONE         0x00000010
#define D3DPMISCCAPS_CULLCW           0x00000020
#define D3DPMISCCAPS_CULLCCW          0x00000040
#define D3DPRASTERCAPS_DITHER         0x00000001
#define D3DPRASTERCAPS_ZTEST          0x00000010
#define D3DPRASTERCAPS_SUBPIXEL       0x00000020
#define D3DPRASTERCAPS_FOGVERTEX      0x00000080
#define D3DPRASTERCAPS_FOGTABLE       0x00000100
#define D3DPRASTERCAPS_ZBIAS          0x00004000
#define D3DPRASTERCAPS_FOGRANGE       0x00010000
#define D3DPRASTERCAPS_ANISOTROPY     0x00020000
#define D3DPRASTERCAPS_WBUFFER        0x00040000
#define D3DPRASTERCAPS_WFOG           0x00100000
#define D3DPRASTERCAPS_ZFOG           0x00200000
#define D3DPCMPCAPS_ALL               0x000000ff
#define D3DPBLENDCAPS_ALL             0x00001fff
#define D3DPSHADECAPS_COLORFLATRGB    0x00000002
#define D3DPSHADECAPS_COLORGOURAUDRGB 0x00000008
#define D3DPSHADECAPS_SPECULARFLATRGB 0x00000080
#define D3DPSHADECAPS_SPECULARGOURAUDRGB 0x00000200
#define D3DPSHADECAPS_ALPHAFLATBLEND  0x00001000
#define D3DPSHADECAPS_ALPHAGOURAUDBLEND 0x00004000
#define D3DPSHADECAPS_FOGFLAT         0x00040000
#define D3DPSHADECAPS_FOGGOURAUD      0x00080000
#define D3DPTEXTURECAPS_PERSPECTIVE   0x00000001
#define D3DPTEXTURECAPS_POW2          0x00000002
#define D3DPTEXTURECAPS_ALPHA         0x00000004
#define D3DPTEXTURECAPS_TRANSPARENCY  0x00000008
#define D3DPTEXTURECAPS_SQUAREONLY    0x00000020
#define D3DPTEXTURECAPS_PROJECTED     0x00000400
#define D3DPTFILTERCAPS_NEAREST       0x00000001
#define D3DPTFILTERCAPS_LINEAR        0x00000002
#define D3DPTFILTERCAPS_MIPNEAREST    0x00000004
#define D3DPTFILTERCAPS_MIPLINEAR     0x00000008
#define D3DPTFILTERCAPS_LINEARMIPNEAREST 0x00000010
#define D3DPTFILTERCAPS_LINEARMIPLINEAR 0x00000020
#define D3DPTFILTERCAPS_MINFPOINT     0x00000100
#define D3DPTFILTERCAPS_MINFLINEAR    0x00000200
#define D3DPTFILTERCAPS_MIPFPOINT     0x00010000
#define D3DPTFILTERCAPS_MIPFLINEAR    0x00020000
#define D3DPTFILTERCAPS_MAGFPOINT     0x01000000
#define D3DPTFILTERCAPS_MAGFLINEAR    0x02000000
#define D3DPTBLENDCAPS_DECAL          0x00000001
#define D3DPTBLENDCAPS_MODULATE       0x00000002
#define D3DPTBLENDCAPS_DECALALPHA     0x00000004
#define D3DPTBLENDCAPS_MODULATEALPHA  0x00000008
#define D3DPTBLENDCAPS_COPY           0x00000040
#define D3DPTBLENDCAPS_ADD            0x00000080
#define D3DPTADDRESSCAPS_WRAP         0x00000001
#define D3DPTADDRESSCAPS_MIRROR       0x00000002
#define D3DPTADDRESSCAPS_CLAMP        0x00000004
#define D3DPTADDRESSCAPS_BORDER       0x00000008
#define D3DPTADDRESSCAPS_INDEPENDENTUV 0x00000010
#define D3DSTENCILCAPS_ALL            0x000000ff
#define D3DTEXOPCAPS_ALL              0x00ffffff
#define D3DFVFCAPS_TEXCOORDCOUNTMASK  0x0000ffff
#define D3DFVFCAPS_PSIZE              0x00100000
#define D3DLIGHTINGMODEL_RGB          0x00000001
#define D3DLIGHTCAPS_POINT            0x00000001
#define D3DLIGHTCAPS_SPOT             0x00000002
#define D3DLIGHTCAPS_DIRECTIONAL      0x00000004
#define DDBD_16_                      0x00000400
#define DDBD_24_                      0x00000200
#define DDBD_32_                      0x00000100
/* --- caps --- */

typedef struct _D3DPT_DEVICEDESC_V1 {
  DWORD dwSize;
  DWORD dwFlags;
  D3DCOLORMODEL_ dcmColorModel;
  DWORD dwDevCaps;
  D3DTRANSFORMCAPS_ dtcTransformCaps;
  BOOL bClipping;
  D3DLIGHTINGCAPS_ dlcLightingCaps;
  D3DPRIMCAPS_ dpcLineCaps;
  D3DPRIMCAPS_ dpcTriCaps;
  DWORD dwDeviceRenderBitDepth;
  DWORD dwDeviceZBufferBitDepth;
  DWORD dwMaxBufferSize;
  DWORD dwMaxVertexCount;
} D3DDEVICEDESC_V1_, *LPD3DDEVICEDESC_V1_;

typedef struct _D3DPT_GLOBALDRIVERDATA {
  DWORD dwSize;
  D3DDEVICEDESC_V1_ hwCaps;
  DWORD dwNumVertices;
  DWORD dwNumClipVertices;
  DWORD dwNumTextureFormats;
  LPDDSURFACEDESC lpTextureFormats;
} D3DHAL_GLOBALDRIVERDATA_, *LPD3DHAL_GLOBALDRIVERDATA_;

typedef struct _D3DPT_D3DEXTENDEDCAPS {
  DWORD dwSize;
  DWORD dwMinTextureWidth, dwMaxTextureWidth;
  DWORD dwMinTextureHeight, dwMaxTextureHeight;
  DWORD dwMinStippleWidth, dwMaxStippleWidth;
  DWORD dwMinStippleHeight, dwMaxStippleHeight;
  DWORD dwMaxTextureRepeat;
  DWORD dwMaxTextureAspectRatio;
  DWORD dwMaxAnisotropy;
  D3DVALUE dvGuardBandLeft;
  D3DVALUE dvGuardBandTop;
  D3DVALUE dvGuardBandRight;
  D3DVALUE dvGuardBandBottom;
  D3DVALUE dvExtentsAdjust;
  DWORD dwStencilCaps;
  DWORD dwFVFCaps;
  DWORD dwTextureOpCaps;
  WORD wMaxTextureBlendStages;
  WORD wMaxSimultaneousTextures;
  DWORD dwMaxActiveLights;
  D3DVALUE dvMaxVertexW;
  WORD wMaxUserClipPlanes;
  WORD wMaxVertexBlendMatrices;
  DWORD dwVertexProcessingCaps;
  DWORD dwReserved1;
  DWORD dwReserved2;
  DWORD dwReserved3;
  DWORD dwReserved4;
} D3DHAL_D3DEXTENDEDCAPS_, *LPD3DHAL_D3DEXTENDEDCAPS_;
/* --- the DrawPrimitives2 token stream (d3dhal.h) --- */

typedef struct _D3DPT_DP2COMMAND {
  BYTE bCommand;
  BYTE bReserved;
  __GNU_EXTENSION union {
    WORD wPrimitiveCount;
    WORD wStateCount;
  };
} D3DHAL_DP2COMMAND_, *LPD3DHAL_DP2COMMAND_;
#define D3DDP2OP_RENDERSTATE_ 8

/* --- the DirectX 8 DDI (d3dhal.h of the DX8 DDK): GetDriverInfo2 and D3DCAPS8 --- */

#define D3DGDI2_MAGIC_                  0xFFFFFFFF
#define D3DGDI2_TYPE_GETD3DCAPS8_       0x00000001
#define D3DGDI2_TYPE_GETFORMATCOUNT_    0x00000002
#define D3DGDI2_TYPE_GETFORMAT_         0x00000003
#define D3DGDI2_TYPE_DXVERSION_         0x00000004

typedef struct _DD_GETDRIVERINFO2DATA_ {
  DWORD dwReserved;
  DWORD dwMagic;
  DWORD dwType;
  DWORD dwExpectedSize;
} DD_GETDRIVERINFO2DATA_;

typedef struct _DD_GETFORMATCOUNTDATA_ {
  DD_GETDRIVERINFO2DATA_ gdi2;
  DWORD dwFormatCount;
  DWORD dwReserved;
} DD_GETFORMATCOUNTDATA_;

typedef struct _DD_GETFORMATDATA_ {
  DD_GETDRIVERINFO2DATA_ gdi2;
  DWORD dwFormatIndex;
  DDPIXELFORMAT format;
} DD_GETFORMATDATA_;

typedef struct _DD_DXVERSION_ {
  DD_GETDRIVERINFO2DATA_ gdi2;
  DWORD dwDXVersion;
  DWORD dwReserved;
} DD_DXVERSION_;

typedef struct _D3DCAPS8_ {
  DWORD DeviceType;
  DWORD AdapterOrdinal;
  DWORD Caps;
  DWORD Caps2;
  DWORD Caps3;
  DWORD PresentationIntervals;
  DWORD CursorCaps;
  DWORD DevCaps;
  DWORD PrimitiveMiscCaps;
  DWORD RasterCaps;
  DWORD ZCmpCaps;
  DWORD SrcBlendCaps;
  DWORD DestBlendCaps;
  DWORD AlphaCmpCaps;
  DWORD ShadeCaps;
  DWORD TextureCaps;
  DWORD TextureFilterCaps;
  DWORD CubeTextureFilterCaps;
  DWORD VolumeTextureFilterCaps;
  DWORD TextureAddressCaps;
  DWORD VolumeTextureAddressCaps;
  DWORD LineCaps;
  DWORD MaxTextureWidth;
  DWORD MaxTextureHeight;
  DWORD MaxVolumeExtent;
  DWORD MaxTextureRepeat;
  DWORD MaxTextureAspectRatio;
  DWORD MaxAnisotropy;
  float MaxVertexW;
  float GuardBandLeft;
  float GuardBandTop;
  float GuardBandRight;
  float GuardBandBottom;
  float ExtentsAdjust;
  DWORD StencilCaps;
  DWORD FVFCaps;
  DWORD TextureOpCaps;
  DWORD MaxTextureBlendStages;
  DWORD MaxSimultaneousTextures;
  DWORD VertexProcessingCaps;
  DWORD MaxActiveLights;
  DWORD MaxUserClipPlanes;
  DWORD MaxVertexBlendMatrices;
  DWORD MaxVertexBlendMatrixIndex;
  float MaxPointSize;
  DWORD MaxPrimitiveCount;
  DWORD MaxVertexIndex;
  DWORD MaxStreams;
  DWORD MaxStreamStride;
  DWORD VertexShaderVersion;
  DWORD MaxVertexShaderConst;
  DWORD PixelShaderVersion;
  float MaxPixelShaderValue;
} D3DCAPS8_;

/* the DX8 format list: DDPIXELFORMAT with DDPF_D3DFORMAT, the D3DFORMAT in
 * dwFourCC and these in the dwRBitMask slot (dwOperations) */
#define DDPF_D3DFORMAT_                        0x00200000
/* the pixel-format flags d3d8.dll describes L8 / A8L8 / A4L4 and A8 with
 * (the 9x DDK's ddrawi.h has only the first) */
#define DDPF_ALPHA_                            0x00000002
#define DDPF_LUMINANCE_                        0x00020000
#define DDPF_BUMPLUMINANCE_                    0x00040000
#define D3DFORMAT_OP_TEXTURE_                  0x00000001
#define D3DFORMAT_OP_OFFSCREEN_RENDERTARGET_   0x00000008
#define D3DFORMAT_OP_SAME_FORMAT_RENDERTARGET_ 0x00000010
#define D3DFORMAT_OP_ZSTENCIL_                 0x00000040
#define D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH_ 0x00000080
#define D3DFORMAT_OP_DISPLAYMODE_              0x00000400
#define D3DFORMAT_OP_3DACCELERATION_           0x00000800

#define D3DDEVCAPS_PUREDEVICE         0x00100000
#define D3DPMISCCAPS_COLORWRITEENABLE 0x00000080
#define D3DPMISCCAPS_CLIPTLVERTS      0x00000200
#define D3DPMISCCAPS_TSSARGTEMP       0x00000400
#define D3DPMISCCAPS_BLENDOP          0x00000800
#define D3DPRASTERCAPS_MIPMAPLODBIAS  0x00002000
#define D3DPRASTERCAPS_COLORPERSPECTIVE 0x00400000
#define D3DPTEXTURECAPS_MIPMAP        0x00004000
#define D3DPTEXTURECAPS_ALPHAPALETTE  0x00000080   /* DX7: palette entries carry alpha (peFlags) */
#define D3DPTADDRESSCAPS_MIRRORONCE   0x00000020
#define D3DLINECAPS_TEXTURE           0x00000001
#define D3DLINECAPS_ZTEST             0x00000002
#define D3DLINECAPS_BLEND             0x00000004
#define D3DLINECAPS_ALPHACMP          0x00000008
#define D3DLINECAPS_FOG               0x00000010
#define D3DVTXPCAPS_TEXGEN            0x00000001
#define D3DVTXPCAPS_MATERIALSOURCE7   0x00000002
#define D3DVTXPCAPS_VERTEXFOG         0x00000004
#define D3DVTXPCAPS_DIRECTIONALLIGHTS 0x00000008
#define D3DVTXPCAPS_POSITIONALLIGHTS  0x00000010
#define D3DVTXPCAPS_LOCALVIEWER       0x00000020
#define D3DTRANSFORMCAPS_CLIP         0x00000001
#define D3DCAPS2_CANRENDERWINDOWED    0x00080000
#define D3DCAPS2_DYNAMICTEXTURES      0x20000000
#define D3DPRESENT_INTERVAL_ONE       0x00000001
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000
#define D3DVS_VERSION_0               0xFFFE0000
#define D3DPS_VERSION_0               0xFFFF0000
#define D3DVS_VERSION_(major, minor)  (0xFFFE0000 | ((major) << 8) | (minor))
#define D3DPS_VERSION_(major, minor)  (0xFFFF0000 | ((major) << 8) | (minor))
#define D3DDEVTYPE_HAL_               1

/* the DX8 DP2 tokens the driver rewrites or drops (D3DHAL_DP2OPERATION) */
#define D3DDP2OP_TEXBLT_                38
#define D3DDP2OP_CREATEVERTEXSHADER_    45
#define D3DDP2OP_DELETEVERTEXSHADER_    46
#define D3DDP2OP_SETVERTEXSHADER_       47
#define D3DDP2OP_SETVERTEXSHADERCONST_  48
#define D3DDP2OP_SETSTREAMSOURCE_       49
#define D3DDP2OP_SETSTREAMSOURCEUM_     50
#define D3DDP2OP_SETINDICES_            51
#define D3DDP2OP_DRAWPRIMITIVE_         52
#define D3DDP2OP_DRAWINDEXEDPRIMITIVE_  53
#define D3DDP2OP_CREATEPIXELSHADER_     54
#define D3DDP2OP_DELETEPIXELSHADER_     55
#define D3DDP2OP_SETPIXELSHADER_        56
#define D3DDP2OP_SETPIXELSHADERCONST_   57
#define D3DDP2OP_CLIPPEDTRIANGLEFAN_    58
#define D3DDP2OP_DRAWPRIMITIVE2_        59
#define D3DDP2OP_DRAWINDEXEDPRIMITIVE2_ 60
#define D3DDP2OP_DRAWRECTPATCH_         61
#define D3DDP2OP_DRAWTRIPATCH_          62
#define D3DDP2OP_VOLUMEBLT_             63
#define D3DDP2OP_BUFFERBLT_             64
#define D3DDP2OP_MULTIPLYTRANSFORM_     65
#define D3DDP2OP_ADDDIRTYRECT_          66
#define D3DDP2OP_ADDDIRTYBOX_           67

#define D3DERR_COMMAND_UNPARSED_ 0x88760BB8

#ifdef __cplusplus
}
#endif

#endif /* D3DPT_DDI_H */

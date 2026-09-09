/*
 * ddhal32.h — the 32-bit DirectDraw HAL call structures the ring-3 half
 * of the 9x driver implements (doc 19 §1).
 *
 * mingw-w64 ships `ddrawi.h`, but only with the *pointer* typedefs for
 * the callback data — the bodies are DDK material it does not carry. Our
 * rule is the XP driver's (doc 15): no Microsoft DDK, so the handful we
 * actually implement are spelled out here from the published interface
 * documentation, exactly as `ddk/d3dnthal.h` does for NT. Layouts are the
 * DDI's; the file grows one structure at a time as callbacks are added,
 * and nothing that is not implemented is declared.
 *
 * `lpDD` is the runtime's own DirectDraw object. The HAL here never
 * dereferences it — everything it needs is in the shared block
 * (`d3dpt9hal.h`) — so it stays a `void *` rather than dragging in the
 * whole `DDRAWI_DIRECTDRAW_GBL` chain.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DDHAL32_H
#define DDHAL32_H

/* What a HAL callback returns: it dealt with the call (the ddRVal in the
 * data is the answer), or it did not and the runtime's own HEL should. */
#ifndef DDHAL_DRIVER_HANDLED
#define DDHAL_DRIVER_NOTHANDLED 0
#define DDHAL_DRIVER_HANDLED    1
#define DDHAL_DRIVER_NOCKEYHW   2
#endif

/* Return if the vertical blank is in progress. Not in ddraw.h with the
 * other DDWAITVB_*: it is the DDI-only value the runtime sends a driver. */
#ifndef DDWAITVB_I_TESTVB
#define DDWAITVB_I_TESTVB   0x80000006ul
#endif

#ifndef DDHAL_PLEASEALLOC_BLOCKSIZE
#define DDHAL_PLEASEALLOC_BLOCKSIZE 0x00000002ul
#endif

typedef struct d3dpt_ddhal_waitvb {
    void *lpDD;                 /* the runtime's DirectDraw object */
    DWORD dwFlags;              /* DDWAITVB_* */
    DWORD bIsInVB;              /* out, for DDWAITVB_I_TESTVB */
    DWORD hEvent;
    HRESULT ddRVal;             /* out */
    void *WaitForVerticalBlank; /* the runtime's own pointer back to us */
} d3dpt_ddhal_waitvb;

/* The two DirectDraw-object callbacks that stand between an application
 * asking for a surface and the runtime allocating one. `CanCreateSurface`
 * is the veto — the driver says whether it could back this description at
 * all — and `CreateSurface` is where a driver that wants to place the
 * surface itself does so, by writing `fpVidMem` into the surface objects
 * the runtime passes in. Declining either is legal on 9x and leaves the
 * work to the runtime, which is what both do for now. */
typedef struct d3dpt_ddhal_cancreatesurface {
    void *lpDD;
    LPDDSURFACEDESC lpDDSurfaceDesc;
    DWORD bIsDifferentPixelFormat;
    HRESULT ddRVal;             /* out */
    void *CanCreateSurface;
} d3dpt_ddhal_cancreatesurface;

typedef struct d3dpt_ddhal_createsurface {
    void *lpDD;
    LPDDSURFACEDESC lpDDSurfaceDesc;
    void **lplpSList;           /* the surfaces, as DDRAWI_DDRAWSURFACE_LCL* */
    DWORD dwSCnt;
    HRESULT ddRVal;             /* out */
    void *CreateSurface;
} d3dpt_ddhal_createsurface;

typedef struct d3dpt_ddhal_destroydriver {
    void *lpDD;
    HRESULT ddRVal;             /* out */
    void *DestroyDriver;
} d3dpt_ddhal_destroydriver;

typedef struct d3dpt_ddhal_setmode {
    void *lpDD;
    DWORD dwModeIndex;
    HRESULT ddRVal;             /* out */
    void *SetMode;
    BOOL inexcl;
    BOOL useRefreshRate;
} d3dpt_ddhal_setmode;

typedef struct d3dpt_ddhal_modeinfo {
    DWORD dwWidth;
    DWORD dwHeight;
    LONG  lPitch;
    DWORD dwBPP;
    WORD  wFlags;
    WORD  wRefreshRate;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwAlphaBitMask;
} d3dpt_ddhal_modeinfo;

/* Surface callbacks (DDHAL_DDSURFACECALLBACKS) */

typedef struct d3dpt_ddhal_destroysurface {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    HRESULT ddRVal;             /* out */
    void *DestroySurface;
} d3dpt_ddhal_destroysurface;

typedef struct d3dpt_ddhal_flip {
    void *lpDD;
    void *lpSurfCurr;           /* DDRAWI_DDRAWSURFACE_LCL* */
    void *lpSurfTarg;           /* DDRAWI_DDRAWSURFACE_LCL* */
    DWORD dwFlags;              /* DDFLIP_* */
    HRESULT ddRVal;             /* out */
    void *Flip;
    void *lpSurfCurrLeft;
    void *lpSurfTargLeft;
} d3dpt_ddhal_flip;

typedef struct d3dpt_ddhal_getflipstatus {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    DWORD dwFlags;              /* DDGFS_* */
    HRESULT ddRVal;             /* out */
    void *GetFlipStatus;
} d3dpt_ddhal_getflipstatus;

typedef struct d3dpt_ddhal_getbltstatus {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    DWORD dwFlags;              /* DDGBS_* */
    HRESULT ddRVal;             /* out */
    void *GetBltStatus;
} d3dpt_ddhal_getbltstatus;

typedef struct d3dpt_ddhal_lock {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    DWORD bHasRect;
    RECTL rArea;
    LPVOID lpSurfData;
    HRESULT ddRVal;             /* out */
    void *Lock;
    DWORD dwFlags;              /* DDLOCK_* */
} d3dpt_ddhal_lock;

typedef struct d3dpt_ddhal_unlock {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    HRESULT ddRVal;             /* out */
    void *Unlock;
} d3dpt_ddhal_unlock;

typedef struct d3dpt_ddhal_blt {
    void *lpDD;
    void *lpDDDestSurface;      /* DDRAWI_DDRAWSURFACE_LCL* */
    RECTL rDest;
    void *lpDDSrcSurface;       /* DDRAWI_DDRAWSURFACE_LCL* */
    RECTL rSrc;
    DWORD dwFlags;              /* DDBLT_* */
    DWORD dwROPFlags;
    DDBLTFX bltFX;
    HRESULT ddRVal;             /* out */
    void *Blt;
    BOOL IsClipped;
    RECTL rOrigDest;
    RECTL rOrigSrc;
    DWORD dwRectCnt;
    LPRECT prDestRects;
} d3dpt_ddhal_blt;

typedef struct d3dpt_ddhal_setcolorkey {
    void *lpDD;
    void *lpDDSurface;          /* DDRAWI_DDRAWSURFACE_LCL* */
    DWORD dwFlags;              /* DDCKEY_* */
    DDCOLORKEY ckNew;
    HRESULT ddRVal;             /* out */
    void *SetColorKey;
} d3dpt_ddhal_setcolorkey;

/* D3D callback flags */
#ifndef D3DHAL2_CB32_SETRENDERTARGET
#define D3DHAL2_CB32_SETRENDERTARGET            0x00000001ul
#define D3DHAL2_CB32_CLEAR                      0x00000002ul
#endif

#ifndef D3DHAL3_CB32_CLEAR2
#define D3DHAL3_CB32_CLEAR2                     0x00000001ul
#define D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE  0x00000004ul
#define D3DHAL3_CB32_DRAWPRIMITIVES2            0x00000008ul
#endif

#ifndef DDHAL_MISC2CB32_CREATESURFACEEX
#define DDHAL_MISC2CB32_CREATESURFACEEX         0x00000002ul
#define DDHAL_MISC2CB32_GETDRIVERSTATE          0x00000004ul
#endif

#ifndef DDHAL_EXEBUFCB32_CANCREATEEXEBUF
#define DDHAL_EXEBUFCB32_CANCREATEEXEBUF        0x00000001ul
#define DDHAL_EXEBUFCB32_CREATEEXEBUF           0x00000002ul
#define DDHAL_EXEBUFCB32_DESTROYEXEBUF          0x00000004ul
#define DDHAL_EXEBUFCB32_LOCKEXEBUF             0x00000008ul
#define DDHAL_EXEBUFCB32_UNLOCKEXEBUF           0x00000010ul
#endif

typedef struct _DDHAL_CREATESURFACEEXDATA {
    DWORD dwFlags;
    LPDDRAWI_DIRECTDRAW_LCL lpDDLcl;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    HRESULT ddRVal;
} DDHAL_CREATESURFACEEXDATA, *LPDDHAL_CREATESURFACEEXDATA;

typedef struct _DDHAL_GETDRIVERSTATEDATA {
    DWORD dwFlags;
    union {
        ULONG_PTR dwhContext;
    };
    LPDDRAWI_DIRECTDRAW_LCL lpDD;
    DWORD dwWhichData;
    DWORD dwActualSize;
    DWORD dwExpectedSize;
    LPVOID lpvData;
    HRESULT ddRVal;
} DDHAL_GETDRIVERSTATEDATA, *LPDDHAL_GETDRIVERSTATEDATA;

typedef struct _D3DHAL_CLEAR2DATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    DWORD dwFillColor;
    D3DVALUE dvFillDepth;
    DWORD dwFillStencil;
    LPD3DRECT_ lpRects;
    DWORD dwNumRects;
    HRESULT ddrval;
} D3DHAL_CLEAR2DATA, *LPD3DHAL_CLEAR2DATA;

typedef struct _D3DHAL_VALIDATETEXTURESTAGESTATEDATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    ULONG_PTR dwReserved;
    DWORD dwNumPasses;
    HRESULT ddrval;
} D3DHAL_VALIDATETEXTURESTAGESTATEDATA, *LPD3DHAL_VALIDATETEXTURESTAGESTATEDATA;

typedef struct _D3DHAL_CONTEXTCREATEDATA {
    union {
        LPDDRAWI_DIRECTDRAW_GBL lpDDGbl;
        LPDDRAWI_DIRECTDRAW_LCL lpDDLcl;
    };
    union {
        LPDIRECTDRAWSURFACE lpDDS;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    };
    union {
        LPDIRECTDRAWSURFACE lpDDSZ;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSZLcl;
    };
    DWORD dwPID;
    ULONG_PTR dwhContext;
    HRESULT ddrval;
} D3DHAL_CONTEXTCREATEDATA, *LPD3DHAL_CONTEXTCREATEDATA;

typedef struct _D3DHAL_CONTEXTDESTROYDATA {
    ULONG_PTR dwhContext;
    HRESULT ddrval;
} D3DHAL_CONTEXTDESTROYDATA, *LPD3DHAL_CONTEXTDESTROYDATA;

typedef struct _D3DHAL_CONTEXTDESTROYALLDATA {
    DWORD dwPID;
    HRESULT ddrval;
} D3DHAL_CONTEXTDESTROYALLDATA, *LPD3DHAL_CONTEXTDESTROYALLDATA;

typedef struct _D3DHAL_SCENECAPTUREDATA {
    ULONG_PTR dwhContext;
    DWORD dwFlag;
    HRESULT ddrval;
} D3DHAL_SCENECAPTUREDATA, *LPD3DHAL_SCENECAPTUREDATA;
#define D3DHAL_SCENE_CAPTURE_START      0x00000000ul
#define D3DHAL_SCENE_CAPTURE_END        0x00000001ul

typedef struct _D3DHAL_SETRENDERTARGETDATA {
    ULONG_PTR dwhContext;
    union {
        LPDIRECTDRAWSURFACE lpDDS;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    };
    union {
        LPDIRECTDRAWSURFACE lpDDSZ;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSZLcl;
    };
    HRESULT ddrval;
} D3DHAL_SETRENDERTARGETDATA, *LPD3DHAL_SETRENDERTARGETDATA;

typedef struct _D3DHAL_DRAWPRIMITIVES2DATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    DWORD dwVertexType;
    LPDDRAWI_DDRAWSURFACE_LCL lpDDCommands;
    DWORD dwCommandOffset;
    DWORD dwCommandLength;
    union {
        LPDDRAWI_DDRAWSURFACE_LCL lpDDVertex;
        LPVOID lpVertices;
    };
    DWORD dwVertexOffset;
    DWORD dwVertexLength;
    DWORD dwReqVertexBufSize;
    DWORD dwReqCommandBufSize;
    LPDWORD lpdwRStates;
    union {
        DWORD dwVertexSize;
        HRESULT ddrval;
    };
    DWORD dwErrorOffset;
} D3DHAL_DRAWPRIMITIVES2DATA, *LPD3DHAL_DRAWPRIMITIVES2DATA;

#define D3DHALDP2_USERMEMVERTICES       0x00000001ul
#define D3DHALDP2_EXECUTEBUFFER         0x00000002ul
#define D3DHALDP2_SWAPVERTEXBUFFER      0x00000004ul
#define D3DHALDP2_SWAPCOMMANDBUFFER     0x00000008ul
#define D3DHALDP2_REQVERTEXBUFSIZE      0x00000010ul
#define D3DHALDP2_REQCOMMANDBUFSIZE     0x00000020ul

typedef struct _D3DHAL_TEXTURECREATEDATA {
    ULONG_PTR dwhContext;
    union {
        LPDIRECTDRAWSURFACE lpDDS;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    };
    DWORD dwHandle;
    HRESULT ddrval;
} D3DHAL_TEXTURECREATEDATA, *LPD3DHAL_TEXTURECREATEDATA;

typedef struct _D3DHAL_TEXTUREDESTROYDATA {
    ULONG_PTR dwhContext;
    DWORD dwHandle;
    HRESULT ddrval;
} D3DHAL_TEXTUREDESTROYDATA, *LPD3DHAL_TEXTUREDESTROYDATA;

typedef struct _D3DHAL_TEXTURESWAPDATA {
    ULONG_PTR dwhContext;
    DWORD dwHandle1;
    DWORD dwHandle2;
    HRESULT ddrval;
} D3DHAL_TEXTURESWAPDATA, *LPD3DHAL_TEXTURESWAPDATA;

typedef struct _D3DHAL_TEXTUREGETSURFDATA {
    ULONG_PTR dwhContext;
    union {
        LPDIRECTDRAWSURFACE lpDDS;
        LPDDRAWI_DDRAWSURFACE_LCL lpDDSLcl;
    };
    DWORD dwHandle;
    HRESULT ddrval;
} D3DHAL_TEXTUREGETSURFDATA, *LPD3DHAL_TEXTUREGETSURFDATA;

typedef DWORD (__stdcall *LPD3DHAL_CONTEXTCREATECB)(LPD3DHAL_CONTEXTCREATEDATA);
typedef DWORD (__stdcall *LPD3DHAL_CONTEXTDESTROYCB)(LPD3DHAL_CONTEXTDESTROYDATA);
typedef DWORD (__stdcall *LPD3DHAL_CONTEXTDESTROYALLCB)(LPD3DHAL_CONTEXTDESTROYALLDATA);
typedef DWORD (__stdcall *LPD3DHAL_SCENECAPTURECB)(LPD3DHAL_SCENECAPTUREDATA);
typedef DWORD (__stdcall *LPD3DHAL_TEXTURECREATECB)(LPD3DHAL_TEXTURECREATEDATA);
typedef DWORD (__stdcall *LPD3DHAL_TEXTUREDESTROYCB)(LPD3DHAL_TEXTUREDESTROYDATA);
typedef DWORD (__stdcall *LPD3DHAL_TEXTURESWAPCB)(LPD3DHAL_TEXTURESWAPDATA);
typedef DWORD (__stdcall *LPD3DHAL_TEXTUREGETSURFCB)(LPD3DHAL_TEXTUREGETSURFDATA);

typedef struct _D3DHAL_CALLBACKS {
    DWORD dwSize;
    LPD3DHAL_CONTEXTCREATECB ContextCreate;
    LPD3DHAL_CONTEXTDESTROYCB ContextDestroy;
    LPD3DHAL_CONTEXTDESTROYALLCB ContextDestroyAll;
    LPD3DHAL_SCENECAPTURECB SceneCapture;
    LPVOID lpReserved10;
    LPVOID lpReserved11;
    LPVOID RenderState;
    LPVOID RenderPrimitive;
    DWORD dwReserved;
    LPD3DHAL_TEXTURECREATECB TextureCreate;
    LPD3DHAL_TEXTUREDESTROYCB TextureDestroy;
    LPD3DHAL_TEXTURESWAPCB TextureSwap;
    LPD3DHAL_TEXTUREGETSURFCB TextureGetSurf;
    LPVOID lpReserved12;
    LPVOID lpReserved13;
    LPVOID lpReserved14;
    LPVOID lpReserved15;
    LPVOID lpReserved16;
    LPVOID lpReserved17;
    LPVOID lpReserved18;
    LPVOID lpReserved19;
    LPVOID lpReserved20;
    LPVOID lpReserved21;
    LPVOID GetState;
    DWORD dwReserved0;
    DWORD dwReserved1;
    DWORD dwReserved2;
    DWORD dwReserved3;
    DWORD dwReserved4;
    DWORD dwReserved5;
    DWORD dwReserved6;
    DWORD dwReserved7;
    DWORD dwReserved8;
    DWORD dwReserved9;
} D3DHAL_CALLBACKS, *LPD3DHAL_CALLBACKS;

typedef DWORD (__stdcall *LPD3DHAL_SETRENDERTARGETCB)(LPD3DHAL_SETRENDERTARGETDATA);

typedef struct _D3DHAL_CLEARDATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    DWORD dwFillColor;
    DWORD dwFillDepth;
    LPD3DRECT_ lpRects;
    DWORD dwNumRects;
    HRESULT ddrval;
} D3DHAL_CLEARDATA, *LPD3DHAL_CLEARDATA;

typedef DWORD (__stdcall *LPD3DHAL_CLEARCB)(LPD3DHAL_CLEARDATA);

typedef struct _D3DHAL_CALLBACKS2 {
    DWORD dwSize;
    DWORD dwFlags;
    LPD3DHAL_SETRENDERTARGETCB SetRenderTarget;
    LPD3DHAL_CLEARCB Clear;
    LPVOID DrawOnePrimitive;
    LPVOID DrawOneIndexedPrimitive;
    LPVOID DrawPrimitives;
} D3DHAL_CALLBACKS2, *LPD3DHAL_CALLBACKS2;

typedef DWORD (__stdcall *LPD3DHAL_CLEAR2CB)(LPD3DHAL_CLEAR2DATA);
typedef DWORD (__stdcall *LPD3DHAL_VALIDATETEXTURESTAGESTATECB)(LPD3DHAL_VALIDATETEXTURESTAGESTATEDATA);
typedef DWORD (__stdcall *LPD3DHAL_DRAWPRIMITIVES2CB)(LPD3DHAL_DRAWPRIMITIVES2DATA);

typedef struct _D3DHAL_CALLBACKS3 {
    DWORD dwSize;
    DWORD dwFlags;
    LPD3DHAL_CLEAR2CB Clear2;
    LPVOID lpvReserved;
    LPD3DHAL_VALIDATETEXTURESTAGESTATECB ValidateTextureStageState;
    LPD3DHAL_DRAWPRIMITIVES2CB DrawPrimitives2;
} D3DHAL_CALLBACKS3, *LPD3DHAL_CALLBACKS3;

#endif

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

#endif

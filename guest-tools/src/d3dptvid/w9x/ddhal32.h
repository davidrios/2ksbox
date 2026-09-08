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

typedef struct d3dpt_ddhal_waitvb {
    void *lpDD;                 /* the runtime's DirectDraw object */
    DWORD dwFlags;              /* DDWAITVB_* */
    DWORD bIsInVB;              /* out, for DDWAITVB_I_TESTVB */
    DWORD hEvent;
    HRESULT ddRVal;             /* out */
    void *WaitForVerticalBlank; /* the runtime's own pointer back to us */
} d3dpt_ddhal_waitvb;

typedef struct d3dpt_ddhal_destroydriver {
    void *lpDD;
    HRESULT ddRVal;             /* out */
    void *DestroyDriver;
} d3dpt_ddhal_destroydriver;

#endif

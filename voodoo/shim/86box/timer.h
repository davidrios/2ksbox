/*
 * timer.h -- 2ksbox's stand-in for 86Box's <86box/timer.h>.
 *
 * 86Box timers carry a 32.32 fixed-point timestamp in TSC ticks. Here a
 * *delay* is nanoseconds of QEMU_CLOCK_VIRTUAL in that same 32.32 form
 * (TIMER_USEC is 1000 << 32; `cpuclock` (video.h) is 1e9 so
 * voodoo_pixelclock_update's clock_const is ns per pixel and its
 * `* (1ULL << 32)` makes a line time in these units), while the *expiry*
 * a timer holds is ns << 16: 48 bits of nanoseconds (78 hours) rather
 * than the 32 (4.3 s -- a wrapped expiry re-arms in the past and the main
 * loop never leaves the timer) and a fraction fine enough not to drift.
 * `tsc` is the virtual clock in ns. Every timer is a QEMUTimer underneath and
 * fires on the main loop with the BQL held -- the same lock the guest's MMIO
 * writes run under, which is what makes 86Box's "the timer and the CPU are
 * one thread" assumptions hold.
 *
 * The callback contract is upstream's: a timer is disabled when its callback
 * runs and stays so unless the callback re-arms it (timer_advance_u64).
 * voodoo_shim.c adds one thing 86Box does not need: a timer whose callback
 * re-arms it a few microseconds ahead (the per-scanline display timer, ~32 us)
 * is run in a loop until it is at least `slack_ns` ahead, so QEMU's main
 * loop wakes ~1000 times a second for it rather than ~31000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_TIMER_H
#define VOODOO_SHIM_TIMER_H

#include <stdint.h>

typedef struct pc_timer_t {
    void    *qt;        /* QEMUTimer */
    uint64_t ts;        /* expiry, ns << 16 */
    void (*callback)(void *priv);
    void    *priv;
    int      enabled;
    int      in_fire;   /* inside the callback: re-arms are batched */
    uint64_t slack_ns;  /* see above; 0 = fire exactly */
} pc_timer_t;

extern uint64_t TIMER_USEC;

#define VOODOO_SHIM_TS_SHIFT 16             /* expiry fraction bits */
#define VOODOO_SHIM_DELAY_SHIFT (32 - VOODOO_SHIM_TS_SHIFT)

/* QEMU_CLOCK_VIRTUAL now, in this file's units */
extern uint64_t voodoo_shim_now_ts(void);   /* ns << VOODOO_SHIM_TS_SHIFT */
extern uint64_t voodoo_shim_tsc(void);      /* ns */
#define tsc (voodoo_shim_tsc())

extern void timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer);
extern void timer_enable(pc_timer_t *timer);
extern void timer_disable(pc_timer_t *timer);

static inline void
timer_advance_u64(pc_timer_t *timer, uint64_t delay)
{
    timer->ts += delay >> VOODOO_SHIM_DELAY_SHIFT;
    timer_enable(timer);
}

static inline void
timer_set_delay_u64(pc_timer_t *timer, uint64_t delay)
{
    timer->ts = voodoo_shim_now_ts() + (delay >> VOODOO_SHIM_DELAY_SHIFT);
    timer_enable(timer);
}

static inline int
timer_is_enabled(pc_timer_t *timer)
{
    return timer->enabled;
}

static inline uint64_t
timer_get_ts_int(pc_timer_t *timer)
{
    return timer->ts >> VOODOO_SHIM_TS_SHIFT;
}

#endif /* VOODOO_SHIM_TIMER_H */

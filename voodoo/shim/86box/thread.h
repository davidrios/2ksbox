/*
 * thread.h -- 2ksbox's stand-in for 86Box's <86box/thread.h>, the same API
 * (threads, manual-reset events with a millisecond timeout, mutexes) over
 * QEMU's qemu-thread. The semantics that matter, and are kept: an event stays
 * set until reset; thread_wait_event returns 1 on timeout and 0 when the
 * event was set; a negative timeout waits for ever.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_THREAD_H
#define VOODOO_SHIM_THREAD_H

typedef void thread_t;
typedef void event_t;
typedef void mutex_t;

#define thread_create(thread_func, param) thread_create_named((thread_func), (param), #thread_func)
extern thread_t *thread_create_named(void (*thread_func)(void *param), void *param, const char *name);
extern int       thread_wait(thread_t *arg);
extern event_t  *thread_create_event(void);
extern void      thread_set_event(event_t *arg);
extern void      thread_reset_event(event_t *arg);
extern int       thread_wait_event(event_t *arg, int timeout);
extern void      thread_destroy_event(event_t *arg);

extern mutex_t *thread_create_mutex(void);
extern void     thread_close_mutex(mutex_t *arg);
extern int      thread_test_mutex(mutex_t *arg);
extern int      thread_wait_mutex(mutex_t *arg);
extern int      thread_release_mutex(mutex_t *mutex);

#endif /* VOODOO_SHIM_THREAD_H */

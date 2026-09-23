/*
 * An LD_PRELOAD that makes QEMU's main loop wait the
 * way it waits on a Windows host: every ppoll() with a timeout sleeps to
 * the next multiple of WAIT_GRANULARITY_NS (15.625 ms, Windows' default
 * timer tick, when unset).
 *
 * QEMU on Windows waits in WaitForMultipleObjects with a millisecond
 * timeout, and Windows rounds that to its timer tick unless the process
 * has asked for a finer one (timeBeginPeriod). Anything paced by a QEMU
 * timer then fires a tick late, which is how a Windows host lost 15 of
 * every 16 of a Win98 guest's 1 kHz timer interrupts (patch 65). This is
 * that host, on Linux, for tools/pit-guest-test.py.
 *
 *   cc -O2 -shared -fPIC -o build/wait-granularity.so tools/wait-granularity.c -ldl
 *   LD_PRELOAD=build/wait-granularity.so qemu-system-i386 ...
 *
 * Linux only (glibc's ppoll, RTLD_NEXT).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *to, const sigset_t *mask)
{
    static int (*real)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
    static long tick;
    struct timespec now, t;
    long ns, cur, end;

    if (!real) {
        const char *e = getenv("WAIT_GRANULARITY_NS");
        real = dlsym(RTLD_NEXT, "ppoll");
        tick = e ? atol(e) : 15625000L;
    }
    if (!to || tick <= 0) {
        return real(fds, n, to, mask);
    }
    ns = to->tv_sec * 1000000000L + to->tv_nsec;
    if (ns <= 0) {
        return real(fds, n, to, mask);
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    cur = now.tv_sec * 1000000000L + now.tv_nsec;
    end = (cur + ns + tick - 1) / tick * tick;      /* the next tick boundary */
    t.tv_sec = (end - cur) / 1000000000L;
    t.tv_nsec = (end - cur) % 1000000000L;
    return real(fds, n, &t, mask);
}

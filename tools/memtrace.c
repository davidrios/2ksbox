/*
 * memtrace.so: who does QEMU's large memcpy/memmove/memset? perf cannot
 * unwind out of glibc's AVX copy loops, so the calls are counted where they
 * happen: LD_PRELOAD it into the process under test; calls of >= 4 KiB are
 * counted per return address and written, with /proc/self/maps, every
 * MT_EVERY large calls and at exit to $MEMTRACE_OUT.<pid>
 * (docs/tracks/m9-tcg-aarch64.md, "Win98 3D").
 *
 *   gcc -O2 -shared -fPIC -fno-builtin -o memtrace.so tools/memtrace.c -ldl -lpthread
 *   printf '#!/bin/sh\nLD_PRELOAD=%s/memtrace.so MEMTRACE_OUT=%s/mt exec %s "$@"\n' \
 *       $PWD $PWD $PWD/build/qemu/qemu-system-i386 > qemu-mt.sh; chmod +x qemu-mt.sh
 *   QEMU_BIN=$PWD/qemu-mt.sh tools/w98-3dmark.sh mt
 *
 * then resolve each return address through the dump's maps with
 * `addr2line -f -C -e <mapped file> <offset>`. Diagnostic, not a test.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MT_MIN   4096
#define MT_SLOTS 4096
#define MT_EVERY 200000

typedef void *(*cpy_t)(void *, const void *, size_t);
typedef void *(*set_t)(void *, int, size_t);
static cpy_t real_memcpy, real_memmove;
static set_t real_memset;
static struct { uintptr_t ra; unsigned kind; unsigned long calls, bytes; } tab[MT_SLOTS];
static unsigned long large;
static pthread_mutex_t mt_lock = PTHREAD_MUTEX_INITIALIZER;
static int resolving;

static void resolve(void)
{
    if (real_memcpy || resolving) {
        return;
    }
    resolving = 1;
    real_memcpy = (cpy_t)dlsym(RTLD_NEXT, "memcpy");
    real_memmove = (cpy_t)dlsym(RTLD_NEXT, "memmove");
    real_memset = (set_t)dlsym(RTLD_NEXT, "memset");
    resolving = 0;
}

static void dump(void)
{
    const char *base = getenv("MEMTRACE_OUT");
    char path[512], line[512];
    FILE *f, *m;
    int i;

    if (!base) {
        return;
    }
    snprintf(path, sizeof path, "%s.%d", base, (int)getpid());
    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "# large calls %lu\n", large);
    for (i = 0; i < MT_SLOTS; i++) {
        if (tab[i].calls) {
            fprintf(f, "%c %#lx %lu %lu\n", "cms"[tab[i].kind], (unsigned long)tab[i].ra,
                    tab[i].calls, tab[i].bytes);
        }
    }
    m = fopen("/proc/self/maps", "r");
    if (m) {
        fprintf(f, "# maps\n");
        while (fgets(line, sizeof line, m)) {
            fputs(line, f);
        }
        fclose(m);
    }
    fclose(f);
}

static void note(uintptr_t ra, unsigned kind, size_t n)
{
    unsigned h = (unsigned)((ra * 0x9E3779B97F4A7C15ull) >> 52) & (MT_SLOTS - 1);
    int probe;

    pthread_mutex_lock(&mt_lock);
    for (probe = 0; probe < 64; probe++, h = (h + 1) & (MT_SLOTS - 1)) {
        if (tab[h].ra == ra && tab[h].kind == kind) {
            break;
        }
        if (!tab[h].ra) {
            tab[h].ra = ra;
            tab[h].kind = kind;
            break;
        }
    }
    if (probe < 64) {
        tab[h].calls++;
        tab[h].bytes += n;
    }
    if (++large % MT_EVERY == 0) {
        dump();
    }
    pthread_mutex_unlock(&mt_lock);
}

static void *slow_copy(void *d, const void *s, size_t n)
{
    unsigned char *dd = d;
    const unsigned char *ss = s;

    if (dd < ss) {
        while (n--) *dd++ = *ss++;
    } else {
        while (n--) dd[n] = ss[n];
    }
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    resolve();
    if (!real_memcpy) {
        return slow_copy(d, s, n);
    }
    if (n >= MT_MIN) {
        note((uintptr_t)__builtin_return_address(0), 0, n);
    }
    return real_memcpy(d, s, n);
}

void *memmove(void *d, const void *s, size_t n)
{
    resolve();
    if (!real_memmove) {
        return slow_copy(d, s, n);
    }
    if (n >= MT_MIN) {
        note((uintptr_t)__builtin_return_address(0), 1, n);
    }
    return real_memmove(d, s, n);
}

void *memset(void *d, int c, size_t n)
{
    resolve();
    if (!real_memset) {
        unsigned char *p = d;
        while (n--) *p++ = (unsigned char)c;
        return d;
    }
    if (n >= MT_MIN) {
        note((uintptr_t)__builtin_return_address(0), 2, n);
    }
    return real_memset(d, c, n);
}

void *__memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }

__attribute__((destructor)) static void mt_fini(void) { dump(); }

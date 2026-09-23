/*
 * An LD_PRELOAD that fails pread64() with EIO on one
 * file, over one range of bytes: a network share's bad moment, on demand.
 *
 *   READ_ERROR_MATCH=<substring of the file's path>
 *   READ_ERROR_FROM=<byte offset> READ_ERROR_TO=<byte offset, exclusive>
 *
 * Every read that overlaps the range fails, every time. libdisc reads an
 * image's payload with pread64 (Rust's FileExt::read_exact_at), so this is
 * how `ATAPI_READ_ERROR=1 tools/atapi-guest-test.py` (the `atapi-read-error`
 * check) puts four unreadable sectors in the middle of a CD audio track
 * (patch 55). Linux only (glibc, /proc/self/fd).
 *
 *   cc -O2 -shared -fPIC -o build/read-error-inject.so tools/read-error-inject.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int bad_fd(int fd) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    return getenv("READ_ERROR_MATCH") && strstr(path, getenv("READ_ERROR_MATCH"));
}
ssize_t pread64(int fd, void *buf, size_t count, off_t off) {
    static ssize_t (*real)(int, void *, size_t, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "pread64");
    long from = getenv("READ_ERROR_FROM") ? atol(getenv("READ_ERROR_FROM")) : 0, to = getenv("READ_ERROR_TO") ? atol(getenv("READ_ERROR_TO")) : 0;
    if (off < to && off + (off_t)count > from && bad_fd(fd)) {
        static int said;
        if (said++ < 3) fprintf(stderr, "read-error-inject: failing pread64 at %ld\n", (long)off);
        errno = EIO;
        return -1;
    }
    return real(fd, buf, count, off);
}

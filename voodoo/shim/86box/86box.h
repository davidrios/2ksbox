/*
 * 86box.h -- 2ksbox's stand-in for 86Box's <86box/86box.h>, as seen by the
 * vendored Voodoo sources (voodoo/86box/). Only what those files use: the
 * MIN/MAX/ABS helpers, the atomics (verbatim from upstream, because the
 * FIFO and render threads' contract is written in them) and the two log
 * entry points, which voodoo_shim.c routes into QEMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_86BOX_H
#define VOODOO_SHIM_86BOX_H

#include <stdint.h>
#include <stdarg.h>

/* guarded: qemu/osdep.h defines MIN/MAX too, and the shim's own .c files
 * include both */
#ifndef MIN
#    define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ABS
#    define ABS(x) ((x) > 0 ? (x) : -(x))
#endif
#define ABSD(x) ((x) > 0.0 ? (x) : -(x))

#define AS_FLOAT(x)  (*((float *) &(x)))
#define AS_DOUBLE(x) (*((double *) &(x)))

#if defined(__GNUC__) || defined(__clang__)
#    define UNLIKELY(x) __builtin_expect(!!(x), 0)
#    define LIKELY(x)   __builtin_expect(!!(x), 1)
#else
#    define UNLIKELY(x) (x)
#    define LIKELY(x)   (x)
#endif

/* Upstream's atomics, unchanged: x86 relies on aligned int stores being
 * atomic and uses volatile; everything else uses C11 atomics. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#    define ATOMIC_INT volatile int
#    define ATOMIC_UINT volatile uint32_t
#    define ATOMIC_DOUBLE volatile double
#    define ATOMIC_LOAD(var) (var)
#    define ATOMIC_STORE(var, val) ((var) = (val))
#    define ATOMIC_INC(var) (++(var))
#    define ATOMIC_DEC(var) (--(var))
#    define ATOMIC_ADD(var, val) ((var) += (val))
#    define ATOMIC_SUB(var, val) ((var) -= (val))
#    define ATOMIC_DOUBLE_ADD(var, val) ((var) += (val))
#else
#    ifndef __cplusplus
#        include <stdatomic.h>
#    endif
#    define ATOMIC_INT atomic_int
#    define ATOMIC_UINT atomic_uint
#    define ATOMIC_DOUBLE _Atomic double
#    define ATOMIC_LOAD(var) atomic_load(&(var))
#    define ATOMIC_STORE(var, val) atomic_store(&(var), (val))
#    define ATOMIC_INC(var) atomic_fetch_add(&(var), 1)
#    define ATOMIC_DEC(var) atomic_fetch_sub(&(var), 1)
#    define ATOMIC_ADD(var, val) atomic_fetch_add(&(var), val)
#    define ATOMIC_SUB(var, val) atomic_fetch_sub(&(var), val)
#    define ATOMIC_DOUBLE_ADD(var, val) atomic_double_add(&(var), val)
#endif

/* voodoo_shim.c: pclog goes to stderr with a "voodoo2:" prefix, fatal is
 * upstream's contract -- it does not return (the callers fall off the end
 * of a switch after it). */
extern void pclog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void pclog_ex(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));
extern void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
extern void fatal_ex(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0), noreturn));

#endif /* VOODOO_SHIM_86BOX_H */

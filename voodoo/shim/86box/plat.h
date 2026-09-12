/*
 * plat.h -- 2ksbox's stand-in for 86Box's <86box/plat.h>: the recompiler's
 * executable memory (plat_mmap with MAP_JIT on macOS, VirtualAlloc on
 * Windows), the host tick counter the wait statistics use, and a sleep.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_PLAT_H
#define VOODOO_SHIM_PLAT_H

#include <stdint.h>
#include <stddef.h>

#define UNUSED(arg) __attribute__((unused)) arg

#if __has_attribute(fallthrough)
#    define fallthrough __attribute__((fallthrough))
#else
#    define fallthrough do { } while (0) /* fallthrough */
#endif

extern uint64_t timer_freq;

extern void    *plat_mmap(size_t size, uint8_t executable, uint8_t *large);
extern void     plat_munmap(void *ptr, size_t size);
extern uint64_t plat_timer_read(void);
extern void     plat_delay_ms(uint32_t count);

#endif /* VOODOO_SHIM_PLAT_H */

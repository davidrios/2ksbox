/*
 * kcrt.c — the two CRT symbols GCC emits calls to even in freestanding
 * code (struct copies, zero-initialisation). Kernel modules link no CRT
 * and win32k.sys exports neither, so both drivers carry these, and so does
 * the 9x ring-3 HAL (d3dpthal.dll), which links no CRT either.
 *
 * They are string instructions, not C loops: every Direct3D batch goes
 * through memcpy into the command window (the runtime's command stream,
 * the vertices), and under TCG a C byte loop is one trip through a
 * three-instruction translated block per byte, where `rep movsd` is one
 * helper call per copy (patch 17). Measured honestly (doc 19 §31): the
 * loop looked like ~30 % of 3DMark 99's race in a profile, but that was
 * patch 35's walks landing on its stores, and the score is the same
 * either way. Written in asm so GCC's loop-idiom pass cannot turn a loop
 * back into a call to itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    const void *s = src;
    size_t dw = n >> 2, tail = n & 3;

    __asm__ volatile("cld\n\trep movsl\n\tmovl %3, %%ecx\n\trep movsb"
                     : "+D"(d), "+S"(s), "+c"(dw)
                     : "r"(tail)
                     : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *d = dst;
    size_t dw = n >> 2, tail = n & 3;
    unsigned int v = (unsigned char)c * 0x01010101u;

    __asm__ volatile("cld\n\trep stosl\n\tmovl %3, %%ecx\n\trep stosb"
                     : "+D"(d), "+c"(dw)
                     : "a"(v), "r"(tail)
                     : "memory");
    return dst;
}

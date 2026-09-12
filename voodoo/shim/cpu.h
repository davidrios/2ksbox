/*
 * cpu.h -- 2ksbox's stand-in for 86Box's "cpu.h" as included by the vendored
 * Voodoo sources. They use it for one thing: `cycles -= voodoo->read_time`,
 * 86Box's way of charging the guest CPU for a PCI access. QEMU has no such
 * counter (TCG charges nothing for MMIO beyond the trap itself), so the
 * writes land in a dummy.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_CPU_H
#define VOODOO_SHIM_CPU_H

extern int voodoo_shim_cycles;
#define cycles voodoo_shim_cycles

#endif /* VOODOO_SHIM_CPU_H */

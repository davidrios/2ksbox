/*
 * pci.h -- 2ksbox's stand-in for 86Box's <86box/pci.h>. pci_add_card records
 * the card's configuration-space handlers; the QEMU PCI device forwards the
 * bytes it does not own itself (0x40-0x43, the initEnable register) to them
 * and lets the PCI core do the BAR. The two timing globals feed
 * `cycles -= ...` accounting that QEMU does not do (see cpu.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_PCI_H
#define VOODOO_SHIM_PCI_H

#include <stdint.h>

enum {
    PCI_ADD_NORMAL = 0x10,
};

extern int pci_burst_time;
extern int pci_nonburst_time;

extern void pci_add_card(uint8_t add_type,
                         uint8_t (*read)(int func, int addr, int len, void *priv),
                         void (*write)(int func, int addr, int len, uint8_t val, void *priv),
                         void *priv, uint8_t *slot);

#endif /* VOODOO_SHIM_PCI_H */

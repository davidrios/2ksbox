/*
 * mem.h -- 2ksbox's stand-in for 86Box's <86box/mem.h>. 86Box maps a device
 * into the guest's physical address space by registering handlers in a
 * mem_mapping_t; here the mapping only *records* the handlers, and the QEMU
 * device (voodoo2.c) calls them from its own MemoryRegion, which the PCI
 * core maps and unmaps with the BAR. mem_mapping_set_addr / _disable are
 * therefore bookkeeping: what the guest wrote to the BAR decides.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_MEM_H
#define VOODOO_SHIM_MEM_H

#include <stdint.h>

#define MEM_MAPPING_EXTERNAL 1

typedef struct _mem_mapping_ {
    int      enable;
    uint32_t base;
    uint32_t size;

    uint8_t (*read_b)(uint32_t addr, void *priv);
    uint16_t (*read_w)(uint32_t addr, void *priv);
    uint32_t (*read_l)(uint32_t addr, void *priv);
    void (*write_b)(uint32_t addr, uint8_t val, void *priv);
    void (*write_w)(uint32_t addr, uint16_t val, void *priv);
    void (*write_l)(uint32_t addr, uint32_t val, void *priv);

    uint8_t *exec;
    uint32_t flags;
    void    *priv;
} mem_mapping_t;

extern void mem_mapping_add(mem_mapping_t *map, uint32_t base, uint32_t size,
                            uint8_t (*read_b)(uint32_t addr, void *priv),
                            uint16_t (*read_w)(uint32_t addr, void *priv),
                            uint32_t (*read_l)(uint32_t addr, void *priv),
                            void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                            void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                            void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                            uint8_t *exec, uint32_t flags, void *priv);
extern void mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size);
extern void mem_mapping_disable(mem_mapping_t *map);
extern void mem_mapping_enable(mem_mapping_t *map);

/* a dword of guest RAM (the command FIFO in AGP mode, Banshee only) */
extern uint32_t mem_readl_phys(uint32_t addr);

#endif /* VOODOO_SHIM_MEM_H */

/*
 * device.h -- 2ksbox's stand-in for 86Box's <86box/device.h>. The vendored
 * vid_voodoo.c defines its configuration table with these types (designated
 * initialisers, so the field names and the constants must match upstream)
 * and reads it back through device_get_config_*; here the answers come from
 * the QEMU device's properties (voodoo_shim_set_config in voodoo_shim.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VOODOO_SHIM_DEVICE_H
#define VOODOO_SHIM_DEVICE_H

#include <stdint.h>

#define CONFIG_END         -1
#define CONFIG_SHIFT       4
#define CONFIG_TYPE_INT    (0 << CONFIG_SHIFT)
#define CONFIG_TYPE_STRING (1 << CONFIG_SHIFT)
#define CONFIG_INT         (0 | CONFIG_TYPE_INT)
#define CONFIG_BINARY      (1 | CONFIG_TYPE_INT)
#define CONFIG_SELECTION   (2 | CONFIG_TYPE_INT)
#define CONFIG_STRING      (0 | CONFIG_TYPE_STRING)
#define CONFIG_BIOS        (3 | CONFIG_TYPE_STRING)

enum {
    DEVICE_PCI = 0x10000,
};

#define BIOS_NORMAL           0
#define BIOS_LIMIT_MIN_MEMORY 0x0100000000000000
#define BIOS_LIMIT_MAX_MEMORY 0x0200000000000000

typedef struct device_config_selection_t {
    const char *description;
    int         value;
} device_config_selection_t;

typedef struct device_config_spinner_t {
    int16_t min;
    int16_t max;
    int16_t step;
} device_config_spinner_t;

typedef struct device_config_bios_t {
    const char *name;
    const char *internal_name;
    uint8_t     bios_type;
    int8_t      files_no;
    uint32_t    local;
    uint32_t    size;
    uint64_t    flags;
    void       *dev[2];
    const char *files[9];
} device_config_bios_t;

typedef struct _device_config_ {
    const char                      *name;
    const char                      *description;
    int                              type;
    const char                      *default_string;
    int                              default_int;
    const char                      *file_filter;
    const device_config_spinner_t    spinner;
    const device_config_selection_t  selection[64];
    const device_config_bios_t       bios[32];
} device_config_t;

typedef struct _device_ {
    const char *name;
    const char *internal_name;
    uint32_t    flags;
    uintptr_t   local;

    void *(*init)(const struct _device_ *);
    void (*close)(void *priv);
    void (*reset)(void *priv);
    int  (*available)(void);
    void (*speed_changed)(void *priv);
    void (*force_redraw)(void *priv);

    const char *alias;
    const char *machine;
    const device_config_t *config;
} device_t;

extern int         device_get_config_int(const char *name);
extern const char *device_get_config_bios(const char *name);
extern uint32_t    device_get_bios_local(const device_t *dev, const char *internal_name);
extern uint64_t    device_get_bios_flags(const device_t *dev, const char *internal_name);

#endif /* VOODOO_SHIM_DEVICE_H */

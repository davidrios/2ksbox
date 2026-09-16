/*
 * census.c -- a TCG plugin that counts what the hardware-MMU design of
 * docs/tracks/m9-tcg-aarch64.md would pay for, on a real workload:
 * guest instructions, loads and stores, the distinct 4 KiB virtual pages
 * touched per second and per window of 65,536 accesses (the nested
 * stage-1+2 TLB holds 3072 of them: a window beyond that misses), and the
 * pages touched and written for the first time (each a mirror fill of
 * ~110-160 ns, each first write a dirty upgrade of ~495 ns in the probe).
 * One line per second of wall time on the plugin log (-d plugin -D file).
 *
 *   -plugin build/hwmmu/libcensus.dylib -d plugin -D census.log
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define PAGES (1u << 20)                 /* 4 GiB of 4 KiB pages */
#define WORDS (PAGES / 64)
#define WINDOW 65536

static struct qemu_plugin_scoreboard *insns_sb;
static qemu_plugin_u64 insns;
static uint64_t loads, stores, acc;   /* acc: every access, the reuse clock */
static uint64_t sec_bits[WORDS], win_bits[WORDS], ever_bits[WORDS], written_bits[WORDS];
static uint64_t sec_pages, win_pages, win_n, win_sum, win_max, win_over3072;
static uint64_t ever_pages, written_pages;
/*
 * The reuse distance of each access's page, in accesses since that page
 * was last touched, in powers-of-two buckets: bucket b counts distances in
 * [2^b, 2^(b+1)) for b = 0..29, bucket 30 the first touch. The probe's
 * kernels touch R pages uniformly at random, so a kernel row corresponds
 * to a mean distance of R accesses: the 64 KiB row to ~16, 4 MiB to ~1K,
 * 8 MiB to ~2K, 16 MiB to ~4K, 32 MiB to ~8K -- with one word per page
 * per access; real code touches many words per page between reuses, so
 * a distance here counts accesses, and the mixture in project.py maps
 * it through the window's own pages-per-access density.
 */
static uint32_t *last_touch;              /* per page: the access count at its last touch */
static uint64_t reuse[32], last_reuse[32];
static uint64_t last_loads, last_stores, last_insns, last_ever, last_written;
static int64_t t0, next_report;
static uint64_t win_acc;

static inline bool set_bit(uint64_t *bits, uint32_t page)
{
    uint64_t w = bits[page >> 6], m = 1ull << (page & 63);
    if (w & m) {
        return false;
    }
    bits[page >> 6] = w | m;
    return true;
}

static void report(int64_t now, bool final)
{
    uint64_t ins = qemu_plugin_u64_sum(insns);
    double dt = (now - t0) / 1e6;
    g_autoptr(GString) s = g_string_new(NULL);

    g_string_printf(s, "census t=%.1f insns=%" PRIu64 " ld=%" PRIu64 " st=%" PRIu64
                    " pages1s=%" PRIu64 " win64k avg=%" PRIu64 " max=%" PRIu64
                    " over3072=%" PRIu64 "/%" PRIu64 " newpages=%" PRIu64
                    " newwritten=%" PRIu64 " (ever %" PRIu64 " / %" PRIu64 ")%s\n",
                    dt, ins - last_insns, loads - last_loads, stores - last_stores,
                    sec_pages, win_n ? win_sum / win_n : 0, win_max, win_over3072, win_n,
                    ever_pages - last_ever, written_pages - last_written,
                    ever_pages, written_pages, final ? " final" : "");
    qemu_plugin_outs(s->str);
    g_string_printf(s, "reuse t=%.1f", dt);
    for (int b = 0; b < 31; b++) {
        g_string_append_printf(s, " %" PRIu64, reuse[b] - last_reuse[b]);
        last_reuse[b] = reuse[b];
    }
    g_string_append(s, "\n");
    qemu_plugin_outs(s->str);
    last_insns = ins; last_loads = loads; last_stores = stores;
    last_ever = ever_pages; last_written = written_pages;
    memset(sec_bits, 0, sizeof(sec_bits));
    sec_pages = 0; win_n = 0; win_sum = 0; win_max = 0; win_over3072 = 0;
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    uint32_t page = (uint32_t)(vaddr >> 12);
    bool store = qemu_plugin_mem_is_store(info);

    if (store) {
        stores++;
        if (set_bit(written_bits, page)) {
            written_pages++;
        }
    } else {
        loads++;
    }
    if (set_bit(ever_bits, page)) {
        ever_pages++;
        reuse[30]++;
    } else {
        uint32_t d = (uint32_t)acc - last_touch[page];
        reuse[d ? 63 - __builtin_clzll(d) : 0]++;
    }
    last_touch[page] = (uint32_t)acc;
    acc++;
    if (set_bit(sec_bits, page)) {
        sec_pages++;
    }
    if (set_bit(win_bits, page)) {
        win_pages++;
    }
    if (++win_acc == WINDOW) {
        win_acc = 0;
        win_n++; win_sum += win_pages;
        if (win_pages > win_max) {
            win_max = win_pages;
        }
        if (win_pages > 3072) {
            win_over3072++;
        }
        memset(win_bits, 0, sizeof(win_bits));
        win_pages = 0;
        int64_t now = g_get_monotonic_time();
        if (now >= next_report) {
            report(now, false);
            next_report = now + 1000000;
        }
    }
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, insns, n);
    for (size_t i = 0; i < n; i++) {
        qemu_plugin_register_vcpu_mem_cb(qemu_plugin_tb_get_insn(tb, i), vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    report(g_get_monotonic_time(), true);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    last_touch = g_malloc0(PAGES * sizeof(uint32_t));
    insns_sb = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(insns_sb);
    t0 = g_get_monotonic_time();
    next_report = t0 + 1000000;
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}

import re, sys, pathlib
Q = pathlib.Path('qemu')
def edit(path, pairs):
    p = Q / path; s = p.read_text()
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"{path}: pattern not unique/found ({s.count(old)}): {old[:70]!r}")
        s = s.replace(old, new)
    p.write_text(s)

# ---------------- spike A: the jump table ----------------
edit('accel/tcg/tb-jmp-cache.h', [(
"""    uint32_t gen;
    struct {""",
"""    uint32_t gen;
    /*
     * Spike A (docs/23 §2, Tiaozhuan's "full address mapping"): a table
     * indexed by the 32-bit pc itself -- 2^32 entries of 8 bytes reserved
     * and populated on demand -- instead of the hashed 65,536-entry array.
     * An entry is the TB pointer in its low 48 bits and the generation it
     * was filled in above them; NULL when the switch is off (jump-table).
     */
    uint64_t *table;
    struct {"""),
("""#endif /* ACCEL_TCG_TB_JMP_CACHE_H */""",
"""#define TB_JMP_TABLE_ENTRIES   (1ull << 32)
#define TB_JMP_TABLE_BYTES     (TB_JMP_TABLE_ENTRIES * sizeof(uint64_t))
#define TB_JMP_TABLE_PTR_MASK  ((1ull << 48) - 1)
#define TB_JMP_TABLE_GEN_BITS  16
#define TB_JMP_TABLE_GEN_MAX   ((1u << TB_JMP_TABLE_GEN_BITS) - 1)

static inline TranslationBlock *tb_jmp_table_tb(uint64_t e)
{
    return (TranslationBlock *)(uintptr_t)(e & TB_JMP_TABLE_PTR_MASK);
}

static inline uint32_t tb_jmp_table_gen(uint64_t e)
{
    return (uint32_t)(e >> 48);
}

static inline uint64_t tb_jmp_table_entry(const TranslationBlock *tb, uint32_t gen)
{
    return (uint64_t)(uintptr_t)tb | ((uint64_t)gen << 48);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */""")])

edit('accel/tcg/tb-hash.h', [(
"""#if TARGET_LONG_BITS <= 32
#define TB_JMP_CACHE_GEN 1""",
"""#if TARGET_LONG_BITS <= 32
#define TB_JMP_CACHE_GEN 1
/* the pc-indexed jump table (spike A): a 32-bit guest on a 64-bit POSIX host */
#if defined(CONFIG_POSIX) && HOST_LONG_BITS == 64
#define TB_JMP_TABLE 1
#else
#define TB_JMP_TABLE 0
#endif""")])

edit('accel/tcg/internal-common.h', [(
"""extern bool tcg_jump_cache_keep;""",
"""extern bool tcg_jump_cache_keep;
/* spike A: the jump cache as a table indexed by pc (docs/23 §2) */
extern bool tcg_jump_table;
/* spike B: the exit-request check at back edges and indirect branches only (docs/23 §7) */
extern bool tcg_irq_check_backedge;""")])

edit('accel/tcg/tcg-all.c', [(
"""bool tcg_jump_cache_keep = true;""",
"""bool tcg_jump_cache_keep = true;
bool tcg_jump_table = true;
bool tcg_irq_check_backedge = true;"""),
("""static bool tcg_get_jump_cache_keep(Object *obj, Error **errp)
{
    return tcg_jump_cache_keep;
}""",
"""static bool tcg_get_jump_cache_keep(Object *obj, Error **errp)
{
    return tcg_jump_cache_keep;
}

static bool tcg_get_jump_table(Object *obj, Error **errp)
{
    return tcg_jump_table;
}

static void tcg_set_jump_table(Object *obj, bool value, Error **errp)
{
    tcg_jump_table = value;
}

static bool tcg_get_irq_check_backedge(Object *obj, Error **errp)
{
    return tcg_irq_check_backedge;
}

static void tcg_set_irq_check_backedge(Object *obj, bool value, Error **errp)
{
    tcg_irq_check_backedge = value;
}"""),
("""    object_class_property_set_description(oc, "jump-cache-keep",
        "Keep the jump cache across TLB flushes and re-validate its entries by physical page");
""",
"""    object_class_property_set_description(oc, "jump-cache-keep",
        "Keep the jump cache across TLB flushes and re-validate its entries by physical page");

    object_class_property_add_bool(oc, "jump-table",
        tcg_get_jump_table, tcg_set_jump_table);
    object_class_property_set_description(oc, "jump-table",
        "Index the jump cache by the guest pc itself (a 32 GiB on-demand table) instead of a hash");

    object_class_property_add_bool(oc, "irq-check-backedge",
        tcg_get_irq_check_backedge, tcg_set_irq_check_backedge);
    object_class_property_set_description(oc, "irq-check-backedge",
        "Check for a pending exit request at backward jumps and indirect branches only, not at every block");
""")])

edit('accel/tcg/cpu-exec.c', [(
"""    hash = tb_jmp_cache_hash_func(pc);
    jc = cpu->tb_jmp_cache;
    key = tb_jmp_cache_key(pc, cpu->tb_jmp_cache->gen);

    tb = qatomic_read(&jc->array[hash].tb);""",
"""    hash = tb_jmp_cache_hash_func(pc);
    jc = cpu->tb_jmp_cache;
    key = tb_jmp_cache_key(pc, cpu->tb_jmp_cache->gen);

#if TB_JMP_TABLE
    if (jc->table) {
        /*
         * Spike A: the entry of this pc, no hash and no conflicts.  A
         * stale generation is re-validated by physical page exactly as
         * patch 42 does for the hashed cache.
         */
        uint64_t *slot;
        uint64_t e;

        tcg_debug_assert((pc >> 32) == 0);
        slot = &jc->table[(uint32_t)pc];
        e = qatomic_read(slot);
        tb = tb_jmp_table_tb(e);
        if (likely(tb &&
                   tb_jmp_table_gen(e) == jc->gen &&
                   tb->cs_base == cs_base &&
                   tb->flags == flags &&
                   tb_cflags(tb) == cflags)) {
            goto hit;
        }
        if (tb &&
            tb->cs_base == cs_base &&
            tb->flags == flags &&
            tb_cflags(tb) == cflags &&
            tb_jmp_cache_revalidate(cpu, tb, pc)) {
            qatomic_set(slot, tb_jmp_table_entry(tb, jc->gen));
            goto hit;
        }
        tb = tb_htable_lookup(cpu, pc, cs_base, flags, cflags);
        if (tb == NULL) {
            return NULL;
        }
        tcg_debug_assert(((uintptr_t)tb >> 48) == 0);
        qatomic_set(slot, tb_jmp_table_entry(tb, jc->gen));
        goto hit;
    }
#endif

    tb = qatomic_read(&jc->array[hash].tb);"""),
("""                h = tb_jmp_cache_hash_func(pc);
                jc = cpu->tb_jmp_cache;
                jc->array[h].pc = tb_jmp_cache_key(pc, cpu->tb_jmp_cache->gen);
                qatomic_set(&jc->array[h].tb, tb);""",
"""                h = tb_jmp_cache_hash_func(pc);
                jc = cpu->tb_jmp_cache;
#if TB_JMP_TABLE
                if (jc->table) {
                    qatomic_set(&jc->table[(uint32_t)pc],
                                tb_jmp_table_entry(tb, jc->gen));
                } else
#endif
                {
                    jc->array[h].pc = tb_jmp_cache_key(pc, cpu->tb_jmp_cache->gen);
                    qatomic_set(&jc->array[h].tb, tb);
                }"""),
("""    cpu->tb_jmp_cache = g_new0(CPUJumpCache, 1);
    tlb_init(cpu);""",
"""    cpu->tb_jmp_cache = g_new0(CPUJumpCache, 1);
#if TB_JMP_TABLE
    if (tcg_jump_table) {
        /* spike A: 32 GiB of address space, materialised a page at a time */
        void *p = mmap(NULL, TB_JMP_TABLE_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

        if (p == MAP_FAILED) {
            warn_report("jump-table: cannot reserve the pc-indexed table (%s); "
                        "using the hashed jump cache", strerror(errno));
            tcg_jump_table = false;
        } else {
            cpu->tb_jmp_cache->table = p;
        }
    }
#endif
    tlb_init(cpu);"""),
("""    tlb_destroy(cpu);
    g_free_rcu(cpu->tb_jmp_cache, rcu);""",
"""    tlb_destroy(cpu);
#if TB_JMP_TABLE
    if (cpu->tb_jmp_cache->table) {
        munmap(cpu->tb_jmp_cache->table, TB_JMP_TABLE_BYTES);
        cpu->tb_jmp_cache->table = NULL;
    }
#endif
    g_free_rcu(cpu->tb_jmp_cache, rcu);""")])

edit('accel/tcg/tb-maint.c', [(
"""        uint32_t h = tb_jmp_cache_hash_func(tb->pc);

        CPU_FOREACH(cpu) {
            CPUJumpCache *jc = cpu->tb_jmp_cache;

            if (qatomic_read(&jc->array[h].tb) == tb) {""",
"""        uint32_t h = tb_jmp_cache_hash_func(tb->pc);

        CPU_FOREACH(cpu) {
            CPUJumpCache *jc = cpu->tb_jmp_cache;

#if TB_JMP_TABLE
            if (jc->table) {
                uint64_t *slot = &jc->table[(uint32_t)tb->pc];

                if (tb_jmp_table_tb(qatomic_read(slot)) == tb) {
                    qatomic_set(slot, 0);
                }
                continue;
            }
#endif
            if (qatomic_read(&jc->array[h].tb) == tb) {""")])

edit('accel/tcg/translate-all.c', [(
"""    for (int i = 0; i < TB_JMP_CACHE_SIZE; i++) {
        qatomic_set(&jc->array[i].tb, NULL);
    }
}""",
"""#if TB_JMP_TABLE
    if (jc->table) {
        /*
         * Spike A: zero the 32 GiB table by replacing its mapping -- the
         * pages it touched go back to the kernel and every entry reads as
         * empty again; nothing is written.
         */
        void *p = mmap(jc->table, TB_JMP_TABLE_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                       -1, 0);

        g_assert(p == jc->table);
        return;
    }
#endif
    for (int i = 0; i < TB_JMP_CACHE_SIZE; i++) {
        qatomic_set(&jc->array[i].tb, NULL);
    }
}"""),
("""void tcg_flush_jmp_cache_lazy(CPUState *cpu)
{
    if (TB_JMP_CACHE_GEN && tcg_jump_cache_keep) {""",
"""void tcg_flush_jmp_cache_lazy(CPUState *cpu)
{
#if TB_JMP_TABLE
    if (cpu->tb_jmp_cache && cpu->tb_jmp_cache->table) {
        /* spike A: 16 generation bits; the wrap is a real clear */
        uint32_t gen = cpu->tb_jmp_cache->gen + 1;

        if (likely(gen <= TB_JMP_TABLE_GEN_MAX)) {
            cpu->tb_jmp_cache->gen = gen;
            return;
        }
        cpu->tb_jmp_cache->gen = 1;
        tcg_flush_jmp_cache(cpu);
        return;
    }
#endif
    if (TB_JMP_CACHE_GEN && tcg_jump_cache_keep) {""")])

edit('accel/tcg/cputlb.c', [(
"""    if (unlikely(!jc)) {
        return;
    }

    i0 = tb_jmp_cache_hash_page(page_addr);""",
"""    if (unlikely(!jc)) {
        return;
    }

#if TB_JMP_TABLE
    if (jc->table) {
        /*
         * Spike A: the page's 4096 entries are 32 KiB of the table, host
         * page aligned; drop the mapping rather than write it, so a page
         * nobody ever branched into is not materialised by its invlpg.
         */
        uint64_t *slot = &jc->table[(uint32_t)(page_addr & TARGET_PAGE_MASK)];
        void *p = mmap(slot, TARGET_PAGE_SIZE * sizeof(uint64_t),
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                       -1, 0);

        g_assert(p == slot);
        return;
    }
#endif
    i0 = tb_jmp_cache_hash_page(page_addr);""")])

edit('accel/tcg/translator.c', [(
"""    tcg_gen_shli_i64(hash, hash, 4);
    tcg_gen_add_i64(entry, entry, hash);

    /* tb = entry->tb; tbs = tb ? tb : the current TB (a safe base) */
    tcg_gen_ld_i64(tb, (TCGv_ptr)entry, offsetof(CPUJumpCache, array[0].tb));
    tcg_gen_movcond_i64(TCG_COND_EQ, tbs, tb, tcg_constant_i64(0),
                        tcg_constant_i64((uintptr_t)tcg_ctx->gen_tb), tb);

    /* m: zero iff the entry is a hit and the chain may be taken */
    tcg_gen_setcondi_i64(TCG_COND_EQ, m, tb, 0);
    tcg_gen_ld_i64(tmp, (TCGv_ptr)entry, offsetof(CPUJumpCache, array[0].pc));
    tcg_gen_xor_i64(tmp, tmp, pc);
    if (TB_JMP_CACHE_GEN) {
        /*
         * The entry's key carries the generation it was filled in (patch
         * 42): a TLB flush bumps the cache's and every entry misses here,
         * as it did when the flush cleared the cache; the C lookup then
         * re-validates the entry instead of paying the hash table.
         */
        tcg_gen_shli_i64(gen, gen, 32);
        tcg_gen_xor_i64(tmp, tmp, gen);
    }
    tcg_gen_or_i64(m, m, tmp);""",
"""#if TB_JMP_TABLE
    if (tcg_jump_table) {
        /*
         * Spike A: entry = jc->table[pc] -- the TB pointer in the low 48
         * bits, the generation above.  No hash: the address chain to the
         * entry load is shift, add, load.
         */
        tcg_gen_ld_i64(tmp, (TCGv_ptr)entry, offsetof(CPUJumpCache, table));
        tcg_gen_shli_i64(hash, pc, 3);
        tcg_gen_add_i64(tmp, tmp, hash);
        tcg_gen_ld_i64(entry, (TCGv_ptr)tmp, 0);
        tcg_gen_andi_i64(tb, entry, TB_JMP_TABLE_PTR_MASK);
        tcg_gen_movcond_i64(TCG_COND_EQ, tbs, tb, tcg_constant_i64(0),
                            tcg_constant_i64((uintptr_t)tcg_ctx->gen_tb), tb);
        tcg_gen_setcondi_i64(TCG_COND_EQ, m, tb, 0);
        tcg_gen_shri_i64(tmp, entry, 48);
        tcg_gen_xor_i64(tmp, tmp, gen);
        tcg_gen_or_i64(m, m, tmp);
    } else
#endif
    {
    tcg_gen_shli_i64(hash, hash, 4);
    tcg_gen_add_i64(entry, entry, hash);

    /* tb = entry->tb; tbs = tb ? tb : the current TB (a safe base) */
    tcg_gen_ld_i64(tb, (TCGv_ptr)entry, offsetof(CPUJumpCache, array[0].tb));
    tcg_gen_movcond_i64(TCG_COND_EQ, tbs, tb, tcg_constant_i64(0),
                        tcg_constant_i64((uintptr_t)tcg_ctx->gen_tb), tb);

    /* m: zero iff the entry is a hit and the chain may be taken */
    tcg_gen_setcondi_i64(TCG_COND_EQ, m, tb, 0);
    tcg_gen_ld_i64(tmp, (TCGv_ptr)entry, offsetof(CPUJumpCache, array[0].pc));
    tcg_gen_xor_i64(tmp, tmp, pc);
    if (TB_JMP_CACHE_GEN) {
        /*
         * The entry's key carries the generation it was filled in (patch
         * 42): a TLB flush bumps the cache's and every entry misses here,
         * as it did when the flush cleared the cache; the C lookup then
         * re-validates the entry instead of paying the hash table.
         */
        tcg_gen_shli_i64(gen, gen, 32);
        tcg_gen_xor_i64(tmp, tmp, gen);
    }
    tcg_gen_or_i64(m, m, tmp);
    }"""),
# ---------------- spike B: the exit-request check at back edges ----------------
("""static TCGOp *gen_tb_start(DisasContextBase *db, uint32_t cflags)
{
    TCGv_i32 count = NULL;
    TCGOp *icount_start_insn = NULL;

    if ((cflags & CF_USE_ICOUNT) || !(cflags & CF_NOIRQ)) {""",
"""/*
 * Spike B (docs/23 §7, Niu et al. SYSTOR 2022): a TB that is not under
 * icount and not in an interrupt shadow skips the exit-request check at
 * its start; the target emits it before every backward direct jump and
 * every indirect branch instead (translator_gen_exitreq_check).  Every
 * cycle in the block graph contains one of those, so an exit request is
 * still seen within one pass over straight-line code; forward chains
 * carry no check at all.
 */
void translator_gen_exitreq_check(DisasContextBase *db)
{
    TCGv_i32 count;

    if (!db->exitreq_deferred) {
        return;
    }
    if (!tcg_ctx->exitreq_label) {
        tcg_ctx->exitreq_label = gen_new_label();
    }
    count = tcg_temp_ebb_new_i32();
    tcg_gen_ld_i32(count, tcg_env,
                   offsetof(ArchCPU, parent_obj.neg.icount_decr.u32)
                   - offsetof(ArchCPU, env));
    tcg_gen_brcondi_i32(TCG_COND_LT, count, 0, tcg_ctx->exitreq_label);
}

static TCGOp *gen_tb_start(DisasContextBase *db, uint32_t cflags)
{
    TCGv_i32 count = NULL;
    TCGOp *icount_start_insn = NULL;

    db->exitreq_deferred = false;
    if (!(cflags & (CF_NOIRQ | CF_USE_ICOUNT)) && tcg_irq_check_backedge) {
        db->exitreq_deferred = true;
        cflags |= CF_NOIRQ;     /* below: no check at the start */
    }

    if ((cflags & CF_USE_ICOUNT) || !(cflags & CF_NOIRQ)) {""")])

edit('include/exec/translator.h', [(
"""    bool plugin_enabled;
    bool fake_insn;""",
"""    bool plugin_enabled;
    bool fake_insn;
    /* spike B: the exit-request check is the target's, at back edges */
    bool exitreq_deferred;"""),
("""void translator_lookup_and_goto_ptr(TCGv_i64 pc, TCGv_i64 cs_base,
                                    TCGv_i32 flags);""",
"""void translator_lookup_and_goto_ptr(TCGv_i64 pc, TCGv_i64 cs_base,
                                    TCGv_i32 flags);

/*
 * translator_gen_exitreq_check() - the exit-request check, where the TB
 * start no longer has it (spike B): the target calls it before a backward
 * direct jump (the pc already set to the target) and before an indirect
 * branch. A no-op when the TB checks at its start.
 */
void translator_gen_exitreq_check(DisasContextBase *db);""")])

edit('target/i386/tcg/translate.c', [(
"""    if (use_goto_tb && translator_use_goto_tb(&s->base, new_pc)) {
        /* jump to same page: we can use a direct jump */
        tcg_gen_goto_tb(tb_num);""",
"""    if (use_goto_tb && translator_use_goto_tb(&s->base, new_pc)) {
        /* jump to same page: we can use a direct jump */
        if (new_pc < s->pc && s->base.exitreq_deferred) {
            /*
             * Spike B: a backward jump carries the exit-request check the
             * TB start no longer has.  eip is the target already (CF_PCREL
             * is always on for this target), so an exit here is the exit
             * the target's own start would have taken.
             */
            tcg_debug_assert(tb_cflags(s->base.tb) & CF_PCREL);
            translator_gen_exitreq_check(&s->base);
        }
        tcg_gen_goto_tb(tb_num);"""),
("""    } else if (mode == DISAS_JUMP &&
               /* give irqs a chance to happen */
               !inhibit_reset) {
        gen_lookup_and_goto_ptr(s);""",
"""    } else if (mode == DISAS_JUMP &&
               /* give irqs a chance to happen */
               !inhibit_reset) {
        /* spike B: an indirect branch carries the check too */
        translator_gen_exitreq_check(&s->base);
        gen_lookup_and_goto_ptr(s);""")])

# ---------------- spike C: the SSE check ceiling (an experiment knob, inexact) ----------------
edit('target/i386/tcg/sse-fast.c.inc', [(
"""#define W 32
#include "sse-fast-lane.c.inc\"""",
"""/*
 * Spike C (docs/23 §4): SSES_NOCHECK=1 in the environment drops every
 * result classification check, an *inexact* experiment that bounds what
 * any cheaper check could buy.  Never on by default.
 */
static int sses_nocheck = -1;
static bool sses_nocheck_on(void)
{
    if (sses_nocheck < 0) {
        const char *e = getenv("SSES_NOCHECK");
        sses_nocheck = e && *e && *e != '0';
    }
    return sses_nocheck;
}

#define W 32
#include "sse-fast-lane.c.inc\"""")])

edit('target/i386/tcg/sse-fast-lane.c.inc', [(
"""    TCGv_iW e = TMP();
    TCGv_iW okl = TMP();

    if (!normal_only) {""",
"""    TCGv_iW e = TMP();
    TCGv_iW okl = TMP();

    if (sses_nocheck_on()) {            /* spike C: the ceiling experiment */
        if (ok) {
            return ok;
        }
        OPW(movi)(okl, 1);
        return okl;
    }
    if (!normal_only) {""")])
print("applied")

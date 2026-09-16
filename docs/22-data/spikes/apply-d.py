import sys, pathlib
Q = pathlib.Path('qemu')
def edit(path, pairs):
    p = Q / path; s = p.read_text()
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"{path}: pattern not unique/found ({s.count(old)}): {old[:70]!r}")
        s = s.replace(old, new)
    p.write_text(s)

# ---- the ring, beside the jump cache ----
edit('accel/tcg/tb-jmp-cache.h', [(
"""    uint64_t *table;
    struct {""",
"""    uint64_t *table;
    /*
     * Spike D (docs/23 §2, MAMBO-X64's return-address prediction): a ring
     * of the guest return addresses the last calls pushed, each with the
     * calling TB and the host address of that TB's landing code -- a
     * `ret` whose popped address matches jumps there with a host `ret`,
     * which the hardware's return-address stack predicts, and the
     * calling TB's flags, cs_base and cflags are checked so the landing
     * (a goto_tb to the continuation) is the exact one.  Any entry is a
     * hint: a stale one misses and the ordinary probe follows.
     */
#define TB_RAS_BITS 6
#define TB_RAS_SIZE (1 << TB_RAS_BITS)
    uint32_t ras_top;
    struct {
        uint64_t pc;        /* the return address as a virtual pc */
        uint64_t tb;        /* the calling TB */
        uint64_t host;      /* its landing code */
        uint64_t pad;
    } ras[TB_RAS_SIZE];
    struct {""")])

edit('accel/tcg/translate-all.c', [(
"""    /* During early initialization, the cache may not yet be allocated. */
    if (unlikely(jc == NULL)) {
        return;
    }
""",
"""    /* During early initialization, the cache may not yet be allocated. */
    if (unlikely(jc == NULL)) {
        return;
    }
    /* spike D: the ring holds host addresses of this code buffer */
    jc->ras_top = 0;
    memset(jc->ras, 0, sizeof(jc->ras));
""")])

edit('accel/tcg/internal-common.h', [(
"""extern bool tcg_irq_check_backedge;""",
"""extern bool tcg_irq_check_backedge;
/* spike D: return-address prediction through a host bl / ret pair (docs/23 §2) */
extern bool tcg_ras;""")])

edit('accel/tcg/tcg-all.c', [(
"""bool tcg_irq_check_backedge = true;""",
"""bool tcg_irq_check_backedge = true;
bool tcg_ras = true;"""),
("""static bool tcg_get_irq_check_backedge(Object *obj, Error **errp)""",
"""static bool tcg_get_ras(Object *obj, Error **errp)
{
    return tcg_ras;
}

static void tcg_set_ras(Object *obj, bool value, Error **errp)
{
    tcg_ras = value;
}

static bool tcg_get_irq_check_backedge(Object *obj, Error **errp)"""),
("""    object_class_property_add_bool(oc, "irq-check-backedge",""",
"""    object_class_property_add_bool(oc, "ras",
        tcg_get_ras, tcg_set_ras);
    object_class_property_set_description(oc, "ras",
        "Predict a ret's target from the call that pushed it, through a host bl/ret pair");

    object_class_property_add_bool(oc, "irq-check-backedge",""")])

# ---- the two TCG ops ----
edit('include/tcg/tcg-opc.h', [(
"""DEF(goto_ptr, 0, 1, 0, TCG_OPF_BB_EXIT | TCG_OPF_BB_END)""",
"""DEF(goto_ptr, 0, 1, 0, TCG_OPF_BB_EXIT | TCG_OPF_BB_END)
/*
 * spike D: call_tb slot, idx, label -- a goto_tb reached through a host
 * `bl` whose return address is stored at slot+16; control comes back to
 * the label when a goto_ret takes that address.  goto_ret ptr is a
 * goto_ptr emitted as a host `ret`.
 */
DEF(call_tb, 0, 1, 2, TCG_OPF_BB_END | IMPL(TCG_TARGET_HAS_ras))
DEF(goto_ret, 0, 1, 0, TCG_OPF_BB_EXIT | TCG_OPF_BB_END | IMPL(TCG_TARGET_HAS_ras))""")])

edit('include/tcg/tcg.h', [(
"""#ifndef TCG_TARGET_HAS_vec_allsign""",
"""#ifndef TCG_TARGET_HAS_ras
#define TCG_TARGET_HAS_ras 0
#endif
#ifndef TCG_TARGET_HAS_vec_allsign""")])

edit('tcg/aarch64/tcg-target.h', [(
"""#define TCG_TARGET_HAS_vec_allsign      1""",
"""#define TCG_TARGET_HAS_vec_allsign      1
#define TCG_TARGET_HAS_ras              1""")])

edit('include/tcg/tcg-op-common.h', [(
"""void tcg_gen_goto_ptr(TCGv_ptr ptr);""",
"""void tcg_gen_goto_ptr(TCGv_ptr ptr);

/* spike D: see tcg-opc.h */
void tcg_gen_call_tb(TCGv_ptr slot, unsigned idx, TCGLabel *after);
void tcg_gen_goto_ret(TCGv_ptr ptr);""")])

edit('tcg/tcg-op.c', [(
"""void tcg_gen_goto_ptr(TCGv_ptr ptr)
{
    tcg_gen_op1i(INDEX_op_goto_ptr, tcgv_ptr_arg(ptr));
}""",
"""void tcg_gen_goto_ptr(TCGv_ptr ptr)
{
    tcg_gen_op1i(INDEX_op_goto_ptr, tcgv_ptr_arg(ptr));
}

void tcg_gen_call_tb(TCGv_ptr slot, unsigned idx, TCGLabel *after)
{
    tcg_debug_assert(!(tcg_ctx->gen_tb->cflags & CF_NO_GOTO_TB));
    tcg_debug_assert(idx <= TB_EXIT_IDXMAX);
#ifdef CONFIG_DEBUG_TCG
    tcg_debug_assert((tcg_ctx->goto_tb_issue_mask & (1 << idx)) == 0);
    tcg_ctx->goto_tb_issue_mask |= 1 << idx;
#endif
    plugin_gen_disable_mem_helpers();
    add_as_label_use(after, tcg_gen_op3(INDEX_op_call_tb, tcgv_ptr_arg(slot),
                                        idx, label_arg(after)));
}

void tcg_gen_goto_ret(TCGv_ptr ptr)
{
    tcg_gen_op1i(INDEX_op_goto_ret, tcgv_ptr_arg(ptr));
}""")])

edit('tcg/tcg.c', [(
"""    case INDEX_op_qemu_st8_a32_i32:
    case INDEX_op_qemu_st8_a64_i32:
        return TCG_TARGET_HAS_qemu_st8_i32;""",
"""    case INDEX_op_qemu_st8_a32_i32:
    case INDEX_op_qemu_st8_a64_i32:
        return TCG_TARGET_HAS_qemu_st8_i32;

    case INDEX_op_call_tb:
    case INDEX_op_goto_ret:
        return TCG_TARGET_HAS_ras;"""),
("""        case INDEX_op_br:
        case INDEX_op_exit_tb:
        case INDEX_op_goto_ptr:
            /* Unconditional branches; everything following is dead.  */
            dead = true;
            break;""",
"""        case INDEX_op_br:
        case INDEX_op_exit_tb:
        case INDEX_op_goto_ptr:
        case INDEX_op_goto_ret:
            /* Unconditional branches; everything following is dead.  */
            dead = true;
            break;""")])

edit('tcg/aarch64/tcg-target.c.inc', [(
"""void tb_target_set_jmp_target(const TranslationBlock *tb, int n,""",
"""/*
 * Spike D: a goto_tb reached through `bl`, so the hardware return-address
 * stack learns the landing.  Layout:
 *     bl   stub
 *     b    after            <- the landing: a goto_ret lands here
 * stub:
 *     str  x30, [slot, #16] <- the ring entry learns the landing
 *     b    <target>         <- patched like goto_tb's (or ldr x16 + br x16)
 *     br   x16
 *     (fallthrough: the exit_tb that follows the op)
 */
static void tcg_out_call_tb(TCGContext *s, TCGReg slot, int which,
                            TCGLabel *after)
{
    intptr_t i_off;

    tcg_out_insn(s, 3206, BL, 2);
    tcg_out_goto_label(s, after);
    tcg_out_st(s, TCG_TYPE_I64, TCG_REG_X30, slot, 16);
    i_off = tcg_pcrel_diff(s, (void *)get_jmp_target_addr(s, which));
    tcg_debug_assert(i_off == sextract64(i_off, 0, 21));
    set_jmp_insn_offset(s, which);
    tcg_out32(s, I3206_B);
    tcg_out_insn(s, 3207, BR, TCG_REG_TMP0);
    set_jmp_reset_offset(s, which);
    tcg_out_bti(s, BTI_J);
}

void tb_target_set_jmp_target(const TranslationBlock *tb, int n,"""),
("""    switch (opc) {
    case INDEX_op_goto_ptr:
        tcg_out_insn(s, 3207, BR, a0);
        break;
""",
"""    switch (opc) {
    case INDEX_op_goto_ptr:
        tcg_out_insn(s, 3207, BR, a0);
        break;
    case INDEX_op_goto_ret:
        tcg_out_insn(s, 3207, RET, a0);
        break;
    case INDEX_op_call_tb:
        tcg_out_call_tb(s, a0, a1, arg_label(a2));
        break;
"""),
("""    switch (op) {
    case INDEX_op_goto_ptr:
        return C_O0_I1(r);

    case INDEX_op_add_f64:""",
"""    switch (op) {
    case INDEX_op_goto_ptr:
    case INDEX_op_goto_ret:
    case INDEX_op_call_tb:
        return C_O0_I1(r);

    case INDEX_op_add_f64:""")])

# ---- the generic halves: the push at a call, the pop at a ret ----
edit('include/exec/translator.h', [(
"""void translator_gen_exitreq_check(DisasContextBase *db);""",
"""void translator_gen_exitreq_check(DisasContextBase *db);

/*
 * Spike D, the return-address ring (docs/23 §2).  translator_ras_enabled()
 * says whether a call may use it.  translator_ras_call() pushes (@ret_pc,
 * the current TB) and emits call_tb for exit @idx; the target follows it
 * with the exit_tb of that index, sets the returned label, and emits the
 * landing: a goto_tb / exit_tb to the instruction after the call, which
 * a matching ret reaches with eip already set.  translator_ras_ret() is
 * the ret side, emitted before the ordinary probe: pops the ring, checks
 * the address and the calling TB's cs_base / flags / cflags, and takes
 * the landing through a host ret; on a miss it falls through.
 */
bool translator_ras_enabled(void);
TCGLabel *translator_ras_call(TCGv_i64 ret_pc, unsigned idx);
void translator_ras_ret(TCGv_i64 pc, TCGv_i64 cs_base, TCGv_i32 flags);""")])

edit('accel/tcg/translator.c', [(
"""void translator_lookup_and_goto_ptr(TCGv_i64 pc, TCGv_i64 cs_base,
                                    TCGv_i32 flags)
{""",
"""#define RAS_CPU_OFF(f) (offsetof(ArchCPU, parent_obj.f) - offsetof(ArchCPU, env))

bool translator_ras_enabled(void)
{
#if UINTPTR_MAX == UINT64_MAX && !defined(CONFIG_USER_ONLY)
    return TCG_TARGET_HAS_ras && tcg_ras && tcg_inline_lookup
        && !(tcg_ctx->gen_tb->cflags & (CF_NO_GOTO_PTR | CF_NO_GOTO_TB))
        && !qatomic_read(&one_insn_per_tb)
        && !qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC
                               | CPU_LOG_TB_NOCHAIN);
#else
    return false;
#endif
}

TCGLabel *translator_ras_call(TCGv_i64 ret_pc, unsigned idx)
{
#if UINTPTR_MAX == UINT64_MAX && !defined(CONFIG_USER_ONLY)
    TCGv_i64 jc = tcg_temp_ebb_new_i64();
    TCGv_i64 slot = tcg_temp_ebb_new_i64();
    TCGv_i32 top = tcg_temp_ebb_new_i32();
    TCGLabel *after = gen_new_label();

    QEMU_BUILD_BUG_ON(sizeof(((CPUJumpCache *)0)->ras[0]) != 32);
    tcg_gen_ld_i64(jc, tcg_env, RAS_CPU_OFF(tb_jmp_cache));
    tcg_gen_ld_i32(top, (TCGv_ptr)jc, offsetof(CPUJumpCache, ras_top));
    tcg_gen_addi_i32(top, top, 1);
    tcg_gen_andi_i32(top, top, TB_RAS_SIZE - 1);
    tcg_gen_st_i32(top, (TCGv_ptr)jc, offsetof(CPUJumpCache, ras_top));
    tcg_gen_extu_i32_i64(slot, top);
    tcg_gen_shli_i64(slot, slot, 5);
    tcg_gen_add_i64(slot, slot, jc);
    tcg_gen_addi_i64(slot, slot, offsetof(CPUJumpCache, ras));
    tcg_gen_st_i64(ret_pc, (TCGv_ptr)slot, 0);
    tcg_gen_st_i64(tcg_constant_i64((uintptr_t)tcg_ctx->gen_tb),
                   (TCGv_ptr)slot, 8);
    plugin_gen_disable_mem_helpers();
    tcg_gen_call_tb((TCGv_ptr)slot, idx, after);
    return after;
#else
    g_assert_not_reached();
#endif
}

void translator_ras_ret(TCGv_i64 pc, TCGv_i64 cs_base, TCGv_i32 flags)
{
#if UINTPTR_MAX == UINT64_MAX && !defined(CONFIG_USER_ONLY)
    TCGv_i64 jc, slot, tmp, tb, tbs, m, host;
    TCGv_i32 top, f, c;
    TCGLabel *miss;

    if (!translator_ras_enabled()) {
        return;
    }
    jc = tcg_temp_ebb_new_i64();
    slot = tcg_temp_ebb_new_i64();
    tmp = tcg_temp_ebb_new_i64();
    tb = tcg_temp_ebb_new_i64();
    tbs = tcg_temp_ebb_new_i64();
    m = tcg_temp_ebb_new_i64();
    host = tcg_temp_ebb_new_i64();
    top = tcg_temp_ebb_new_i32();
    f = tcg_temp_ebb_new_i32();
    c = tcg_temp_ebb_new_i32();
    miss = gen_new_label();

    /* pop: the entry at the top, then top - 1 */
    tcg_gen_ld_i64(jc, tcg_env, RAS_CPU_OFF(tb_jmp_cache));
    tcg_gen_ld_i32(top, (TCGv_ptr)jc, offsetof(CPUJumpCache, ras_top));
    tcg_gen_extu_i32_i64(slot, top);
    tcg_gen_shli_i64(slot, slot, 5);
    tcg_gen_add_i64(slot, slot, jc);
    tcg_gen_addi_i64(slot, slot, offsetof(CPUJumpCache, ras));
    tcg_gen_subi_i32(top, top, 1);
    tcg_gen_andi_i32(top, top, TB_RAS_SIZE - 1);
    tcg_gen_st_i32(top, (TCGv_ptr)jc, offsetof(CPUJumpCache, ras_top));

    /* m: zero iff the entry is the return this ret makes, exactly */
    tcg_gen_ld_i64(tmp, (TCGv_ptr)slot, 0);
    tcg_gen_xor_i64(m, tmp, pc);
    tcg_gen_ld_i64(tb, (TCGv_ptr)slot, 8);
    tcg_gen_movcond_i64(TCG_COND_EQ, tbs, tb, tcg_constant_i64(0),
                        tcg_constant_i64((uintptr_t)tcg_ctx->gen_tb), tb);
    tcg_gen_setcondi_i64(TCG_COND_EQ, tmp, tb, 0);
    tcg_gen_or_i64(m, m, tmp);
    tcg_gen_ld_i64(tmp, (TCGv_ptr)tbs, offsetof(TranslationBlock, cs_base));
    tcg_gen_xor_i64(tmp, tmp, cs_base);
    tcg_gen_or_i64(m, m, tmp);
    tcg_gen_ld_i64(tmp, tcg_env, RAS_CPU_OFF(breakpoints.tqh_first));
    tcg_gen_or_i64(m, m, tmp);
    tcg_gen_ld_i32(f, (TCGv_ptr)tbs, offsetof(TranslationBlock, flags));
    tcg_gen_xor_i32(f, f, flags);
    tcg_gen_ld_i32(c, (TCGv_ptr)tbs, offsetof(TranslationBlock, cflags));
    tcg_gen_or_i32(f, f, c);
    tcg_gen_ld_i32(c, tcg_env, RAS_CPU_OFF(tcg_cflags));
    tcg_gen_xor_i32(f, f, c);
    tcg_gen_ld_i32(c, tcg_env, RAS_CPU_OFF(singlestep_enabled));
    tcg_gen_or_i32(f, f, c);
    tcg_gen_extu_i32_i64(tmp, f);
    tcg_gen_or_i64(m, m, tmp);
    tcg_gen_ld_i64(host, (TCGv_ptr)slot, 16);
    tcg_gen_brcondi_i64(TCG_COND_NE, m, 0, miss);
    plugin_gen_disable_mem_helpers();
    tcg_gen_goto_ret((TCGv_ptr)host);
    gen_set_label(miss);
#endif
}

void translator_lookup_and_goto_ptr(TCGv_i64 pc, TCGv_i64 cs_base,
                                    TCGv_i32 flags)
{""")])

# ---- the i386 target ----
edit('target/i386/tcg/translate.c', [(
"""    bool rep_fast;              /* REP MOVS/STOS page runs through mem_helper.c */""",
"""    bool rep_fast;              /* REP MOVS/STOS page runs through mem_helper.c */
    bool ras_call;              /* spike D: the jump being emitted is a call's */
    bool ras_ret;               /* spike D: the TB ends with a ret */"""),
("""    dc->x87s_ntmps = 0;""",
"""    dc->x87s_ntmps = 0;
    dc->ras_call = false;
    dc->ras_ret = false;"""),
# the call side, in gen_jmp_rel
("""            translator_gen_exitreq_check(&s->base);
        }
        tcg_gen_goto_tb(tb_num);
        if (!(tb_cflags(s->base.tb) & CF_PCREL)) {
            tcg_gen_movi_tl(cpu_eip, new_eip);
        }
        tcg_gen_exit_tb(s->base.tb, tb_num);
        s->base.is_jmp = DISAS_NORETURN;""",
"""            translator_gen_exitreq_check(&s->base);
        }
        if (s->ras_call && tb_num == 0 && CODE32(s) && ot == MO_32
            && new_pc != s->pc                    /* call $+5; pop: no return */
            && (tb_cflags(s->base.tb) & CF_PCREL)
            && translator_use_goto_tb(&s->base, s->pc)
            && translator_ras_enabled()) {
            /*
             * Spike D: the call goes through a host bl, the ring learns
             * (return address, this TB, the landing), and the landing --
             * where a matching ret arrives with eip already the return
             * address -- chains to the continuation through exit 1.
             */
            TCGv_i32 t = tcg_temp_new_i32();
            TCGv_i32 cs = tcg_temp_new_i32();
            TCGv_i64 ret_pc = tcg_temp_new_i64();
            TCGLabel *after;

            /* the return address as a virtual pc: cs_base + eip after the call */
            tcg_gen_addi_i32(t, cpu_eip, s->pc - new_pc);
            tcg_gen_ld_i32(cs, tcg_env, offsetof(CPUX86State, segs[R_CS].base));
            tcg_gen_add_i32(t, t, cs);
            tcg_gen_extu_i32_i64(ret_pc, t);
            after = translator_ras_call(ret_pc, 0);
            tcg_gen_exit_tb(s->base.tb, 0);
            gen_set_label(after);
            tcg_gen_goto_tb(1);
            tcg_gen_exit_tb(s->base.tb, 1);
            s->base.is_jmp = DISAS_NORETURN;
            return;
        }
        tcg_gen_goto_tb(tb_num);
        if (!(tb_cflags(s->base.tb) & CF_PCREL)) {
            tcg_gen_movi_tl(cpu_eip, new_eip);
        }
        tcg_gen_exit_tb(s->base.tb, tb_num);
        s->base.is_jmp = DISAS_NORETURN;"""),
# the ret side, before the probe
("""    tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, segs[R_CS].base));
    tcg_gen_extu_i32_i64(cs_base, t);
    tcg_gen_add_i32(t, t, cpu_eip);
    tcg_gen_extu_i32_i64(pc, t);

    translator_lookup_and_goto_ptr(pc, cs_base, flags);""",
"""    tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, segs[R_CS].base));
    tcg_gen_extu_i32_i64(cs_base, t);
    tcg_gen_add_i32(t, t, cpu_eip);
    tcg_gen_extu_i32_i64(pc, t);

    if (s->ras_ret) {
        /* spike D: a ret tries the ring first */
        translator_ras_ret(pc, cs_base, flags);
    }
    translator_lookup_and_goto_ptr(pc, cs_base, flags);""")])

edit('target/i386/tcg/emit.c.inc', [(
"""static void gen_CALL(DisasContext *s, X86DecodedInsn *decode)
{
    gen_push_v(s, eip_next_tl(s));
    gen_JMP(s, decode);
}""",
"""static void gen_CALL(DisasContext *s, X86DecodedInsn *decode)
{
    gen_push_v(s, eip_next_tl(s));
    s->ras_call = true;         /* spike D */
    gen_JMP(s, decode);
    s->ras_call = false;
}"""),
("""    MemOp ot = gen_pop_T0(s);
    gen_stack_update(s, adjust + (1 << ot));
    gen_op_jmp_v(s, s->T0);
    gen_bnd_jmp(s);
    s->base.is_jmp = DISAS_JUMP;""",
"""    MemOp ot = gen_pop_T0(s);
    gen_stack_update(s, adjust + (1 << ot));
    gen_op_jmp_v(s, s->T0);
    gen_bnd_jmp(s);
    s->ras_ret = true;          /* spike D */
    s->base.is_jmp = DISAS_JUMP;""")])
print("applied")

import sys, pathlib
Q = pathlib.Path('qemu')
def edit(path, pairs):
    p = Q / path; s = p.read_text()
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"{path}: pattern not unique/found ({s.count(old)}): {old[:70]!r}")
        s = s.replace(old, new)
    p.write_text(s)

edit('include/exec/translator.h', [(
"""    /* spike B: the exit-request check is the target's, at back edges */
    bool exitreq_deferred;""",
"""    /* spike B: the exit-request check is the target's, at back edges */
    bool exitreq_deferred;
    /* spike B: an I/O instruction ended this TB; its jump checks whatever its direction */
    bool exitreq_io;""")])

edit('accel/tcg/translator.c', [(
"""    if (db->is_jmp == DISAS_NEXT) {
        db->is_jmp = DISAS_TOO_MANY;
    }
    return true;
}""",
"""    if (db->is_jmp == DISAS_NEXT) {
        db->is_jmp = DISAS_TOO_MANY;
    }
    /*
     * Spike B: a device access may have raised an interrupt (the PIT's
     * overdue edge, patch 34, delivered on the access itself) and the
     * TB start that used to take it before the next instruction has no
     * check now -- the jump that ends this TB carries one instead.
     */
    db->exitreq_io = true;
    return true;
}"""),
("""    db->exitreq_deferred = false;
    if (!(cflags & (CF_NOIRQ | CF_USE_ICOUNT)) && tcg_irq_check_backedge) {""",
"""    db->exitreq_deferred = false;
    db->exitreq_io = false;
    if (!(cflags & (CF_NOIRQ | CF_USE_ICOUNT)) && tcg_irq_check_backedge) {""")])

edit('target/i386/tcg/translate.c', [(
"""        if (new_pc < s->pc && s->base.exitreq_deferred) {""",
"""        if ((new_pc < s->pc || s->base.exitreq_io) && s->base.exitreq_deferred) {""")])
print("applied")

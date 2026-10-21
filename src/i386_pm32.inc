/* B86_PM32_V1 (r65): protected-mode control registers for paging and the
 * page-fault exception. Runs first in the PM chain; shared by 16/32-bit CS.
 *   0F 20 /r  MOV r32, CR0|CR2|CR3   (CR0 reads PE/MP/EM/TS and PG)
 *   0F 22 /r  MOV CR0|CR2|CR3, r32
 *   0F 00 /1  STR, 0F 00 /3 LTR (task register only; no task switches)
 * CR0: PG may be set only with PE; PG changes and CR3 writes flush the TLB.
 * Clearing PE (with PG) keeps PM25/PM29's checked far-JMP exit protocol.
 * #PF: b86_step rolls the faulting instruction back and calls
 * b86_pm32_page_fault(): CR2 = faulting linear, error code pushed after the
 * PM26 frame (gate width), EIP = first byte of the faulting instruction.
 * Task gates (#PF) and IRET with NT=1 switch tasks (32-bit TSS).
 * Returns -1 for every other opcode. */
#ifndef B86_PM32_FLASH
#define B86_PM32_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static int b86_pm32_task_switch(B86Cpu *c, uint16_t sel, unsigned kind,
                                uint32_t ret_eip, int has_err, uint32_t err);
static B86_PM32_FLASH int b86_pm32_step(B86Cpu *c)
{
    uint32_t start, ip, val, lin = 0u;
    unsigned code32, cr, reg, prefixes = 0u, p67 = 0u, segpref = 0u, seg = B86_DS;
    uint8_t op, ext, modrm;
    B86PM6EA ea;
    B86PM2Fault fault;
    if (!c || !c->mem || c->model != B86_CPU_386 || !(c->msw & 1u) || c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved == B86_PM3_TRANSITION_MARKER ||
        !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS] & 3u) || c->ip > UINT32_MAX - 15u)
        return -1;
    code32 = !!(c->pm386.seg[B86_CS].flags & 4u);
    if (!code32 && c->ip > 0xFFFFu) return -1;
    start = ip = c->ip;
    for (;;) {                                 /* operand/address size do not apply */
        if (!b86_pm4_byte(c, &ip, start, &op)) return -1;
        if (op == 0x66u || op == 0x67u) { if (++prefixes > 2u) return -1; if (op == 0x67u) p67 ^= 1u; continue; }
        if (op == 0x26u || op == 0x2Eu || op == 0x36u || op == 0x3Eu || op == 0x64u || op == 0x65u) {
            if (segpref) return -1;
            segpref = 1u;
            seg = op == 0x26u ? B86_ES : op == 0x2Eu ? B86_CS : op == 0x36u ? B86_SS :
                  op == 0x3Eu ? B86_DS : op == 0x64u ? B86_FS : B86_GS;
            continue;
        }
        break;
    }
    if (op == 0xCFu && (c->flags & 0x4000u)) {             /* IRET, NT=1: task return */
        uint16_t link = (uint16_t)(*b86_host(c, c->tr_base) | (*b86_host(c, c->tr_base + 1u) << 8));
        return b86_pm32_task_switch(c, link, 1u, code32 ? ip : (ip & 0xFFFFu), 0, 0u);
    }
    if (op != 0x0Fu) return -1;
    if (!b86_pm4_byte(c, &ip, start, &ext)) return -1;
    if (ext == 0x00u) {
        /* 0F 00 /1 STR r/m16, 0F 00 /3 LTR r/m16 (other /n stay with PM25) */
        uint32_t peek = ip;
        uint16_t sel;
        if (!b86_pm4_byte(c, &peek, start, &modrm)) return -1;
        if (((modrm >> 3) & 7u) != 1u && ((modrm >> 3) & 7u) != 3u) return -1;
        if (!b86_pm6_ea(c, &ip, start, (int)(code32 ^ p67), &ea)) return B86_UNSUPPORTED;
        if (segpref && !ea.is_reg) ea.seg = seg;
        if (!ea.is_reg && b86_pm2_check_access(c, ea.seg, ea.off, 2u,
                ((modrm >> 3) & 7u) == 1u ? B86_PM1_WRITE : B86_PM1_READ, &lin, &fault) != B86_PM2_OK)
            return B86_UNSUPPORTED;
        if (((modrm >> 3) & 7u) == 1u) {                       /* STR */
            if (ea.is_reg) c->r[ea.rm] = (c->r[ea.rm] & 0xFFFF0000u) | c->tr_sel;
            else { *b86_host(c, lin) = (uint8_t)c->tr_sel; *b86_host(c, lin + 1u) = (uint8_t)(c->tr_sel >> 8); smc(c, lin, 2u); }
        } else {                                                /* LTR */
            B86PM0Desc d;
            uint32_t at;
            sel = ea.is_reg ? (uint16_t)c->r[ea.rm]
                            : (uint16_t)(*b86_host(c, lin) | (*b86_host(c, lin + 1u) << 8));
            if (!(sel & 0xFFF8u) || (sel & 4u)) return B86_UNSUPPORTED;            /* #GP */
            if (b86_pm1_descriptor(c, sel, &d) != B86_PM1_OK || d.code_data ||
                ((d.access & 15u) != 1u && (d.access & 15u) != 9u))
                return B86_UNSUPPORTED;                                         /* #GP / #NP */
            at = c->gdt_base + (sel & 0xFFF8u) + 5u;                             /* mark busy */
            *b86_host(c, at) = (uint8_t)(*b86_host(c, at) | 2u);
            c->tr_sel = sel; c->tr_base = d.base; c->tr_limit = d.limit;
        }
        if (c->pf_pending) return B86_UNSUPPORTED;
        c->ip = code32 ? ip : (ip & 0xFFFFu);
        ++c->icount;
        return B86_OK;
    }
    if (ext != 0x20u && ext != 0x22u) return -1;
    if (segpref) return -1;
    if (!b86_pm4_byte(c, &ip, start, &modrm)) return B86_UNSUPPORTED;
    cr = (modrm >> 3) & 7u;
    reg = modrm & 7u;                           /* mod is ignored: always a register */
    if (cr != 0u && cr != 2u && cr != 3u) return B86_UNSUPPORTED;   /* #UD */
    if (ext == 0x20u) {
        c->r[reg] = cr == 0u ? ((uint32_t)c->msw & 0x1Fu) | (c->pg ? 0x80000000u : 0u)
                  : cr == 2u ? c->cr2 : c->cr3;
    } else {
        val = c->r[reg];
        if (cr == 2u) c->cr2 = val;
        else if (cr == 3u) { c->cr3 = val; b86_pg_flush(c); }
        else {
            if ((val & 0x80000000u) && !(val & 1u)) return B86_UNSUPPORTED;   /* #GP */
            if (!(val & 1u)) {
                /* PE 1 -> 0: same contract as PM25 (B86_PM_UNIFIED_V9). */
                if (c->pm386.seg[B86_CS].reserved || (c->flags&0x20000u))
                    return B86_UNSUPPORTED;
                c->pm386.seg[B86_CS].reserved = B86_PM29_EXIT_MARKER;
            }
            if (!!(val & 0x80000000u) != !!c->pg) b86_pg_flush(c);
            c->pg = (uint8_t)((val >> 31) & 1u);
            c->msw = (uint16_t)(val & 0xFu);
        }
    }
    c->ip = code32 ? ip : (ip & 0xFFFFu);
    ++c->icount;
    return B86_OK;
}

/* Deliver the recorded page fault. c->ip must already be the faulting
 * instruction's first byte (b86_step restored it). */
static B86_PM32_FLASH int b86_pm32_page_fault(B86Cpu *c)
{
    B86PM2Fault fault;
    uint32_t err = c->pf_err, slot = 0u, newsp, gate;
    unsigned w, ss32, k;
    int rc;
    c->cr2 = c->pf_lin;
    c->pf_pending = 0u;
    ++b86_pg_stat_faults;
    if ((uint32_t)14u * 8u + 7u <= c->idt_limit &&
        (*b86_host(c, c->idt_base + 14u * 8u + 5u) & 0x9Fu) == 0x85u) {   /* task gate */
        uint16_t tss = (uint16_t)(*b86_host(c, c->idt_base + 14u * 8u + 2u) |
                                  (*b86_host(c, c->idt_base + 14u * 8u + 3u) << 8));
        if (c->pf_pending) return B86_UNSUPPORTED;
        return b86_pm32_task_switch(c, tss, 0u, c->ip, 1, err);
    }
    rc = b86_pm26_interrupt(c, 14u, c->ip);
    if (rc != B86_OK || c->pf_pending) return B86_UNSUPPORTED;     /* double fault */
    gate = *b86_host(c, c->idt_base + 14u * 8u + 5u) & 15u;
    w = gate >= 14u ? 4u : 2u;
    ss32 = !!(c->pm386.seg[B86_SS].flags & 4u);
    newsp = b86_pm7_stack_add(c->r[B86_SP], (int)ss32, 0u - w);
    if (b86_pm2_check_access(c, B86_SS, ss32 ? newsp : (newsp & 0xFFFFu), w,
                             B86_PM1_WRITE, &slot, &fault) != B86_PM2_OK || c->pf_pending)
        return B86_UNSUPPORTED;
    for (k = 0u; k < w; ++k) *b86_host(c, slot + k) = (uint8_t)(err >> (8u * k));
    smc(c, slot, w);
    c->r[B86_SP] = newsp;
    return B86_OK;
}

/* ---- r65: 386 hardware task switch (32-bit TSS only) --------------------
 * Used for task gates in the IDT (DOS/4GW's VMM takes #PF through one) and
 * for IRET with EFLAGS.NT set (return to the TSS back-link).
 * kind 0: interrupt/exception through a task gate (new task busy, NT set,
 *         back-link written); kind 1: IRET task return (old task not busy).
 * The outgoing task's EIP is `ret_eip`. Error code (if any) is pushed on
 * the incoming task's stack. CR0.TS is set, as on hardware. */
static uint32_t b86_pm32_rd32(B86Cpu *c, uint32_t a)
{
    return (uint32_t)*b86_host(c, a) | ((uint32_t)*b86_host(c, a + 1u) << 8) |
           ((uint32_t)*b86_host(c, a + 2u) << 16) | ((uint32_t)*b86_host(c, a + 3u) << 24);
}
static void b86_pm32_wr32(B86Cpu *c, uint32_t a, uint32_t v)
{
    for (unsigned k = 0u; k < 4u; ++k) *b86_host(c, a + k) = (uint8_t)(v >> (8u * k));
}
static B86_PM32_FLASH int b86_pm32_task_switch(B86Cpu *c, uint16_t sel, unsigned kind,
                                                uint32_t ret_eip, int has_err, uint32_t err)
{
    B86PM0Desc d;
    B86PM2Plan plan[6];
    B86PM2Fault fault;
    static const unsigned order[6] = { B86_CS, B86_SS, B86_DS, B86_ES, B86_FS, B86_GS };
    static const unsigned tssoff[6] = { 0x4Cu, 0x50u, 0x54u, 0x48u, 0x58u, 0x5Cu };
    uint32_t nb, ob = c->tr_base, eflags, neip, nflags, ncr3, slot = 0u, newsp;
    uint16_t nsel[6], nldt, oldtr = c->tr_sel;
    unsigned i, cpl, type;
    ++b86_pg_stat_tswitch;
    if ((sel & 4u) || !(sel & 0xFFF8u) || !c->tr_sel || c->tr_limit < 0x67u) return B86_UNSUPPORTED;
    if (b86_pm1_descriptor(c, sel, &d) != B86_PM1_OK || d.code_data || d.limit < 0x67u)
        return B86_UNSUPPORTED;
    type = d.access & 15u;
    if (kind == 0u ? type != 9u : type != 11u) return B86_UNSUPPORTED;   /* 32-bit TSS, busy rules */
    nb = d.base;
    if (c->lz_kind != LZ_NONE || c->lz_ikind != LZ_NONE) b86_flags_materialize(c);
    eflags = (c->flags & 0xFFFF0000u) | b86_get_flags(c);
    if (kind == 1u) eflags &= ~0x4000u;                     /* returning task leaves NT clear */
    /* save the outgoing task */
    b86_pm32_wr32(c, ob + 0x20u, ret_eip);
    b86_pm32_wr32(c, ob + 0x24u, eflags);
    for (i = 0u; i < 8u; ++i) b86_pm32_wr32(c, ob + 0x28u + 4u * i, c->r[i]);
    b86_pm32_wr32(c, ob + 0x48u, c->seg[B86_ES] & 0xFFFFu);
    b86_pm32_wr32(c, ob + 0x4Cu, c->seg[B86_CS] & 0xFFFFu);
    b86_pm32_wr32(c, ob + 0x50u, c->seg[B86_SS] & 0xFFFFu);
    b86_pm32_wr32(c, ob + 0x54u, c->seg[B86_DS] & 0xFFFFu);
    b86_pm32_wr32(c, ob + 0x58u, c->seg386[0]);
    b86_pm32_wr32(c, ob + 0x5Cu, c->seg386[1]);
    if (kind == 1u) {                                      /* old task no longer busy */
        uint32_t at = c->gdt_base + (oldtr & 0xFFF8u) + 5u;
        *b86_host(c, at) = (uint8_t)(*b86_host(c, at) & ~2u);
    } else {
        uint32_t at = c->gdt_base + (sel & 0xFFF8u) + 5u;
        *b86_host(c, at) = (uint8_t)(*b86_host(c, at) | 2u);
        b86_pm32_wr32(c, nb + 0u, oldtr);                  /* back-link */
    }
    /* load the incoming task */
    ncr3 = b86_pm32_rd32(c, nb + 0x1Cu);
    neip = b86_pm32_rd32(c, nb + 0x20u);
    nflags = b86_pm32_rd32(c, nb + 0x24u);
    for (i = 0u; i < 6u; ++i) nsel[i] = (uint16_t)b86_pm32_rd32(c, nb + tssoff[i]);
    nldt = (uint16_t)b86_pm32_rd32(c, nb + 0x60u);
    if (nflags & 0x20000u) return B86_UNSUPPORTED;         /* VM86 task */
    for (i = 0u; i < 8u; ++i) c->r[i] = b86_pm32_rd32(c, nb + 0x28u + 4u * i);
    c->tr_sel = sel; c->tr_base = nb; c->tr_limit = d.limit;
    if (c->pg) { c->cr3 = ncr3; b86_pg_flush(c); }
    else c->cr3 = ncr3;
    c->msw = (uint16_t)(c->msw | 8u);                      /* CR0.TS */
    if (b86_pm1_prepare_ldtr(c, nldt) != B86_PM1_OK) return B86_UNSUPPORTED;
    c->flags = (nflags | 2u) | (kind == 0u ? 0x4000u : 0u);
    c->lz_kind = LZ_NONE; c->lz_ikind = LZ_NONE;
    cpl = nsel[0] & 3u;
    for (i = 0u; i < 6u; ++i)
        if (b86_pm2_plan_segment(c, order[i], nsel[i], cpl, order[i] == B86_CS ? neip : 0u,
                                 &plan[i], &fault) != B86_PM2_OK)
            return B86_UNSUPPORTED;
    for (i = 0u; i < 6u; ++i)
        if (b86_pm2_commit_segment(c, &plan[i], cpl) != B86_PM2_OK) return B86_UNSUPPORTED;
    if (has_err) {
        unsigned ss32 = !!(c->pm386.seg[B86_SS].flags & 4u);
        newsp = b86_pm7_stack_add(c->r[B86_SP], (int)ss32, 0u - 4u);
        if (b86_pm2_check_access(c, B86_SS, ss32 ? newsp : (newsp & 0xFFFFu), 4u,
                                 B86_PM1_WRITE, &slot, &fault) != B86_PM2_OK)
            return B86_UNSUPPORTED;
        b86_pm32_wr32(c, slot, err);
        c->r[B86_SP] = newsp;
    }
    ++c->icount;
    return c->pf_pending ? B86_UNSUPPORTED : B86_OK;
}


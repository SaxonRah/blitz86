/* B86_PM30_V1: protected-mode indirect far CALL/JMP, FF /3 and FF /5
 * (m16:16 and m16:32), shared by 16- and 32-bit code segments, with 66h/67h
 * and one segment-override prefix. DOS/4GW's 16-bit kernel dispatches with
 * JMP FAR [CS:xxxx]-style pointers right after PE entry.
 * Same-CPL0 code-segment targets only (PM2 plan): call gates, task gates and
 * privilege changes stay B86_UNSUPPORTED. The pointer read, the descriptor,
 * the destination limit and both stack slots are validated before any guest
 * state changes. Returns -1 for every other opcode.
 */
#ifndef B86_PM30_FLASH
#define B86_PM30_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM30_FLASH int b86_pm30_step(B86Cpu *c)
{
    uint32_t start, ip, lin = 0u, target = 0u, retip, oldsp, sp1, sp2, l1 = 0u, l2 = 0u;
    unsigned code32, op32, ad32, width, seg = B86_DS, segprefix = 0u, p66 = 0u, p67 = 0u, ss32, k;
    uint16_t selector;
    uint8_t op, modrm;
    B86PM6EA ea;
    B86PM2Fault fault;
    B86PM2Plan plan;
    if (!c || !c->mem || c->model != B86_CPU_386 || !(c->msw & 1u) || c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved == B86_PM3_TRANSITION_MARKER ||
        !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS] & 3u) || c->ip > UINT32_MAX - 15u)
        return -1;
    code32 = !!(c->pm386.seg[B86_CS].flags & 4u);
    if (!code32 && c->ip > 0xFFFFu) return -1;
    start = ip = c->ip;
    for (;;) {
        if (!b86_pm4_byte(c, &ip, start, &op)) return -1;
        if (op == 0x66u) { if (p66) return -1; p66 = 1u; continue; }
        if (op == 0x67u) { if (p67) return -1; p67 = 1u; continue; }
        if (op == 0x26u || op == 0x2Eu || op == 0x36u || op == 0x3Eu || op == 0x64u || op == 0x65u) {
            if (segprefix) return -1;
            segprefix = 1u;
            seg = op == 0x26u ? B86_ES : op == 0x2Eu ? B86_CS : op == 0x36u ? B86_SS :
                  op == 0x3Eu ? B86_DS : op == 0x64u ? B86_FS : B86_GS;
            continue;
        }
        break;
    }
    if (op != 0xFFu) return -1;
    {
        uint32_t peek = ip;
        if (!b86_pm4_byte(c, &peek, start, &modrm)) return -1;
        if (((modrm >> 3) & 7u) != 3u && ((modrm >> 3) & 7u) != 5u) return -1;
    }
    op32 = code32 ^ p66;
    ad32 = code32 ^ p67;
    width = op32 ? 4u : 2u;
    if (!b86_pm6_ea(c, &ip, start, (int)ad32, &ea)) return B86_UNSUPPORTED;
    if (ea.is_reg) return B86_UNSUPPORTED;                 /* #UD: far needs memory */
    if (segprefix) ea.seg = seg;
    if (b86_pm2_check_access(c, ea.seg, ea.off, width + 2u, B86_PM1_READ, &lin, &fault) != B86_PM2_OK)
        return B86_UNSUPPORTED;
    for (k = 0; k < width; ++k) target |= (uint32_t)*b86_host(c, lin + k) << (8u * k);
    selector = (uint16_t)(*b86_host(c, lin + width) | (*b86_host(c, lin + width + 1u) << 8));
    retip = code32 ? ip : (ip & 0xFFFFu);
    if (b86_pm2_plan_segment(c, B86_CS, selector, 0u, target, &plan, &fault) != B86_PM2_OK)
        return B86_UNSUPPORTED;
    if (!(plan.cache.flags & 4u) && target > 0xFFFFu) return B86_UNSUPPORTED;
    if (((modrm >> 3) & 7u) == 5u) {                       /* /5 JMP FAR */
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) return B86_UNSUPPORTED;
        ++c->icount;
        return B86_OK;
    }
    /* /3 CALL FAR: push CS, then the return offset (both operand-size wide). */
    ss32 = !!(c->pm386.seg[B86_SS].flags & 4u);
    oldsp = c->r[B86_SP];
    sp1 = b86_pm7_stack_add(oldsp, (int)ss32, 0u - width);
    sp2 = b86_pm7_stack_add(sp1, (int)ss32, 0u - width);
    if (b86_pm2_check_access(c, B86_SS, ss32 ? sp1 : (sp1 & 0xFFFFu), width, B86_PM1_WRITE, &l1, &fault) != B86_PM2_OK ||
        b86_pm2_check_access(c, B86_SS, ss32 ? sp2 : (sp2 & 0xFFFFu), width, B86_PM1_WRITE, &l2, &fault) != B86_PM2_OK)
        return B86_UNSUPPORTED;
    {
        uint32_t oldcs = c->seg[B86_CS] & 0xFFFFu;
        for (k = 0; k < width; ++k) {
            *b86_host(c, l1 + k) = (uint8_t)(oldcs >> (8u * k));
            *b86_host(c, l2 + k) = (uint8_t)(retip >> (8u * k));
        }
        smc(c, l1, width); smc(c, l2, width);
    }
    c->r[B86_SP] = sp2;
    if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) return B86_UNSUPPORTED;
    ++c->icount;
    return B86_OK;
}

/* B86_PG_V1 (r65): 80386 two-level paging for protected mode.
 * Included by interp.c before the PM stages. CR0.PG is mirrored in c->pg and
 * is only set while PE=1, so real mode and the JIT never translate.
 *  - 4 KiB pages, PDE/PTE present bits; supervisor/user and R/W are not
 *    enforced (a 386 at CPL0 ignores them and the guest runs at CPL0);
 *  - Accessed is set on the PDE, Accessed+Dirty on the PTE whenever a
 *    translation is filled (every access is treated as a possible write so a
 *    VMM never discards a modified page);
 *  - 64-entry direct-mapped TLB, flushed by CR3 writes and PG changes;
 *  - a miss records the first faulting linear address (CR2) and returns a
 *    scratch page; b86_step rolls the instruction back and delivers #PF.
 * Physical frames beyond guest RAM read/write the scratch page. */
#ifndef B86_PG_FLASH
#define B86_PG_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static uint8_t b86_pg_sink[256u + 16u];   /* scratch for non-present/unbacked pages */
/* r65 statistics (diagnostics only) */
unsigned long b86_pg_stat_faults, b86_pg_stat_tswitch, b86_pg_stat_walks, b86_pg_stat_flush;

static inline uint32_t b86_pg_rd32(const B86Cpu *c, uint32_t a)
{
    const uint8_t *p = b86_host_phys(c, a);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void b86_pg_wr32(B86Cpu *c, uint32_t a, uint32_t v)
{
    uint8_t *p = b86_host_phys(c, a);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static B86_PG_FLASH void b86_pg_flush(B86Cpu *c)
{
    ++b86_pg_stat_flush;
    memset(c->tlb_tag, 0, sizeof c->tlb_tag);
}
/* 1 = translated (*phys), 0 = not present. Never records a fault. */
static B86_PM_HOT int b86_pg_walk(B86Cpu *c, uint32_t lin, uint32_t *phys)
{
    const unsigned slot = (lin >> 12) & 63u;
    const uint32_t tag = (lin & 0xFFFFF000u) | 1u;
    uint32_t pdea, pde, ptea, pte;
    if (c->tlb_tag[slot] == tag) { *phys = c->tlb_phys[slot] | (lin & 0xFFFu); return 1; }
    ++b86_pg_stat_walks;
    pdea = (c->cr3 & 0xFFFFF000u) + ((lin >> 22) << 2);
    if (pdea > B86_RAM_BYTES - 4u) return 0;
    pde = b86_pg_rd32(c, pdea);
    if (!(pde & 1u)) return 0;
    ptea = (pde & 0xFFFFF000u) + (((lin >> 12) & 0x3FFu) << 2);
    if (ptea > B86_RAM_BYTES - 4u) return 0;
    pte = b86_pg_rd32(c, ptea);
    if (!(pte & 1u)) return 0;
    if (!(pde & 0x20u)) b86_pg_wr32(c, pdea, pde | 0x20u);
    if ((pte & 0x60u) != 0x60u) b86_pg_wr32(c, ptea, pte | 0x60u);
    c->tlb_tag[slot] = tag;
    c->tlb_phys[slot] = pte & 0xFFFFF000u;
    *phys = (pte & 0xFFFFF000u) | (lin & 0xFFFu);
    return 1;
}
static void b86_pg_fault(B86Cpu *c, uint32_t lin, unsigned write)
{
    if (c->pf_pending) return;                 /* keep the first fault */
    c->pf_pending = 1u;
    c->pf_lin = lin;
    c->pf_err = (uint8_t)((write ? 2u : 0u) | ((c->seg[B86_CS] & 3u) == 3u ? 4u : 0u));
}
B86_PM_HOT uint8_t *b86_pg_host(B86Cpu *c, uint32_t lin)
{
    uint32_t phys;
    const unsigned slot = (lin >> 12) & 63u;                 /* r66: TLB hit inline */
    if (c->tlb_tag[slot] == ((lin & 0xFFFFF000u) | 1u)) {
        phys = c->tlb_phys[slot] | (lin & 0xFFFu);
        if (phys <= B86_RAM_BYTES - 1u) return b86_host_phys(c, phys);
    }
    if (!b86_pg_walk(c, lin, &phys)) {
        b86_pg_fault(c, lin, 0u);
        return b86_pg_sink + (lin & 0xFFu);
    }
    if (phys > B86_RAM_BYTES - 1u) return b86_pg_sink + (phys & 0xFFu);
    return b86_host_phys(c, phys);
}
/* Preflight every page of [lin, lin+width): 0 = #PF recorded. */
static B86_PG_FLASH int b86_pg_probe(B86Cpu *c, uint32_t lin, uint32_t width, unsigned write)
{
    uint32_t phys, at = lin, last = lin + (width ? width - 1u : 0u);
    for (;;) {
        if (!b86_pg_walk(c, at, &phys)) { b86_pg_fault(c, at, write); return 0; }
        if ((at & 0xFFFFF000u) == (last & 0xFFFFF000u)) return 1;
        at = (at & 0xFFFFF000u) + 0x1000u;
    }
}

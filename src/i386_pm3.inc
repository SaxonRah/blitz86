/* BB_386_STAGE5C5E_V1: deliberately bounded 80386 protected-mode entry.
 * Before a far JMP the old real-mode CS descriptor cache is still in use;
 * after the validated far JMP the next instruction is fail-closed. */
#include "b86_pm3.h"
#ifndef B86_PM3_FLASH
#define B86_PM3_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM3_FLASH int b86_pm3_enter(B86Cpu *cpu, uint32_t cr0)
{
    B86PM1Cache caches[6];
    unsigned i;
    if (!cpu || !cpu->mem) return B86_PM3_ARGUMENT;
    if (cpu->model != B86_CPU_386 || cpu->shutdown386 ||
        (cpu->msw & 1u) || !(cr0 & 1u) || (cr0 & ~0x1Fu) || cpu->ip > 0xFFFFu)
        return B86_PM3_UNSUPPORTED;
    /* Transactional validation: all six old real-mode caches are computed
     * before setting CR0.PE, preserving selector-derived real-mode bases. */
    memset(caches, 0, sizeof caches);
    for (i = 0; i < 6; ++i) {
        uint16_t selector = (uint16_t)(i < 4 ? cpu->seg[i] : cpu->seg386[i-4]);
        uint32_t base = (uint32_t)selector << 4;
        if ((uint64_t)base + 0xFFFFu >= B86_MEM_BYTES)
            return B86_PM3_UNSUPPORTED;
        caches[i].base = base;
        caches[i].limit = 0xFFFFu;
        caches[i].selector = selector;
        caches[i].access = i == B86_CS ? 0x9Bu : 0x93u;
        caches[i].flags = 0;
        caches[i].valid = 1;
    }
    /* Execute-transition sentinel belongs only to the initial CS cache. */
    caches[B86_CS].reserved = B86_PM3_TRANSITION_MARKER;
    for (i = 0; i < 6; ++i) cpu->pm386.seg[i] = caches[i];
    cpu->msw = (uint16_t)((cpu->msw & (uint16_t)~0xFu) | (cr0 & 0xFu));
    return B86_PM3_OK;
}

static B86_PM3_FLASH int b86_pm3_fetch(const B86Cpu *c, uint32_t off, uint8_t *out)
{
    uint64_t lin;
    const B86PM1Cache *cs = &c->pm386.seg[B86_CS];
    if (off > cs->limit || !cs->valid || !(cs->access & 0x80u)) return 0;
    lin = (uint64_t)cs->base + off;
    if (lin >= B86_RAM_BYTES) return 0;
    *out = *b86_host(c, (uint32_t)lin);
    return 1;
}

B86_PM3_FLASH int b86_pm3_transition_step(B86Cpu *c)
{
    B86PM2Plan plan;
    B86PM2Fault fault;
    uint32_t pc, off = 0;
    uint16_t selector;
    uint8_t b, pref = 0;
    unsigned i, width;
    if (!c || c->model != B86_CPU_386 || !(c->msw & 1u) ||
        c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved != B86_PM3_TRANSITION_MARKER)
        return B86_UNSUPPORTED;
    pc = c->ip;
    if (!b86_pm3_fetch(c, pc, &b)) return B86_UNSUPPORTED;
    if (b == 0x66u) {
        pref = 1;
        if (!b86_pm3_fetch(c, ++pc, &b)) return B86_UNSUPPORTED;
    }
    /* PE is set but existing CS is 16-bit. Only this one transition far
     * jump is implemented; do not accidentally execute real-mode opcodes. */
    if (b != 0xEAu) return B86_UNSUPPORTED;
    width = pref ? 4u : 2u;
    for (i=0; i<width; ++i) {
        if (!b86_pm3_fetch(c, pc+1u+i, &b)) return B86_UNSUPPORTED;
        off |= (uint32_t)b << (8u*i);
    }
    if (!b86_pm3_fetch(c, pc+1u+width, &b)) return B86_UNSUPPORTED;
    selector = b;
    if (!b86_pm3_fetch(c, pc+2u+width, &b)) return B86_UNSUPPORTED;
    selector |= (uint16_t)b << 8;
    /* Stage 5C5D validates executable type, CPL=0, presence, target limit
     * and representable physical memory BEFORE committing visible CS/EIP. */
    if (b86_pm2_plan_segment(c, B86_CS, selector, 0, off, &plan, &fault)
        != B86_PM2_OK)
        return B86_UNSUPPORTED; /* PE #GP/#NP delivery not available yet */
    if (b86_pm2_commit_segment(c, &plan, 0) != B86_PM2_OK)
        return B86_UNSUPPORTED;
    /* PM2 commit clears transition marker. All subsequent PE instructions
     * fail closed until descriptor-backed fetch/execute is implemented. */
    ++c->icount;
    return B86_OK;
}

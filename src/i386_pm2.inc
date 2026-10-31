/* BB_386_STAGE5C5D_V1: cold, fail-closed protected segment-load planning.
 * PE execution remains disabled. Does not participate in existing real-mode
 * fetch, memory operations, JIT or interrupts. */
#include "b86_pm2.h"
#ifndef B86_PM2_FLASH
#define B86_PM2_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
#ifndef B86_PJ_V86_SRAM   /* V86: hot runtime functions in SRAM instead of flash */
#define B86_PJ_V86_SRAM 1
#endif

static B86_PM2_FLASH int b86_pm2_fault(B86PM2Fault *fault,
                                      unsigned vector, unsigned code)
{
    fault->vector = (uint8_t)vector;
    fault->error_code = (uint16_t)code;
    fault->reserved = 0;
    return B86_PM2_FAULT;
}

#if B86_PJ_V86_SRAM
__attribute__((noinline)) B86_PM_HOT int b86_pm2_plan_segment(const B86Cpu *cpu, unsigned seg,
#else
B86_PM2_FLASH int b86_pm2_plan_segment(const B86Cpu *cpu, unsigned seg,
#endif
                                        uint16_t selector, unsigned cpl,
                                        uint32_t target_eip, B86PM2Plan *plan,
                                        B86PM2Fault *fault)
{
    B86PM0Desc d;
    B86PM2Plan p;
    unsigned rpl = selector & 3u;
    unsigned error = selector & 0xFFFCu;
    int rc;
    if (!cpu || !cpu->mem || !plan || !fault || seg >= 6u || cpl > 3u)
        return B86_PM2_ARGUMENT;
    if (cpu->model != B86_CPU_386) return B86_PM2_UNSUPPORTED;
    if (seg != B86_CS && target_eip != 0u) return B86_PM2_ARGUMENT;
    memset(&p, 0, sizeof(p));
    p.magic = B86_PM2_PLAN_MAGIC;
    p.seg = (uint8_t)seg;
    p.cpl = (uint8_t)cpl;
    p.selector = selector;
    if ((selector & 0xFFF8u) == 0u) {
        if (seg == B86_CS || seg == B86_SS)
            return b86_pm2_fault(fault, B86_PM2_GP, 0u);
        p.cache.selector = selector; /* null data segment: inaccessible */
        *plan = p;
        return B86_PM2_OK;
    }
    rc = b86_pm1_descriptor(cpu, selector, &d);
    /* PM1 fills the descriptor even if its P bit is clear. Privilege and
     * type checks precede the not-present fault on x86 segment loads. */
    if (rc != B86_PM1_OK && rc != B86_PM1_NOT_PRESENT) {
        if (rc == B86_PM1_SELECTOR || rc == B86_PM1_TABLE_LIMIT ||
            rc == B86_PM1_NO_LDT)
            return b86_pm2_fault(fault, B86_PM2_GP, error);
        return B86_PM2_UNSUPPORTED; /* physical guest memory not representable */
    }
    if (!d.code_data) return b86_pm2_fault(fault, B86_PM2_GP, error);
    if (seg == B86_SS) {
        if (d.executable || !d.read_write)
            return b86_pm2_fault(fault, B86_PM2_GP, error);
        if (d.dpl != cpl || rpl != cpl)
            return b86_pm2_fault(fault, B86_PM2_GP, error);
    } else if (seg == B86_CS) {
        if (!d.executable)
            return b86_pm2_fault(fault, B86_PM2_GP, error);
        if (d.conforming_expand) {
            if (d.dpl > cpl)
                return b86_pm2_fault(fault, B86_PM2_GP, error);
        } else if (d.dpl != cpl || rpl > cpl) {
            return b86_pm2_fault(fault, B86_PM2_GP, error);
        }
        /* Same-level far transfer only: preserve the current CPL. */
        p.selector = (uint16_t)((selector & 0xFFFCu) | cpl);
        p.target_eip = target_eip;
    } else if (d.executable) {
        /* Nonconforming readable code may be used as a data selector.
         * Conforming readable code has additional RPL/privilege rules;
         * defer it rather than manufacturing #GP. */
        if (d.conforming_expand) return B86_PM2_UNSUPPORTED;
        if (!d.read_write) return b86_pm2_fault(fault, B86_PM2_GP, error);
        if (cpl > d.dpl || rpl > d.dpl)
            return b86_pm2_fault(fault, B86_PM2_GP, error);
    } else if (cpl > d.dpl || rpl > d.dpl) {
        return b86_pm2_fault(fault, B86_PM2_GP, error);
    }
    if (!d.present)
        return b86_pm2_fault(fault, seg == B86_SS ? B86_PM2_SS : B86_PM2_NP, error);
    if (seg == B86_CS) {
        if (target_eip > d.limit)
            return b86_pm2_fault(fault, B86_PM2_GP, 0u);
        if (!cpu->pg && (uint64_t)d.base + target_eip >= B86_RAM_BYTES)   /* r65 */
            return B86_PM2_UNSUPPORTED;
    }
    p.cache.base = d.base;
    p.cache.limit = d.limit;
    p.cache.selector = p.selector;
    p.cache.access = d.access;
    p.cache.flags = d.flags;
    p.cache.valid = 1u;
    *plan = p;
    return B86_PM2_OK;
}

#if B86_PJ_V86_SRAM
__attribute__((noinline)) B86_PM_HOT int b86_pm2_commit_segment(B86Cpu *cpu,
#else
B86_PM2_FLASH int b86_pm2_commit_segment(B86Cpu *cpu,
#endif
                                          const B86PM2Plan *plan, unsigned cpl)
{
    if (!cpu || !plan || cpl > 3u || plan->magic != B86_PM2_PLAN_MAGIC ||
        plan->seg >= 6u || plan->cpl != cpl ||
        plan->cache.selector != plan->selector)
        return B86_PM2_ARGUMENT;
    if (cpu->model != B86_CPU_386 || !(cpu->msw & 1u))
        return B86_PM2_UNSUPPORTED;
    /* One cold commit point; validation performed before touching any state.
     * These selectors are *not* interpreted as segment*16 bases in PE.
     * Legacy segp[] remains untouched because PE execution is blocked. */
    if (plan->seg <= B86_DS) cpu->seg[plan->seg] = plan->selector;
    else cpu->seg386[plan->seg - B86_FS] = plan->selector;
    cpu->pm386.seg[plan->seg] = plan->cache;
    if (plan->seg == B86_CS) cpu->ip = plan->target_eip;
    return B86_PM2_OK;
}

B86_PM2_FLASH int b86_pm2_check_access(const B86Cpu *cpu, unsigned seg,
                                       uint32_t offset, uint32_t width,
                                       unsigned access, uint32_t *linear,
                                       B86PM2Fault *fault)
{
    int rc;
    if (!cpu || !fault || !linear || seg >= 6u || !width || !access ||
        (access & ~(unsigned)(B86_PM1_READ|B86_PM1_WRITE|B86_PM1_EXEC)))
        return B86_PM2_ARGUMENT;
    if (cpu->model != B86_CPU_386) return B86_PM2_UNSUPPORTED;
    rc = b86_pm1_translate(cpu, seg, offset, width, access, linear);
    if (rc == B86_PM1_OK) return B86_PM2_OK;
    if (rc == B86_PM1_SEGMENT_LIMIT || rc == B86_PM1_SELECTOR ||
        rc == B86_PM1_ACCESS || rc == B86_PM1_TYPE || rc == B86_PM1_NOT_PRESENT)
        return b86_pm2_fault(fault, seg == B86_SS ? B86_PM2_SS : B86_PM2_GP, 0u);
    return B86_PM2_UNSUPPORTED;
}

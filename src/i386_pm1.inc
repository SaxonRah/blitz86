/* BB_386_STAGE5C5C_V1: pre-transition descriptor-cache foundation.
 * The existing real-mode CPU/JIT never consults these caches. CR0.PE,
 * interrupts, protected control transfers, paging and 32-bit IP execution
 * are NOT enabled by this stage. PM0 decoding is reused unchanged.
 */
#include "b86_pm1.h"
#ifndef B86_PM1_FLASH
#define B86_PM1_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

static B86_PM1_FLASH int b86_pm1_table(const B86Cpu *cpu, uint16_t selector,
                                      uint32_t *base, uint32_t *limit)
{
    if ((selector & 4u) != 0u) {
        if (!cpu->pm386.ldtr_valid) return B86_PM1_NO_LDT;
        *base = cpu->pm386.ldtr_base;
        *limit = cpu->pm386.ldtr_limit;
    } else {
        *base = cpu->gdt_base;
        *limit = cpu->gdt_limit;
    }
    return B86_PM1_OK;
}

B86_PM1_FLASH int b86_pm1_descriptor(const B86Cpu *cpu,
                                      uint16_t selector, B86PM0Desc *out)
{
    uint32_t base, limit, index;
    uint64_t address;
    uint8_t bytes[8];
    int rc;
    if (!cpu || !cpu->mem || !out) return B86_PM1_ARGUMENT;
    if (cpu->model < B86_CPU_386) return B86_PM1_UNSUPPORTED;
    if ((selector & 0xFFF8u) == 0u) return B86_PM1_SELECTOR;
    rc = b86_pm1_table(cpu, selector, &base, &limit);
    if (rc != B86_PM1_OK) return rc;
    index = selector & 0xFFF8u;
    if (index > limit || limit - index < 7u) return B86_PM1_TABLE_LIMIT;
    address = (uint64_t)base + (uint64_t)index;
    if (!cpu->pg && address + 8u > B86_RAM_BYTES) return B86_PM1_MEMORY_LIMIT;   /* r65 */
    for (unsigned i=0; i<8u; ++i)
        bytes[i] = *b86_host(cpu, (uint32_t)address + i);
    rc = b86_pm0_decode(bytes, selector, out);
    if (rc == B86_PM0_NOT_PRESENT) return B86_PM1_NOT_PRESENT;
    return rc == B86_PM0_OK ? B86_PM1_OK : B86_PM1_ARGUMENT;
}

B86_PM1_FLASH int b86_pm1_prepare_ldtr(B86Cpu *cpu, uint16_t selector)
{
    B86PM0Desc d;
    int rc;
    if (!cpu || !cpu->mem) return B86_PM1_ARGUMENT;
    if (cpu->model < B86_CPU_386) return B86_PM1_UNSUPPORTED;
    if ((selector & 0xFFF8u) == 0u && !(selector & 4u)) {
        cpu->pm386.ldtr_valid = 0u;
        cpu->pm386.ldtr_selector = 0u;
        return B86_PM1_OK;
    }
    /* LDTR descriptor must live in the GDT (TI=0). */
    if (selector & 4u) return B86_PM1_SELECTOR;
    rc = b86_pm1_descriptor(cpu, selector, &d);
    if (rc) return rc;
    if (d.code_data || (d.access & 0x0Fu) != 2u)
        return B86_PM1_TYPE;
    /* The LDT table itself must be representable in guest memory. */
    if (!cpu->pg && (uint64_t)d.base + (uint64_t)d.limit >= B86_RAM_BYTES)   /* r65 */
        return B86_PM1_MEMORY_LIMIT;
    cpu->pm386.ldtr_base = d.base;
    cpu->pm386.ldtr_limit = d.limit;
    cpu->pm386.ldtr_selector = selector;
    cpu->pm386.ldtr_valid = 1u;
    return B86_PM1_OK;
}

B86_PM1_FLASH int b86_pm1_prepare_segment(B86Cpu *cpu, unsigned seg,
                                            uint16_t selector, unsigned cpl)
{
    B86PM0Desc d;
    B86PM1Cache cache;
    int rc;
    unsigned rpl = selector & 3u;
    if (!cpu || !cpu->mem || seg >= 6u || cpl > 3u)
        return B86_PM1_ARGUMENT;
    if (cpu->model < B86_CPU_386) return B86_PM1_UNSUPPORTED;
    /* Loading CS requires protected-mode branch/interrupt semantics. */
    if (seg == B86_CS) return B86_PM1_UNSUPPORTED;
    if ((selector & 0xFFF8u) == 0u) {
        if (seg == B86_SS) return B86_PM1_SELECTOR;
        memset(&cpu->pm386.seg[seg], 0, sizeof(cpu->pm386.seg[seg]));
        cpu->pm386.seg[seg].selector = selector;
        return B86_PM1_OK;
    }
    rc = b86_pm1_descriptor(cpu, selector, &d);
    if (rc) return rc;
    if (!d.code_data) return B86_PM1_TYPE;
    if (seg == B86_SS) {
        /* Stack selector: writable data, DPL=RPL=CPL. */
        if (d.executable || !d.read_write) return B86_PM1_TYPE;
        if (d.dpl != cpl || rpl != cpl) return B86_PM1_PRIVILEGE;
    } else {
        /* Conservative: only data or readable nonconforming code in PM1.
         * Conforming-code selection requires additional privilege rules. */
        if (d.executable && (!d.read_write || d.conforming_expand))
            return B86_PM1_UNSUPPORTED;
        if ((cpl > d.dpl) || (rpl > d.dpl))
            return B86_PM1_PRIVILEGE;
    }
    memset(&cache, 0, sizeof(cache));
    cache.base = d.base;
    cache.limit = d.limit;
    cache.selector = selector;
    cache.access = d.access;
    cache.flags = d.flags;
    cache.valid = 1u;
    /* The cache is intentionally decoupled from the visible CPU selectors.
     * Later stages will commit hidden and visible state atomically. */
    cpu->pm386.seg[seg] = cache;
    return B86_PM1_OK;
}

B86_PM1_FLASH int b86_pm1_translate(const B86Cpu *cpu, unsigned seg,
                                      uint32_t offset, uint32_t width,
                                      unsigned access, uint32_t *linear)
{
    const B86PM1Cache *c;
    uint32_t maximum;
    uint64_t end, phys;
    int executable, writable, expand_down;
    if (!cpu || !linear || seg >= 6u || !width ||
        !access || (access & ~(unsigned)(B86_PM1_READ|B86_PM1_WRITE|B86_PM1_EXEC)))
        return B86_PM1_ARGUMENT;
    if (cpu->model < B86_CPU_386) return B86_PM1_UNSUPPORTED;
    c = &cpu->pm386.seg[seg];
    if (!c->valid) return B86_PM1_SELECTOR;
    if (!(c->access & 0x80u)) return B86_PM1_NOT_PRESENT;
    if (!(c->access & 0x10u)) return B86_PM1_TYPE;
    executable = (c->access & 8u) != 0u;
    writable = (c->access & 2u) != 0u;
    expand_down = !executable && ((c->access & 4u) != 0u);
    if ((access & B86_PM1_EXEC) && !executable) return B86_PM1_ACCESS;
    if ((access & B86_PM1_WRITE) && (executable || !writable))
        return B86_PM1_ACCESS;
    if ((access & B86_PM1_READ) && executable && !writable)
        return B86_PM1_ACCESS;
    if (seg == B86_SS && (executable || !writable)) return B86_PM1_TYPE;
    end = (uint64_t)offset + (uint64_t)width - 1u;
    if (end > UINT32_MAX) return B86_PM1_SEGMENT_LIMIT;
    if (expand_down) {
        maximum = (c->flags & 4u) ? 0xFFFFFFFFu : 0xFFFFu;
        if (offset <= c->limit || end > maximum)
            return B86_PM1_SEGMENT_LIMIT;
    } else if (end > c->limit) return B86_PM1_SEGMENT_LIMIT;
    phys = (uint64_t)c->base + (uint64_t)offset;
    if (cpu->pg) {                         /* r65: linear; preflight the pages */
        if (phys > 0xFFFFFFFFull - (width - 1u)) return B86_PM1_SEGMENT_LIMIT;
        if (!b86_pg_probe((B86Cpu *)cpu, (uint32_t)phys, width, (access & B86_PM1_WRITE) != 0u))
            return B86_PM1_PAGE;
    } else if (phys + width > B86_RAM_BYTES) return B86_PM1_MEMORY_LIMIT;
    *linear = (uint32_t)phys;
    return B86_PM1_OK;
}

/* BB_386_PM0_V1 - flash-only 386 descriptor decode / GDT lookup.
 * No PE transitions, segment caches, privilege checks or exception delivery.
 */
#include "b86_pm0.h"
#define B86_PM0_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))

B86_PM0_FLASH int b86_pm0_decode(const uint8_t bytes[8], uint16_t selector,
                                 B86PM0Desc *out)
{
    uint32_t limit, base;
    uint8_t a, f;
    if (!bytes || !out) return B86_PM0_BAD_ARGUMENT;
    limit = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            (((uint32_t)bytes[6] & 15u) << 16);
    base = (uint32_t)bytes[2] | ((uint32_t)bytes[3] << 8) |
           ((uint32_t)bytes[4] << 16) | ((uint32_t)bytes[7] << 24);
    a = bytes[5];
    f = bytes[6] >> 4;
    memset(out, 0, sizeof(*out));
    out->selector = selector;
    out->base = base;
    out->raw_limit = limit;
    out->granularity = (f >> 3) & 1u;
    out->limit = out->granularity ? (limit << 12) | 0xFFFu : limit;
    out->access = a;
    out->flags = f;
    out->dpl = (a >> 5) & 3u;
    out->present = (a >> 7) & 1u;
    out->code_data = (a >> 4) & 1u;
    out->executable = (a >> 3) & 1u;
    out->conforming_expand = (a >> 2) & 1u;
    out->read_write = (a >> 1) & 1u;
    out->accessed = a & 1u;
    out->default32 = (f >> 2) & 1u;
    return out->present ? B86_PM0_OK : B86_PM0_NOT_PRESENT;
}

B86_PM0_FLASH int b86_pm0_lookup_gdt(const B86Cpu *cpu,
                                     uint16_t selector, B86PM0Desc *out)
{
    uint32_t index;
    uint64_t address;
    uint8_t bytes[8];
    if (!cpu || !out || !cpu->mem) return B86_PM0_BAD_ARGUMENT;
    if ((selector & 0xFFF8u) == 0) return B86_PM0_NULL;
    if (selector & 4u) return B86_PM0_LDT;
    index = (uint32_t)(selector & 0xFFF8u);
    /* An eight-byte descriptor must fit both GDTR.limit and guest memory. */
    if (index > cpu->gdt_limit || cpu->gdt_limit - index < 7u)
        return B86_PM0_TABLE_LIMIT;
    address = (uint64_t)cpu->gdt_base + index;
    if (address + 8u > B86_MEM_BYTES) return B86_PM0_MEM_LIMIT;
    for (unsigned i = 0; i < 8u; ++i)
        bytes[i] = *b86_host(cpu, (uint32_t)address + i);
    return b86_pm0_decode(bytes, selector, out);
}

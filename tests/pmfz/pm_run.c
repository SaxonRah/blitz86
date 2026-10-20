/* PM differential harness (blitz86 side). stdin: 9 u32 (eax..edi in x86 order,
 * eflags), u32 code_len, code bytes (placed at 0x10000), data image of 0x1000
 * bytes at 0x20000. Runs flat CPL0 32-bit protected mode until EIP reaches
 * the end of code or 200 steps. stdout: status, 9 u32, eip, data image. */
#include "b86.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint8_t mem[B86_MEM_BYTES] __attribute__((aligned(64)));
static B86Cpu c;
int main(void)
{
    uint32_t regs[9], len; uint8_t code[4096];
    if (fread(regs, 4, 9, stdin) != 9 || fread(&len, 4, 1, stdin) != 1 || len > sizeof code) return 2;
    if (fread(code, 1, len, stdin) != len) return 2;
    if (fread(mem + 0x20000, 1, 0x1000, stdin) != 0x1000) return 2;
    memcpy(mem + 0x10000, code, len);
    b86_init(&c, mem);
    c.model = B86_CPU_386;
    c.amask = 0x1FFFFFu;
    for (int i = 0; i < 8; i++) c.r[i] = regs[i];
    c.flags = (regs[8] & 0x00000CD5u) | 0x2u;
    c.msw = 0x11;              /* PE + ET */
    int code16 = getenv("CODE16") != NULL;
    static const int segs[6] = {0,1,2,3,4,5};
    for (int i = 0; i < 6; i++) {
        B86PM1Cache *k = &c.pm386.seg[segs[i]];
        k->base = 0; k->limit = 0xFFFFFFFFu; k->valid = 1; k->reserved = 0;
        k->flags = code16 ? 0u : (4u | 8u);
        if (code16) { k->base = i == B86_CS ? 0x10000u : 0x20000u; k->limit = 0xFFFFu; }
        if (i == B86_CS) { k->selector = 8; k->access = 0x9B; }
        else { k->selector = 0x10; k->access = 0x93; }
    }
    c.seg[B86_ES] = c.seg[B86_SS] = c.seg[B86_DS] = 0x10; c.seg[B86_CS] = 8;
    c.seg386[0] = c.seg386[1] = 0x10;
    c.gdt_base = 0x1000; c.gdt_limit = 23;
    static const uint8_t gdt[24] = {0,0,0,0,0,0,0,0, 0xFF,0xFF,0,0,0,0x9B,0xCF,0, 0xFF,0xFF,0,0,0,0x93,0xCF,0};
    memcpy(mem + 0x1000, gdt, 24);
    c.ip = code16 ? 0 : 0x10000;
    uint32_t end = code16 ? len : 0x10000 + len;
    int rc = B86_OK, steps = 0;
    while (c.ip != end && steps < 100000) {
        rc = b86_step(&c);
        if (rc != B86_OK) break;
        steps++;
    }
    uint32_t out[11];
    out[0] = (uint32_t)rc; for (int i = 0; i < 8; i++) out[1+i] = c.r[i];
    b86_flags_materialize(&c);
    out[9] = c.flags; out[10] = code16 ? c.ip + 0x10000 : c.ip;
    fwrite(out, 4, 11, stdout);
    fwrite(mem + 0x20000, 1, 0x1000, stdout);
    return 0;
}

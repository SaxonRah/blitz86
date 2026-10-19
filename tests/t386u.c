/* r56: targeted 386 unit test - one instruction (+HLT) with random 32-bit
   registers and flags, interpreter vs JIT (hot threshold 0). Usage:
   t386u "66 01 E8" [iters] */
#include "b86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef B86_PAGED
static uint8_t mem_raw[0xA0000u + B86_MEM_BYTES] __attribute__((aligned(0x200000)));
#define mem (mem_raw + 0xA0000u)
#else
static uint8_t mem[B86_MEM_BYTES] __attribute__((aligned(64)));
#endif
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
int main(int argc, char **argv)
{
    uint8_t code[32]; int n = 0;
    for (char *p = argv[1]; *p; ) { while (*p == ' ') p++; if (!*p) break; code[n++] = (uint8_t)strtoul(p, &p, 16); }
    code[n++] = 0xF4;
    int iters = argc > 2 ? atoi(argv[2]) : 2000, bad = 0;
    void *buf = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static uint8_t snap[B86_MEM_BYTES];
    B86Cpu a, b;
    b86_init(&b, mem); b.model = B86_CPU_386;
    struct B86Jit *j = b86_jit_create(&b, buf, 1 << 20);
    b86_jit_set_hot_threshold(j, 0);
    for (int it = 0; it < iters; ++it) {
        uint32_t r[8], fl = rnd() & 0x0FD5u;
        for (int i = 0; i < 8; ++i) r[i] = rnd();
        r[B86_SP] = 0x8000u + (rnd() & 0x3FFCu);
        for (int i = 0x20000; i < 0x40000; i += 4) { uint32_t v = rnd(); memcpy(mem + i, &v, 4); }
        memcpy(mem + 0x10000, code, (size_t)n);
        memcpy(snap, mem, B86_MEM_BYTES);
        b86_init(&a, snap); a.model = B86_CPU_386;
        for (int i = 0; i < 8; ++i) a.r[i] = r[i];
        b86_set_seg(&a, B86_CS, 0x1000); b86_set_seg(&a, B86_DS, 0x2000); b86_set_seg(&a, B86_ES, 0x3000); b86_set_seg(&a, B86_SS, 0x3000);
        a.ip = 0; b86_set_flags(&a, (uint16_t)fl);
        int ra = b86_run_interp(&a, 10);
        b86_jit_flush(j);
        for (int i = 0; i < 8; ++i) b.r[i] = r[i];
        b86_set_seg(&b, B86_CS, 0x1000); b86_set_seg(&b, B86_DS, 0x2000); b86_set_seg(&b, B86_ES, 0x3000); b86_set_seg(&b, B86_SS, 0x3000);
        b.ip = 0; b86_set_flags(&b, (uint16_t)fl); b.irq = 0;
        int rb = b86_jit_run(&b, 10);
        int ok = ra == B86_HALT && rb == B86_HALT && (uint16_t)a.ip == (uint16_t)b.ip;
        for (int i = 0; i < 8; ++i) if (a.r[i] != b.r[i]) ok = 0;
        uint16_t fa = b86_get_flags(&a), fb2 = b86_get_flags(&b);
        if (fa != fb2) ok = 0;
        if (memcmp(snap, mem, B86_MEM_BYTES)) ok = 0;
        if (!ok && bad++ < 5) {
            printf("iter %d: rc %d/%d ip %04X/%04X flags %04X/%04X (in %04X)", it, ra, rb, (uint16_t)a.ip, (uint16_t)b.ip, fa, fb2, fl);
            for (int i = 0; i < 8; ++i) if (a.r[i] != b.r[i]) printf(" r%d %08X/%08X (in %08X)", i, a.r[i], b.r[i], r[i]);
            printf("\n");
        }
    }
    const B86JitStats *s = b86_jit_stats(j);
    printf("%s: %d/%d mismatches, native %llu of %llu\n", argv[1], bad, iters,
           (unsigned long long)(s->guest_insns - s->helper_insns), (unsigned long long)s->guest_insns);
    return bad != 0;
}

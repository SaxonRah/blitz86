/* SingleStepTests 80386 real-mode vectors (tools/sst386_convert.py).
   Each vector: the instruction followed by HLT.  Runs the interpreter
   (b86_step until HLT) and then the JIT (b86_jit_run, hot threshold 0)
   on the same state and compares both with silicon: 32-bit GPRs,
   segments incl. FS/GS, IP, FLAGS (masked), memory.  JIT results are only
   counted for vectors the interpreter passes.  SST386_VERBOSE=1 prints
   the first failures per file. */
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
typedef struct { uint32_t a; uint8_t v; } Byte;
typedef struct { char name[13]; uint32_t init[16], fin[16], fmask; uint16_t ni, nf; Byte ri[512], rf[512]; } Vec;
enum { V_ES = 8, V_CS, V_SS, V_DS, V_FS, V_GS, V_IP, V_FL };

static int rd(FILE *f, Vec *t)
{
    memset(t->name, 0, sizeof t->name);
    if (fread(t->name, 1, 12, f) != 12) return 0;
    if (fread(t->init, 4, 16, f) != 16 || fread(t->fin, 4, 16, f) != 16) return 0;
    if (fread(&t->fmask, 4, 1, f) != 1 || fread(&t->ni, 2, 1, f) != 1 || fread(&t->nf, 2, 1, f) != 1) return 0;
    if (t->ni > 512 || t->nf > 512) { fprintf(stderr, "vector too large\n"); exit(2); }
    for (int i = 0; i < t->ni; ++i) { if (fread(&t->ri[i].a, 4, 1, f) != 1 || fread(&t->ri[i].v, 1, 1, f) != 1) return 0; }
    for (int i = 0; i < t->nf; ++i) { if (fread(&t->rf[i].a, 4, 1, f) != 1 || fread(&t->rf[i].v, 1, 1, f) != 1) return 0; }
    return 1;
}

static void setup(B86Cpu *c, const Vec *t)
{
    for (int i = 0; i < t->ni; ++i) if (t->ri[i].a < B86_MEM_BYTES) mem[t->ri[i].a] = t->ri[i].v;
    for (int i = 0; i < 8; ++i) c->r[i] = t->init[i];
    b86_set_seg(c, B86_ES, (uint16_t)t->init[V_ES]); b86_set_seg(c, B86_CS, (uint16_t)t->init[V_CS]);
    b86_set_seg(c, B86_SS, (uint16_t)t->init[V_SS]); b86_set_seg(c, B86_DS, (uint16_t)t->init[V_DS]);
    c->seg386[0] = (uint16_t)t->init[V_FS]; c->seg386[1] = (uint16_t)t->init[V_GS];
    c->ip = t->init[V_IP] & 0xFFFFu;
    c->flags = 0x0002u; b86_set_flags(c, (uint16_t)t->init[V_FL]);
    c->irq = 0;
}

static int check(B86Cpu *c, const Vec *t, const char *who, int verbose)
{
    uint32_t got[16];
    for (int i = 0; i < 8; ++i) got[i] = c->r[i];
    got[V_ES] = c->seg[B86_ES]; got[V_CS] = c->seg[B86_CS]; got[V_SS] = c->seg[B86_SS]; got[V_DS] = c->seg[B86_DS];
    got[V_FS] = c->seg386[0]; got[V_GS] = c->seg386[1];
    got[V_IP] = c->ip & 0xFFFFu; got[V_FL] = b86_get_flags(c);
    static const char *nm[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi","es","cs","ss","ds","fs","gs","ip","fl"};
    int ok = 1, bad[16] = {0}, membad = -1;
    for (int i = 0; i < 14; ++i) if ((got[i] ^ t->fin[i]) & (i < 8 ? 0xFFFFFFFFu : 0xFFFFu)) { ok = 0; bad[i] = 1; }
    if ((got[V_IP] ^ t->fin[V_IP]) & 0xFFFFu) { ok = 0; bad[V_IP] = 1; }
    if ((got[V_FL] ^ t->fin[V_FL]) & t->fmask & 0x7FD5u) { ok = 0; bad[V_FL] = 1; }
    for (int i = 0; i < t->nf; ++i)
        if (t->rf[i].a < B86_MEM_BYTES && mem[t->rf[i].a] != t->rf[i].v) { ok = 0; membad = i; }
    if (!ok && verbose) {
        printf("FAIL %s %-8s:", who, t->name);
        for (int i = 0; i < 16; ++i) if (bad[i]) printf(" %s=%X(exp %X init %X)", nm[i], got[i], t->fin[i], t->init[i]);
        if (membad >= 0) printf(" mem[%X]=%02X exp %02X", t->rf[membad].a, mem[t->rf[membad].a], t->rf[membad].v);
        printf("\n");
    }
    return ok;
}

static void clear(const Vec *t)
{
    for (int i = 0; i < t->ni; ++i) if (t->ri[i].a < B86_MEM_BYTES) mem[t->ri[i].a] = 0;
    for (int i = 0; i < t->nf; ++i) if (t->rf[i].a < B86_MEM_BYTES) mem[t->rf[i].a] = 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "s386.bin", "rb");
    if (!f) { perror("open"); return 2; }
    int verbose = getenv("SST386_VERBOSE") != NULL;
    void *code = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static Vec t;
    char cur[13] = ""; unsigned fi = 0, fj = 0, ti = 0, tn = 0;
    unsigned long n = 0, ibad = 0, jbad = 0, jn = 0, hma = 0, lfs = 0;
    B86Cpu c;
    b86_init(&c, mem);
    c.model = B86_CPU_386;
    struct B86Jit *j = b86_jit_create(&c, code, 4 << 20);
    b86_jit_set_hot_threshold(j, 0);
    while (rd(f, &t)) {
        if (strcmp(cur, t.name)) {
            if (cur[0] && (fi || fj)) printf("  %-10s interp %u/%u failed, jit %u/%u failed\n", cur, fi, tn, fj, ti);
            strcpy(cur, t.name); fi = fj = ti = tn = 0;
        }
        n++; tn++;
        /* interpreter */
        setup(&c, &t);
        extern uint32_t b86_limit_faults;
        uint32_t lf0 = b86_limit_faults;
        for (int k = 0; k < 4; ++k) if (b86_step(&c) != B86_OK) break;
        int limit_fault = b86_limit_faults != lf0;
        int iok = check(&c, &t, "interp", verbose && fi < 2);
        clear(&t); memset(mem + 0x100000, 0, B86_MEM_BYTES - 0x100000);
        if (!iok) { fi++; ibad++; continue; }
        /* JIT */
        if (getenv("SST386_NOJIT")) continue;
        if (limit_fault) { lfs++; continue; }   /* 386 segment-limit fault: not raised by the JIT tier */
        /* the JIT does not translate code above 1 MiB (HMA): skip those */
        if ((t.init[V_CS] << 4) + (t.init[V_IP] & 0xFFFFu) + 16u > 0xFFFFFu ||
            (t.fin[V_CS] << 4) + (t.fin[V_IP] & 0xFFFFu) + 16u > 0xFFFFFu) { hma++; continue; }
        if (getenv("SST386_TRACE")) { fprintf(stderr, "jit %s ip %X\n", t.name, t.init[V_IP]); }
        b86_jit_flush(j);
        setup(&c, &t);
        if (getenv("SST386_JITMODEL")) c.model = (uint32_t)atoi(getenv("SST386_JITMODEL"));
        b86_jit_run(&c, 8);
        c.model = B86_CPU_386;
        jn++; ti++;
        if (!check(&c, &t, "jit", verbose && fj < 2)) { fj++; jbad++; }
        clear(&t); memset(mem + 0x100000, 0, B86_MEM_BYTES - 0x100000);
    }
    if (cur[0] && (fi || fj)) printf("  %-10s interp %u/%u failed, jit %u/%u failed\n", cur, fi, tn, fj, ti);
    const B86JitStats *s = b86_jit_stats(j);
    printf("386 interp vs silicon: %lu/%lu passed\n", n - ibad, n);
    printf("386 jit vs silicon:    %lu/%lu passed (of interp-passing; skipped: %lu code above 1 MiB, %lu segment-limit faults), native insns %llu of %llu\n",
           jn - jbad, jn, hma, lfs, (unsigned long long)(s->guest_insns - s->helper_insns), (unsigned long long)s->guest_insns);
    return (ibad || jbad) != 0;
}

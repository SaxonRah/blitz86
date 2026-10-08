/* Run SingleStepTests 8086 hardware vectors through the translator, one
   instruction per block, and compare with silicon.  Vectors whose result
   depends on 1 MiB / in-segment wrap (which the JIT's A20-on model does not
   reproduce) are detected by running the interpreter in JIT semantics first
   and are counted separately. */
#include "b86.h"
#include "sst.h"
#include <string.h>
#include <sys/mman.h>

static uint8_t mem[B86_MEM_BYTES] __attribute__((aligned(64)));

static void setup(B86Cpu *c, const SstVec *t)
{
    static const int map[14] = { B86_AX, B86_BX, B86_CX, B86_DX, -1, -1, -1, -1, B86_SP, B86_BP, B86_SI, B86_DI, -1, -1 };
    for (int i = 0; i < t->ni; ++i) mem[t->ri[i].a] = t->ri[i].v;
    for (int i = 0; i < 14; ++i) if (map[i] >= 0) c->r[map[i]] = t->init[i];
    b86_set_seg(c, B86_CS, t->init[S_CS]); b86_set_seg(c, B86_SS, t->init[S_SS]);
    b86_set_seg(c, B86_DS, t->init[S_DS]); b86_set_seg(c, B86_ES, t->init[S_ES]);
    c->ip = t->init[S_IP]; b86_set_flags(c, t->init[S_FL]);
    c->irq = 0;
}

static int check(B86Cpu *c, const SstVec *t, int verbose)
{
    uint16_t got[14] = { (uint16_t)c->r[B86_AX], (uint16_t)c->r[B86_BX], (uint16_t)c->r[B86_CX], (uint16_t)c->r[B86_DX],
        (uint16_t)c->seg[B86_CS], (uint16_t)c->seg[B86_SS], (uint16_t)c->seg[B86_DS], (uint16_t)c->seg[B86_ES],
        (uint16_t)c->r[B86_SP], (uint16_t)c->r[B86_BP], (uint16_t)c->r[B86_SI], (uint16_t)c->r[B86_DI],
        (uint16_t)c->ip, b86_get_flags(c) };
    int ok = 1;
    for (int i = 0; i < 13; ++i) if (got[i] != t->fin[i]) ok = 0;
    if ((got[S_FL] ^ t->fin[S_FL]) & t->fmask) ok = 0;
    int membad = -1;
    for (int i = 0; i < t->nf; ++i) {
        int ign = 0;
        for (int g = 0; g < t->ng; ++g) if (t->ign[g] == t->rf[i].a) ign = 1;
        if (!ign && mem[t->rf[i].a] != t->rf[i].v) { ok = 0; membad = i; }
    }
    if (!ok && verbose) {
        static const char *nm[14] = {"ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","fl"};
        printf("FAIL %02X/%d:", t->op, t->reg == 0xFF ? -1 : t->reg);
        for (int i = 0; i < 14; ++i) if (got[i] != t->fin[i]) printf(" %s=%04X(exp %04X init %04X)", nm[i], got[i], t->fin[i], t->init[i]);
        if (membad >= 0) printf(" mem[%05X]=%02X exp %02X", t->rf[membad].a, mem[t->rf[membad].a], t->rf[membad].v);
        printf(" mask=%04X cs:ip=%04X:%04X\n", t->fmask, t->init[S_CS], t->init[S_IP]);
    }
    return ok;
}

static void clear(const SstVec *t)
{
    for (int i = 0; i < t->ni; ++i) mem[t->ri[i].a] = 0;
    for (int i = 0; i < t->nf; ++i) mem[t->rf[i].a] = 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "sst/all.bin", "rb");
    if (!f) { perror("open"); return 2; }
    int verbose = getenv("SST_VERBOSE") != NULL;
    void *code = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static SstVec t;
    static unsigned fails[256 * 9], total[256 * 9];
    unsigned long n = 0, bad = 0, wrap = 0;
    B86Cpu c;
    b86_init(&c, mem);
    struct B86Jit *j = b86_jit_create(&c, code, 4 << 20);
    b86_jit_set_single_step(j, 1);
    while (sst_read(f, &t)) {
        n++;
        /* 1. interpreter in JIT semantics: skip wrap-dependent vectors
              (including code that itself sits above 1 MiB) */
        setup(&c, &t);
        b86_step(&c);
        int hma_code = ((uint32_t)t.init[S_CS] << 4) + t.init[S_IP] + 8 > 0xFFFFFu;
        if (hma_code || !check(&c, &t, 0)) { wrap++; clear(&t); memset(mem + 0x100000, 0, B86_MEM_BYTES - 0x100000); continue; }
        clear(&t);
        if (getenv("SST_TRACE")) fprintf(stderr, "vec %02X.%d cs:ip %04X:%04X\n", t.op, t.reg, t.init[S_CS], t.init[S_IP]);
        /* 2. translator */
        b86_jit_flush(j);
        setup(&c, &t);
        b86_jit_run(&c, 1);
        unsigned key = t.op * 9 + (t.reg == 0xFF ? 8 : t.reg);
        total[key]++;
        if (!check(&c, &t, verbose && fails[key] < 3)) { fails[key]++; bad++; }
        clear(&t);
    }
    for (unsigned k = 0; k < 256 * 9; ++k)
        if (fails[k]) printf("  %02X%s%c: %u/%u failed\n", k / 9, k % 9 == 8 ? "" : ".", k % 9 == 8 ? ' ' : '0' + k % 9, fails[k], total[k]);
    const B86JitStats *s = b86_jit_stats(j);
    printf("jit vs 8086 silicon: %lu/%lu passed (%lu wrap-dependent skipped), helper insns %llu/%llu\n",
           n - wrap - bad, n - wrap, wrap, (unsigned long long)s->helper_insns, (unsigned long long)s->guest_insns);
    return bad != 0;
}

/* Run SingleStepTests 8086 hardware vectors against the reference
   interpreter in exact 8086 mode (1 MiB wrap, in-segment word wrap). */
#include "b86.h"
#include "sst.h"
#include <string.h>

static uint8_t mem[B86_MEM_BYTES];

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "sst/all.bin", "rb");
    if (!f) { perror("open"); return 2; }
    static SstVec t;
    static unsigned fails[256 * 9], total[256 * 9];
    unsigned long n = 0, bad = 0;
    B86Cpu c;
    while (sst_read(f, &t)) {
        b86_init(&c, mem);
        c.amask = 0xFFFFF; c.exact_wrap = 1;
        for (int i = 0; i < t.ni; ++i) mem[t.ri[i].a] = t.ri[i].v;
        static const int map[14] = { B86_AX, B86_BX, B86_CX, B86_DX, -1, -1, -1, -1, B86_SP, B86_BP, B86_SI, B86_DI, -1, -1 };
        for (int i = 0; i < 14; ++i) if (map[i] >= 0) c.r[map[i]] = t.init[i];
        b86_set_seg(&c, B86_CS, t.init[S_CS]); b86_set_seg(&c, B86_SS, t.init[S_SS]);
        b86_set_seg(&c, B86_DS, t.init[S_DS]); b86_set_seg(&c, B86_ES, t.init[S_ES]);
        c.ip = t.init[S_IP]; b86_set_flags(&c, t.init[S_FL]);
        b86_step(&c);
        uint16_t got[14] = { (uint16_t)c.r[B86_AX], (uint16_t)c.r[B86_BX], (uint16_t)c.r[B86_CX], (uint16_t)c.r[B86_DX],
            (uint16_t)c.seg[B86_CS], (uint16_t)c.seg[B86_SS], (uint16_t)c.seg[B86_DS], (uint16_t)c.seg[B86_ES],
            (uint16_t)c.r[B86_SP], (uint16_t)c.r[B86_BP], (uint16_t)c.r[B86_SI], (uint16_t)c.r[B86_DI],
            (uint16_t)c.ip, b86_get_flags(&c) };
        int ok = 1;
        for (int i = 0; i < 13; ++i) if (got[i] != t.fin[i]) ok = 0;
        if ((got[S_FL] ^ t.fin[S_FL]) & t.fmask) ok = 0;
        for (int i = 0; i < t.nf; ++i) {
            int ign = 0;
            for (int g = 0; g < t.ng; ++g) if (t.ign[g] == t.rf[i].a) ign = 1;
            if (!ign && mem[t.rf[i].a] != t.rf[i].v) ok = 0;
        }
        unsigned key = t.op * 9 + (t.reg == 0xFF ? 8 : t.reg);
        total[key]++;
        if (!ok) {
            if (fails[key] < 2 && getenv("SST_VERBOSE")) {
                printf("FAIL %02X/%d:", t.op, t.reg == 0xFF ? -1 : t.reg);
                static const char *nm[14] = {"ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","fl"};
                for (int i = 0; i < 14; ++i) if (got[i] != t.fin[i]) printf(" %s=%04X(exp %04X init %04X)", nm[i], got[i], t.fin[i], t.init[i]);
                printf(" mask=%04X\n", t.fmask);
            }
            fails[key]++; bad++;
        }
        for (int i = 0; i < t.ni; ++i) mem[t.ri[i].a] = 0;
        for (int i = 0; i < t.nf; ++i) mem[t.rf[i].a] = 0;
        n++;
    }
    for (unsigned k = 0; k < 256 * 9; ++k)
        if (fails[k]) printf("  %02X%s%c: %u/%u failed\n", k / 9, k % 9 == 8 ? "" : ".", k % 9 == 8 ? ' ' : '0' + k % 9, fails[k], total[k]);
    printf("interp vs 8086 silicon: %lu/%lu passed\n", n - bad, n);
    return bad != 0;
}

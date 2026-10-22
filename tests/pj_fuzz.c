/* PJ differential fuzzer: random flat-32 sequences, native block vs interpreter. */
#include "../src/interp.c"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void flat(B86Cpu *c)
{
    for (int s = 0; s < 6; ++s) { B86PM1Cache *d = &c->pm386.seg[s]; d->valid = 1; d->base = 0; d->limit = 0xFFFFFFFFu;
        d->access = s == B86_CS ? 0x9B : 0x93; d->flags = 0xC; d->selector = s == B86_CS ? 8 : 16; }
    c->seg[B86_CS] = 8; c->seg[B86_DS] = c->seg[B86_ES] = c->seg[B86_SS] = 16;
}
static uint8_t *P; static unsigned L;
static void b(uint32_t v) { P[L++] = (uint8_t)v; }
static void d32(uint32_t v) { b(v); b(v >> 8); b(v >> 16); b(v >> 24); }
static const unsigned dst[3] = { 0, 1, 2 }, base[4] = { 3, 5, 6, 7 }, r8d[6] = { 0, 1, 2, 4, 5, 6 };
static void modrm_any(unsigned reg, int allow_mem, int regdst)
{
    if (!allow_mem || rnd() % 2) { unsigned rm = regdst ? dst[rnd() % 3] : rnd() % 8; b(0xC0 | (reg << 3) | rm); return; }
    unsigned bs = base[rnd() % 4];
    switch (rnd() % 3) {
    case 0: b(0x40 | (reg << 3) | bs); b(rnd() & 0x7F); break;                     /* [base+disp8] */
    case 1: b(0x04 | (reg << 3)); b(((rnd() % 3) << 6) | (base[rnd() % 4] << 3) | bs); break;   /* [base+idx*s] */
    default: b(0x05 | (reg << 3)); d32(0x30000 + (rnd() & 0xFFF)); break;          /* [disp32] */
    }
}
static void gen_one(int last)
{
    unsigned k = rnd() % 32, o16 = rnd() % 4 == 0;
    if (last) {
        switch (rnd() % 4) {
        case 0: b(0xFF); modrm_any(2, 1, 0); if ((P[L-1] & 0xC0) == 0xC0 && L >= 2 && P[L-2] == 0xFF) P[L-1] = 0xD0 | (rnd() % 3); return;  /* CALL r/m32 */
        case 1: b(0xFF); modrm_any(4, 1, 0); if ((P[L-1] & 0xC0) == 0xC0 && L >= 2 && P[L-2] == 0xFF) P[L-1] = 0xE0 | (rnd() % 3); return;  /* JMP r/m32 */
        case 2: b(0xC3); return;
        default: b(0x70 | (rnd() & 15)); b(rnd() & 0x3F); return;
        }
    }
    if (rnd() % 16 == 0) { b(0xFF); modrm_any(6, 1, 0); if ((P[L-1] & 0xC0) == 0xC0 && L >= 2 && P[L-2] == 0xFF) P[L-1] = 0xF0 | (rnd() % 8); b(0x58 + dst[rnd() % 3]); return; }  /* PUSH r/m32; POP r */
    switch (k) {
    case 0: case 1: { unsigned alu = (unsigned[]){0,1,4,5,6,7}[rnd() % 6], form = rnd() % 4;
        if (o16 && (form & 1)) b(0x66);
        b((alu << 3) | form);
        if (form < 2) { /* r/m op= reg: register dst must be 0-2; for 8-bit avoid BH */
            unsigned reg = rnd() % 8; if (!(form & 1)) reg = r8d[rnd() % 6];
            modrm_any(reg, 1, 1); }
        else { unsigned reg = (form & 1) ? dst[rnd() % 3] : r8d[rnd() % 6]; modrm_any(0, 1, 0); P[L - ((P[L-1]==0)?1:1)] = P[L-1];
            /* rebuild: reg field = destination */
            L -= 0; }
        break; }
    case 2: { unsigned alu = (unsigned[]){0,1,4,5,6,7}[rnd() % 6], w = rnd() % 2; if (o16 && w) b(0x66);
        b((alu << 3) | 4 | w); if (!w) b(rnd()); else if (o16) { b(rnd()); b(rnd()); } else d32(rnd()); break; }
    case 3: { unsigned op = (unsigned[]){0x80,0x81,0x83}[rnd() % 3], reg = (unsigned[]){0,1,4,5,6,7}[rnd() % 6];
        if (o16 && op != 0x80) b(0x66);
        b(op); if (op == 0x80) { if (rnd() % 2) b(0xC0 | (reg << 3) | r8d[rnd() % 6]); else modrm_any(reg, 1, 1); } else modrm_any(reg, 1, 1);
        if (op == 0x81) { if (o16) { b(rnd()); b(rnd()); } else d32(rnd()); } else b(rnd()); break; }
    case 4: if (o16) b(0x66); b((rnd() % 2 ? 0x40 : 0x48) + dst[rnd() % 3]); break;
    case 5: { unsigned op = 0x88 + rnd() % 4; if (o16 && (op & 1)) b(0x66); b(op);
        if (op & 2) { unsigned reg = (op & 1) ? dst[rnd() % 3] : r8d[rnd() % 6]; modrm_any(reg, 1, 0); }
        else modrm_any((op & 1) ? rnd() % 8 : rnd() % 8, 1, 1);
        if ((op & 2) == 0 && (P[L-1] & 0xC0) == 0xC0 && !(op & 1)) { /* 8-bit reg dst rm: map BH->AH */ uint8_t m = P[L-1]; if ((m & 7) == 3 || (m & 7) == 7) P[L-1] = m & ~4; }
        break; }
    case 6: if (o16) b(0x66); b(0xB8 + dst[rnd() % 3]); if (o16) { b(rnd()); b(rnd()); } else d32(rnd()); break;
    case 7: b(0xB0 + r8d[rnd() % 6]); b(rnd()); break;
    case 8: if (o16) b(0x66); b(0x8D); modrm_any(dst[rnd() % 3], 1, 0); if ((P[L-1] & 0xC0) == 0xC0) { L--; b(0x40 | (dst[rnd()%3] << 3) | 3); b(rnd()); } break;
    case 9: { b(0x0F); unsigned op = (unsigned[]){0xB6,0xB7,0xBE,0xBF}[rnd() % 4]; b(op); modrm_any(dst[rnd() % 3], 1, 0);
        if ((P[L-1] & 0xC0) == 0xC0 && (op == 0xB6 || op == 0xBE)) { uint8_t m = P[L-1]; if ((m & 7) == 3 || (m & 7) == 7) P[L-1] = m & ~4; } break; }
    case 10: { unsigned op = (unsigned[]){0xC0,0xC1,0xD0,0xD1}[rnd() % 4], reg = (unsigned[]){4,5,7}[rnd() % 3];
        if (o16 && (op & 1)) b(0x66);
        b(op); if (op & 1) modrm_any(reg, 1, 1); else b(0xC0 | (reg << 3) | r8d[rnd() % 6]);
        if (op <= 0xC1) b(1 + rnd() % ((op & 1) ? (o16 ? 15 : 31) : 7)); break; }
    case 11: { unsigned op = 0xF6 + rnd() % 2, reg = (unsigned[]){0,2,3}[rnd() % 3]; if (o16 && (op & 1)) b(0x66);
        b(op); if (op & 1) modrm_any(reg, 1, 1); else b(0xC0 | (reg << 3) | r8d[rnd() % 6]);
        if (reg == 0) { if (op == 0xF6) b(rnd()); else if (o16) { b(rnd()); b(rnd()); } else d32(rnd()); } break; }
    case 12: b(0xF7); modrm_any(4 + rnd() % 2, 1, 1); if ((P[L-1] & 0xC0) == 0xC0) P[L-1] = (P[L-1] & ~7) | (rnd() % 8); break;
    case 13: b(0x0F); b(0xAF); modrm_any(dst[rnd() % 3], 1, 0); break;
    case 14: b(rnd() % 2 ? 0x69 : 0x6B); modrm_any(dst[rnd() % 3], 1, 0); if (P[L - 1 - 0] && P[0]) {} if (P[L-1]) {}
        if (P[L - 1] || 1) { if (P[L-1] == 0xFF) {} } if (P[L - 1] || 1) {} if (P[L-1] & 0) {}
        if (P[L - 1] || 1) { if (rnd() % 2) d32(rnd()); else b(rnd()); } break;
    case 15: b(0x0F); b(0x90 + rnd() % 16); if (rnd() % 2) b(0xC0 | r8d[rnd() % 6]); else modrm_any(0, 1, 1); break;
    case 16: if (o16) b(0x66); b(0x98 + rnd() % 2); break;
    case 17: b(0x50 + rnd() % 8); b(0x58 + dst[rnd() % 3]); break;
    case 18: b(rnd() % 2 ? 0x68 : 0x6A); if (P[L-1] == 0x68) d32(rnd()); else b(rnd()); b(0x58 + dst[rnd() % 3]); break;
    case 19: { unsigned op = 0x84 + rnd() % 2; if (o16 && (op & 1)) b(0x66); b(op); modrm_any(rnd() % 8, 1, 1); break; }
    case 20: { unsigned op = 0xA0 + rnd() % 4; if (o16 && (op & 1)) b(0x66); b(op); d32(0x30000 + (rnd() & 0xFFF)); break; }
    case 21: { /* DIV/IDIV with small divisors to avoid #DE most of the time */
        unsigned op = 0xF6 + rnd() % 2; if (o16 && (op & 1)) b(0x66);
        b(op); b(0xC0 | ((6 + rnd() % 2) << 3) | 3); break; }  /* divide by BL/BX/EBX (pointer: nonzero) */
    case 22: b(0xD3); modrm_any((unsigned[]){4,5,7}[rnd() % 3], 1, 1); break;
    case 23: b(rnd() % 2 ? 0x06 : 0x1E); b(0x58 + dst[rnd() % 3]); break;
    case 24: { unsigned alu = 2 + rnd() % 2, form = rnd() % 4; if (o16 && (form & 1)) b(0x66);   /* ADC/SBB */
        b((alu << 3) | form);
        if (form < 2) modrm_any((form & 1) ? rnd() % 8 : r8d[rnd() % 6], 1, 1);
        else modrm_any((form & 1) ? dst[rnd() % 3] : r8d[rnd() % 6], 1, 0);
        if ((form & 2) && (P[L-1] & 0xC0) == 0xC0) { uint8_t mm = P[L-1]; P[L-1] = (mm & 0xC7) | (((form & 1) ? dst[rnd()%3] : r8d[rnd()%6]) << 3); }
        break; }
    case 25: b(0x83); modrm_any(2 + rnd() % 2, 1, 1); b(rnd()); break;
    case 26: b(0x90 + 1 + rnd() % 2); break;                 /* XCHG eax, ecx/edx */
    case 27: { unsigned op = 0x86 + rnd() % 2; b(op); modrm_any(dst[rnd() % 3], 1, 1); break; }
    case 28: b(0x8C); modrm_any(rnd() % 4, 1, 1); break;
    case 29: { /* string op on the data area with small counts */
        b(0xB9); d32(rnd() % 9);                              /* mov ecx, 0..8 */
        b(0xBE); d32(0x28000 + (rnd() & 0x3FC)); b(0xBF); d32(0x2A000 + (rnd() & 0x3FC));
        if (rnd() % 4 == 0) b(0xFD); else b(0xFC);          /* STD / CLD (interpreter-declined: ends block) */
        if (rnd() % 2) b(0xF3);
        if (rnd() % 3 == 0 && o16) b(0x66);
        b((unsigned[]){0xA4,0xA5,0xAA,0xAB}[rnd() % 4]);
        break; }
    case 30: b(0x26); b(0x8B); modrm_any(dst[rnd() % 3], 1, 0); if ((P[L-1] & 0xC0) == 0xC0) { L--; b(0x40 | (dst[rnd()%3] << 3) | 3); b(rnd() & 0x7F); } break;
    default: { unsigned op = 0xFE + rnd() % 2; if (o16 && (op & 1)) b(0x66); b(op); modrm_any(rnd() % 2, 1, 1); break; }
    }
}
int main(int argc, char **argv)
{
    static B86Cpu C, D;
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 1000, bad = 0, nat = 0, tot = 0, skipped = 0;
    if (argc > 2) rs = (uint32_t)atoi(argv[2]);
    uint8_t *M = calloc(1, B86_RAM_BYTES + 64), *M2 = calloc(1, B86_RAM_BYTES + 64), *M0 = calloc(1, B86_RAM_BYTES + 64);
    void *code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *st = calloc(1, b86_pj_state_bytes());
    for (unsigned it = 0; it < iters; ++it) {
        memset(&C, 0, sizeof C);
        b86_init(&C, M); C.model = B86_CPU_386; C.msw = 1; flat(&C);
        b86_pj_attach(&C, st, b86_pj_state_bytes(), code, 1 << 20);
        for (unsigned a = 0x20000; a < 0x32000; ++a) M[a] = (uint8_t)rnd();
        memset(M + 0x1000, 0x90, 512);
        P = M + 0x1000; L = 0;
        unsigned n = 1 + rnd() % 12;
        for (unsigned i = 0; i < n; ++i) gen_one(i == n - 1 && rnd() % 2);
        C.ip = 0x1000; C.r[0] = rnd(); C.r[1] = rnd(); C.r[2] = rnd(); C.r[4] = 0x8000;
        C.r[3] = 0x20000 + (rnd() & 0xFFF); C.r[5] = 0x22000 + (rnd() & 0xFFF); C.r[6] = 0x24000 + (rnd() & 0xFFF); C.r[7] = 0x26000 + (rnd() & 0xFFF);
        C.flags = 2 | (rnd() & 0x8D5);
        memcpy(M2, M, B86_RAM_BYTES); memcpy(M0, M, 0x40000);
        D = C; D.mem = M2; D.pj = NULL;
        unsigned nn = b86_pj_try(&C);
        tot += n; nat += nn;
        if (!nn) continue;
        b86_flags_materialize(&C);
        { unsigned k; for (k = 0; k < nn; ++k) if (b86_step(&D) != B86_OK) break; if (k < nn) { ++skipped; continue; } }
        b86_flags_materialize(&D);
        int bad1 = memcmp(C.r, D.r, sizeof C.r) || C.ip != D.ip || ((C.flags ^ D.flags) & ~(uint32_t)B86_AF & 0xFFF) ||
                   memcmp(M + 0x20000, M2 + 0x20000, 0x20000) || memcmp(M + 0x7000, M2 + 0x7000, 0x1000);
        if (bad1 && ++bad <= 12) {
            printf("MISMATCH it=%u n=%u code:", it, nn); for (unsigned q = 0; q < L; ++q) printf("%02X", M0[0x1000 + q]); printf("\n");
            printf("  native eip=%08X fl=%04X", C.ip, C.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", C.r[i]); printf("\n");
            printf("  interp eip=%08X fl=%04X", D.ip, D.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", D.r[i]); printf("\n");
            if (memcmp(M + 0x20000, M2 + 0x20000, 0x20000) || memcmp(M + 0x7000, M2 + 0x7000, 0x1000)) printf("  memory differs\n");
        }
    }
    printf("fuzz: %u sequences, %u native instructions, %u interpreter-refused, %u mismatches\n", iters, nat, skipped, bad);
    return bad != 0;
}

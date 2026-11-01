/* PJ differential fuzzer: random flat-32 sequences, native block vs interpreter. */
#define PJ_V103_TEST 1   /* V103: lets mode 26 switch the bulk string path */
#include "../src/interp.c"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
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

/* V81 harness: forms added by V57..V80 (and the V81 gap fills). Each case
 * keeps addresses inside the compared data/stack windows. */
static void sreg(unsigned r, uint32_t v) { b(0xB8 + r); d32(v); }       /* MOV r32, imm32 */
static void ptr_setup(void)
{
    sreg(1, rnd() % 7);                                                  /* ECX 0..6 */
    sreg(6, 0x28000 + (rnd() & 0x3FC) + (rnd() % 4 == 0 ? rnd() % 4 : 0));
    sreg(7, 0x2A000 + (rnd() & 0x3FC) + (rnd() % 4 == 0 ? rnd() % 4 : 0));
    b(rnd() % 4 == 0 ? 0xFD : 0xFC);                                     /* STD / CLD */
}
static void gen_new(int last)
{
    unsigned o16 = rnd() % 4 == 0;
    if (last) {
        switch (rnd() % 3) {
        case 0: b(0xE0 + rnd() % 4); b(rnd() & 0x3F); return;          /* LOOPNE/LOOPE/LOOP/JECXZ */
        case 1: b(0xC3); return;
        default: b(0x70 | (rnd() & 15)); b(rnd() & 0x3F); return;
        }
    }
    switch (rnd() % 24) {
    case 21: switch (rnd() % 3) { case 0: b(0x60); break; case 1: b(0x61); break; default: b(0x60); b(0x58 + dst[rnd() % 3]); b(0x61); } break;  /* PUSHAD/POPAD */
    case 22: sreg(1, rnd() % 4 == 0 ? rnd() % 1200 : 3 + rnd() % 120);         /* long REP MOVS/STOS: helper route */
             sreg(6, 0x28000 + (rnd() & 0x3FC)); sreg(7, 0x2C000 + (rnd() & 0x3FC)); b(0xFC);
             if (rnd() % 2) b(0xF3); else b(0xF2); if (rnd() % 4 == 0) b(0x66); b((unsigned[]){0xA4,0xA5,0xAA,0xAB}[rnd() % 4]); break;
    case 0: b(0x50 + dst[rnd() % 3]); b(0x8F); modrm_any(0, 1, 1); break;      /* PUSH r; POP r/m32 */
    case 1: sreg(3, 0x20000 + (rnd() & 0xF00)); b(0xD7); break;                /* XLAT */
    case 2: ptr_setup(); if (rnd() % 2) b(0xF3); if (o16) b(0x66);             /* [REP] LODS */
            b(0xAC + rnd() % 2); break;
    case 3: ptr_setup(); { unsigned r = rnd() % 3; if (r == 1) b(0xF3); else if (r == 2) b(0xF2); }
            if (o16) b(0x66); b((unsigned[]){0xA4,0xA5,0xAA,0xAB}[rnd() % 4]); break;   /* [REP] MOVS/STOS */
    case 4: ptr_setup(); { unsigned r = rnd() % 3; if (r == 1) b(0xF3); else if (r == 2) b(0xF2); }
            if (o16) b(0x66); b((unsigned[]){0xA6,0xA7,0xAE,0xAF}[rnd() % 4]); break;   /* [REPE/NE] CMPS/SCAS */
    case 5: case 6: { unsigned op = (unsigned[]){0xC0,0xC1,0xD0,0xD1,0xD2,0xD3}[rnd() % 6], reg = rnd() % 4;   /* ROL/ROR/RCL/RCR */
        if (op >= 0xD2) { b(0xB1); b(rnd() % 4 == 0 ? rnd() : rnd() % 40); }     /* MOV CL, imm8 */
        if (o16 && (op & 1)) b(0x66);
        b(op); if (op & 1) modrm_any(reg, 1, 1); else if (rnd() % 2) b(0xC0 | (reg << 3) | r8d[rnd() % 6]); else modrm_any(reg, 1, 1);
        if (op <= 0xC1) b(rnd() % 4 == 0 ? rnd() : rnd() % 40); break; }
    case 7: { unsigned op = 0xD2 + rnd() % 2, reg = (unsigned[]){4,5,7}[rnd() % 3];   /* narrow CL shifts */
        b(0xB1); b(rnd() % 4 == 0 ? rnd() : rnd() % 20);
        if (op == 0xD3) b(0x66);
        b(op); if (rnd() % 2) b(0xC0 | (reg << 3) | (op == 0xD2 ? r8d[rnd() % 6] : dst[rnd() % 3])); else modrm_any(reg, 1, 1); break; }
    case 8: { unsigned op = (unsigned[]){0xA3,0xAB,0xB3,0xBB}[rnd() % 4], idx = dst[rnd() % 3];  /* BT/BTS/BTR/BTC r/m, r */
        sreg(idx, rnd() % 4 == 0 ? rnd() : rnd() % 300 - (rnd() % 2 ? 100 : 0));
        if (o16) b(0x66);
        b(0x0F); b(op); modrm_any(idx, 1, 1); break; }
    case 9: { unsigned reg = 4 + rnd() % 4; if (o16) b(0x66);                        /* 0F BA /4..7 imm8 */
        b(0x0F); b(0xBA); modrm_any(reg, 1, 1); b(rnd()); break; }
    case 10: if (o16) b(0x66); b(0x0F); b(0xBC + rnd() % 2); modrm_any(dst[rnd() % 3], 1, 0);   /* BSF/BSR */
        if ((P[L-1] & 0xC0) == 0xC0 && rnd() % 3 == 0) { /* zero source */ } break;
    case 11: { unsigned op = (unsigned[]){0xA4,0xA5,0xAC,0xAD}[rnd() % 4];          /* SHLD/SHRD */
        if (op & 1) { b(0xB1); b(rnd() % 4 == 0 ? rnd() : rnd() % 40); }
        if (o16) b(0x66);
        b(0x0F); b(op); modrm_any(rnd() % 8, 1, 1); if (!(op & 1)) b(rnd() % 4 == 0 ? rnd() : rnd() % 40); break; }
    case 12: b((unsigned[]){0xF8,0xF9,0xF5,0x9E,0x9F}[rnd() % 5]); break;            /* CLC/STC/CMC/SAHF/LAHF */
    case 13: if (rnd() % 2) { if (o16) b(0x66); b(0x9C); b(0x58 + dst[rnd() % 3]); }  /* PUSHF[D]; POP r */
             else { b(0x68); d32((rnd() & 0x0CD5u) | 2u); if (o16) { b(0x66); b(0x9D); b(0x66); b(0x9D); } else b(0x9D); }  /* POPF[D] */
             break;
    case 14: { unsigned op = 0xF6 + rnd() % 2; if (o16 && (op & 1)) b(0x66);          /* MUL/IMUL r/m */
        b(op); if (op & 1) modrm_any(4 + rnd() % 2, 1, 1); else if (rnd() % 2) b(0xC0 | ((4 + rnd() % 2) << 3) | r8d[rnd() % 6]); else modrm_any(4 + rnd() % 2, 1, 1); break; }
    case 15: { unsigned op = 0xF6 + rnd() % 2, sg = rnd() % 2;                         /* DIV/IDIV r/m (mostly no #DE) */
        if (op == 0xF7 && !o16) { b(0x99); if (!sg) { b(0x31); b(0xD2); } }           /* CDQ / XOR EDX,EDX */
        if (op == 0xF7 && o16) { b(0x66); b(0x99); if (!sg) { b(0x66); b(0x31); b(0xD2); } b(0x66); }
        b(op); if (rnd() % 2) b(0xC0 | ((6 + sg) << 3) | 3); else modrm_any(6 + sg, 1, 1); break; }
    case 16: { unsigned n = rnd() % 3 == 0 ? 0 : (rnd() & 0x3C); b(0xC8); b(n); b(0); b(0x89); b(0x45); b(0xFC);  /* ENTER n,0; MOV [EBP-4],EAX */
        if (rnd() % 2) b(0x66); b(0xC9); break; }                                     /* [66] LEAVE */
    case 17: b(0x50 + dst[rnd() % 3]); b(0x8F); b(0xC0 | dst[rnd() % 3]); break;      /* POP r32 via 8F /0 */
    case 18: if (o16) b(0x66); b(0x8F); b(0x04); b(0x24); break;                      /* POP [ESP] (ESP-relative order) */
    case 19: { unsigned op = 0xF6 + rnd() % 2; if (o16 && (op & 1)) b(0x66);          /* NOT/NEG */
        b(op); modrm_any(2 + rnd() % 2, 1, 1); break; }
    case 20: b(0x0F); b(0xC8 + dst[rnd() % 3]); break;                                /* BSWAP */
    default: { unsigned op = 0x0F; b(op); b((unsigned[]){0xA3,0xBC,0xBD,0xB6,0xBF}[rnd() % 5]); modrm_any(dst[rnd() % 3], 1, 0); break; }
    }
}
/* V82 harness: 16-bit protected mode (DOS/4GW kernel style). CS base 0x10000
 * (16-bit, CPL0); DS=SS base 0x20000, ES base 0x30000, FS base 0x24000 (limit
 * 0x7FFF), GS=ES; GDT at linear 0x500. All data lies in the compared window. */
static uint32_t g_csbase;
static void gdt_put(uint8_t *M, unsigned sel, uint32_t base, uint32_t limit, unsigned access, unsigned flags)
{
    uint8_t *d = M + 0x500 + (sel & ~7u);
    d[0] = (uint8_t)limit; d[1] = (uint8_t)(limit >> 8); d[2] = (uint8_t)base; d[3] = (uint8_t)(base >> 8);
    d[4] = (uint8_t)(base >> 16); d[5] = (uint8_t)access; d[6] = (uint8_t)(((limit >> 16) & 15u) | (flags << 4)); d[7] = (uint8_t)(base >> 24);
}
static void seg16(B86Cpu *c, int s, unsigned sel, uint32_t base, uint32_t limit, unsigned access)
{
    B86PM1Cache *d = &c->pm386.seg[s];
    d->valid = 1; d->base = base; d->limit = limit; d->access = (uint8_t)access; d->flags = 0; d->selector = (uint16_t)sel;
    if (s < 4) c->seg[s] = (uint16_t)sel; else c->seg386[s - 4] = (uint16_t)sel;
}
static void pm16(B86Cpu *c, uint8_t *M)
{
    memset(M + 0x500, 0, 0x40);
    gdt_put(M, 0x08, 0x10000, 0xFFFF, 0x9B, 0);
    gdt_put(M, 0x10, 0x20000, 0xFFFF, 0x93, 0);
    gdt_put(M, 0x18, 0x30000, 0xFFFF, 0x93, 0);
    gdt_put(M, 0x20, 0x24000, 0x7FFF, 0x93, 0);
    c->gdt_base = 0x500; c->gdt_limit = 0x27;
    seg16(c, B86_CS, 0x08, 0x10000, 0xFFFF, 0x9B);
    seg16(c, B86_DS, 0x10, 0x20000, 0xFFFF, 0x93);
    seg16(c, B86_SS, 0x10, 0x20000, 0xFFFF, 0x93);
    seg16(c, B86_ES, 0x18, 0x30000, 0xFFFF, 0x93);
    seg16(c, 4, 0x20, 0x24000, 0x7FFF, 0x93);   /* FS */
    seg16(c, 5, 0x18, 0x30000, 0xFFFF, 0x93);   /* GS */
}
static const unsigned sels[4] = { 0x10, 0x18, 0x20, 0x00 };
static void gen_pm16(int last)
{
    if (last) {
        switch (rnd() % 4) {
        case 0: b(0xC3); return;                                              /* RET (near, 16-bit) */
        case 1: b(0xE0 + rnd() % 4); b(rnd() & 0x3F); return;
        case 2: { const unsigned at = 0x1000 + L; b(0x0E); b(0x68); b((at + 5) & 0xFF); b((at + 5) >> 8); b(0xCB); return; }  /* PUSH CS; PUSH ip; RETF */
        default: b(0x70 | (rnd() & 15)); b(rnd() & 0x3F); return;
        }
    }
    switch (rnd() % 31) {
    case 29: { static const unsigned acc[7] = { 0x04, 0x0C, 0x24, 0x2C, 0x34, 0x3C, 0xA8 };     /* V92: AL/AX, imm */
        const unsigned o = acc[rnd() % 7] | (rnd() % 2);
        b(o); b(rnd()); if (o & 1u) b(rnd()); break; }
    case 30: { static const unsigned pf[7] = { 0, 0, 0x26, 0x36, 0x3E, 0x64, 0x65 };           /* V92: A0-A3 moffs16 */
        static const unsigned offs[6] = { 0x0000, 0x1234, 0x7FFE, 0x7FFF, 0x8000, 0xFFFF };
        const unsigned p = pf[rnd() % 7], o = 0xA0 + rnd() % 4, off = rnd() % 3 ? (rnd() & 0x0FFE) : offs[rnd() % 6];
        if (p) b(p); b(o); b(off & 0xFF); b(off >> 8); break; }
    case 27: { unsigned n = rnd() % 4 == 0 ? 0 : (rnd() & 0x3E); b(0xC8); b(n); b(0); b(0); if (rnd() % 2) b(0xC9); break; }   /* V85: ENTER n,0 [LEAVE] */
    case 28: { static const unsigned pf[6] = { 0, 0x26, 0x36, 0x3E, 0x64, 0x65 }; unsigned p = pf[rnd() % 6];            /* V85: PUSH x; POP m16 */
        b(0x50 + rnd() % 8); if (p) b(p); b(0x8F); modrm_any(0, 1, 1); break; }
    case 24: { unsigned k = rnd() % 3;                                                                      /* V84: LEAVE */
        if (k == 0) { b(0xBD); b(rnd() & 0xFE); b(0x80 + rnd() % 64); }                                    /* MOV BP, imm16 */
        else if (k == 1) { b(0x55); b(0x89); b(0xE5); b(0x83); b(0xEC); b(rnd() & 0x1E); }                  /* PUSH BP; MOV BP,SP; SUB SP,n */
        b(0xC9); break; }
    case 25: { static const unsigned pf[6] = { 0, 0x26, 0x36, 0x3E, 0x64, 0x65 }; unsigned p = pf[rnd() % 6];   /* V84: 8C mem, FF /6 */
        if (p) b(p);
        if (rnd() % 2) { b(0x8C); modrm_any(rnd() % 6, 1, 1); } else { b(0xFF); modrm_any(6, 1, 1); } break; }
    case 26: { static const unsigned pf[6] = { 0, 0x26, 0x36, 0x3E, 0x64, 0x65 }; unsigned p = pf[rnd() % 6];   /* V84: LES/LDS via far pointer at [BX] */
        static const unsigned base[6] = { 0x20000, 0x30000, 0x20000, 0x20000, 0x24000, 0x30000 };
        (void)base;
        if (p) b(p); b(0xC7); b(0x07); b(rnd()); b(rnd());                                                  /* MOV [BX], off */
        if (p) b(p); b(0xC7); b(0x47); b(0x02); b(sels[rnd() % 4]); b(0);                                    /* MOV [BX+2], sel */
        if (p) b(p); b(rnd() % 2 ? 0xC4 : 0xC5); b(0x07 | ((rnd() % 8) << 3)); break; }
    case 20: case 21: { static const unsigned pf[6] = { 0, 0x26, 0x36, 0x3E, 0x64, 0x65 };   /* V83: 66 (+seg, either order) memory forms */
        unsigned p = pf[rnd() % 6], k = rnd() % 9;
        if (p && rnd() % 2) { b(p); b(0x66); } else { b(0x66); if (p) b(p); }
        switch (k) {
        case 0: b(((rnd() % 8) << 3) | (rnd() % 2 ? 1 : 3)); modrm_any(rnd() % 8, 1, 0); break;
        case 1: b(rnd() % 2 ? 0x81 : 0x83); modrm_any(rnd() % 8, 1, 1); b(rnd()); b(rnd()); b(rnd()); b(rnd()); break;
        case 2: b((unsigned[]){0x85,0x89,0x8B}[rnd() % 3]); modrm_any(rnd() % 8, 1, 0); break;
        case 3: b(0xC7); modrm_any(0, 1, 1); d32(rnd()); break;
        case 4: b(0xFF); modrm_any(rnd() % 2, 1, 1); break;
        case 5: b(0xF7); modrm_any((unsigned[]){0,2,3}[rnd() % 3], 1, 1); d32(rnd()); break;
        case 6: b(rnd() % 2 ? 0xD1 : 0xC1); modrm_any((unsigned[]){0,1,2,3,4,5,7}[rnd() % 7], 1, 1); b(rnd() % 4 == 0 ? 1 : rnd() % 32); break;
        case 7: b(0x87); modrm_any(rnd() % 8, 1, 1); break;
        default: b(((unsigned[]){2,3}[rnd() % 2] << 3) | (rnd() % 2 ? 1 : 3)); b(0xC0 | (rnd() % 64)); break;   /* 66 ADC/SBB reg */
        } break; }
    case 22: { static const unsigned pf[4] = { 0, 0x26, 0x64, 0x36 }; unsigned p = pf[rnd() % 4];     /* V83: rotates by 1 */
        if (p) b(p);
        if (rnd() % 3 == 0) { b(rnd() % 2 ? 0xC0 : 0xC1); b(0xC0 | ((rnd() % 4) << 3) | (rnd() % 8)); b(rnd() % 4 ? 1 : 33); }
        else { if (rnd() % 4 == 0) b(0x66); b(rnd() % 2 ? 0xD0 : 0xD1); modrm_any(rnd() % 4, 1, 1); } break; }
    case 23: { static const unsigned pf[4] = { 0, 0x26, 0x65, 0x3E }; unsigned p = pf[rnd() % 4];     /* V83: XCHG */
        if (p) b(p); if (rnd() % 4 == 0) b(0x66); b(rnd() % 2 ? 0x86 : 0x87); modrm_any(rnd() % 8, 1, 1); break; }
    case 16: { unsigned r = rnd() % 8, q = rnd() % 3; b(0x50 + r); if (rnd() % 2) b(0x50 + rnd() % 8); b(0x58 + q); if (rnd() % 3 == 0) b(0x5C); break; }  /* PUSH/POP r16 (incl. SP) */
    case 17: { static const unsigned pf[7] = { 0, 0x26, 0x36, 0x3E, 0x64, 0x65, 0 }; unsigned p = pf[rnd() % 7]; if (p) b(p);       /* C6/C7, FE/FF, F6/F7 */
        switch (rnd() % 4) {
        case 0: b(0xC6 + rnd() % 2); modrm_any(0, 1, 1); b(rnd()); b(rnd()); break;
        case 1: b(0xFE + rnd() % 2); modrm_any(rnd() % 2, 1, 1); break;
        case 2: b(0xF6 + rnd() % 2); modrm_any((unsigned[]){0,2,3}[rnd() % 3], 1, 1); b(rnd()); b(rnd()); break;
        default: b(0x80 + (rnd() % 4 == 3 ? 3 : rnd() % 2)); modrm_any(2 + rnd() % 2, 1, 1); b(rnd()); b(rnd()); break;
        } break; }
    case 18: { static const unsigned pf[4] = { 0x26, 0x64, 0x65, 0x36 }; b(pf[rnd() % 4]);                                         /* override ALU/ADC/SBB mem */
        b(((rnd() % 8) << 3) | (rnd() % 4)); modrm_any(rnd() % 8, 1, 0); if ((P[L-1] & 0xC0) == 0xC0) P[L-1] &= 0x3F; break; }
    case 19: { b(0xB1); b(rnd() % 40); b(0xD2 + rnd() % 2); b(0xC0 | ((unsigned[]){4,5,7}[rnd() % 3] << 3) | (rnd() % 3)); break; }  /* CL shifts reg */
    case 0: case 1: { static const unsigned pf[6] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };   /* seg-override MOV */
        b(pf[rnd() % 6]); if (rnd() % 3 == 0) b(0x66); b((unsigned[]){0x88,0x89,0x8A,0x8B}[rnd() % 4]); modrm_any(rnd() % 8, 1, 0); break; }
    case 2: { static const unsigned pf[6] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };           /* seg-override ALU */
        b(pf[rnd() % 6]); b(((unsigned[]){0,1,4,5,6,7})[rnd() % 6] << 3 | (rnd() % 4)); modrm_any(rnd() % 8, 1, 0); break; }
    case 3: b((unsigned[]){0x06,0x1E,0x0E,0x16}[rnd() % 4]); b((unsigned[]){0x07,0x1F,0x58,0x59}[rnd() % 4]); break;  /* PUSH seg; POP seg/r */
    case 4: b(0x0F); b(rnd() % 2 ? 0xA0 : 0xA8); b(0x0F); b(rnd() % 2 ? 0xA1 : 0xA9); break;                      /* PUSH/POP FS/GS */
    case 5: { unsigned r = rnd() % 3, sr = (unsigned[]){0,3,4,5}[rnd() % 4];                                        /* MOV Sreg, r16 */
        b(0xB8 + r); b(sels[rnd() % 4]); b(0); b(0x8E); b(0xC0 | (sr << 3) | r); break; }
    case 6: b(0x8C); b(0xC0 | ((rnd() % 6) << 3) | (rnd() % 3)); break;                                               /* MOV r16, Sreg */
    case 7: { unsigned op = rnd() % 2 ? 0xC4 : 0xC5;                                                                   /* LES/LDS via a stored far pointer */
        b(0xC7); b(0x07); b(rnd()); b(rnd()); b(0xC7); b(0x47); b(0x02); b(sels[rnd() % 3]); b(0);
        b(op); b(0x07 | ((rnd() % 3) << 3)); break; }
    case 8: { unsigned op = (unsigned[]){0xD0,0xD1,0xD1,0xC1,0xD3}[rnd() % 5], reg = rnd() % 8;                     /* shifts/rotates */
        if (op == 0xD3) { b(0xB1); b(rnd() % 20); }
        if (rnd() % 4 == 0 && op != 0xD0) b(0x66);
        b(op); if (rnd() % 2) b(0xC0 | (reg << 3) | (rnd() % 3)); else modrm_any(reg, 1, 1);
        if (op == 0xC1) b(rnd() % 20); break; }
    case 9: { unsigned k = rnd() % 6; b(0x66);                                                                         /* 66-prefixed */
        switch (k) {
        case 0: b(0xB8 + rnd() % 3); d32(rnd()); break;
        case 1: b(0x50 + rnd() % 8); b(0x66); b(0x58 + rnd() % 3); break;
        case 2: b(((unsigned[]){0,1,4,5,6,7})[rnd() % 6] << 3 | (rnd() % 4)); modrm_any(rnd() % 3, 1, 1); break;
        case 3: b(0x0F); b((unsigned[]){0xB6,0xB7,0xBE,0xBF,0xAF}[rnd() % 5]); modrm_any(rnd() % 3, 1, 0); break;
        case 4: b(0xC1); b(0xC0 | ((unsigned[]){4,5,7,0,1}[rnd() % 5] << 3) | (rnd() % 3)); b(rnd() % 32); break;
        default: b(0x0F); b(rnd() % 2 ? 0xA4 : 0xAC); b(0xC0 | ((rnd() % 8) << 3) | (rnd() % 3)); b(rnd() % 32); break;
        } break; }
    case 10: b(0x0F); b(0x02); b(0xC0 | ((rnd() % 3) << 3) | 3); break;                                               /* LAR r16, BX */
    case 11: b(0x9C); b(0x58 + rnd() % 3); break;                                                                       /* PUSHF; POP r */
    case 12: b(0x0F); b(0x80 + rnd() % 16); b(rnd() & 0x1F); b(0); break;                                              /* Jcc rel16 */
    case 13: { unsigned k = rnd() % 3; if (k == 0) { b(0xE8); b(0); b(0); } else if (k == 1) { b(0x68); b(rnd()); b(rnd()); b(0x58 + rnd() % 3); } else { b(0xC8); b(4); b(0); b(0xC9); } break; }
    default: if (rnd() % 2) gen_new(0); else gen_one(0); break;
    }
}
static int gen_mode;
/* V93 mode 13: instructions the program generator must not place in a loop body:
 * control transfers (the program supplies its own), REP prefixes (retirement
 * counts), INT/IRET/POPF/HLT and far/indirect branches. */
/* V95: paging for modes 19/20. Page directory at 0x300000, one table at
 * 0x301000 covering 0..4 MiB: identity, except linear 0x20000-0x3FFFF ->
 * physical 0x220000 (extended memory) and, for mode 20, linear 0x1000 ->
 * physical 0x201000 (the program's code page in extended memory). */
static void pg_setup(B86Cpu *c, uint8_t *m, int code_ext)
{
    uint32_t *pd = (uint32_t *)(void *)(m + 0x300000), *pt = (uint32_t *)(void *)(m + 0x301000), i;
    memset(pd, 0, 4096);
    pd[0] = 0x301000u | 7u;
    for (i = 0; i < 1024u; ++i) {
        uint32_t ph = i << 12;
        if (i >= 0x20u && i < 0x40u) ph = (0x220u + (i - 0x20u)) << 12;
        if (code_ext && i == 1u) ph = 0x201000u;
        pt[i] = ph | 7u;
    }
    c->cr3 = 0x300000u; c->pg = 1u;
    memset(c->tlb_tag, 0, sizeof c->tlb_tag);
}
static int pj13_bad(const uint8_t *p, unsigned n)
{
    unsigned k = 0u, op;
    while (k < n && (p[k] == 0x66u || p[k] == 0x67u || p[k] == 0x26u || p[k] == 0x2Eu || p[k] == 0x36u ||
                     p[k] == 0x3Eu || p[k] == 0x64u || p[k] == 0x65u || p[k] == 0xF0u || p[k] == 0xF2u || p[k] == 0xF3u)) {
        if (p[k] == 0xF2u || p[k] == 0xF3u) return 1;
        ++k;
    }
    if (k >= n) return 1;
    op = p[k];
    if (op == 0x0Fu) return k + 1u >= n || (p[k + 1u] >= 0x80u && p[k + 1u] <= 0x8Fu);
    if ((op >= 0x70u && op <= 0x7Fu) || (op >= 0xE0u && op <= 0xE3u) || op == 0xE9u || op == 0xEBu || op == 0xEAu ||
        op == 0x9Au || op == 0xC2u || op == 0xC3u || op == 0xCAu || op == 0xCBu || op == 0xCCu || op == 0xCDu ||
        op == 0xCEu || op == 0xCFu || op == 0x9Du || op == 0xF4u || op == 0x6Cu || op == 0x6Du || op == 0x6Eu || op == 0x6Fu ||
        (op >= 0xA4u && op <= 0xA7u) || (op >= 0xAAu && op <= 0xAFu))
        return 1;
    if (op == 0xFFu && k + 1u < n) { const unsigned r = (p[k + 1u] >> 3) & 7u; if (r >= 2u && r <= 5u) return 1; }
    return 0;
}
static unsigned long m100_promotions, m100_hot_resets;   /* V100: summed over programs */
static unsigned long m13_programs, m13_dispatch, m13_retired, m13_bad, m13_unlinks, m13_smc, m13_evict, m21_act[7], run_to;
/* V94 mode 18: flat 32-bit native forms (register ALU/MOV, EAX and 83
 * immediates, MOV r32,imm32) for self-modifying flat programs. */
static void gen18(void)
{
    static const unsigned alu[6] = { 0x01, 0x09, 0x21, 0x29, 0x31, 0x39 };
    switch (rnd() % 5) {
    case 0: b(alu[rnd() % 6] | (rnd() % 2 ? 2u : 0u)); b(0xC0 | (rnd() % 64)); if ((P[L - 1] & 0x07) == 4 || (P[L - 1] & 0x38) == 0x20) P[L - 1] = 0xC0 | (P[L - 1] & 0x3B); break;
    case 1: b((unsigned[]){0x05,0x0D,0x25,0x2D,0x35,0x3D}[rnd() % 6]); d32(rnd()); break;
    case 2: { unsigned r = rnd() % 8; if (r == 4) r = 3; b(0x83); b(0xC0 | ((unsigned[]){0,1,4,5,6,7}[rnd() % 6] << 3) | r); b(rnd()); break; }
    case 3: { unsigned d = rnd() % 8, sr = rnd() % 8; if (d == 4) d = 3; b(0x89); b(0xC0 | (sr << 3) | d); break; }
    default: { unsigned r = rnd() % 8; if (r == 4) r = 3; b(0xB8 + r); d32(rnd()); break; }
    }
}
/* V103 mode 25: flag producers (IMUL/MUL, logic, ADD/SUB/ADC/INC, CLC/STC)
 * followed by SHLD/SHRD (imm8 / CL, 16/32-bit, register and [EBX] memory). */
static void gen25(void)
{
    unsigned r = rnd() % 8, s2 = rnd() % 8;
    if (r == 4) r = 3;
    if (s2 == 4) s2 = 0;
    switch (rnd() % 12) {
    case 0: b(0x0F); b(0xAF); b(0xC0 | (r << 3) | s2); break;                     /* IMUL r32,r32 */
    case 1: b(0x6B); b(0xC0 | (r << 3) | s2); b(rnd()); break;                    /* IMUL r32,r32,imm8 */
    case 2: b(0xF7); b(0xC0 | ((rnd() % 2 ? 4u : 5u) << 3) | s2); break;           /* MUL/IMUL r32 */
    case 3: b((unsigned[]){0x21,0x09,0x31,0x85}[rnd() % 4]); b(0xC0 | (s2 << 3) | r); break;   /* logic */
    case 4: b((unsigned[]){0x01,0x29,0x11,0x19}[rnd() % 4]); b(0xC0 | (s2 << 3) | r); break;   /* ADD/SUB/ADC/SBB */
    case 5: b(0x40 + r); break;                                                   /* INC r32 */
    case 6: b((unsigned[]){0xF8,0xF9,0xF5}[rnd() % 3]); break;                    /* CLC/STC/CMC */
    case 7: b(0xB8 + r); d32(rnd()); break;
    case 8: case 9: {                                                             /* SHLD/SHRD r32,r32,imm8 */
        const unsigned o16 = rnd() % 4u == 0u;
        if (o16) b(0x66);
        b(0x0F); b(rnd() % 2 ? 0xA4 : 0xAC); b(0xC0 | (s2 << 3) | r);
        b(rnd() % 4u == 0u ? rnd() : (o16 ? 1u + rnd() % 16u : 1u + rnd() % 31u)); break; }
    case 10: b(0x0F); b(rnd() % 2 ? 0xA5 : 0xAD); b(0xC0 | (s2 << 3) | r); break; /* SHLD/SHRD CL */
    default: b(0x0F); b(rnd() % 2 ? 0xA4 : 0xAC); b(0x03 | (s2 << 3)); b(1u + rnd() % 31u); break;   /* [EBX] */
    }
}
/* V104 mode 28: 32-bit ADD/ADC/SUB/SBB/CMP (register and immediate) and other
 * producers followed by rotates by immediate (C0/C1, 8/16/32-bit, counts 0..31)
 * and SHLD/SHRD: the native ADD/SUB-family flag materializer. */
static void gen28(void)
{
    unsigned r = rnd() % 8, s2 = rnd() % 8;
    if (r == 4) r = 3;
    if (s2 == 4) s2 = 0;
    switch (rnd() % 14) {
    case 0: case 1: b((unsigned[]){0x01,0x29,0x11,0x19,0x39}[rnd() % 5]); b(0xC0 | (s2 << 3) | r); break;   /* ADD/SUB/ADC/SBB/CMP */
    case 2: b(0x81); b(0xC0 | ((unsigned[]){0,5,2,3,7}[rnd() % 5] << 3) | r); d32(rnd()); break;          /* imm32 forms */
    case 3: b(0x83); b(0xC0 | ((unsigned[]){0,5,2,3,7}[rnd() % 5] << 3) | r); b(rnd()); break;             /* imm8 forms */
    case 4: b(rnd() % 2 ? 0x40 + r : 0x48 + r); break;                                                    /* INC/DEC overlay */
    case 5: b(rnd() % 2 ? 0x21 : 0x31); b(0xC0 | (s2 << 3) | r); break;                                   /* AND/XOR */
    case 6: case 7: b((unsigned[]){0xF8,0xF9,0xF5,0x9F,0x9F,0x9F}[rnd() % 6]); break;                     /* CLC/STC/CMC, LAHF (AF -> AH) */
    case 8: b(0xB8 + r); d32(rnd() % 4u == 0u ? (rnd() % 3u == 0u ? 0u : 0x80000000u + rnd() % 4u) : rnd()); break;
    case 9: case 10: b(0xC1); b(0xC0 | ((rnd() % 4u) << 3) | r); b(rnd() % 6u == 0u ? rnd() : rnd() % 32u); break;  /* ROL/ROR/RCL/RCR r32 */
    case 11: b(0xC0); b(0xC0 | ((rnd() % 4u) << 3) | (rnd() % 8u)); b(rnd() % 32u); break;                  /* r8 */
    case 12: b(0x66); b(0xC1); b(0xC0 | ((rnd() % 4u) << 3) | r); b(rnd() % 32u); break;                   /* r16 */
    default: b(0x0F); b(rnd() % 2 ? 0xA4 : 0xAC); b(0xC0 | (s2 << 3) | r); b(1u + rnd() % 31u); break;     /* SHLD/SHRD */
    }
}
/* V93 mode 14: only forms the 16-bit tier runs natively (register ALU/MOV,
 * accumulator and 83 immediates, MOV r16,imm16), so chaining is visible. */
static void gen14(void)
{
    static const unsigned alu[6] = { 0x01, 0x09, 0x21, 0x29, 0x31, 0x39 };
    switch (rnd() % 5) {
    case 0: b(alu[rnd() % 6] | (rnd() % 2 ? 2u : 0u)); b(0xC0 | (rnd() % 64)); break;                    /* ALU r16,r16 */
    case 1: b((unsigned[]){0x05,0x0D,0x25,0x2D,0x35,0x3D}[rnd() % 6]); b(rnd()); b(rnd()); break;          /* ALU AX,imm16 */
    case 2: b(0x83); b(0xC0 | ((unsigned[]){0,1,4,5,6,7}[rnd() % 6] << 3) | (rnd() % 8)); b(rnd()); break;  /* ALU r16,imm8 */
    case 3: b(rnd() % 2 ? 0x89 : 0x8B); b(0xC0 | (rnd() % 64)); if ((P[L - 1] & 0x38) == 0x20) P[L - 1] ^= 0x08; break; /* MOV r16,r16 (not to SP) */
    default: { unsigned r = rnd() % 8; if (r == 4) r = 3; b(0xB8 + r); b(rnd()); b(rnd()); break; }          /* MOV r16,imm16 (not SP) */
    }
}
static unsigned cur_it; static uint8_t *cur_code; static unsigned cur_len; static B86Cpu cur_init;
static uint32_t initial_seed;
static void dump_case(const char *why)
{
    printf("%s seed=%u mode=%d it=%u code:", why, initial_seed, gen_mode, cur_it); for (unsigned q = 0; q < cur_len; ++q) printf("%02X", cur_code[q]);
    printf("\n  init fl=%04X", cur_init.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" r%d=%08X", i, cur_init.r[i]); printf("\n");
}
static void on_crash(int sig) { (void)sig; dump_case("CRASH"); fflush(stdout); _exit(3); }
static int rep_string_at(const uint8_t *m, uint32_t ip)
{
    unsigned i = 0, rep = 0, op;
    for (;; ++i) { op = m[ip + i];
        if (op == 0xF2 || op == 0xF3) { rep = 1; continue; }
        if (op == 0x66 || op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
        break; }
    return rep && ((op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF));
}
static int cpu_differs(const B86Cpu *C, const B86Cpu *D, const uint8_t *M, const uint8_t *M2)
{
    if (memcmp(C->seg, D->seg, sizeof C->seg) || memcmp(C->seg386, D->seg386, sizeof C->seg386)) return 1;
    for (int k = 0; k < 6; ++k) if (C->pm386.seg[k].base != D->pm386.seg[k].base || C->pm386.seg[k].limit != D->pm386.seg[k].limit) return 1;
    return memcmp(C->r, D->r, sizeof C->r) || C->ip != D->ip || ((C->flags ^ D->flags) & ~(uint32_t)B86_AF & 0xFFF) ||
           memcmp(M + 0x20000, M2 + 0x20000, 0x20000) || memcmp(M + 0x7000, M2 + 0x7000, 0x1000);
}
#if B86_V103_DESC8
/* V103 mode 27: b86_pm1_descriptor (one-lookup fast path) against the original
 * byte loop, with paging on: random GDT/LDT bases (mapped, remapped, unmapped,
 * page-crossing descriptors), cold or warm TLB, random selectors. Return code,
 * decoded descriptor, page-fault state and the whole TLB must match. */
static int m27_ref(B86Cpu *cpu, uint16_t selector, B86PM0Desc *out)
{
    uint32_t base, limit, index;
    uint64_t address;
    uint8_t bytes[8];
    int rc;
    if ((selector & 0xFFF8u) == 0u) return B86_PM1_SELECTOR;
    rc = b86_pm1_table(cpu, selector, &base, &limit);
    if (rc != B86_PM1_OK) return rc;
    index = selector & 0xFFF8u;
    if (index > limit || limit - index < 7u) return B86_PM1_TABLE_LIMIT;
    address = (uint64_t)base + (uint64_t)index;
    if (!cpu->pg && address + 8u > B86_RAM_BYTES) return B86_PM1_MEMORY_LIMIT;
    for (unsigned i = 0; i < 8u; ++i) bytes[i] = *b86_host(cpu, (uint32_t)address + i);
    rc = b86_pm0_decode(bytes, selector, out);
    if (rc == B86_PM0_NOT_PRESENT) return B86_PM1_NOT_PRESENT;
    return rc == B86_PM0_OK ? B86_PM1_OK : B86_PM1_ARGUMENT;
}
static uint32_t m27_base(void)
{
    switch (rnd() % 5) {
    case 0: return 0x3FF000u + (rnd() % 0x1000u);            /* runs into the unmapped 4 MiB+ */
    case 1: return 0x20000u + (rnd() % 0x20000u);             /* remapped pages */
    case 2: return 0x400000u + (rnd() % 0x10000u);            /* unmapped */
    default: return rnd() % 0x2F0000u;                        /* identity, below the page tables */
    }
}
static int m27_run(unsigned iters, uint32_t seed)
{
    static B86Cpu A, B;
    uint8_t *M = calloc(1, B86_RAM_BYTES + 64);
    unsigned it, bad = 0u, ok = 0u, faults = 0u, k;
    const uint32_t f0 = b86_v103_desc[1];
    rs = seed;
    for (k = 0; k < 0x300000u; ++k) M[k] = (uint8_t)rnd();
    for (it = 0; it < iters; ++it) {
        B86PM0Desc da, db;
        uint16_t sel = (uint16_t)rnd();
        int ra, rb;
        memset(&A, 0, sizeof A);
        b86_init(&A, M); A.model = B86_CPU_386; A.msw = 1; flat(&A);
        pg_setup(&A, M, 0);
        A.gdt_base = m27_base(); A.gdt_limit = rnd() % 4u ? 0xFFFFu : rnd() % 0x400u;
        A.pm386.ldtr_valid = rnd() % 4u != 0u; A.pm386.ldtr_base = m27_base(); A.pm386.ldtr_limit = 0xFFFFu;
        for (k = rnd() % 4u; k; --k) {                           /* warm some TLB entries */
            uint32_t ph, at = (rnd() % 2u ? A.gdt_base : A.pm386.ldtr_base) + (rnd() % 0x10000u);
            (void)b86_pg_walk(&A, at, &ph);
        }
        if (rnd() % 3u == 0u) sel = (uint16_t)((sel & 7u) | ((0xFF8u - (A.gdt_base & 0xFFFu) + (rnd() % 16u)) & 0xFFF8u));   /* near a page end */
        if (rnd() % 3u == 0u) {                                   /* warm exactly the descriptor's page */
            uint32_t ph;
            (void)b86_pg_walk(&A, ((sel & 4u) ? A.pm386.ldtr_base : A.gdt_base) + (sel & 0xFFF8u), &ph);
        }
        A.pf_pending = 0u;
        B = A;
        memset(&da, 0, sizeof da); memset(&db, 0, sizeof db);
        ra = b86_pm1_descriptor(&A, sel, &da);
        rb = m27_ref(&B, sel, &db);
        faults += B.pf_pending != 0u;
        ok += ra == B86_PM1_OK;
        if (ra != rb || (ra == B86_PM1_OK && memcmp(&da, &db, sizeof da)) || A.pf_pending != B.pf_pending ||
            (A.pf_pending && A.pf_lin != B.pf_lin) || memcmp(A.tlb_tag, B.tlb_tag, sizeof A.tlb_tag) ||
            memcmp(A.tlb_phys, B.tlb_phys, sizeof A.tlb_phys)) {
            if (bad < 10u) printf("MISMATCH mode27 it=%u sel=%04X gdt=%08X rc %d/%d pf %u/%u\n", it, sel, A.gdt_base, ra, rb, A.pf_pending, B.pf_pending);
            ++bad;
        }
    }
    printf("mode27: %u descriptor reads (paging on), %u mismatches, %u valid, %u with a page fault, %u by one lookup\n",
           iters, bad, ok, faults, b86_v103_desc[1] - f0);
    return bad ? 1 : 0;
}
#endif
#if B86_V103_STRBULK
/* V103 mode 26: REP MOVS/STOS through b86_pj_str with the bulk path on and off,
 * and a plain reference; random sizes, alignments, overlaps, DF, and an injected
 * 'kill' (smc_kills++ when a chosen byte is about to be written, once). After a
 * refusal (return 0) the rest is completed by the reference, as the interpreter
 * would. Final memory and registers must be identical in all three runs. */
static uint32_t m26_poison, m26_armed;
static void m26_hook(B86Cpu *c, uint32_t a, uint32_t n)
{
    if (m26_armed && m26_poison - a < n) { m26_armed = 0u; ++b86_pj_state(c)->smc_kills; }
}
static void m26_ref(B86Cpu *c, unsigned op, unsigned w)
{
    const unsigned kind = op <= 0xA5u ? 0u : 1u;
    const uint32_t step = (c->flags & B86_DF) ? (uint32_t)0u - w : w;
    while (c->r[B86_CX]) {
        uint32_t v = c->r[0], k;
        if (kind == 0u) { v = 0u; for (k = 0; k < w; ++k) v |= (uint32_t)c->mem[c->r[B86_SI] + k] << (8u * k); }
        for (k = 0; k < w; ++k) c->mem[c->r[B86_DI] + k] = (uint8_t)(v >> (8u * k));
        if (kind == 0u) c->r[B86_SI] += step;
        c->r[B86_DI] += step; --c->r[B86_CX];
    }
}
static int m26_run(unsigned iters, uint32_t seed, void *st, void *code)
{
    static B86Cpu A, B, R;
    uint8_t *MA = calloc(1, B86_RAM_BYTES + 64), *MB = calloc(1, B86_RAM_BYTES + 64), *MR = calloc(1, B86_RAM_BYTES + 64);
    uint8_t *cm = calloc(1, (B86_MEM_BYTES >> B86_LINE_SHIFT) + 2u);
    unsigned it, bad = 0u, refused = 0u, injected = 0u;
#if B86_V103_STATS
#define M26_BULK() (pj_v103s ? pj_v103s->n[9] : 0u)
#else
#define M26_BULK() 0u                        /* statistics off: bulk runs not counted */
#endif
    const uint32_t bulk0 = M26_BULK();
    rs = seed;
    for (it = 0; it < iters; ++it) {
        const unsigned op = (unsigned[]){0xA4,0xA5,0xAA,0xAB}[rnd() % 4], w = (op & 1u) ? (rnd() % 2 ? 4u : 2u) : 1u;
        const uint32_t desc = op | (1u << 8) | (w << 9);
        uint32_t si = 0x20000u + (rnd() % 0x80000u), di, k, cx, eax = rnd(), fl, poison;
        unsigned run, inject;
        B86Cpu *cs[3] = { &A, &B, &R };
        uint8_t *ms[3] = { MA, MB, MR };
        switch (rnd() % 4) {                      /* overlaps in both directions, or apart */
        case 0: di = si + (rnd() % 64u) - 32u; break;
        case 1: di = si + (rnd() % 4096u); break;
        default: di = 0x20000u + (rnd() % 0x80000u); break;
        }
        if (rnd() % 3u) { si &= ~(w - 1u); di &= ~(w - 1u); }
        cx = rnd() % 2u ? rnd() % 3000u : rnd() % 40u;
        fl = 2u | (rnd() % 8u == 0u ? (uint32_t)B86_DF : 0u);
        inject = rnd() % 3u == 0u;
        poison = cx ? ((fl & B86_DF) ? di - rnd() % (cx * w) : di + rnd() % (cx * w)) : di;
        injected += inject;
        for (k = 0x1F000u; k < 0xB0000u; ++k) MA[k] = (uint8_t)rnd();
        memcpy(MB, MA, B86_RAM_BYTES); memcpy(MR, MA, B86_RAM_BYTES);
        memset(cm, 0, (B86_MEM_BYTES >> B86_LINE_SHIFT) + 2u);
        cm[poison >> B86_LINE_SHIFT] = 1u;
        for (run = 0; run < 3; ++run) {
            B86Cpu *c = cs[run];
            memset(c, 0, sizeof *c);
            b86_init(c, ms[run]); c->model = B86_CPU_386; c->msw = 1; flat(c);
            c->r[0] = eax; c->r[B86_SI] = si; c->r[B86_DI] = di; c->r[B86_CX] = cx; c->flags = fl;
            if (run < 2) {                         /* 0: bulk on, 1: bulk off (original loop) */
                if (!b86_pj_attach(c, st, b86_pj_state_bytes(), code, 1u << 20)) return 2;
                c->codemap = cm; c->smc_hook = m26_hook;
                m26_poison = poison; m26_armed = inject;
                pj_v103_bulk_on = run == 0;
                if (!b86_pj_str(c, desc)) { ++refused; m26_ref(c, op, w); }   /* the interpreter's replay */
                m26_armed = 0u;
            } else m26_ref(c, op, w);              /* 2: plain reference */
        }
        pj_v103_bulk_on = 1u;
        for (run = 1; run < 3; ++run) {
            if (memcmp(MA + 0x1F000u, ms[run] + 0x1F000u, 0x91000u) ||
                A.r[0] != cs[run]->r[0] || A.r[B86_SI] != cs[run]->r[B86_SI] || A.r[B86_DI] != cs[run]->r[B86_DI] || A.r[B86_CX] != cs[run]->r[B86_CX]) {
                if (bad < 10u) printf("MISMATCH mode26 it=%u vs-run=%u op=%02X w=%u si=%05X di=%05X cx=%u df=%u inject=%u\n",
                                      it, run, op, w, si, di, cx, (fl & B86_DF) != 0u, inject);
                ++bad;
            }
        }
    }
    printf("mode26: %u cases, %u mismatches, %u injected kills, %u refusals replayed, %u bulk runs\n",
           iters, bad, injected, refused, M26_BULK() - bulk0);
    return bad ? 1 : 0;
}
#endif
int main(int argc, char **argv)
{
    static B86Cpu C, D;
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 1000, bad = 0, nat = 0, tot = 0, skipped = 0, partial = 0;
    if (argc > 2) rs = (uint32_t)atoi(argv[2]);
    if (argc > 3) gen_mode = atoi(argv[3]);
    const size_t code_bytes = argc > 4 ? (size_t)strtoul(argv[4], NULL, 0) : (size_t)(1u << 20);   /* V94 */
    const size_t hot_bytes = argc > 5 ? (size_t)strtoul(argv[5], NULL, 0) : 0u;                      /* V100 */
    uint8_t *hot_buf = NULL;   /* 0 original, 1 V57+ forms, 2 mixed */
    initial_seed = rs;
    uint8_t *M = calloc(1, B86_RAM_BYTES + 64), *M2 = calloc(1, B86_RAM_BYTES + 64), *M0 = calloc(1, B86_RAM_BYTES + 64);
    void *code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *st = calloc(1, b86_pj_state_bytes());
    signal(SIGSEGV, on_crash); signal(SIGILL, on_crash); signal(SIGBUS, on_crash);
#if B86_V103_STRBULK
    if (gen_mode == 26) return m26_run(iters, rs, st, code);
#endif
#if B86_V103_DESC8
    if (gen_mode == 27) return m27_run(iters, rs);
#endif
    for (unsigned it = 0; it < iters; ++it) {
        memset(&C, 0, sizeof C);
        b86_init(&C, M); C.model = B86_CPU_386; C.msw = 1;
        if (gen_mode >= 10 && gen_mode < 17) { pm16(&C, M); g_csbase = 0x10000; } else { flat(&C); g_csbase = 0; }
        if (!b86_pj_attach(&C, st, b86_pj_state_bytes(), code, code_bytes)) {   /* V99 */
            printf("b86_pj_attach refused a %lu-byte code buffer (smaller than its tail + 16 KiB)\n", (unsigned long)code_bytes);
            return 2;
        }
        if (hot_bytes && !hot_buf) {   /* V100: like the Pico (SRAM vs PSRAM ~244 MiB apart): far beyond B.W/BL reach */
            uintptr_t at;
            for (at = (uintptr_t)code + 0x10000000u; !hot_buf && at; at += 0x01000000u) {
                void *h = mmap((void *)at, hot_bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (h == MAP_FAILED) continue;
                if ((uintptr_t)h - (uintptr_t)code < 0x08000000u && (uintptr_t)code - (uintptr_t)h < 0x08000000u) { munmap(h, hot_bytes); continue; }
                hot_buf = h;
            }
            if (!hot_buf) { printf("could not map the hot tier far from the code cache\n"); return 2; }
            printf("hot tier at %p, code cache at %p (distance %ld MiB)\n", (void *)hot_buf, (void *)code,
                   (long)(((intptr_t)hot_buf - (intptr_t)code) / (1 << 20)));
        }
        if (hot_bytes && !b86_pj_attach_hot(&C, hot_buf, hot_bytes)) {             /* V100 */
            printf("b86_pj_attach_hot refused %lu bytes\n", (unsigned long)hot_bytes);
            return 2;
        }
#if BB_PJ_PM16_REG_TIER && BB_PJ_PM16_ADAPTIVE
        b86_pj_state(&C)->pm16_adapt_cooldown = 0u;   /* test every case natively */
#endif
        for (unsigned a = 0x20000; a < 0x32000; ++a) M[a] = (uint8_t)rnd();
        memset(M + g_csbase + 0x1000, 0x90, 512);
        P = M + g_csbase + 0x1000; L = 0;
        if (gen_mode >= 13 && gen_mode <= 28) {      /* V93: whole programs with loops (V94: 17/18 flat; V95: 19/20 + paging; V99: 21-23 visits; V100: 24) */
            /* mode 15: DS based just below the code (0x10800), so ordinary stores
             * modify the running program: SMC kills and unlinks of chained blocks.
             * mode 16: native forms (mode 14) plus MOV [moffs16],AX stores aimed at
             * the imm16 of MOV r16,imm16 / ALU AX,imm16 instructions of the loop. */
            unsigned imm_at[64], n_imm = 0u, st_at[16], n_st = 0u;
            if (gen_mode == 15 || gen_mode == 16) C.pm386.seg[B86_DS].base = 0x10800u;
            unsigned bstart[8], blocks = 2u + rnd() % 5u, k, g;
            for (k = 0; k < blocks; ++k) {
                unsigned ni = 1u + rnd() % 5u, i;
                bstart[k] = L;
                for (i = 0; i < ni; ++i) {
                    const unsigned l0 = L;
                    unsigned tries = 0;
                    if (gen_mode == 16 && n_st < 16u && rnd() % 5u == 0u) {          /* placeholder store */
                        st_at[n_st++] = L; b(0xA3); b(0); b(0); continue;
                    }
                    if ((gen_mode == 18 || gen_mode == 20) && n_st < 16u && rnd() % 5u == 0u) {          /* V94: flat MOV [abs32],EAX placeholder */
                        st_at[n_st++] = L; b(0xA3); b(0); b(0); b(0); b(0); continue;
                    }
                    do { L = l0; if (gen_mode == 28) gen28(); else if (gen_mode == 25) gen25(); else if (gen_mode == 14 || gen_mode == 16) gen14(); else if (gen_mode == 18 || gen_mode >= 20) gen18(); else if (gen_mode == 17 || gen_mode == 19) { if (rnd() % 2) gen_new(0); else gen_one(0); } else gen_pm16(0); }
                    while (++tries < 12u && (pj13_bad(P + l0, L - l0) ||    /* V94: flat chunks may hold several instructions: no REP anywhere */
                           ((gen_mode == 17 || gen_mode == 19) && (memchr(P + l0, 0xF2, L - l0) || memchr(P + l0, 0xF3, L - l0)))));
                    if ((gen_mode == 17 || gen_mode == 19) && (memchr(P + l0, 0xF2, L - l0) || memchr(P + l0, 0xF3, L - l0))) L = l0;
                    if (pj13_bad(P + l0, L - l0)) L = l0;
                    else if ((gen_mode == 18 || gen_mode >= 20) && n_imm < 64u && L - l0 == 5u &&
                             ((P[l0] >= 0xB8u && P[l0] <= 0xBFu) || P[l0] == 0x05u || P[l0] == 0x0Du || P[l0] == 0x25u ||
                              P[l0] == 0x2Du || P[l0] == 0x35u || P[l0] == 0x3Du))
                        imm_at[n_imm++] = l0 + 1u;
                    else if (gen_mode == 16 && n_imm < 64u && L - l0 == 3u &&
                             ((P[l0] >= 0xB8u && P[l0] <= 0xBFu) || P[l0] == 0x05u || P[l0] == 0x0Du || P[l0] == 0x25u ||
                              P[l0] == 0x2Du || P[l0] == 0x35u || P[l0] == 0x3Du))
                        imm_at[n_imm++] = l0 + 1u;
                }
                if (rnd() % 3 == 0 && k + 1u < blocks) {                 /* forward Jcc over a NOP pad */
                    const unsigned pad = 1u + rnd() % 4u;
                    unsigned q;
                    b(0x70 | (rnd() & 15)); b(pad); for (q = 0; q < pad; ++q) b(0x90);
                }
            }
            {   unsigned q;                                                            /* aim the stores */
                for (q = 0; q < n_st; ++q) {
                    if (gen_mode == 18 || gen_mode == 20) {                 /* V94: flat absolute address in the code */
                        uint32_t a = 0x1000u + (n_imm ? imm_at[rnd() % n_imm] : rnd() % (L ? L : 1u));
                        if (gen_mode == 20) {                               /* V95: only instructions BEFORE the store */
                            unsigned e2[64], ne2 = 0u, z;
                            for (z = 0; z < n_imm; ++z) if (imm_at[z] + 4u <= st_at[q]) e2[ne2++] = imm_at[z];
                            a = ne2 ? 0x1000u + e2[rnd() % ne2] : 0x20000u + (rnd() & 0xFFCu);
                        }
                        P[st_at[q] + 1u] = (uint8_t)a; P[st_at[q] + 2u] = (uint8_t)(a >> 8);
                        P[st_at[q] + 3u] = (uint8_t)(a >> 16); P[st_at[q] + 4u] = (uint8_t)(a >> 24);
                        continue;
                    }
                    const unsigned off = n_imm ? 0x800u + imm_at[rnd() % n_imm] : 0x800u + (rnd() % (L ? L : 1u));
                    P[st_at[q] + 1u] = (uint8_t)off; P[st_at[q] + 2u] = (uint8_t)(off >> 8);
                } }
            {   const int rel = (int)bstart[rnd() % blocks] - (int)(L + 2u);          /* backward Jcc */
                if (rel >= -128) { b(0x70 | (rnd() & 15)); b((uint8_t)rel); } }
            {   const int rel = -(int)(L + 2u);                                       /* JMP to the start */
                if (rel >= -128) { b(0xEB); b((uint8_t)rel); }
                else if (gen_mode == 17 || gen_mode == 25 || gen_mode == 28) { const int r32 = -(int)(L + 5u); b(0xE9); b(r32 & 0xFF); b((r32 >> 8) & 0xFF); b((r32 >> 16) & 0xFF); b((r32 >> 24) & 0xFF); }
                else { const int r16 = -(int)(L + 3u); b(0xE9); b(r16 & 0xFF); b((r16 >> 8) & 0xFF); } }
            C.ip = 0x1000; C.r[0] = rnd(); C.r[1] = rnd(); C.r[2] = rnd(); C.r[4] = 0x8000;
            C.r[3] = 0x20000 + (rnd() & 0xFFF); C.r[5] = 0x22000 + (rnd() & 0xFFF); C.r[6] = 0x24000 + (rnd() & 0xFFF); C.r[7] = 0x26000 + (rnd() & 0xFFF);
            C.flags = 2 | (rnd() & 0x8D5);
            if (gen_mode >= 19 && gen_mode != 25 && gen_mode != 28) { /* V95: paging on (V99: 21-23 too; 23 keeps code low) */
                pg_setup(&C, M, (gen_mode >= 20 && gen_mode <= 22) || gen_mode == 24);
                if ((gen_mode >= 20 && gen_mode <= 22) || gen_mode == 24) memcpy(M + 0x201000, M + 0x1000, 0x1000);
                memcpy(M + 0x220000, M + 0x20000, 0x20000);             /* data as the program expects */
            }
            memcpy(M2, M, B86_RAM_BYTES); memcpy(M0, M, 0x40000);
            D = C; D.mem = M2; D.pj = NULL;
            cur_it = it; cur_code = M0 + g_csbase + 0x1000; cur_len = L; cur_init = C;
            C.icount = 0; D.icount = 0;
            run_to = 3000u;
            if (gen_mode >= 21 && gen_mode != 25 && gen_mode != 28) { /* V99: two simulated real-mode visits (V100: 24 = 21 + forced hot resets) */
                unsigned visit;
                for (visit = 0u; visit < 2u; ++visit) {
                    const unsigned long at = C.icount + 1000u;      /* native runs retire up to 2048 per dispatch */
                    for (g = 0; C.icount < at && g < 200000u; ++g) { ++m13_dispatch; if (b86_step(&C) != B86_OK) break; }
                    for (g = 0; D.icount < C.icount && g < 2000000u; ++g) if (b86_step(&D) != B86_OK) break;
                    {   const unsigned act = !visit ? 0u : gen_mode == 22 ? 5u : gen_mode == 23 ? (rnd() % 2u ? 0u : 6u) : gen_mode == 24 ? 0u : rnd() % 5u;
                        const uint32_t imm = n_imm ? imm_at[rnd() % n_imm] : 0u;
                        const uint8_t nb = (uint8_t)(rnd() | 1u);
                        const uint32_t ms = C.msw; const uint8_t pgv = C.pg;
                        if (visit) ++m21_act[act];
                        switch (act) {
                        case 0: break;                                          /* nothing written */
                        case 1: if (n_imm) {                                    /* host bridge store into the code */
                                    M[0x201000u + imm] ^= nb; M2[0x201000u + imm] ^= nb;
                                    b86_v34_note_host_phys(0x201000u + imm, 1u); }
                                break;
                        case 2: if (n_imm) {                                    /* real-mode CPU store into the code */
                                    M[0x201000u + imm] ^= nb; M2[0x201000u + imm] ^= nb;
                                    C.msw &= ~1u; C.pg = 0u; smc(&C, 0x201000u + imm, 1u); C.msw = ms; C.pg = pgv; }
                                break;
                        case 3: {                                               /* remap the code page to a modified copy */
                                    uint32_t *pt1 = (uint32_t *)(void *)(M + 0x301000u), *pt2 = (uint32_t *)(void *)(M2 + 0x301000u);
                                    memcpy(M + 0x203000u, M + 0x201000u, 0x1000u);
                                    if (n_imm) M[0x203000u + imm] ^= nb;
                                    memcpy(M2 + 0x203000u, M + 0x203000u, 0x1000u);
                                    pt1[1] = 0x203000u | 7u; pt2[1] = 0x203000u | 7u;
                                    b86_pg_flush(&C); b86_pg_flush(&D); }
                                break;
                        case 4:                                                 /* real-mode store into low memory */
                                M[0x9000u] ^= nb; M2[0x9000u] ^= nb;
                                C.msw &= ~1u; C.pg = 0u; smc(&C, 0x9000u, 1u); C.msw = ms; C.pg = pgv;
                                break;
                        case 6: if (n_imm) {                                    /* mode 23: real-mode store into LOW code */
                                    M[0x1000u + imm] ^= nb; M2[0x1000u + imm] ^= nb;
                                    C.msw &= ~1u; C.pg = 0u; smc(&C, 0x1000u + imm, 1u); C.msw = ms; C.pg = pgv; }
                                break;
                        default: if (n_imm) {                                   /* negative control: unreported code store */
                                    M[0x201000u + imm] ^= nb; M2[0x201000u + imm] ^= nb; }
                                break;
                        }
                        if (gen_mode == 24 && hot_bytes) {                      /* V100: forced hot reset, freed space reused */
                            B86Pj *jh = b86_pj_state(&C);
                            pj_hot_reset(&C, jh);
                            memset(jh->code + jh->hot_start, 0xDE, jh->hot_base + jh->hot_cap - jh->hot_start);   /* UDF traps */
                            __builtin___clear_cache((char *)(jh->code + jh->hot_start), (char *)(jh->code + jh->hot_base + jh->hot_cap));
                        }
                        b86_pj_mark_rm_epoch(&C);
                    }
                }
                run_to = C.icount + 3000u;                          /* keep running well after the event */
            }
            for (g = 0; C.icount < run_to && g < 200000u; ++g) { ++m13_dispatch; if (b86_step(&C) != B86_OK) break; }
            for (g = 0; D.icount < C.icount && g < 2000000u; ++g) if (b86_step(&D) != B86_OK) break;
            b86_flags_materialize(&C); b86_flags_materialize(&D);
            { B86Pj *jj93 = b86_pj_state(&C); m13_unlinks += jj93->unlinks; m13_evict += jj93->evictions;
              m100_promotions += jj93->promotions; m100_hot_resets += jj93->hot_resets;
              m13_smc += jj93->flush_smc + (memcmp(M + (gen_mode == 20 ? 0x201000 : g_csbase + 0x1000), M0 + g_csbase + 0x1000, L) != 0); }
            ++m13_programs; m13_retired += C.icount; tot += (unsigned)C.icount; nat += (unsigned)C.icount;
            const int code15 = ((gen_mode == 15 || gen_mode == 16) && memcmp(M + 0x10000, M2 + 0x10000, 0x2000)) ||
                               (gen_mode == 18 && memcmp(M + 0x1000, M2 + 0x1000, 0x1000)) ||
                               (gen_mode >= 19 && memcmp(M + 0x220000, M2 + 0x220000, 0x20000)) ||
                               (gen_mode >= 20 && memcmp(M + 0x201000, M2 + 0x201000, 0x1000)) ||
                               (gen_mode == 23 && memcmp(M + 0x1000, M2 + 0x1000, 0x1000));
            if ((C.icount != D.icount || cpu_differs(&C, &D, M, M2) || code15) && ++bad <= 12) {
                ++m13_bad;
                printf("MISMATCH13 retired native=%llu interp=%llu ", (unsigned long long)C.icount, (unsigned long long)D.icount); dump_case("");
                printf("  native eip=%08X fl=%04X", C.ip, C.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", C.r[i]); printf("\n");
                printf("  interp eip=%08X fl=%04X", D.ip, D.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", D.r[i]); printf("\n");
                if (memcmp(M + 0x20000, M2 + 0x20000, 0x20000)) printf("  memory differs\n");
            } else if (C.icount != D.icount || cpu_differs(&C, &D, M, M2) || code15) ++m13_bad;
            continue;
        }
        unsigned n = 1 + rnd() % 12;
        for (unsigned i = 0; i < n; ++i) { int last = i == n - 1 && rnd() % 2;
            if (gen_mode == 12) gen_pm16(last);
            else if (gen_mode % 10 == 1 || (gen_mode % 10 == 2 && rnd() % 2)) gen_new(last); else gen_one(last); }
        C.ip = 0x1000; C.r[0] = rnd(); C.r[1] = rnd(); C.r[2] = rnd(); C.r[4] = 0x8000;
        C.r[3] = 0x20000 + (rnd() & 0xFFF); C.r[5] = 0x22000 + (rnd() & 0xFFF); C.r[6] = 0x24000 + (rnd() & 0xFFF); C.r[7] = 0x26000 + (rnd() & 0xFFF);
        C.flags = 2 | (rnd() & 0x8D5);
        memcpy(M2, M, B86_RAM_BYTES); memcpy(M0, M, 0x40000);
        D = C; D.mem = M2; D.pj = NULL;
        cur_it = it; cur_code = M0 + g_csbase + 0x1000; cur_len = L; cur_init = C;
        unsigned nn = b86_pj_try(&C);
        /* PM16 admission is adaptive/two-touch: an entry refusal leaves the
         * guest state untouched, so retry a few times like the dispatcher. */
        for (unsigned tries = 0; !nn && gen_mode >= 10 && tries < 2u && C.ip == 0x1000u; ++tries) nn = b86_pj_try(&C);
        tot += n; nat += nn;
        if (!nn) continue;
        b86_flags_materialize(&C);
        {   /* count retirements only: an interpreter step that leaves EIP on a
             * REP string op ran one restartable chunk, not an instruction */
            unsigned k = 0, guard;
            for (guard = 0; k < nn && guard < 200000u; ++guard) {
                const uint32_t ip = D.ip; const int rep = rep_string_at(M2, g_csbase + ip);
                if (b86_step(&D) != B86_OK) break;
                if (!(rep && D.ip == ip)) ++k;
            }
            if (k < nn) { ++skipped; continue; } }
        b86_flags_materialize(&D);
        int bad1 = cpu_differs(&C, &D, M, M2);
        /* The interpreter also runs long REPs in chunks (one b86_step need
         * not retire it): finish an interpreter REP that native retired. */
        if (bad1 && D.ip != C.ip && rep_string_at(M0, g_csbase + D.ip)) {
            const uint32_t at = D.ip;
            for (unsigned g = 0; g < 100000u && D.ip == at; ++g) if (b86_step(&D) != B86_OK) break;
            b86_flags_materialize(&D);
            bad1 = cpu_differs(&C, &D, M, M2);
        }
        /* A REP string op may stop part-way with EIP still on it (restartable):
         * replay exactly the native element count k in the interpreter. */
        if (bad1 && C.ip == D.ip && rep_string_at(M0, g_csbase + C.ip) && D.r[1] > C.r[1]) {
            const uint32_t keep = C.r[1];
            D.r[1] = D.r[1] - C.r[1];
            for (unsigned g = 0; g < 400000u && D.r[1] && D.ip == C.ip; ++g) if (b86_step(&D) != B86_OK) break;
            if (D.r[1] == 0u) { D.r[1] = keep; D.ip = C.ip; ++partial; }
            b86_flags_materialize(&D);
            bad1 = cpu_differs(&C, &D, M, M2);
        }
        if (bad1 && ++bad <= 12) {
            printf("MISMATCH n=%u ", nn); dump_case("");
            printf("  native eip=%08X fl=%04X", C.ip, C.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", C.r[i]); printf("\n");
            printf("  interp eip=%08X fl=%04X", D.ip, D.flags & 0xFFF); for (int i = 0; i < 8; ++i) printf(" %08X", D.r[i]); printf("\n");
            if (memcmp(M + 0x20000, M2 + 0x20000, 0x20000) || memcmp(M + 0x7000, M2 + 0x7000, 0x1000)) printf("  memory differs\n");
        }
    }
#if B86_V99_SELECTIVE
    if (gen_mode >= 21)
        printf("v99: second-visit events none=%lu host=%lu rm-cpu=%lu remap=%lu low=%lu unreported=%lu low-code=%lu | selective=%lu full=%lu (dirty %lu, map %lu, stale %lu, no snapshot %lu) links kept=%lu cut=%lu entries kept=%lu cut=%lu\n",
               m21_act[0], m21_act[1], m21_act[2], m21_act[3], m21_act[4], m21_act[5], m21_act[6],
               pj_v99_n[0], pj_v99_n[1], pj_v99_n[2], pj_v99_n[3], pj_v99_n[4], pj_v99_n[5], pj_v99_n[6], pj_v99_n[7], pj_v99_n[8], pj_v99_n[9]);
#else
    if (gen_mode >= 21)
        printf("v99: off; second-visit events none=%lu host=%lu rm-cpu=%lu remap=%lu low=%lu unreported=%lu low-code=%lu\n",
               m21_act[0], m21_act[1], m21_act[2], m21_act[3], m21_act[4], m21_act[5], m21_act[6]);
#endif
#if B86_V103_STATS
    if (gen_mode >= 13 && pj_v103s) {
        unsigned q;
        printf("v103: mat-sites");
        for (q = 1u; q < 13u; ++q) printf("%c%u", q == 1u ? '=' : ',', pj_v103s->site[q]);
        printf(" kinds");
        for (q = 0u; q < (unsigned)LZ_COUNT; ++q) if (pj_v103s->kind[q]) printf(" %u:%u", q, pj_v103s->kind[q]);
        printf(" | mem slow=%u from-psram=%u code-writes=%u | desc %u, one-lookup %u\n", pj_v103s->n[1], pj_v103s->n[7], pj_v103s->n[6],
               b86_v103_desc[0] + b86_v103_desc[1], b86_v103_desc[1]);
    }
#endif
#if B86_V104_LINKSKIP
    if (gen_mode >= 13)
        printf("v104: link passes=%lu skipped=%lu skipcheck missed=%lu\n", pj_v104_n[0], pj_v104_n[1], pj_v104_n[2]);
#endif
#if B86_V102_KILLFAST || B86_V102_POOL_RESTART
    if (gen_mode >= 13)
        printf("v102: targeted kills=%lu killcheck missed=%lu pool restarts=%lu restart unlinks=%lu\n",
               pj_v102_n[0], pj_v102_n[1], pj_v102_n[2], pj_v102_n[3]);
#endif
#if B86_V101_FAST
    if (gen_mode >= 13)
        printf("v101: veneers reused=%lu class bits=%lu masked PTE compares=%lu crosscheck: live disagreements=%lu dead targets kept=%lu\n",
               pj_v101_n[0], pj_v101_n[3], pj_v101_n[4], pj_v101_n[1], pj_v101_n[2]);
#endif
#if B86_V100_XTIER || B86_V100_SMC_EXIT
    if (gen_mode >= 13)
        printf("v100: hot tier %lu bytes | links made=%lu cross-tier=%lu pool-full=%lu pool-restarts=%lu | smc-exits=%lu | hot promotions=%lu resets=%lu\n",
               (unsigned long)hot_bytes, pj_v100_n[3], pj_v100_n[0], pj_v100_n[1], pj_v100_n[4], pj_v100_n[5],
               m100_promotions, m100_hot_resets);
#endif
    if (gen_mode >= 13 && gen_mode <= 24)
        printf("evictions=%lu (code cache %lu bytes)\n", m13_evict, (unsigned long)code_bytes);
    if (gen_mode >= 13 && gen_mode <= 24)
        printf("mode%d: programs=%lu dispatches=%lu retired=%lu per-dispatch=%.1f mismatches=%lu unlinks=%lu code-modified-programs=%lu\n",
               gen_mode, m13_programs, m13_dispatch, m13_retired, m13_dispatch ? (double)m13_retired / m13_dispatch : 0.0, m13_bad, m13_unlinks, m13_smc);
#if defined(B86_PJ_V93_PM16_LINK) && B86_PJ_V93_PM16_LINK && BB_PJ_PM16_REG_TIER
    if (gen_mode >= 10) printf("v93 links: tries=%lu ctx-refused=%lu linked=%lu\n", pj_v93_n[0], pj_v93_n[1], pj_v93_n[2]);
#endif
#if defined(B86_PJ_V82_PM16) && B86_PJ_V82_PM16 && BB_PJ_PM16_REG_TIER
    if (gen_mode >= 10)
        printf("v82 translations: push=%lu pop=%lu alu=%lu adc-sbb=%lu movimm=%lu incdec=%lu grp3=%lu shift=%lu shift-cl=%lu segmov=%lu stores=%lu\n",
               pj_v82_n[0], pj_v82_n[1], pj_v82_n[2], pj_v82_n[3], pj_v82_n[4], pj_v82_n[5], pj_v82_n[6], pj_v82_n[7], pj_v82_n[8], pj_v82_n[9], pj_v82_n[11]);
#endif
#if defined(B86_PJ_V85_PM16) && B86_PJ_V85_PM16 && BB_PJ_PM16_REG_TIER
    if (gen_mode >= 10) printf("v85 translations: enter=%lu pop-mem=%lu\n", pj_v85_n[0], pj_v85_n[1]);
#endif
#if defined(B86_PJ_V85_TIER_SPLIT) && B86_PJ_V85_TIER_SPLIT
    printf("v85 tier (entries/insns): flat-psram=%lu/%lu flat-sram=%lu/%lu pm16-psram=%lu/%lu pm16-sram=%lu/%lu\n",
           pj_v85_ent[0], pj_v85_ins[0], pj_v85_ent[1], pj_v85_ins[1], pj_v85_ent[2], pj_v85_ins[2], pj_v85_ent[3], pj_v85_ins[3]);
#endif
#if defined(B86_PJ_V84_PM16) && B86_PJ_V84_PM16 && BB_PJ_PM16_REG_TIER
    if (gen_mode >= 10)
        printf("v84 translations: leave=%lu mov-mem-sreg=%lu push-seg-mem=%lu les-lds=%lu\n", pj_v84_n[0], pj_v84_n[1], pj_v84_n[2], pj_v84_n[3]);
#endif
#if defined(B86_PJ_V83_PM16) && B86_PJ_V83_PM16 && BB_PJ_PM16_REG_TIER
    if (gen_mode >= 10)
        printf("v83 translations: o32-alu=%lu o32-adc-sbb=%lu o32-incdec=%lu o32-grp3=%lu o32-shift=%lu o32-mov=%lu rotate1=%lu xchg=%lu\n",
               pj_v83_n[0], pj_v83_n[1], pj_v83_n[2], pj_v83_n[3], pj_v83_n[4], pj_v83_n[5], pj_v83_n[6], pj_v83_n[7]);
#endif
    printf("fuzz: %u sequences, %u native instructions, %u interpreter-refused, %u partial-rep, %u mismatches\n", iters, nat, skipped, partial, bad);
    if (iters != 0 && nat == 0) { fprintf(stderr, "FAIL: zero native PJ coverage for seed=%u mode=%d\n", initial_seed, gen_mode); return 2; }
    return bad != 0;
}

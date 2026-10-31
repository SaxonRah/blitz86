/* PJ differential fuzzer: random flat-32 sequences, native block vs interpreter. */
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
    switch (rnd() % 29) {
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
int main(int argc, char **argv)
{
    static B86Cpu C, D;
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 1000, bad = 0, nat = 0, tot = 0, skipped = 0, partial = 0;
    if (argc > 2) rs = (uint32_t)atoi(argv[2]);
    if (argc > 3) gen_mode = atoi(argv[3]);   /* 0 original, 1 V57+ forms, 2 mixed */
    initial_seed = rs;
    uint8_t *M = calloc(1, B86_RAM_BYTES + 64), *M2 = calloc(1, B86_RAM_BYTES + 64), *M0 = calloc(1, B86_RAM_BYTES + 64);
    void *code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *st = calloc(1, b86_pj_state_bytes());
    signal(SIGSEGV, on_crash); signal(SIGILL, on_crash); signal(SIGBUS, on_crash);
    for (unsigned it = 0; it < iters; ++it) {
        memset(&C, 0, sizeof C);
        b86_init(&C, M); C.model = B86_CPU_386; C.msw = 1;
        if (gen_mode >= 10) { pm16(&C, M); g_csbase = 0x10000; } else { flat(&C); g_csbase = 0; }
        b86_pj_attach(&C, st, b86_pj_state_bytes(), code, 1 << 20);
#if BB_PJ_PM16_REG_TIER && BB_PJ_PM16_ADAPTIVE
        b86_pj_state(&C)->pm16_adapt_cooldown = 0u;   /* test every case natively */
#endif
        for (unsigned a = 0x20000; a < 0x32000; ++a) M[a] = (uint8_t)rnd();
        memset(M + g_csbase + 0x1000, 0x90, 512);
        P = M + g_csbase + 0x1000; L = 0;
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

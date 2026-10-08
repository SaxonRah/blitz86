/* blitz86 translator: frontend (decode, flag liveness, lowering) and
 * runtime (block cache, chaining, dispatcher, self-modifying code).
 *
 * Design rules (see DESIGN.md):
 *  - Translate everything. An instruction without a native lowering becomes
 *    an inline call to the reference interpreter for that one instruction;
 *    it never rejects a region or returns to a different execution tier.
 *  - Guest registers live in host registers across chained blocks.
 *  - Flags: per-block backward liveness with successor lookahead. A producer
 *    either feeds an in-block branch through host NZCV (fused), or writes a
 *    two-word lazy record (kind, a, res), or nothing at all.
 *  - SMC: stores test a per-64-byte line map; only real code bytes inside a
 *    live block's guarded range invalidate it.
 */
#include "backend.h"
#include <stdlib.h>
#include <string.h>
#ifdef B86_DEBUG_DUMP
#include <stdio.h>
#endif

#define MAXB        8192u
#define MAPN        16384u
#define PG_SHIFT    9u                      /* SMC bucket page: 512 bytes */
#define NPG         ((B86_MEM_BYTES >> PG_SHIFT) + 2u)
#define MAX_SPAN    480u                    /* guarded bytes per block   */
#define DEF_INSNS   48u
#define ALLF        ((uint16_t)B86_ARITH)

#define OFF(f) ((unsigned)offsetof(B86Cpu, f))

typedef struct Block {
    uint32_t key;
    uint32_t glo, ghi;          /* guarded linear range [glo, ghi)        */
    uint8_t *host;
    uint8_t *dead_stub;
    uint32_t next_pg;           /* bucket chain: index + 1                 */
    uint32_t map_slot;
    uint16_t ninsn;
    uint8_t dead;
} Block;

typedef struct B86Jit {
    B86Cpu *cpu;
    uint8_t *buf, *buf_end, *code_start;
    int (*enter)(B86Cpu *, uintptr_t);
    Emit e;
    Block *blk;
    uint32_t nblk;
    uint32_t *map;              /* key slot -> block index + 1             */
    uint32_t *pg_head;          /* SMC page -> block index + 1             */
    B86Fast *fast;
    uint8_t *codemap;
    uint32_t flush_gen;
    unsigned max_insns;
    int no_lookahead;
    int single_step;            /* testing: never enter a block from the fast table */
    B86JitStats st;
} J;

/* ------------------------------------------------------------------------ */
/* Decoder                                                                  */
/* ------------------------------------------------------------------------ */

enum { C_SEQ, C_JCC, C_JMP, C_CALL, C_RET, C_IND, C_END, C_HLT };

typedef struct Insn {
    uint16_t ip, next, target;
    uint8_t op, w, seg, rep, nprefix;
    uint8_t has_modrm, mod, reg, rm;
    int16_t disp;
    uint16_t imm, imm2;
    uint8_t cls, native, nzclob, selfclob;
    uint16_t fuse, fdef, live, live_taken;
    uint8_t arm, lazy, fused, mode;     /* producer / consumer decisions */
    uint8_t prod;                       /* fused consumer: producer index */
    uint8_t valid;                      /* x86 flags representable in NZCV */
} Insn;

static const uint8_t modrm_tab[256] = {
    /* 0 */ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
    /* 1 */ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
    /* 2 */ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
    /* 3 */ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
    /* 4 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 5 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 6 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 7 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 8 */ 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    /* 9 */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* A */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* B */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* C */ 0,0,0,0,1,1,1,1, 0,0,0,0,0,0,0,0,
    /* D */ 1,1,1,1,0,0,0,0, 1,1,1,1,1,1,1,1,
    /* E */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* F */ 0,0,0,0,0,0,1,1, 0,0,0,0,0,0,1,1,
};

static inline uint8_t fb(B86Cpu *c, uint32_t cs, uint32_t ip)
{
    return c->mem[(cs << 4) + (ip & 0xFFFFu)];
}

/* immediate bytes following modrm/opcode (8086 map) */
static int imm_size(uint8_t op, uint8_t reg)
{
    if (op < 0x40 && (op & 7) == 4) return 1;
    if (op < 0x40 && (op & 7) == 5) return 2;
    switch (op) {
    case 0x80: case 0x82: case 0x83: case 0xA8: case 0xC6: case 0xCD:
    case 0xD4: case 0xD5: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xEB: case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        return 1;
    case 0x81: case 0xA9: case 0xC7: case 0xC0: case 0xC2: case 0xC8: case 0xCA:
    case 0xE8: case 0xE9: case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        return 2;
    case 0x9A: case 0xEA: return 4;
    case 0xF6: return reg < 2 ? 1 : 0;
    case 0xF7: return reg < 2 ? 2 : 0;
    }
    if (op >= 0x60 && op <= 0x7F) return 1;
    if (op >= 0xB0 && op <= 0xB7) return 1;
    if (op >= 0xB8 && op <= 0xBF) return 2;
    return 0;
}

static void decode(B86Cpu *c, uint32_t cs, uint16_t ip, Insn *d)
{
    memset(d, 0, sizeof *d);
    d->ip = ip;
    d->seg = 0xFF;
    uint32_t p = ip;
    uint8_t op;
    for (;;) {
        op = fb(c, cs, p++);
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) { d->seg = (op >> 3) & 3; d->nprefix++; continue; }
        if (op == 0xF2 || op == 0xF3) { d->rep = op; d->nprefix++; continue; }
        if (op == 0xF0 || op == 0xF1) { d->nprefix++; continue; }
        break;
    }
    d->op = op;
    d->w = op & 1;
    if (modrm_tab[op]) {
        uint8_t m = fb(c, cs, p++);
        d->has_modrm = 1;
        d->mod = m >> 6; d->reg = (m >> 3) & 7; d->rm = m & 7;
        if (d->mod == 0 && d->rm == 6) { d->disp = (int16_t)(fb(c, cs, p) | fb(c, cs, p + 1) << 8); p += 2; }
        else if (d->mod == 1) { d->disp = (int8_t)fb(c, cs, p); p += 1; }
        else if (d->mod == 2) { d->disp = (int16_t)(fb(c, cs, p) | fb(c, cs, p + 1) << 8); p += 2; }
    }
    int n = imm_size(op, d->reg);
    if (n == 1) d->imm = fb(c, cs, p);
    else if (n >= 2) d->imm = (uint16_t)(fb(c, cs, p) | fb(c, cs, p + 1) << 8);
    if (n == 4) d->imm2 = (uint16_t)(fb(c, cs, p + 2) | fb(c, cs, p + 3) << 8);
    p += (uint32_t)n;
    d->next = (uint16_t)p;
    if (p > 0xFFFFu) d->cls = C_END;    /* crosses the segment end: helper */
}

/* x86 condition -> flags it reads */
static const uint16_t cc_use[8] = {
    B86_OF, B86_CF, B86_ZF, B86_CF | B86_ZF, B86_SF, B86_PF,
    B86_SF | B86_OF, B86_SF | B86_OF | B86_ZF
};

/* Classify: control class, flag use/def and whether we lower natively. */
static void classify(Insn *d)
{
    uint8_t op = d->op;
    int memop = d->has_modrm && d->mod != 3;
    d->fuse = ALLF; d->fdef = 0; d->native = 0;
    if (d->cls == C_END) return;
    if (d->nprefix > 3) { return; }

    if (op < 0x40 && (op & 7) < 6) {
        int a = op >> 3;
        d->fdef = ALLF; d->fuse = (a == 2 || a == 3) ? B86_CF : 0;
        d->native = (a != 2 && a != 3) && !d->rep;
        return;
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: d->fuse = 0; d->native = 1; return;
    case 0x07: case 0x17: case 0x1F: d->fuse = 0; d->native = 1; return;
    case 0x0F: d->cls = C_END; return;
    case 0x27: case 0x2F: d->fuse = B86_AF | B86_CF; d->fdef = ALLF & ~B86_OF; return;
    case 0x37: case 0x3F: d->fuse = B86_AF; d->fdef = B86_AF | B86_CF; return;
    case 0x9C: d->fuse = ALLF; return;
    case 0x9D: d->fuse = 0; d->fdef = ALLF; return;
    case 0x9E: d->fuse = 0; d->fdef = ALLF & ~B86_OF; return;
    case 0x9F: d->fuse = ALLF & ~B86_OF; return;
    case 0xF5: d->fuse = B86_CF; d->fdef = B86_CF; return;
    case 0xF8: case 0xF9: d->fuse = 0; d->fdef = B86_CF; return;
    case 0xFA: case 0xFB: case 0xFC: case 0xFD: d->fuse = 0; d->native = 1; return;
    case 0xD6: d->fuse = B86_CF; return;
    case 0xD5: d->fuse = 0; d->fdef = ALLF; return;
    case 0x90: d->fuse = 0; d->native = 1; return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: d->fuse = 0; d->native = 1; return;
    case 0x84: case 0x85: case 0xA8: case 0xA9: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
    case 0x86: d->fuse = 0; return;
    case 0x87: d->fuse = 0; d->native = !memop; return;
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0xC6: case 0xC7:
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        d->fuse = 0; d->native = 1; return;
    case 0x8C: d->fuse = 0; d->native = 1; return;
    case 0x8D: d->fuse = 0; d->native = memop; return;
    case 0x8E: if ((d->reg & 3) == B86_CS) { d->cls = C_END; return; }
        d->fuse = 0; d->native = 1; return;
    case 0x8F: d->fuse = 0; return;
    case 0xC4: case 0xC5: case 0xD7: d->fuse = 0; return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
    case 0x9B: d->fuse = 0; return;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        d->fuse = 0; return;
    case 0x80: case 0x81: case 0x82: case 0x83:
        d->fdef = ALLF; d->fuse = (d->reg == 2 || d->reg == 3) ? B86_CF : 0;
        d->native = (d->reg != 2 && d->reg != 3); return;
    case 0xF6: case 0xF7:
        switch (d->reg) {
        case 0: case 1: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
        case 2: d->fuse = 0; d->native = 1; return;
        case 3: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
        case 4: case 5: d->fuse = 0; d->fdef = ALLF & ~B86_AF; return;  /* interp leaves AF */
        default: d->fuse = ALLF; return;     /* DIV may fault: INT pushes flags */
        }
    case 0xFE:
        if (d->reg < 2) { d->fuse = 0; d->fdef = ALLF & ~B86_CF; d->native = 1; }
        return;
    case 0xFF:
        switch (d->reg) {
        case 0: case 1: d->fuse = 0; d->fdef = ALLF & ~B86_CF; d->native = 1; return;
        case 2: d->cls = C_IND; d->fuse = ALLF; d->native = be_has_t2 && !(d->mod == 3 && d->rm == B86_SP); return;
        case 4: d->cls = C_IND; d->fuse = ALLF; d->native = 1; return;
        case 3: case 5: d->cls = C_END; return;
        default: d->fuse = 0; d->native = !(d->mod == 3 && d->rm == B86_SP); return;
        }
    case 0xD0: case 0xD1:
        d->fuse = (d->reg == 2 || d->reg == 3) ? B86_CF : 0;
        d->fdef = d->reg < 4 ? (B86_CF | B86_OF) : ALLF;
        d->native = (d->reg == 4 || d->reg == 5 || d->reg == 7); /* only if flags dead */
        return;
    case 0xD2: case 0xD3: /* count may be 0: defines nothing for sure */
        d->fuse = (d->reg == 2 || d->reg == 3) ? B86_CF : 0; d->fdef = 0; return;
    case 0xD4: d->fuse = ALLF; d->fdef = 0; return;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        d->fuse = 0; return;
    case 0xA6: case 0xA7: case 0xAE: case 0xAF:
        d->fuse = 0; d->fdef = d->rep ? 0 : ALLF; return;
    case 0xE0: case 0xE1: d->cls = C_JCC; d->target = (uint16_t)(d->next + (int8_t)d->imm);
        d->fuse = B86_ZF; d->native = 1; d->nzclob = 1; return;
    case 0xE2: case 0xE3: d->cls = C_JCC; d->target = (uint16_t)(d->next + (int8_t)d->imm);
        d->fuse = 0; d->native = 1; d->nzclob = 1; return;
    case 0xE8: d->cls = C_CALL; d->target = (uint16_t)(d->next + d->imm); d->fuse = 0; d->native = 1; return;
    case 0xE9: d->cls = C_JMP; d->target = (uint16_t)(d->next + d->imm); d->fuse = 0; d->native = 1; return;
    case 0xEB: d->cls = C_JMP; d->target = (uint16_t)(d->next + (int8_t)d->imm); d->fuse = 0; d->native = 1; return;
    case 0xC2: case 0xC0: case 0xC3: case 0xC1: d->cls = C_RET; d->fuse = ALLF; d->native = 1; return;
    case 0xF4: d->cls = C_HLT; d->fuse = ALLF; d->native = 1; return;
    case 0x9A: case 0xCA: case 0xC8: case 0xCB: case 0xC9: case 0xCC: case 0xCD:
    case 0xCE: case 0xCF: case 0xEA:
        d->cls = C_END; return;
    }
    if (op >= 0x40 && op <= 0x4F) { d->fuse = 0; d->fdef = ALLF & ~B86_CF; d->native = 1; return; }
    if (op >= 0x50 && op <= 0x5F) { d->fuse = 0; d->native = 1; return; }
    if (op >= 0x60 && op <= 0x7F) {
        d->cls = C_JCC; d->target = (uint16_t)(d->next + (int8_t)d->imm);
        d->fuse = cc_use[(op & 15) >> 1]; d->native = 1; return;
    }
    if (op >= 0xB0 && op <= 0xBF) { d->fuse = 0; d->native = 1; return; }
}

/* ------------------------------------------------------------------------ */
/* Flag liveness                                                            */
/* ------------------------------------------------------------------------ */

static int ends_block(const Insn *d)
{
    return d->cls == C_JMP || d->cls == C_CALL || d->cls == C_RET ||
           d->cls == C_IND || d->cls == C_END || d->cls == C_HLT;
}

/* Flags live on entry to `ip`: scan forward until every flag is defined.
   The scanned bytes join the block's guarded range (so SMC there also
   invalidates this block); if that would exceed one SMC page we give up
   and assume every flag is live. */
typedef struct Guard { uint32_t lo, hi; } Guard;

static uint16_t lookahead(J *j, uint32_t cs, uint16_t ip, Guard *g)
{
    if (j->no_lookahead) return ALLF;
    uint16_t need = 0, defd = 0, res = 0;
    uint16_t p = ip;
    int done = 0;
    for (int i = 0; i < 12 && !done; ++i) {
        Insn d;
        decode(j->cpu, cs, p, &d);
        classify(&d);
        if (d.next < p) return ALLF;
        need |= d.fuse & ~defd;
        if (d.cls == C_JCC || ends_block(&d)) { res = (uint16_t)(need | (ALLF & ~defd)); p = d.next; done = 1; break; }
        defd |= d.fdef;
        p = d.next;
        if ((defd & ALLF) == ALLF) { res = need; done = 1; }
    }
    if (!done) res = (uint16_t)(need | (ALLF & ~defd));
    if (res == ALLF) return ALLF;
    uint32_t lo = (cs << 4) + ip, hi = (cs << 4) + p;
    uint32_t nlo = lo < g->lo ? lo : g->lo, nhi = hi > g->hi ? hi : g->hi;
    if (nhi - nlo > 512u) return ALLF;
    g->lo = nlo; g->hi = nhi;
    return res;
}

static uint16_t live_before(const Insn *d)
{
    uint16_t after = d->live | (d->cls == C_JCC ? d->live_taken : 0);
    return (uint16_t)((after & ~d->fdef) | d->fuse);
}

/* NZCV interpretation produced by a producer insn, and which x86 flags it
   represents. Returns 0 if the producer cannot feed NZCV. */
static int producer_class(const Insn *d, uint8_t *valid)
{
    uint8_t op = d->op;
    int xop = -1;
    if (op < 0x40 && (op & 7) < 6) xop = op >> 3;
    else if (op >= 0x80 && op <= 0x83) xop = d->reg;
    else if (op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) xop = 4;
    else if ((op == 0xF6 || op == 0xF7) && d->reg < 2) xop = 4;
    else if ((op == 0xF6 || op == 0xF7) && d->reg == 3) { *valid = 0xF; return FM_SUB; }
    else if ((op >= 0x40 && op <= 0x47) || ((op == 0xFE || op == 0xFF) && d->reg == 0)) { *valid = 0x7; return FM_ADD; }
    else if ((op >= 0x48 && op <= 0x4F) || ((op == 0xFE || op == 0xFF) && d->reg == 1)) { *valid = 0x7; return FM_SUB; }
    if (xop < 0) return 0;
    *valid = 0xF; /* bit0 OF bit1 SF bit2 ZF bit3 CF */
    switch (xop) {
    case 0: return FM_ADD;
    case 5: case 7: return FM_SUB;
    case 1: case 4: case 6: return be_logic_mode;
    default: return 0;
    }
}

static uint8_t flagbits(uint16_t f)
{
    return (uint8_t)(((f & B86_OF) ? 1 : 0) | ((f & B86_SF) ? 2 : 0) |
                     ((f & B86_ZF) ? 4 : 0) | ((f & B86_CF) ? 8 : 0) |
                     ((f & (B86_PF | B86_AF)) ? 0x80 : 0));
}

static int arm_cond(int mode, int cc)
{
    static const signed char sub_t[16] = { AC_VS, AC_VC, AC_CC, AC_CS, AC_EQ, AC_NE, AC_LS, AC_HI,
                                           AC_MI, AC_PL, -1, -1, AC_LT, AC_GE, AC_LE, AC_GT };
    static const signed char add_t[16] = { AC_VS, AC_VC, AC_CS, AC_CC, AC_EQ, AC_NE, -1, -1,
                                           AC_MI, AC_PL, -1, -1, AC_LT, AC_GE, AC_LE, AC_GT };
    return mode == FM_SUB ? sub_t[cc] : mode == FM_ADD ? add_t[cc] : -1;
}

static int is_mem_dst_producer(const Insn *d)
{
    uint8_t op = d->op;
    if (!d->has_modrm || d->mod == 3) return 0;
    if (op < 0x40 && (op & 7) < 2) return (op >> 3) != 7;
    if (op >= 0x80 && op <= 0x83) return d->reg != 7;
    if (op == 0xF6 || op == 0xF7) return d->reg == 3;
    if (op == 0xFE || op == 0xFF) return d->reg < 2;
    return 0;
}

/* native lowering performs a checked guest store */
static int checked_store(const Insn *d)
{
    uint8_t op = d->op;
    if (op == 0xA2 || op == 0xA3) return 1;
    if (!d->has_modrm || d->mod == 3) return 0;
    if (is_mem_dst_producer(d)) return 1;
    switch (op) {
    case 0x88: case 0x89: case 0xC6: case 0xC7: case 0x8C: case 0xD0: case 0xD1: return 1;
    case 0xF6: case 0xF7: return d->reg == 2;
    }
    return 0;
}

static int is_jcc(const Insn *d) { return d->op >= 0x60 && d->op <= 0x7F && d->native; }

static uint16_t live_after(const Insn *d)
{
    return (uint16_t)(d->live | (d->cls == C_JCC ? d->live_taken : 0));
}

/* Can instruction m (between a producer and its consumer) destroy NZCV?
   Conservative: any native producer whose flags are live may itself
   become an NZCV producer. */
static int nz_clobber(const Insn *m)
{
    if (!m->native || m->nzclob) return 1;
    if (be_store_clobbers_nzcv && checked_store(m)) return 1;
    if (is_jcc(m) && !m->fused) return 1;
    if (m->fdef && (live_after(m) & m->fdef)) return 1;
    return 0;
}

static void analyze(J *j, uint32_t cs, Insn *v, int n, uint16_t live_out, Guard *g)
{
    uint16_t live = live_out;
    for (int i = n - 1; i >= 0; --i) {
        Insn *d = &v[i];
        if (d->cls == C_JCC) d->live_taken = lookahead(j, cs, d->target, g);
        d->live = live;
        live = live_before(d);
    }
    /* Pass A: fuse each native Jcc with its producer through NZCV. */
    for (int q = 0; q < n; ++q) {
        Insn *c = &v[q];
        if (!is_jcc(c)) continue;
        uint16_t use = cc_use[(c->op & 15) >> 1];
        int p = -1;
        for (int m = q - 1; m >= 0; --m) {
            if (v[m].fdef & use) { p = m; break; }
            if (nz_clobber(&v[m])) break;
        }
        if (p < 0) continue;
        Insn *pr = &v[p];
        uint8_t valid = 0;
        int mode = pr->native ? producer_class(pr, &valid) : 0;
        if (!mode || (pr->fdef & use) != use) continue;
        if ((flagbits(use) & ~valid) != 0) continue;
        int ac = arm_cond(mode, c->op & 15);
        if (ac < 0) continue;
        if (is_mem_dst_producer(pr) && be_store_clobbers_nzcv) continue;
        c->fused = 1; c->mode = (uint8_t)ac; c->prod = (uint8_t)p;
        pr->arm = 1; pr->mode = (uint8_t)mode;
    }
    /* Pass B: lazy record unless every reader is a fused consumer. */
    for (int i = 0; i < n; ++i) {
        Insn *p = &v[i];
        if (!p->native || !p->fdef) continue;
        uint16_t rem = p->fdef & live_after(p);
        if (!rem) continue;
        /* An SMC store that kills its own block exits right after the store,
           so flags held only in NZCV across a checked store need the record. */
        if (checked_store(p) && p->arm) p->lazy = 1;
        int k;
        for (k = i + 1; k < n && rem; ++k) {
            Insn *q = &v[k];
            if (q->native && checked_store(q) && (live_after(q) & rem)) p->lazy = 1;
            if ((q->fuse & rem) && !(q->fused && q->prod == i)) p->lazy = 1;
            if (q->cls == C_JCC && (q->live_taken & rem)) p->lazy = 1;
            if (!q->native && (live_before(q) & rem)) p->lazy = 1;
            rem &= (uint16_t)~q->fdef;
        }
        if (rem && (v[n - 1].live & rem)) p->lazy = 1;
    }
}

/* ------------------------------------------------------------------------ */
/* Lowering helpers                                                         */
/* ------------------------------------------------------------------------ */

typedef struct Side { uint8_t *site; uint16_t target; uint8_t poll; uint32_t retire; } Side;

typedef struct Tx {
    J *j;
    Emit *e;
    uint32_t cs;
    Side side[64];
    int nside;
    int fail;
    int inc_pending;            /* an INC/DEC overlay record may be live */
} Tx;

static void add_side(Tx *t, uint8_t *site, uint16_t target, uint16_t from)
{
    if (t->nside >= 64) { t->fail = 1; return; }
    t->side[t->nside].site = site;
    t->side[t->nside].target = target;
    t->side[t->nside].poll = target <= from;
    t->side[t->nside].retire = t->e->retire;
    t->nside++;
}

/* EA of modrm memory operand into V_T0 */
static void ea_modrm(Tx *t, const Insn *d)
{
    static const signed char b1[8] = { B86_BX, B86_BX, B86_BP, B86_BP, B86_SI, B86_DI, B86_BP, B86_BX };
    static const signed char b2[8] = { B86_SI, B86_DI, B86_SI, B86_DI, -1, -1, -1, -1 };
    static const uint8_t ds[8] = { B86_DS, B86_DS, B86_SS, B86_SS, B86_DS, B86_DS, B86_SS, B86_DS };
    int base1 = b1[d->rm], base2 = b2[d->rm], seg = ds[d->rm];
    if (d->mod == 0 && d->rm == 6) { base1 = -1; seg = B86_DS; }
    if (d->seg != 0xFF) seg = d->seg;
    be_ea(t->e, V_T0, seg, base1, base2, d->disp);
}

static void ea_off(Tx *t, const Insn *d, int dst)
{
    static const signed char b1[8] = { B86_BX, B86_BX, B86_BP, B86_BP, B86_SI, B86_DI, B86_BP, B86_BX };
    static const signed char b2[8] = { B86_SI, B86_DI, B86_SI, B86_DI, -1, -1, -1, -1 };
    int base1 = b1[d->rm], base2 = b2[d->rm];
    if (d->mod == 0 && d->rm == 6) base1 = -1;
    be_ea_off(t->e, dst, base1, base2, d->disp);
}

/* get byte register r8 (0..7) as a vreg holding it in low bits */
static int get8(Tx *t, int r8, int tmp)
{
    if (r8 < 4) return r8;
    be_ubfx(t->e, tmp, r8 - 4, 8, 8);
    return tmp;
}
static void put8(Tx *t, int r8, int v)
{
    if (r8 < 4) be_bfi(t->e, r8, v, 0, 8);
    else be_bfi(t->e, r8 - 4, v, 8, 8);
}

enum { O_R16, O_R8, O_MEM, O_IMM };
typedef struct { int k, r; uint32_t imm; } Op;

static int lz_kind(int xop, int w, int incdec)
{
    if (incdec == 1) return w ? LZ_INC16 : LZ_INC8;
    if (incdec == 2) return w ? LZ_DEC16 : LZ_DEC8;
    if (xop == 0) return w ? LZ_ADD16 : LZ_ADD8;
    if (xop == 5 || xop == 7) return w ? LZ_SUB16 : LZ_SUB8;
    return w ? LZ_LOG16 : LZ_LOG8;
}

/* Generic two-operand ALU: xop 0 ADD 1 OR 4 AND 5 SUB 6 XOR 7 CMP 8 TEST.
   incdec: 1 INC, 2 DEC (src = imm 1). Registers are planned first; nothing
   is emitted unless the whole instruction can be lowered (a partial lazy
   record followed by a helper fallback would corrupt the flags). */
typedef struct Pool { int busy[11]; } Pool;
static int pick(Pool *p)
{
    int lim = be_has_t2 ? V_T2 : V_T1;
    for (int r = V_T0; r <= lim; ++r) if (!p->busy[r]) { p->busy[r] = 1; return r; }
    return -1;
}

static int emit_alu(Tx *t, Insn *d, int xop, int w, Op dst, Op src, int incdec)
{
    Emit *e = t->e;
    int sh = w ? 16 : 24;
    int writes = xop != 7 && xop != 8;
    int logic = xop == 1 || xop == 4 || xop == 6 || xop == 8;
    int sub = xop == 5 || xop == 7;
    int need_flags = d->arm || d->lazy;
    int aop = xop == 0 ? AOP_ADD : sub ? AOP_SUB : xop == 1 ? AOP_ORR : xop == 4 || xop == 8 ? AOP_AND : AOP_EOR;
    int mem = dst.k == O_MEM || src.k == O_MEM;
    uint32_t imm = src.imm;

    if (!writes && !need_flags) return 1;           /* dead CMP/TEST */

    /* ---- plan ---- */
    Pool pool = {{0}};
    int a = -1, b = -1, ax = -1, bx = -1, res, x, tmp = -1;
    if (dst.k == O_MEM) { pool.busy[V_T0] = 1; pool.busy[V_T1] = 1; a = V_T1; }
    else if (src.k == O_MEM) { pool.busy[V_T1] = 1; b = V_T1; }   /* T0 free after load */
    if (dst.k == O_R16) a = dst.r;
    else if (dst.k == O_R8) {
        if (dst.r < 4) a = dst.r;
        else { a = pick(&pool); ax = dst.r - 4; if (a < 0) return 0; }
    }
    if (src.k == O_R16) b = src.r;
    else if (src.k == O_R8) {
        if (src.r < 4) b = src.r;
        else { b = pick(&pool); bx = src.r - 4; if (b < 0) return 0; }
    }
    if (dst.k == O_MEM && !writes) pool.busy[V_T0] = 0;       /* no store: address dies after load */
    /* CMP feeding both NZCV and a lazy record recomputes a - b after the
       shifted compare, so its scratch must not alias a. */
    int keep_a = !writes && d->arm && d->lazy && !logic;
    if (dst.k == O_R16 && writes) res = dst.r;
    else if (dst.k == O_MEM && writes) res = V_T1;
    else if (a >= V_T0 && !keep_a) res = a;
    else { res = pick(&pool); if (res < 0) return 0; }
    if (res >= V_T0) x = res;
    else { x = pick(&pool); if (x < 0) return 0; }
    if (src.k == O_IMM && d->arm && !logic && !be_imm_ok_sh(imm, sh)) {
        tmp = pick(&pool);
        if (tmp < 0) return 0;
    }
    if (d->arm && logic && res < V_T0) { /* test_res needs a scratch: x */ }

    /* ---- emit ---- */
    unsigned o_kind = incdec ? OFF(lz_ikind) : OFF(lz_kind);
    unsigned o_a = incdec ? OFF(lz_ia) : OFF(lz_a);
    unsigned o_res = incdec ? OFF(lz_ires) : OFF(lz_res);
    if (mem) ea_modrm(t, d);
    if (d->lazy) {
        be_stctx_imm(e, (uint32_t)lz_kind(xop, w, incdec), o_kind);
        if (incdec) t->inc_pending = 1;
        else if (t->inc_pending) { be_stctx_imm(e, LZ_NONE, OFF(lz_ikind)); t->inc_pending = 0; }
    }
    if (mem) be_load(e, w, V_T1, V_T0);
    if (ax >= 0) be_ubfx(e, a, ax, 8, 8);
    if (bx >= 0) be_ubfx(e, b, bx, 8, 8);
    if (d->lazy && !logic) be_stctx(e, a, o_a);

    if (logic) {
        int r = writes ? res : x;
        if (src.k == O_IMM) be_opi(e, aop, r, a, imm); else be_op(e, aop, r, a, b);
        if (d->lazy) be_stctx(e, r, o_res);
        if (dst.k == O_R8 && writes) put8(t, dst.r, r);
        if (d->arm) be_test_res(e, (r >= V_T0) ? r : x, r, sh);
    } else if (d->arm) {
        if (src.k == O_IMM) {
            if (writes) be_addsubi_sh(e, sub, x, res, a, imm, sh, tmp);
            else be_cmpi_sh(e, x, a, imm, sh, tmp);
        } else {
            if (writes) be_addsub_sh(e, sub, x, res, a, b, sh);
            else be_cmp_sh(e, x, a, b, sh);
        }
        if (d->lazy) {
            if (!writes) {
                if (src.k == O_IMM) be_opi(e, AOP_SUB, x, a, imm); else be_op(e, AOP_SUB, x, a, b);
                be_stctx(e, x, o_res);
            } else be_stctx(e, res, o_res);
        }
        if (dst.k == O_R8 && writes) put8(t, dst.r, res);
    } else {
        int r = writes ? res : x;
        if (src.k == O_IMM) be_opi(e, aop, r, a, imm); else be_op(e, aop, r, a, b);
        if (d->lazy) be_stctx(e, r, o_res);
        if (dst.k == O_R8 && writes) put8(t, dst.r, r);
    }
    if (dst.k == O_MEM && writes) be_store(e, w, V_T1, V_T0, 1, d->next);
    return 1;
}

static void emit_push(Tx *t, int v, uint16_t next)
{
    be_opi(t->e, AOP_SUB, B86_SP, B86_SP, 2);
    be_ea(t->e, V_T0, B86_SS, B86_SP, -1, 0);
    be_store(t->e, 1, v, V_T0, 0, next);
}

static void emit_pop_to(Tx *t, int dst)
{
    be_ea(t->e, V_T0, B86_SS, B86_SP, -1, 0);
    if (dst == B86_SP) { be_load(t->e, 1, B86_SP, V_T0); return; }
    be_load(t->e, 1, dst, V_T0);
    be_opi(t->e, AOP_ADD, B86_SP, B86_SP, 2);
}

static Op op_rm(const Insn *d, int w)
{
    Op o = { O_MEM, 0, 0 };
    if (d->mod == 3) { o.k = w ? O_R16 : O_R8; o.r = d->rm; }
    return o;
}
static Op op_reg(const Insn *d, int w) { Op o = { w ? O_R16 : O_R8, d->reg, 0 }; return o; }
static Op op_imm(uint32_t v) { Op o = { O_IMM, 0, v }; return o; }

/* Load a r/m operand (w) into vreg dst (low bits valid). */
static void load_rm(Tx *t, const Insn *d, int w, int dst)
{
    if (d->mod == 3) {
        if (w) be_mov(t->e, dst, d->rm);
        else if (d->rm < 4) be_mov(t->e, dst, d->rm);
        else be_ubfx(t->e, dst, d->rm - 4, 8, 8);
        return;
    }
    ea_modrm(t, d);
    be_load(t->e, w, dst, V_T0);
}

/* Lower one instruction natively. Returns 0 to request the helper. */
static int lower(Tx *t, Insn *d)
{
    Emit *e = t->e;
    uint8_t op = d->op;
    int w = d->w;

    if (op < 0x40 && (op & 7) < 6) {
        int xop = op >> 3;
        switch (op & 7) {
        case 0: case 1: return emit_alu(t, d, xop, w, op_rm(d, w), op_reg(d, w), 0);
        case 2: case 3: return emit_alu(t, d, xop, w, op_reg(d, w), op_rm(d, w), 0);
        case 4: { Op a = { O_R8, 0, 0 }; return emit_alu(t, d, xop, 0, a, op_imm(d->imm), 0); }
        default: { Op a = { O_R16, 0, 0 }; return emit_alu(t, d, xop, 1, a, op_imm(d->imm), 0); }
        }
    }
    if (op >= 0x80 && op <= 0x83) {
        uint32_t imm = op == 0x83 ? (uint16_t)(int8_t)d->imm : d->imm;
        return emit_alu(t, d, d->reg, op & 1, op_rm(d, op & 1), op_imm(imm), 0);
    }
    if (op >= 0x40 && op <= 0x4F) {
        Op r = { O_R16, op & 7, 0 };
        int dec = op >= 0x48;
        return emit_alu(t, d, dec ? 5 : 0, 1, r, op_imm(1), dec ? 2 : 1);
    }
    if ((op == 0xFE || op == 0xFF) && d->reg < 2) {
        return emit_alu(t, d, d->reg ? 5 : 0, w, op_rm(d, w), op_imm(1), d->reg ? 2 : 1);
    }
    if (op >= 0x50 && op <= 0x57) { emit_push(t, op & 7, d->next); return 1; }
    if (op >= 0x58 && op <= 0x5F) { emit_pop_to(t, op & 7); return 1; }
    if (op >= 0xB0 && op <= 0xB7) { be_movi(e, V_T0, d->imm); put8(t, op & 7, V_T0); return 1; }
    if (op >= 0xB8 && op <= 0xBF) { be_movi(e, op & 7, d->imm); return 1; }
    if ((op >= 0x60 && op <= 0x7F)) {
        int cc = op & 15;
        uint8_t *site;
        if (d->fused) {
            site = be_jcc(e, d->mode);      /* ARM condition from analysis */
        } else {
            be_call_cond(e, cc);
            t->inc_pending = 0;
            site = be_cbnz(e, V_T0);
        }
        add_side(t, site, d->target, d->ip);
        return 1;
    }

    switch (op) {
    case 0x84: case 0x85: return emit_alu(t, d, 8, w, op_rm(d, w), op_reg(d, w), 0);
    case 0xA8: { Op a = { O_R8, 0, 0 }; return emit_alu(t, d, 8, 0, a, op_imm(d->imm), 0); }
    case 0xA9: { Op a = { O_R16, 0, 0 }; return emit_alu(t, d, 8, 1, a, op_imm(d->imm), 0); }
    case 0xF6: case 0xF7:
        if (d->reg < 2) return emit_alu(t, d, 8, w, op_rm(d, w), op_imm(d->imm), 0);
        if (d->reg == 2) {           /* NOT */
            if (d->mod == 3) {
                if (w) be_mvn(e, d->rm, d->rm);
                else be_opi(e, AOP_EOR, d->rm & 3, d->rm & 3, d->rm < 4 ? 0xFFu : 0xFF00u);
                return 1;
            }
            ea_modrm(t, d);
            be_load(e, w, V_T1, V_T0);
            be_mvn(e, V_T1, V_T1);
            be_store(e, w, V_T1, V_T0, 1, d->next);
            return 1;
        }
        if (d->reg == 3) {           /* NEG */
            int sh = w ? 16 : 24;
            int mem = d->mod != 3;
            if (mem) ea_modrm(t, d);
            if (d->lazy) {
                be_stctx_imm(e, w ? LZ_SUB16 : LZ_SUB8, OFF(lz_kind)); be_stctx_imm(e, 0, OFF(lz_a));
                if (t->inc_pending) { be_stctx_imm(e, LZ_NONE, OFF(lz_ikind)); t->inc_pending = 0; }
            }
            int src, res;
            if (mem) { be_load(e, w, V_T1, V_T0); src = res = V_T1; }
            else if (w) { src = res = d->rm; }
            else { src = get8(t, d->rm, V_T0); res = V_T1; }
            if (d->arm) be_neg_sh(e, (res < 8) ? V_T0 : res, res, src, sh);
            else be_neg(e, res, src);
            if (d->lazy) be_stctx(e, res, OFF(lz_res));
            if (mem) be_store(e, w, V_T1, V_T0, 1, d->next);
            else if (!w) put8(t, d->rm, res);
            return 1;
        }
        return 0;
    case 0x86: return 0;
    case 0x87: /* reg-reg XCHG */
        if (d->reg == d->rm) return 1;
        be_mov(e, V_T0, d->reg); be_mov(e, d->reg, d->rm); be_mov(e, d->rm, V_T0);
        return 1;
    case 0x88: case 0x89:
        if (d->mod == 3) {
            if (w) be_mov(e, d->rm, d->reg);
            else put8(t, d->rm, get8(t, d->reg, V_T0));
            return 1;
        }
        ea_modrm(t, d);
        if (w || d->reg < 4) be_store(e, w, d->reg, V_T0, 1, d->next);
        else { be_ubfx(e, V_T1, d->reg - 4, 8, 8); be_store(e, 0, V_T1, V_T0, 1, d->next); }
        return 1;
    case 0x8A: case 0x8B:
        if (d->mod == 3) {
            if (w) be_mov(e, d->reg, d->rm);
            else put8(t, d->reg, get8(t, d->rm, V_T0));
            return 1;
        }
        ea_modrm(t, d);
        if (w) be_load(e, 1, d->reg, V_T0);
        else { be_load(e, 0, V_T1, V_T0); put8(t, d->reg, V_T1); }
        return 1;
    case 0xC6: case 0xC7:
        if (d->mod == 3) {
            if (w) be_movi(e, d->rm, d->imm);
            else { be_movi(e, V_T0, d->imm); put8(t, d->rm, V_T0); }
            return 1;
        }
        ea_modrm(t, d);
        be_movi(e, V_T1, d->imm);
        be_store(e, w, V_T1, V_T0, 1, d->next);
        return 1;
    case 0xA0: case 0xA1:
        be_ea(e, V_T0, d->seg != 0xFF ? d->seg : B86_DS, -1, -1, (int16_t)d->imm);
        if (w) be_load(e, 1, B86_AX, V_T0);
        else { be_load(e, 0, V_T1, V_T0); put8(t, 0, V_T1); }
        return 1;
    case 0xA2: case 0xA3:
        be_ea(e, V_T0, d->seg != 0xFF ? d->seg : B86_DS, -1, -1, (int16_t)d->imm);
        be_store(e, w, B86_AX, V_T0, 1, d->next);
        return 1;
    case 0x8C:
        if (d->mod == 3) { be_ldctx(e, d->rm, OFF(seg[d->reg & 3])); return 1; }
        ea_modrm(t, d);
        be_ldctx(e, V_T1, OFF(seg[d->reg & 3]));
        be_store(e, 1, V_T1, V_T0, 1, d->next);
        return 1;
    case 0x8E:
        if (d->mod == 3) { be_set_seg(e, d->reg & 3, d->rm); return 1; }
        ea_modrm(t, d);
        be_load(e, 1, V_T1, V_T0);
        be_set_seg(e, d->reg & 3, V_T1);
        return 1;
    case 0x8D: ea_off(t, d, d->reg); return 1;
    case 0x90: return 1;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        be_mov(e, V_T0, B86_AX); be_mov(e, B86_AX, op & 7); be_mov(e, op & 7, V_T0);
        return 1;
    case 0x98: be_sxtb(e, B86_AX, B86_AX); return 1;
    case 0x99: be_sbfx(e, B86_DX, B86_AX, 15, 1); return 1;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        be_ldctx(e, V_T1, OFF(seg[op >> 3]));
        emit_push(t, V_T1, d->next);
        return 1;
    case 0x07: case 0x17: case 0x1F:
        emit_pop_to(t, V_T1);
        be_set_seg(e, op >> 3, V_T1);
        return 1;
    case 0xFA: case 0xFB: case 0xFC: case 0xFD: {
        uint32_t bit = op < 0xFC ? B86_IF : B86_DF;
        be_ldctx(e, V_T0, OFF(flags));
        if (op & 1) be_opi(e, AOP_ORR, V_T0, V_T0, bit);
        else be_opi(e, AOP_AND, V_T0, V_T0, (uint16_t)~bit);
        be_stctx(e, V_T0, OFF(flags));
        return 1; }
    case 0xD0: case 0xD1: {
        if (d->live & d->fdef) return 0;     /* flags needed: helper */
        int src, dst;
        int mem = d->mod != 3;
        if (mem) { ea_modrm(t, d); be_load(e, w, V_T1, V_T0); src = dst = V_T1; }
        else if (w) { src = dst = d->rm; }
        else { src = get8(t, d->rm, V_T1); dst = V_T1; }
        unsigned bits = w ? 16 : 8;
        if (d->reg == 4) be_lsl(e, dst, src, 1);
        else if (d->reg == 5) be_ubfx(e, dst, src, 1, bits - 1);
        else be_sbfx(e, dst, src, 1, bits - 1);
        if (mem) be_store(e, w, V_T1, V_T0, 1, d->next);
        else if (!w) put8(t, d->rm, dst);
        return 1; }
    case 0xE2: /* LOOP */
        be_opi(e, AOP_SUB, B86_CX, B86_CX, 1);
        add_side(t, be_cbnz16(e, B86_CX), d->target, d->ip);
        return 1;
    case 0xE3: /* JCXZ */
        add_side(t, be_cbz16(e, B86_CX), d->target, d->ip);
        return 1;
    case 0xE0: case 0xE1: { /* LOOPNZ / LOOPZ */
        be_opi(e, AOP_SUB, B86_CX, B86_CX, 1);
        be_call_cond(e, op == 0xE1 ? 4 : 5);   /* E / NE */
        t->inc_pending = 0;
        uint8_t *skip = be_cbz16(e, B86_CX);
        add_side(t, be_cbnz(e, V_T0), d->target, d->ip);
        be_bind(e, skip, e->p);
        return 1; }
    case 0xE8: /* CALL near: push return, chain to target */
        be_movi(e, V_T1, d->next);
        emit_push(t, V_T1, d->next);
        be_exit_chain(e, d->target, d->target <= d->ip);
        return 1;
    case 0xE9: case 0xEB:
        be_exit_chain(e, d->target, d->target <= d->ip);
        return 1;
    case 0xC3: case 0xC1: case 0xC2: case 0xC0:
        be_ea(e, V_T0, B86_SS, B86_SP, -1, 0);
        be_load(e, 1, V_T0, V_T0);
        be_opi(e, AOP_ADD, B86_SP, B86_SP, (op == 0xC2 || op == 0xC0) ? (uint16_t)(2 + d->imm) : 2);
        be_exit_ip_reg(e, V_T0);
        return 1;
    case 0xFF:
        if (d->reg == 4) {           /* JMP near r/m */
            load_rm(t, d, 1, V_T0);
            be_exit_ip_reg(e, V_T0);
            return 1;
        }
        if (d->reg == 2) {           /* CALL near r/m (needs 3 temps) */
            load_rm(t, d, 1, V_T2);
            be_movi(e, V_T1, d->next);
            emit_push(t, V_T1, d->next);
            be_mov(e, V_T0, V_T2);
            be_exit_ip_reg(e, V_T0);
            return 1;
        }
        if (d->reg >= 6) {           /* PUSH r/m */
            load_rm(t, d, 1, V_T1);
            emit_push(t, V_T1, d->next);
            return 1;
        }
        return 0;
    case 0xF4:
        be_exit_ip_imm(e, d->next, XR_HALT);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Block cache                                                              */
/* ------------------------------------------------------------------------ */

static uint32_t map_hash(uint32_t key) { return (key * 2654435761u) >> (32 - 14); }
static uint32_t fast_hash(uint32_t key) { return (key ^ (key >> 16)) & (B86_FASTN - 1); }

static Block *map_find(J *j, uint32_t key)
{
    uint32_t h = map_hash(key);
    for (uint32_t i = 0; i < MAPN; ++i) {
        uint32_t s = (h + i) & (MAPN - 1);
        uint32_t v = j->map[s];
        if (!v) return NULL;
        Block *b = &j->blk[v - 1];
        if (b->key == key && !b->dead) return b;
    }
    return NULL;
}

static void map_insert(J *j, uint32_t idx)
{
    Block *b = &j->blk[idx];
    uint32_t h = map_hash(b->key);
    for (uint32_t i = 0; i < MAPN; ++i) {
        uint32_t s = (h + i) & (MAPN - 1);
        uint32_t v = j->map[s];
        if (!v || (j->blk[v - 1].key == b->key && j->blk[v - 1].dead)) {
            j->map[s] = idx + 1;
            b->map_slot = s;
            return;
        }
    }
}

static void fast_reset(J *j)
{
    for (uint32_t i = 0; i < B86_FASTN; ++i) {
        j->fast[i].key = 0xFFFFFFFFu;
        j->fast[i].host = be_code_ptr(j->e.x_miss);
    }
}

void b86_jit_flush(J *j)
{
    for (uint32_t i = 0; i < j->nblk; ++i) {
        Block *b = &j->blk[i];
        j->map[b->map_slot] = 0;
        j->pg_head[b->glo >> PG_SHIFT] = 0;
        uint32_t f = fast_hash(b->key);
        j->fast[f].key = 0xFFFFFFFFu;
        j->fast[f].host = be_code_ptr(j->e.x_miss);
        for (uint32_t l = b->glo >> B86_LINE_SHIFT; l <= (b->ghi - 1) >> B86_LINE_SHIFT; ++l)
            j->codemap[l] = 0;
    }
    j->nblk = 0;
    j->e.p = j->code_start;
    j->flush_gen++;
    j->st.flushes++;
}

static void kill_block(J *j, uint32_t idx)
{
    Block *b = &j->blk[idx];
    if (b->dead) return;
    b->dead = 1;
    for (uint32_t l = b->glo >> B86_LINE_SHIFT; l <= (b->ghi - 1) >> B86_LINE_SHIFT; ++l)
        if (j->codemap[l] && j->codemap[l] != 255) j->codemap[l]--;
    uint32_t f = fast_hash(b->key);
    if (j->fast[f].key == b->key) {
        j->fast[f].key = 0xFFFFFFFFu;
        j->fast[f].host = be_code_ptr(j->e.x_miss);
    }
    be_kill_entry(b->host, b->dead_stub);
    j->st.smc_invalidations++;
}

static void invalidate(J *j, uint32_t lo, uint32_t hi)
{
    uint32_t p0 = (lo >> PG_SHIFT), p1 = (hi - 1) >> PG_SHIFT;
    if (p0) p0--;
    for (uint32_t p = p0; p <= p1 && p < NPG; ++p) {
        for (uint32_t v = j->pg_head[p]; v; v = j->blk[v - 1].next_pg) {
            Block *b = &j->blk[v - 1];
            if (!b->dead && b->glo < hi && lo < b->ghi) kill_block(j, v - 1);
        }
    }
}

static void smc_hook(B86Cpu *c, uint32_t lin, uint32_t n)
{
    J *j = c->jit;
    j->st.smc_hits++;
    invalidate(j, lin, lin + n);
}

/* ------------------------------------------------------------------------ */
/* Translation                                                              */
/* ------------------------------------------------------------------------ */

static Block *translate(J *j, uint32_t cs, uint16_t ip)
{
    B86Cpu *c = j->cpu;
    static Insn v[128];
    int n = 0;
    uint32_t base = cs << 4;
    uint32_t glo = base + ip, ghi = glo;
    uint16_t pc = ip;
    unsigned maxn = j->max_insns ? j->max_insns : DEF_INSNS;

    while ((unsigned)n < maxn) {
        Insn *d = &v[n];
        decode(c, cs, pc, d);
        classify(d);
        if (d->next < pc) { if (n == 0) { d->cls = C_END; d->native = 0; } else break; } /* wraps */
        if (n > 0 && base + d->next - glo > MAX_SPAN) break;
        ghi = base + d->next;
        n++;
        if (ends_block(d)) break;
        pc = d->next;
    }
    Insn *last = &v[n - 1];
    uint32_t glo0 = glo, ghi0 = ghi;

    if (j->nblk >= MAXB || (size_t)(j->buf_end - j->e.p) < 16384) b86_jit_flush(j);

    uint32_t idx = j->nblk;
    Block *b = &j->blk[idx];
    Emit *e = &j->e;
    uint8_t *start = e->p;
    static Tx tx;

    /* Analysis assumes every `native` insn lowers. If one cannot (register
       pressure on Thumb-2), demote it to the helper and redo the block, so
       no consumer is ever fused to a producer that did not set NZCV. */
    for (int attempt = 0;; ++attempt) {
        for (int i = 0; i < n; ++i) {
            v[i].arm = v[i].lazy = v[i].fused = v[i].mode = v[i].prod = 0;
            v[i].live = v[i].live_taken = 0;
        }
        Guard g = { glo0, ghi0 };
        uint16_t live_out = ALLF;
        if (last->cls == C_JMP || last->cls == C_CALL) live_out = lookahead(j, cs, last->target, &g);
        else if (!ends_block(last)) live_out = lookahead(j, cs, last->next, &g);
        analyze(j, cs, v, n, live_out, &g);
        glo = g.lo; ghi = g.hi;

        memset(b, 0, sizeof *b);
        b->key = (cs << 16) | ip;
        b->glo = glo; b->ghi = ghi; b->ninsn = (uint16_t)n;
        e->blk = idx;
        e->nslow = 0;
        e->overflow = 0;
        e->p = start;
        b->host = start;
        memset(&tx, 0, sizeof tx);
        tx.j = j; tx.e = e; tx.cs = cs;
        tx.inc_pending = 1;

        int demoted = 0;
        uint64_t helpers = 0;
        for (int i = 0; i < n; ++i) {
            Insn *d = &v[i];
            e->retire = (uint32_t)i + 1;
            int ok = d->native && lower(&tx, d);
            if (!ok && d->native) { d->native = 0; demoted = 1; break; }
            if (!ok) {
                be_call_step(e, d->ip, d->next);
                tx.inc_pending = 0;            /* the interpreter materializes */
                helpers++;
                if (d->cls != C_SEQ && d->cls != C_JCC) { be_exit_dyn(e); break; }
            }
            if (i == n - 1 && !ends_block(d)) be_exit_chain(e, d->next, 0);
        }
        if (demoted && attempt < n) continue;
        j->st.helper_insns += helpers;
        break;
    }
    for (int s = 0; s < tx.nside; ++s) {
        be_bind(e, tx.side[s].site, e->p);
        e->retire = tx.side[s].retire;
        be_exit_chain(e, tx.side[s].target, tx.side[s].poll);
    }
    be_finish_block(e);
    b->dead_stub = e->p;
    e->retire = 0;
    be_exit_ip_imm(e, ip, XR_LOOKUP);

    if (e->overflow || tx.fail) {
        /* out of space or an internal limit: flush and retry smaller */
        b86_jit_flush(j);
        if (tx.fail && maxn > 4) { unsigned m = j->max_insns; j->max_insns = maxn / 2; Block *r = translate(j, cs, ip); j->max_insns = m; return r; }
        return translate(j, cs, ip);
    }
    be_flush_icache(start, (size_t)(e->p - start));
#ifdef B86_DEBUG_DUMP
    {
        char nm[64];
        snprintf(nm, sizeof nm, "/tmp/blk_%04X_%04X.bin", (unsigned)cs, (unsigned)ip);
        FILE *df = fopen(nm, "wb");
        if (df) { fwrite(start, 1, (size_t)(e->p - start), df); fclose(df); }
    }
#endif

    j->nblk++;
    map_insert(j, idx);
    uint32_t pg = glo >> PG_SHIFT;
    b->next_pg = j->pg_head[pg];
    j->pg_head[pg] = idx + 1;
    for (uint32_t l = glo >> B86_LINE_SHIFT; l <= (ghi - 1) >> B86_LINE_SHIFT; ++l)
        if (j->codemap[l] != 255) j->codemap[l]++;
    j->st.blocks++;
    j->st.guest_insns += (uint64_t)n;
    j->st.host_bytes += (uint64_t)(e->p - start);
    return b;
}

/* ------------------------------------------------------------------------ */
/* Helpers called from generated code                                       */
/* ------------------------------------------------------------------------ */

uint32_t b86h_step(B86Cpu *c, uint32_t ip_next, uint32_t blk)
{
    J *j = c->jit;
    uint32_t cs0 = c->seg[B86_CS];
    c->ip = ip_next & 0xFFFFu;
    int r = b86_step(c);
    if (r == B86_HALT) { c->irq |= 0x80000000u; return 1; }
    if (c->seg[B86_CS] != cs0 || c->ip != (ip_next >> 16)) return 1;
    return j->blk[blk].dead;
}

uint32_t b86h_cond(B86Cpu *c, uint32_t cc)
{
    b86_flags_materialize(c);
    uint32_t f = c->flags, r;
    switch (cc >> 1) {
    case 0: r = (f & B86_OF) != 0; break;
    case 1: r = (f & B86_CF) != 0; break;
    case 2: r = (f & B86_ZF) != 0; break;
    case 3: r = (f & (B86_CF | B86_ZF)) != 0; break;
    case 4: r = (f & B86_SF) != 0; break;
    case 5: r = (f & B86_PF) != 0; break;
    case 6: r = ((f & B86_SF) != 0) != ((f & B86_OF) != 0); break;
    default: r = (f & B86_ZF) || (((f & B86_SF) != 0) != ((f & B86_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

void b86h_flags(B86Cpu *c) { b86_flags_materialize(c); }

uint32_t b86h_smc(B86Cpu *c, uint8_t *host, uint32_t len, uint32_t blk)
{
    J *j = c->jit;
    uint32_t lin = (uint32_t)(host - c->mem);
    j->st.smc_hits++;
    invalidate(j, lin, lin + len);
    return j->blk[blk].dead;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

J *b86_jit_create(B86Cpu *c, void *code, size_t size)
{
    J *j = calloc(1, sizeof *j);
    j->cpu = c;
    j->buf = code;
    j->buf_end = (uint8_t *)code + size;
    j->blk = calloc(MAXB, sizeof *j->blk);
    j->map = calloc(MAPN, sizeof *j->map);
    j->pg_head = calloc(NPG, sizeof *j->pg_head);
    j->fast = calloc(B86_FASTN, sizeof *j->fast);
    j->codemap = calloc(B86_LINES, 1);
    c->jit = j;
    c->codemap = j->codemap;
    c->codemap_host = j->codemap - ((uintptr_t)c->mem >> B86_LINE_SHIFT);
    c->fast = j->fast;
    c->smc_hook = smc_hook;
    j->e.base = j->buf;
    j->e.p = j->buf;
    j->e.end = j->buf_end;
    j->e.cpu = c;
    j->enter = (int (*)(B86Cpu *, uintptr_t))be_emit_runtime(&j->e);
    j->code_start = j->e.p;
    be_flush_icache(j->buf, (size_t)(j->e.p - j->buf));
    fast_reset(j);
    return j;
}

void b86_jit_destroy(J *j)
{
    if (!j) return;
    if (j->cpu) { j->cpu->jit = NULL; j->cpu->codemap = NULL; j->cpu->smc_hook = NULL; }
    free(j->blk); free(j->map); free(j->pg_head); free(j->fast); free(j->codemap); free(j);
}

const B86JitStats *b86_jit_stats(J *j) { return &j->st; }
void b86_jit_set_max_block(J *j, unsigned n) { j->max_insns = n; }
void b86_jit_set_lookahead(J *j, int on) { j->no_lookahead = !on; }
void b86_jit_set_no_fast(J *j, int on) { j->single_step = on; }
void b86_jit_set_count_exits(J *j, int on) { j->e.count_exits = on; }
void b86_jit_set_single_step(J *j, int on)
{
    j->single_step = on;
    j->max_insns = on ? 1u : 0u;
    j->no_lookahead = on;
}

int b86_jit_run(B86Cpu *c, uint64_t max_dispatch)
{
    J *j = c->jit;
    uint64_t n = 0;
    uint8_t *patch_site = NULL;
    uint32_t patch_gen = 0;
    for (;;) {
        if (c->irq & 0x80000000u) { c->irq &= 0x7FFFFFFFu; return B86_HALT; }
        if (c->irq) return B86_EXIT;
        if (n >= max_dispatch) return B86_BUDGET;
        n++;
        uint32_t key = (c->seg[B86_CS] << 16) | (c->ip & 0xFFFFu);
        Block *b = map_find(j, key);
        if (!b) b = translate(j, c->seg[B86_CS], (uint16_t)c->ip);
        if (patch_site && patch_gen == j->flush_gen) {
            be_patch_branch(patch_site, b->host);
            j->st.chains++;
        }
        patch_site = NULL;
        /* SEED51187_DIAG: fast table population disabled. */
        j->st.dispatches++;
        int r = j->enter(c, be_code_ptr(b->host));
        switch (r) {
        case XR_HALT: return B86_HALT;
        case XR_CHAIN:
            patch_site = (uint8_t *)c->patch;
            patch_gen = j->flush_gen;
            break;
        default: break;
        }
    }
}

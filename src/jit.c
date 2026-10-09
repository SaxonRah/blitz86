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
#include <stdio.h>
#include <string.h>
#ifdef B86_DEBUG_DUMP
#include <stdio.h>
#endif

#ifndef B86_MAXB
#define B86_MAXB    8192u           /* blocks per cache generation */
#endif
#ifndef B86_MAP_BITS
#define B86_MAP_BITS 14u            /* block hash: 2^bits slots (> 2 * MAXB) */
#endif
#define MAXB        B86_MAXB
#define MAPN        (1u << B86_MAP_BITS)
#define PG_SHIFT    9u                      /* SMC bucket page: 512 bytes */
#define NPG         ((B86_MEM_BYTES >> PG_SHIFT) + 2u)
#define MAX_SPAN    480u                    /* guarded bytes per block   */
#define DEF_INSNS   48u
#define ALLF        ((uint16_t)B86_ARITH)

#define OFF(f) ((unsigned)offsetof(B86Cpu, f))
#ifdef B86_COND_HISTO
#include <stdio.h>
#endif
#ifdef B86_COND_HISTO      /* debug: record the guest site of each cond/flags call */
#define DBG_SITE(t, d) be_stctx_imm((t)->e, ((t)->cs << 4) + (d)->ip, OFF(retired))
uint32_t b86_cond_ip[1 << 20], b86_flags_ip[1 << 20], b86_disp_ip[1 << 20], b86_xr[8];
uint64_t b86_rep_bytes[4]; uint32_t b86_rep_es[65536]; uint32_t b86_cold_ip[1 << 20];
#else
#define DBG_SITE(t, d) ((void)0)
#endif
#define BIT_GET(m, a) (((m)[(a) >> 3] >> ((a) & 7u)) & 1u)
#define BIT_SET(m, a) ((m)[(a) >> 3] |= (uint8_t)(1u << ((a) & 7u)))
#define COVN ((B86_MEM_BYTES + 7u) >> 3)
/* Paged Thumb-2 indexes the SMC line map with UBFX(addr, 6, 15): a 32 Ki
   table where guest lines start at ((mem >> 6) & 0x7FFF). Addresses outside
   the guest buffer (mapped SRAM frames) land elsewhere in the table, which
   stays zero if the embedder places the buffer so they cannot collide. */
#define CM_PAGED 32768u
#ifndef HEAT_BITS
#define HEAT_BITS 12                    /* tiering: 2^n cold entry counters */
#endif
#define HEATN (1u << HEAT_BITS)
#ifndef HEAT_DECAY
#define HEAT_DECAY 32768u               /* cold dispatches between halvings */
#endif

/* Metadata allocator. The block table, block map, SMC buckets and RET
   metadata are only touched by the dispatcher, so an embedder can place them
   in slow memory (RP2350 PSRAM) and keep SRAM for the code buffer:
   -DB86_CALLOC=my_calloc -DB86_FREE=my_free */
#ifdef B86_NOW
uint64_t B86_NOW(void);
#endif
#ifdef B86_MISSES
uint64_t B86_MISSES(void);
#endif
#ifndef B86_CALLOC
#define B86_CALLOC calloc
#define B86_FREE free
#else
void *B86_CALLOC(size_t n, size_t size);
void B86_FREE(void *p);
#endif

typedef struct Block {
    uint32_t key;
    uint32_t glo, ghi;          /* guarded linear range [glo, ghi)        */
    uint32_t iend;              /* end of the translated instruction bytes */
    uint8_t *host;
    uint8_t *dead_stub;
    uint32_t next_pg;           /* bucket chain (see pg_link)              */
    uint32_t xlo[2], xhi[2];    /* extra guarded ranges (lookahead targets) */
    uint32_t next_pgx[2];
    uint8_t nx;
    uint32_t map_slot;
    uint16_t ninsn;
    uint8_t dead;
    uint8_t spec;               /* continuation specialized for one RET site */
    uint8_t *ret_b, *ret_rec;   /* spec: that site's hit branch / record path */
} Block;

struct Insn;
#define RMN 256u

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
    int no_chain;               /* testing: never patch chain exits */
    int hot_external;           /* fast/codemap supplied by the caller */
    uint8_t *cm_table;          /* codemap allocation (paged: 32 Ki entries)   */
    void *tv, *tw;              /* translator scratch (Insn[128] each, B86_CALLOC) */
    int no_spec;                /* testing: no RET-specialized continuations */
    int no_join;                /* testing: superblocks never stop at translated code */
    int split_mid;              /* experiment: kill blocks covering a new mid entry */
    uint8_t *cov;               /* bit per guest byte: start of a translated insn (this generation) */
    uint8_t *ent;               /* bit per guest byte: a block entry (this generation) */
    uint32_t *heat_key;         /* cold entries: CS:IP tag per slot      */
    uint8_t *heat;              /* cold entries: decayed execution count */
    unsigned hot_threshold;     /* translate an entry after this many runs (0: at once) */
    uint32_t heat_ticks;        /* cold dispatches since the last decay  */
    uint8_t *shadow;            /* optional copy of translated guest bytes */
    uint8_t *dead;              /* per block: killed (SRAM mirror of blk[].dead) */
    struct RetMeta *rm;         /* RET sites carrying deferred flag records */
    uint32_t nrm;
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
    uint16_t nfd;                       /* native only if these flags are dead */
    uint16_t armneed;                   /* flags fused consumers read from NZCV */
    uint8_t defer;                      /* lazy record rebuilt at exits instead */
    uint8_t wm;                         /* guest GPRs (bit per reg16) written   */
    uint8_t pendb;                      /* INC/DEC overlay possibly live before */
    uint8_t inv;                        /* unrolled LOOP copy: exit when CX == 0 */
    uint8_t virt;                       /* 1: virtual producer (record pending in
                                           registers); 2: marker, record already in ctx */
    uint8_t valid;                      /* x86 flags representable in NZCV */
    uint8_t ijoin;                      /* forward Jcc: index of its in-block target (0: exit) */
    uint8_t zsrc;                       /* non-fused JE/JNE/JS/JNS: producer index + 1 whose
                                           inline record's lz_res gives ZF/SF directly */
    uint8_t endsynth;                   /* last insn: block-end exit needs only CF; it is
                                           read from NZCV (ARM cond endsynth-1) */
    uint8_t synth;                      /* fused JB/JAE whose exit needs only CF: the
                                           exit writes the known CF instead of records */
    uint8_t cmode;                      /* fused carry consumer: ARM condition for CF
                                           (mode is reused when it also produces NZCV) */
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

B86_HOT static inline uint8_t fb(B86Cpu *c, uint32_t cs, uint32_t ip)
{
    return *b86_host(c, (cs << 4) + (ip & 0xFFFFu));
}

/* immediate bytes following modrm/opcode (8086 map) */
B86_HOT static int imm_size(uint8_t op, uint8_t reg)
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

B86_HOT static void decode(B86Cpu *c, uint32_t cs, uint16_t ip, Insn *d)
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
B86_HOT static void classify(Insn *d)
{
    uint8_t op = d->op;
    int memop = d->has_modrm && d->mod != 3;
    d->fuse = ALLF; d->fdef = 0; d->native = 0;
    if (d->cls == C_END) return;
    if (d->nprefix > 3) { return; }

    if (op < 0x40 && (op & 7) < 6) {
        int a = op >> 3;
        d->fdef = ALLF; d->fuse = (a == 2 || a == 3) ? B86_CF : 0;
        d->native = !d->rep;
        return;
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: d->fuse = 0; d->native = 1; return;
    case 0x07: case 0x17: case 0x1F: d->fuse = 0; d->native = 1; return;
    case 0x0F: d->cls = C_END; return;
    case 0x27: case 0x2F: d->fuse = B86_AF | B86_CF; d->fdef = ALLF & ~B86_OF; return;
    case 0x37: case 0x3F: d->fuse = B86_AF; d->fdef = B86_AF | B86_CF; return;
    case 0x9C: d->fuse = ALLF; d->native = 1; d->nzclob = 1; return;
    case 0x9D: d->fuse = 0; d->fdef = ALLF; d->native = 1; d->nzclob = 1; return;
    case 0x9E: d->fuse = 0; d->fdef = ALLF & ~B86_OF; d->native = 1; d->nzclob = 1; return;
    case 0x9F: d->fuse = ALLF & ~B86_OF; d->native = 1; d->nzclob = 1; return;
    case 0xF5: case 0xF8: case 0xF9:   /* CF via ctx flags; materializes() keeps older records inline */
        d->fuse = op == 0xF5 ? B86_CF : 0; d->fdef = B86_CF; d->native = 1; d->nzclob = 1; return;
    case 0xFA: case 0xFB: case 0xFC: case 0xFD: d->fuse = 0; d->native = 1; return;
    case 0xD6: d->fuse = B86_CF; return;
    case 0xD5: d->fuse = 0; d->fdef = ALLF; return;
    case 0x90: d->fuse = 0; d->native = 1; return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: d->fuse = 0; d->native = 1; return;
    case 0x84: case 0x85: case 0xA8: case 0xA9: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
    case 0x86: d->fuse = 0; d->native = !memop; return;
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
        d->native = 1; return;
    case 0xF6: case 0xF7:
        switch (d->reg) {
        case 0: case 1: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
        case 2: d->fuse = 0; d->native = 1; return;
        case 3: d->fuse = 0; d->fdef = ALLF; d->native = 1; return;
        case 4: case 5: d->fuse = 0; d->fdef = ALLF & ~B86_AF;   /* interp leaves AF */
            d->native = 1; d->nzclob = 1; return;          /* flags via light call when live */
        case 6: d->fuse = ALLF; d->native = d->w; d->nzclob = 1; return;   /* DIV r/m16: UDIV, faults via interp */
        case 7: d->fuse = ALLF; d->native = d->w && !d->rep; d->nzclob = 1; return;   /* IDIV r/m16: SDIV */
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
        d->native = (d->reg == 4 || d->reg == 5 || d->reg == 7) ||
                    (d->reg < 4 && d->mod == 3 && (d->w || d->rm < 4));   /* ROL/ROR/RCL/RCR r,1 */
        if (d->reg < 4) d->nzclob = 1;
        return;
    case 0xD2: case 0xD3: /* count may be 0: defines nothing for sure */
        d->fuse = (d->reg == 2 || d->reg == 3) ? B86_CF : 0; d->fdef = 0;
        if (d->reg == 4 || d->reg == 5 || d->reg == 7) { d->native = 1; d->nfd = ALLF; d->nzclob = 1; }
        return;
    case 0xD4: d->fuse = ALLF; d->fdef = 0; return;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB:
        d->fuse = 0; d->native = 1;          /* REP form: bulk helper */
        if (d->rep) d->nzclob = 1;
        return;
    case 0xAC: case 0xAD:
        d->fuse = 0; d->native = !d->rep; return;
    case 0xA6: case 0xA7: case 0xAE: case 0xAF:
        d->fuse = 0; d->fdef = d->rep ? 0 : ALLF; return;
    case 0xE0: case 0xE1: d->cls = C_JCC; d->target = (uint16_t)(d->next + (int8_t)d->imm);
        d->fuse = B86_ZF; d->native = 1; d->nzclob = 1; return;
    case 0xE2: case 0xE3: d->cls = C_JCC; d->target = (uint16_t)(d->next + (int8_t)d->imm);
        d->fuse = 0; d->native = 1; d->nzclob = 1; return;
    case 0xE8: d->cls = C_CALL; d->target = (uint16_t)(d->next + d->imm); d->fuse = 0; d->native = 1; return;
    case 0xE9: d->cls = C_JMP; d->target = (uint16_t)(d->next + d->imm); d->fuse = 0; d->native = 1; return;
    case 0xEB: d->cls = C_JMP; d->target = (uint16_t)(d->next + (int8_t)d->imm); d->fuse = 0; d->native = 1; return;
    case 0xC2: case 0xC0: case 0xC3: case 0xC1: d->cls = C_RET; d->fuse = 0; d->native = 1; return;
    case 0xF4: d->cls = C_HLT; d->fuse = 0; d->native = 1; return;
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

B86_COLD static int ends_block(const Insn *d)
{
    return d->cls == C_JMP || d->cls == C_CALL || d->cls == C_RET ||
           d->cls == C_IND || d->cls == C_END || d->cls == C_HLT;
}

/* A RET whose exit would rebuild deferred flag records: the hit path of its
   inline cache enters a continuation specialized with these producers as a
   virtual prefix, so the record is rebuilt only where actually needed. */
typedef struct RetMeta { uint8_t *site, *miss, *recp, *recb; int npre; Insn pre[2]; } RetMeta;
/* recp: start of `write record; B normal-target`, recb: that B */

/* Flags live on entry to `ip`: scan forward until every flag is defined.
   The scanned bytes join the block's guarded range (so SMC there also
   invalidates this block); if that would exceed one SMC page we give up
   and assume every flag is live. */
typedef struct Guard { uint32_t lo, hi; uint32_t xlo[2], xhi[2]; int nx; } Guard;

/* Add [lo,hi) to the guard: merge into a range if the result stays within
   one SMC page span, else open an extra range (at most two). */
B86_COLD static int guard_add(Guard *g, uint32_t lo, uint32_t hi)
{
    uint32_t nlo = lo < g->lo ? lo : g->lo, nhi = hi > g->hi ? hi : g->hi;
    if (nhi - nlo <= 512u) { g->lo = nlo; g->hi = nhi; return 1; }
    for (int r = 0; r < g->nx; ++r) {
        nlo = lo < g->xlo[r] ? lo : g->xlo[r]; nhi = hi > g->xhi[r] ? hi : g->xhi[r];
        if (nhi - nlo <= 512u) { g->xlo[r] = nlo; g->xhi[r] = nhi; return 1; }
    }
    if (g->nx < 2 && hi - lo <= 512u) { g->xlo[g->nx] = lo; g->xhi[g->nx] = hi; g->nx++; return 1; }
    return 0;
}

/* Flags live on entry to `ip`. Scans forward (following direct JMP and
   CALL) until every flag is defined. Scanned bytes join the block's guard
   so that modifying them invalidates the block. */
B86_COLD static uint16_t lookahead_d(J *j, uint32_t cs, uint16_t ip, Guard *g, int depth);
B86_COLD static uint16_t lookahead(J *j, uint32_t cs, uint16_t ip, Guard *g) { return lookahead_d(j, cs, ip, g, 1); }
/* depth > 0: a conditional branch (Jcc/LOOP/JCXZ) does not end the scan;
   its taken edge is scanned recursively and the fall-through continues. */
B86_COLD static uint16_t lookahead_d(J *j, uint32_t cs, uint16_t ip, Guard *g, int depth)
{
    if (j->no_lookahead) return ALLF;
    uint16_t need = 0, defd = 0, res = ALLF;
    uint16_t p = ip, seg = ip;
    Guard t = *g;
    int done = 0, follows = 0;
    for (int i = 0; i < 16 && !done; ++i) {
        Insn d;
        decode(j->cpu, cs, p, &d);
        classify(&d);
        if (d.next < p) return ALLF;
        need |= d.fuse & ~defd;
        if ((d.cls == C_JMP || d.cls == C_CALL) && follows < 2) {
            if (!guard_add(&t, (cs << 4) + seg, (cs << 4) + d.next)) return ALLF;
            p = seg = d.target; follows++;
            continue;
        }
        if (d.cls == C_JCC && depth > 0 && (uint16_t)(ALLF & ~defd)) {
            uint16_t tk = lookahead_d(j, cs, d.target, &t, depth - 1);   /* adds its bytes to t */
            need |= tk & ~defd;
            p = d.next;
            continue;
        }
        if (d.cls == C_JCC || ends_block(&d)) { res = (uint16_t)(need | (ALLF & ~defd)); p = d.next; done = 1; break; }
        defd |= d.fdef;
        p = d.next;
        if ((defd & ALLF) == ALLF) { res = need; done = 1; }
    }
    if (!done) res = (uint16_t)(need | (ALLF & ~defd));
    if (res == ALLF) return ALLF;
    if (!guard_add(&t, (cs << 4) + seg, (cs << 4) + p)) return ALLF;
    *g = t;
    return res;
}

B86_COLD static uint16_t live_before(const Insn *d)
{
    uint16_t after = d->live | (d->cls == C_JCC ? d->live_taken : 0);
    return (uint16_t)((after & ~d->fdef) | d->fuse);
}

B86_COLD static int is_rcx(const Insn *d);
/* NZCV interpretation produced by a producer insn, and which x86 flags it
   represents. Returns 0 if the producer cannot feed NZCV. */
B86_COLD static int producer_class(const Insn *d, uint8_t *valid)
{
    if (d->virt) return 0;
    uint8_t op = d->op;
    int xop = -1;
    if (op < 0x40 && (op & 7) < 6) xop = op >> 3;
    else if (op >= 0x80 && op <= 0x83) xop = d->reg;
    else if (op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) xop = 4;
    else if ((op == 0xF6 || op == 0xF7) && d->reg < 2) xop = 4;
    else if ((op == 0xF6 || op == 0xF7) && d->reg == 3) { *valid = 0xF; return FM_SUB; }
    else if ((op >= 0x40 && op <= 0x47) || ((op == 0xFE || op == 0xFF) && d->reg == 0)) { *valid = 0x7; return FM_ADD; }
    else if ((op >= 0x48 && op <= 0x4F) || ((op == 0xFE || op == 0xFF) && d->reg == 1)) { *valid = 0x7; return FM_SUB; }
    if ((op == 0xD0 || op == 0xD1) && d->reg == 4) { *valid = 0xF; return FM_ADD; }  /* ADDS x,x,x */
    if ((op == 0xD0 || op == 0xD1) && (d->reg == 5 || d->reg == 7)) { *valid = 0x8; return FM_ADD; }  /* SHR/SAR 1: C = bit 0 */
    if (is_rcx(d)) { *valid = d->reg == 2 ? 0x9 : 0x8; return FM_ADD; }  /* RCL: CF,OF  RCR: CF */
    if ((op == 0xF5 || op == 0xF8 || op == 0xF9) && d->native) { *valid = 0x8; return FM_ADD; }  /* CMC/CLC/STC */
    if (xop < 0) return 0;
    *valid = 0xF; /* bit0 OF bit1 SF bit2 ZF bit3 CF */
    switch (xop) {
    case 0: return FM_ADD;
    case 5: case 7: return FM_SUB;
    case 1: case 4: case 6: return be_logic_mode;
    default: return 0;
    }
}

B86_COLD static uint8_t flagbits(uint16_t f)
{
    return (uint8_t)(((f & B86_OF) ? 1 : 0) | ((f & B86_SF) ? 2 : 0) |
                     ((f & B86_ZF) ? 4 : 0) | ((f & B86_CF) ? 8 : 0) |
                     ((f & (B86_PF | B86_AF)) ? 0x80 : 0));
}

B86_COLD static int arm_cond(int mode, int cc)
{
    static const signed char sub_t[16] = { AC_VS, AC_VC, AC_CC, AC_CS, AC_EQ, AC_NE, AC_LS, AC_HI,
                                           AC_MI, AC_PL, -1, -1, AC_LT, AC_GE, AC_LE, AC_GT };
    static const signed char add_t[16] = { AC_VS, AC_VC, AC_CS, AC_CC, AC_EQ, AC_NE, -1, -1,
                                           AC_MI, AC_PL, -1, -1, AC_LT, AC_GE, AC_LE, AC_GT };
    return mode == FM_SUB ? sub_t[cc] : mode == FM_ADD ? add_t[cc] : -1;
}

B86_COLD static int is_mem_dst_producer(const Insn *d)
{
    uint8_t op = d->op;
    if (!d->has_modrm || d->mod == 3) return 0;
    if (op < 0x40 && (op & 7) < 2) return (op >> 3) != 7;
    if (op >= 0x80 && op <= 0x83) return d->reg != 7;
    if (op == 0xF6 || op == 0xF7) return d->reg == 3;
    if (op == 0xFE || op == 0xFF) return d->reg < 2;
    if (op == 0xD0 || op == 0xD1) return 1;
    return 0;
}

/* native lowering performs a checked guest store */
B86_COLD static int checked_store(const Insn *d)
{
    uint8_t op = d->op;
    if (op == 0xA2 || op == 0xA3) return 1;
    if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) return 1;   /* STOS/MOVS (+REP) */
    if ((op >= 0x50 && op <= 0x57) || op == 0x06 || op == 0x0E || op == 0x16 || op == 0x1E || op == 0xE8 || op == 0x9C)
        return 1;                                                          /* PUSH / CALL */
    if (op == 0xFF && (d->reg == 2 || d->reg >= 6)) return 1;
    if (!d->has_modrm || d->mod == 3) return 0;
    if (is_mem_dst_producer(d)) return 1;
    switch (op) {
    case 0x88: case 0x89: case 0xC6: case 0xC7: case 0x8C: case 0xD0: case 0xD1: return 1;
    case 0xF6: case 0xF7: return d->reg == 2;
    }
    return 0;
}

B86_COLD static int is_jcc(const Insn *d) { return d->op >= 0x60 && d->op <= 0x7F && d->native; }
/* RCL/RCR r,1: native carry consumers (carry-in from NZCV when fused) */
B86_COLD static int is_rcx(const Insn *d)
{
    return d->native && (d->op == 0xD0 || d->op == 0xD1) && (d->reg == 2 || d->reg == 3) && d->mod == 3;
}
B86_COLD static int is_carry_alu(const Insn *d)
{
    if (!d->native) return 0;
    if (d->op < 0x40 && (d->op & 7) < 6) return (d->op >> 3) == 2 || (d->op >> 3) == 3;
    if (d->op >= 0x80 && d->op <= 0x83) return d->reg == 2 || d->reg == 3;
    return 0;
}

B86_COLD static uint16_t live_after(const Insn *d)
{
    return (uint16_t)(d->live | (d->cls == C_JCC ? d->live_taken : 0));
}

/* Can instruction m (between a producer and its consumer) destroy NZCV?
   Conservative: any native producer whose flags are live may itself
   become an NZCV producer. */
B86_COLD static int nz_clobber(const Insn *m)
{
    if (!m->native || m->nzclob) return 1;
    if (be_store_clobbers_nzcv && checked_store(m)) return 1;
    if (is_jcc(m) && !m->fused) return 1;
    if (m->fdef && (live_after(m) & m->fdef)) return 1;
    return 0;
}


/* Guest GPRs an instruction may write (bit per 16-bit register). */
B86_COLD static uint8_t r8bit(int r) { return (uint8_t)(1u << (r & 3)); }
B86_COLD static uint8_t wmask(const Insn *d)
{
    uint8_t op = d->op;
    int m3 = d->has_modrm && d->mod == 3;
    if (!d->native) return 0xFF;
    if (op < 0x40 && (op & 7) < 6) {
        if ((op >> 3) == 7) return 0;
        switch (op & 7) {
        case 0: return m3 ? r8bit(d->rm) : 0;
        case 1: return m3 ? (uint8_t)(1u << d->rm) : 0;
        case 2: return r8bit(d->reg);
        case 3: return (uint8_t)(1u << d->reg);
        default: return 1;
        }
    }
    if (op >= 0x40 && op <= 0x4F) return (uint8_t)(1u << (op & 7));
    if (op >= 0x50 && op <= 0x57) return 1u << B86_SP;
    if (op >= 0x58 && op <= 0x5F) return (uint8_t)((1u << B86_SP) | (1u << (op & 7)));
    if (op >= 0xB0 && op <= 0xB7) return r8bit(op & 7);
    if (op >= 0xB8 && op <= 0xBF) return (uint8_t)(1u << (op & 7));
    if (op >= 0x60 && op <= 0x7F) return 0;
    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83:
        return (d->reg == 7 || !m3) ? 0 : ((op & 1) ? (uint8_t)(1u << d->rm) : r8bit(d->rm));
    case 0x84: case 0x85: case 0xA8: case 0xA9: case 0x90: return 0;
    case 0x87: return (uint8_t)((1u << d->reg) | (1u << d->rm));
    case 0x88: case 0xC6: return m3 ? r8bit(d->rm) : 0;
    case 0x89: case 0xC7: case 0x8C: return m3 ? (uint8_t)(1u << d->rm) : 0;
    case 0x8A: return r8bit(d->reg);
    case 0x8B: case 0x8D: return (uint8_t)(1u << d->reg);
    case 0x8E: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xF5: case 0xF8: case 0xF9:
    case 0xE3: case 0xE9: case 0xEB: case 0xF4: return 0;
    case 0xA0: case 0xA1: case 0x98: case 0x9F: return 1;
    case 0x9C: case 0x9D: return 1u << B86_SP;
    case 0x9E: return 0;
    case 0xA2: case 0xA3: return 0;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: return (uint8_t)(1u | (1u << (op & 7)));
    case 0x99: return 1u << B86_DX;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x07: case 0x17: case 0x1F:
    case 0xE8: case 0xC2: case 0xC3: case 0xC0: case 0xC1: return 1u << B86_SP;
    case 0xE0: case 0xE1: case 0xE2: return 1u << B86_CX;
    case 0xF6: case 0xF7:
        if (d->reg < 2) return 0;
        if (d->reg < 4) return m3 ? ((op & 1) ? (uint8_t)(1u << d->rm) : r8bit(d->rm)) : 0;
        return (uint8_t)((1u << B86_AX) | (1u << B86_DX));
    case 0xFE: return m3 ? r8bit(d->rm) : 0;
    case 0xFF:
        if (d->reg < 2) return m3 ? (uint8_t)(1u << d->rm) : 0;
        return (d->reg == 4) ? 0 : (uint8_t)(1u << B86_SP);
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        return m3 ? ((op & 1) ? (uint8_t)(1u << d->rm) : r8bit(d->rm)) : 0;
    }
    return 0xFF;
}

/* A producer whose lazy record can be rebuilt from registers at an exit. */
typedef struct DeferInfo { int kind, w, ra, rb, writes, overlay; uint32_t imm; } DeferInfo;
B86_COLD static int defer_info(const Insn *d, DeferInfo *o)
{
    uint8_t op = d->op;
    int xop = -1, w = op & 1, ra = -1, rb = -1, imm_src = 0, incdec = 0;
    uint32_t imm = 0;
    int m3 = d->has_modrm && d->mod == 3;
    if (!d->native || d->rep) return 0;
    if (op < 0x40 && (op & 7) < 6) {
        xop = op >> 3;
        switch (op & 7) {
        case 0: case 1: if (!m3) return 0; ra = d->rm; rb = d->reg; break;
        case 2: case 3: if (!m3) return 0; ra = d->reg; rb = d->rm; break;
        case 4: w = 0; ra = 0; imm_src = 1; imm = d->imm; break;
        default: w = 1; ra = 0; imm_src = 1; imm = d->imm; break;
        }
    } else if (op >= 0x80 && op <= 0x83) {
        if (!m3) return 0;
        xop = d->reg; ra = d->rm; imm_src = 1;
        imm = op == 0x83 ? (uint16_t)(int8_t)d->imm : d->imm;
    } else if (op == 0x84 || op == 0x85) {
        if (!m3) return 0;
        xop = 8; ra = d->rm; rb = d->reg;
    } else if (op == 0xA8 || op == 0xA9) {
        xop = 8; ra = 0; imm_src = 1; imm = d->imm;
    } else if ((op == 0xF6 || op == 0xF7) && d->reg < 2) {
        if (!m3) return 0;
        xop = 8; ra = d->rm; imm_src = 1; imm = d->imm;
    } else if (op >= 0x40 && op <= 0x4F) {
        w = 1; ra = op & 7; incdec = op >= 0x48 ? 2 : 1; xop = incdec == 2 ? 5 : 0; imm_src = 1; imm = 1;
    } else if ((op == 0xFE || op == 0xFF) && d->reg < 2) {
        if (!m3) return 0;
        ra = d->rm; incdec = d->reg ? 2 : 1; xop = d->reg ? 5 : 0; imm_src = 1; imm = 1;
    } else return 0;
    if (xop == 2 || xop == 3) return 0;                       /* ADC/SBB: carry-in */
    if (!w && (ra >= 4 || (!imm_src && rb >= 4))) return 0;   /* AH..BH need extraction */
    int writes = xop != 7 && xop != 8;
    int logic = xop == 1 || xop == 4 || xop == 6 || xop == 8;
    if (writes && !logic && !imm_src && (rb & (w ? 7 : 3)) == (ra & (w ? 7 : 3))) return 0; /* b clobbered */
    o->w = w; o->ra = w ? ra : (ra & 3); o->rb = imm_src ? -1 : (w ? rb : (rb & 3)); o->imm = imm;
    o->writes = writes; o->overlay = incdec != 0;
    if (incdec) o->kind = incdec == 2 ? (w ? LZ_DEC16 : LZ_DEC8) : (w ? LZ_INC16 : LZ_INC8);
    else if (logic) o->kind = w ? LZ_LOG16 : LZ_LOG8;
    else if (xop == 0) o->kind = w ? LZ_ADD16 : LZ_ADD8;
    else o->kind = w ? LZ_SUB16 : LZ_SUB8;
    o->writes = writes;
    if (logic && xop != 8) o->writes = 1;
    o->ra = o->ra; (void)logic;
    return 1;
}
B86_COLD static uint8_t defer_regs(const DeferInfo *o)
{
    return (uint8_t)((1u << o->ra) | (o->rb >= 0 ? (1u << o->rb) : 0u));
}

/* Lowerings that call into C and fold the lazy records into the flags
   word (clearing the INC/DEC overlay). A full record rebuilt at a later exit
   would override flags defined after it, so no deferral across these. */
B86_COLD static int materializes(const Insn *d)
{
    uint8_t op = d->op;
    if (!d->native) return 1;
    if (is_jcc(d) && !d->fused) return 1;
    if (op == 0xE0 || op == 0xE1 || op == 0xF5 || op == 0xF8 || op == 0xF9) return 1;
    if (op == 0x9C || op == 0x9E || op == 0x9F) return 1;
    if ((op == 0xD0 || op == 0xD1) && d->reg < 2) return 1;
    if (is_rcx(d)) return 1;   /* defines only CF/OF: older ZF/SF records must not be deferred past it */
    if ((op == 0xF6 || op == 0xF7) && d->reg >= 4) return 1;
    if (is_carry_alu(d) && !d->fused) return 1;
    return 0;
}

/* CALL's push uses check mode 2 (no exit), so it is not an exit point. */
B86_COLD static int smc_exit_store(const Insn *d)
{
    if (d->op == 0xE8 || (d->op == 0xFF && d->reg == 2)) return 0;
    return 1;
}

/* In-block forward branches. A forward Jcc whose target is a later
   instruction of the same superblock branches there directly instead of
   leaving through a side exit, provided every skipped instruction is plain
   native code that neither defines nor clobbers flags (no flag producer, no
   helper, no C call): then flag state, pending records and deferred-record
   registers are identical on both paths, and the only difference is the
   retired count, which the fall-through path settles at the join. */
B86_COLD static int checked_store(const Insn *d);
B86_COLD static int simple_between(const Insn *x)
{
    if (x->cls != C_SEQ || !x->native || x->fdef || x->virt || x->rep) return 0;
    if (!x->nzclob) return 1;
    /* A plain store only clobbers NZCV through its SMC check (Thumb-2).
       Pass A already refuses to fuse across it on the fall-through path, so
       no consumer after the join relies on NZCV from before the branch. */
    return checked_store(x) && x->has_modrm && x->mod != 3 &&
           (x->op == 0x88 || x->op == 0x89 || x->op == 0xC6 || x->op == 0xC7 || x->op == 0x8C);
}
B86_COLD static void plan_joins(Insn *v, int npre, int n)
{
    for (int q = npre; q < n; ++q) v[q].ijoin = 0;
    for (int q = npre; q < n; ++q) {
        Insn *d = &v[q];
        if (d->cls != C_JCC || !d->native || d->op < 0x60 || d->op > 0x7F || d->target <= d->ip) continue;
        for (int m = q + 1; m < n; ++m) {
            if (v[m].ip == d->target) { d->ijoin = (uint8_t)m; break; }
            if (!simple_between(&v[m]) || v[m].ip > d->target) break;
        }
    }
}

B86_COLD static void analyze(J *j, uint32_t cs, Insn *v, int n, uint16_t live_out, Guard *g)
{
    uint16_t live = live_out;
    for (int i = n - 1; i >= 0; --i) {
        Insn *d = &v[i];
        if (d->cls == C_JCC) d->live_taken = d->ijoin ? live_before(&v[d->ijoin]) : lookahead(j, cs, d->target, g);
        d->live = live;
        live = live_before(d);
    }
    /* Instructions lowered only when their flag results are dead. */
    for (int i = 0; i < n; ++i)
        if (v[i].nfd && (live_after(&v[i]) & v[i].nfd)) v[i].native = 0;
    /* Pass A: fuse each native Jcc with its producer through NZCV. */
    for (int q = 0; q < n; ++q) {
        Insn *c = &v[q];
        int carry = is_carry_alu(c) || is_rcx(c) || (c->op == 0xF5 && c->native);
        if (!is_jcc(c) && !carry) continue;
        uint16_t use = carry ? B86_CF : cc_use[(c->op & 15) >> 1];
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
        int ac = carry ? arm_cond(mode, 2) : arm_cond(mode, c->op & 15);   /* 2 = "B": CF set */
        if (ac < 0) continue;
        if (is_mem_dst_producer(pr) && be_store_clobbers_nzcv) continue;
        c->fused = 1; c->mode = (uint8_t)ac; c->cmode = (uint8_t)ac; c->prod = (uint8_t)p;
        pr->arm = 1; pr->mode = (uint8_t)mode; pr->armneed |= use;
    }
    /* JE/JNE/JS/JNS that could not fuse (typically: a checked store on
       Thumb-2 clobbered NZCV) read ZF/SF straight from the in-block
       producer's record result instead of materializing flags in C. */
    for (int q = 0; q < n; ++q) {
        Insn *c = &v[q];
        int cc = c->op & 15;
        if (!is_jcc(c) || c->fused || !(cc == 4 || cc == 5 || cc == 8 || cc == 9)) continue;
        uint16_t use = (cc < 8) ? B86_ZF : B86_SF;
        for (int m = q - 1; m >= 0; --m) {
            if (!(v[m].fdef & use)) {
                /* helpers (and CL shifts) may write a record of their own */
                if (!v[m].native || (v[m].nfd & use) || v[m].virt) break;
                continue;
            }
            const Insn *p = &v[m];
            int o = p->op;
            int ok = p->native && !p->virt &&
                     ((o < 0x40 && (o & 7) < 6) || (o >= 0x80 && o <= 0x85) || o == 0xA8 || o == 0xA9 ||
                      ((o == 0xD0 || o == 0xD1) && (p->reg == 4 || p->reg == 5 || p->reg == 7)) ||
                     ((o == 0xF6 || o == 0xF7) && p->reg < 2));          /* TEST r/m, imm */
            if (ok) c->zsrc = (uint8_t)(m + 1);
            break;
        }
    }
    /* A taken JB (JAE) means CF = 1 (0). When nothing but CF is live at that
       exit, the exit stores CF into ctx flags directly (no record, no call). */
    for (int q = 0; q < n; ++q) {
        Insn *c = &v[q];
        if (is_jcc(c) && c->fused && !c->ijoin && ((c->op & 15) == 2 || (c->op & 15) == 3) &&
            c->live_taken == B86_CF) c->synth = 1;
    }
    /* Same at the block-end exit (fall-through or JMP): if only CF is live
       there and its producer can hold it in NZCV up to the exit, the exit
       stores it into ctx flags instead of the producer writing a record. */
    {
        Insn *last = &v[n - 1];
        int k = -1;
        if (!ends_block(last)) k = n - 1;
        else if (last->op == 0xE9 || last->op == 0xEB) k = n - 2;
        if (k >= 0 && last->live == B86_CF) {
            int p = -1;
            for (int m = k; m >= 0; --m) {
                if (v[m].fdef & B86_CF) { p = m; break; }
                if (nz_clobber(&v[m])) break;
            }
            uint8_t valid = 0;
            int mode = (p >= 0 && v[p].native && !v[p].virt) ? producer_class(&v[p], &valid) : 0;
            int ac = mode ? arm_cond(mode, 2) : -1;
            if (ac >= 0 && (valid & 0x8) && (v[p].fdef & B86_CF) &&
                !(is_mem_dst_producer(&v[p]) && be_store_clobbers_nzcv) && !is_carry_alu(&v[p])) {
                v[p].arm = 1; v[p].mode = (uint8_t)mode; v[p].armneed |= B86_CF;
                last->endsynth = (uint8_t)(ac + 1);
            }
        }
    }
    for (int i = 0; i < n; ++i) v[i].wm = v[i].virt ? 0 : wmask(&v[i]);
    /* Pass B: lazy record unless every reader is a fused consumer. A record
       needed only at exits (side exits / block end) is deferred into the
       exit path when it can be rebuilt from registers there. */
    for (int i = 0; i < n; ++i) {
        Insn *p = &v[i];
        if (!p->native || !p->fdef) continue;
        uint16_t rem = p->fdef & live_after(p);
        if (!rem) continue;
        DeferInfo di;
        int can = defer_info(p, &di);
        uint8_t need = can ? defer_regs(&di) : 0xFF, touched = 0;
        int mid = 0, exits = 0;
        /* An SMC store that kills its own block exits right after the store,
           so flags held only in NZCV across a checked store need the record. */
        if (checked_store(p) && p->arm) mid = 1;
        int k;
        for (k = i + 1; k < n && rem; ++k) {
            Insn *q = &v[k];
            touched |= q->wm;
            if (q->native && checked_store(q) && smc_exit_store(q) && (live_after(q) & rem)) mid = 1;
            if ((q->fuse & rem) && !(q->fused && q->prod == i)) mid = 1;
            if (q->cls == C_JCC && !q->ijoin && !q->synth && (q->live_taken & rem)) { exits = 1; if (touched & need) can = 0; }
            if (!q->native && (live_before(q) & rem)) mid = 1;
            if (materializes(q) && (rem & ~q->fdef)) mid = 1;   /* q folds records; p's flags outlive it */
            rem &= (uint16_t)~q->fdef;
        }
        if (rem && (v[n - 1].live & rem) && !v[n - 1].endsynth) { exits = 1; if (touched & need) can = 0; }
        p->lazy = (uint8_t)(mid || exits);
        p->defer = (uint8_t)(p->lazy && !mid && can);
        if (p->virt == 2) p->lazy = p->defer = 0;    /* already in ctx */
    }
}

/* ------------------------------------------------------------------------ */
/* Lowering helpers                                                         */
/* ------------------------------------------------------------------------ */

typedef struct Side { uint8_t *site; uint16_t target; uint8_t poll; uint32_t retire; int idx; uint16_t live; } Side;

typedef struct Tx {
    J *j;
    Emit *e;
    uint32_t cs;
    Side side[64];
    int nside;
    int fail;
    int inc_pending;            /* an INC/DEC overlay record may be live */
    Insn *v;                    /* the block being lowered */
    int cur;                    /* index of the instruction being lowered */
    uint8_t *jsite[128];        /* in-block forward branch to bind at index i */
} Tx;

static void emit_deferred(Tx *t, int k, uint16_t live);

/* Backward conditional exit laid out inline: `skip` is the branch taken when
   the condition FAILS. Taken path = records + poll + one patchable B. */
B86_COLD static void emit_synth_cf(Tx *t, int cf);
B86_COLD static void emit_back_exit(Tx *t, uint8_t *skip, uint16_t target)
{
    if (t->v[t->cur].synth) emit_synth_cf(t, (t->v[t->cur].op & 15) == 2);
    else emit_deferred(t, t->cur, t->v[t->cur].live_taken);
    be_exit_chain(t->e, target, 1);
    be_bind(t->e, skip, t->e->p);
}

B86_COLD static void add_side(Tx *t, uint8_t *site, uint16_t target, uint16_t from)
{
    if (t->nside >= 64) { t->fail = 1; return; }
    t->side[t->nside].site = site;
    t->side[t->nside].target = target;
    t->side[t->nside].poll = target <= from;
    t->side[t->nside].retire = t->e->retire;
    t->side[t->nside].idx = t->cur;
    t->side[t->nside].live = t->v[t->cur].live_taken;
    t->nside++;
}

/* Rebuild deferred lazy records on an exit path: flags state after
   instruction k, `live` = flags needed past the exit. */
B86_COLD static void emit_record(Tx *t, const DeferInfo *o, int clear_overlay)
{
    Emit *e = t->e;
    unsigned ok = o->overlay ? OFF(lz_ikind) : OFF(lz_kind);
    unsigned oa = o->overlay ? OFF(lz_ia) : OFF(lz_a);
    unsigned orr = o->overlay ? OFF(lz_ires) : OFF(lz_res);
    int logic = o->kind == LZ_LOG8 || o->kind == LZ_LOG16;
    int sub = o->kind == LZ_SUB8 || o->kind == LZ_SUB16 || o->kind == LZ_DEC8 || o->kind == LZ_DEC16;
    be_stctx_imm(e, (uint32_t)o->kind, ok);
    if (clear_overlay) be_stctx_imm(e, LZ_NONE, OFF(lz_ikind));
    if (o->writes) {
        be_stctx(e, o->ra, orr);                        /* result lives in ra */
        if (!logic) {                                   /* a = res -/+ b */
            if (o->rb < 0) be_opi(e, sub ? AOP_ADD : AOP_SUB, V_T0, o->ra, o->imm);
            else be_op(e, sub ? AOP_ADD : AOP_SUB, V_T0, o->ra, o->rb);
            be_stctx(e, V_T0, oa);
        }
    } else {                                            /* CMP / TEST */
        int aop = logic ? AOP_AND : AOP_SUB;
        if (!logic) be_stctx(e, o->ra, oa);
        if (o->rb < 0) be_opi(e, aop, V_T0, o->ra, o->imm);
        else be_op(e, aop, V_T0, o->ra, o->rb);
        be_stctx(e, V_T0, orr);
    }
}

B86_COLD static void deferred_pq(Tx *t, int k, uint16_t live, int *P, int *Q, int *pa, int *qa)
{
    Insn *v = t->v;
    DeferInfo o;
    *P = *Q = -1; *pa = *qa = 0;
    if (!live) return;
    for (int m = k; m >= 0; --m) {
        if (v[m].fdef & B86_CF) { *P = m; break; }
        if (*Q < 0 && (v[m].fdef & B86_ZF)) *Q = m;
    }
    *pa = *P >= 0 && v[*P].defer && (live & v[*P].fdef) && defer_info(&v[*P], &o);
    *qa = *Q >= 0 && v[*Q].defer && (live & v[*Q].fdef) && defer_info(&v[*Q], &o);
}

B86_COLD static void emit_synth_cf(Tx *t, int cf)
{
    Emit *e = t->e;
    be_stctx_imm(e, LZ_NONE, OFF(lz_kind));
    be_ldctx(e, V_T0, OFF(flags));
    if (cf) be_opi(e, AOP_ORR, V_T0, V_T0, B86_CF);
    else be_opi(e, AOP_AND, V_T0, V_T0, (uint16_t)~B86_CF);
    be_stctx(e, V_T0, OFF(flags));
}

B86_COLD static void emit_end_synth(Tx *t, int ac)
{
    Emit *e = t->e;
    be_get_carry(e, V_T0, ac);
    be_stctx_imm(e, LZ_NONE, OFF(lz_kind));       /* may clobber T1 */
    be_ldctx(e, V_T1, OFF(flags));
    be_opi(e, AOP_AND, V_T1, V_T1, (uint16_t)~B86_CF);
    be_op(e, AOP_ORR, V_T1, V_T1, V_T0);
    be_stctx(e, V_T1, OFF(flags));
}

B86_COLD static void emit_deferred(Tx *t, int k, uint16_t live)
{
    Insn *v = t->v;
    int P = -1, Q = -1;
    if (!live) return;
    for (int m = k; m >= 0; --m) {
        if (v[m].fdef & B86_CF) { P = m; break; }
        if (Q < 0 && (v[m].fdef & B86_ZF)) Q = m;
    }
    DeferInfo o;
    if (P >= 0 && v[P].defer && (live & v[P].fdef) && defer_info(&v[P], &o))
        emit_record(t, &o, Q < 0 && v[P].pendb);
    if (Q >= 0 && v[Q].defer && (live & v[Q].fdef) && defer_info(&v[Q], &o))
        emit_record(t, &o, 0);
}

/* Make ctx `flags` valid for CF (and, with ikind, for everything) without a
   C call when nothing is pending: a materialized state (lz_kind == LZ_NONE)
   is common after CMC/STC/CLC, POPF, helpers and synthesized exits.
   Clobbers V_T0/V_T1 and NZCV. Leaves t->inc_pending alone (an INC/DEC
   overlay may still be pending on the fast path; it never changes CF). */
B86_COLD static void flags_if_lazy(Tx *t, Insn *d, int with_overlay)
{
    Emit *e = t->e;
    be_ldctx(e, V_T0, OFF(lz_kind));
    if (with_overlay) { be_ldctx(e, V_T1, OFF(lz_ikind)); be_op(e, AOP_ORR, V_T0, V_T0, V_T1); }
    uint8_t *skip = be_cbz(e, V_T0);
    (DBG_SITE(t, d), be_call_flags(e));
    be_bind(e, skip, e->p);
    (void)d;
}

/* EA of modrm memory operand into V_T0 */
B86_COLD static void ea_modrm(Tx *t, const Insn *d)
{
    static const signed char b1[8] = { B86_BX, B86_BX, B86_BP, B86_BP, B86_SI, B86_DI, B86_BP, B86_BX };
    static const signed char b2[8] = { B86_SI, B86_DI, B86_SI, B86_DI, -1, -1, -1, -1 };
    static const uint8_t ds[8] = { B86_DS, B86_DS, B86_SS, B86_SS, B86_DS, B86_DS, B86_SS, B86_DS };
    int base1 = b1[d->rm], base2 = b2[d->rm], seg = ds[d->rm];
    if (d->mod == 0 && d->rm == 6) { base1 = -1; seg = B86_DS; }
    if (d->seg != 0xFF) seg = d->seg;
    be_ea(t->e, V_T0, seg, base1, base2, d->disp);
}

B86_COLD static void ea_off(Tx *t, const Insn *d, int dst)
{
    static const signed char b1[8] = { B86_BX, B86_BX, B86_BP, B86_BP, B86_SI, B86_DI, B86_BP, B86_BX };
    static const signed char b2[8] = { B86_SI, B86_DI, B86_SI, B86_DI, -1, -1, -1, -1 };
    int base1 = b1[d->rm], base2 = b2[d->rm];
    if (d->mod == 0 && d->rm == 6) base1 = -1;
    be_ea_off(t->e, dst, base1, base2, d->disp);
}

/* get byte register r8 (0..7) as a vreg holding it in low bits */
B86_COLD static int get8(Tx *t, int r8, int tmp)
{
    if (r8 < 4) return r8;
    be_ubfx(t->e, tmp, r8 - 4, 8, 8);
    return tmp;
}
B86_COLD static void put8(Tx *t, int r8, int v)
{
    if (r8 < 4) be_bfi(t->e, r8, v, 0, 8);
    else be_bfi(t->e, r8 - 4, v, 8, 8);
}

enum { O_R16, O_R8, O_MEM, O_IMM };
typedef struct { int k, r; uint32_t imm; } Op;

B86_COLD static int lz_kind(int xop, int w, int incdec)
{
    if (xop == 2) return w ? LZ_ADC16 : LZ_ADC8;
    if (xop == 3) return w ? LZ_SBB16 : LZ_SBB8;
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
B86_COLD static int pick(Pool *p)
{
    int lim = be_has_t2 ? V_T2 : V_T1;
    for (int r = V_T0; r <= lim; ++r) if (!p->busy[r]) { p->busy[r] = 1; return r; }
    return -1;
}

B86_COLD static int emit_alu(Tx *t, Insn *d, int xop, int w, Op dst, Op src, int incdec)
{
    Emit *e = t->e;
    int sh = w ? 16 : 24;
    int writes = xop != 7 && xop != 8;
    int logic = xop == 1 || xop == 4 || xop == 6 || xop == 8;
    int carry = xop == 2 || xop == 3;
    int sub = xop == 5 || xop == 7 || xop == 3;
    int lz = d->lazy && !d->defer;                 /* record written in line */
    int need_flags = d->arm || lz;
    int nzonly = d->arm && !(d->armneed & ~(B86_ZF | B86_SF));
    int aop = (xop == 0 || xop == 2) ? AOP_ADD : sub ? AOP_SUB : xop == 1 ? AOP_ORR : xop == 4 || xop == 8 ? AOP_AND : AOP_EOR;
    if (carry && d->arm) return 0;               /* ADC/SBB never feed NZCV */
    int mem = dst.k == O_MEM || src.k == O_MEM;
    uint32_t imm = src.imm;

    if (!writes && !need_flags) return 1;           /* dead CMP/TEST */

    /* ---- plan ---- */
    Pool pool = {{0}};
    int a = -1, b = -1, ax = -1, bx = -1, res, x, tmp = -1;
    if (dst.k == O_MEM) { pool.busy[V_T0] = 1; pool.busy[V_T1] = 1; a = V_T1; }
    else if (src.k == O_MEM) { pool.busy[V_T1] = 1; b = V_T1; }   /* T0 free after load */
    /* no store: the address in T0 dies at the load, before any operand
       extraction (CMP/TEST [mem],AH..BH on Thumb-2 needs it) */
    if (dst.k == O_MEM && !writes) pool.busy[V_T0] = 0;
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
    /* CMP feeding both NZCV and a lazy record recomputes a - b after the
       shifted compare, so its scratch must not alias a. */
    int keep_a = !writes && d->arm && lz && !logic;
    int inplace = 0;            /* CMP into NZCV + record with two scratches: a is consumed */
    if (dst.k == O_R16 && writes) res = dst.r;
    else if (dst.k == O_MEM && writes) res = V_T1;
    else if (a >= V_T0 && !keep_a) res = a;
    else {
        res = pick(&pool);
        if (res < 0) {
            if (!(keep_a && a >= V_T0 && src.k != O_IMM)) return 0;
            res = a; inplace = 1;
        }
    }
    if (res >= V_T0) x = res;
    else { x = pick(&pool); if (x < 0) return 0; }
    if (src.k == O_IMM && d->arm && !logic && !be_imm_ok_sh(imm, sh)) {
        tmp = pick(&pool);
        if (tmp < 0) return 0;
    }
    int cft = -1;
    if (carry) {
        if (res >= V_T0 && res != a) pool.busy[res] = 1;
        if (x >= V_T0 && x != res && x != a && x != b) pool.busy[x] = 0;   /* x unused */
        cft = pick(&pool);
        if (cft < 0) return 0;
        if (!d->fused) flags_if_lazy(t, d, 0);        /* CF from ctx; clobbers temps */
    }
    if (d->arm && logic && res < V_T0) { /* test_res needs a scratch: x */ }

    /* ---- emit ---- */
    unsigned o_kind = incdec ? OFF(lz_ikind) : OFF(lz_kind);
    unsigned o_a = incdec ? OFF(lz_ia) : OFF(lz_a);
    unsigned o_res = incdec ? OFF(lz_ires) : OFF(lz_res);
    if (mem) ea_modrm(t, d);
    if (lz) {
        be_stctx_imm(e, (uint32_t)lz_kind(xop, w, incdec), o_kind);
        if (incdec) t->inc_pending = 1;
        else if (t->inc_pending) { be_stctx_imm(e, LZ_NONE, OFF(lz_ikind)); t->inc_pending = 0; }
        if (carry && src.k == O_IMM) be_stctx_imm(e, imm, OFF(lz_b));
    }
    if (mem) be_load(e, w, V_T1, V_T0);
    if (ax >= 0) be_ubfx(e, a, ax, 8, 8);
    if (bx >= 0) be_ubfx(e, b, bx, 8, 8);
    if (carry) {               /* after the load: a src address in T0 is dead now */
        if (d->fused) be_get_carry(e, cft, d->mode);
        else { be_ldctx(e, cft, OFF(flags)); be_ubfx(e, cft, cft, 0, 1); }
    }
    if (lz && !logic) be_stctx(e, a, o_a);
    if (carry) {
        if (lz && src.k != O_IMM) be_stctx(e, b, OFF(lz_b));
        if (src.k == O_IMM) be_opi(e, aop, res, a, imm); else be_op(e, aop, res, a, b);
        be_op(e, aop, res, res, cft);
        if (lz) be_stctx(e, res, o_res);
        if (dst.k == O_R8) put8(t, dst.r, res);
        if (dst.k == O_MEM) be_store(e, w, V_T1, V_T0, 1, d->next);
        return 1;
    }

    if (logic) {
        int r = writes ? res : x;
        if (src.k == O_IMM) be_opi(e, aop, r, a, imm); else be_op(e, aop, r, a, b);
        if (lz) be_stctx(e, r, o_res);
        if (dst.k == O_R8 && writes) put8(t, dst.r, r);
        if (d->arm && nzonly) be_test_nz(e, (r >= V_T0) ? r : x, r, sh);
        else if (d->arm) be_test_res(e, (r >= V_T0) ? r : x, r, sh);
    } else if (d->arm && nzonly && writes) {         /* Z/S only: plain op + test */
        if (src.k == O_IMM) be_opi(e, aop, res, a, imm); else be_op(e, aop, res, a, b);
        if (lz) be_stctx(e, res, o_res);
        if (dst.k == O_R8) put8(t, dst.r, res);
        be_test_nz(e, (res >= V_T0) ? res : x, res, sh);
    } else if (d->arm) {
        if (src.k == O_IMM) {
            if (writes) be_addsubi_sh(e, sub, x, res, a, imm, sh, tmp);
            else be_cmpi_sh(e, x, a, imm, sh, tmp);
        } else {
            if (writes) be_addsub_sh(e, sub, x, res, a, b, sh);
            else be_cmp_sh(e, x, a, b, sh);
        }
        if (inplace) {                    /* x == a holds a << sh */
            be_op_lsl(e, AOP_SUB, a, a, b, (unsigned)sh);
            be_ubfx(e, a, a, (unsigned)sh, 32u - (unsigned)sh);
            be_stctx(e, a, o_res);
        } else if (lz) {
            if (!writes) {
                if (src.k == O_IMM) be_opi(e, AOP_SUB, x, a, imm); else be_op(e, AOP_SUB, x, a, b);
                be_stctx(e, x, o_res);
            } else be_stctx(e, res, o_res);
        }
        if (dst.k == O_R8 && writes) put8(t, dst.r, res);
    } else {
        int r = writes ? res : x;
        if (src.k == O_IMM) be_opi(e, aop, r, a, imm); else be_op(e, aop, r, a, b);
        if (lz) be_stctx(e, r, o_res);
        if (dst.k == O_R8 && writes) put8(t, dst.r, r);
    }
    if (dst.k == O_MEM && writes) be_store(e, w, V_T1, V_T0, 1, d->next);
    return 1;
}

/* PUSH is SMC-checked like any store: a stack that grows into code must
   invalidate it (seed 51187). mode 2 = invalidate but continue (CALL). */
B86_COLD static void emit_push(Tx *t, int v, uint16_t next, int mode)
{
    be_opi(t->e, AOP_SUB, B86_SP, B86_SP, 2);
    be_ea(t->e, V_T0, B86_SS, B86_SP, -1, 0);         /* SS: untranslated, T1 kept */
    be_store(t->e, 1, v, V_T0, mode, next);
}

B86_COLD static void emit_pop_to(Tx *t, int dst)
{
    be_ea(t->e, V_T0, B86_SS, B86_SP, -1, 0);
    if (dst == B86_SP) { be_load(t->e, 1, B86_SP, V_T0); return; }
    be_load(t->e, 1, dst, V_T0);
    be_opi(t->e, AOP_ADD, B86_SP, B86_SP, 2);
}

/* reg += (DF ? -n : n), n = 1 or 2; uses `tmp` */
B86_COLD static void step_index(Tx *t, int reg, int w, int tmp)
{
    be_ldctx(t->e, tmp, OFF(flags));
    be_ubfx(t->e, tmp, tmp, 10, 1);                /* DF */
    be_opi(t->e, AOP_ADD, reg, reg, w ? 2u : 1u);
    be_op_lsl(t->e, AOP_SUB, reg, reg, tmp, w ? 2u : 1u);
}

/* LODS / STOS / MOVS without REP natively; REP MOVS/STOS via bulk helper */
B86_COLD static int emit_string(Tx *t, Insn *d)
{
    Emit *e = t->e;
    uint8_t op = d->op;
    int w = op & 1;
    int sseg = d->seg != 0xFF ? d->seg : B86_DS;
    if (d->rep) {
        if (op >= 0xAC) return 0;
        be_call_helper(e, (void *)b86h_rep, (uint32_t)d->ip | (uint32_t)d->next << 16);
        t->inc_pending = t->inc_pending;          /* helper does not touch lazy flags */
        return 1;
    }
    switch (op & 0xFE) {
    case 0xAC:                                    /* LODS */
        be_ea(e, V_T0, sseg, B86_SI, -1, 0);
        if (w) be_load(e, 1, B86_AX, V_T0);
        else { be_load(e, 0, V_T1, V_T0); put8(t, 0, V_T1); }
        step_index(t, B86_SI, w, V_T0);
        return 1;
    case 0xAA:                                    /* STOS */
        be_ea(e, V_T0, B86_ES, B86_DI, -1, 0);
        be_store(e, w, B86_AX, V_T0, 1, d->next);
        step_index(t, B86_DI, w, V_T0);
        return 1;
    default: {                                    /* MOVS */
        be_ea(e, V_T0, sseg, B86_SI, -1, 0);
        int v = be_has_t2 ? V_T2 : V_T1;
        be_load(e, w, v, V_T0);
        if (!be_has_t2) be_stctx(e, V_T1, OFF(scratch));   /* ES base load clobbers T1 */
        be_ea(e, V_T0, B86_ES, B86_DI, -1, 0);
        if (!be_has_t2) be_ldctx(e, V_T1, OFF(scratch));
        be_store(e, w, v, V_T0, 1, d->next);
        step_index(t, B86_SI, w, V_T0);
        be_ldctx(e, V_T0, OFF(flags));
        be_ubfx(e, V_T0, V_T0, 10, 1);
        be_opi(e, AOP_ADD, B86_DI, B86_DI, w ? 2u : 1u);
        be_op_lsl(e, AOP_SUB, B86_DI, B86_DI, V_T0, w ? 2u : 1u);
        return 1; }
    }
}

B86_COLD static Op op_rm(const Insn *d, int w)
{
    Op o = { O_MEM, 0, 0 };
    if (d->mod == 3) { o.k = w ? O_R16 : O_R8; o.r = d->rm; }
    return o;
}
B86_COLD static Op op_reg(const Insn *d, int w) { Op o = { w ? O_R16 : O_R8, d->reg, 0 }; return o; }
B86_COLD static Op op_imm(uint32_t v) { Op o = { O_IMM, 0, v }; return o; }

/* Load a r/m operand (w) into vreg dst (low bits valid). */
B86_COLD static void load_rm(Tx *t, const Insn *d, int w, int dst)
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
B86_COLD static int lower(Tx *t, Insn *d)
{
    Emit *e = t->e;
    uint8_t op = d->op;
    int w = d->w;

    if (d->virt) {                    /* specialized continuation prefix */
        DeferInfo o;
        if (d->virt == 1 && d->lazy && !d->defer && defer_info(d, &o)) {
            int full = (d->fdef & B86_CF) != 0;
            Insn *nx = &t->v[t->cur + 1];
            int qafter = full && nx->virt && !(nx->fdef & B86_CF);
            emit_record(t, &o, full && !qafter && t->inc_pending);
            if (full && !qafter) t->inc_pending = 0;
            if (!full) t->inc_pending = 1;
        }
        return 1;
    }

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
    if (op >= 0x50 && op <= 0x57) { emit_push(t, op & 7, d->next, 1); return 1; }
    if (op >= 0x58 && op <= 0x5F) { emit_pop_to(t, op & 7); return 1; }
    if (op >= 0xB0 && op <= 0xB7) { be_movi(e, V_T0, d->imm); put8(t, op & 7, V_T0); return 1; }
    if (op >= 0xB8 && op <= 0xBF) { be_movi(e, op & 7, d->imm); return 1; }
    if ((op >= 0x60 && op <= 0x7F)) {
        int cc = op & 15;
        uint8_t *site;
        int back = d->target <= d->ip;
        if (d->fused) {
            site = be_jcc(e, back ? (d->mode ^ 1) : d->mode);   /* ARM condition */
        } else if (d->zsrc) {                    /* ZF/SF from the producer's lz_res */
            const Insn *p = &t->v[d->zsrc - 1];
            int bits = (p->op == 0x80 || p->op == 0x82) ? 8 : (p->op & 1) ? 16 : 8;
            be_ldctx(e, V_T0, OFF(lz_res));
            int nz;                              /* taken when (tested value != 0) */
            if (cc == 4 || cc == 5) {
                if (bits == 8) be_ubfx(e, V_T0, V_T0, 0, 8);
                nz = cc == 5;
            } else {
                be_ubfx(e, V_T0, V_T0, (unsigned)bits - 1u, 1);
                nz = cc == 8;
            }
            if (back) nz = !nz;
            site = (bits == 16 && (cc == 4 || cc == 5)) ? (nz ? be_cbnz16(e, V_T0) : be_cbz16(e, V_T0))
                                                        : (nz ? be_cbnz(e, V_T0) : be_cbz(e, V_T0));
        } else {
            DBG_SITE(t, d), be_call_cond(e, cc);
            t->inc_pending = 0;
            site = back ? be_cbz(e, V_T0) : be_cbnz(e, V_T0);
        }
        if (back) emit_back_exit(t, site, d->target);
        else if (d->ijoin) t->jsite[d->ijoin] = site;
        else add_side(t, site, d->target, d->ip);
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
        if (d->reg == 4 || d->reg == 5) {     /* MUL / IMUL, flags dead */
            int sgn = d->reg == 5;
            load_rm(t, d, w, V_T1);
            if (w) {
                if (sgn) { be_sbfx(e, V_T1, V_T1, 0, 16); be_sbfx(e, V_T0, B86_AX, 0, 16); }
                else { be_ubfx(e, V_T1, V_T1, 0, 16); be_ubfx(e, V_T0, B86_AX, 0, 16); }
                be_mul(e, V_T0, V_T0, V_T1);
                be_mov(e, B86_AX, V_T0);
                be_ubfx(e, B86_DX, V_T0, 16, 16);
            } else {
                if (sgn) { be_sbfx(e, V_T1, V_T1, 0, 8); be_sbfx(e, V_T0, B86_AX, 0, 8); }
                else { be_ubfx(e, V_T1, V_T1, 0, 8); be_ubfx(e, V_T0, B86_AX, 0, 8); }
                be_mul(e, B86_AX, V_T0, V_T1);
            }
            if (d->live & d->fdef) {             /* flags live: CF/OF/SF/ZF/PF in C */
                be_call_light(e, (void *)b86h_mulflags, w ? V_T0 : B86_AX, (uint32_t)w | (uint32_t)sgn << 1);
                t->inc_pending = 0;
            }
            return 1;
        }
        if (d->reg == 6 && w) {                  /* DIV r/m16: UDIV + MLS */
            if (d->mod != 3) { ea_modrm(t, d); be_load(e, 1, V_T1, V_T0); }
            else be_ubfx(e, V_T1, d->rm, 0, 16);
            uint8_t *zero = be_cbz(e, V_T1);
            be_ubfx(e, V_T0, B86_AX, 0, 16);
            be_op_lsl(e, AOP_ORR, V_T0, V_T0, B86_DX, 16);   /* n = DX:AX */
            be_udiv(e, B86_DX, V_T0, V_T1);                  /* q (DX as temp) */
            be_ubfx(e, B86_AX, B86_DX, 16, 16);              /* q >> 16 (AX as temp) */
            uint8_t *ovf = be_cbnz(e, B86_AX);
            be_mov(e, B86_AX, B86_DX);                       /* AX = q */
            be_mls(e, B86_DX, B86_DX, V_T1, V_T0);           /* DX = n - q*s */
            uint8_t *done = be_jmp(e);
            be_bind(e, ovf, e->p);                           /* restore AX, DX */
            be_mov(e, B86_AX, V_T0);
            be_ubfx(e, B86_DX, V_T0, 16, 16);
            be_bind(e, zero, e->p);
            be_call_step(e, d->ip, d->next);                 /* interpreter raises INT 0 */
            t->inc_pending = 1;
            be_bind(e, done, e->p);
            return 1;
        }
        if (d->reg == 7 && w) {                  /* IDIV r/m16: SDIV + MLS */
            if (d->mod != 3) { ea_modrm(t, d); be_load(e, 1, V_T1, V_T0); be_sbfx(e, V_T1, V_T1, 0, 16); }
            else be_sbfx(e, V_T1, d->rm, 0, 16);
            uint8_t *zero = be_cbz(e, V_T1);
            be_ubfx(e, V_T0, B86_AX, 0, 16);
            be_op_lsl(e, AOP_ORR, V_T0, V_T0, B86_DX, 16);   /* n = DX:AX (int32) */
            be_sdiv(e, B86_DX, V_T0, V_T1);                  /* q (DX as temp) */
            be_opi(e, AOP_ADD, B86_AX, B86_DX, 0x8000u);     /* q + 32768 must be 1..65535 */
            uint8_t *ovf1 = be_cbz(e, B86_AX);
            be_ubfx(e, B86_AX, B86_AX, 16, 16);
            uint8_t *ovf2 = be_cbnz(e, B86_AX);
            be_mov(e, B86_AX, B86_DX);                       /* AX = q */
            be_mls(e, B86_DX, B86_DX, V_T1, V_T0);           /* DX = n - q*s */
            uint8_t *done = be_jmp(e);
            be_bind(e, ovf1, e->p); be_bind(e, ovf2, e->p);  /* restore AX, DX */
            be_mov(e, B86_AX, V_T0);
            be_ubfx(e, B86_DX, V_T0, 16, 16);
            be_bind(e, zero, e->p);
            be_call_step(e, d->ip, d->next);                 /* interpreter raises INT 0 */
            t->inc_pending = 1;
            be_bind(e, done, e->p);
            return 1;
        }
        return 0;
    case 0x86:                                   /* XCHG r8, r8 */
        if (d->mod != 3) return 0;
        if (d->reg == d->rm) return 1;
        be_mov(e, V_T0, get8(t, d->reg, V_T0));
        be_mov(e, V_T1, get8(t, d->rm, V_T1));
        put8(t, d->reg, V_T1);
        put8(t, d->rm, V_T0);
        return 1;
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
        emit_push(t, V_T1, d->next, 1);
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
    case 0xD0: case 0xD1:
        if (is_rcx(d)) {                         /* RCL/RCR r,1 (AX..DI, AL..BL) */
            unsigned bits = w ? 16 : 8;
            int sh = w ? 16 : 24;
            int r = w ? d->rm : d->rm;           /* rm < 4 for bytes */
            int rcl = d->reg == 2;
            int ctx = d->lazy != 0;              /* CF/OF needed in ctx flags */
            /* carry-in -> T0 */
            if (d->fused) {
                be_get_carry(e, V_T0, d->cmode);
                if (ctx) {                       /* fold older records first (OF must not be overridden) */
                    be_stctx(e, V_T0, OFF(scratch));
                    flags_if_lazy(t, d, 1); t->inc_pending = 0;
                    be_ldctx(e, V_T0, OFF(scratch));
                }
            } else {
                flags_if_lazy(t, d, ctx); if (ctx) t->inc_pending = 0;
                be_ldctx(e, V_T0, OFF(flags));
                be_ubfx(e, V_T0, V_T0, 0, 1);
            }
            /* NZCV for fused consumers, from the original value */
            if (d->arm) {
                if (rcl) be_addsub_sh(e, 0, V_T1, V_T1, r, r, sh);   /* C = MSB, V = OF */
                else be_carry_from_bit(e, V_T1, r, 0);              /* C = bit 0 */
            }
            if (rcl) {
                be_op_lsl(e, AOP_ORR, V_T1, V_T0, r, 1);         /* (a << 1) | cf */
                if (ctx) be_ubfx(e, V_T0, r, bits - 1, 1);       /* CF out */
                be_bfi(e, r, V_T1, 0, bits);
                if (ctx) {                                        /* OF = MSB(res) ^ CF */
                    be_ubfx(e, V_T1, r, bits - 1, 1);
                    be_op(e, AOP_EOR, V_T1, V_T1, V_T0);
                }
            } else {
                be_ubfx(e, V_T1, r, 1, bits - 1);
                be_op_lsl(e, AOP_ORR, V_T1, V_T1, V_T0, bits - 1); /* (a >> 1) | cf << top */
                if (ctx) be_ubfx(e, V_T0, r, 0, 1);              /* CF out */
                be_bfi(e, r, V_T1, 0, bits);
                if (ctx) {                                        /* OF = two top bits of res */
                    be_op_lsl(e, AOP_EOR, V_T1, r, r, 1);
                    be_ubfx(e, V_T1, V_T1, bits - 1, 1);
                }
            }
            if (ctx) {                           /* flags = flags & ~(CF|OF) | CF | OF */
                be_op_lsl(e, AOP_ORR, V_T0, V_T0, V_T1, 11);
                be_ldctx(e, V_T1, OFF(flags));
                be_opi(e, AOP_AND, V_T1, V_T1, (uint16_t)~(B86_CF | B86_OF));
                be_op(e, AOP_ORR, V_T1, V_T1, V_T0);
                be_stctx(e, V_T1, OFF(flags));
            }
            return 1;
        }
        if (d->reg < 2) {                        /* ROL/ROR r,1 (register operand) */
            unsigned bits = w ? 16 : 8;
            int r = w ? d->rm : (d->rm & 3);
            be_ubfx(e, V_T0, r, 0, bits);
            be_op_lsl(e, AOP_ORR, V_T0, V_T0, r, bits);          /* a:a */
            be_ubfx(e, V_T0, V_T0, d->reg == 0 ? bits - 1 : 1, bits);
            int fl = (d->live & d->fdef) != 0;
            if (fl) { be_ubfx(e, V_T1, r, 0, bits); be_op_lsl(e, AOP_ORR, V_T1, V_T1, V_T0, 16); }
            if (w) be_mov(e, r, V_T0); else be_bfi(e, r, V_T0, 0, 8);
            if (fl) { be_call_light(e, (void *)b86h_rot1, V_T1, (uint32_t)d->reg | (uint32_t)w << 1); t->inc_pending = 0; }
            return 1;
        }
        /* fall through: SHL/SHR/SAR by 1 */
    {                   /* SHL/SHR/SAR by 1 */
        int mem = d->mod != 3;
        int sh = w ? 16 : 24;
        unsigned bits = w ? 16 : 8;
        int kind = (d->reg == 4 ? LZ_SHL8 : d->reg == 5 ? LZ_SHR8 : LZ_SAR8) + w;
        int a, res, x = -1;
        if (mem) { a = res = V_T1; if (d->arm) { if (!be_has_t2) return 0; x = V_T2; } }
        else if (w) { a = res = d->rm; x = V_T0; }
        else { a = (d->rm < 4) ? d->rm : V_T1; res = V_T1; x = V_T0; }
        if (mem) ea_modrm(t, d);
        if (d->lazy) {
            be_stctx_imm(e, (uint32_t)kind, OFF(lz_kind));
            if (t->inc_pending) { be_stctx_imm(e, LZ_NONE, OFF(lz_ikind)); t->inc_pending = 0; }
        }
        if (mem) be_load(e, w, V_T1, V_T0);
        else if (!w && d->rm >= 4) be_ubfx(e, V_T1, d->rm - 4, 8, 8);
        if (d->lazy) be_stctx(e, a, OFF(lz_a));
        if (d->arm && d->reg == 4) be_addsub_sh(e, 0, x, res, a, a, sh);   /* x = a<<sh; ADDS x,x,x */
        else if (d->reg == 4) be_lsl(e, res, a, 1);
        else if (d->arm) {                                       /* SHR/SAR: C = bit 0, then shift */
            be_carry_from_bit(e, x, a, 0);
            if (d->reg == 5) be_ubfx(e, res, a, 1, bits - 1); else be_sbfx(e, res, a, 1, bits - 1);
        }
        else if (d->reg == 5) be_ubfx(e, res, a, 1, bits - 1);
        else be_sbfx(e, res, a, 1, bits - 1);
        if (d->lazy) be_stctx(e, res, OFF(lz_res));
        if (mem) be_store(e, w, V_T1, V_T0, 1, d->next);
        else if (!w) put8(t, d->rm, res);
        return 1; }
    case 0xD2: case 0xD3: {                   /* SHL/SHR/SAR by CL, flags dead */
        int mem = d->mod != 3;
        int v;
        if (mem) { ea_modrm(t, d); be_load(e, w, V_T1, V_T0); v = V_T1; }
        else if (w) v = d->rm;
        else { v = V_T1; if (d->rm < 4) be_mov(e, V_T1, d->rm); else be_ubfx(e, V_T1, d->rm - 4, 8, 8); }
        int src = v;
        if (d->reg == 5) { be_ubfx(e, V_T1, v, 0, w ? 16 : 8); src = V_T1; }
        else if (d->reg == 7) { be_sbfx(e, V_T1, v, 0, w ? 16 : 8); src = V_T1; }
        be_shift_reg(e, d->reg == 4 ? 0 : d->reg == 5 ? 1 : 2, v, src, B86_CX);
        if (mem) be_store(e, w, V_T1, V_T0, 1, d->next);
        else if (!w) put8(t, d->rm, V_T1);
        return 1; }
    case 0xF5: case 0xF8: case 0xF9: {        /* CMC / CLC / STC */
        /* new CF -> T0 (CMC: carry-in from NZCV when fused, else ctx) */
        if (op != 0xF5) be_movi(e, V_T0, op == 0xF9 ? 1u : 0u);
        else if (d->fused) be_get_carry(e, V_T0, d->cmode);
        else {
            flags_if_lazy(t, d, 0);                /* an overlay keeps CF: may stay pending */
            be_ldctx(e, V_T0, OFF(flags));
            be_ubfx(e, V_T0, V_T0, 0, 1);
        }
        if (op == 0xF5) be_opi(e, AOP_EOR, V_T0, V_T0, 1u);
        if (d->lazy) {                             /* CF into ctx flags */
            if (op != 0xF5 || d->fused) {
                be_stctx(e, V_T0, OFF(scratch));
                flags_if_lazy(t, d, 0);
                be_ldctx(e, V_T0, OFF(scratch));
            }
            be_ldctx(e, V_T1, OFF(flags));
            be_opi(e, AOP_AND, V_T1, V_T1, (uint16_t)~B86_CF);
            be_op(e, AOP_ORR, V_T1, V_T1, V_T0);
            be_stctx(e, V_T1, OFF(flags));
        }
        if (d->arm) be_carry_from_bit(e, V_T1, V_T0, 0);   /* NZCV: C = CF for fused consumers */
        return 1; }
    case 0xE2: /* LOOP */
        be_opi(e, AOP_SUB, B86_CX, B86_CX, 1);
        if (d->inv) { add_side(t, be_cbz16(e, B86_CX), d->target, d->ip); return 1; }
        if (d->target <= d->ip) emit_back_exit(t, be_cbz16(e, B86_CX), d->target);
        else add_side(t, be_cbnz16(e, B86_CX), d->target, d->ip);
        return 1;
    case 0xE3: /* JCXZ */
        if (d->target <= d->ip) emit_back_exit(t, be_cbnz16(e, B86_CX), d->target);
        else add_side(t, be_cbz16(e, B86_CX), d->target, d->ip);
        return 1;
    case 0xE0: case 0xE1: { /* LOOPNZ / LOOPZ */
        be_opi(e, AOP_SUB, B86_CX, B86_CX, 1);
        DBG_SITE(t, d), be_call_cond(e, op == 0xE1 ? 4 : 5);   /* E / NE */
        t->inc_pending = 0;
        uint8_t *skip = be_cbz16(e, B86_CX);
        add_side(t, be_cbnz(e, V_T0), d->target, d->ip);
        be_bind(e, skip, e->p);
        return 1; }
    case 0xE8: /* CALL near: push return, chain to target */
        emit_deferred(t, t->cur - 1, d->live);
        be_movi(e, V_T1, d->next);
        emit_push(t, V_T1, d->next, 2);
        be_exit_chain(e, d->target, d->target <= d->ip);
        return 1;
    case 0xE9: case 0xEB:
        if (d->endsynth) emit_end_synth(t, d->endsynth - 1); else emit_deferred(t, t->cur - 1, d->live);
        be_exit_chain(e, d->target, d->target <= d->ip);
        return 1;
    case 0xC3: case 0xC1: case 0xC2: case 0xC0: {
        int P, Q, pa, qa;
        uint16_t spadd = (op == 0xC2 || op == 0xC0) ? (uint16_t)(2 + d->imm) : 2;
        deferred_pq(t, t->cur - 1, d->live, &P, &Q, &pa, &qa);
        DeferInfo op_, oq_;
        int sp_used = (pa && defer_info(&t->v[P], &op_) && (defer_regs(&op_) & (1u << B86_SP))) ||
                      (qa && defer_info(&t->v[Q], &oq_) && (defer_regs(&oq_) & (1u << B86_SP)));
        if (!(pa || qa) || sp_used || t->j->nrm >= RMN) {
            emit_deferred(t, t->cur - 1, d->live);
            be_ea(e, V_T0, B86_SS, B86_SP, -1, 0);
            be_load(e, 1, V_T0, V_T0);
            be_opi(e, AOP_ADD, B86_SP, B86_SP, spadd);
            be_exit_ret(e, d->ip);
            return 1;
        }
        /* records only on the cold paths; the cache hit enters a continuation
           specialized with the pending producers as a virtual prefix */
        be_ea(e, V_T0, B86_SS, B86_SP, -1, 0);
        be_load(e, 1, V_T0, V_T0);
        be_opi(e, AOP_ADD, B86_SP, B86_SP, spadd);
        uint8_t *site, *bne, *birq, *bhit;
        be_ret_cache(e, &site, &bne, &birq, &bhit);
        e->no_count = 1;                                        /* ret_cache counted */
        uint8_t *miss = e->p;                                   /* miss -> lookup */
        be_stctx(e, V_T0, OFF(ip)); emit_deferred(t, t->cur - 1, d->live); be_ldctx(e, V_T0, OFF(ip));
        be_exit_ip_reg(e, V_T0);
        be_bind(e, birq, e->p);                                 /* irq */
        be_stctx(e, V_T0, OFF(ip)); emit_deferred(t, t->cur - 1, d->live); be_ldctx(e, V_T0, OFF(ip));
        be_exit_irq_ip(e);
        uint8_t *fill = e->p;                                   /* first use */
        be_stctx(e, V_T0, OFF(ip)); emit_deferred(t, t->cur - 1, d->live); be_ldctx(e, V_T0, OFF(ip));
        be_ret_fill(e, d->ip, site);
        uint8_t *recp = e->p;
        emit_deferred(t, t->cur - 1, d->live);                  /* record, then normal block */
        uint8_t *recb = be_jmp(e);
        be_bind(e, bne, fill); be_bind(e, bhit, fill); be_bind(e, recb, fill);
        e->no_count = 0;
        RetMeta *m = &t->j->rm[t->j->nrm++];
        m->site = site; m->miss = miss; m->recp = recp; m->recb = recb; m->npre = 0;
        if (pa) { m->pre[m->npre] = t->v[P]; m->pre[m->npre].virt = 1; m->npre++; }
        if (Q >= 0 && (qa || pa)) { m->pre[m->npre] = t->v[Q]; m->pre[m->npre].virt = (uint8_t)(qa ? 1 : 2); m->npre++; }
        return 1; }
    case 0xFF:
        if (d->reg == 4) {           /* JMP near r/m */
            emit_deferred(t, t->cur - 1, d->live);
            load_rm(t, d, 1, V_T0);
            be_exit_ip_reg(e, V_T0);
            return 1;
        }
        if (d->reg == 2) {           /* CALL near r/m (needs 3 temps) */
            emit_deferred(t, t->cur - 1, d->live);
            load_rm(t, d, 1, V_T2);
            be_movi(e, V_T1, d->next);
            emit_push(t, V_T1, d->next, 2);
            be_mov(e, V_T0, V_T2);
            be_exit_ip_reg(e, V_T0);
            return 1;
        }
        if (d->reg >= 6) {           /* PUSH r/m */
            load_rm(t, d, 1, V_T1);
            emit_push(t, V_T1, d->next, 1);
            return 1;
        }
        return 0;
    case 0xF4:
        emit_deferred(t, t->cur - 1, d->live);
        be_exit_ip_imm(e, d->next, XR_HALT);
        return 1;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        return emit_string(t, d);
    case 0x9C:                                   /* PUSHF */
        (DBG_SITE(t, d), be_call_flags(e)); t->inc_pending = 0;
        be_ldctx(e, V_T1, OFF(flags));
        be_opi(e, AOP_AND, V_T1, V_T1, 0x0FD5u);
        be_opi(e, AOP_ORR, V_T1, V_T1, 0xF002u);
        emit_push(t, V_T1, d->next, 1);
        return 1;
    case 0x9D:                                   /* POPF */
        emit_pop_to(t, V_T1);
        be_opi(e, AOP_AND, V_T1, V_T1, 0x0FD5u);
        be_opi(e, AOP_ORR, V_T1, V_T1, 0xF002u);
        be_stctx(e, V_T1, OFF(flags));
        be_stctx_imm(e, LZ_NONE, OFF(lz_kind));
        be_stctx_imm(e, LZ_NONE, OFF(lz_ikind));
        t->inc_pending = 0;
        return 1;
    case 0x9E:                                   /* SAHF */
        (DBG_SITE(t, d), be_call_flags(e)); t->inc_pending = 0;
        be_ldctx(e, V_T0, OFF(flags));
        be_ubfx(e, V_T1, B86_AX, 8, 8);
        be_opi(e, AOP_AND, V_T1, V_T1, 0xD5u);
        be_opi(e, AOP_ORR, V_T1, V_T1, 0x02u);
        be_bfi(e, V_T0, V_T1, 0, 8);
        be_stctx(e, V_T0, OFF(flags));
        return 1;
    case 0x9F:                                   /* LAHF */
        (DBG_SITE(t, d), be_call_flags(e)); t->inc_pending = 0;
        be_ldctx(e, V_T0, OFF(flags));
        be_opi(e, AOP_AND, V_T0, V_T0, 0xD5u);
        be_opi(e, AOP_ORR, V_T0, V_T0, 0x02u);
        be_bfi(e, B86_AX, V_T0, 8, 8);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Block cache                                                              */
/* ------------------------------------------------------------------------ */

B86_HOT static uint32_t map_hash(uint32_t key) { return (key * 2654435761u) >> (32 - B86_MAP_BITS); }
B86_HOT static uint32_t fast_hash(uint32_t key) { return (key ^ (key >> 16)) & (B86_FASTN - 1); }

B86_HOT static Block *map_find(J *j, uint32_t key)
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

B86_HOT static void map_insert(J *j, uint32_t idx)
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

B86_HOT static void fast_reset(J *j)
{
    for (uint32_t i = 0; i < B86_FASTN; ++i) {
        j->fast[i].key = 0xFFFFFFFFu;
        j->fast[i].host = be_code_ptr(j->e.x_miss);
    }
}

B86_HOT void b86_jit_flush(J *j)
{
    for (uint32_t i = 0; i < j->nblk; ++i) {
        Block *b = &j->blk[i];
        {   /* insns and entry lie in [glo, ghi); a wrapping one-insn block has ghi < glo */
            uint32_t lo = b->glo >> 3, hi = b->ghi > b->glo ? (b->ghi + 7u) >> 3 : lo + 1u;
            if (hi > COVN) hi = COVN;
            if (lo < hi) { memset(j->cov + lo, 0, hi - lo); memset(j->ent + lo, 0, hi - lo); }
        }
        j->map[b->map_slot] = 0;
        j->pg_head[b->glo >> PG_SHIFT] = 0;
        for (int r = 0; r < b->nx; ++r) {
            j->pg_head[b->xlo[r] >> PG_SHIFT] = 0;
            for (uint32_t l = b->xlo[r] >> B86_LINE_SHIFT; l <= (b->xhi[r] - 1) >> B86_LINE_SHIFT; ++l) j->codemap[l] = 0;
        }
        uint32_t f = fast_hash(b->key);
        j->fast[f].key = 0xFFFFFFFFu;
        j->fast[f].host = be_code_ptr(j->e.x_miss);
        for (uint32_t l = b->glo >> B86_LINE_SHIFT; l <= (b->ghi - 1) >> B86_LINE_SHIFT; ++l)
            j->codemap[l] = 0;
    }
    j->nblk = 0;
    j->nrm = 0;
    /* A new generation: entries that were hot stay hot (retranslated on
       first dispatch); lukewarm ones start over. Zeroing everything made
       every flush re-interpret the whole hot set hot_threshold times. */
    if (j->heat)
        for (uint32_t i = 0; i < HEATN; ++i)
            j->heat[i] = (j->hot_threshold && j->heat[i] >= j->hot_threshold) ? (uint8_t)j->hot_threshold : 0;
    j->heat_ticks = 0;
    j->e.p = j->code_start;
    j->flush_gen++;
    j->st.flushes++;
}

B86_HOT static void kill_block(J *j, uint32_t idx)
{
    Block *b = &j->blk[idx];
    if (b->dead) return;
    b->dead = 1;
    j->dead[idx] = 1;
    for (int r = 0; r <= b->nx; ++r) {
        uint32_t lo = r ? b->xlo[r - 1] : b->glo, hi = r ? b->xhi[r - 1] : b->ghi;
        for (uint32_t l = lo >> B86_LINE_SHIFT; l <= (hi - 1) >> B86_LINE_SHIFT; ++l)
            if (j->codemap[l] && j->codemap[l] != 255) j->codemap[l]--;
    }
    uint32_t f = fast_hash(b->key);
    if (j->fast[f].key == b->key) {
        j->fast[f].key = 0xFFFFFFFFu;
        j->fast[f].host = be_code_ptr(j->e.x_miss);
    }
    be_kill_entry(b->host, b->dead_stub);
    if (b->spec && b->ret_b) be_patch_branch(b->ret_b, b->ret_rec);
    j->st.smc_invalidations++;
}

B86_HOT static void invalidate(J *j, uint32_t lo, uint32_t hi)
{
    uint32_t p0 = (lo >> PG_SHIFT), p1 = (hi - 1) >> PG_SHIFT;
    if (p0) p0--;
    for (uint32_t p = p0; p <= p1 && p < NPG; ++p) {
        /* Each guarded range owns its own encoded bucket link.  Unlink
         * dead blocks in O(1) while walking.  Preserve extra ranges and
         * other pages' independent links.  Blocks are not recycled. */
        uint32_t *link = &j->pg_head[p];
        uint32_t depth = 0, removed = 0;
        while (*link) {
            uint32_t v = *link;
            uint32_t idx = (v & 0x0FFFFFFFu) - 1, r = v >> 28;
            Block *b = &j->blk[idx];
            uint32_t *next = r ? &b->next_pgx[r - 1] : &b->next_pg;
            ++depth;
            ++j->st.smc_bucket_visits;
            if (b->dead) {
                ++j->st.smc_dead_skips;
                ++removed;
                *link = *next;
                continue;
            }
            uint32_t blo = r ? b->xlo[r - 1] : b->glo;
            uint32_t bhi = r ? b->xhi[r - 1] : b->ghi;
            if (blo < hi && lo < bhi) {
                kill_block(j, idx);
                ++removed;
                *link = *next;
                continue;
            }
            link = next;
        }
        if (depth > j->st.smc_bucket_max_depth)
            j->st.smc_bucket_max_depth = depth;
        if (removed) {
            ++j->st.smc_bucket_compactions;
            j->st.smc_bucket_unlinks += removed;
        }
    }
}

B86_HOT static void smc_hook(B86Cpu *c, uint32_t lin, uint32_t n)
{
    J *j = c->jit;
    j->st.smc_hits++;
    invalidate(j, lin, lin + n);
}

/* ------------------------------------------------------------------------ */
/* Translation                                                              */
/* ------------------------------------------------------------------------ */

static Block *translate_ex(J *j, uint32_t cs, uint16_t ip, const Insn *pre, int npre);

/* A new entry point inside a live block's instructions: kill that block so
   its retranslation stops (joins) here instead of keeping a second copy of
   the tail. Costs one retranslation; keeps each guest instruction in the
   cache about once, which is what lets a whole program's hot code fit. */
B86_COLD static void split_at(J *j, uint32_t a)
{
    uint32_t p1 = a >> PG_SHIFT, p0 = p1 ? p1 - 1 : 0;
    for (uint32_t p = p0; p <= p1 && p < NPG; ++p) {
        for (uint32_t v = j->pg_head[p]; v; ) {
            uint32_t idx = (v & 0x0FFFFFFFu) - 1, r = v >> 28;
            Block *b = &j->blk[idx];
            v = r ? b->next_pgx[r - 1] : b->next_pg;
            if (r || b->dead) continue;
            uint32_t entry = (b->key >> 16) * 16u + (b->key & 0xFFFFu);
            if (entry < a && a < b->iend) { kill_block(j, idx); j->st.smc_invalidations--; j->st.splits++; }
        }
    }
}
B86_COLD static Block *translate(J *j, uint32_t cs, uint16_t ip) { return translate_ex(j, cs, ip, NULL, 0); }

B86_COLD static Block *translate_ex(J *j, uint32_t cs, uint16_t ip, const Insn *pre, int npre)
{
    B86Cpu *c = j->cpu;
    Insn *v = (Insn *)j->tv;                  /* B86_CALLOC'd: off the SRAM .bss */
    int n = npre;
    for (int i = 0; i < npre; ++i) v[i] = pre[i];
    uint32_t base = cs << 4;
    uint32_t glo = base + ip, ghi = glo;
    uint16_t pc = ip;
    unsigned maxn = j->max_insns ? j->max_insns : DEF_INSNS;
    /* Joins: a superblock stops (and chains) where it would run into code
       already translated in this generation - at another block's entry, or
       where it re-enters translated code from untranslated code. Without
       this, paths that merge are translated once per path (3DBENCH: 1.7x
       the code per cache generation and constant flushes). */
    int prev_cov = !j->no_join && BIT_GET(j->cov, base + pc);
    /* (No splitting: killing the block that covers a new mid entry frees no
       code space within the generation, costs a retranslation, and leaves
       every branch already chained to it detouring through the dispatcher
       via its dead stub. The tail is duplicated once instead.) */
    if (prev_cov && !npre && !BIT_GET(j->ent, base + pc) && j->split_mid) split_at(j, base + pc);

    while ((unsigned)n < maxn + (unsigned)npre) {
        if (n > npre && !j->no_join) {
            uint32_t a = base + pc;
            if (BIT_GET(j->ent, a)) { j->st.joins++; break; }
            int cv = BIT_GET(j->cov, a);
            if (cv && !prev_cov) { j->st.joins++; break; }
            prev_cov = cv;
        }
        if (n > npre && b86_page_mapped(j->cpu, base + pc)) break;
        Insn *d = &v[n];
        decode(c, cs, pc, d);
        classify(d);
        if (d->next < pc) { if (n == npre) { d->cls = C_END; d->native = 0; } else break; } /* wraps */
        if (n > npre && base + d->next - glo > MAX_SPAN) break;
        ghi = base + d->next;
        n++;
        if (ends_block(d)) break;
        pc = d->next;
    }
    /* Unroll a small self-loop (back-edge to the block start): copies end in
       the INVERTED branch, which exits to the loop's fall-through and
       otherwise falls into the next copy; only the last copy carries the
       interrupt poll and the back-edge. Semantics are unchanged. */
    if (!j->single_step && !j->max_insns && !npre) {
        int k = -1;
        for (int i = 0; i < n; ++i) {
            Insn *d = &v[i];
            if (d->cls == C_JCC && d->native && d->target == ip &&
                ((d->op >= 0x60 && d->op <= 0x7F) || d->op == 0xE2)) { k = i; break; }
        }
        if (k >= 0) {
            int body = k + 1, U = body <= 6 ? 4 : body <= 12 ? 2 : 1;
            while (U > 1 && n + (U - 1) * body > 120) U--;
            if (U > 1) {
                Insn *w = (Insn *)j->tw;
                int m = 0;
                for (int u = 0; u < U - 1; ++u)
                    for (int i = 0; i <= k; ++i) {
                        w[m] = v[i];
                        if (i == k) {
                            Insn *b = &w[m];
                            uint16_t fall = b->next;
                            if (b->op == 0xE2) b->inv = 1; else b->op ^= 1;
                            b->target = fall;
                            b->next = ip;
                        }
                        m++;
                    }
                for (int i = 0; i < n; ++i) w[m++] = v[i];
                memcpy(v, w, sizeof(Insn) * (size_t)m);
                n = m;
            }
        }
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
            v[i].arm = v[i].lazy = v[i].fused = v[i].mode = v[i].prod = v[i].cmode = v[i].synth = v[i].endsynth = v[i].zsrc = 0;
            v[i].live = v[i].live_taken = 0;
        }
        Guard g = { glo0, ghi0, {0, 0}, {0, 0}, 0 };
        if (!j->no_join) plan_joins(v, npre, n);
        uint16_t live_out = ALLF;
        if (last->cls == C_JMP || last->cls == C_CALL) live_out = lookahead(j, cs, last->target, &g);
        else if (!ends_block(last)) live_out = lookahead(j, cs, last->next, &g);
        analyze(j, cs, v, n, live_out, &g);
        glo = g.lo; ghi = g.hi;

        memset(b, 0, sizeof *b);
        j->dead[idx] = 0;
        b->key = (cs << 16) | ip;
        b->glo = glo; b->ghi = ghi; b->ninsn = (uint16_t)n; b->iend = ghi0;
        b->nx = (uint8_t)g.nx;
        for (int r = 0; r < g.nx; ++r) { b->xlo[r] = g.xlo[r]; b->xhi[r] = g.xhi[r]; }
        e->blk = idx;
        e->nslow = 0;
        e->overflow = 0;
        e->p = start;
        b->host = start;
        memset(&tx, 0, sizeof tx);
        tx.j = j; tx.e = e; tx.cs = cs;
        tx.inc_pending = 1;
        tx.v = v;

        int demoted = 0;
        uint64_t helpers = 0;
        int jskip[128], radj = 0;
        memset(jskip, 0xFF, sizeof jskip);
        for (int q = npre; q < n; ++q) if (v[q].ijoin) jskip[v[q].ijoin] = v[q].ijoin - q - 1;
        for (int i = 0; i < n; ++i) {
            Insn *d = &v[i];
            if (jskip[i] >= 0) {                 /* join of an in-block forward branch */
                be_count(e, (uint32_t)jskip[i]);   /* fall-through path ran the skipped insns */
                if (tx.jsite[i]) { be_bind(e, tx.jsite[i], e->p); j->st.inner_branches++; }
                radj += jskip[i];
            }
            e->retire = (uint32_t)(i + 1 - npre - radj);
            tx.cur = i;
            d->pendb = (uint8_t)tx.inc_pending;
            int ok = d->native && lower(&tx, d);
            if (!ok && d->native) { d->native = 0; demoted = 1; break; }
            if (!ok) {
                be_call_step(e, d->ip, d->next);
                tx.inc_pending = 0;            /* the interpreter materializes */
                helpers++;
                if (d->cls != C_SEQ && d->cls != C_JCC) { be_exit_dyn(e); break; }
            }
            if (i == n - 1 && !ends_block(d)) {
                if (d->endsynth) emit_end_synth(&tx, d->endsynth - 1); else emit_deferred(&tx, i, d->live);
                be_exit_chain(e, d->next, 0);
            }
        }
        if (demoted && attempt < n) continue;
        j->st.helper_insns += helpers;
        break;
    }
    for (int s = 0; s < tx.nside; ++s) {
        be_bind(e, tx.side[s].site, e->p);
        e->retire = tx.side[s].retire;
        if (v[tx.side[s].idx].synth) emit_synth_cf(&tx, (v[tx.side[s].idx].op & 15) == 2);
        else emit_deferred(&tx, tx.side[s].idx, tx.side[s].live);
        be_exit_chain(e, tx.side[s].target, tx.side[s].poll);
    }
    be_finish_block(e);
    b->dead_stub = e->p;
    e->retire = 0;
    if (npre) {
        /* entry state = the RET site's state: write every pending virtual
           record before leaving (the retranslated target may need it) */
        int P = -1, Q = -1;
        for (int i = 0; i < npre; ++i) { if (v[i].fdef & B86_CF) P = i; else Q = i; }
        DeferInfo o;
        if (P >= 0 && v[P].virt == 1 && defer_info(&v[P], &o)) emit_record(&tx, &o, Q < 0);
        if (Q >= 0 && v[Q].virt == 1 && defer_info(&v[Q], &o)) emit_record(&tx, &o, 0);
    }
    be_exit_ip_imm(e, ip, XR_LOOKUP);

    if (e->overflow || tx.fail) {
        /* out of space or an internal limit: flush and retry smaller */
        b86_jit_flush(j);
        if (tx.fail && maxn > 4) { unsigned m = j->max_insns; j->max_insns = maxn / 2; Block *r = translate_ex(j, cs, ip, pre, npre); j->max_insns = m; return r; }
        return translate_ex(j, cs, ip, pre, npre);
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
    for (int i = npre; i < n; ++i) BIT_SET(j->cov, base + v[i].ip);
    if (!npre) BIT_SET(j->ent, base + ip);
    if (npre) b->spec = 1; else map_insert(j, idx);
    for (int r = 0; r <= b->nx; ++r) {
        uint32_t lo = r ? b->xlo[r - 1] : b->glo, hi = r ? b->xhi[r - 1] : b->ghi;
        uint32_t pg = lo >> PG_SHIFT;
        if (r) b->next_pgx[r - 1] = j->pg_head[pg]; else b->next_pg = j->pg_head[pg];
        j->pg_head[pg] = (idx + 1) | ((uint32_t)r << 28);
        for (uint32_t l = lo >> B86_LINE_SHIFT; l <= (hi - 1) >> B86_LINE_SHIFT; ++l)
            if (j->codemap[l] != 255) j->codemap[l]++;
        if (j->shadow) memcpy(j->shadow + lo, c->mem + lo, hi - lo);
        if (c->code_hook) c->code_hook(c, lo, hi - lo);
    }
    j->st.blocks++;
    j->st.guest_insns += (uint64_t)(n - npre);
    j->st.host_bytes += (uint64_t)(e->p - start);
    return b;
}

/* ------------------------------------------------------------------------ */
/* Helpers called from generated code                                       */
/* ------------------------------------------------------------------------ */

#ifdef B86_HELPER_HISTO
uint64_t b86_helper_histo[256];
uint64_t b86_helper_histo2[32];   /* [F6/F7 ? 16 : 0] + w*8 + reg */
uint32_t b86_helper_ip[1<<20];
#endif
B86_HOT uint32_t b86h_step(B86Cpu *c, uint32_t ip_next, uint32_t blk)
{
    c->jit->st.rt_step++;
#ifdef B86_HELPER_HISTO
    {
        uint32_t a = (c->seg[B86_CS] << 4) + (ip_next & 0xFFFFu), k = 0;
        uint8_t op = c->mem[a];
        while ((op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0xF2 || op == 0xF3 || op == 0xF0) && k++ < 4) op = c->mem[++a];
        b86_helper_histo[op]++;
        { extern uint32_t b86_helper_ip[1<<20]; b86_helper_ip[((c->seg[B86_CS] << 4) + (ip_next & 0xFFFFu)) & 0xFFFFF]++; }
        if (op == 0xD1 || op == 0xD0 || op == 0xF7 || op == 0xF6) b86_helper_histo2[(op & 1) * 8 + ((c->mem[a + 1] >> 3) & 7) + (op >= 0xF6 ? 16 : 0)]++;
    }
#endif
    J *j = c->jit;
    uint32_t cs0 = c->seg[B86_CS];
    c->ip = ip_next & 0xFFFFu;
    int r = b86_step(c);
    if (r == B86_HALT) { c->irq |= 0x80000000u; return 1; }
    if (c->irq) return 1;                 /* host asked to stop (e.g. from an INT hook) */
    if (c->seg[B86_CS] != cs0 || c->ip != (ip_next >> 16)) return 1;
    return j->dead[blk];
}

/* REP MOVS / REP STOS in bulk. Same results as the interpreter: element-
   wise forward/backward semantics, 16-bit SI/DI wrap, A20-on addressing.
   Falls back to the interpreter for anything unusual. */
#ifdef B86_PAGED
B86_HOT static inline uint8_t *tr_host(const B86Cpu *c, uint8_t *nominal)
{
    return nominal + c->pt[((uintptr_t)nominal >> 12) & 511u];
}
/* REP STOS/MOVS over nominal ranges that touch a mapped page: page-sized
   chunks, each translated. Returns 0 (caller's flat path) if none mapped. */
B86_HOT static int rep_paged(B86Cpu *c, uint8_t *dst, uint8_t *src, uint32_t span, int w)
{
    int any = 0;
    for (uintptr_t p = (uintptr_t)dst >> 12; p <= ((uintptr_t)dst + span - 1) >> 12; ++p) any |= c->pt[p & 511u] != 0;
    if (src) for (uintptr_t p = (uintptr_t)src >> 12; p <= ((uintptr_t)src + span - 1) >> 12; ++p) any |= c->pt[p & 511u] != 0;
    if (!any) return 0;
    if (src && dst > src && dst < src + span) {          /* overlap: element-exact */
        uint32_t sz = w ? 2u : 1u;
        for (uint32_t i = 0; i < span; i += sz) {
            uint8_t lo = *tr_host(c, src + i), hi = w ? *tr_host(c, src + i + 1) : 0;
            *tr_host(c, dst + i) = lo;
            if (w) *tr_host(c, dst + i + 1) = hi;
        }
        return 1;
    }
    uint8_t lo = (uint8_t)c->r[B86_AX], hi = (uint8_t)(c->r[B86_AX] >> 8);
    for (uint32_t done = 0; done < span; ) {
        uint32_t lim = span - done, r;
        r = 4096u - (uint32_t)(((uintptr_t)dst + done) & 4095u); if (r < lim) lim = r;
        if (src) { r = 4096u - (uint32_t)(((uintptr_t)src + done) & 4095u); if (r < lim) lim = r; }
        uint8_t *hd = tr_host(c, dst + done);
        if (src) memmove(hd, tr_host(c, src + done), lim);
        else if (!w) memset(hd, lo, lim);
        else for (uint32_t k = 0; k < lim; ++k) hd[k] = ((done + k) & 1u) ? hi : lo;
        done += lim;
    }
    return 1;
}
#endif

B86_HOT uint32_t b86h_rep(B86Cpu *c, uint32_t ip_next, uint32_t blk)
{
    J *j = c->jit;
    j->st.rt_rep++;
    uint32_t cs = c->seg[B86_CS];
    uint16_t ip = (uint16_t)ip_next;
    int sseg = B86_DS;
    uint8_t op;
    for (uint16_t p = ip;; ++p) {
        op = *b86_host(c, (cs << 4) + p);
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) sseg = (op >> 3) & 3;
        else if (op != 0xF2 && op != 0xF3 && op != 0xF0 && op != 0xF1) break;
    }
    uint32_t n = (uint16_t)c->r[B86_CX];
    int w = op & 1, movs = op < 0xA8;
    uint32_t sz = w ? 2u : 1u;
    int back = (c->flags & B86_DF) != 0;
    uint16_t si = (uint16_t)c->r[B86_SI], di = (uint16_t)c->r[B86_DI];
    uint32_t span = n * sz;
#ifdef B86_COND_HISTO
    { extern uint64_t b86_rep_bytes[4]; extern uint32_t b86_rep_es[65536];
      b86_rep_bytes[(movs ? 2 : 0) + w] += span; b86_rep_es[c->seg[B86_ES]] += span; }
#endif
    /* fast path: forward, no 16-bit wrap of SI/DI during the run */
    if (!back && n && (uint32_t)di + span <= 0x10000u && (!movs || (uint32_t)si + span <= 0x10000u)) {
        uint8_t *dst = c->segp[B86_ES] + di;
#ifdef B86_PAGED
        {
            uint32_t pg = (uint32_t)(dst - c->mem) >> 12;
            if (pg < (B86_MEM_BYTES >> 12)) c->rep_page_bytes[pg] += span;
        }
        if (rep_paged(c, dst, movs ? c->segp[sseg] + si : NULL, span, w)) {
            if (movs) si = (uint16_t)(si + span);
        } else
#endif
        if (movs) {
            uint8_t *src = c->segp[sseg] + si;
            if (dst > src && dst < src + span) {          /* overlap: element-exact */
                if (w) for (uint32_t i = 0; i < n; ++i) { uint8_t lo = src[2 * i], hi = src[2 * i + 1]; dst[2 * i] = lo; dst[2 * i + 1] = hi; }
                else for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
            } else memmove(dst, src, span);
            si = (uint16_t)(si + span);
        } else if (!w) memset(dst, (int)(c->r[B86_AX] & 0xFF), span);
#ifndef B86_REP_WIDE
        else { uint8_t lo = (uint8_t)c->r[B86_AX], hi = (uint8_t)(c->r[B86_AX] >> 8);
               for (uint32_t i = 0; i < n; ++i) { dst[2 * i] = lo; dst[2 * i + 1] = hi; } }
#else
        else {                                     /* STOSW: 32-bit stores (2 words each) */
            uint32_t ax = c->r[B86_AX] & 0xFFFFu, i = 0;
            uint8_t lo = (uint8_t)ax, hi = (uint8_t)(ax >> 8);
            if (((uintptr_t)dst & 1u) == 0) {
                if (((uintptr_t)dst & 2u) && n) { *(uint16_t *)dst = (uint16_t)ax; i = 1; }
                uint32_t pat = ax | (ax << 16);
                uint32_t *d32 = (uint32_t *)(dst + 2 * i);
                uint32_t pairs = (n - i) / 2u;
                for (uint32_t k = 0; k < pairs; ++k) d32[k] = pat;
                i += 2u * pairs;
            }
            for (; i < n; ++i) { dst[2 * i] = lo; dst[2 * i + 1] = hi; }
        }
#endif
        di = (uint16_t)(di + span);
        c->r[B86_CX] = 0; c->r[B86_SI] = si; c->r[B86_DI] = di;
        c->ip = ip_next >> 16;
        uint32_t lin = (uint32_t)(dst - c->mem);
        uint32_t l0 = lin >> B86_LINE_SHIFT, l1 = (lin + span - 1) >> B86_LINE_SHIFT;
        for (uint32_t l = l0; l <= l1; ++l)
            if (j->codemap[l]) { j->st.smc_hits++; invalidate(j, lin, lin + span); break; }
        return j->dead[blk];
    }
    return b86h_step(c, ip_next, blk);
}

B86_HOT void b86h_rot1(B86Cpu *c, uint32_t a_res, uint32_t kind)
{
    uint32_t res = a_res >> 16, w = (kind >> 1) & 1u, sb = w ? 0x8000u : 0x80u;
    uint32_t cf, of;
    c->jit->st.rt_light++;
    b86_flags_materialize(c);
    if ((kind & 1u) == 0) { cf = res & 1u; of = ((res & sb) != 0) ^ cf; }          /* ROL */
    else { cf = (res & sb) != 0; of = ((res ^ (res << 1)) & sb) != 0; }           /* ROR */
    c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF)) | (cf ? B86_CF : 0u) | (of ? B86_OF : 0u);
}

B86_HOT void b86h_mulflags(B86Cpu *c, uint32_t prod, uint32_t kind)
{
    uint32_t w = kind & 1u, sgn = (kind >> 1) & 1u, hi, ov;
    c->jit->st.rt_light++;
    b86_flags_materialize(c);
    if (w) { hi = prod >> 16; ov = sgn ? (int32_t)prod != (int16_t)prod : hi != 0; }
    else { hi = (prod >> 8) & 0xFFu; ov = sgn ? (int32_t)(int16_t)prod != (int8_t)prod : (prod & 0xFF00u) != 0; }
    uint32_t m = w ? 0xFFFFu : 0xFFu, sb = w ? 0x8000u : 0x80u, h = hi & m;
    uint32_t f = c->flags & ~(uint32_t)(B86_CF | B86_OF | B86_SF | B86_ZF | B86_PF);
    if (ov) f |= B86_CF | B86_OF;
    if (h & sb) f |= B86_SF;
    f |= b86_parity[h & 0xFFu];
    c->flags = f;                                   /* ZF cleared (interpreter semantics) */
}

B86_HOT uint32_t b86h_cond(B86Cpu *c, uint32_t cc)
{
    c->jit->st.rt_cond++;
#ifdef B86_COND_HISTO
    b86_cond_ip[c->retired & 0xFFFFFu]++;
#endif
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

B86_HOT void b86h_flags(B86Cpu *c)
{
    c->jit->st.rt_flags++;
#ifdef B86_COND_HISTO
    b86_flags_ip[c->retired & 0xFFFFFu]++;
#endif
    b86_flags_materialize(c);
}

B86_HOT uint32_t b86h_smc(B86Cpu *c, uint8_t *host, uint32_t lenflags, uint32_t blk)
{
    J *j = c->jit;
    uint32_t lin = (uint32_t)(host - c->mem);
    if (host < c->mem || lin >= B86_MEM_BYTES) return 0;   /* a mapped frame: never holds code */
    j->st.smc_hits++;
    j->st.rt_smc++;
    invalidate(j, lin, lin + (lenflags & 0xFFu));
    if (lenflags & 0x100u) return 0;          /* invalidate but keep going (CALL push) */
    return j->dead[blk];
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

#ifdef B86_PAGED
B86_HOT int b86_page_mapped(const B86Cpu *c, uint32_t lin)
{
    return c->pt[((uintptr_t)(c->mem + lin) >> 12) & 511u] != 0;
}
int b86_map_range(B86Cpu *c, uint32_t lin, uint32_t len, uint8_t *frame)
{
    if (!be_paged || ((uintptr_t)c->mem & 4095u) || (lin & 4095u) || (len & 4095u) || !len ||
        lin + len > B86_MEM_BYTES || !frame)
        return -1;
    if (c->jit && c->jit->cm_table) {
        /* stores into the frame index the SMC table with UBFX(addr, 6, 15):
           those entries must be neither the page table nor guest lines */
        uint32_t base = (uint32_t)(((uintptr_t)c->mem >> B86_LINE_SHIFT) & (CM_PAGED - 1u));
        for (uint32_t a = 0; a < len + 64u; a += 64u) {
            uint32_t i = (uint32_t)(((uintptr_t)(frame + a) >> B86_LINE_SHIFT) & (CM_PAGED - 1u));
            /* page-table bytes [0, 2 KiB) are mostly zero; an occasional
               nonzero byte only sends a store to the (guarded) slow path */
            if (i >= base && i < base + B86_LINES) return -2;   /* frame collides with guest lines */
        }
    }
    if (c->jit && c->jit->cm_table) c->pt = (int32_t *)(void *)c->jit->cm_table;
    for (uint32_t a = lin; a < lin + len; a += 4096u) if (b86_page_mapped(c, a)) return -1;
    J *j = c->jit;
    if (j && j->codemap)
        for (uint32_t l = lin >> B86_LINE_SHIFT; l < (lin + len) >> B86_LINE_SHIFT; ++l)
            if (j->codemap[l]) return -3;        /* translated code lives there */
    memcpy(frame, c->mem + lin, len);
    for (uint32_t a = 0; a < len; a += 4096u)
        c->pt[((uintptr_t)(c->mem + lin + a) >> 12) & 511u] = (int32_t)((intptr_t)(frame + a) - (intptr_t)(c->mem + lin + a));
    return 0;
}
void b86_unmap_range(B86Cpu *c, uint32_t lin, uint32_t len)
{
    for (uint32_t a = lin & ~4095u; a < lin + len && a < B86_MEM_BYTES; a += 4096u) {
        uint32_t i = (uint32_t)(((uintptr_t)(c->mem + a) >> 12) & 511u);
        if (!c->pt[i]) continue;
        memcpy(c->mem + a, c->mem + a + c->pt[i], 4096u);
        c->pt[i] = 0;
    }
}
#else
B86_HOT int b86_page_mapped(const B86Cpu *c, uint32_t lin) { (void)c; (void)lin; return 0; }
int b86_map_range(B86Cpu *c, uint32_t lin, uint32_t len, uint8_t *frame) { (void)c; (void)lin; (void)len; (void)frame; return -1; }
void b86_unmap_range(B86Cpu *c, uint32_t lin, uint32_t len) { (void)c; (void)lin; (void)len; }
#endif

B86_HOT size_t b86_jit_hot_bytes(void)
{
    return B86_FASTN * sizeof(B86Fast) + (be_paged ? CM_PAGED : ((B86_LINES + 15u) & ~15u));
}

void b86_jit_destroy(J *j);
B86_HOT J *b86_jit_create(B86Cpu *c, void *code, size_t size)
{
    return b86_jit_create_ex(c, code, size, NULL, 0);
}

/* `hot` (optional, >= b86_jit_hot_bytes(), fastest RAM) holds the tables
   that translated code reads on every indirect jump and every store: the
   fast lookup table and the SMC line map. Everything else is touched only
   by the dispatcher and may live in slow memory (PSRAM). */
B86_HOT J *b86_jit_create_ex(B86Cpu *c, void *code, size_t size, void *hot, size_t hot_size)
{
    /* the SMC line map is indexed by host address >> 6: guest memory must be
       64-byte aligned or stores near line edges would be checked on the
       wrong line */
    if (((uintptr_t)c->mem & ((1u << B86_LINE_SHIFT) - 1u)) != 0) return NULL;
    J *j = calloc(1, sizeof *j);                 /* small, hot: normal (SRAM) heap */
    if (!j) return NULL;
    j->dead = calloc(MAXB, 1);
    j->tv = B86_CALLOC(128, sizeof(Insn));
    j->tw = B86_CALLOC(128, sizeof(Insn));
    j->cpu = c;
    j->buf = code;
    j->buf_end = (uint8_t *)code + size;
    j->blk = B86_CALLOC(MAXB, sizeof *j->blk);
    j->map = B86_CALLOC(MAPN, sizeof *j->map);
    j->pg_head = B86_CALLOC(NPG, sizeof *j->pg_head);
    j->rm = B86_CALLOC(RMN, sizeof *j->rm);
    j->cov = B86_CALLOC(COVN, 1);
    j->heat_key = B86_CALLOC(HEATN, sizeof *j->heat_key);
    j->heat = B86_CALLOC(HEATN, 1);
    j->ent = B86_CALLOC(COVN, 1);
    if (hot && hot_size >= b86_jit_hot_bytes()) {
        memset(hot, 0, b86_jit_hot_bytes());
        j->fast = (B86Fast *)hot;
        j->cm_table = (uint8_t *)hot + B86_FASTN * sizeof(B86Fast);
        j->hot_external = 1;
    } else {
        j->fast = B86_CALLOC(B86_FASTN, sizeof *j->fast);
        j->cm_table = B86_CALLOC(be_paged ? CM_PAGED : B86_LINES, 1);
    }
    j->codemap = j->cm_table;
    if (be_paged && j->cm_table) {
        uint32_t base = (uint32_t)(((uintptr_t)c->mem >> B86_LINE_SHIFT) & (CM_PAGED - 1u));
        if (((uintptr_t)c->mem & 4095u) || base + B86_LINES > CM_PAGED) {   /* layout the table cannot index */
            if (!j->hot_external) B86_FREE(j->cm_table);
            j->cm_table = NULL;
        }
        if (base < 0x800u && j->cm_table) {        /* page table lives in table bytes [0, 2 KiB) */
            if (!j->hot_external) B86_FREE(j->cm_table);
            j->cm_table = NULL;
        }
        j->codemap = j->cm_table ? j->cm_table + base : NULL;
    }
    if (!j->dead || !j->tv || !j->tw || !j->blk || !j->map || !j->pg_head || !j->rm || !j->cov || !j->ent || !j->heat_key || !j->heat || !j->fast || !j->codemap) { b86_jit_destroy(j); return NULL; }
    c->jit = j;
    c->codemap = j->codemap;
    c->codemap_host = be_paged ? j->cm_table : j->codemap - ((uintptr_t)c->mem >> B86_LINE_SHIFT);
#ifdef B86_PAGED
    if (be_paged) {                               /* move any existing mappings into the shared table */
        memcpy(j->cm_table, c->pt, 512u * sizeof(int32_t));
        c->pt = (int32_t *)(void *)j->cm_table;
    }
#endif
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

B86_HOT void b86_jit_destroy(J *j)
{
    if (!j) return;
    if (j->cpu) { j->cpu->jit = NULL; j->cpu->codemap = NULL; j->cpu->smc_hook = NULL; }
    B86_FREE(j->blk); B86_FREE(j->map); B86_FREE(j->pg_head); B86_FREE(j->rm);
    B86_FREE(j->cov); B86_FREE(j->ent); B86_FREE(j->heat_key); B86_FREE(j->heat);
    if (!j->hot_external) { B86_FREE(j->fast); B86_FREE(j->cm_table); }
    free(j->dead);
    B86_FREE(j->tv); B86_FREE(j->tw);
    free(j);
}

B86_HOT const B86JitStats *b86_jit_stats(J *j) { return &j->st; }
B86_HOT void b86_jit_set_max_block(J *j, unsigned n) { j->max_insns = n; }
B86_HOT void b86_jit_set_lookahead(J *j, int on) { j->no_lookahead = !on; }
B86_HOT void b86_jit_set_no_fast(J *j, int on) { j->single_step = on; }
B86_HOT void b86_jit_set_no_chain(J *j, int on) { j->no_chain = on; }
B86_HOT void b86_jit_set_no_spec(J *j, int on) { j->no_spec = on; }
B86_HOT void b86_jit_set_no_join(J *j, int on) { j->no_join = on; }
B86_HOT void b86_jit_set_hot_threshold(J *j, unsigned n) { j->hot_threshold = n > 255u ? 255u : n; }
B86_HOT void b86_jit_set_count_retired(J *j, int on) { j->e.count_ret = on; }
B86_HOT void b86_jit_invalidate(J *j, uint32_t lin, uint32_t len) { if (len) invalidate(j, lin, lin + len); }

B86_HOT void b86_jit_set_shadow(J *j, uint8_t *shadow) { j->shadow = shadow; }

/* Someone else may have written [lin, lin+len). Compare only the 64-byte
   lines that hold translated code against the shadow and invalidate exactly
   the bytes that differ, so data stored next to code kills nothing. Without
   a shadow this falls back to invalidating the whole range. */
B86_HOT uint32_t b86_jit_sync_external(J *j, uint32_t lin, uint32_t len)
{
    uint32_t killed = 0;
    if (!len) return 0;
    if (!j->shadow) { invalidate(j, lin, lin + len); return 1; }
    const uint8_t *mem = j->cpu->mem;
    uint32_t l0 = lin >> B86_LINE_SHIFT, l1 = (lin + len - 1) >> B86_LINE_SHIFT;
    for (uint32_t l = l0; l <= l1 && l < B86_LINES; ++l) {
        if (!j->codemap[l]) continue;
        uint32_t base = l << B86_LINE_SHIFT, n = 1u << B86_LINE_SHIFT;
        if (base + n > B86_MEM_BYTES) n = B86_MEM_BYTES - base;
        if (!memcmp(mem + base, j->shadow + base, n)) continue;
        uint32_t a = 0, b = n;
        while (mem[base + a] == j->shadow[base + a]) ++a;
        while (mem[base + b - 1] == j->shadow[base + b - 1]) --b;
        invalidate(j, base + a, base + b);
        memcpy(j->shadow + base, mem + base, n);
        ++killed;
    }
    return killed;
}
B86_HOT void b86_jit_set_count_exits(J *j, int on) { j->e.count_exits = on; }
B86_HOT void b86_jit_set_single_step(J *j, int on)
{
    j->single_step = on;
    j->max_insns = on ? 1u : 0u;
    j->no_lookahead = on;
}

/* Tiering. Code is translated only once an entry point has been reached
   hot_threshold times within a decay window; until then the dispatcher
   interprets it one straight-line run at a time. Code that runs a few times
   per frame (setup, per-object code, DOS itself) then never occupies the
   code buffer, which is what keeps a program's hot loops resident instead of
   flushing the whole cache every frame. */
/* cold-run control classes: 1 ends the run, 2 short conditional branch
   (continue when not taken), 4 prefix; FF and 8E are decided by ModRM.reg */
static const uint8_t cold_ctl[256] = {
    [0x0F] = 1, [0x26] = 4, [0x2E] = 4, [0x36] = 4, [0x3E] = 4,
    [0x60 ... 0x7F] = 2, [0x9A] = 1,
    [0xC0 ... 0xC3] = 1, [0xC8 ... 0xCF] = 1,
    [0xE0 ... 0xE3] = 2, [0xE8 ... 0xEB] = 1,
    [0xF0 ... 0xF3] = 4, [0xF4] = 1,
};

B86_HOT static int cold_run(J *j, B86Cpu *c, uint32_t key, int force)
{
    uint32_t h = (key * 2654435761u) >> (32 - HEAT_BITS);
    if (j->heat_key[h] != key) { j->heat_key[h] = key; j->heat[h] = 0; }
    if (++j->heat_ticks >= HEAT_DECAY) {
        j->heat_ticks = 0;
        for (uint32_t i = 0; i < HEATN; ++i) j->heat[i] >>= 1;
    }
    if (j->heat[h] < 255) j->heat[h]++;
    if (!force && j->heat[h] >= j->hot_threshold) return 0;
    /* interpret one straight-line run: stop after a control transfer, at a
       translated entry, at the trap segment, or when the host wants control */
    uint32_t steps = 0;
    for (;;) {
        uint32_t cs = c->seg[B86_CS];
        uint16_t ip0 = (uint16_t)c->ip;
#ifdef B86_COLD_OLD
        { Insn d; decode(c, cs, ip0, &d); classify(&d); }
#endif
        /* (o) cheap control classification from the opcode bytes instead of
           a full decode()+classify() per interpreted instruction */
        uint16_t p = ip0;
        uint8_t op = fb(c, cs, p);
        while ((cold_ctl[op] & 4) && (uint16_t)(p - ip0) < 4u) op = fb(c, cs, ++p);   /* prefixes */
        unsigned k = cold_ctl[op] & 3u;
        if (op == 0xFF || op == 0x8E) {
            uint8_t reg = (fb(c, cs, (uint16_t)(p + 1)) >> 3) & 7;
            k = op == 0xFF ? (reg >= 2 && reg <= 5) : ((reg & 3) == B86_CS);
        }
        uint16_t jnext = (uint16_t)(p + 2);           /* short Jcc/LOOP/JCXZ */
        int r = b86_step(c);
        steps++;
        if (r == B86_HALT) { c->irq |= 0x80000000u; break; }
        /* keep going through a not-taken branch: stopping there would make
           every fall-through a dispatch target, heat it, and translate it
           as its own entry (fragmenting superblocks at each Jcc) */
        if (c->irq || steps >= 64u) break;
        if (c->seg[B86_CS] != cs || c->seg[B86_CS] == c->trap_cs) break;
        if (k == 1 || (k == 2 && (uint16_t)c->ip != jnext)) break;
        if ((uint16_t)c->ip < ip0 && k == 0) break;   /* wrapped the segment */
        if (BIT_GET(j->ent, (c->seg[B86_CS] << 4) + (c->ip & 0xFFFFu))) break;
    }
    if (j->e.count_ret) c->icnt += steps;
#ifdef B86_COND_HISTO
    { extern uint32_t b86_cold_ip[1 << 20]; b86_cold_ip[((key >> 16) * 16u + (key & 0xFFFFu)) & 0xFFFFFu] += steps; }
#endif
    j->st.cold_insns += steps;
    j->st.cold_runs++;
    return 1;
}

B86_HOT int b86_jit_run(B86Cpu *c, uint64_t max_dispatch)
{
#ifdef B86_PAGED
    if (be_paged && c->jit && c->jit->cm_table) c->pt = (int32_t *)(void *)c->jit->cm_table;   /* struct copies */
#endif
    J *j = c->jit;
    uint64_t n = 0;
    uint8_t *patch_site = NULL, *rf_site = NULL;
    uint32_t patch_gen = 0, rf_gen = 0;
    for (;;) {
        if (c->irq & 0x80000000u) { c->irq &= 0x7FFFFFFFu; return B86_HALT; }
        if (c->irq) return B86_EXIT;
        if (c->seg[B86_CS] == c->trap_cs) return B86_TRAP;
        if (n >= max_dispatch) return B86_BUDGET;
        n++;
        uint32_t key = (c->seg[B86_CS] << 16) | (c->ip & 0xFFFFu);
        uint32_t f = fast_hash(key);
        uint8_t *host;
        if (!j->single_step && j->fast[f].key == key && !rf_site &&
            j->fast[f].host != be_code_ptr(j->e.x_miss)) {
            /* SRAM fast table: no block-table access (may live in PSRAM) */
            host = (uint8_t *)(j->fast[f].host & ~(uintptr_t)1u);
            j->st.fast_dispatches++;
        } else {
            Block *b = map_find(j, key);
            if (!b) {
#ifdef B86_NOW
                uint64_t t0 = B86_NOW();
#endif
#ifdef B86_MISSES
                uint64_t m0 = B86_MISSES();
#endif
                if (!j->single_step && b86_page_mapped(c, ((key >> 16) << 4) + (key & 0xFFFFu))) {
                    cold_run(j, c, key, 1);       /* code in a mapped page: never translated */
                    patch_site = NULL; rf_site = NULL;
                    continue;
                }
                if (j->hot_threshold && !j->single_step && cold_run(j, c, key, 0)) {
                    patch_site = NULL; rf_site = NULL;
                    continue;
                }
                b = translate(j, c->seg[B86_CS], (uint16_t)c->ip);
#ifdef B86_NOW
                j->st.translate_us += B86_NOW() - t0;
#endif
#ifdef B86_MISSES
                j->st.translate_misses += B86_MISSES() - m0;
#endif
            }
            host = b->host;
            if (rf_site && rf_gen == j->flush_gen && !j->no_chain) {
                RetMeta *m = NULL;
                for (uint32_t r = 0; r < j->nrm; ++r) if (j->rm[r].site == rf_site) { m = &j->rm[r]; break; }
                if (m && j->no_spec) { be_patch_branch(m->recb, b->host); be_patch_ret(rf_site, (uint16_t)c->ip, m->recp, m->miss); }
                else if (!m) be_patch_ret(rf_site, (uint16_t)c->ip, b->host, NULL);
                else {
                    uint8_t *site = rf_site;
                    uint8_t *normal = b->host;
                    Block *sb = translate_ex(j, c->seg[B86_CS], (uint16_t)c->ip, m->pre, m->npre);
                    if (rf_gen == j->flush_gen) {
                        be_patch_branch(m->recb, normal);
                        sb->ret_b = be_ret_hit_branch(site);
                        sb->ret_rec = m->recp;
                        be_patch_ret(site, (uint16_t)c->ip, sb->host, m->miss);
                    } else { rf_site = NULL; patch_site = NULL; continue; }   /* flushed: re-dispatch */
                }
            }
            rf_site = NULL;
            if (!j->single_step) {
                j->fast[f].key = key;
                j->fast[f].host = be_code_ptr(host);
            }
        }
        if (patch_site && patch_gen == j->flush_gen && !j->no_chain) {
            be_patch_branch(patch_site, host);
            j->st.chains++;
        }
        patch_site = NULL;
        j->st.dispatches++;
#ifdef B86_COND_HISTO
        { extern uint32_t b86_disp_ip[1 << 20]; b86_disp_ip[((key >> 16) * 16u + (key & 0xFFFFu)) & 0xFFFFFu]++; }
#endif
        int r = j->enter(c, be_code_ptr(host));
#ifdef B86_COND_HISTO
        { extern uint32_t b86_xr[8]; b86_xr[r & 7]++; }
#endif
        switch (r) {
        case XR_HALT: return B86_HALT;
        case XR_CHAIN:
            patch_site = (uint8_t *)c->patch;
            patch_gen = j->flush_gen;
            break;
        case XR_RETFILL:
            rf_site = (uint8_t *)c->patch;
            rf_gen = j->flush_gen;
            break;
        default: break;
        }
    }
}

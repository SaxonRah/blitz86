/* B86_PJ_V1 (r70): native Thumb-2 translation of flat 32-bit protected-mode
 * code (DOS/4GW programs). Included by interp.c after the PM stages.
 *
 * Scope (checked on every entry, otherwise the PM40/stage path runs):
 *   CR0.PE, EFLAGS.VM=0, CS 32-bit with base 0 and limit 4 GiB, DS/ES/SS
 *   base 0 and limit 4 GiB (SS.B=1), no PE transition or SS shadow pending.
 * Blocks: straight-line runs of supported instructions ending at a control
 *   transfer, an unsupported instruction or B86_PJ_MAXI instructions.
 * Guest registers live in r4-r11 inside a block; the B86Cpu pointer is kept
 *   at [sp]. Arithmetic flags are written as the interpreter's lazy records
 *   (LZ_ADD32/LZ_SUB32/LZ_LOG32/LZ_INC32/LZ_DEC32 and 8-bit kinds), so every
 *   exit leaves exactly the state the interpreter would. Conditional
 *   branches recompute ARM flags from the record of the last producer in
 *   the same block, or ask pj_cond() otherwise.
 * Memory: every access calls pj_mem() (segment base 0; page walk through
 *   the TLB; refuses page-crossing, unmapped or beyond-RAM accesses). A
 *   refusal exits the block *before* that instruction changes anything, and
 *   the interpreter then executes it (raising #PF where needed).
 * Self-modifying code: linear code pages are tracked in a 4096-bit filter;
 *   any store reported through smc() to a tracked page flushes all blocks.
 */
#ifdef B86_PJ_VERIFY
#include <stdio.h>
#endif
#ifndef B86_PJ_MAXI
#define B86_PJ_MAXI 32u
#endif
#ifndef B86_PJ_HASH
#define B86_PJ_HASH 1024u
#endif
#if !defined(B86_PJ_CHAIN) && defined(B86_PJ_VERIFY)
#define B86_PJ_CHAIN 64u                         /* verify: short chains fit the write log */
#endif
#ifndef B86_PJ_CHAIN
#define B86_PJ_CHAIN 2048u                       /* max instructions per chained run (modimm) */
#endif
#ifndef B86_PJ_CPS
#define B86_PJ_CPS 1024u                         /* exact code-page set size (power of 2) */
#endif
/* BB_STAGE4W2_PROFILE: actual production layout, verified from Pico -D flags.
 * Refuse all others; never resize PJ state silently. */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE && \
    ((B86_PJ_HASH != 512u) || (B86_PJ_CPS != 256u))
#error "Stage4W2 expected B86_PJ_HASH=512 and B86_PJ_CPS=256"
#endif
#ifndef B86_PJ_COLD
#define B86_PJ_COLD __attribute__((noinline,cold))
#endif

/* BB_PJ_PM16_REG_TIER_V1: deliberately conservative nonflat 16-bit CS tier.
 * No native guest data/stack references or in-mode chain links. */
#ifndef BB_PJ_PM16_REG_TIER
#define BB_PJ_PM16_REG_TIER 1
/* BB_PJ_PM16_ADAPTIVE_V1: guarded PM16 admission; disabled = no behavior change. */
#ifndef BB_PJ_PM16_ADAPTIVE
#define BB_PJ_PM16_ADAPTIVE 1
#endif
#endif
typedef struct B86Pj {
    uint8_t *code;
    uint32_t cap, used;
    uint32_t tag[B86_PJ_HASH];          /* eip | 1 (eip is never odd-tagged)  */
    uint32_t ent[B86_PJ_HASH];          /* code offset + 1, or 0 = interpret   */
    uint32_t cpg[128];                  /* linear code page filter (4096 bits) */
    uint32_t cps[B86_PJ_CPS];           /* exact set of code pages (page+1), open addressing */
    /* BB_STAGE4W2_PROFILE: measurements only; no changes to dispatch decisions. */
    unsigned long profile_lookup_misses, profile_slot_replacements;
    unsigned long profile_cps_refusals, profile_last_report;
    unsigned long flush_smc, flush_full, links;
    uint8_t *tail_z, *tail, *dyn, *chk, *entry, *memtramp;   /* shared code at the start of the buffer */
    unsigned long blocks, flushes, runs, insns, exits_mem, declines, fallback;
    unsigned long stop_op[256];         /* which opcode ended blocks           */
    /* BB_PJ_ES_STABLE_CACHE_V1: cache is safe in either ES state;
     * ES-dependent instructions are checked in emitted code at runtime. */
    unsigned es_mode;
    unsigned long es_mode_switches, es_relaxed_entries, es_guarded_ops;
    /* Refused guest EIPs, stored as EIP+1. Rejection is only consulted
     * while ES is non-flat; a stale entry can only force safe C fallback.
     * 64x4-byte direct mapped sites fit the existing 8 KiB PJ state arena. */
    uint32_t es_skip_eip[64];
    unsigned long es_skip_checks, es_skip_hits, es_skip_marks;
    /* BB_PJ_TLB_0F_V1: sampled only at genuine translation failures. */
    unsigned long stop_0f[256];
    unsigned long prof_mem_calls, prof_mem_paged, prof_tlb_hits, prof_walks;
    unsigned long prof_mem_refused, prof_mem_cross, prof_mem_bounds, prof_mem_writes;
#if BB_PJ_PM16_REG_TIER
    uint32_t pm16_active, pm16_base, pm16_limit, pm16_kind;
#endif
/* BB_PJ_PM16_NEGCACHE_V1: not reset by b86_pj_flush().
     * Inexact/stale negative hits only deopt to C; no native execution. */
    unsigned long pm16_neg_hits, pm16_neg_inserts;
#if BB_PJ_PM16_ADAPTIVE
    uint32_t pm16_adapt_calls, pm16_adapt_insns, pm16_adapt_cooldown;
    uint32_t pm16_adapt_skips, pm16_adapt_triggers;
#endif
    int flush_pending;
    /* BB_PJ_REPNE_EFFICIENCY_V1: separate yield vs mapped-memory refusal. */
    unsigned long repne_calls, repne_completed, repne_budget_yields;
    unsigned long repne_refusals, repne_comparisons, repne_spans;
} B86Pj;

unsigned long b86_pj_stat_runs, b86_pj_stat_insns;

/* BB_PJ_ES_SKIP_ASSOC_V1: A/B mode controls only the ES refusal-site
 * lookup policy; both policies occupy the same 64-word state array. */
#ifndef B86_PJ_ES_SKIP_WAYS
#define B86_PJ_ES_SKIP_WAYS 2
#endif
#if B86_PJ_ES_SKIP_WAYS != 1 && B86_PJ_ES_SKIP_WAYS != 2
#error B86_PJ_ES_SKIP_WAYS must be either 1 or 2
#endif

/* ---- Thumb-2 emitter ----------------------------------------------------- */
typedef struct { uint8_t *p, *end; int bad; } PjE;
static void pje16(PjE *e, uint32_t h) { if (e->p + 2 > e->end) { e->bad = 1; return; } e->p[0] = (uint8_t)h; e->p[1] = (uint8_t)(h >> 8); e->p += 2; }
static void pje32(PjE *e, uint32_t h1, uint32_t h2) { pje16(e, h1); pje16(e, h2); }
enum { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP = 13, LR = 14, PC = 15 };
#define GR(i) (4 + (i))                      /* guest reg i -> r4..r11 */
static int pj_modimm(uint32_t v, uint32_t *enc)
{
    unsigned rot;
    if (v < 256u) { *enc = v; return 1; }
    if ((v & 0xFF00FFu) == v && (v >> 16) == (v & 0xFFu)) { *enc = 0x100u | (v & 0xFFu); return 1; }
    if ((v & 0xFF00FF00u) == v && (v >> 24) == ((v >> 8) & 0xFFu)) { *enc = 0x200u | ((v >> 8) & 0xFFu); return 1; }
    if (((v & 0xFFu) * 0x01010101u) == v) { *enc = 0x300u | (v & 0xFFu); return 1; }
    for (rot = 8; rot < 32; ++rot) {
        uint32_t x = (v << rot) | (v >> (32 - rot));
        if (x >= 0x80u && x < 0x100u) { *enc = (rot << 7) | (x & 0x7Fu); return 1; }
    }
    return 0;
}
static void pj_movw(PjE *e, int d, uint32_t v)
{ pje32(e, 0xF240u | ((v >> 1) & 0x400u) | ((v >> 12) & 0xFu), ((v << 4) & 0x7000u) | ((uint32_t)d << 8) | (v & 0xFFu)); }
static void pj_movt(PjE *e, int d, uint32_t v)
{ pje32(e, 0xF2C0u | ((v >> 1) & 0x400u) | ((v >> 12) & 0xFu), ((v << 4) & 0x7000u) | ((uint32_t)d << 8) | (v & 0xFFu)); }
static void pj_imm32(PjE *e, int d, uint32_t v) { pj_movw(e, d, v & 0xFFFFu); if (v >> 16) pj_movt(e, d, v >> 16); }
/* data processing, shifted register: op AND0 BIC1 ORR2 ORN3 EOR4 ADD8 ADC10 SBC11 SUB13 RSB14 */
static void pj_dpr(PjE *e, unsigned op, int s, int d, int n, int m, unsigned type, unsigned amt)
{ pje32(e, 0xEA00u | (op << 5) | ((unsigned)s << 4) | (unsigned)n, ((amt >> 2) << 12) | ((unsigned)d << 8) | ((amt & 3u) << 6) | (type << 4) | (unsigned)m); }
static int pj_dpi(PjE *e, unsigned op, int s, int d, int n, uint32_t v)
{
    uint32_t enc;
    if (!pj_modimm(v, &enc)) return 0;
    pje32(e, 0xF000u | ((enc >> 1) & 0x400u) | (op << 5) | ((unsigned)s << 4) | (unsigned)n, ((enc << 4) & 0x7000u) | ((unsigned)d << 8) | (enc & 0xFFu));
    return 1;
}
static void pj_mov(PjE *e, int d, int m) { pj_dpr(e, 2u, 0, d, PC, m, 0u, 0u); }
static void pj_shift(PjE *e, int d, int m, unsigned type, unsigned n) { pj_dpr(e, 2u, 0, d, PC, m, type, n); }  /* type 0 LSL 1 LSR 2 ASR */
static void pj_ldst(PjE *e, uint32_t h1, int t, int n, unsigned off) { if (off > 0xFFFu) e->bad = 1; pje32(e, h1 | (unsigned)n, ((unsigned)t << 12) | (off & 0xFFFu)); }
_Static_assert(offsetof(B86Cpu, pj_stop) <= 4092u, "PJ addresses B86Cpu fields with 12-bit offsets");
#define PJ_LDR 0xF8D0u
#define PJ_STR 0xF8C0u
#define PJ_LDRB 0xF890u
#define PJ_STRB 0xF880u
#define PJ_LDRH 0xF8B0u
#define PJ_STRH 0xF8A0u
#define PJ_LDRSB 0xF990u
#define PJ_LDRSH 0xF9B0u
static void pj_ext(PjE *e, uint32_t h1, int d, int m) { pje32(e, h1, 0xF080u | ((unsigned)d << 8) | (unsigned)m); }   /* SXTB FA4F SXTH FA0F UXTB FA5F UXTH FA1F */
static void pj_bfi(PjE *e, int d, int n, unsigned lsb, unsigned width)
{ const unsigned msb = lsb + width - 1u; pje32(e, 0xF360u | (unsigned)n, ((lsb >> 2) << 12) | ((unsigned)d << 8) | ((lsb & 3u) << 6) | msb); }
static void pj_ubfx(PjE *e, int d, int n, unsigned lsb, unsigned width)
{ pje32(e, 0xF3C0u | (unsigned)n, ((lsb >> 2) << 12) | ((unsigned)d << 8) | ((lsb & 3u) << 6) | (width - 1u)); }
static void pj_blx(PjE *e, int m) { pje16(e, 0x4780u | ((unsigned)m << 3)); }
static void pj_call(PjE *e, const void *fn)
{
    const intptr_t off = (intptr_t)((uintptr_t)fn & ~(uintptr_t)1u) - (intptr_t)(uintptr_t)(e->p + 4);
    if (off >= -(1 << 24) && off < (1 << 24)) {          /* BL: +-16 MiB */
        const uint32_t s = ((uint32_t)off >> 24) & 1u, i1 = ((uint32_t)off >> 23) & 1u, i2 = ((uint32_t)off >> 22) & 1u;
        const uint32_t j1 = (~(i1 ^ s)) & 1u, j2 = (~(i2 ^ s)) & 1u;
        pje32(e, 0xF000u | (s << 10) | (((uint32_t)off >> 12) & 0x3FFu), 0xD000u | (j1 << 13) | (j2 << 11) | (((uint32_t)off >> 1) & 0x7FFu));
        return;
    }
    pj_imm32(e, R12, (uint32_t)(uintptr_t)fn); pj_blx(e, R12);   /* r12: r0-r3 carry arguments */
}
/* B.W / Bcc.W with a 32-bit placeholder, patched by pj_patch_b */
static uint8_t *pj_b(PjE *e, int cond)
{
    uint8_t *at = e->p;
    if (cond < 0) pje32(e, 0xF000u, 0x9000u); else pje32(e, 0xF000u | ((unsigned)cond << 6), 0x8000u);
    return at;
}
static void pj_patch_b(uint8_t *at, uint8_t *to)
{
    const int32_t off = (int32_t)(to - (at + 4));
    const uint32_t h1 = (uint32_t)at[0] | ((uint32_t)at[1] << 8), h2 = (uint32_t)at[2] | ((uint32_t)at[3] << 8);
    uint32_t n1, n2;
    if ((h2 & 0x1000u)) {                          /* B.W T4: +-16 MiB */
        const uint32_t s = (off >> 24) & 1u, i1 = (off >> 23) & 1u, i2 = (off >> 22) & 1u;
        const uint32_t j1 = (~(i1 ^ s)) & 1u, j2 = (~(i2 ^ s)) & 1u;
        n1 = 0xF000u | (s << 10) | (((uint32_t)off >> 12) & 0x3FFu);
        n2 = 0x9000u | (j1 << 13) | (j2 << 11) | (((uint32_t)off >> 1) & 0x7FFu);
    } else {                                       /* Bcc.W T3: +-1 MiB */
        const uint32_t s = (off >> 20) & 1u, j1 = (off >> 18) & 1u, j2 = (off >> 19) & 1u;
        n1 = (h1 & 0xFBC0u & ~0x03FFu) | (h1 & 0x03C0u) | (s << 10) | (((uint32_t)off >> 12) & 0x3Fu);
        n2 = 0x8000u | (j1 << 13) | (j2 << 11) | (((uint32_t)off >> 1) & 0x7FFu);
    }
    at[0] = (uint8_t)n1; at[1] = (uint8_t)(n1 >> 8); at[2] = (uint8_t)n2; at[3] = (uint8_t)(n2 >> 8);
}

/* ---- runtime helpers called from native code --------------------------- */
#define PJ_PGH(pg) ((((uint32_t)(pg)) * 0x9E3779B1u) >> 20)   /* 4096-bit hashed page filter */
static B86Pj *b86_pj_state(const B86Cpu *c) { return (B86Pj *)c->pj; }
/* Stage4U: keep native translations when x87 writes ordinary guest data.
 * Each cached translated code page is indexed both by its guest linear page
 * and by its physical page, with distinct keys in the existing exact cps
 * table. This preserves detection of aliases without enlarging the 8 KiB
 * PJ state arena. A false-positive bloom hit is resolved by cps. */
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL && \
    ((defined(B86_X87_PG_WRITES_TRIAL) && B86_X87_PG_WRITES_TRIAL) || \
     (defined(B86_X87_PG_CW_STORE) && B86_X87_PG_CW_STORE))
static void b86_x87_pg_note_pj_store(B86Cpu *c,uint32_t physical,unsigned size);
#endif

static int pj_cps_has(const B86Pj *j, uint32_t pg)
{
    uint32_t i = (pg * 0x9E3779B1u) & (B86_PJ_CPS - 1u), k;
    for (k = 0; k < B86_PJ_CPS; ++k, i = (i + 1u) & (B86_PJ_CPS - 1u)) {
        if (j->cps[i] == pg + 1u) return 1;
        if (!j->cps[i]) return 0;
    }
    return 0;
}
/* Physical aliases are stored under tagged page keys. Ordinary guest
 * linear page numbers are at most 20 bits, so 0x40000000 is disjoint.
 * A hit is an exact translated physical code page: self-modifying writes
 * still schedule a full flush, while data-only writes leave PJ intact. */
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL && \
    ((defined(B86_X87_PG_WRITES_TRIAL) && B86_X87_PG_WRITES_TRIAL) || \
     (defined(B86_X87_PG_CW_STORE) && B86_X87_PG_CW_STORE))
static void b86_x87_pg_note_pj_store(B86Cpu *c,uint32_t physical,unsigned size)
{
    B86Pj *j=b86_pj_state(c);
    uint32_t pg,last;
    if(!j || !size || physical>UINT32_MAX-(size-1u))return;
    pg=physical>>12;last=(physical+size-1u)>>12;
    for(;;++pg){
        const uint32_t key=0x40000000u | pg;
        const uint32_t h=PJ_PGH(key);
        if((j->cpg[h>>5]&(1u<<(h&31u))) && pj_cps_has(j,key)){
            j->flush_pending=1;c->pj_stop=1u;return;
        }
        if(pg==last)break;
    }
}
#endif
static int pj_cps_add(B86Pj *j, uint32_t pg)
{
    uint32_t i = (pg * 0x9E3779B1u) & (B86_PJ_CPS - 1u), k;
    for (k = 0; k < (B86_PJ_CPS * 3u) / 4u; ++k, i = (i + 1u) & (B86_PJ_CPS - 1u)) {
        if (j->cps[i] == pg + 1u) return 1;
        if (!j->cps[i]) { j->cps[i] = pg + 1u; return 1; }
    }
    return 0;
}
static inline void b86_pj_note_store(B86Cpu *c, uint32_t lin, uint32_t n)
{
    B86Pj *j = b86_pj_state(c);
    uint32_t a = lin >> 12, b = (lin + n - 1u) >> 12;
    if (!j) return;
    for (; ; ++a) {
        const uint32_t h = PJ_PGH(a);
        if ((j->cpg[h >> 5] & (1u << (h & 31u))) && pj_cps_has(j, a)) { j->flush_pending = 1; c->pj_stop = 1u; return; }
        if (a == b) break;
    }
}
#ifdef B86_PJ_VERIFY
static struct { uint8_t *p; uint8_t n, old[4]; } pj_vl[1024];
static unsigned pj_nvl;
static void pj_vlog(uint8_t *p, unsigned n)
{ if (pj_nvl < 1024u) { pj_vl[pj_nvl].p = p; pj_vl[pj_nvl].n = (uint8_t)n; memcpy(pj_vl[pj_nvl].old, p, n); ++pj_nvl; } }
unsigned long b86_pj_vfy_checked, b86_pj_vfy_bad;
#endif
/* host pointer for flat linear lin, size|write<<8; NULL: let the interpreter do it */
/* BB_PJ_TLB_0F_V1: choose profiler A/B implementation at compile time.
 * Mode 1 skips b86_pg_walk on a TLB hit. Both paths use the identical
 * original pg_walk on TLB misses, with the same page/fault/store ordering.
 * Mode 0 profiles the existing page walker for a controlled A/B test. */
#define BB_PJ_TLB_MODE 1
/* B86_PJ_MEMSTATS_LEAN_V1: hot memory statistics are diagnostic only.
 * Mode 0 keeps all physical/virtual memory, paging, SMC and write behavior;
 * only profiling-counter increments disappear at compile time.
 * Mode 1 retains byte-for-byte original helper statements. */
#ifndef BB_PJ_MEMSTATS
#define BB_PJ_MEMSTATS 0
#endif
/* B86_PJ_READ_TLB_LEAN_V1: optional single-pass read TLB fast path.
 * The only shortcut is a read (no write/other flags), size 1/2/4, fully
 * contained within an already translated page, with a valid existing TLB
 * entry. The original checked helper remains the fallback for everything
 * else. The TLB hit uses exactly the same cached phys calculation and RAM
 * bounds rule as the original b86_pj_mem() path. */
#ifndef BB_PJ_READ_TLB_LEAN
#define BB_PJ_READ_TLB_LEAN 1
#endif
static B86_PM_HOT uint8_t *b86_pj_mem(B86Cpu *c, uint32_t lin, uint32_t sw)
{
    const uint32_t size = sw & 0xFFu;
    B86Pj *j = b86_pj_state(c);
#if BB_PJ_READ_TLB_LEAN
    if ((sw & ~0xFFu) == 0u && c->pg &&
        (size == 1u || size == 2u || size == 4u) &&
        (lin & 0xFFFu) + size <= 0x1000u) {
        const unsigned slot = (lin >> 12) & 63u;
        const uint32_t tag = (lin & 0xFFFFF000u) | 1u;
        if (c->tlb_tag[slot] == tag) {
            const uint32_t phys = c->tlb_phys[slot] | (lin & 0xFFFu);
            if (phys > B86_RAM_BYTES - size) return NULL;
            return b86_host_phys(c, phys);
        }
    }
#endif
    uint32_t phys;
    if (BB_PJ_MEMSTATS && j) ++j->prof_mem_calls;
    if ((lin & 0xFFFu) + size > 0x1000u) {
        if (BB_PJ_MEMSTATS && j) ++j->prof_mem_cross;
        return NULL;
    }
    if (c->pg) {
        const unsigned slot = (lin >> 12) & 63u;
        const uint32_t tag = (lin & 0xFFFFF000u) | 1u;
        const int hit = c->tlb_tag[slot] == tag;
        if (BB_PJ_MEMSTATS && j) {
            ++j->prof_mem_paged;
            if (hit) ++j->prof_tlb_hits;
            else ++j->prof_walks;
        }
#if BB_PJ_TLB_MODE
        if (hit) phys = c->tlb_phys[slot] | (lin & 0xFFFu);
        else if (!b86_pg_walk(c, lin, &phys)) {
            if (BB_PJ_MEMSTATS && j) ++j->prof_mem_refused;
            return NULL;
        }
#else
        if (!b86_pg_walk(c, lin, &phys)) {
            if (BB_PJ_MEMSTATS && j) ++j->prof_mem_refused;
            return NULL;
        }
#endif
    } else phys = lin;
    if (phys > B86_RAM_BYTES - size) {
        if (BB_PJ_MEMSTATS && j) { ++j->prof_mem_refused; ++j->prof_mem_bounds; }
        return NULL;
    }
    if (sw & 0x100u) {
        if (BB_PJ_MEMSTATS && j) ++j->prof_mem_writes;
        /* STAGE4Y: smc() itself calls b86_pj_note_store() under PE.
         * The previous explicit call doubled every native-write code-page
         * lookup. Keep the common SMC handler for both PJ and RM JIT.
         * Installer validates the common handler before changing this file. */
        smc(c, lin, size);
    }
#ifdef B86_PJ_VERIFY
    if (sw & 0x100u) pj_vlog(b86_host_phys(c, phys), size);
#endif
    return b86_host_phys(c, phys);
}
/* B86_PJ_ES8_TLB_FAST_V1: the 26 8A native helper may reuse a
 * read-mapped page already validated by the 386 paging TLB. This avoids
 * b86_pm2_check_access -> b86_pg_probe -> b86_pg_walk on a TLB hit;
 * b86_pj_mem remains responsible for actual physical-bounds validation.
 * Only readable, present, expand-UP data segments with in-range offsets
 * use the shortcut. Every other descriptor and every TLB miss retains the
 * original checked path (including fault/exception behavior). */
#ifndef BB_PJ_ES8_TLB_FAST
#define BB_PJ_ES8_TLB_FAST 1
#endif
static B86_PM_HOT uint8_t *b86_pj_es_mov8_ptr(B86Cpu *c, uint32_t off)
{
#if BB_PJ_ES8_TLB_FAST
    const B86PM1Cache *s = &c->pm386.seg[B86_ES];
    if (c->model == B86_CPU_386 && c->pg && s->valid &&
        (s->access & 0x90u) == 0x90u &&
        (s->access & 0x0Cu) == 0u &&
        off <= s->limit && off <= UINT32_MAX - s->base) {
        const uint32_t at = s->base + off;
        const unsigned slot = (at >> 12) & 63u;
        if (c->tlb_tag[slot] == ((at & 0xFFFFF000u) | 1u))
            return b86_pj_mem(c, at, 1u);
    }
#endif
    {
        B86PM2Fault fault;
        uint32_t linear = 0u;
        if (b86_pm2_check_access(c, B86_ES, off, 1u, B86_PM1_READ,
                                &linear, &fault) != B86_PM2_OK) return NULL;
        return b86_pj_mem(c, linear, 1u);
    }
}

/* DIV/IDIV: dividend hi:lo, divisor d (width w, signed sgn); quotient and
 * remainder in b86_pj_divres. 0 = #DE (the interpreter raises it). */
uint32_t b86_pj_divres[2];
static B86_PM_HOT uint32_t b86_pj_div(uint32_t lo, uint32_t hi, uint32_t d, uint32_t wsgn)
{
    const unsigned w = wsgn & 7u, sgn = (wsgn >> 3) & 1u, bits = 8u * w;
    const uint32_t m = w == 4u ? 0xFFFFFFFFu : (1u << bits) - 1u;
    d &= m;
    if (!d) return 0u;
    if (!sgn) {
        const uint64_t num = w == 4u ? ((uint64_t)hi << 32) | lo : w == 2u ? (uint64_t)(((hi & 0xFFFFu) << 16) | (lo & 0xFFFFu))
                                                                 : (uint64_t)(lo & 0xFFFFu);   /* 8-bit: AX */
        const uint64_t q = num / d;
        if (q > m) return 0u;
        b86_pj_divres[0] = (uint32_t)q; b86_pj_divres[1] = (uint32_t)(num % d);
    } else {
        const int64_t den = w == 4u ? (int32_t)d : w == 2u ? (int16_t)d : (int8_t)d;
        const int64_t num = w == 4u ? (int64_t)(((uint64_t)hi << 32) | lo) :
                            w == 2u ? (int64_t)(int32_t)(((hi & 0xFFFFu) << 16) | (lo & 0xFFFFu)) : (int64_t)(int16_t)lo;
        int64_t q;
        if (den == -1 && num == INT64_MIN) return 0u;
        q = num / den;
        if (q > (int64_t)(m >> 1) || q < -(int64_t)(m >> 1) - 1) return 0u;
        b86_pj_divres[0] = (uint32_t)q; b86_pj_divres[1] = (uint32_t)(num % den);
    }
    return 1u;
}
/* MOVS/STOS/LODS (desc = op | rep<<8 | width<<9), guest registers already in
 * c->r. 1 = instruction completed; 0 = stop here (nothing or part done --
 * REP is restartable at any element). */
static B86_PM_HOT uint32_t b86_pj_str(B86Cpu *c, uint32_t desc)
{
    const unsigned op = desc & 0xFFu, rep = (desc >> 8) & 1u, w = (desc >> 9) & 7u;
    const unsigned kind = op <= 0xA5u ? 0u : op <= 0xABu ? 1u : 2u;
    const uint32_t step = (c->flags & B86_DF) ? (uint32_t)0u - w : w;
    uint32_t n = rep ? c->r[B86_CX] : 1u;
    if (!n) return 1u;
#if defined(B86_PJ_VERIFY) && !defined(B86_PJ_STR_MAX)
#define B86_PJ_STR_MAX 64u                        /* the verifier logs at most this many stores */
#endif
#ifdef B86_PJ_STR_MAX
    if (n > B86_PJ_STR_MAX) return 0u;            /* all or nothing, so a checker can compare */
    {   /* ...including page faults part-way: check every element first */
        uint32_t k, si = c->r[B86_SI], di = c->r[B86_DI], ph;
        for (k = 0; k < n; ++k, si += step, di += step) {
            if (kind != 1u && (((si & 0xFFFu) + w > 0x1000u) || (c->pg && !b86_pg_walk(c, si, &ph)))) return 0u;
            if (kind != 2u && (((di & 0xFFFu) + w > 0x1000u) || (c->pg && !b86_pg_walk(c, di, &ph)))) return 0u;
        }
    }
#endif
    while (n) {
        uint8_t *sp = NULL, *dp = NULL;
        uint32_t v;
        if (kind != 1u && !(sp = b86_pj_mem(c, c->r[B86_SI], w))) return 0u;
        if (kind != 2u && !(dp = b86_pj_mem(c, c->r[B86_DI], w | 0x100u))) return 0u;
        v = kind == 1u ? c->r[0] : w == 4u ? (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)
                                 : w == 2u ? (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) : sp[0];
        if (kind == 2u) {
            if (w == 4u) c->r[0] = v; else if (w == 2u) c->r[0] = (c->r[0] & 0xFFFF0000u) | (v & 0xFFFFu);
            else c->r[0] = (c->r[0] & 0xFFFFFF00u) | (v & 0xFFu);
        } else {
            dp[0] = (uint8_t)v;
            if (w > 1u) { dp[1] = (uint8_t)(v >> 8); if (w > 2u) { dp[2] = (uint8_t)(v >> 16); dp[3] = (uint8_t)(v >> 24); } }
        }
        if (kind != 1u) c->r[B86_SI] += step;
        if (kind != 2u) c->r[B86_DI] += step;
        --n;
        if (rep) c->r[B86_CX] = n;
    }
    return 1u;
}
/* CLC/STC/CMC/CLD/STD on the materialized flags */
static B86_PM_HOT uint32_t b86_pj_flagop(B86Cpu *c, uint32_t op)
{
    b86_flags_materialize(c);
    switch (op) {
    case 0xF8: c->flags &= ~(uint32_t)B86_CF; break;
    case 0xF9: c->flags |= B86_CF; break;
    case 0xF5: c->flags ^= B86_CF; break;
    case 0xFC: c->flags &= ~(uint32_t)B86_DF; break;
    default:   c->flags |= B86_DF; break;
    }
    return 0u;
}
/* POP DS/ES: checked selector load (CPL0, like PM26). 0 = refused. */
static B86_PM_HOT uint32_t b86_pj_loadseg(B86Cpu *c, uint32_t seg, uint32_t sel)
{
    B86PM2Plan plan;
    if (!p40_plan(c, seg, (uint16_t)sel, &plan)) return 0u;
    return b86_pm2_commit_segment(c, &plan, 0u) == B86_PM2_OK;
}
/* x86 condition cc from the interpreter state (materializes lazy flags) */
static B86_PM_HOT uint32_t b86_pj_cond(B86Cpu *c, uint32_t cc)
{
    b86_flags_materialize(c);
    return (uint32_t)p40_cond(c->flags, cc);
}

/* BB_STAGE5I_PORT_TRACE: report full 32-bit guest EIP for actual native
 * protected-mode PIT I/O, bounded output. Never changes port result/state. */
extern int printf(const char *restrict, ...); /* Stage5I V2: Pico TU declaration */
static unsigned long bb5i_pit_in_count, bb5i_pit_out_count;
static void bb5i_pit_trace(const B86Cpu *c, uint32_t port, unsigned out)
{
    unsigned long *const count = out ? &bb5i_pit_out_count : &bb5i_pit_in_count;
    if ((!out && port != 0x40u) || (out && port != 0x43u)) return;
    ++*count;
    if (*count <= 8u || ((*count & 65535u) == 0u))
        printf("[bb5i-pit] io=%s total=%lu eip=%08lX base=%08lX selector=%04X pg=%u\n",
               out ? "out43" : "in40", *count, (unsigned long)c->ip,
               (unsigned long)c->pm386.seg[B86_CS].base,
               (unsigned)c->seg[B86_CS], (unsigned)!!c->pg);
}
/* BB_STAGE5I_PORT_TRACE_END */
/* BB_PJ_FASTIO_V1: execute common control/I-O instructions inside native PJ.
 * Privilege refusal is side-effect-free: the exit-stay path delegates to the
 * existing protected-mode interpreter for its architectural exception.
 * OUT callbacks in blitzBUS inspect c->ip; the emitter stores guest IP and
 * live registers immediately before entering this helper. */
static B86_PM_HOT uint32_t b86_pj_fast_cli(B86Cpu *c)
{
    if ((c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) return 0u;
    c->flags &= ~(uint32_t)B86_IF;
    return 1u;
}
static B86_PM_HOT uint32_t b86_pj_fast_outimm8(B86Cpu *c, uint32_t port, uint32_t al)
{
    if (!c->out8 || (c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) return 0u;
    /* Interpreter normally has materialized arithmetic flags by this point.
     * This also makes the flag state visible if the device hook inspects it. */
    b86_flags_materialize(c);
    bb5i_pit_trace(c, port, 1u);
    c->out8(c, (uint16_t)port, (uint8_t)al);
    return 1u;
}

/* BB_STAGE4Z_BYTEIO: successful byte IN returns 0..255; refusal returns
 * 256, which cannot be confused with a valid zero-byte port value.
 * Same CPL<=IOPL and callback presence requirements as PM40's E4/EC.
 * I/O callbacks see the synchronized guest IP/registers and flags. */
static B86_PM_HOT uint32_t b86_pj_fast_in8(B86Cpu *c, uint32_t port)
{
    if (!c->in8 || (c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) return 256u;
    b86_flags_materialize(c);
    bb5i_pit_trace(c, port, 0u);
    return (uint32_t)c->in8(c, (uint16_t)port);
}

/* BB_PJ_LOWFRUIT_V1: hot helpers for additional 386 instructions.
 * Only forms with checked semantics are translated. Unrecognized or faulting
 * forms retain the original interpreter fallback and exception behavior. */
static B86_PM_HOT uint32_t b86_pj_rotate1(B86Cpu *c, uint32_t a, uint32_t desc)
{
    const unsigned w = desc & 7u, sub = (desc >> 8) & 7u;
    const unsigned bits = 8u * w;
    const uint32_t mask = w == 4u ? 0xFFFFFFFFu : (1u << bits) - 1u;
    const uint32_t msb = 1u << (bits - 1u);
    uint32_t r, cf, of;
    b86_flags_materialize(c);
    a &= mask;
    switch (sub) {
    case 0u: /* ROL: CF = new bit 0; OF = high bit XOR CF. */
        r = ((a << 1u) | (a >> (bits - 1u))) & mask;
        cf = r & 1u;
        of = ((r & msb) != 0u) ^ (cf != 0u);
        break;
    case 1u: /* ROR: CF = new high bit; OF = new high two bits XOR. */
        r = (a >> 1u) | ((a & 1u) ? msb : 0u);
        cf = (r & msb) != 0u;
        of = ((r & msb) != 0u) ^ ((r & (msb >> 1u)) != 0u);
        break;
    case 2u: /* RCL through the prior carry flag. */
        cf = (a & msb) != 0u;
        r = ((a << 1u) | !!(c->flags & B86_CF)) & mask;
        of = ((r & msb) != 0u) ^ (cf != 0u);
        break;
    default: /* RCR: the prior CF enters the top bit. */
        cf = a & 1u;
        r = (a >> 1u) | ((c->flags & B86_CF) ? msb : 0u);
        of = ((r & msb) != 0u) ^ ((r & (msb >> 1u)) != 0u);
        break;
    }
    c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF)) |
               (cf ? B86_CF : 0u) | (of ? B86_OF : 0u);
    return r;
}
/* BB_PJ_REPNE_EFFICIENCY_V1. 256 checked comparisons per native entry,
 * and a single page walk per contiguous source/destination page span.
 * Preserve the last CMP's full flags even when yielding or declining memory.
 * Crucially this does NOT conflate a budget yield with a memory refusal. */
#ifndef B86_PJ_REPNE_BUDGET
#define B86_PJ_REPNE_BUDGET 1024u /* Stage5B: bounded scan, still restartable */
#endif
static B86_PM_HOT uint32_t b86_pj_repne_cmp(B86Cpu *c, uint32_t desc)
{
    B86Pj *j = b86_pj_state(c);
    const unsigned op = desc & 255u, w = (desc >> 8) & 7u;
    const int cmps = op == 0xA6u || op == 0xA7u;
    const int backwards = (c->flags & B86_DF) != 0;
    const uint32_t step = backwards ? (uint32_t)0u - w : w;
    const uint32_t mask = w == 4u ? UINT32_MAX : (1u << (w * 8u)) - 1u;
    uint32_t final_a = 0u, final_b = 0u;
    unsigned performed = 0u;
    int equal = 0, refused = 0;
    if (j) ++j->repne_calls;
    b86_flags_materialize(c);
    while (c->r[B86_CX] && performed < B86_PJ_REPNE_BUDGET) {
        const uint32_t si = c->r[B86_SI], di = c->r[B86_DI];
        uint8_t *sp = NULL, *dp;
        const unsigned soff = si & 0xFFFu, doff = di & 0xFFFu;
        unsigned take = B86_PJ_REPNE_BUDGET - performed, i;
        unsigned span;
        if (take > c->r[B86_CX]) take = c->r[B86_CX];
        if (cmps) {
            sp = b86_pj_mem(c, si, w);
            if (!sp) { refused = 1; break; }
            span = backwards ? soff / w + 1u : (0x1000u - soff) / w;
            if (take > span) take = span;
        }
        dp = b86_pj_mem(c, di, w);
        if (!dp) { refused = 1; break; }
        span = backwards ? doff / w + 1u : (0x1000u - doff) / w;
        if (take > span) take = span;
        /* Both complete operands remain in these mapped physical pages for
         * the whole span. A crossing re-enters the checked mapper above. */
        if (!take) { refused = 1; break; }
        if (j) ++j->repne_spans;
        /* BB_STAGE5B_REPNE_SCASB: A mapped, read-only, forward SCASB span
         * is already validated not to cross a 4-KiB guest page. memchr
         * identifies exactly the first equality, preserving x86 REPNE
         * stop semantics. The final comparison alone defines EFLAGS.
         * Reverse, CMPS, 16/32-bit, and mapping-failure cases are unchanged. */
        if (w == 1u && !cmps && !backwards) {
            const uint8_t target = (uint8_t)c->r[B86_AX];
            const uint8_t *found = (const uint8_t *)memchr(dp, target, take);
            const unsigned count = found ? (unsigned)(found - dp) + 1u : take;
            final_a = (uint32_t)target;
            final_b = found ? (uint32_t)*found : (uint32_t)dp[take - 1u];
            c->r[B86_DI] += count;
            c->r[B86_CX] -= count;
            performed += count;
            if (found) { equal = 1; break; }
            continue;
        }
        for (i = 0u; i < take; ++i) {
            const size_t offset = (size_t)i * w;
            const uint8_t *pdi = backwards ? dp - offset : dp + offset;
            final_a = cmps ? p40_ld(backwards ? sp - offset : sp + offset, w)
                           : c->r[B86_AX] & mask;
            final_b = p40_ld(pdi, w);
            if (cmps) c->r[B86_SI] += step;
            c->r[B86_DI] += step;
            --c->r[B86_CX];
            ++performed;
            if (final_a == final_b) { equal = 1; break; }
        }
        if (equal) break;
    }
    /* No flags become externally visible between REPNE comparisons.  Only
     * the last executed comparison matters; one ALU update suffices. */
    if (performed) {
        (void)p40_alu(c, 7u, final_a, final_b, w);
        if (j) j->repne_comparisons += performed;
    }
    if (refused) { if (j) ++j->repne_refusals; return 0u; }
    if (equal || !c->r[B86_CX]) {
        if (j) ++j->repne_completed;
        return 1u;
    }
    /* A partial native instruction is restarted at its original guest EIP.
     * The C dispatcher may handle an interrupt before its next entry. */
    if (j) ++j->repne_budget_yields;
    return 0u;
}

/* BB_PJ_C1_E3_66_V1: bounded immediate-count C1 ROL/ROR/RCL/RCR.
 * Called only for a nonzero masked count.  Full arithmetic flags are
 * materialized before updating CF/OF; SF/ZF/PF/AF must remain unchanged.
 * The 386 masks the count to five bits; a 16-bit rotate may wrap through
 * 16 (ROL/ROR) or 17 (RCL/RCR) bits, which looping preserves exactly. */
static B86_PM_HOT uint32_t b86_pj_c1_rotate(B86Cpu *c, uint32_t value, uint32_t desc)
{
    const unsigned w = desc & 7u, sub = (desc >> 8) & 3u, count = (desc >> 16) & 31u;
    const unsigned bits = w * 8u;
    const uint32_t mask = w == 4u ? UINT32_MAX : (1u << bits) - 1u;
    const uint32_t msb = 1u << (bits - 1u);
    uint32_t v = value & mask, carry, overflow;
    unsigned i;
    b86_flags_materialize(c);
    carry = (c->flags & B86_CF) != 0u;
    for (i = 0u; i < count; ++i) {
        uint32_t oldcf = carry;
        if (sub == 0u) { /* ROL */
            carry = (v & msb) != 0u;
            v = ((v << 1u) & mask) | carry;
        } else if (sub == 1u) { /* ROR */
            carry = v & 1u;
            v = (v >> 1u) | (carry ? msb : 0u);
        } else if (sub == 2u) { /* RCL through CF */
            carry = (v & msb) != 0u;
            v = ((v << 1u) & mask) | oldcf;
        } else { /* RCR through CF */
            carry = v & 1u;
            v = (v >> 1u) | (oldcf ? msb : 0u);
        }
    }
    overflow = sub == 0u || sub == 2u
        ? (((v & msb) != 0u) ^ (carry != 0u))
        : (((v & msb) != 0u) ^ ((v & (msb >> 1u)) != 0u));
    c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF)) |
               (carry ? B86_CF : 0u) | (overflow ? B86_OF : 0u);
    if (count != 1u) c->pm40_undef |= B86_OF; /* OF architecturally undefined */
    return v;
}

/* BB_PJ_SHIFTD_GS_X87_V1: 386 SHLD/SHRD immediate, exactly PM27 flags.
 * A nonzero count is guaranteed by translation; for a 16-bit operand, count
 * greater than 16 remains on the checked C fallback. The host helper only
 * handles the result/flags. PJ still uses its checked memory trampoline. */
static B86_PM_HOT uint32_t b86_pj_shiftd_imm(B86Cpu *c, uint32_t dst,
                                              uint32_t src, uint32_t desc)
{
    const unsigned count = desc & 31u;
    const unsigned w = (desc >> 8) & 7u;
    const unsigned left = (desc >> 16) & 1u;
    const unsigned bits = w * 8u;
    const uint32_t mask = w == 2u ? 0xFFFFu : UINT32_MAX;
    const uint32_t sign = 1u << (bits - 1u);
    uint32_t result, cf, oldflags;
    /* 0F A5/AD with CL masked to zero retires without changing flags.
     * Only register operands use this dynamic-count native form; no memory
     * access or side effects occur when the count is zero. The old immediate
     * translation never calls this helper with a zero count. */
    if (!count) return dst;
    /* Materialize flags left by any preceding translated instruction before
     * changing the subset defined by this instruction. */
    b86_flags_materialize(c);
    dst &= mask; src &= mask;
    if (left) {
        result = count == bits ? src : ((dst << count) | (src >> (bits - count))) & mask;
        cf = (dst >> (bits - count)) & 1u;
    } else {
        result = count == bits ? src : ((dst >> count) | (src << (bits - count))) & mask;
        cf = (dst >> (count - 1u)) & 1u;
    }
    oldflags = c->flags;
    c->flags = (oldflags & ~(uint32_t)(B86_CF | B86_PF | B86_ZF | B86_SF)) |
               cf | b86_parity[result & 255u] |
               (result == 0u ? B86_ZF : 0u) | ((result & sign) ? B86_SF : 0u);
    if (count == 1u) {
        c->flags &= ~(uint32_t)B86_OF;
        if (left ? (((result & sign) != 0u) != (cf != 0u))
                 : (((dst & sign) != 0u) != ((result & sign) != 0u)))
            c->flags |= B86_OF;
    }
    return result;
}

/* ---- translator ----------------------------------------------------------- */
typedef struct { uint8_t *at; uint32_t ip, n; int dyn; } PjExit;   /* dyn: 0 static/chainable, 1 EIP in r1, 2 static, never chained */
typedef struct {
    B86Cpu *c;
    PjE e;
    const uint8_t *b; unsigned n, pos;           /* instruction bytes */
    uint32_t ip0;                                /* EIP of the instruction being translated */
    unsigned ninsn;
    PjExit ex[96]; unsigned nex;
    int last_kind;                               /* static last flag producer in block, -1 unknown */
    int ikind_clear;                             /* lz_ikind known to be LZ_NONE */
    int creg_ok;                                 /* r12 holds the B86Cpu pointer */
} PjT;

#define CREG R12                                  /* B86Cpu pointer, reloaded after calls */
/* Runtime guard lives after the segment classifier; translator sees this
 * declaration even though the helper is defined lower in the source. */
static uint32_t b86_pj_es_runtime_check(B86Cpu *c, uint32_t eip);
static void pjt_c(PjT *t) { if (!t->creg_ok) { pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u); t->creg_ok = 1; } }
static void pjt_call(PjT *t, const void *fn) { pj_call(&t->e, fn); t->creg_ok = 0; }
static void pjt_exit(PjT *t, uint32_t ip, unsigned n, int dyn)
{ if (t->nex < 96u) { PjExit *x = &t->ex[t->nex++]; x->at = pj_b(&t->e, -1); x->ip = ip; x->n = n; x->dyn = dyn; } else t->e.bad = 1; }
static void pjt_exit_cond(PjT *t, int cond, uint32_t ip, unsigned n)
{ if (t->nex < 96u) { PjExit *x = &t->ex[t->nex++]; x->at = pj_b(&t->e, cond); x->ip = ip; x->n = n; x->dyn = 0; } else t->e.bad = 1; }
/* exit that re-runs the same instruction in the interpreter: never chained */
static void pjt_exit_stay(PjT *t, int cond, uint32_t ip, unsigned n)
{ if (t->nex < 96u) { PjExit *x = &t->ex[t->nex++]; x->at = pj_b(&t->e, cond); x->ip = ip; x->n = n; x->dyn = 2; } else t->e.bad = 1; }

/* lazy flag record stores (values in registers va/vres) */
static void pjt_lazy(PjT *t, uint32_t kind, int va, int vres)
{
    const int inc = kind == LZ_INC32 || kind == LZ_DEC32 || kind == LZ_INC16 || kind == LZ_DEC16 ||
                    kind == LZ_INC8 || kind == LZ_DEC8;              /* r70b: 16-bit forms were missing */
    pjt_c(t);
    pj_movw(&t->e, LR, kind);
    if (inc) {
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
        pj_ldst(&t->e, PJ_STR, va, CREG, (unsigned)offsetof(B86Cpu, lz_ia));
        pj_ldst(&t->e, PJ_STR, vres, CREG, (unsigned)offsetof(B86Cpu, lz_ires));
        t->ikind_clear = 0;
    } else {
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
        if (va >= 0) pj_ldst(&t->e, PJ_STR, va, CREG, (unsigned)offsetof(B86Cpu, lz_a));
        pj_ldst(&t->e, PJ_STR, vres, CREG, (unsigned)offsetof(B86Cpu, lz_res));
        if (!t->ikind_clear) {
            pj_movw(&t->e, LR, LZ_NONE);
            pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
            t->ikind_clear = 1;
        }
    }
    t->last_kind = (int)kind;
}

/* ModRM decode (32-bit addressing, no prefixes). Address into R1. */
typedef struct { unsigned mod, reg, rm; } PjM;
static int pjt_u8(PjT *t, uint32_t *v) { if (t->pos >= t->n) return 0; *v = t->b[t->pos++]; return 1; }
static int pjt_u32(PjT *t, uint32_t *v)
{ if (t->pos + 4u > t->n) return 0; *v = (uint32_t)t->b[t->pos] | ((uint32_t)t->b[t->pos+1] << 8) | ((uint32_t)t->b[t->pos+2] << 16) | ((uint32_t)t->b[t->pos+3] << 24); t->pos += 4u; return 1; }
static void pjt_addimm(PjT *t, int d, int n, uint32_t v)
{
    if (!v) { if (d != n) pj_mov(&t->e, d, n); return; }
    if (pj_dpi(&t->e, 8u, 0, d, n, v)) return;
    if (pj_dpi(&t->e, 13u, 0, d, n, 0u - v)) return;
    pj_imm32(&t->e, R0, v); pj_dpr(&t->e, 8u, 0, d, n, R0, 0u, 0u);
}
static int pjt_modrm(PjT *t, PjM *m)
{
    uint32_t b, sib, disp = 0u;
    if (!pjt_u8(t, &b)) return 0;
    m->mod = b >> 6; m->reg = (b >> 3) & 7u; m->rm = b & 7u;
    if (m->mod == 3u) return 1;
    if (m->rm == 4u) {
        unsigned base, idx, sc;
        int have = 0;
        if (!pjt_u8(t, &sib)) return 0;
        sc = sib >> 6; idx = (sib >> 3) & 7u; base = sib & 7u;
        if (base == 5u && m->mod == 0u) { if (!pjt_u32(t, &disp)) return 0; }
        else { pj_mov(&t->e, R1, GR(base)); have = 1; }
        if (idx != 4u) {
            if (have) pj_dpr(&t->e, 8u, 0, R1, R1, GR(idx), 0u, sc);
            else { pj_shift(&t->e, R1, GR(idx), 0u, sc); have = 1; }
        }
        if (!have) pj_movw(&t->e, R1, 0u);
    } else if (m->rm == 5u && m->mod == 0u) {
        if (!pjt_u32(t, &disp)) return 0;
        pj_movw(&t->e, R1, 0u);
    } else pj_mov(&t->e, R1, GR(m->rm));
    if (m->mod == 1u) { if (!pjt_u8(t, &disp)) return 0; disp = (uint32_t)(int32_t)(int8_t)disp; }
    else if (m->mod == 2u) { if (!pjt_u32(t, &disp)) return 0; }
    pjt_addimm(t, R1, R1, disp);
    return 1;
}
/* host pointer for linear R1 -> R0 through the shared trampoline:
 *   MOVW r2,size|write<<8; BL memtramp; .word eip, retired
 * A refusal leaves the block at this instruction (it has not run). */
static void pjt_mem(PjT *t, unsigned size, int write)
{
    const B86Pj *j = (const B86Pj *)t->c->pj;
    pj_movw(&t->e, R2, size | (write ? 0x100u : 0u));
    pjt_call(t, (const void *)j->memtramp);
    pje16(&t->e, t->ip0 & 0xFFFFu); pje16(&t->e, t->ip0 >> 16);
    pje16(&t->e, (t->ninsn - 1u) & 0xFFFFu); pje16(&t->e, (t->ninsn - 1u) >> 16);
}
/* 8-bit register helpers: AL CL DL BL AH CH DH BH */
static void pjt_get8(PjT *t, int d, unsigned r) { pj_ubfx(&t->e, d, GR(r & 3u), (r & 4u) ? 8u : 0u, 8u); }
static void pjt_put8(PjT *t, unsigned r, int s) { pj_bfi(&t->e, GR(r & 3u), s, (r & 4u) ? 8u : 0u, 8u); }

/* ---- width-generic helpers (w = 1, 2, 4) ---- */
static const uint32_t pj_k_add[5] = { 0, LZ_ADD8, LZ_ADD16, 0, LZ_ADD32 };
static const uint32_t pj_k_sub[5] = { 0, LZ_SUB8, LZ_SUB16, 0, LZ_SUB32 };
static const uint32_t pj_k_log[5] = { 0, LZ_LOG8, LZ_LOG16, 0, LZ_LOG32 };
static const uint32_t pj_k_inc[5] = { 0, LZ_INC8, LZ_INC16, 0, LZ_INC32 };
static const uint32_t pj_k_dec[5] = { 0, LZ_DEC8, LZ_DEC16, 0, LZ_DEC32 };
static const uint32_t pj_k_szpc[5] = { 0, LZ_SZPC8, LZ_SZPC16, 0, LZ_SZPC32 };
static int pj_kw(int k)                          /* operand width of a lazy kind */
{
    if (k == LZ_ADD8 || k == LZ_SUB8 || k == LZ_LOG8 || k == LZ_INC8 || k == LZ_DEC8 || k == LZ_SZPC8) return 1;
    if (k == LZ_ADD16 || k == LZ_SUB16 || k == LZ_LOG16 || k == LZ_INC16 || k == LZ_DEC16 || k == LZ_SZPC16) return 2;
    return 4;
}
static int pj_kis(int k, const uint32_t *tab) { return k == (int)tab[1] || k == (int)tab[2] || k == (int)tab[4]; }
static void pjt_getr(PjT *t, int d, unsigned r, unsigned w)
{ if (w == 4u) pj_mov(&t->e, d, GR(r)); else if (w == 2u) pj_ext(&t->e, 0xFA1Fu, d, GR(r)); else pjt_get8(t, d, r); }
static void pjt_putr(PjT *t, unsigned r, unsigned w, int s)
{ if (w == 4u) pj_mov(&t->e, GR(r), s); else if (w == 2u) pj_bfi(&t->e, GR(r), s, 0u, 16u); else pjt_put8(t, r, s); }
static void pjt_ld(PjT *t, int d, int base, unsigned w) { pj_ldst(&t->e, w == 4u ? PJ_LDR : w == 2u ? PJ_LDRH : PJ_LDRB, d, base, 0u); }
static void pjt_st(PjT *t, int sreg, int base, unsigned w) { pj_ldst(&t->e, w == 4u ? PJ_STR : w == 2u ? PJ_STRH : PJ_STRB, sreg, base, 0u); }

/* ALU on values in a and b (garbage above w allowed); full result in d.
 * op: 0 ADD 1 OR 4 AND 5 SUB 6 XOR 7 CMP (ADC/SBB are left to the interpreter). */
static int pjt_alu(PjT *t, unsigned op, unsigned w, int d, int a, int b)
{
    switch (op) {
    case 0: pj_mov(&t->e, R2, a); pj_dpr(&t->e, 8u, 0, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d); pjt_lazy(t, pj_k_add[w], R2, R3); return 1;
    case 5: case 7: {
        const int dd = op == 7u ? R3 : d;
        pj_mov(&t->e, R2, a); pj_dpr(&t->e, 13u, 0, dd, a, b, 0u, 0u);
        if (dd != R3) pj_mov(&t->e, R3, dd);
        pjt_lazy(t, pj_k_sub[w], R2, R3); return 1; }
    case 1: pj_dpr(&t->e, 2u, 0, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d); pjt_lazy(t, pj_k_log[w], -1, R3); return 1;
    case 4: pj_dpr(&t->e, 0u, 0, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d); pjt_lazy(t, pj_k_log[w], -1, R3); return 1;
    case 6: pj_dpr(&t->e, 4u, 0, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d); pjt_lazy(t, pj_k_log[w], -1, R3); return 1;
    case 2: case 3: {                         /* ADC / SBB: CF saved at [sp+16] by pjt_cf */
        static const uint32_t kadc[5] = { 0, LZ_ADC8, LZ_ADC16, 0, LZ_ADC32 }, ksbb[5] = { 0, LZ_SBB8, LZ_SBB16, 0, LZ_SBB32 };
        pj_mov(&t->e, R2, a);
        pj_ldst(&t->e, PJ_LDR, LR, SP, 16u);
        pj_dpr(&t->e, op == 2u ? 8u : 13u, 0, d, a, b, 0u, 0u);
        pj_dpr(&t->e, op == 2u ? 8u : 13u, 0, d, d, LR, 0u, 0u);
        pj_mov(&t->e, R3, d);
        pjt_c(t);
        pj_movw(&t->e, LR, op == 2u ? kadc[w] : ksbb[w]);
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
        pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_a));
        pj_ldst(&t->e, PJ_STR, b, CREG, (unsigned)offsetof(B86Cpu, lz_b));
        pj_ldst(&t->e, PJ_STR, R3, CREG, (unsigned)offsetof(B86Cpu, lz_res));
        pj_movw(&t->e, LR, LZ_NONE);
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
        t->ikind_clear = 1;
        t->last_kind = (int)(op == 2u ? kadc[w] : ksbb[w]);
        return 1; }
    default: return 0;
    }
}
/* CF (0/1) of the current flags into [sp+16], before any operand is loaded */
static void pjt_cf(PjT *t)
{
    pjt_c(t);
    pj_mov(&t->e, R0, CREG);
    pj_movw(&t->e, R1, 2u);                    /* cc B: CF == 1 */
    pjt_call(t, (const void *)b86_pj_cond);
    pj_ldst(&t->e, PJ_STR, R0, SP, 16u);
}
/* x86 cc -> ARM cond after a CMP-equivalent of a SUB/CMP producer */
static const int8_t pj_armcc[16] = { 6, 7, 3, 2, 0, 1, 9, 8, 4, 5, -1, -1, 11, 10, 13, 12 };

/* Set ARM flags for x86 condition cc from the block's last producer, or call
 * pj_cond. Returns the ARM condition that is TRUE when the x86 cc holds. */
static int pjt_flags_for(PjT *t, unsigned cc)
{
    const int k = t->last_kind;
    int arm = pj_armcc[cc];
    int helper = arm < 0 || k < 0;
    const int cfcc = (cc >> 1) == 1u || (cc >> 1) == 3u;      /* B/AE/BE/A use CF */
    const int incdec = k >= 0 && (pj_kis(k, pj_k_inc) || pj_kis(k, pj_k_dec));
    if (incdec && cfcc) helper = 1;
    if (k >= 0 && pj_kis(k, pj_k_add) && cfcc) helper = 1;
    if (k >= 0 && !pj_kis(k, pj_k_add) && !pj_kis(k, pj_k_sub) && !pj_kis(k, pj_k_log) && !incdec) helper = 1;   /* SZPC, ADC/SBB, ... */
    if (!helper) {
        const unsigned w = (unsigned)pj_kw(k), sh = 32u - 8u * w;
        const unsigned oa = incdec ? (unsigned)offsetof(B86Cpu, lz_ia) : (unsigned)offsetof(B86Cpu, lz_a);
        const unsigned orr = incdec ? (unsigned)offsetof(B86Cpu, lz_ires) : (unsigned)offsetof(B86Cpu, lz_res);
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDR, R1, CREG, oa);
        pj_ldst(&t->e, PJ_LDR, R2, CREG, orr);
        if (pj_kis(k, pj_k_sub)) {
            pj_dpr(&t->e, 13u, 0, R3, R1, R2, 0u, 0u);              /* b = a - res */
            if (sh) { pj_shift(&t->e, R1, R1, 0u, sh); pj_shift(&t->e, R3, R3, 0u, sh); }
            pj_dpr(&t->e, 13u, 1, PC, R1, R3, 0u, 0u);              /* CMP a, b */
        } else if (pj_kis(k, pj_k_add)) {
            pj_dpr(&t->e, 13u, 0, R3, R2, R1, 0u, 0u);              /* b = res - a */
            if (sh) { pj_shift(&t->e, R1, R1, 0u, sh); pj_shift(&t->e, R3, R3, 0u, sh); }
            pj_dpr(&t->e, 8u, 1, PC, R1, R3, 0u, 0u);               /* CMN a, b */
        } else if (pj_kis(k, pj_k_log)) {
            if (sh) pj_shift(&t->e, R2, R2, 0u, sh);
            pj_dpi(&t->e, 13u, 1, PC, R2, 0u);                      /* CMP res, #0: C=1 V=0 */
        } else {                                                    /* INC/DEC, cc without CF */
            if (sh) pj_shift(&t->e, R1, R1, 0u, sh);
            pj_imm32(&t->e, R3, 1u << sh);
            pj_dpr(&t->e, pj_kis(k, pj_k_inc) ? 8u : 13u, 1, PC, R1, R3, 0u, 0u);
        }
        return arm;
    }
    pjt_c(t);
    pj_mov(&t->e, R0, CREG);
    pj_movw(&t->e, R1, cc);
    pjt_call(t, (const void *)b86_pj_cond);
    pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
    return 1;                                                       /* NE */
}
static void pjt_jcc(PjT *t, unsigned cc, uint32_t target, uint32_t next)
{
    const int arm = pjt_flags_for(t, cc);
    pjt_exit_cond(t, arm, target, t->ninsn);
    pjt_exit(t, next, t->ninsn, 0);
}
/* local forward branch helpers */
static uint8_t *pjt_bfwd(PjT *t, int cond) { return pj_b(&t->e, cond); }
static void pjt_here(PjT *t, uint8_t *at) { pj_patch_b(at, t->e.p); }

/* r/m operand: register or memory (pointer saved at [sp+4]) */
typedef struct { int mem; unsigned rm; } PjOp;
static int pjt_rm_load(PjT *t, const PjM *m, unsigned w, int write, int d, PjOp *o)
{
    o->mem = m->mod != 3u; o->rm = m->rm;
    if (!o->mem) { pjt_getr(t, d, m->rm, w); return 1; }
    pjt_mem(t, w, write);
    if (write) pj_ldst(&t->e, PJ_STR, R0, SP, 4u);
    pjt_ld(t, d, R0, w);
    return 1;
}
static void pjt_rm_store(PjT *t, const PjOp *o, unsigned w, int sreg)
{
    if (!o->mem) { pjt_putr(t, o->rm, w, sreg); return; }
    pj_ldst(&t->e, PJ_LDR, R0, SP, 4u);
    pjt_st(t, sreg, R0, w);
}
/* shifts SHL/SHR/SAR by an immediate count n (1..31) on value in R1 (w bits). */
static int pjt_shift(PjT *t, unsigned kind, unsigned w, unsigned n)
{
    const unsigned bits = 8u * w;
    if (n == 0u || n >= bits) return 0;
    if (kind == 4u) {                               /* SHL: CF = bit (bits-n) of a */
        pj_ubfx(&t->e, R2, R1, bits - n, 1u);
        pj_shift(&t->e, R1, R1, 0u, n);
        pj_ubfx(&t->e, R3, R1, bits - 1u, 1u);      /* msb(res) */
        pj_dpr(&t->e, 4u, 0, R3, R3, R2, 0u, 0u);   /* OF = msb ^ CF */
    } else if (kind == 5u) {                        /* SHR */
        if (w < 4u) pj_ubfx(&t->e, R1, R1, 0u, bits);
        pj_ubfx(&t->e, R2, R1, n - 1u, 1u);
        pj_ubfx(&t->e, R3, R1, bits - 1u, 1u);      /* OF = msb(a) */
        pj_shift(&t->e, R1, R1, 1u, n);
    } else if (kind == 7u) {                        /* SAR */
        if (w == 1u) pj_ext(&t->e, 0xFA4Fu, R1, R1); else if (w == 2u) pj_ext(&t->e, 0xFA0Fu, R1, R1);
        pj_ubfx(&t->e, R2, R1, n - 1u, 1u);
        pj_movw(&t->e, R3, 0u);
        pj_shift(&t->e, R1, R1, 2u, n);
    } else return 0;
    pj_dpr(&t->e, 2u, 0, R2, R2, R3, 0u, 11u);      /* lz_b = CF | OF<<11 */
    pjt_c(t);
    pj_movw(&t->e, LR, pj_k_szpc[w]);
    pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
    pj_ldst(&t->e, PJ_STR, R1, CREG, (unsigned)offsetof(B86Cpu, lz_res));
    pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_b));
    if (!t->ikind_clear) { pj_movw(&t->e, LR, LZ_NONE); pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind)); t->ikind_clear = 1; }
    t->last_kind = (int)pj_k_szpc[w];
    return 1;
}

static int pjt_imm(PjT *t, unsigned w, uint32_t *v)
{
    if (w == 4u) return pjt_u32(t, v);
    if (w == 1u) return pjt_u8(t, v);
    if (t->pos + 2u > t->n) return 0;
    *v = (uint32_t)t->b[t->pos] | ((uint32_t)t->b[t->pos + 1u] << 8); t->pos += 2u; return 1;
}

/* Translate one instruction at t->b[t->pos]. Returns 1 = continue block,
 * 2 = block ended (exits emitted), 0 = not supported (block ends before it). */
static B86_PJ_COLD int pjt_insn(PjT *t)
{
    PjM m;
    PjOp o;
    uint32_t op, imm, op2;
    unsigned o16 = 0u, w, np = 0u, repz = 0u, repne = 0u, segpref = 0u, es_override = 0u;
    const unsigned start = t->pos, nex0 = t->nex;
    uint8_t *const e0 = t->e.p;
    const int lk0 = t->last_kind, ik0 = t->ikind_clear, cr0 = t->creg_ok;
    t->ninsn++;                                    /* counted as retired once its exit is taken */
    /* a refused instruction leaves no code, exits or flag knowledge behind */
#define PJ_FAIL() do { t->ninsn--; t->pos = start; t->e.p = e0; t->nex = nex0; t->last_kind = lk0; t->ikind_clear = ik0; t->creg_ok = cr0; return 0; } while (0)
#define PJ_NEXT() (t->ip0 + (t->pos - start))
    for (;;) {
        if (!pjt_u8(t, &op) || ++np > 4u) PJ_FAIL();
        if (op == 0x66u) { o16 = 1u; continue; }
        if (op == 0x3Eu || op == 0x36u || op == 0x2Eu || op == 0x26u) {
            /* 26h uses ES. Its validity is checked in generated Thumb-2
             * when the instruction executes, not when it is translated. */
            if (op == 0x26u) es_override = 1u;
            segpref = 1u; continue;
        }
        if (op == 0xF3u) { repz = 1u; continue; }
        if (op == 0xF2u) { repz = 1u; repne = 1u; continue; }
        break;
    }
    w = o16 ? 2u : 4u;
    /* BB_PJ_ES_MOV8_V1: exactly one 26h prefix and MOV r8,ES:[r/m].
     * Offset R1 is already calculated by the standard 32-bit ModRM decoder.
     * A checked C helper returns the mapped byte pointer; on failure, exit
     * BEFORE updating the destination and retry through the interpreter.
     * No ES-mode-dependent code or cache invalidation is required. */
    if (es_override && op == 0x8Au && np == 2u && !repz) {
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        if (m.mod == 3u) { /* the override is ignored for register sources */
            pjt_get8(t, R1, m.rm);
            pjt_put8(t, m.reg, R1);
            return 1;
        }
        pjt_c(t);
        pj_mov(&t->e, R0, CREG); /* R1 still holds the effective offset */
        pjt_call(t, (const void *)b86_pj_es_mov8_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u); /* CMP pointer, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pj_ldst(&t->e, PJ_LDRB, R1, R0, 0u);
        pjt_put8(t, m.reg, R1);  /* AL/CL/DL/BL and AH/CH/DH/BH */
        return 1;
    }
    /* Same cached Thumb-2 is correct in both ES modes. The guest ES
     * descriptor is rechecked BEFORE any ES-dependent side effects.
     * 8E segment loads use their existing checked helper and force a C
     * boundary; linked blocks cannot silently change DS/SS semantics. */
    if (es_override || op == 0xA4u || op == 0xA5u ||
        op == 0xAAu || op == 0xABu || op == 0xA6u || op == 0xA7u ||
        op == 0xAEu || op == 0xAFu) {
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_imm32(&t->e, R1, t->ip0); /* exact denied guest instruction */
        pjt_call(t, (const void *)b86_pj_es_runtime_check);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u); /* CHECK result, preserve x86 flags */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
    }
    if (repz && op != 0xA4u && op != 0xA5u && op != 0xAAu && op != 0xABu &&
        !(repne && (op == 0xA6u || op == 0xA7u || op == 0xAEu || op == 0xAFu))) PJ_FAIL();
    /* In contrast to MOVS/STOS, REPNE CMPS/SCAS uses the default DS and ES
     * segments. Segment overrides must remain on the interpreter path. */
    if (repne && segpref) PJ_FAIL();
    /* ALU 00-3D (ADC/SBB fetch CF first) */
    if (op < 0x40u && (op & 7u) < 6u) {
        const unsigned alu = op >> 3, form = op & 7u, ww = (form & 1u) ? w : 1u;
        if (alu == 2u || alu == 3u) pjt_cf(t);
        if (form < 4u) {
            if (!pjt_modrm(t, &m)) PJ_FAIL();
            if (form < 2u) {                          /* r/m op= reg */
                pjt_rm_load(t, &m, ww, alu != 7u, R1, &o);
                pjt_getr(t, R0, m.reg, ww);
                pjt_alu(t, alu, ww, alu == 7u ? R3 : R1, R1, R0);
                if (alu != 7u) pjt_rm_store(t, &o, ww, R1);
            } else {                                  /* reg op= r/m */
                pjt_rm_load(t, &m, ww, 0, R1, &o);
                pjt_getr(t, R0, m.reg, ww);
                pjt_alu(t, alu, ww, alu == 7u ? R3 : R0, R0, R1);
                if (alu != 7u) pjt_putr(t, m.reg, ww, R0);
            }
        } else {
            if (!pjt_imm(t, ww, &imm)) PJ_FAIL();
            pjt_getr(t, R0, 0u, ww);
            pj_imm32(&t->e, R1, imm);
            pjt_alu(t, alu, ww, alu == 7u ? R3 : R0, R0, R1);
            if (alu != 7u) pjt_putr(t, 0u, ww, R0);
        }
        return 1;
    }
    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        pjt_getr(t, R2, op & 7u, w);
        if (op < 0x48u) pj_dpi(&t->e, 8u, 0, R3, R2, 1u); else pj_dpi(&t->e, 13u, 0, R3, R2, 1u);
        pjt_putr(t, op & 7u, w, R3);
        pjt_lazy(t, op < 0x48u ? pj_k_inc[w] : pj_k_dec[w], R2, R3);
        return 1;
    /* BB_PJ_C1_E3_66_V1: 66 PUSH/POP/PUSH imm16 with 32-bit SS.B stack.
     * Check the entire 16-bit guest access before updating ESP or registers.
     * PUSH SP uses old SP; POP SP applies the popped value after ESP += 2. */
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        pj_dpi(&t->e, 13u, 0, R1, GR(4), w);
        pjt_mem(t, w, 1);
        if (w == 4u) pj_ldst(&t->e, PJ_STR, GR(op & 7u), R0, 0u);
        else { pjt_getr(t, R1, op & 7u, 2u); pjt_st(t, R1, R0, 2u); }
        pj_dpi(&t->e, 13u, 0, GR(4), GR(4), w);
        return 1;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        pj_mov(&t->e, R1, GR(4));
        pjt_mem(t, w, 0);
        pj_dpi(&t->e, 8u, 0, GR(4), GR(4), w);
        if (w == 4u) pj_ldst(&t->e, PJ_LDR, GR(op & 7u), R0, 0u);
        else { pjt_ld(t, R1, R0, 2u); pjt_putr(t, op & 7u, 2u, R1); }
        return 1;
    case 0x68: case 0x6A:
        if (op == 0x68u) { if (!pjt_imm(t, w, &imm)) PJ_FAIL(); }
        else { if (!pjt_u8(t, &imm)) PJ_FAIL(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        pj_dpi(&t->e, 13u, 0, R1, GR(4), w);
        pjt_mem(t, w, 1);
        pj_imm32(&t->e, R1, imm);
        pjt_st(t, R1, R0, w);
        pj_dpi(&t->e, 13u, 0, GR(4), GR(4), w);
        return 1;
    case 0x69: case 0x6B: {                          /* IMUL r, r/m, imm */
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        pjt_rm_load(t, &m, w, 0, R1, &o);
        if (op == 0x6Bu) { if (!pjt_u8(t, &imm)) PJ_FAIL(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        else if (!pjt_imm(t, w, &imm)) PJ_FAIL();
        if (o16) PJ_FAIL();
        pj_imm32(&t->e, R0, imm);
        goto imul_rr;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        if (o16 || !pjt_u8(t, &imm)) PJ_FAIL();
        pjt_jcc(t, op & 15u, PJ_NEXT() + (uint32_t)(int32_t)(int8_t)imm, PJ_NEXT());
        return 2;
    case 0x80: case 0x81: case 0x83: {
        const unsigned ww = op == 0x80u ? 1u : w;
        if (t->pos < t->n && ((t->b[t->pos] >> 3) & 7u) - 2u < 2u) pjt_cf(t);   /* ADC/SBB: CF before the address is formed */
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        pjt_rm_load(t, &m, ww, m.reg != 7u, R1, &o);
        if (op == 0x83u) { if (!pjt_u8(t, &imm)) PJ_FAIL(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        else if (!pjt_imm(t, ww, &imm)) PJ_FAIL();
        pj_imm32(&t->e, R0, imm);
        pjt_alu(t, m.reg, ww, m.reg == 7u ? R3 : R1, R1, R0);
        if (m.reg != 7u) pjt_rm_store(t, &o, ww, R1);
        return 1; }
    case 0x84: case 0x85: {
        const unsigned ww = op == 0x84u ? 1u : w;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        pjt_rm_load(t, &m, ww, 0, R1, &o);
        pjt_getr(t, R0, m.reg, ww);
        pjt_alu(t, 4u, ww, R3, R1, R0);
        return 1; }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        const unsigned ww = (op & 1u) ? w : 1u;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        if (op <= 0x89u) {
            if (m.mod == 3u) { pjt_getr(t, R1, m.reg, ww); pjt_putr(t, m.rm, ww, R1); return 1; }
            pjt_mem(t, ww, 1);
            if (ww == 4u) pj_ldst(&t->e, PJ_STR, GR(m.reg), R0, 0u);
            else { pjt_getr(t, R1, m.reg, ww); pjt_st(t, R1, R0, ww); }
        } else {
            if (m.mod == 3u) { pjt_getr(t, R1, m.rm, ww); pjt_putr(t, m.reg, ww, R1); return 1; }
            pjt_mem(t, ww, 0);
            if (ww == 4u) pj_ldst(&t->e, PJ_LDR, GR(m.reg), R0, 0u);
            else { pjt_ld(t, R1, R0, ww); pjt_putr(t, m.reg, ww, R1); }
        }
        return 1; }
    case 0x8D:
        if (!pjt_modrm(t, &m) || m.mod == 3u) PJ_FAIL();
        pjt_putr(t, m.reg, w, R1);
        return 1;
    case 0x90: return 1;
    /* FWAIT/WAIT: absent FPU is a no-op except CR0.MP & CR0.TS, where the
     * existing interpreter must raise #NM through the protected IDT. */
    case 0x9B:
        if (np != 1u) PJ_FAIL();
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDRH, R0, CREG, (unsigned)offsetof(B86Cpu, msw));
        pj_dpi(&t->e, 0u, 0, R0, R0, 0xAu); /* AND r0, r0, #MP|TS */
        pj_dpi(&t->e, 13u, 1, PC, R0, 0xAu); /* CMP masked bits with 0xA */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        return 1;
    /* BB_PJ_FASTIO_V1: CLI changes IF, but must preserve pending lazy ALU
     * flags; an IOPL violation is retried through the interpreter. */
    case 0xFA:
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pjt_call(t, (const void *)b86_pj_fast_cli);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);      /* CMP result, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        return 1;
    /* OUT imm8,AL: synchronize guest register file and current EIP before
     * host I/O callback. Reload the registers in case the hook modified them.
     * The callback is executed exactly once; a refused port leaves no effects. */
    case 0xE6:
        if (!pjt_u8(t, &imm)) PJ_FAIL();
        pjt_c(t);
        pje32(&t->e, 0xE880u | CREG, 0x0FF0u); /* STMIA r12,{r4-r11} */
        pj_imm32(&t->e, R3, t->ip0);
        pj_ldst(&t->e, PJ_STR, R3, CREG, (unsigned)offsetof(B86Cpu, ip));
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, imm);                /* 8-bit port */
        pj_mov(&t->e, R2, GR(0));              /* EAX, helper uses AL */
        pjt_call(t, (const void *)b86_pj_fast_outimm8);
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pje32(&t->e, 0xE890u | CREG, 0x0FF0u); /* LDMIA r12,{r4-r11} */
        t->creg_ok = 1;
        /* Helper materialized the previously lazy arithmetic flags. */
        t->last_kind = -1;
        t->ikind_clear = 1;
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);     /* CMP result, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        return 1;
    /* BB_STAGE4Z_BYTEIO: byte IN AL,imm8 / IN AL,DX / OUT DX,AL.
     * All other port widths retain the checked PM interpreter. No prefix
     * variants are translated. The existing E6 OUT imm8,AL stays intact.
     * In particular, do not emulate I/O privilege bitmap semantics here:
     * CPL>IOPL refuses and falls back before calling any device hook. */
    case 0xE4: case 0xEC: case 0xEE: {
        const unsigned input = op != 0xEEu;
        if (np != 1u) PJ_FAIL();
        if (op == 0xE4u && !pjt_u8(t, &imm)) PJ_FAIL();
        pjt_c(t);
        /* Expose the live guest state to the existing device callback. */
        pje32(&t->e, 0xE880u | CREG, 0x0FF0u); /* STMIA r12,{r4-r11} */
        pj_imm32(&t->e, R3, t->ip0);
        pj_ldst(&t->e, PJ_STR, R3, CREG, (unsigned)offsetof(B86Cpu, ip));
        pj_mov(&t->e, R0, CREG);
        if (op == 0xE4u) pj_movw(&t->e, R1, imm);
        else pj_mov(&t->e, R1, GR(B86_DX)); /* helper masks DX to 16 bits */
        if (!input) pj_mov(&t->e, R2, GR(0)); /* current EAX supplies AL */
        pjt_call(t, input ? (const void *)b86_pj_fast_in8
                          : (const void *)b86_pj_fast_outimm8);
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pje32(&t->e, 0xE890u | CREG, 0x0FF0u); /* callback may update registers */
        t->creg_ok = 1;
        t->last_kind = -1;
        t->ikind_clear = 1;  /* helper materializes pending flags on success */
        if (input) {
            /* Only refused calls return 256; valid input is always 0..255. */
            pj_dpi(&t->e, 13u, 1, PC, R0, 256u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            pjt_put8(t, 0u, R0); /* update AL without disturbing upper EAX */
        } else {
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        }
        return 1; }
    case 0xF5: case 0xF8: case 0xF9: case 0xFC: case 0xFD:
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, op);
        pjt_call(t, (const void *)b86_pj_flagop);
        t->last_kind = -1; t->ikind_clear = 1;      /* flags now materialized, lazy records cleared */
        return 1;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:   /* XCHG eAX, r */
        pjt_getr(t, R1, 0u, w); pjt_getr(t, R2, op & 7u, w);
        pjt_putr(t, 0u, w, R2); pjt_putr(t, op & 7u, w, R1);
        return 1;
    case 0x86: case 0x87: {                          /* XCHG r/m, r */
        const unsigned ww = op == 0x86u ? 1u : w;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        pjt_rm_load(t, &m, ww, 1, R1, &o);
        pjt_getr(t, R2, m.reg, ww);
        pjt_putr(t, m.reg, ww, R1);
        pjt_rm_store(t, &o, ww, R2);
        return 1; }
    case 0x8C:                                       /* MOV r/m, Sreg (zero-extended into r32) */
        if (!pjt_modrm(t, &m) || m.reg > 5u) PJ_FAIL();
        pjt_c(t);                                    /* selector into r2 (r1 holds the address) */
        if (m.reg <= 3u) { pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, seg) + 4u * m.reg); pj_ext(&t->e, 0xFA1Fu, R2, R2); }
        else pj_ldst(&t->e, PJ_LDRH, R2, CREG, (unsigned)offsetof(B86Cpu, seg386) + 2u * (m.reg - 4u));
        if (m.mod == 3u) { pjt_putr(t, m.rm, w, R2); return 1; }
        pj_ldst(&t->e, PJ_STR, R2, SP, 4u);
        pjt_mem(t, 2u, 1);
        pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
        pjt_st(t, R1, R0, 2u);
        return 1;
    /* MOV ES/DS, r16: reuse POP-DS/ES's validated segment-load helper.
     * Segment base/limit may change, so end native block immediately and
     * re-check flatness in C. Non-register, SS/FS/GS and prefixed forms
     * stay on the interpreter path until separately validated. */
    case 0x8E:
        if (np != 1u || !pjt_modrm(t, &m) || m.mod != 3u ||
            (m.reg != B86_DS && m.reg != B86_ES)) PJ_FAIL();
        pjt_getr(t, R2, m.rm, 2u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, m.reg);
        pjt_call(t, (const void *)b86_pj_loadseg);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pjt_exit(t, PJ_NEXT(), t->ninsn, 2); /* re-check flatness */
        return 2;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: {   /* string ops through a helper */
        const unsigned ww = (op & 1u) ? w : 1u;
        if (repz && op >= 0xACu) PJ_FAIL();
        pjt_c(t);
        pje32(&t->e, 0xE880u | CREG, 0x0FF0u);      /* STMIA r12, {r4-r11} */
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, op | (repz << 8) | (ww << 9));
        pjt_call(t, (const void *)b86_pj_str);
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pje32(&t->e, 0xE890u | CREG, 0x0FF0u);      /* LDMIA r12, {r4-r11} */
        t->creg_ok = 1;
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        return 1; }
    /* F2 CMPSB/W/D and SCASB/W/D: comparison loop with exact register
     * progress and ZF termination; memory errors fall back without replaying
     * already completed comparisons. Host helpers use the live guest state. */
    case 0xA6: case 0xA7: case 0xAE: case 0xAF: {
        const unsigned ww = (op & 1u) ? w : 1u;
        if (!repne) PJ_FAIL();
        pjt_c(t);
        pje32(&t->e, 0xE880u | CREG, 0x0FF0u); /* STMIA r12,{r4-r11} */
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, op | (ww << 8));
        pjt_call(t, (const void *)b86_pj_repne_cmp);
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pje32(&t->e, 0xE890u | CREG, 0x0FF0u); /* LDMIA r12,{r4-r11} */
        t->creg_ok = 1;
        t->last_kind = -1;
        t->ikind_clear = 1;
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        return 1; }
    case 0x98:                                       /* CWDE / CBW */
        if (o16) { pj_ext(&t->e, 0xFA4Fu, R1, GR(0)); pjt_putr(t, 0u, 2u, R1); }
        else pj_ext(&t->e, 0xFA0Fu, GR(0), GR(0));
        return 1;
    case 0x99:                                       /* CDQ / CWD */
        if (o16) { pj_ext(&t->e, 0xFA0Fu, R1, GR(0)); pj_shift(&t->e, R1, R1, 2u, 31u); pjt_putr(t, 2u, 2u, R1); }
        else pj_shift(&t->e, GR(2), GR(0), 2u, 31u);
        return 1;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        const unsigned ww = (op & 1u) ? w : 1u;
        if (!pjt_u32(t, &imm)) PJ_FAIL();
        pj_imm32(&t->e, R1, imm);
        pjt_mem(t, ww, op >= 0xA2u);
        if (op < 0xA2u) { pjt_ld(t, R1, R0, ww); pjt_putr(t, 0u, ww, R1); }
        else { pjt_getr(t, R1, 0u, ww); pjt_st(t, R1, R0, ww); }
        return 1; }
    case 0xA8: case 0xA9: {
        const unsigned ww = op == 0xA8u ? 1u : w;
        if (!pjt_imm(t, ww, &imm)) PJ_FAIL();
        pjt_getr(t, R1, 0u, ww);
        pj_imm32(&t->e, R0, imm);
        pjt_alu(t, 4u, ww, R3, R1, R0);
        return 1; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        if (!pjt_u8(t, &imm)) PJ_FAIL();
        pj_movw(&t->e, R1, imm);
        pjt_put8(t, op & 7u, R1);
        return 1;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (!pjt_imm(t, w, &imm)) PJ_FAIL();
        if (w == 4u) pj_imm32(&t->e, GR(op & 7u), imm);
        else { pj_movw(&t->e, R1, imm); pjt_putr(t, op & 7u, 2u, R1); }
        return 1;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: {
        const unsigned ww = (op & 1u) ? w : 1u;
        unsigned n;
        if (!pjt_modrm(t, &m) || (m.reg > 3u && m.reg != 4u && m.reg != 5u && m.reg != 7u)) PJ_FAIL();
        if (m.reg <= 3u && (op != 0xD1u && op != 0xC1u ||
            !(np == 1u || (np == 2u && o16)))) PJ_FAIL();
        if (op <= 0xC1u) { const unsigned save = t->pos; uint32_t c8;
            /* the count byte follows the ModRM displacement */
            if (!pjt_u8(t, &c8)) PJ_FAIL();
            n = c8 & 31u; (void)save;
        } else n = 1u;
        if (n == 0u || (m.reg > 3u && n >= 8u * ww)) PJ_FAIL();
        pjt_rm_load(t, &m, ww, 1, R1, &o);
        if (m.reg <= 3u) { /* D1 by 1; C1 by imm8 (16/32-bit operands) */
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_imm32(&t->e, R2, (m.reg << 8) | ww | (n << 16));
            pjt_call(t, op == 0xD1u ? (const void *)b86_pj_rotate1
                                       : (const void *)b86_pj_c1_rotate);
            pj_mov(&t->e, R1, R0);
            pjt_rm_store(t, &o, ww, R1);
            t->last_kind = -1; t->ikind_clear = 1;
            return 1;
        }
        if (!pjt_shift(t, m.reg, ww, n)) PJ_FAIL();
        pjt_rm_store(t, &o, ww, R1);
        return 1; }
    case 0xD3: {                                     /* SHL/SHR/SAR r/m32, CL */
        uint8_t *skip;
        if (o16 || !pjt_modrm(t, &m) || (m.reg != 4u && m.reg != 5u && m.reg != 7u)) PJ_FAIL();
        pjt_rm_load(t, &m, 4u, 1, R1, &o);
        pj_dpi(&t->e, 0u, 1, R0, GR(1), 31u);         /* ANDS r0, ecx, #31 (count 0: no change) */
        skip = pjt_bfwd(t, 0 /* EQ */);
        pj_dpi(&t->e, 13u, 0, R2, R0, 1u);            /* r2 = n-1 */
        if (m.reg == 4u) {                            /* CF = (a << (n-1)) >> 31 */
            pje32(&t->e, 0xFA01u, 0xF000u | (R2 << 8) | R2);   /* LSL r2, r1, r2 */
            pj_shift(&t->e, R2, R2, 1u, 31u);
            pje32(&t->e, 0xFA01u, 0xF000u | (R1 << 8) | R0);   /* LSL r1, r1, r0 */
            pj_shift(&t->e, R3, R1, 1u, 31u);
            pj_dpr(&t->e, 4u, 0, R3, R3, R2, 0u, 0u);          /* OF = msb ^ CF */
        } else if (m.reg == 5u) {
            pje32(&t->e, 0xFA21u, 0xF000u | (R2 << 8) | R2);   /* LSR r2, r1, r2 */
            pj_dpi(&t->e, 0u, 0, R2, R2, 1u);
            pj_shift(&t->e, R3, R1, 1u, 31u);                  /* OF = msb(a) */
            pje32(&t->e, 0xFA21u, 0xF000u | (R1 << 8) | R0);   /* LSR r1, r1, r0 */
        } else {
            pje32(&t->e, 0xFA41u, 0xF000u | (R2 << 8) | R2);   /* ASR r2, r1, r2 */
            pj_dpi(&t->e, 0u, 0, R2, R2, 1u);
            pj_movw(&t->e, R3, 0u);
            pje32(&t->e, 0xFA41u, 0xF000u | (R1 << 8) | R0);   /* ASR r1, r1, r0 */
        }
        pj_dpr(&t->e, 2u, 0, R2, R2, R3, 0u, 11u);
        pjt_c(t);
        pj_movw(&t->e, LR, LZ_SZPC32);
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
        pj_ldst(&t->e, PJ_STR, R1, CREG, (unsigned)offsetof(B86Cpu, lz_res));
        pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_b));
        pj_movw(&t->e, LR, LZ_NONE);
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
        pjt_rm_store(t, &o, 4u, R1);
        pjt_here(t, skip);
        t->creg_ok = 0;                               /* joined paths: r12 may not be loaded */
        t->last_kind = -1; t->ikind_clear = 0;        /* flags depend on the count */
        return 1; }
    case 0x06: case 0x1E:                            /* PUSH ES / PUSH DS (zero-extended) */
        if (o16) PJ_FAIL();
        pj_dpi(&t->e, 13u, 0, R1, GR(4), 4u);
        pjt_mem(t, 4u, 1);
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDR, R1, CREG, (unsigned)offsetof(B86Cpu, seg) + 4u * (op == 0x06u ? B86_ES : B86_DS));
        pj_ext(&t->e, 0xFA1Fu, R1, R1);
        pj_ldst(&t->e, PJ_STR, R1, R0, 0u);
        pj_dpi(&t->e, 13u, 0, GR(4), GR(4), 4u);
        return 1;
    case 0x07: case 0x1F:                            /* POP ES / POP DS (block ends: DS may stop being flat) */
        if (o16) PJ_FAIL();
        pj_mov(&t->e, R1, GR(4));
        pjt_mem(t, 4u, 0);
        pj_ldst(&t->e, PJ_LDRH, R2, R0, 0u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, op == 0x07u ? B86_ES : B86_DS);
        pjt_call(t, (const void *)b86_pj_loadseg);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pj_dpi(&t->e, 8u, 0, GR(4), GR(4), 4u);
        pjt_exit(t, PJ_NEXT(), t->ninsn, 2);          /* segment changed: re-check flatness in C */
        return 2;
    case 0xC2: case 0xC3:
        if (o16) PJ_FAIL();
        if (op == 0xC2u && !pjt_imm(t, 2u, &imm)) PJ_FAIL();
        pj_mov(&t->e, R1, GR(4));
        pjt_mem(t, 4u, 0);
        pj_ldst(&t->e, PJ_LDR, R1, R0, 0u);
        pjt_addimm(t, GR(4), GR(4), 4u + (op == 0xC2u ? imm : 0u));
        pjt_exit(t, 0u, t->ninsn, 1);              /* dynamic: EIP in r1 */
        return 2;
    case 0xC6: case 0xC7: {
        const unsigned ww = op == 0xC6u ? 1u : w;
        if (!pjt_modrm(t, &m) || m.reg != 0u) PJ_FAIL();
        if (m.mod == 3u) { if (!pjt_imm(t, ww, &imm)) PJ_FAIL(); pj_imm32(&t->e, R1, imm); pjt_putr(t, m.rm, ww, R1); return 1; }
        pjt_mem(t, ww, 1);
        if (!pjt_imm(t, ww, &imm)) PJ_FAIL();
        pj_imm32(&t->e, R1, imm);
        pjt_st(t, R1, R0, ww);
        return 1; }
    case 0xC9:                                       /* LEAVE */
        if (o16) PJ_FAIL();
        pj_mov(&t->e, R1, GR(5));
        pjt_mem(t, 4u, 0);
        pj_dpi(&t->e, 8u, 0, GR(4), GR(5), 4u);
        pj_ldst(&t->e, PJ_LDR, GR(5), R0, 0u);
        return 1;
    /* BB_PJ_C1_E3_66_V1: E3 rel8 = JECXZ in 32-bit address mode.
     * 67 JCXZ remains on the interpreter path (67 is not decoded here).
     * The branch tests ECX without changing guest arithmetic flags. */
    case 0xE3:
        if (np != 1u || !pjt_u8(t, &imm)) PJ_FAIL();
        imm = (uint32_t)(int32_t)(int8_t)imm;
        pj_dpi(&t->e, 13u, 1, PC, GR(1), 0u); /* CMP ecx, #0 */
        pjt_exit_cond(t, 0 /* EQ */, PJ_NEXT() + imm, t->ninsn);
        pjt_exit(t, PJ_NEXT(), t->ninsn, 0);
        return 2;
    case 0xE8: {
        if (o16 || !pjt_u32(t, &imm)) PJ_FAIL();
        pj_dpi(&t->e, 13u, 0, R1, GR(4), 4u);
        pjt_mem(t, 4u, 1);
        pj_imm32(&t->e, R1, PJ_NEXT());
        pj_ldst(&t->e, PJ_STR, R1, R0, 0u);
        pj_dpi(&t->e, 13u, 0, GR(4), GR(4), 4u);
        pjt_exit(t, PJ_NEXT() + imm, t->ninsn, 0);
        return 2; }
    case 0xE9: case 0xEB:
        if (o16) PJ_FAIL();
        if (op == 0xE9u) { if (!pjt_u32(t, &imm)) PJ_FAIL(); }
        else { if (!pjt_u8(t, &imm)) PJ_FAIL(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        pjt_exit(t, PJ_NEXT() + imm, t->ninsn, 0);
        return 2;
    case 0xF6: case 0xF7: {
        const unsigned ww = op == 0xF6u ? 1u : w;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        if (m.reg == 0u) {                            /* TEST r/m, imm */
            pjt_rm_load(t, &m, ww, 0, R1, &o);
            if (!pjt_imm(t, ww, &imm)) PJ_FAIL();
            pj_imm32(&t->e, R0, imm);
            pjt_alu(t, 4u, ww, R3, R1, R0);
            return 1;
        }
        if (m.reg == 2u || m.reg == 3u) {             /* NOT / NEG */
            pjt_rm_load(t, &m, ww, 1, R1, &o);
            if (m.reg == 2u) pj_dpr(&t->e, 3u, 0, R1, PC, R1, 0u, 0u);   /* MVN */
            else { pj_movw(&t->e, R0, 0u); pjt_alu(t, 5u, ww, R1, R0, R1); }
            pjt_rm_store(t, &o, ww, R1);
            return 1;
        }
        if (m.reg >= 6u) {                            /* DIV / IDIV: helper, #DE left to the interpreter */
            pjt_rm_load(t, &m, ww, 0, R2, &o);
            if (ww == 1u) { pj_ext(&t->e, 0xFA1Fu, R0, GR(0)); pj_movw(&t->e, R1, 0u); }   /* AX */
            else { pj_mov(&t->e, R0, GR(0)); pj_mov(&t->e, R1, GR(2)); }
            pj_movw(&t->e, R3, ww | (m.reg == 7u ? 8u : 0u));
            pjt_call(t, (const void *)b86_pj_div);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            pj_imm32(&t->e, R3, (uint32_t)(uintptr_t)b86_pj_divres);
            pj_ldst(&t->e, PJ_LDR, R0, R3, 0u);
            pj_ldst(&t->e, PJ_LDR, R1, R3, 4u);
            if (ww == 1u) { pj_bfi(&t->e, R0, R1, 8u, 8u); pjt_putr(t, 0u, 2u, R0); }   /* AL=q AH=r */
            else { pjt_putr(t, 0u, ww, R0); pjt_putr(t, 2u, ww, R1); }
            return 1;                                 /* flags unchanged (undefined) */
        }
        if ((m.reg == 4u || m.reg == 5u) && ww == 4u) {   /* MUL / IMUL EDX:EAX = EAX * r/m32 */
            pjt_rm_load(t, &m, 4u, 0, R1, &o);
            /* UMULL/SMULL RdLo=r0, RdHi=r2, Rn=r4(EAX), Rm=r1 */
            pje32(&t->e, m.reg == 4u ? 0xFBA0u | GR(0) : 0xFB80u | GR(0), (R0 << 12) | (R2 << 8) | R1);
            pj_mov(&t->e, GR(0), R0); pj_mov(&t->e, GR(2), R2);
            if (m.reg == 4u) {                           /* CF=OF = hi != 0 */
                pj_dpi(&t->e, 13u, 1, PC, R2, 0u);
            } else {                                     /* CF=OF = hi != sign(lo) */
                pj_dpr(&t->e, 13u, 1, PC, R2, R0, 2u, 31u);   /* CMP hi, lo ASR #31 */
            }
            pj_movw(&t->e, R2, 0u);
            { uint8_t *sk = pjt_bfwd(t, 0 /* EQ */); pj_movw(&t->e, R2, 0x801u); pjt_here(t, sk); }
            goto szpc_lo;
        }
        PJ_FAIL(); }
    case 0xFE: case 0xFF: {
        const unsigned ww = op == 0xFEu ? 1u : w;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        if (m.reg <= 1u) {
            pjt_rm_load(t, &m, ww, 1, R2, &o);
            if (m.reg == 0u) pj_dpi(&t->e, 8u, 0, R3, R2, 1u); else pj_dpi(&t->e, 13u, 0, R3, R2, 1u);
            pjt_rm_store(t, &o, ww, R3);
            pjt_lazy(t, m.reg == 0u ? pj_k_inc[ww] : pj_k_dec[ww], R2, R3);
            return 1;
        }
        if (op == 0xFEu || o16) PJ_FAIL();
        if (m.reg == 6u) {                             /* PUSH r/m32 */
            pjt_rm_load(t, &m, 4u, 0, R1, &o);
            pj_ldst(&t->e, PJ_STR, R1, SP, 4u);
            pj_dpi(&t->e, 13u, 0, R1, GR(4), 4u);
            pjt_mem(t, 4u, 1);
            pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
            pj_ldst(&t->e, PJ_STR, R1, R0, 0u);
            pj_dpi(&t->e, 13u, 0, GR(4), GR(4), 4u);
            return 1;
        }
        if (m.reg == 4u) {                             /* JMP r/m32 */
            pjt_rm_load(t, &m, 4u, 0, R1, &o);
            pjt_exit(t, 0u, t->ninsn, 1);
            return 2;
        }
        if (m.reg == 2u) {                             /* CALL r/m32 */
            pjt_rm_load(t, &m, 4u, 0, R1, &o);
            pj_ldst(&t->e, PJ_STR, R1, SP, 4u);           /* [sp+8] is the chained count */
            pj_dpi(&t->e, 13u, 0, R1, GR(4), 4u);
            pjt_mem(t, 4u, 1);
            pj_imm32(&t->e, R1, PJ_NEXT());
            pj_ldst(&t->e, PJ_STR, R1, R0, 0u);
            pj_dpi(&t->e, 13u, 0, GR(4), GR(4), 4u);
            pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
            pjt_exit(t, 0u, t->ninsn, 1);
            return 2;
        }
        PJ_FAIL(); }
    case 0x0F:
        if (!pjt_u8(t, &op2)) PJ_FAIL();
        /* BB_PJ_SHIFTD_GS_X87_V1: native SHLD/SHRD r/m16/32,r,imm8.
         * Read the immediate before emitting any memory access, so a count
         * of zero is a true no-op and the PM27-undefined 16-bit range stays
         * in its original interpreter path. The helper updates FLAGS, not a
         * transient ARM CPSR; its result is stored only after it succeeds. */
        if (op2 == 0xA4u || op2 == 0xACu) {
            PjOp sh;
            uint32_t c8;
            unsigned n;
            if (repz || !pjt_modrm(t, &m) || !pjt_u8(t, &c8)) PJ_FAIL();
            n = c8 & 31u;
            if (n > 8u * w) PJ_FAIL();
            if (!n) return 1;
            pjt_rm_load(t, &m, w, 1, R1, &sh);
            pjt_getr(t, R2, m.reg, w);
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_imm32(&t->e, R3, n | (w << 8) | ((op2 == 0xA4u) ? (1u << 16) : 0u));
            pjt_call(t, (const void *)b86_pj_shiftd_imm);
            pj_mov(&t->e, R1, R0);
            pjt_rm_store(t, &sh, w, R1);
            t->last_kind = -1; t->ikind_clear = 1;
            return 1;
        }
        /* PUSH FS/GS: 32-bit flat SS only (the PJ entry contract). Source
         * selector is a 16-bit register; PM26 zero-extends when w=4. POP
         * FS/GS remains in PM26 because it must validate the descriptor. */
        if (op2 == 0xA0u || op2 == 0xA8u) {
            const unsigned sr = op2 == 0xA0u ? B86_FS : B86_GS;
            if (np != 1u || o16 || segpref || repz) PJ_FAIL();
            pj_dpi(&t->e, 13u, 0, R1, GR(4), 4u);
            pjt_mem(t, 4u, 1);
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDRH, R1, CREG,
                    (unsigned)(offsetof(B86Cpu, seg386) +
                     (sr - B86_FS) * sizeof(t->c->seg386[0])));
            pj_ldst(&t->e, PJ_STR, R1, R0, 0u);
            pj_dpi(&t->e, 13u, 0, GR(4), GR(4), 4u);
            return 1;
        }
        /* BB_PJ_CL_SHIFTD_REG_V1: SHLD/SHRD r32,r32,CL (0F A5/AD).
         * No ES access, no guest memory, no 16-bit undefined-count range.
         * Preserve the interpreter for 16-bit and memory operands. CL is
         * sampled from live ECX BEFORE the destination register is written.
         * The existing PM27-equivalent helper handles counts 1..31 and flags;
         * a masked zero count is a no-op. */
        if (op2 == 0xA5u || op2 == 0xADu) {
            PjOp sh;
            if (repz || o16 || !pjt_modrm(t, &m) || m.mod != 3u) PJ_FAIL();
            pjt_rm_load(t, &m, 4u, 1, R1, &sh);
            pjt_getr(t, R2, m.reg, 4u);
            pjt_c(t);
            pj_imm32(&t->e, R3, (4u << 8) |
                     ((op2 == 0xA5u) ? (1u << 16) : 0u));
            pj_ubfx(&t->e, R0, GR(1), 0u, 5u); /* ECX[4:0] */
            pj_dpr(&t->e, 2u, 0, R3, R3, R0, 0u, 0u); /* ORR desc,CL */
            pj_mov(&t->e, R0, CREG);
            pjt_call(t, (const void *)b86_pj_shiftd_imm);
            pj_mov(&t->e, R1, R0);
            pjt_rm_store(t, &sh, 4u, R1);
            t->last_kind = -1; t->ikind_clear = 1;
            return 1;
        }
        /* BB_PJ_NATIVE_POP_FSGS_V1: validated 32-bit stack POP FS/GS.
         * 0F A1 / 0F A9: the existing pointer trampoline checks paging and
         * boundaries, and b86_pj_loadseg validates/commits the selector.
         * The guest ESP changes only after BOTH checks succeed. A refused
         * selector or memory access replays the untouched instruction in C.
         * Unlike POP DS/ES, FS/GS do not affect the native flat ES contract,
         * so a successful POP continues in this block (no dispatcher exit).
         * 66h, segment override and REP forms stay with the interpreter. */
#ifndef BB_PJ_NATIVE_POP_FSGS
#define BB_PJ_NATIVE_POP_FSGS 1
#endif
#if BB_PJ_NATIVE_POP_FSGS
        if (op2 == 0xA1u || op2 == 0xA9u) {
            const unsigned sr = op2 == 0xA1u ? B86_FS : B86_GS;
            if (np != 1u || o16 || segpref || repz) PJ_FAIL();
            pj_mov(&t->e, R1, GR(4));           /* linear SS:ESP, no ESP update */
            pjt_mem(t, 4u, 0);                   /* checked read, retry on fault */
            pj_ldst(&t->e, PJ_LDRH, R2, R0, 0u); /* selector = low 16 bits */
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_movw(&t->e, R1, sr);
            pjt_call(t, (const void *)b86_pj_loadseg);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            pj_dpi(&t->e, 8u, 0, GR(4), GR(4), 4u);
            return 1;
        }
#endif
        if (op2 >= 0x80u && op2 <= 0x8Fu) {
            if (o16 || !pjt_u32(t, &imm)) PJ_FAIL();
            pjt_jcc(t, op2 & 15u, PJ_NEXT() + imm, PJ_NEXT());
            return 2;
        }
        if (op2 >= 0x90u && op2 <= 0x9Fu) {           /* SETcc r/m8 */
            int arm;
            uint8_t *sk;
            if (!pjt_modrm(t, &m)) PJ_FAIL();
            if (m.mod != 3u) { pjt_mem(t, 1u, 1); pj_ldst(&t->e, PJ_STR, R0, SP, 4u); o.mem = 1; } else { o.mem = 0; o.rm = m.rm; }
            arm = pjt_flags_for(t, op2 & 15u);
            pj_movw(&t->e, R1, 1u);
            sk = pjt_bfwd(t, arm);
            pj_movw(&t->e, R1, 0u);
            pjt_here(t, sk);
            pjt_rm_store(t, &o, 1u, R1);
            return 1;
        }
        if (op2 == 0xAFu) {                           /* IMUL r, r/m */
            if (o16 || !pjt_modrm(t, &m)) PJ_FAIL();
            pjt_rm_load(t, &m, 4u, 0, R1, &o);
            pj_mov(&t->e, R0, GR(m.reg));
        imul_rr:
            /* SMULL lo=r0? use r2:r3 then write reg */
            pje32(&t->e, 0xFB80u | R0, (R3 << 12) | (R2 << 8) | R1);   /* SMULL r3(lo), r2(hi), r0, r1 */
            pj_mov(&t->e, GR(m.reg), R3);
            pj_dpr(&t->e, 13u, 1, PC, R2, R3, 2u, 31u);  /* CMP hi, lo ASR #31 */
            pj_mov(&t->e, R0, R3);
            pj_movw(&t->e, R2, 0u);
            { uint8_t *sk = pjt_bfwd(t, 0 /* EQ */); pj_movw(&t->e, R2, 0x801u); pjt_here(t, sk); }
        szpc_lo:
            pjt_c(t);
            pj_movw(&t->e, LR, LZ_SZPC32);
            pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
            pj_ldst(&t->e, PJ_STR, R0, CREG, (unsigned)offsetof(B86Cpu, lz_res));
            pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_b));
            if (!t->ikind_clear) { pj_movw(&t->e, LR, LZ_NONE); pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind)); t->ikind_clear = 1; }
            t->last_kind = LZ_SZPC32;
            return 1;
        }
        if (op2 == 0xB6u || op2 == 0xB7u || op2 == 0xBEu || op2 == 0xBFu) {
            const unsigned sw = (op2 & 1u) ? 2u : 1u;
            if (!pjt_modrm(t, &m)) PJ_FAIL();
            pjt_rm_load(t, &m, sw, 0, R1, &o);
            if (op2 == 0xBEu) pj_ext(&t->e, 0xFA4Fu, R1, R1);
            if (op2 == 0xBFu) pj_ext(&t->e, 0xFA0Fu, R1, R1);
            pjt_putr(t, m.reg, w, R1);
            return 1;
        }
        PJ_FAIL();
    default:
        PJ_FAIL();
    }
#undef PJ_FAIL
#undef PJ_NEXT
}

/* Block tail: r1 = EIP, r0 = retired in this block; adds the chained count
 * kept at [sp+8] and records LR (after a chainable stub's BL) in pj_exit.
 * tail_z first clears LR (exit not chainable). */
static void pjt_tail(PjT *t, uint8_t **tail_z, uint8_t **tail)
{
    *tail_z = t->e.p;
    pj_movw(&t->e, LR, 0u);
    *tail = t->e.p;
    pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
    pje32(&t->e, 0xE880u | CREG, 0x0FF0u);        /* STMIA r12, {r4-r11} (r[] at offset 0) */
    pj_ldst(&t->e, PJ_STR, R1, CREG, (unsigned)offsetof(B86Cpu, ip));
    pj_ldst(&t->e, PJ_LDR, R2, SP, 8u);
    pj_dpr(&t->e, 8u, 0, R0, R0, R2, 0u, 0u);
    pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, pj_exit));
    pj_dpi(&t->e, 8u, 0, SP, SP, 20u);             /* drop the 20-byte frame */
    pje32(&t->e, 0xE8BDu, 0x8FF0u);                /* POP {r4-r11, pc} */
}
/* Exit stub. Chainable static exits have a fixed 20-byte layout
 *   MOVW r1,lo; MOVT r1,hi; MOVW r0,n; BL tail; NOP; NOP
 * linking replaces the BL with BL chk and the NOPs with B.W <target body>. */
static void pjt_stub(PjT *t, const PjExit *x, const B86Pj *j)
{
    uint8_t *const tail_z = j->tail_z, *const tail = j->tail;
    pj_patch_b(x->at, t->e.p);
    if (x->dyn == 0) {
        pj_movw(&t->e, R1, x->ip & 0xFFFFu); pj_movt(&t->e, R1, x->ip >> 16);
        pj_movw(&t->e, R0, x->n);
        { uint8_t *bl = t->e.p; pje32(&t->e, 0xF000u, 0xD000u);       /* BL tail (patched below) */
          pj_patch_b(bl, tail);
          bl[3] |= 0x40u; }                                           /* B.W T4 -> BL: set bit 14 of hw2 */
        pje32(&t->e, 0xBF00u, 0xBF00u);                               /* +16: B.W target once linked */
        return;
    }
    if (x->dyn == 2) pj_imm32(&t->e, R1, x->ip);
    pj_movw(&t->e, R0, x->n);
    pj_patch_b(pj_b(&t->e, -1), x->dyn == 1 ? j->dyn : tail_z);   /* EIP in r1: look the target up natively */
}

/* Shared code: tails and the native lookup for dynamic exits (RET, indirect
 * JMP/CALL): r1 = target EIP, r0 = retired in the leaving block. A hit
 * enters the target's chain entry with r3 = r0; a miss returns to C. */
static B86_PJ_COLD void pj_emit_shared(B86Pj *j)
{
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    /* PJ emits on core 0 only. Keep the 96-exit workspace out of the 4 KiB
     * hardware scratch stack; this is not on the execution hot path. */
    static PjT tt;
#else
    PjT tt;  /* host tests retain a reentrant automatic workspace */
#endif
    PjT *t = &tt;
    uint8_t *miss1, *miss2;
    unsigned hb = 0u;
    while ((1u << hb) < B86_PJ_HASH) ++hb;
    memset(t, 0, sizeof *t);
    t->e.p = j->code; t->e.end = j->code + j->cap;
    pjt_tail(t, &j->tail_z, &j->tail);
    /* chk: linked static exit. r0 = retired, r1 = target EIP, lr -> B.W target */
    j->chk = t->e.p;
    {
        uint8_t *f1, *f2;
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, irq));
        pj_ldst(&t->e, PJ_LDR, R3, CREG, (unsigned)offsetof(B86Cpu, pj_stop));
        pj_dpr(&t->e, 2u, 1, R2, R2, R3, 0u, 0u);          /* ORRS */
        f1 = pj_b(&t->e, 1 /* NE */);
        pj_ldst(&t->e, PJ_LDR, R2, SP, 8u);
        pj_dpi(&t->e, 13u, 1, PC, R2, B86_PJ_CHAIN);
        f2 = pj_b(&t->e, 2 /* HS */);
        pj_dpr(&t->e, 8u, 0, R2, R2, R0, 0u, 0u);
        pj_ldst(&t->e, PJ_STR, R2, SP, 8u);
        pje16(&t->e, 0x4700u | (LR << 3));                  /* BX lr */
        pj_patch_b(f1, j->tail_z); pj_patch_b(f2, j->tail_z);
    }
    /* BB_PJ_SHARED_MEMTRAMP_FAST: one native read-TLB fast path per JIT,
     * NOT one per translated instruction. R1=linear, R2=size|write<<8,
     * LR addresses the two caller literal words (EIP and retired).
     * Preserve R1 and R2 for the exact original C slow path. No writes
     * take the fast path; all guest writes retain SMC protection.
     * [sp+4] and [sp+16] belong to callers; never use them as scratch.
     * Fail closed for unusual widths, paging off, crossings, TLB misses,
     * and out-of-RAM addresses. No new B86Cpu/B86Pj fields. */
#ifndef BB_PJ_SHARED_MEMTRAMP_FAST
#define BB_PJ_SHARED_MEMTRAMP_FAST 1
#endif
    j->memtramp = t->e.p;
    {
        uint8_t *fail;
        pj_ldst(&t->e, PJ_STR, LR, SP, 12u);
#if BB_PJ_SHARED_MEMTRAMP_FAST
        {
            uint8_t *miss[10];
            unsigned nm = 0u, i;
#if defined(B86_PAGED) && B86_RAM_BYTES > B86_MEM_BYTES
            uint8_t *skip_delta;
#endif
            /* Scratch: R0,R3,R12,LR only. R1 and R2 must survive a miss;
             * LR is saved at [sp+12]. R4-R11 are guest registers. */
            pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG, (unsigned)offsetof(B86Cpu, pg));
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            miss[nm++] = pj_b(&t->e, 0 /* EQ: paging disabled */);
            pj_dpi(&t->e, 13u, 1, PC, R2, 1u);
            miss[nm++] = pj_b(&t->e, 3 /* LO: invalid size */);
            pj_dpi(&t->e, 13u, 1, PC, R2, 4u);
            miss[nm++] = pj_b(&t->e, 8 /* HI: write/invalid size */);
            pj_dpi(&t->e, 13u, 1, PC, R2, 3u);
            miss[nm++] = pj_b(&t->e, 0 /* EQ: invalid size */);
            pj_ubfx(&t->e, R0, R1, 0u, 12u);
            pj_dpr(&t->e, 8u, 0, R0, R0, R2, 0u, 0u);
            pj_dpi(&t->e, 13u, 1, PC, R0, 4096u);
            miss[nm++] = pj_b(&t->e, 8 /* HI: page crossing */);
            pj_ubfx(&t->e, R3, R1, 12u, 6u);
            pj_shift(&t->e, LR, R1, 1u, 12u);
            pj_shift(&t->e, LR, LR, 0u, 12u);
            pj_dpi(&t->e, 2u, 0, LR, LR, 1u);
            pjt_addimm(t, R0, CREG, (uint32_t)offsetof(B86Cpu, tlb_tag));
            pje32(&t->e, 0xF850u | R0, (R0 << 12) | (2u << 4) | R3);
            pj_dpr(&t->e, 13u, 1, PC, R0, LR, 0u, 0u);
            miss[nm++] = pj_b(&t->e, 1 /* NE: TLB miss */);
            pjt_addimm(t, R0, CREG, (uint32_t)offsetof(B86Cpu, tlb_phys));
            pje32(&t->e, 0xF850u | R0, (R0 << 12) | (2u << 4) | R3);
            pj_ubfx(&t->e, R3, R1, 0u, 12u);
            pj_dpr(&t->e, 2u, 0, R0, R0, R3, 0u, 0u);
            /* phys <= B86_RAM_BYTES - size avoids arithmetic wraparound. */
            pj_imm32(&t->e, LR, B86_RAM_BYTES);
            pj_dpr(&t->e, 13u, 0, LR, LR, R2, 0u, 0u);
            pj_dpr(&t->e, 13u, 1, PC, R0, LR, 0u, 0u);
            miss[nm++] = pj_b(&t->e, 8 /* HI: beyond guest RAM */);
#if defined(B86_PAGED) && B86_RAM_BYTES > B86_MEM_BYTES
            pj_mov(&t->e, R3, R0); /* preserve phys for low-window test */
#endif
            pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, mem));
            pj_dpr(&t->e, 8u, 0, R0, R0, LR, 0u, 0u);
#ifdef B86_PAGED
#if B86_RAM_BYTES > B86_MEM_BYTES
            pj_imm32(&t->e, LR, B86_MEM_BYTES);
            pj_dpr(&t->e, 13u, 1, PC, R3, LR, 0u, 0u);
            skip_delta = pj_b(&t->e, 2 /* HS: extended RAM is unmapped */);
#endif
            pj_ubfx(&t->e, R3, R0, 12u, 9u);
            pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, pt));
            pj_dpr(&t->e, 8u, 0, LR, LR, R3, 0u, 2u);
            pj_ldst(&t->e, PJ_LDR, LR, LR, 0u);
            pj_dpr(&t->e, 8u, 0, R0, R0, LR, 0u, 0u);
#if B86_RAM_BYTES > B86_MEM_BYTES
            pj_patch_b(skip_delta, t->e.p);
#endif
#endif
            /* Return directly after the two literal words at the callsite. */
            pj_ldst(&t->e, PJ_LDR, LR, SP, 12u);
            pj_dpi(&t->e, 8u, 0, LR, LR, 8u);
            pje16(&t->e, 0x4700u | (LR << 3));
            pje16(&t->e, 0xBF00u);
            /* Every rejected access resumes the unmodified C slow path. */
            for (i = 0u; i < nm; ++i) pj_patch_b(miss[i], t->e.p);
        }
#endif
        pj_ldst(&t->e, PJ_LDR, R0, SP, 0u);
        pj_call(&t->e, (const void *)b86_pj_mem);
        pj_ldst(&t->e, PJ_LDR, LR, SP, 12u);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        fail = pj_b(&t->e, 0 /* EQ */);
        pj_dpi(&t->e, 8u, 0, LR, LR, 8u);
        pje16(&t->e, 0x4700u | (LR << 3));                  /* BX lr: continue after the literal */
        pje16(&t->e, 0xBF00u);
        pj_patch_b(fail, t->e.p);
        pj_dpi(&t->e, 13u, 0, LR, LR, 1u);
        pj_ldst(&t->e, PJ_LDR, R1, LR, 0u);
        pj_ldst(&t->e, PJ_LDR, R0, LR, 4u);
        pj_patch_b(pj_b(&t->e, -1), j->tail_z);
    }
    /* entry(c, body): PUSH {r4-r11,lr}; SUB sp,#12; [sp]=c; load regs; chained count = 0; BX body */
    j->entry = t->e.p;
    pje32(&t->e, 0xE92Du, 0x4FF0u);
    pj_dpi(&t->e, 13u, 0, SP, SP, 20u);   /* [sp]=c [sp+4]=scratch [sp+8]=count [sp+12]=memtramp lr */
    pj_ldst(&t->e, PJ_STR, R0, SP, 0u);
    pje32(&t->e, 0xE890u, 0x0FF0u);
    pj_movw(&t->e, R2, 0u);
    pj_ldst(&t->e, PJ_STR, R2, SP, 8u);
    pje16(&t->e, 0x4700u | (R1 << 3));                      /* BX r1 */
    pje16(&t->e, 0xBF00u);
    j->dyn = t->e.p;
    pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
    pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, pj));
    pj_movw(&t->e, R3, 0x79B1u); pj_movt(&t->e, R3, 0x9E37u);          /* 2654435761 */
    pje32(&t->e, 0xFB00u | R1, 0xF000u | (R3 << 8) | R3);             /* MUL r3, r1, r3 */
    pj_ubfx(&t->e, R3, R3, 16u, hb);
    pjt_addimm(t, R2, R2, (uint32_t)offsetof(B86Pj, tag));
    pje32(&t->e, 0xF850u | R2, (LR << 12) | (2u << 4) | R3);          /* LDR lr, [r2, r3, LSL #2] */
    pj_dpi(&t->e, 2u, 0, R12, R1, 1u);                                  /* r12 = eip | 1 */
    pj_dpr(&t->e, 13u, 1, PC, LR, R12, 0u, 0u);
    miss1 = pj_b(&t->e, 1 /* NE */);
    pjt_addimm(t, R2, R2, (uint32_t)(offsetof(B86Pj, ent) - offsetof(B86Pj, tag)));
    pje32(&t->e, 0xF850u | R2, (LR << 12) | (2u << 4) | R3);          /* LDR lr, [r2, r3, LSL #2] (ent) */
    pj_dpi(&t->e, 13u, 1, PC, LR, 0u);
    miss2 = pj_b(&t->e, 0 /* EQ */);
    pj_imm32(&t->e, R12, (uint32_t)(uintptr_t)j->code);
    pj_dpr(&t->e, 8u, 0, LR, LR, R12, 0u, 0u);                          /* code + ent = body | 1 */
    {   /* same checks as chk, then count and enter */
        uint8_t *f1, *f2;
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, irq));
        pj_ldst(&t->e, PJ_LDR, R3, CREG, (unsigned)offsetof(B86Cpu, pj_stop));
        pj_dpr(&t->e, 2u, 1, R2, R2, R3, 0u, 0u);
        f1 = pj_b(&t->e, 1 /* NE */);
        pj_ldst(&t->e, PJ_LDR, R2, SP, 8u);
        pj_dpi(&t->e, 13u, 1, PC, R2, B86_PJ_CHAIN);
        f2 = pj_b(&t->e, 2 /* HS */);
        pj_dpr(&t->e, 8u, 0, R2, R2, R0, 0u, 0u);
        pj_ldst(&t->e, PJ_STR, R2, SP, 8u);
        pje16(&t->e, 0x4700u | (LR << 3));                              /* BX lr */
        pj_patch_b(f1, j->tail_z); pj_patch_b(f2, j->tail_z);
    }
    pj_patch_b(miss1, j->tail_z); pj_patch_b(miss2, j->tail_z);
    j->used = (uint32_t)(t->e.p - j->code);
#if defined(__linux__)
    __builtin___clear_cache((char *)j->code, (char *)t->e.p);
#else
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
}
static B86_PJ_COLD void b86_pj_flush(B86Cpu *c)
{
    B86Pj *j = b86_pj_state(c);
    if (!j) return;
    memset(j->tag, 0, sizeof j->tag);
    memset(j->ent, 0, sizeof j->ent);
    memset(j->cpg, 0, sizeof j->cpg);
    memset(j->cps, 0, sizeof j->cps);
    memset(j->es_skip_eip, 0, sizeof j->es_skip_eip);
    j->used = 0u;
    j->flush_pending = 0;
    c->pj_stop = 0u;
    pj_emit_shared(j);
    ++j->flushes;
}

/* Read up to 160 code bytes at flat EIP (stops at an unmapped page). */
static B86_PJ_COLD unsigned pj_fetch(B86Cpu *c, uint32_t eip, uint8_t *buf, unsigned max)
{
    unsigned n = 0u;
    while (n < max) {
        const uint32_t a = eip + n;
        uint32_t phys, chunk = 0x1000u - (a & 0xFFFu);
        if (chunk > max - n) chunk = max - n;
        if (c->pg) { if (!b86_pg_walk(c, a, &phys)) break; } else phys = a;
        if (phys >= B86_RAM_BYTES || chunk > B86_RAM_BYTES - phys) break;
        memcpy(buf + n, b86_host_phys(c, phys), chunk);
        n += chunk;
    }
    return n;
}

/* BB_PJ_PM16_MOVREAD_V1: only checked MOV r8/r16, [16-bit EA]. */
#ifndef BB_PJ_PM16_MOV_READ
#define BB_PJ_PM16_MOV_READ 1
#endif
/* BB_PJ_PM16_PERF_GATE_V1: identical firmware except PM16 admission gate. */
#define BB_PJ_PM16_PERF_GATE 1
/* BB_PJ_PM16_TRUTHDIAG_V1: read-only counters; DEBUG BUILD ONLY. */
#ifndef BB_PJ_PM16_TRUTHDIAG
#define BB_PJ_PM16_TRUTHDIAG 1
#endif
#if BB_PJ_PM16_TRUTHDIAG
#include <stdio.h>
static struct {
    uint32_t emit_attempt, emit_ok, emit_fail;
    uint32_t pref66, pref66_mov, pref66_stack, pref66_0f, pref66_other;
    uint32_t read_attempt, read_success, write_attempt, write_success;
} bb_pm16_diag;
#define BB_PM16_DIAG_INC(field) (++bb_pm16_diag.field)
#else
#define BB_PM16_DIAG_INC(field) ((void)0)
#endif
/* BB_PJ_PM16_OP66REG_V1: operand-override register-only PM16 tier.
 * No memory, stack, segment, control transfer or further prefixes. */
#ifndef BB_PJ_PM16_OP66REG
#define BB_PJ_PM16_OP66REG 1
#endif
/* BB_PJ_PM16_MOVSTORE_V1: checked PM16 MOV memory stores only. */
#ifndef BB_PJ_PM16_MOV_WRITE
#define BB_PJ_PM16_MOV_WRITE 1
#endif
#if BB_PJ_PM16_REG_TIER
#if BB_PJ_PM16_MOV_WRITE
/* Checked store: segment checks and physical translation BEFORE any write.
 * No expand-down, byte/word crossing, unaligned word, code segment, or
 * address wrapping. Unsupported but legal cases deopt to the interpreter.
 * Always exit the native block after a successful store to honor SMC stop. */
static B86_PM_HOT uint32_t b86_pj_pm16_write(B86Cpu *c, uint32_t off,
                                               uint32_t desc, uint32_t value)
{
    const unsigned seg = (desc >> 8) & 7u, width = desc & 0xFFu;
    const B86PM1Cache *s;
    uint64_t linear;
    uint8_t *p;
    BB_PM16_DIAG_INC(write_attempt);
    if ((width != 1u && width != 2u) || (seg != B86_DS && seg != B86_SS) ||
        off > 0xFFFFu) return 0u;
    s = &c->pm386.seg[seg];
    /* S=1, data=1, writable=1, expand-up=1. Avoid descriptor subtleties
     * not covered by the existing conservative MOV-read tier. */
    if (!s->valid || s->reserved || (s->access & 0x1Au) != 0x12u ||
        (s->access & 0x04u) || off > s->limit ||
        (uint32_t)(width - 1u) > s->limit - off) return 0u;
    linear = (uint64_t)s->base + (uint64_t)off;
    if (linear + width - 1u > UINT32_MAX) return 0u;
    if (width == 2u && (linear & 1u)) return 0u;
    p = b86_pj_mem(c, (uint32_t)linear, width | 0x100u);
    if (!p) return 0u;
    p[0] = (uint8_t)value;
    if (width == 2u) p[1] = (uint8_t)(value >> 8);
    BB_PM16_DIAG_INC(write_success);
    return 1u;
}
#endif /* BB_PJ_PM16_MOV_WRITE */

#if BB_PJ_PM16_MOV_READ
/* Checked segment-aware read pointer. All failures deopt BEFORE changing
 * the destination GPR; the interpreter owns descriptor exceptions/page faults.
 * This tier only handles expand-up readable DATA, not code or expand-down. */
static B86_PM_HOT uint8_t *b86_pj_pm16_read_ptr(B86Cpu *c,
                                                   uint32_t off, uint32_t desc)
{
    const unsigned seg = (desc >> 8) & 7u, width = desc & 0xFFu;
    const B86PM1Cache *s;
    uint64_t linear;
    BB_PM16_DIAG_INC(read_attempt);
    if ((width != 1u && width != 2u) || (seg != B86_DS && seg != B86_SS) ||
        off > 0xFFFFu) return NULL;
    s = &c->pm386.seg[seg];
    if (!s->valid || s->reserved || (s->access & 0x1Au) != 0x12u ||
        (s->access & 0x04u) || off > s->limit ||
        (uint32_t)(width - 1u) > s->limit - off) return NULL;
    linear = (uint64_t)s->base + (uint64_t)off;
    if (linear + width - 1u > UINT32_MAX) return NULL;
    /* Never require unaligned halfword semantics of the Cortex-M33: the
     * interpreter handles these valid but less common x86 reads. */
    if (width == 2u && (linear & 1u)) return NULL;
    uint8_t *p = b86_pj_mem(c, (uint32_t)linear, width);
    if (p) BB_PM16_DIAG_INC(read_success);
    return p;
}
#endif /* BB_PJ_PM16_MOV_READ */

/* 16-bit protected-mode fetch with CS.base and CS.limit enforced BEFORE
 * translating. Refuse near wrap/limit boundaries rather than guess at a
 * partially fetched 16-bit instruction. All accesses use the checked fetch. */
static B86_PJ_COLD unsigned pj_fetch_pm16(B86Cpu *c, uint32_t eip,
                                            uint8_t *bytes, unsigned max)
{
    const B86PM1Cache *cs = &c->pm386.seg[B86_CS];
    const uint64_t lin = (uint64_t)cs->base + eip;
    uint32_t n = max;
    if (eip > 0xFFFFu || eip > cs->limit || lin > UINT32_MAX) return 0u;
    if (n > 0x10000u - eip) n = 0x10000u - eip;
    if (cs->limit - eip < n - 1u) n = cs->limit - eip + 1u;
    if ((uint64_t)n > (uint64_t)UINT32_MAX - lin + 1u)
        n = (unsigned)((uint64_t)UINT32_MAX - lin + 1u);
    return pj_fetch(c, (uint32_t)lin, bytes, n);
}

/* Only simple 16-bit register operations and relative control transfers.
 * No 66/67, segment override, LOCK/REP, stack or memory form is accepted.
 * The checked interpreter handles everything else. */
static B86_PJ_COLD int pjt_insn_pm16(PjT *t)
{
    uint32_t op, modrm, imm;
    const unsigned start = t->pos, ex0 = t->nex;
    uint8_t *const p0 = t->e.p;
    const int lk0 = t->last_kind, ik0 = t->ikind_clear, cr0 = t->creg_ok;
    const unsigned base_ip = t->ip0;
    unsigned w, alu, form, a, b;
#define PJ16_FAIL() do { BB_PM16_DIAG_INC(emit_fail); t->pos=start; --t->ninsn; t->nex=ex0; \
    t->e.p=p0; t->last_kind=lk0; t->ikind_clear=ik0; t->creg_ok=cr0; return 0; } while(0)
#define PJ16_NEXT() ((base_ip + (t->pos - start)) & 0xFFFFu)
    BB_PM16_DIAG_INC(emit_attempt);
    ++t->ninsn;
    if (!pjt_u8(t, &op)) PJ16_FAIL();
    /* Count the opcode AFTER 66; these are decoder translation attempts,
     * NOT executed-instruction counts. Keep separate from runtime helpers. */
    if (op == 0x66u) {
        const unsigned x = t->pos < t->n ? t->b[t->pos] : 0xFFu;
        BB_PM16_DIAG_INC(pref66);
        if (x >= 0x88u && x <= 0x8Bu) BB_PM16_DIAG_INC(pref66_mov);
        else if (x >= 0x50u && x <= 0x5Fu) BB_PM16_DIAG_INC(pref66_stack);
        else if (x == 0x0Fu) BB_PM16_DIAG_INC(pref66_0f);
        else BB_PM16_DIAG_INC(pref66_other);
    }

#if BB_PJ_PM16_OP66REG
    /* In 16-bit code, 66 changes operand size to 32. This opt-in path
     * accepts only forms with ModRM.mod=3 (registers), or imm32 to GPR.
     * On any decoding failure PJ16_FAIL completely discards emitted code. */
    if (op == 0x66u) {
        uint32_t op66, mr, imm66;
        unsigned a66, b66, alu66, form66;
        if (!pjt_u8(t, &op66)) PJ16_FAIL();
        if (op66 == 0x90u) do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0); /* XCHG EAX,EAX */
        if (op66 >= 0xB8u && op66 <= 0xBFu) {
            if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
            pj_imm32(&t->e, R1, imm66);
            pjt_putr(t, op66 & 7u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 >= 0x40u && op66 <= 0x4Fu) {
            pjt_getr(t, R2, op66 & 7u, 4u);
            if (op66 < 0x48u) pj_dpi(&t->e, 8u, 0, R3, R2, 1u);
            else pj_dpi(&t->e, 13u, 0, R3, R2, 1u);
            pjt_putr(t, op66 & 7u, 4u, R3);
            pjt_lazy(t, op66 < 0x48u ? pj_k_inc[4] : pj_k_dec[4], R2, R3);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 >= 0x91u && op66 <= 0x97u) {
            pjt_getr(t, R1, 0u, 4u);
            pjt_getr(t, R2, op66 & 7u, 4u);
            pjt_putr(t, 0u, 4u, R2);
            pjt_putr(t, op66 & 7u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 == 0x89u || op66 == 0x8Bu) {
            if (!pjt_u8(t, &mr) || (mr >> 6) != 3u) PJ16_FAIL();
            a66 = (mr >> 3) & 7u; b66 = mr & 7u;
            if (op66 == 0x8Bu) {
                pjt_getr(t, R1, b66, 4u); pjt_putr(t, a66, 4u, R1);
            } else {
                pjt_getr(t, R1, a66, 4u); pjt_putr(t, b66, 4u, R1);
            }
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 < 0x40u && ((op66 & 7u) == 1u || (op66 & 7u) == 3u)) {
            alu66 = op66 >> 3; form66 = op66 & 7u;
            if (alu66 != 0u && alu66 != 1u && alu66 != 4u &&
                alu66 != 5u && alu66 != 6u && alu66 != 7u) PJ16_FAIL();
            if (!pjt_u8(t, &mr) || (mr >> 6) != 3u) PJ16_FAIL();
            a66 = (mr >> 3) & 7u; b66 = mr & 7u;
            if (form66 == 3u) {
                pjt_getr(t, R0, a66, 4u); pjt_getr(t, R1, b66, 4u);
                if (!pjt_alu(t, alu66, 4u, alu66 == 7u ? R3 : R0, R0, R1)) PJ16_FAIL();
                if (alu66 != 7u) pjt_putr(t, a66, 4u, R0);
            } else {
                pjt_getr(t, R1, b66, 4u); pjt_getr(t, R0, a66, 4u);
                if (!pjt_alu(t, alu66, 4u, alu66 == 7u ? R3 : R1, R1, R0)) PJ16_FAIL();
                if (alu66 != 7u) pjt_putr(t, b66, 4u, R1);
            }
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 == 0x81u || op66 == 0x83u) {
            if (!pjt_u8(t, &mr) || (mr >> 6) != 3u) PJ16_FAIL();
            alu66 = (mr >> 3) & 7u; b66 = mr & 7u;
            if (alu66 != 0u && alu66 != 1u && alu66 != 4u &&
                alu66 != 5u && alu66 != 6u && alu66 != 7u) PJ16_FAIL();
            if (op66 == 0x83u) {
                if (!pjt_u8(t, &imm66)) PJ16_FAIL();
                imm66 = (uint32_t)(int32_t)(int8_t)imm66;
            } else if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
            pjt_getr(t, R1, b66, 4u);
            pj_imm32(&t->e, R0, imm66);
            if (!pjt_alu(t, alu66, 4u, alu66 == 7u ? R3 : R1, R1, R0)) PJ16_FAIL();
            if (alu66 != 7u) pjt_putr(t, b66, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 == 0x85u || op66 == 0xA9u) {
            if (op66 == 0x85u) {
                if (!pjt_u8(t, &mr) || (mr >> 6) != 3u) PJ16_FAIL();
                pjt_getr(t, R1, mr & 7u, 4u);
                pjt_getr(t, R0, (mr >> 3) & 7u, 4u);
            } else {
                if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
                pjt_getr(t, R1, 0u, 4u);
                pj_imm32(&t->e, R0, imm66);
            }
            if (!pjt_alu(t, 4u, 4u, R3, R1, R0)) PJ16_FAIL();
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if ((op66 & 7u) == 5u && op66 <= 0x3Du) {
            alu66 = op66 >> 3;
            if (alu66 != 0u && alu66 != 1u && alu66 != 4u &&
                alu66 != 5u && alu66 != 6u && alu66 != 7u) PJ16_FAIL();
            if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
            pjt_getr(t, R1, 0u, 4u);
            pj_imm32(&t->e, R0, imm66);
            if (!pjt_alu(t, alu66, 4u, alu66 == 7u ? R3 : R1, R1, R0)) PJ16_FAIL();
            if (alu66 != 7u) pjt_putr(t, 0u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 == 0xC7u) { /* MOV r/m32, imm32: /0, register form only */
            if (!pjt_u8(t, &mr) || (mr >> 6) != 3u || ((mr >> 3) & 7u)) PJ16_FAIL();
            if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
            pj_imm32(&t->e, R1, imm66); pjt_putr(t, mr & 7u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        PJ16_FAIL();
    }
#endif /* BB_PJ_PM16_OP66REG */
    /* Other prefixes and unsupported 66 forms decline naturally. */
    if (op == 0x90u) do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0); /* NOP */
    if (op >= 0xB8u && op <= 0xBFu) {
        if (!pjt_imm(t, 2u, &imm)) PJ16_FAIL();
        pj_movw(&t->e, R1, imm);
        pjt_putr(t, op & 7u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    if (op >= 0x40u && op <= 0x4Fu) {
        pjt_getr(t, R2, op & 7u, 2u);
        if (op < 0x48u) pj_dpi(&t->e, 8u, 0, R3, R2, 1u);
        else pj_dpi(&t->e, 13u, 0, R3, R2, 1u);
        pjt_putr(t, op & 7u, 2u, R3);
        pjt_lazy(t, op < 0x48u ? pj_k_inc[2] : pj_k_dec[2], R2, R3);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    if (op >= 0x91u && op <= 0x97u) {
        pjt_getr(t, R1, 0u, 2u);
        pjt_getr(t, R2, op & 7u, 2u);
        pjt_putr(t, 0u, 2u, R2);
        pjt_putr(t, op & 7u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#if BB_PJ_PM16_MOV_WRITE
    /* MOV [EA16],reg8/reg16 (88/89) ONLY. No prefixes, no guest stack,
     * no DS/ES override. A refusal executes the exact old C instruction. */
    if ((op == 0x88u || op == 0x89u) && t->pos < t->n &&
        (t->b[t->pos] >> 6) != 3u) {
        uint32_t bmod, disp = 0u, width = op == 0x89u ? 2u : 1u;
        unsigned mod, rm, reg, seg, base = 255u, index = 255u;
        if (!pjt_u8(t, &bmod)) PJ16_FAIL();
        mod = bmod >> 6; rm = bmod & 7u; reg = (bmod >> 3) & 7u;
        seg = ((rm == 2u || rm == 3u) || (rm == 6u && mod != 0u)) ? B86_SS : B86_DS;
        if (mod == 0u && rm == 6u) {
            if (!pjt_imm(t, 2u, &disp)) PJ16_FAIL();
            pj_movw(&t->e, R1, disp);
        } else {
            switch (rm) {
            case 0u: base=B86_BX; index=B86_SI; break;
            case 1u: base=B86_BX; index=B86_DI; break;
            case 2u: base=B86_BP; index=B86_SI; break;
            case 3u: base=B86_BP; index=B86_DI; break;
            case 4u: base=B86_SI; break;
            case 5u: base=B86_DI; break;
            case 6u: base=B86_BP; break;
            default: base=B86_BX; break;
            }
            pjt_getr(t, R1, base, 2u);
            if (index != 255u) {
                pjt_getr(t, R2, index, 2u);
                pj_dpr(&t->e, 8u, 0, R1, R1, R2, 0u, 0u);
            }
            if (mod == 1u) {
                if (!pjt_u8(t, &disp)) PJ16_FAIL();
                disp = (uint32_t)(int32_t)(int8_t)disp;
            } else if (mod == 2u) {
                if (!pjt_imm(t, 2u, &disp)) PJ16_FAIL();
            }
            pjt_addimm(t, R1, R1, disp);
            pj_ubfx(&t->e, R1, R1, 0u, 16u);
        }
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, width | (seg << 8));
        pjt_getr(t, R3, reg, width);
        pjt_call(t, (const void *)b86_pj_pm16_write);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u); /* CMP success, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        /* End the block after ANY successful write: SMC and page validity. */
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* BB_PJ_PM16_MOV_WRITE */
#if BB_PJ_PM16_MOV_READ
    /* MOV reg8/reg16, [EA16] ONLY (8A/8B, ModRM.mod != 3).
     * Operand segment is resolved at execution time, not baked into code.
     * No segment prefix, memory write, stack op, or 32-bit operand here. */
    if ((op == 0x8Au || op == 0x8Bu) && t->pos < t->n &&
        (t->b[t->pos] >> 6) != 3u) {
        uint32_t bmod, disp = 0u, width = op == 0x8Bu ? 2u : 1u;
        unsigned mod, rm, reg, seg, base = 255u, index = 255u;
        if (!pjt_u8(t, &bmod)) PJ16_FAIL();
        mod = bmod >> 6; rm = bmod & 7u; reg = (bmod >> 3) & 7u;
        seg = ((rm == 2u || rm == 3u) || (rm == 6u && mod != 0u)) ? B86_SS : B86_DS;
        if (mod == 0u && rm == 6u) {
            if (!pjt_imm(t, 2u, &disp)) PJ16_FAIL();
            pj_movw(&t->e, R1, disp);
        } else {
            switch (rm) {
            case 0u: base=B86_BX; index=B86_SI; break;
            case 1u: base=B86_BX; index=B86_DI; break;
            case 2u: base=B86_BP; index=B86_SI; break;
            case 3u: base=B86_BP; index=B86_DI; break;
            case 4u: base=B86_SI; break;
            case 5u: base=B86_DI; break;
            case 6u: base=B86_BP; break;
            default: base=B86_BX; break;
            }
            pjt_getr(t, R1, base, 2u);
            if (index != 255u) {
                pjt_getr(t, R2, index, 2u);
                pj_dpr(&t->e, 8u, 0, R1, R1, R2, 0u, 0u);
            }
            if (mod == 1u) {
                if (!pjt_u8(t, &disp)) PJ16_FAIL();
                disp = (uint32_t)(int32_t)(int8_t)disp;
            } else if (mod == 2u) {
                if (!pjt_imm(t, 2u, &disp)) PJ16_FAIL();
            }
            pjt_addimm(t, R1, R1, disp);
            pj_ubfx(&t->e, R1, R1, 0u, 16u);
        }
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, width | (seg << 8));
        pjt_call(t, (const void *)b86_pj_pm16_read_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);  /* CMP pointer, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pjt_ld(t, R1, R0, width);
        pjt_putr(t, reg, width, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#endif /* BB_PJ_PM16_MOV_READ */
    /* Exact register-only MOV (including 8-bit AH/BH etc). */
    if (op >= 0x88u && op <= 0x8Bu) {
        if (!pjt_u8(t, &modrm) || (modrm >> 6) != 3u) PJ16_FAIL();
        w = (op & 1u) ? 2u : 1u;
        a = (modrm >> 3) & 7u; b = modrm & 7u;
        if (op & 2u) { pjt_getr(t, R1, b, w); pjt_putr(t, a, w, R1); }
        else { pjt_getr(t, R1, a, w); pjt_putr(t, b, w, R1); }
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* ADD/OR/AND/SUB/XOR/CMP r8/r16, r8/r16: ModRM.mod=3 only. */
    if (op < 0x40u && (op & 7u) < 4u) {
        alu = op >> 3; form = op & 7u;
        if (alu != 0u && alu != 1u && alu != 4u &&
            alu != 5u && alu != 6u && alu != 7u) PJ16_FAIL();
        if (!pjt_u8(t, &modrm) || (modrm >> 6) != 3u) PJ16_FAIL();
        w = (form & 1u) ? 2u : 1u;
        a = (modrm >> 3) & 7u; b = modrm & 7u;
        if (form & 2u) { /* reg <- reg op r/m */
            pjt_getr(t, R0, a, w); pjt_getr(t, R1, b, w);
            if (!pjt_alu(t, alu, w, alu == 7u ? R3 : R0, R0, R1)) PJ16_FAIL();
            if (alu != 7u) pjt_putr(t, a, w, R0);
        } else { /* r/m <- r/m op reg */
            pjt_getr(t, R1, b, w); pjt_getr(t, R0, a, w);
            if (!pjt_alu(t, alu, w, alu == 7u ? R3 : R1, R1, R0)) PJ16_FAIL();
            if (alu != 7u) pjt_putr(t, b, w, R1);
        }
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    if (op == 0x80u || op == 0x81u || op == 0x83u) {
        if (!pjt_u8(t, &modrm) || (modrm >> 6) != 3u) PJ16_FAIL();
        alu = (modrm >> 3) & 7u; b = modrm & 7u;
        if (alu != 0u && alu != 1u && alu != 4u &&
            alu != 5u && alu != 6u && alu != 7u) PJ16_FAIL();
        w = op == 0x80u ? 1u : 2u;
        if (op == 0x83u) {
            if (!pjt_u8(t, &imm)) PJ16_FAIL();
            imm = (uint32_t)(int32_t)(int8_t)imm;
        } else if (!pjt_imm(t, w, &imm)) PJ16_FAIL();
        pjt_getr(t, R1, b, w);
        pj_imm32(&t->e, R0, imm);
        if (!pjt_alu(t, alu, w, alu == 7u ? R3 : R1, R1, R0)) PJ16_FAIL();
        if (alu != 7u) pjt_putr(t, b, w, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    if (op == 0x84u || op == 0x85u) {
        if (!pjt_u8(t, &modrm) || (modrm >> 6) != 3u) PJ16_FAIL();
        w = (op & 1u) ? 2u : 1u;
        pjt_getr(t, R1, modrm & 7u, w);
        pjt_getr(t, R0, (modrm >> 3) & 7u, w);
        if (!pjt_alu(t, 4u, w, R3, R1, R0)) PJ16_FAIL();
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* All control transfers return through the unlinked C dispatcher. */
    if (op == 0xEBu || op == 0xE9u) {
        if (op == 0xEBu) {
            if (!pjt_u8(t, &imm)) PJ16_FAIL();
            imm = (uint32_t)(int32_t)(int8_t)imm;
        } else {
            if (!pjt_imm(t, 2u, &imm)) PJ16_FAIL();
            imm = (uint32_t)(int32_t)(int16_t)imm;
        }
        pjt_exit(t, (PJ16_NEXT() + imm) & 0xFFFFu, t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
    if (op >= 0x70u && op <= 0x7Fu) {
        int arm;
        if (!pjt_u8(t, &imm)) PJ16_FAIL();
        imm = (uint32_t)(int32_t)(int8_t)imm;
        arm = pjt_flags_for(t, op & 15u);
        pjt_exit_cond(t, arm, (PJ16_NEXT() + imm) & 0xFFFFu, t->ninsn);
        /* dyn=2 returns unlinked and uses the specified fallthrough EIP. */
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
    PJ16_FAIL();
#undef PJ16_FAIL
#undef PJ16_NEXT
}

/* A nonflat 16-bit code segment is eligible for the register tier even if
 * DS, ES, SS are nonflat. No generated code accesses those segments. */
static inline int pj_pm16_reg_ok(const B86Cpu *c)
{
    const B86PM1Cache *cs = &c->pm386.seg[B86_CS];
    return (c->msw & 1u) && !(c->flags & (0x20000u | B86_TF)) &&
           cs->valid && !cs->reserved && !(cs->flags & 4u) &&
           (cs->access & 0x18u) == 0x18u && c->ip <= 0xFFFFu &&
           c->ip <= cs->limit && (uint64_t)cs->base + c->ip <= UINT32_MAX;
}
#endif /* BB_PJ_PM16_REG_TIER */

static B86_PJ_COLD uint32_t b86_pj_translate(B86Cpu *c, uint32_t eip)
{
    B86Pj *j = b86_pj_state(c);
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    /* Single CPU owns PJ translation on the Pico. Cache-building can invoke
     * pj_emit_shared while this function is active; keeping both workspaces
     * off the 4 KiB scratch stack avoids that nested stack pressure. */
    static uint8_t bytes[192];
    static PjT t;
#else
    uint8_t bytes[192];
    PjT t;
#endif
    int r = 1;
    unsigned i;
    uint32_t start_off;
    if (j->cap - j->used < 4096u) { b86_pj_flush(c); ++j->flush_full; }
    memset(&t, 0, sizeof t);
    t.c = c;
#if BB_PJ_PM16_REG_TIER
    t.n = j->pm16_active ? pj_fetch_pm16(c, eip, bytes, sizeof bytes)
                          : pj_fetch(c, eip, bytes, sizeof bytes);
#else
    t.n = pj_fetch(c, eip, bytes, sizeof bytes);
#endif
    t.b = bytes;
    t.last_kind = -1;
    start_off = (j->used + 3u) & ~3u;
    t.e.p = j->code + start_off; t.e.end = j->code + j->cap;
    while (t.ninsn < B86_PJ_MAXI && t.pos + 16u <= t.n) {
#if BB_PJ_PM16_REG_TIER
        t.ip0 = j->pm16_active ? ((eip + t.pos) & 0xFFFFu) : eip + t.pos;
        r = j->pm16_active ? pjt_insn_pm16(&t) : pjt_insn(&t);
#else
        t.ip0 = eip + t.pos;
        r = pjt_insn(&t);
#endif
        if (r != 1) break;
    }
    if (r == 1 || r == 0) {                       /* fall into the next instruction */
        if (r == 0 && t.pos < t.n) {
            /* BB_PJ_TLB_0F_V1: classify the 0F second opcode byte when
             * the original translator declines an instruction, including
             * 66/segment/REP-prefixed instructions. Never count successes. */
            unsigned k = t.pos, pfx = 0u;
            ++j->stop_op[t.b[t.pos]];
            while (k < t.n && pfx < 5u) {
                const uint8_t b = t.b[k];
                if (b == 0x66u || b == 0x67u || b == 0xF0u ||
                    b == 0xF2u || b == 0xF3u || b == 0x26u ||
                    b == 0x2Eu || b == 0x36u || b == 0x3Eu ||
                    b == 0x64u || b == 0x65u) { ++pfx; ++k; continue; }
                if (b == 0x0Fu && k + 1u < t.n) ++j->stop_0f[t.b[k + 1u]];
                break;
            }
        }
#if BB_PJ_PM16_REG_TIER
        pjt_exit(&t, j->pm16_active ? ((eip + t.pos) & 0xFFFFu) : eip + t.pos,
                 t.ninsn, j->pm16_active ? 2 : 0);
#else
        pjt_exit(&t, eip + t.pos, t.ninsn, 0);
#endif
    }
    if (t.ninsn == 0u || t.e.bad) return 0u;
    for (i = 0; i < t.nex; ++i) pjt_stub(&t, &t.ex[i], j);
    if (t.e.bad) return 0u;
    for (i = 0; i < t.pos; i += 1u) {             /* mark code pages */
#if BB_PJ_PM16_REG_TIER
        const uint32_t lin = (j->pm16_active ? j->pm16_base : 0u) + eip + i;
        const uint32_t pg = lin >> 12, h = PJ_PGH(pg);
        if (i && pg == ((lin - 1u) >> 12)) continue;
#else
        const uint32_t pg = (eip + i) >> 12, h = PJ_PGH(pg);
        if (i && pg == ((eip + i - 1u) >> 12)) continue;
#endif
        j->cpg[h >> 5] |= 1u << (h & 31u);
        if (!pj_cps_add(j, pg)) { ++j->profile_cps_refusals; b86_pj_flush(c); return 0u; }
        /* Stage4U: mark the same source bytes by physical code page, too.
         * If the mapping disappeared, refuse this block rather than cache
         * native code without alias tracking. */
        {
            uint32_t phys, pkey, ph;
#if BB_PJ_PM16_REG_TIER
            const uint32_t source_linear=lin;
#else
            const uint32_t source_linear=eip+i;
#endif
            if(c->pg){
                if(!b86_pg_walk(c,source_linear,&phys)){
                    b86_pj_flush(c);return 0u;
                }
            }else phys=source_linear;
            if(phys>=B86_RAM_BYTES){b86_pj_flush(c);return 0u;}
            pkey=0x40000000u | (phys>>12);
            ph=PJ_PGH(pkey);
            j->cpg[ph>>5]|=1u<<(ph&31u);
            if(!pj_cps_add(j,pkey)){++j->profile_cps_refusals;b86_pj_flush(c);return 0u;}
        }
    }
    j->used = (uint32_t)(t.e.p - j->code);
#if defined(__linux__)
    __builtin___clear_cache((char *)j->code + start_off, (char *)t.e.p);
#else
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");      /* Cortex-M: no I-cache on SRAM */
#endif
    ++j->blocks;
    return start_off + 1u;
}

/* Flat-model check for PJ entry. */
static inline int pj_flat(const B86Cpu *c)
{
    const B86PM1Cache *s = c->pm386.seg;
    return (s[B86_CS].flags & 4u) && !s[B86_CS].reserved && s[B86_CS].base == 0u && s[B86_CS].limit == 0xFFFFFFFFu &&
           s[B86_DS].valid && s[B86_DS].base == 0u && s[B86_DS].limit == 0xFFFFFFFFu && (s[B86_DS].access & 0x1Au) == 0x12u &&
                  s[B86_SS].valid && s[B86_SS].base == 0u && s[B86_SS].limit == 0xFFFFFFFFu && (s[B86_SS].flags & 4u) &&
           !s[B86_SS].reserved && (s[B86_SS].access & 0x1Au) == 0x12u && !(c->flags & 0x20000u);
}

/* BB_PJ_ES_INDEPENDENT_V1: only the translator's strict mode requires ES
 * to be flat. Relaxed-mode blocks cannot use or mutate ES; they are never
 * shared with strict-mode blocks (cache flushes on mode transitions). */
static inline int pj_es_flat(const B86Cpu *c)
{
    const B86PM1Cache *s = c->pm386.seg;
    return s[B86_ES].valid && s[B86_ES].base == 0u &&
           s[B86_ES].limit == UINT32_MAX &&
           (s[B86_ES].access & 0x1Au) == 0x12u;
}

/* ES-dependent instruction guard. Called from emitted Thumb-2 before
 * the guest instruction has any side effects. Denied instruction remains
 * at original EIP, for the checked C interpreter to execute exactly once. */
static B86_PM_HOT uint32_t b86_pj_es_runtime_check(B86Cpu *c, uint32_t eip)
{
    B86Pj *j = b86_pj_state(c);
    if (pj_es_flat(c)) return 1u;
    if (j) {
        ++j->es_guarded_ops;
        /* Never record the reserved zero tag. The C instruction remains
         * responsible for descriptor validation and all guest side effects. */
        if (eip != UINT32_MAX) {
#if B86_PJ_ES_SKIP_WAYS == 1
            /* Original direct-mapped 64-site behavior; A/B control. */
            const unsigned h = ((eip * 2654435761u) >> 26) & 63u;
            j->es_skip_eip[h] = eip + 1u;
#else
            /* 32 buckets x 2 ways, MRU in slot 0; same 256-byte arena.
             * A guard rejection has no guest-visible effects. */
            const unsigned h = (((eip * 2654435761u) >> 27) & 31u) * 2u;
            const uint32_t tag = eip + 1u;
            if (j->es_skip_eip[h] != tag) {
                j->es_skip_eip[h + 1u] = j->es_skip_eip[h];
                j->es_skip_eip[h] = tag;
            }
#endif
            ++j->es_skip_marks;
        }
    }
    return 0u;
}

/* Patch the static exit the last run left by into a direct jump to the
 * target block's chain entry (translating the target if needed). */
static B86_PJ_COLD void b86_pj_link(B86Cpu *c)
{
    B86Pj *j = b86_pj_state(c);
    uint8_t *stub = (uint8_t *)((c->pj_exit & ~(uintptr_t)1u) - 16u);   /* LR after BL tail at +12 */
    const unsigned long gen = j->flushes;
    uint32_t h = ((c->ip * 2654435761u) >> 16) & (B86_PJ_HASH - 1u), ent;
    c->pj_exit = 0u;
    if (j->flush_pending || stub < j->code || stub + 20u > j->code + j->used) return;
    if (j->tag[h] != (c->ip | 1u)) {
        j->tag[h] = c->ip | 1u;
        j->ent[h] = b86_pj_translate(c, c->ip);
        if (!j->ent[h]) ++j->declines;
    }
    ent = j->ent[h];
    if (!ent || j->flushes != gen) return;           /* translation flushed the cache: stub is gone */
    {
        /* +12: BL chk; +16: B.W target body */
        stub[12] = 0x00u; stub[13] = 0xF0u; stub[14] = 0x00u; stub[15] = 0x90u;
        pj_patch_b(stub + 12, j->chk); stub[15] |= 0x40u;          /* B.W -> BL */
        stub[16] = 0x00u; stub[17] = 0xF0u; stub[18] = 0x00u; stub[19] = 0x90u;
        pj_patch_b(stub + 16, j->code + ent - 1u);
        ++j->links;
#if defined(__linux__)
        __builtin___clear_cache((char *)stub, (char *)stub + 20);
#else
        __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    }
}

/* BB_STAGE5I_OPCODE_PROBE: one-time read-only capture of the exact full-EIP
 * hot paths observed during Stage5H run2. Uses normal JIT fetch, not the
 * 16-bit real-mode watch physical-address conversion. Flat CS only. */
static void bb5i_capture_hot_opcodes(B86Cpu *c)
{
    static const uint32_t hot[] = {
        0x100C8C6Cu, 0x100C8C6Du, 0x100C9B8Cu, 0x100C9B8Du,
        0x100DFF35u, 0x100DFD7Au, 0x100DFC98u, 0x100DFCA9u
    };
    static uint32_t emitted;
    unsigned i;
    for (i = 0u; i < sizeof hot / sizeof hot[0]; ++i) {
        uint8_t buf[24];
        unsigned n, x;
        if (c->ip != hot[i] || (emitted & (1u << i))) continue;
        /* pj_flat was already established by the caller, hence page/segment
         * validity is unchanged. In particular this never reads IO regions. */
        n = pj_fetch(c, c->ip, buf, sizeof buf);
        emitted |= 1u << i;
        printf("[bb5i-code] eip=%08lX base=%08lX size=%u bytes=",
               (unsigned long)c->ip,
               (unsigned long)c->pm386.seg[B86_CS].base, n);
        for (x = 0u; x < n; ++x) printf("%02X", (unsigned)buf[x]);
        printf("\n");
        return;
    }
}
/* BB_STAGE5I_OPCODE_PROBE_END */
/* BB_STAGE5H_DISPATCH_PROFILE: observation only; samples one in 2048 JIT
 * entry attempts and one in 128 translation misses. The 32-entry SpaceSaving
 * table tracks entry-point frequency, NOT time inside linked native loops.
 * Nothing here changes cache tags, translation, guest state or native code. */
#define BB5H_SLOTS 32u
static struct Bb5HHot { uint32_t eip, base, mode, count, misses; } bb5h_hot[BB5H_SLOTS];
static uint32_t bb5h_try_clock, bb5h_miss_clock;
static unsigned long bb5h_try_samples, bb5h_miss_samples, bb5h_last_block_report;
static void bb5h_note(const B86Cpu *c, uint32_t mode, unsigned miss)
{
    unsigned i, free_slot = BB5H_SLOTS, min_slot = 0u;
    const uint32_t ip = c->ip, base = c->pm386.seg[B86_CS].base;
    for (i = 0u; i < BB5H_SLOTS; ++i) {
        if (bb5h_hot[i].count && bb5h_hot[i].eip == ip &&
            bb5h_hot[i].base == base && bb5h_hot[i].mode == mode) {
            ++bb5h_hot[i].count;
            bb5h_hot[i].misses += (uint32_t)!!miss;
            return;
        }
        if (!bb5h_hot[i].count && free_slot == BB5H_SLOTS) free_slot = i;
        if (bb5h_hot[i].count < bb5h_hot[min_slot].count) min_slot = i;
    }
    i = free_slot != BB5H_SLOTS ? free_slot : min_slot;
    { const uint32_t prior = bb5h_hot[i].count;
      bb5h_hot[i].eip = ip;
      bb5h_hot[i].base = base;
      bb5h_hot[i].mode = mode;
      bb5h_hot[i].count = prior + 1u;
      bb5h_hot[i].misses = (uint32_t)!!miss;
    }
}
static void bb5h_dump(void)
{
    unsigned r, i;
    uint32_t seen = 0u;
    printf("[bb5h-summary] entry-samples=%lu miss-samples=%lu one-entry-in=2048 one-miss-in=128\n",
           bb5h_try_samples, bb5h_miss_samples);
    for (r=0u; r<6u; ++r) {
        unsigned best = BB5H_SLOTS;
        for (i=0u; i<BB5H_SLOTS; ++i) {
            if ((seen & (1u << i)) || !bb5h_hot[i].count) continue;
            if (best == BB5H_SLOTS || bb5h_hot[i].count > bb5h_hot[best].count) best = i;
        }
        if (best == BB5H_SLOTS) break;
        seen |= (1u << best);
        printf("[bb5h-hot] rank=%u mode=%lu base=%08lX eip=%08lX linear=%08lX samples=%lu missed=%lu\n",
               r+1u, (unsigned long)bb5h_hot[best].mode,
               (unsigned long)bb5h_hot[best].base, (unsigned long)bb5h_hot[best].eip,
               (unsigned long)(bb5h_hot[best].base+bb5h_hot[best].eip),
               (unsigned long)bb5h_hot[best].count,
               (unsigned long)bb5h_hot[best].misses);
    }
}
/* BB_STAGE5H_DISPATCH_PROFILE_END */

/* Run one native block at EIP if possible. Returns retired instructions (0 = none). */
static B86_PM_HOT unsigned b86_pj_try(B86Cpu *c)
{
    B86Pj *j = b86_pj_state(c);
    uint32_t h, ent;
    unsigned n;
#if BB_PJ_PM16_REG_TIER
    {
        const unsigned flat = j && pj_flat(c);
        const unsigned reg16 = BB_PJ_PM16_PERF_GATE && j && !flat && pj_pm16_reg_ok(c);
        const B86PM1Cache *cs = &c->pm386.seg[B86_CS];
        const uint32_t kind = (uint32_t)cs->selector | ((uint32_t)cs->flags << 16) |
                              ((uint32_t)cs->access << 24);
        /* Sample before early deoptimization, so unsupported PM16 visits
         * can be identified without affecting their execution. */
        if (((++bb5h_try_clock) & 2047u) == 0u) {
            ++bb5h_try_samples;
            bb5h_note(c, reg16 ? 16u : (flat ? 32u : 0u), 0u);
            if (flat) bb5i_capture_hot_opcodes(c);
        }
        if (!flat && !reg16) return 0u;
#if BB_PJ_PM16_ADAPTIVE
        /* Skip BEFORE the PM16 cache/mode transition, translation and
         * potential full flush. This deoptimizes to the checked interpreter.
         * Even if the guest changes code while skipped, no native code is
         * entered; the next sampled entry repeats normal PJ validity checks. */
        if (reg16 && j->pm16_adapt_cooldown) {
            --j->pm16_adapt_cooldown;
            ++j->pm16_adapt_skips;
            return 0u;
        }
#endif
        if (reg16) {
            if (!j->pm16_active || j->pm16_base != cs->base ||
                j->pm16_limit != cs->limit || j->pm16_kind != kind) {
                b86_pj_flush(c);
                j->es_mode = 0u;
            }
            j->pm16_active = 1u;
            j->pm16_base = cs->base;
            j->pm16_limit = cs->limit;
            j->pm16_kind = kind;
        } else if (j->pm16_active) {
            b86_pj_flush(c);
            j->pm16_active = 0u;
            j->es_mode = 0u;
        }
    }
#else
    if (!j || !pj_flat(c)) return 0u;
#endif
    /* No cache flush on ES transitions. EVERY compiled ES-sensitive form
     * now emits a runtime check; linked blocks use the same guards. */
    {
#if BB_PJ_PM16_REG_TIER
        const unsigned mode = j->pm16_active ? 0u : (pj_es_flat(c) ? 1u : 2u);
#else
        const unsigned mode = pj_es_flat(c) ? 1u : 2u;
#endif
        if (j->es_mode && j->es_mode != mode) ++j->es_mode_switches;
        j->es_mode = mode;
        if (mode == 2u) ++j->es_relaxed_entries;
    }
    if (j->flush_pending) { b86_pj_flush(c); ++j->flush_smc; }
    /* After an ES-dependent instruction refuses native execution, the
     * exact EIP is run directly in C on later non-flat ES visits. This
     * avoids millions of zero-retirement native entry/exit roundtrips. */
    if (j->es_mode == 2u) {
        ++j->es_skip_checks;
        /* The reserved tag zero is never recorded, and never matches. */
        if (c->ip != UINT32_MAX) {
#if B86_PJ_ES_SKIP_WAYS == 1
            const unsigned sh = ((c->ip * 2654435761u) >> 26) & 63u;
            if (j->es_skip_eip[sh] == c->ip + 1u) {
                ++j->es_skip_hits;
                return 0u;
            }
#else
            const unsigned sh = (((c->ip * 2654435761u) >> 27) & 31u) * 2u;
            const uint32_t tag = c->ip + 1u;
            if (j->es_skip_eip[sh] == tag) {
                ++j->es_skip_hits;
                return 0u;
            }
            if (j->es_skip_eip[sh + 1u] == tag) {
                /* Maintain MRU ordering for a two-way LRU approximation.
                 * No invalidation or translation is performed here. */
                j->es_skip_eip[sh + 1u] = j->es_skip_eip[sh];
                j->es_skip_eip[sh] = tag;
                ++j->es_skip_hits;
                return 0u;
            }
#endif
        }
    }
    h = ((c->ip * 2654435761u) >> 16) & (B86_PJ_HASH - 1u);
    if (j->tag[h] != (c->ip | 1u)) {
        ++j->profile_lookup_misses;
        if (((++bb5h_miss_clock) & 127u) == 0u) {
            ++bb5h_miss_samples;
            bb5h_note(c, j->pm16_active ? 16u : 32u, 1u);
        }

        if (j->tag[h]) ++j->profile_slot_replacements;
/* BB_PJ_PM16_NEGCACHE_V1: check persistent negative key only on a
     * miss in the ordinary EIP cache. Linear keys eliminate most CS-base
     * aliasing. A false/stale hit only runs the checked interpreter. */
    if (j->pm16_active) {
        const uint32_t pm16_lin = j->pm16_base + c->ip;
        const uint32_t pm16_slot = (pm16_lin * 2654435761u) >> 23;
        const uint32_t *const pm16_neg =
            (const uint32_t *)(const void *)(j->code + j->cap);
        if (pm16_neg[pm16_slot] == pm16_lin + 1u) {
            ++j->pm16_neg_hits;
            return 0u;
        }
    }

        j->tag[h] = c->ip | 1u;
        j->ent[h] = b86_pj_translate(c, c->ip);
        if (!j->ent[h]) ++j->declines;
/* BB_PJ_PM16_NEGCACHE_V1: record only a failed PM16 translation.
         * It is safe to remember this across code and descriptor changes:
         * later hits always interpret instead of executing old native code. */
        if (!j->ent[h] && j->pm16_active) {
            const uint32_t pm16_lin = j->pm16_base + c->ip;
            uint32_t *const pm16_neg =
                (uint32_t *)(void *)(j->code + j->cap);
            pm16_neg[(pm16_lin * 2654435761u) >> 23] = pm16_lin + 1u;
            ++j->pm16_neg_inserts;
        }

    }
    ent = j->ent[h];
    if (!ent) return 0u;
#ifdef B86_PJ_VERIFY
    uint32_t pr[8], pip = c->ip, pfl = c->flags, plz[7];
    uint64_t pic = c->icount;
    memcpy(pr, c->r, sizeof pr);
    plz[0] = c->lz_kind; plz[1] = c->lz_a; plz[2] = c->lz_res; plz[3] = c->lz_b; plz[4] = c->lz_ikind; plz[5] = c->lz_ia; plz[6] = c->lz_ires;
    pj_nvl = 0u;
#endif
    c->pj_exit = 0u;
    n = ((unsigned (*)(B86Cpu *, uintptr_t))(void *)(uintptr_t)((uintptr_t)j->entry | 1u))(c, (uintptr_t)(j->code + ent - 1u) | 1u);
#ifdef B86_PJ_VERIFY
    if (n) {
        uint32_t nr[8], nip = c->ip, nfl;
        static uint8_t nw[1024][4];
        unsigned k, bad = 0u;
        b86_flags_materialize(c); nfl = c->flags;
        memcpy(nr, c->r, sizeof nr);
        for (k = 0; k < pj_nvl; ++k) memcpy(nw[k], pj_vl[k].p, pj_vl[k].n);
        for (k = pj_nvl; k-- > 0u;) memcpy(pj_vl[k].p, pj_vl[k].old, pj_vl[k].n);
        memcpy(c->r, pr, sizeof pr); c->ip = pip; c->flags = pfl;
        c->lz_kind = plz[0]; c->lz_a = plz[1]; c->lz_res = plz[2]; c->lz_b = plz[3]; c->lz_ikind = plz[4]; c->lz_ia = plz[5]; c->lz_ires = plz[6];
        c->icount = pic;
        c->pj = NULL;
        for (k = 0; k < n; ++k) if (b86_step(c) != B86_OK) { bad |= 32u; break; }
        c->pj = j;
        b86_flags_materialize(c);
        for (k = 0; k < 8; ++k) if (c->r[k] != nr[k]) bad |= 1u;
        if (c->ip != nip) bad |= 2u;
        if ((c->flags ^ nfl) & ~(uint32_t)B86_AF) bad |= 4u;
        for (k = 0; k < pj_nvl; ++k) {
            unsigned q, later = 0u;
            for (q = k + 1u; q < pj_nvl; ++q) if (pj_vl[q].p == pj_vl[k].p) later = 1u;
            if (!later && memcmp(pj_vl[k].p, nw[k], pj_vl[k].n)) bad |= 8u;
        }
        c->icount = pic;                   /* b86_step's caller adds n */
        ++b86_pj_vfy_checked;
        if (bad && ++b86_pj_vfy_bad <= 30u) {
            fprintf(stderr, "[pj-verify] MISMATCH%s%s%s%s%s block %08X n=%u\n   native:", (bad & 1u) ? " regs" : "", (bad & 2u) ? " eip" : "",
                    (bad & 4u) ? " flags" : "", (bad & 8u) ? " memory" : "", (bad & 32u) ? " interp-stop" : "", pip, n);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", nr[k]);
            fprintf(stderr, " eip=%08X fl=%08X\n   interp:", nip, nfl);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", c->r[k]);
            fprintf(stderr, " eip=%08X fl=%08X\n   pre   :", c->ip, c->flags);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", pr[k]);
            fprintf(stderr, " fl=%08X\n", pfl);
        }
    }
#endif
#if BB_PJ_PM16_ADAPTIVE
    /* Only PM16 actual native calls contribute to the admission sample.
     * A 64-entry window below 4 guest instructions/entry indicates that
     * the short register-only tier is not amortizing its dispatcher costs.
     * Probe again after 4096 interpreter entry opportunities. */
    if (j->pm16_active) {
        j->pm16_adapt_insns += n;
        if (++j->pm16_adapt_calls >= 64u) {
            if (j->pm16_adapt_insns < 256u) {
                j->pm16_adapt_cooldown = 4096u;
                ++j->pm16_adapt_triggers;
            }
            j->pm16_adapt_calls = 0u;
            j->pm16_adapt_insns = 0u;
        }
    }
#endif
    ++j->runs; j->insns += n;
    if (c->pj_exit && n && pj_flat(c)) b86_pj_link(c);
    if (!n) ++j->exits_mem;
    return n;
}

/* ---- public API ------------------------------------------------------------ */
/* BB_PJ_ES_INDEPENDENT_V1: compact totals for A/B profiling. */
void b86_pj_es_stats(const B86Cpu *c, unsigned long out[4])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    memset(out, 0, 4u * sizeof out[0]);
    if (!j) return;
    out[0] = j->es_mode;
    out[1] = j->es_mode_switches;
    out[2] = j->es_relaxed_entries;
    out[3] = j->es_guarded_ops;
}
/* BB_PJ_ES_GUARD_BYPASS_V1: lightweight counters; no per-frame log. */
void b86_pj_es_skip_stats(const B86Cpu *c, unsigned long out[3])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    memset(out, 0, 3u * sizeof out[0]);
    if (!j) return;
    out[0] = j->es_skip_checks;
    out[1] = j->es_skip_hits;
    out[2] = j->es_skip_marks;
}
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
_Static_assert(sizeof(B86Pj) <= 8192u, "ES skip cache exceeds existing 8 KiB PJ arena");
#endif
size_t b86_pj_state_bytes(void) { return sizeof(B86Pj); }
int b86_pj_attach(B86Cpu *c, void *state, size_t state_bytes, void *code, size_t code_bytes)
{
    B86Pj *j = (B86Pj *)state;
    if (!c || !state || state_bytes < sizeof(B86Pj) || !code || code_bytes < 16384u + 512u * sizeof(uint32_t) || (((uintptr_t)code) & 3u)) return 0;
    memset(j, 0, sizeof *j);
    j->code = (uint8_t *)code; j->cap = (uint32_t)code_bytes - 512u * sizeof(uint32_t);
    memset(j->code + j->cap, 0, 512u * sizeof(uint32_t)); /* persistent negative table, outside emitter bounds */
    c->pj = j;
    pj_emit_shared(j);
    return 1;
}
void b86_pj_stats(const B86Cpu *c, unsigned long out[8])
{
    const B86Pj *j = (const B86Pj *)c->pj;
    memset(out, 0, 8 * sizeof out[0]);
    if (!j) return;
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    /* Reporting is outside the native/JIT hot path. */
    if (j->blocks >= bb5h_last_block_report + 20000ul) {
        bb5h_last_block_report = j->blocks;
        bb5h_dump();
    }
#endif
    out[0] = j->blocks; out[1] = j->flushes; out[2] = j->runs; out[3] = j->insns;
    out[4] = j->exits_mem; out[5] = j->declines; out[6] = j->used; out[7] = j->cap;
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE && \
    defined(B86_X87_TRACE_FIRST_REJECT) && B86_X87_TRACE_FIRST_REJECT
    /* Print at most once per 20,000 newly generated translations.
     * Updates from b86_pj_stats(), not from hot native code. */
    if(j->blocks >= j->profile_last_report + 20000u){
        B86Pj *mutable_j = (B86Pj *)(void *)j;
        mutable_j->profile_last_report = j->blocks;
        printf("[bb386-pj-cache] hash=%u cps=%u miss=%lu replace=%lu cps-refuse=%lu smc=%lu full=%lu blocks=%lu\n",
               (unsigned)B86_PJ_HASH,(unsigned)B86_PJ_CPS,
               j->profile_lookup_misses,j->profile_slot_replacements,
               j->profile_cps_refusals,j->flush_smc,j->flush_full,j->blocks);
    }
#endif
    /* Legacy packed ABI overflows 32-bit unsigned long at 4,295 SMC
     * flushes. Preserve it for blitzBUS's existing decoder, but report the
     * true individual counts in trace builds when approaching wraparound. */
    out[1] = j->flush_smc * 1000000ul + j->flush_full;
#if defined(B86_X87_TRACE_FIRST_REJECT) && B86_X87_TRACE_FIRST_REJECT
    {
        static unsigned long last_exact=0;
        if(j->flush_smc>=4000u &&
           (last_exact==0u || j->flush_smc-last_exact>=512u)){
            printf("[bb386-pj-exact] smc=%lu full=%lu blocks=%lu used=%lu\n",
                   j->flush_smc,j->flush_full,j->blocks,(unsigned long)j->used);
            last_exact=j->flush_smc;
        }
    }
#endif
}
/* BB_PJ_REPNE_EFFICIENCY_V1: read-only diagnostics for blitzBUS. */
void b86_pj_repne_stats(const B86Cpu *c, unsigned long out[6])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    memset(out, 0, 6u * sizeof out[0]);
    if (!j) return;
    out[0] = j->repne_calls;
    out[1] = j->repne_completed;
    out[2] = j->repne_budget_yields;
    out[3] = j->repne_refusals;
    out[4] = j->repne_comparisons;
    out[5] = j->repne_spans;
}
void b86_pj_stop_ops(const B86Cpu *c, unsigned long out[256])
{
    const B86Pj *j = (const B86Pj *)c->pj;
    if (j) memcpy(out, j->stop_op, 256 * sizeof out[0]); else memset(out, 0, 256 * sizeof out[0]);
}
/* BB_PJ_TLB_0F_V1: read-only direct pointer avoids a 1 KiB debug stack copy. */
const unsigned long *b86_pj_0f_stop(const B86Cpu *c)
{
    const B86Pj *j = (const B86Pj *)c->pj;
    return j ? j->stop_0f : NULL;
}
void b86_pj_mem_profile(const B86Cpu *c, unsigned long out[8])
{
    const B86Pj *j = (const B86Pj *)c->pj;
    memset(out, 0, 8u * sizeof out[0]);
    if (!j) return;
    out[0] = j->prof_mem_calls;
    out[1] = j->prof_mem_paged;
    out[2] = j->prof_tlb_hits;
    out[3] = j->prof_walks;
    out[4] = j->prof_mem_refused;
    out[5] = j->prof_mem_cross;
    out[6] = j->prof_mem_bounds;
    out[7] = j->prof_mem_writes;
#if BB_PJ_PM16_TRUTHDIAG
    /* Same point as the existing [bb386-mem] dump. Read-only: no JIT state
     * mutation, no serial traffic on translated/native hot paths. */
    printf("[bb386-pm16diag] emit-attempt=%lu emit-ok=%lu emit-fail=%lu 66=%lu 66-mov=%lu 66-stack=%lu 66-0f=%lu 66-other=%lu read-attempt=%lu read-ok=%lu write-attempt=%lu write-ok=%lu\n",
           (unsigned long)bb_pm16_diag.emit_attempt,
           (unsigned long)bb_pm16_diag.emit_ok,
           (unsigned long)bb_pm16_diag.emit_fail,
           (unsigned long)bb_pm16_diag.pref66,
           (unsigned long)bb_pm16_diag.pref66_mov,
           (unsigned long)bb_pm16_diag.pref66_stack,
           (unsigned long)bb_pm16_diag.pref66_0f,
           (unsigned long)bb_pm16_diag.pref66_other,
           (unsigned long)bb_pm16_diag.read_attempt,
           (unsigned long)bb_pm16_diag.read_success,
           (unsigned long)bb_pm16_diag.write_attempt,
           (unsigned long)bb_pm16_diag.write_success);
#endif

}
unsigned b86_pj_tlb_mode(void) { return BB_PJ_TLB_MODE; }


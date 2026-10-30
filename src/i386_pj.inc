#include "b86_div.h"   /* r73i */
#include "b86_v30f_mode.h"
#include "b86_v30f_admission.h"
/* Single translation unit includes admission governor; no build-file changes. */
#include "b86_v30f_admission.c"
#ifdef B86_PJ_RANGE_CHECK
#include <stdio.h>
#endif
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
/* V31: lazy exact RM->PM coherence. This DOES NOT rely on a complete
 * guest-physical write journal. Before any previously cached native code
 * can run following a real-mode or mapping epoch, detach every native link,
 * clear the direct entries, then exact-validate EACH block at its first
 * subsequent reuse. Unknown/corrupt link metadata fails closed to full
 * cache flush. Mode 0 restores full V29 scan and is the A/B control. */
#ifndef B86_PJ_V31_LAZY_EPOCH
#define B86_PJ_V31_LAZY_EPOCH 1
#endif
#if B86_PJ_V31_LAZY_EPOCH != 0 && B86_PJ_V31_LAZY_EPOCH != 1
#error "B86_PJ_V31_LAZY_EPOCH must be 0 or 1"
#endif
/* V30K: all native execution and existing safety checks are unchanged.
 * Mode 0 (default): remove six volatile hardware counter/timer reads PER
 * native run and all per-run split-stat accumulation from the hot dispatcher.
 * Mode 1: exact original v30j XIP split profiling for controlled A/B.
 * PC sampling, TLB counters, page reconciliations and cache audits remain.
 * The 12-field XIP split report is intentionally unavailable in mode 0. */
#ifndef B86_PJ_V30K_XIP_SPLIT
#define B86_PJ_V30K_XIP_SPLIT 0
#endif
#if B86_PJ_V30K_XIP_SPLIT != 0 && B86_PJ_V30K_XIP_SPLIT != 1
#error B86_PJ_V30K_XIP_SPLIT must be 0 or 1
#endif

/* V46: extend straight-line PM32 translations to reduce cold/native
 * dispatcher exits. The original 32-insn budget remains available with
 * B86_PJ_V46_LONG_BLOCKS=0; no guest semantics or validation bypass.
 * The 192-byte fetch window and 96-exit guard remain unchanged. */
#ifndef B86_PJ_V46_LONG_BLOCKS
#define B86_PJ_V46_LONG_BLOCKS 1
#endif
#if B86_PJ_V46_LONG_BLOCKS != 0 && B86_PJ_V46_LONG_BLOCKS != 1
#error B86_PJ_V46_LONG_BLOCKS must be 0 or 1
#endif
#ifndef B86_PJ_MAXI
#if B86_PJ_V46_LONG_BLOCKS
#define B86_PJ_MAXI 48u
#else
#define B86_PJ_MAXI 32u
#endif
#endif
#ifndef B86_PJ_HASH
#define B86_PJ_HASH 1024u
#endif
/* V54: PM16 FF-family /0,/1,/2,/4,/6 + E8 + C3 native translation.
 * A PM16 dynamic exit MUST NOT use the flat PM32 native hash table.
 * Dynamic PM16 exits retain r1 and return to the checked C dispatcher.
 * No interpreter call on successfully emitted forms. */
#ifndef B86_PJ_V54_NATIVE_CONTROL
#define B86_PJ_V54_NATIVE_CONTROL 1
#endif
#if B86_PJ_V54_NATIVE_CONTROL != 0 && B86_PJ_V54_NATIVE_CONTROL != 1
#error B86_PJ_V54_NATIVE_CONTROL must be 0 or 1
#endif

/* V55: PM16 native dynamic target lookup. A value of 0 restores the exact
 * v54 translator, including its checked-C PM16 target exit. */
#ifndef B86_PJ_V55_PM16_DYN
#define B86_PJ_V55_PM16_DYN 1
#endif
#if B86_PJ_V55_PM16_DYN != 0 && B86_PJ_V55_PM16_DYN != 1
#error B86_PJ_V55_PM16_DYN must be 0 or 1
#endif
/* V56: PM16 PUSH immediate, POP register, PUSH [EA16], RET imm16,
 * and context-checked chaining of direct CALL/JMP on a native cache hit.
 * Zero is a byte-exact preprocessor control for installed v55. */
#ifndef B86_PJ_V56_STACK_CHAIN
#define B86_PJ_V56_STACK_CHAIN 1
#endif
#if B86_PJ_V56_STACK_CHAIN != 0 && B86_PJ_V56_STACK_CHAIN != 1
#error B86_PJ_V56_STACK_CHAIN must be 0 or 1
#endif
/* V57: Flat PM32 native LOOP/LOOPE/LOOPNE and register-only POP r/m16/32.
 * Restrict to the 32-bit address-size contract; do not admit 67-prefix or
 * effective-address POP forms without their separate ordering analysis.
 * Disabled mode is byte-exact preprocessed v56. */
#ifndef B86_PJ_V57_PM32_COVERAGE
#define B86_PJ_V57_PM32_COVERAGE 1
#endif
#if B86_PJ_V57_PM32_COVERAGE != 0 && B86_PJ_V57_PM32_COVERAGE != 1
#error B86_PJ_V57_PM32_COVERAGE must be 0 or 1
#endif

/* V45: two-way native dynamic lookup using the existing flat PM32 table.
 * A miss probes the neighboring slot before returning to C. No new state,
 * guest-memory bypass, or epoch-validation shortcut. PM16 is unchanged. */
#ifndef B86_PJ_V45_DYN2
#define B86_PJ_V45_DYN2 1
#endif
#if B86_PJ_V45_DYN2 != 0 && B86_PJ_V45_DYN2 != 1
#error B86_PJ_V45_DYN2 must be 0 or 1
#endif
#if B86_PJ_V45_DYN2 && ((B86_PJ_HASH < 2u) || ((B86_PJ_HASH & (B86_PJ_HASH - 1u)) != 0u))
#error B86_PJ_V45_DYN2 requires a power-of-two hash table with at least two slots
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

/* V15 near-Jcc switch: defined even when PM16 tier is disabled, so the
 * diagnostics compile in both the normal and profiler-only configurations. */
#ifndef BB_PJ_PM16_JCC0F_V15
#define BB_PJ_PM16_JCC0F_V15 1
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
    /* r71: the exact code-page set and per-page block lists live after the
     * code buffer (see PJ_TAIL_BYTES); only the 4096-bit filter stays here. */
    /* BB_STAGE4W2_PROFILE: measurements only; no changes to dispatch decisions. */
    unsigned long profile_lookup_misses, profile_slot_replacements;
    unsigned long profile_cps_refusals, profile_last_report;
    unsigned long flush_smc, flush_full, links;
    unsigned long flush_pm16, flush_cps;     /* r71: flush causes */
    unsigned long smc_events, smc_kills;     /* r71: precise invalidation */
    unsigned long refinds;                   /* r71: hash misses served from the page lists */
    unsigned long fetch_refusals;            /* r71: entry code page not present */
    unsigned long v30i_tlb_hits, v30i_tlb_walks; /* v30i entry fetch TLB A/B */
    /* BB_PJ_RING_EVICT_V1 (r71): the buffer is two halves filled in turn.
     * When the current half is full the OTHER half is evicted (links into it
     * undone, hash entries dropped, page lists rebuilt from the survivors)
     * instead of flushing everything. */
    uint32_t shared_end, half, cur_half, half_fill[2];
    uint32_t nlinks;                         /* entries in the link log */
    unsigned long evictions, unlinks, gen;
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
    uint8_t *tail_z, *tail, *dyn, *dyn16, *chk, *entry, *memtramp;
#else
    uint8_t *tail_z, *tail, *dyn, *chk, *entry, *memtramp;
#endif   /* shared code at the start of the buffer */
    uint8_t *memt[6];                        /* r71: per size/write entries: MOVW r2,sw; B memtramp */
    uint8_t *exit_s;                         /* r71: unlinked compact static exit */
    uint8_t *lzh[24];                        /* r71: shared lazy-flag record writers (BB_PJ_LAZY_CALL_V1) */
    /* BB_PJ_HOT_TIER_V1 (r72): optional SRAM tier for the hottest flat blocks.
     * The XIP cache model put ~92% of Descent demo cache misses on PJ code
     * fetches from PSRAM; the hottest ~96 KiB of blocks carry most of them.
     * A block dispatched PJ_HOT_AFTER times is translated again into SRAM;
     * its PSRAM copy becomes a trampoline (LDR PC) to the SRAM copy. The SRAM
     * region has its own copy of the shared routines (BL/B reach is +-16 MiB
     * and SRAM is ~240 MiB from PSRAM). When it fills it is emptied at once. */
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
    struct PjSh { uint8_t *tail_z, *tail, *dyn, *dyn16, *chk, *entry, *memtramp, *exit_s, *memt[6], *lzh[24]; } hsh;
#else
    struct PjSh { uint8_t *tail_z, *tail, *dyn, *chk, *entry, *memtramp, *exit_s, *memt[6], *lzh[24]; } hsh;
#endif
    uint8_t *hot;                            /* NULL: no SRAM tier */
    uint32_t hot_base, hot_cap, hot_start, hot_used;   /* offsets relative to code (mod 2^32) */
    uint32_t nredir, maxredir, tr_hot, hot_after;   /* r72c: adaptive promotion threshold */
    unsigned long promo_at_reset, hot_tick;          /* r72d: threshold decay */
    /* r72e: XIP-cache split. Host gives the XIP access / hit counters and a
     * microsecond timer; each native run and each gap between runs is charged
     * to SRAM-tier runs, PSRAM runs or "outside" (everything else). */
    const volatile uint32_t *xacc, *xhit, *xus;
    uint32_t xla, xlh, xlt;
    unsigned long xs[12];
    uint32_t gshift, pcs_n;                          /* r72f: granule size, PC samples taken */
    unsigned long promotions, hot_resets, hot_refused;
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
    /* V15: compact, translation-STOP-only 0F histogram by code mode.
     * 0=PM16 code, 1=flat PM32 code. 16 selected opcodes, no 256-bin
     * duplicate; never interpreted as dynamic interpreter executions. */
    uint32_t v15_0f_stop[2][16];
    uint32_t v15_0f_total[2], v15_0f_prefixed[2];
    unsigned long prof_mem_calls, prof_mem_paged, prof_tlb_hits, prof_walks;
    unsigned long prof_mem_refused, prof_mem_cross, prof_mem_bounds, prof_mem_writes;
#if BB_PJ_PM16_REG_TIER
    uint32_t pm16_active, pm16_base, pm16_limit, pm16_kind;
    uint32_t pm16_cur, pm16_rr;          /* r71: current / next-victim PM16 context slot */
#endif
/* BB_PJ_PM16_NEGCACHE_V1: not reset by b86_pj_flush().
     * Inexact/stale negative hits only deopt to C; no native execution. */
    unsigned long pm16_neg_hits, pm16_neg_inserts;
#if BB_PJ_PM16_ADAPTIVE
    uint32_t pm16_adapt_calls, pm16_adapt_insns, pm16_adapt_cooldown;
    uint32_t pm16_adapt_skips, pm16_adapt_triggers;
#endif
    /* V27: one shared cross-mode invalidation boundary; no per-opcode guards.
     * Marked on real-mode dispatcher entry, consumed on first PJ admission.
     * The first cut is deliberately a conservative correctness control. */
    uint32_t v27_rm_pending;
    unsigned long v27_rm_episodes, v27_barriers, v27_reported;
    /* V28: bounded, byte-exact RM->PM block coherence.  Paged code uses
     * the conservative V27 full flush until page-generation support lands. */
    uint32_t v28_paged_seen;
    unsigned long v28_checks, v28_kills, v28_keeps, v28_fallbacks;
    unsigned long v28_reported_checks;
    /* V29: unified physical provenance, mapping-epoch validation.
     * This is shared across PM16/PM32 and the real-mode epoch barrier. */
    uint32_t v29_seen_pg, v29_seen_cr3;
    unsigned long v29_seen_tlb_flush;
    uint32_t v29_map_observed;
    unsigned long v29_map_changes, v29_paged_checks, v29_paged_kills;
    unsigned long v29_reported_checks, v29_reported_changes;
    /* V30A: exact nonpaged validation rather than wholesale PG-off flush. */
    unsigned long v30_pg0_checks, v30_pg0_kills, v30_pg0_keeps;
    unsigned long v30_reported_checks;
    /* V30C: park all native PM code during an intermediate PG-off mode
     * when previously translated paged code exists. No dormant translation
     * may run until full physical-page/byte reconciliation at PG-on. */
    uint32_t v30c_parked;
    unsigned long v30c_parks, v30c_deopts, v30c_resumes;
    unsigned long v30c_reported_parks, v30c_reported_resumes;
    int flush_pending;
    /* BB_PJ_REPNE_EFFICIENCY_V1: separate yield vs mapped-memory refusal. */
    unsigned long repne_calls, repne_completed, repne_budget_yields;
    unsigned long repne_refusals, repne_comparisons, repne_spans;
} B86Pj;

unsigned long b86_pj_stat_runs, b86_pj_stat_insns;
static unsigned long v30d_last_sample_blocks, v30d_last_sample_declines;
static unsigned long v30d_last_sample_evicts;
/* V30F single PJ instance. Slot contents live in the PSRAM tail. */
static b86_v30f_state v30f_admit;



/* BB_PJ_PRECISE_SMC_V1 (r71)
 * Descent's renderer patches its own unrolled span loops (100F2280..100F2410)
 * all the time. A whole-cache flush per patch made PJ retranslate its entire
 * working set over and over (108K translations per 37M instructions in the
 * demo, plus refills of the PSRAM buffer that evict everything from the
 * 16 KiB XIP cache). Now a store kills only the blocks whose x86 bytes it
 * overlaps. Every block is preceded by a 20-byte header
 *     [0] linear start  [1] byte length | dead<<31  [2] next on first page
 *     [3] next on second page  [4] EIP
 * and every code page heads a list of its blocks. A killed block's entry is
 * overwritten with an exit to C at its EIP, so chained jumps into it leave
 * native code; its hash entry is dropped and the next visit retranslates.
 * The exact code-page set grew from 256 to B86_PJ_CPS (default 2048) keys and
 * moved, with the list heads, beyond the emitter's bounds (it used to
 * overflow with Descent's ~300 code pages and flush everything). */
#undef B86_PJ_CPS
/* r71: a small SRAM code buffer (blitzBUS BB_PJ_PSRAM=0, e.g. Stage6D's
 * SRAM128) gets small side tables so most of the buffer stays code. */
#if defined(BB_PJ_PSRAM) && !BB_PJ_PSRAM
#define PJ_SMALL_TAIL 1
#else
#define PJ_SMALL_TAIL 0
#endif
#ifndef B86_PJ_CPS_KEYS
#define B86_PJ_CPS_KEYS (PJ_SMALL_TAIL ? 512u : 2048u)
#endif
#define B86_PJ_CPS B86_PJ_CPS_KEYS
#define PJ_HDR_BYTES 32u   /* r71: + [5] total block bytes, [6] physical page key; r72: [7] dispatch count */
#define PJ_NEG_BYTES (512u * sizeof(uint32_t))
/* BB_PJ_PM16_CTX_CACHE_V1 (r71): PM16 blocks no longer share the flat EIP
 * table. Every switch between Descent's flat code and a 16-bit DOS/4GW code
 * segment (each timer IRQ, page fault and DPMI call) used to flush the whole
 * translation cache: 3450 flushes in the 10.7M-instruction startup alone.
 * PM16 blocks now live in PJ_PM16_CTX per-segment tables keyed by (CS base,
 * limit, kind); flat blocks and their links survive PM16 visits. */
#define PJ_PM16_CTX 4u
#define PJ_PM16_ENT (PJ_SMALL_TAIL ? 128u : 512u)
#define PJ_PM16_BYTES (PJ_PM16_CTX * 4u * sizeof(uint32_t) + PJ_PM16_CTX * 2u * PJ_PM16_ENT * sizeof(uint32_t))
#ifndef B86_PJ_LINKS
#define B86_PJ_LINKS (PJ_SMALL_TAIL ? 1024u : 8192u)   /* link log entries (stub, target) */
#endif
#define PJ_LINK_BYTES (B86_PJ_LINKS * 2u * sizeof(uint32_t))
/* r72f: granule index: first PSRAM block starting in each 2^gshift-byte granule
 * of the code buffer (offset + 1), so a sampled PC can be mapped to its block */
/* r73c: B86_PJ_PCS=0 (default) builds without the sample index and drain:
 * the code buffer and translation path are exactly r72e's */
#ifndef B86_PJ_PCS
#define B86_PJ_PCS 0
#endif
#define PJ_GIDX_N (PJ_SMALL_TAIL ? 256u : 2048u)
#define PJ_GIDX_BYTES (B86_PJ_PCS ? PJ_GIDX_N * sizeof(uint32_t) : 0u)
/* V30F: reserve slots within the existing PSRAM code-buffer allocation.
 * 8 extra bytes permit 8-byte alignment even if caller aligns only to 4.
 * Nothing is appended to the limited 8 KiB B86Pj state arena. */
#define PJ_TAIL_V30F_BASE (PJ_NEG_BYTES + 2u * B86_PJ_CPS * sizeof(uint32_t) + PJ_PM16_BYTES + PJ_LINK_BYTES + PJ_GIDX_BYTES)
#define PJ_TAIL_V30F_SLOTS 512u
#define PJ_TAIL_V30F_EXTRA (PJ_TAIL_V30F_SLOTS * sizeof(b86_v30f_slot) + 8u)
#define PJ_TAIL_BYTES (PJ_TAIL_V30F_BASE + PJ_TAIL_V30F_EXTRA)
/* r72f: PC sampler state (host timer interrupt -> b86_pj_pc_sample) */
#define PJ_PCS_RING (B86_PJ_PCS ? 64u : 4u)   /* r73e: tiny when the sampler is compiled out */
static volatile uint32_t pj_pcs_ring[PJ_PCS_RING], pj_pcs_w, pj_pcs_r;
static volatile uint32_t pj_pcs_lo, pj_pcs_hi, pj_pcs_sh, pj_pcs_hlo, pj_pcs_hhi;
static volatile unsigned long pj_pcs_cls[8];
/* r73: heavy-hitter table of C-code PCs (SRAM other than the tier, and flash):
 * 256 slots, 4-way probing, space-saving replacement; key = pc >> 3 */
#define PJ_PCS_TOPN (B86_PJ_PCS ? 256u : 4u)
static volatile uint32_t pj_pcs_tkey[2][PJ_PCS_TOPN], pj_pcs_tcnt[2][PJ_PCS_TOPN];
static volatile uint8_t pj_pcs_promote_on = 1u;
static B86_PM_HOT void pj_pcs_top_add2(unsigned t, uint32_t pc)
{
    volatile uint32_t *tk = pj_pcs_tkey[t], *tc = pj_pcs_tcnt[t];
    const uint32_t key = (pc >> 3) | 0x80000000u;
    uint32_t i = ((key * 0x9E3779B1u) >> 24) & (PJ_PCS_TOPN - 1u), k, mi = i, mc = 0xFFFFFFFFu;
    for (k = 0u; k < 4u; ++k, i = (i + 1u) & (PJ_PCS_TOPN - 1u)) {
        if (tk[i] == key) { ++tc[i]; return; }
        if (!tk[i]) { tk[i] = key; tc[i] = 1u; return; }
        if (tc[i] < mc) { mc = tc[i]; mi = i; }
    }
    tk[mi] = key; tc[mi] = mc + 1u;
}
#define pj_pcs_top_add(pc) pj_pcs_top_add2(0u, (pc))
static unsigned pj_pcs_top_get(unsigned t, unsigned n, uint32_t *pc, uint32_t *cnt)
{
    volatile uint32_t *tk = pj_pcs_tkey[t], *tc = pj_pcs_tcnt[t];
    unsigned k, m = 0u, i;
    for (k = 0u; k < n; ++k) {
        uint32_t best = 0u, bi = 0u;
        for (i = 0u; i < PJ_PCS_TOPN; ++i) if (tk[i] && tc[i] > best) { best = tc[i]; bi = i; }
        if (!best) break;
        pc[m] = (tk[bi] & 0x7FFFFFFFu) << 3; cnt[m] = best; ++m;
        tc[bi] = 0u;
    }
    for (i = 0u; i < PJ_PCS_TOPN; ++i) { tk[i] = 0u; tc[i] = 0u; }
    return m;
}
/* r73: the n most-sampled C-code PCs since the last call (8-byte buckets),
 * largest first; clears the table. Returns how many were written. */
unsigned b86_pj_pc_top(unsigned n, uint32_t *pc, uint32_t *cnt) { return pj_pcs_top_get(0u, n, pc, cnt); }
/* r73c: the same for the interrupted code's LR (its caller, for leaf
 * functions such as divides, memchr or busy waits) */
unsigned b86_pj_pc_top_lr(unsigned n, uint32_t *pc, uint32_t *cnt) { return pj_pcs_top_get(1u, n, pc, cnt); }
/* r73: switch sample-driven promotion on or off (profiling stays on) */
void b86_pj_pc_promote(int on) { pj_pcs_promote_on = on ? 1u : 0u; }
/* Interrupt-safe; classifies the sample and queues PCs in PSRAM blocks.
 * cls: 0 PSRAM blocks, 1 PSRAM shared routines, 2 SRAM tier, 3 other SRAM,
 * 4 flash, 5 other PSRAM, 6 anything else; 7 = promotions made from samples */
B86_PM_HOT void b86_pj_pc_sample(uint32_t pc)
{
    pc &= ~1u;
    if (pj_pcs_hi && pc - pj_pcs_lo < pj_pcs_hi - pj_pcs_lo) {
        if (pc - pj_pcs_lo < pj_pcs_sh) ++pj_pcs_cls[1];
        else { const uint32_t w = pj_pcs_w; ++pj_pcs_cls[0]; pj_pcs_ring[w & (PJ_PCS_RING - 1u)] = pc; pj_pcs_w = w + 1u; }
    } else if (pj_pcs_hhi && pc - pj_pcs_hlo < pj_pcs_hhi - pj_pcs_hlo) ++pj_pcs_cls[2];
    else if (pc - 0x20000000u < 0x00082000u) { ++pj_pcs_cls[3]; pj_pcs_top_add(pc); }
    else if (pc - 0x10000000u < 0x01000000u) { ++pj_pcs_cls[4]; pj_pcs_top_add(pc); }
    else if (pc - 0x11000000u < 0x01000000u) ++pj_pcs_cls[5];
    else ++pj_pcs_cls[6];
}
/* r73c: sample with the interrupted LR; the LR is counted for C-code PCs */
B86_PM_HOT void b86_pj_pc_sample_lr(uint32_t pc, uint32_t lr)
{
    b86_pj_pc_sample(pc);
    pc &= ~1u;
    if (pc - 0x20000000u < 0x00082000u ? !(pj_pcs_hhi && pc - pj_pcs_hlo < pj_pcs_hhi - pj_pcs_hlo)
                                       : pc - 0x10000000u < 0x01000000u)
        pj_pcs_top_add2(1u, lr & ~1u);
}
void b86_pj_pc_stats(unsigned long out[8])
{
    unsigned k;
    for (k = 0u; k < 8u; ++k) out[k] = pj_pcs_cls[k];
}
static inline uint32_t *pj_cpsv(const B86Pj *j) { return (uint32_t *)(void *)(j->code + j->cap + PJ_NEG_BYTES); }
static inline uint32_t *pj_heads(const B86Pj *j) { return pj_cpsv(j) + B86_PJ_CPS; }
static inline uint32_t *pj_hdr(const B86Pj *j, uint32_t off) { return (uint32_t *)(void *)(j->code + off); }
#ifndef PJ_HOT_AFTER
#define PJ_HOT_AFTER 32u                  /* r72: dispatches before a flat block moves to SRAM */
#endif
static inline int pj_in_hot(const B86Pj *j, uint32_t off) { return j->hot && off - j->hot_base < j->hot_cap; }
static inline uint32_t *pj_redir(const B86Pj *j) { return (uint32_t *)(void *)(j->hot + j->hot_cap); }
static void pj_sh_get(const B86Pj *j, struct PjSh *m)
{
    unsigned k;
    m->tail_z = j->tail_z; m->tail = j->tail; m->dyn = j->dyn; m->chk = j->chk; m->entry = j->entry;
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
    m->dyn16 = j->dyn16;
#endif
    m->memtramp = j->memtramp; m->exit_s = j->exit_s;
    for (k = 0; k < 6u; ++k) m->memt[k] = j->memt[k];
    for (k = 0; k < 24u; ++k) m->lzh[k] = j->lzh[k];
}
static void pj_sh_set(B86Pj *j, const struct PjSh *m)
{
    unsigned k;
    j->tail_z = m->tail_z; j->tail = m->tail; j->dyn = m->dyn; j->chk = m->chk; j->entry = m->entry;
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
    j->dyn16 = m->dyn16;
#endif
    j->memtramp = m->memtramp; j->exit_s = m->exit_s;
    for (k = 0; k < 6u; ++k) j->memt[k] = m->memt[k];
    for (k = 0; k < 24u; ++k) j->lzh[k] = m->lzh[k];
}
static inline uint32_t *pj_pm16_region(const B86Pj *j) { return pj_heads(j) + B86_PJ_CPS; }
static inline uint32_t *pj_pm16_desc(const B86Pj *j, unsigned k) { return pj_pm16_region(j) + 4u * k; }
static inline uint32_t *pj_pm16_tag(const B86Pj *j, unsigned k) { return pj_pm16_region(j) + 4u * PJ_PM16_CTX + 2u * PJ_PM16_ENT * k; }
static inline uint32_t *pj_pm16_ent(const B86Pj *j, unsigned k) { return pj_pm16_tag(j, k) + PJ_PM16_ENT; }
static inline uint32_t *pj_links(const B86Pj *j) { return pj_pm16_region(j) + PJ_PM16_BYTES / sizeof(uint32_t); }
static inline uint32_t *pj_gidx(const B86Pj *j) { return pj_links(j) + 2u * B86_PJ_LINKS; }

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
/* r71: register moves use the 16-bit MOV (T1) form; never with SP/PC. */
static void pj_mov(PjE *e, int d, int m)
{
    if (d != SP && d != PC && m != SP && m != PC) { pje16(e, 0x4600u | ((unsigned)(d & 8) << 4) | ((unsigned)m << 3) | (unsigned)(d & 7)); return; }
    pj_dpr(e, 2u, 0, d, PC, m, 0u, 0u);
}
static void pj_shift(PjE *e, int d, int m, unsigned type, unsigned n) { pj_dpr(e, 2u, 0, d, PC, m, type, n); }  /* type 0 LSL 1 LSR 2 ASR */
/* BB_PJ_T16_LDST_V6: prefer architectural Thumb-1 T1 immediate load/store
 * encoding when BOTH registers are low and the offset is representable.
 * Also handle architectural SP-relative 16-bit STR/LDR (T2).
 * These are non-flag-setting encodings of the same host data operation.
 * Signed loads, high-register bases/results, unaligned/scaled out-of-range
 * offsets, and all other instructions use the unchanged Thumb-2 fallback. */
#ifndef BB_PJ_T16_LDST_V6
#define BB_PJ_T16_LDST_V6 1
#endif
static void pj_ldst(PjE *e, uint32_t h1, int t, int n, unsigned off)
{
    if (off > 0xFFFu) e->bad = 1;
#if BB_PJ_T16_LDST_V6
    if (off <= 0xFFFu && t >= R0 && t <= R7) {
        if (n >= R0 && n <= R7) {
            uint32_t opcode = 0u;
            unsigned shift = 0u;
            switch (h1) {
            case 0xF8C0u: opcode = 0x6000u; shift = 2u; break; /* STR word */
            case 0xF8D0u: opcode = 0x6800u; shift = 2u; break; /* LDR word */
            case 0xF880u: opcode = 0x7000u; shift = 0u; break; /* STRB */
            case 0xF890u: opcode = 0x7800u; shift = 0u; break; /* LDRB */
            case 0xF8A0u: opcode = 0x8000u; shift = 1u; break; /* STRH */
            case 0xF8B0u: opcode = 0x8800u; shift = 1u; break; /* LDRH */
            default: break;
            }
            if (opcode && off <= (31u << shift) && !(off & ((1u << shift) - 1u))) {
                pje16(e, opcode | ((off >> shift) << 6) | ((unsigned)n << 3) | (unsigned)t);
                return;
            }
        } else if (n == SP && off <= 1020u && !(off & 3u)) {
            if (h1 == 0xF8C0u || h1 == 0xF8D0u) {
                pje16(e, (h1 == 0xF8D0u ? 0x9800u : 0x9000u) |
                         ((unsigned)t << 8) | (off >> 2));
                return;
            }
        }
    }
#endif
    pje32(e, h1 | (unsigned)n, ((unsigned)t << 12) | (off & 0xFFFu));
}
_Static_assert(offsetof(B86Cpu, pj_stop) <= 4092u, "PJ addresses B86Cpu fields with 12-bit offsets");
#define PJ_LDR 0xF8D0u
#define PJ_STR 0xF8C0u
#define PJ_LDRB 0xF890u
#define PJ_STRB 0xF880u
#define PJ_LDRH 0xF8B0u
#define PJ_STRH 0xF8A0u
#define PJ_LDRSB 0xF990u
#define PJ_LDRSH 0xF9B0u
/* V8: Flag-preserving Thumb-16 SXTH/SXTB/UXTH/UXTB when both
 * registers are low (r0-r7). Unknown forms and high registers retain the
 * original 32-bit encoder, byte-for-byte. Neither instruction sets APSR.
 * 1011 0010 op(2) Rm(3) Rd(3): SXTH=0,SXTB=1,UXTH=2,UXTB=3. */
#ifndef BB_PJ_T16_EXT_V8
#define BB_PJ_T16_EXT_V8 1
#endif
static void pj_ext(PjE *e, uint32_t h1, int d, int m)
{
#if BB_PJ_T16_EXT_V8
    if ((unsigned)d < 8u && (unsigned)m < 8u) {
        uint32_t op;
        switch (h1) {
        case 0xFA0Fu: op = 0u; break; /* SXTH */
        case 0xFA4Fu: op = 1u; break; /* SXTB */
        case 0xFA1Fu: op = 2u; break; /* UXTH */
        case 0xFA5Fu: op = 3u; break; /* UXTB */
        default: op = 4u; break;
        }
        if (op < 4u) {
            pje16(e, 0xB200u | (op << 6) | ((unsigned)m << 3) | (unsigned)d);
            return;
        }
    }
#endif
    pje32(e, h1, 0xF080u | ((unsigned)d << 8) | (unsigned)m);
}
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
#ifdef B86_PJ_RANGE_CHECK
    if (off < -(1 << 24) || off >= (1 << 24)) { fprintf(stderr, "[pj] branch out of range: %p -> %p\n", (void *)at, (void *)to); __builtin_trap(); }
#endif
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
#if B86_PJ_V31_LAZY_EPOCH
/* V47: generation-tagged 512-way positive exact-validation memo. Each
 * code-cache/mode epoch changes the tag, so steady RM->PM transitions no
 * longer clear the memo array. A generation wrap clears tags before reuse.
 * Exact physical IDs and guest bytes are STILL checked on any miss. */
#ifndef B86_PJ_V47_EPOCH_MEMO
#define B86_PJ_V47_EPOCH_MEMO 1
#endif
#if B86_PJ_V47_EPOCH_MEMO != 0 && B86_PJ_V47_EPOCH_MEMO != 1
#error B86_PJ_V47_EPOCH_MEMO must be 0 or 1
#endif
#if B86_PJ_V47_EPOCH_MEMO
#define PJ_V31_MEMO_N 512u
#else
#define PJ_V31_MEMO_N 256u
#endif
/* Positive-only, address-tagged memo. The two counters distinguish reuse
 * after PSRAM half eviction and hot-tier recycling. Every epoch clears it. */
static uint32_t pj_v31_memo[PJ_V31_MEMO_N];
#if B86_PJ_V47_EPOCH_MEMO
static uint8_t pj_v47_memo_tag[PJ_V31_MEMO_N];
static uint8_t pj_v47_memo_epoch;
#endif
static unsigned long pj_v31_gen_seen, pj_v31_flush_seen, pj_v31_hot_seen;
static uint8_t pj_v31_active;
static unsigned long pj_v31_epochs, pj_v31_unlinked, pj_v31_exact,
                     pj_v31_memo_hits, pj_v31_kills, pj_v31_fallbacks;
static void pj_v31_memo_reset(const B86Pj *j)
{
#if B86_PJ_V47_EPOCH_MEMO
    /* Zero is reserved for an invalid/uninitialized tag. All old entries
     * are rejected after every rotation, including wraparound. */
    if (++pj_v47_memo_epoch == 0u) {
        memset(pj_v47_memo_tag, 0, sizeof pj_v47_memo_tag);
        pj_v47_memo_epoch = 1u;
    }
#else
    memset(pj_v31_memo, 0, sizeof pj_v31_memo);
#endif
    pj_v31_gen_seen = j->gen;
    pj_v31_flush_seen = j->flushes;
    pj_v31_hot_seen = j->hot_resets;
}
static void pj_v31_memo_sync(const B86Pj *j)
{
    if (pj_v31_gen_seen != j->gen || pj_v31_flush_seen != j->flushes ||
        pj_v31_hot_seen != j->hot_resets) pj_v31_memo_reset(j);
}
#endif

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
        if (pj_cpsv(j)[i] == pg + 1u) return 1;
        if (!pj_cpsv(j)[i]) return 0;
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
        if (pj_cpsv(j)[i] == pg + 1u) return (int)i + 1;
        if (!pj_cpsv(j)[i]) { pj_cpsv(j)[i] = pg + 1u; return (int)i + 1; }
    }
    return 0;
}
static int pj_cps_slot(const B86Pj *j, uint32_t pg)       /* slot+1, or 0 */
{
    uint32_t i = (pg * 0x9E3779B1u) & (B86_PJ_CPS - 1u), k;
    for (k = 0; k < B86_PJ_CPS; ++k, i = (i + 1u) & (B86_PJ_CPS - 1u)) {
        if (pj_cpsv(j)[i] == pg + 1u) return (int)i + 1;
        if (!pj_cpsv(j)[i]) return 0;
    }
    return 0;
}
static void pj_kill_page_range(B86Cpu *c, B86Pj *j, uint32_t pg, uint32_t lo, uint32_t hi);
static inline void b86_pj_note_store(B86Cpu *c, uint32_t lin, uint32_t n)
{
    B86Pj *j = b86_pj_state(c);
    uint32_t a = lin >> 12, b = (lin + n - 1u) >> 12;
    if (!j) return;
    for (; ; ++a) {
        const uint32_t h = PJ_PGH(a);
        if ((j->cpg[h >> 5] & (1u << (h & 31u))) && pj_cps_has(j, a)) {
            ++j->smc_events;
            pj_kill_page_range(c, j, a, lin, lin + n);   /* r71: only overlapping blocks */
        }
        if (a == b) break;
    }
}
static B86_PJ_COLD void pj_kill_block(B86Cpu *c, B86Pj *j, uint32_t off)
{
    uint32_t *hd = pj_hdr(j, off);
    const uint32_t ent = off + PJ_HDR_BYTES + 1u, eip = hd[4];
    uint8_t *code = j->code + off + PJ_HDR_BYTES;
    PjE e;
    unsigned k;
    hd[1] |= 0x80000000u;
    /* entry -> MOVW/MOVT r1,eip; MOVW r0,#0; B.W tail_z (back to C at eip) */
    e.p = code; e.end = code + 16; e.bad = 0;
    pj_movw(&e, R1, eip & 0xFFFFu); pj_movt(&e, R1, eip >> 16);
    pj_movw(&e, R0, 0u);
    pj_patch_b(pj_b(&e, -1), pj_in_hot(j, off) ? j->hsh.tail_z : j->tail_z);
#if defined(__linux__)
    __builtin___clear_cache((char *)code, (char *)code + 16);
#else
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    for (k = 0; k < B86_PJ_HASH; ++k) if (j->ent[k] == ent) { j->ent[k] = 0u; j->tag[k] = 0u; }
    for (k = 0; k < PJ_PM16_CTX * PJ_PM16_ENT; ++k) {
        uint32_t *tg = pj_pm16_tag(j, k / PJ_PM16_ENT), *en = pj_pm16_ent(j, k / PJ_PM16_ENT);
        if (en[k % PJ_PM16_ENT] == ent) { en[k % PJ_PM16_ENT] = 0u; tg[k % PJ_PM16_ENT] = 0u; }
    }
    ++j->smc_kills;
    c->pj_stop = 1u;                              /* leave native code at the next chain point */
}
static B86_PJ_COLD void pj_kill_page_range(B86Cpu *c, B86Pj *j, uint32_t pg, uint32_t lo, uint32_t hi)
{
    const int slot = pj_cps_slot(j, pg);
    uint32_t at = slot ? pj_heads(j)[slot - 1] : 0u;
    while (at) {
        uint32_t *hd = pj_hdr(j, at - 1u);
        const uint32_t start = hd[0], len = hd[1] & 0x3FFFFFFFu;
        const uint32_t next = (start >> 12) == pg ? hd[2] : hd[3];
        if (!(hd[1] & 0x80000000u) && start < hi && lo < start + len) pj_kill_block(c, j, at - 1u);
        at = next;
    }
}
/* r71: BB_PJ_BLOCK_REFIND_V1. The EIP hash tables are direct-mapped: two hot
 * blocks with colliding hashes used to evict each other and get translated
 * again on every alternation (1.14M translations and 644 buffer refills in
 * 600M demo instructions). On a hash miss, look the block up in its code
 * page's list first; only translate when no live copy exists. */
static B86_PM_HOT uint32_t pj_find_block(const B86Pj *j, uint32_t lin, uint32_t eip, uint32_t pm16)
{
    const uint32_t pg = lin >> 12;
    const int slot = pj_cps_slot(j, pg);
    uint32_t at = slot ? pj_heads(j)[slot - 1] : 0u;
    while (at) {
        const uint32_t *hd = pj_hdr(j, at - 1u);
        const uint32_t next = (hd[0] >> 12) == pg ? hd[2] : hd[3];
        if (hd[0] == lin && hd[4] == eip && !(hd[1] & 0x80000000u) && ((hd[1] >> 30) & 1u) == pm16)
            return at - 1u + PJ_HDR_BYTES + 1u;
        at = next;
    }
    return 0u;
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
/* V30I: native entry instruction-fetch TLB hit. A successful TLB entry
 * already came from the 386 paging walker and carries its Accessed-bit
 * effect; the established b86_pj_mem() fast path uses the identical tag.
 * A miss, out-of-RAM frame, or disabled mode ALWAYS uses b86_pg_walk().
 * This does not skip mapping-epoch, SMC, or source-byte validation. */
#ifndef B86_PJ_V30I_ENTRY_TLB
#define B86_PJ_V30I_ENTRY_TLB 1
#endif
#if B86_PJ_V30I_ENTRY_TLB != 0 && B86_PJ_V30I_ENTRY_TLB != 1
#error B86_PJ_V30I_ENTRY_TLB must be 0 or 1
#endif
static inline int pj_v30i_entry_tlb_hit(const B86Cpu *c, uint32_t lin)
{
#if B86_PJ_V30I_ENTRY_TLB
    const unsigned slot = (lin >> 12) & 63u;
    const uint32_t tag = (lin & 0xFFFFF000u) | 1u;
    return c->pg && c->tlb_tag[slot] == tag &&
           c->tlb_phys[slot] <= B86_RAM_BYTES - 0x1000u;
#else
    (void)c; (void)lin;
    return 0;
#endif
}
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
{   /* r73i: hardware UDIV/SDIV + divlu instead of libgcc 64-bit division */
    return (uint32_t)b86_divx(lo, hi, d, wsgn & 7u, (wsgn >> 3) & 1u, &b86_pj_divres[0], &b86_pj_divres[1]);
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
/* r71: STI as PM40 does it (IF=1, IOPL checked). A pending IRQ is taken at
 * the next chain point or block exit, i.e. after at least one more
 * instruction, which also covers the architectural STI shadow. */
static B86_PM_HOT uint32_t b86_pj_fast_sti(B86Cpu *c)
{
    if ((c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) return 0u;
    c->flags |= B86_IF;
    return 1u;
}
static B86_PM_HOT uint32_t b86_pj_fast_cli(B86Cpu *c)
{
    if ((c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) return 0u;
    c->flags &= ~(uint32_t)B86_IF;
    return 1u;
}
static B86_PM_HOT uint32_t b86_pj_fast_outimm8(B86Cpu *c, uint32_t port, uint32_t al)
{
#ifdef B86_PJ_VERIFY
    (void)c; (void)port; (void)al; return 0u;   /* r71: device I/O cannot be replayed by the verifier */
#endif
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
#ifdef B86_PJ_VERIFY
    (void)c; (void)port; return 256u;           /* r71: device I/O cannot be replayed by the verifier */
#endif
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
    uint8_t *fuse_at;                            /* r71: ARM flags valid for last_kind if e.p == fuse_at */
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
/* r71: STRD rt, rt2, [r12, #off] (off multiple of 4, < 1024) */
_Static_assert(offsetof(B86Cpu, lz_res) == offsetof(B86Cpu, lz_a) + 4u && offsetof(B86Cpu, lz_ires) == offsetof(B86Cpu, lz_ia) + 4u,
               "STRD needs adjacent lazy-flag fields");
static void pjt_strd(PjT *t, int rt, int rt2, unsigned off)
{ pje32(&t->e, 0xE9C0u | (unsigned)CREG, ((unsigned)rt << 12) | ((unsigned)rt2 << 8) | (off >> 2)); }
/* r71: BB_PJ_LAZY_CALL_V1. The record stores (kind, a, res, ikind clear)
 * were ~16-24 bytes inline at every flag producer. Shared writers take
 * a in r2 and res in r3 (clobber r2, r12): one 4-byte BL per producer. */
static int pj_lzh_index(uint32_t kind, int clr)
{
    static const uint32_t ks[12] = { LZ_ADD8, LZ_ADD16, LZ_ADD32, LZ_SUB8, LZ_SUB16, LZ_SUB32,
                                     LZ_LOG8, LZ_LOG16, LZ_LOG32, LZ_INC32, LZ_DEC32, LZ_INC16 };
    unsigned k;
    for (k = 0; k < 12u; ++k) if (ks[k] == kind) return (int)(k * 2u + (clr ? 1u : 0u));
    return -1;
}
static void pjt_lazy(PjT *t, uint32_t kind, int va, int vres)
{
    {
        const B86Pj *jj = (const B86Pj *)t->c->pj;
        const int inc = kind == LZ_INC32 || kind == LZ_DEC32 || kind == LZ_INC16;
        const int ix = pj_lzh_index(kind, !inc && !t->ikind_clear);
        const int is_log = kind == LZ_LOG8 || kind == LZ_LOG16 || kind == LZ_LOG32;
        if (jj && ix >= 0 && jj->lzh[ix] && vres == R3 && (is_log ? va < 0 : va == R2)) {
            pjt_call(t, (const void *)jj->lzh[ix]);
            if (inc) t->ikind_clear = 0; else t->ikind_clear = 1;
            t->last_kind = (int)kind;
            return;
        }
    }
    const int inc = kind == LZ_INC32 || kind == LZ_DEC32 || kind == LZ_INC16 || kind == LZ_DEC16 ||
                    kind == LZ_INC8 || kind == LZ_DEC8;              /* r70b: 16-bit forms were missing */
    pjt_c(t);
    pj_movw(&t->e, LR, kind);
    if (inc) {
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
        pjt_strd(t, va, vres, (unsigned)offsetof(B86Cpu, lz_ia));     /* r71: lz_ia, lz_ires adjacent */
        t->ikind_clear = 0;
    } else {
        pj_ldst(&t->e, PJ_STR, LR, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
        if (va >= 0) pjt_strd(t, va, vres, (unsigned)offsetof(B86Cpu, lz_a));   /* r71: lz_a, lz_res adjacent */
        else pj_ldst(&t->e, PJ_STR, vres, CREG, (unsigned)offsetof(B86Cpu, lz_res));
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
/* V24 unified ModR/M descriptor. Translation time only: no x86 state changes.
 * Admits every byte; memory/register eligibility is the caller's decision.
 * A truncated input is refused without consuming data. */
typedef struct PjUnifiedModRM {
    unsigned raw, mod, reg, rm;
} PjUnifiedModRM;
static PjUnifiedModRM pjt_unified_modrm_fields(uint32_t byte)
{
    PjUnifiedModRM m;
    m.raw = byte & 255u;
    m.mod = m.raw >> 6u;
    m.reg = (m.raw >> 3u) & 7u;
    m.rm = m.raw & 7u;
    return m;
}
static int pjt_unified_modrm(PjT *t, PjUnifiedModRM *out)
{
    uint32_t byte;
    if (!out || !pjt_u8(t, &byte)) return 0;
    *out = pjt_unified_modrm_fields(byte);
    return 1;
}
static int pjt_unified_modrm_regonly(PjT *t, uint32_t *raw)
{
    PjUnifiedModRM m;
    if (!raw || !pjt_unified_modrm(t, &m) || m.mod != 3u) return 0;
    *raw = m.raw;
    return 1;
}

/* BB_PJ_IMM12_FOLD_V5: general Thumb-2 immediate address generation.
 * ADDW/SUBW immediate T4 is a single 32-bit, non-flag-setting instruction.
 * Unlike the prior MOVW+ADD fallback it also does not clobber R0.
 * Byte encodings are checked against the ARM-targeted assembler in tests.
 * The existing modified-immediate and large-offset paths are untouched. */
#ifndef BB_PJ_IMM12_FOLD_V5
#define BB_PJ_IMM12_FOLD_V5 1
#endif
static void pj_imm12w_v5(PjE *e, int sub, int d, int n, uint32_t imm)
{
    if (imm > 4095u) { e->bad = 1; return; }
    pje32(e,
          (sub ? 0xF2A0u : 0xF200u) | (((imm >> 11) & 1u) << 10) | (unsigned)n,
          (((imm >> 8) & 7u) << 12) | ((unsigned)d << 8) | (imm & 255u));
}
static void pjt_addimm(PjT *t, int d, int n, uint32_t v)
{
    if (!v) { if (d != n) pj_mov(&t->e, d, n); return; }
    if (pj_dpi(&t->e, 8u, 0, d, n, v)) return;
    if (pj_dpi(&t->e, 13u, 0, d, n, 0u - v)) return;
#if BB_PJ_IMM12_FOLD_V5
    /* Preserve the flag-independent semantics of pj_dpi(..., s=0).
     * Avoid PC/SP encodings, which have special ARM alias rules. */
    if (d != PC && n != PC && d != SP && n != SP) {
        if (v <= 4095u) { pj_imm12w_v5(&t->e, 0, d, n, v); return; }
        if ((uint32_t)(0u - v) <= 4095u) {
            pj_imm12w_v5(&t->e, 1, d, n, (uint32_t)(0u - v)); return;
        }
    }
#endif
    pj_imm32(&t->e, R0, v); pj_dpr(&t->e, 8u, 0, d, n, R0, 0u, 0u);
}
/* V7: absolute-address ModRM/SIB forms can load the final displacement
 * directly, rather than zeroing R1 and performing an unnecessary ADD.
 * Only mod=00 rm=101 or mod=00 SIB(base=101,index=none) qualify.
 * Indexed, base-relative, signed displacement and register forms keep
 * the original path. No guest flags, TLB, or CPU-state changes. */
#ifndef BB_PJ_ABS_ADDR_V7
#define BB_PJ_ABS_ADDR_V7 1
#endif
static int pjt_modrm(PjT *t, PjM *m)
{
    uint32_t sib, disp = 0u;
    PjUnifiedModRM head;
    if (!pjt_unified_modrm(t, &head)) return 0;
    m->mod = head.mod; m->reg = head.reg; m->rm = head.rm;
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
#if BB_PJ_ABS_ADDR_V7
        if (!have) { pj_imm32(&t->e, R1, disp); disp = 0u; }
#else
        if (!have) pj_movw(&t->e, R1, 0u);
#endif
    } else if (m->rm == 5u && m->mod == 0u) {
        if (!pjt_u32(t, &disp)) return 0;
#if BB_PJ_ABS_ADDR_V7
        pj_imm32(&t->e, R1, disp); disp = 0u;
#else
        pj_movw(&t->e, R1, 0u);
#endif
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
    /* r71: BB_PJ_MEMCALL_SHORT_V1: per size/write entry (no MOVW at the
     * call site) and a 6-byte literal {eip, retired16}: 10 bytes, was 16 */
    pjt_call(t, (const void *)j->memt[(size == 4u ? 2u : size - 1u) + (write ? 3u : 0u)]);
    pje16(&t->e, t->ip0 & 0xFFFFu); pje16(&t->e, t->ip0 >> 16);
    pje16(&t->e, (t->ninsn - 1u) & 0xFFFFu);
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
    /* r71: 32-bit producers set the ARM flags too (S forms); a conditional
     * branch emitted right after (nothing in between) uses them directly
     * instead of reloading the lazy record (BB_PJ_JCC_FUSE_V1). */
    case 0: pj_mov(&t->e, R2, a); pj_dpr(&t->e, 8u, w == 4u, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d); pjt_lazy(t, pj_k_add[w], R2, R3); t->fuse_at = w == 4u ? t->e.p : NULL; return 1;
    case 5: case 7: {
        const int dd = op == 7u ? R3 : d;
        int narrow = 0;
        pj_mov(&t->e, R2, a); pj_dpr(&t->e, 13u, w == 4u, dd, a, b, 0u, 0u);
        if (dd != R3) pj_mov(&t->e, R3, dd);
        if (w < 4u && op == 7u && b != LR) {        /* r71: 8/16-bit CMP: compare the operands shifted to bit 31 */
            const unsigned sh = 32u - 8u * w;
            pj_shift(&t->e, LR, R2, 0u, sh);
            pj_dpr(&t->e, 13u, 1, PC, LR, b, 0u, sh);   /* CMP lr, b, LSL #sh */
            narrow = 1;
        }
        pjt_lazy(t, pj_k_sub[w], R2, R3);           /* writers never touch the ARM flags */
        t->fuse_at = (w == 4u || narrow) ? t->e.p : NULL;
        return 1; }
    case 1: pj_dpr(&t->e, 2u, w == 4u, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d);
        if (w < 4u) pj_dpr(&t->e, 2u, 1, LR, PC, R3, 0u, 32u - 8u * w);   /* r71: MOVS lr, r3, LSL #sh: N/Z of the narrow result */
        pjt_lazy(t, pj_k_log[w], -1, R3);
        t->fuse_at = t->e.p; return 1;
    case 4: pj_dpr(&t->e, 0u, w == 4u, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d);
        if (w < 4u) pj_dpr(&t->e, 2u, 1, LR, PC, R3, 0u, 32u - 8u * w);   /* r71: MOVS lr, r3, LSL #sh: N/Z of the narrow result */
        pjt_lazy(t, pj_k_log[w], -1, R3);
        t->fuse_at = t->e.p; return 1;
    case 6: pj_dpr(&t->e, 4u, w == 4u, d, a, b, 0u, 0u); pj_mov(&t->e, R3, d);
        if (w < 4u) pj_dpr(&t->e, 2u, 1, LR, PC, R3, 0u, 32u - 8u * w);   /* r71: MOVS lr, r3, LSL #sh: N/Z of the narrow result */
        pjt_lazy(t, pj_k_log[w], -1, R3);
        t->fuse_at = t->e.p; return 1;
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
    /* r71: BB_PJ_JCC_FUSE_V1 */
    if (t->fuse_at && t->fuse_at == t->e.p && pj_armcc[cc] >= 0) {
        const int k0 = t->last_kind;
        const int cf0 = (cc >> 1) == 1u || (cc >> 1) == 3u;
        if (k0 == LZ_SUB32 || k0 == LZ_SUB16 || k0 == LZ_SUB8) return pj_armcc[cc];
        if (k0 == LZ_ADD32 && !cf0) return pj_armcc[cc];
        if ((k0 == LZ_INC32 || k0 == LZ_DEC32) && !cf0) return pj_armcc[cc];
        if ((k0 == LZ_LOG32 || k0 == LZ_LOG16 || k0 == LZ_LOG8) && (cc == 4u || cc == 5u || cc == 8u || cc == 9u)) return pj_armcc[cc];
    }
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

/* V50: first native instruction coverage batch. 0 retains exact v48 code. */
#ifndef B86_PJ_V50_NATIVE_COVERAGE
#define B86_PJ_V50_NATIVE_COVERAGE 1
#endif
#if B86_PJ_V50_NATIVE_COVERAGE != 0 && B86_PJ_V50_NATIVE_COVERAGE != 1
#error B86_PJ_V50_NATIVE_COVERAGE must be 0 or 1
#endif

/* V51: PM16 LEA plus checked segment-stack native coverage.  The legacy
 * V50 translator is retained byte-for-byte at preprocessor level when off. */
#ifndef B86_PJ_V51_NATIVE_COVERAGE
#define B86_PJ_V51_NATIVE_COVERAGE 1
#endif
#if B86_PJ_V51_NATIVE_COVERAGE != 0 && B86_PJ_V51_NATIVE_COVERAGE != 1
#error B86_PJ_V51_NATIVE_COVERAGE must be 0 or 1
#endif

/* V52: PM16 segment MOV register forms and ES override native reads.
 * 0 is byte-exact V51 control after preprocessing; 1 adds guarded coverage. */
#ifndef B86_PJ_V52_NATIVE_SEGMOV_ES
#define B86_PJ_V52_NATIVE_SEGMOV_ES 1
#endif
#if B86_PJ_V52_NATIVE_SEGMOV_ES != 0 && B86_PJ_V52_NATIVE_SEGMOV_ES != 1
#error B86_PJ_V52_NATIVE_SEGMOV_ES must be 0 or 1
#endif

/* V53: PM16 checked 8E memory source and native ES store coverage.
 * 0 is the exact V52 translation/preprocessor control. */
#ifndef B86_PJ_V53_NATIVE_SEGMEM
#define B86_PJ_V53_NATIVE_SEGMEM 1
#endif
#if B86_PJ_V53_NATIVE_SEGMEM != 0 && B86_PJ_V53_NATIVE_SEGMEM != 1
#error B86_PJ_V53_NATIVE_SEGMEM must be 0 or 1
#endif

/* V23: the shared protected-mode instruction-head parser.  This is a
 * translation-time frontend component, not a guest-execution call.  Policies
 * intentionally preserve the historical eligibility boundaries: flat32
 * recognizes its original prefix subset (maximum four bytes including opcode);
 * PM16 consumes exactly one byte, leaving 66h to its existing PM16 decoder.
 * Future 8086/186/286 frontends can reuse this parser after their own
 * instruction-admission contracts have been established. */
typedef struct PjUnifiedHead {
    uint32_t opcode;
    unsigned bytes, op_override, repz, repne, segpref, es_override;
    unsigned operand_width, address_width;
} PjUnifiedHead;
enum { PJ_HEAD_PM16 = 0u, PJ_HEAD_FLAT32 = 1u };
static int pjt_unified_head(PjT *t, unsigned policy, PjUnifiedHead *h)
{
    uint32_t op;
    unsigned np = 0u, o16 = 0u, repz = 0u, repne = 0u;
    unsigned segpref = 0u, es_override = 0u;
    if (!h || policy > PJ_HEAD_FLAT32) return 0;
    if (policy == PJ_HEAD_PM16) {
        /* Preserve V22: do NOT consume the byte after 66h here. */
        if (!pjt_u8(t, &op)) return 0;
        h->opcode = op; h->bytes = 1u;
        h->op_override = 0u; h->repz = 0u; h->repne = 0u;
        h->segpref = 0u; h->es_override = 0u;
        h->operand_width = 2u; h->address_width = 2u;
        return 1;
    }
    for (;;) {
        if (!pjt_u8(t, &op) || ++np > 4u) return 0;
        if (op == 0x66u) { o16 = 1u; continue; }
        if (op == 0x3Eu || op == 0x36u || op == 0x2Eu || op == 0x26u) {
            /* Preserve original flat32 ES-override state across prefixes. */
            if (op == 0x26u) es_override = 1u;
            segpref = 1u; continue;
        }
        if (op == 0xF3u) { repz = 1u; continue; }
        if (op == 0xF2u) { repz = 1u; repne = 1u; continue; }
        break;
    }
    h->opcode = op; h->bytes = np;
    h->op_override = o16; h->repz = repz; h->repne = repne;
    h->segpref = segpref; h->es_override = es_override;
    h->operand_width = o16 ? 2u : 4u; h->address_width = 4u;
    return 1;
}

/* V22 unified JIT frontend primitives. Both PM16 and flat32 register MOVs
 * share exactly the same width-aware register emitter. The EA16 decoder is
 * shared by PM16 MOV, MOVZX/MOVSX and the optional 32-bit memory MOV tier.
 * These helpers generate the same Thumb-2 as the V21 source; they deliberately
 * do not bypass descriptor, paging, or self-modifying-code checks. */
static void pjt_unified_regmov(PjT *t, uint32_t op, uint32_t mr, unsigned width)
{
    const unsigned reg = (mr >> 3u) & 7u, rm = mr & 7u;
    const unsigned src = (op & 2u) ? rm : reg;
    const unsigned dst = (op & 2u) ? reg : rm;
    pjt_getr(t, R1, src, width);
    pjt_putr(t, dst, width, R1);
}

/* Decode 16-bit ModRM addressing into native R1 and return the default
 * segment DS/SS. Caller has already consumed the ModRM byte and checks
 * mod != 3. A short guest code fetch returns 0, allowing the caller's
 * existing PJ16_FAIL to undo emission, EIP, flags and retirement state. */
static int pjt_unified_ea16(PjT *t, uint32_t mr, unsigned *segment)
{
    const unsigned mod = mr >> 6u, rm = mr & 7u;
    uint32_t disp = 0u;
    unsigned base = 255u, index = 255u;
    if (mod == 3u || !segment) return 0;
    *segment = ((rm == 2u || rm == 3u) ||
                (rm == 6u && mod != 0u)) ? B86_SS : B86_DS;
    if (mod == 0u && rm == 6u) {
        if (!pjt_imm(t, 2u, &disp)) return 0;
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
            if (!pjt_u8(t, &disp)) return 0;
            disp = (uint32_t)(int32_t)(int8_t)disp;
        } else if (mod == 2u) {
            if (!pjt_imm(t, 2u, &disp)) return 0;
        }
        pjt_addimm(t, R1, R1, disp);
        pj_ubfx(&t->e, R1, R1, 0u, 16u);
    }
    return 1;
}

#if B86_PJ_V53_NATIVE_SEGMEM
/* The common protected-mode ES store is emitted as Thumb-2. This guard
 * admits only cached, writable, expand-up data descriptors, in-limit and
 * nonwrapping byte/word ranges, with aligned halfword accesses. It makes
 * NO guest-visible changes before all checks pass. The existing native
 * memt write trampoline performs the paging, mapped-frame, SMC and MMIO
 * checks; on refusal it returns to the interpreter before a native store. */
static void pjt_v53_es_write_guard(PjT *t, unsigned width)
{
    const unsigned sc = (unsigned)offsetof(B86Cpu, pm386) +
        (unsigned)offsetof(B86PM1State, seg) +
        B86_ES * (unsigned)sizeof(B86PM1Cache);
    pjt_c(t); /* R12 is CPU pointer, R1 is zero-extended EA16 */
    pj_ldst(&t->e, PJ_LDRB, R0, CREG,
            sc + (unsigned)offsetof(B86PM1Cache, valid));
    pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
    pjt_exit_stay(t, 0 /* EQ: invalid */, t->ip0, t->ninsn - 1u);
    pj_ldst(&t->e, PJ_LDRB, R0, CREG,
            sc + (unsigned)offsetof(B86PM1Cache, reserved));
    pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
    pjt_exit_stay(t, 1 /* NE: reserved */, t->ip0, t->ninsn - 1u);
    pj_ldst(&t->e, PJ_LDRB, R0, CREG,
            sc + (unsigned)offsetof(B86PM1Cache, access));
    pj_dpi(&t->e, 0u, 0, R0, R0, 0x1Eu);
    pj_dpi(&t->e, 13u, 1, PC, R0, 0x12u);
    pjt_exit_stay(t, 1 /* NE: not writable expand-up data */, t->ip0,
                  t->ninsn - 1u);
    pj_ldst(&t->e, PJ_LDR, R3, CREG,
            sc + (unsigned)offsetof(B86PM1Cache, limit));
    pjt_addimm(t, R2, R1, width - 1u);
    pj_dpr(&t->e, 13u, 1, PC, R2, R3, 0u, 0u);
    pjt_exit_stay(t, 8 /* HI: end offset beyond limit */, t->ip0,
                  t->ninsn - 1u);
    pj_ldst(&t->e, PJ_LDR, R0, CREG,
            sc + (unsigned)offsetof(B86PM1Cache, base));
    /* Check base + LAST byte as uint32, not merely base + first byte. */
    pj_dpr(&t->e, 8u, 1, R2, R2, R0, 0u, 0u);
    pjt_exit_stay(t, 2 /* CS: linear end overflows */, t->ip0,
                  t->ninsn - 1u);
    pj_dpr(&t->e, 8u, 0, R1, R1, R0, 0u, 0u);
    if (width == 2u) {
        pj_dpi(&t->e, 0u, 1, R2, R1, 1u);
        pjt_exit_stay(t, 1 /* NE: unaligned halfword */, t->ip0,
                      t->ninsn - 1u);
    }
    t->fuse_at = NULL; /* guard CMP/ANDS changes native condition flags */
}
#endif /* B86_PJ_V53_NATIVE_SEGMEM */

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
    uint8_t *const fz0 = t->fuse_at;
    t->ninsn++;                                    /* counted as retired once its exit is taken */
    /* a refused instruction leaves no code, exits or flag knowledge behind */
#define PJ_FAIL() do { t->ninsn--; t->pos = start; t->e.p = e0; t->nex = nex0; t->last_kind = lk0; t->ikind_clear = ik0; t->creg_ok = cr0; t->fuse_at = fz0; return 0; } while (0)
#define PJ_NEXT() (t->ip0 + (t->pos - start))
    {   /* V23: the same decoder frontend is shared with PM16. */
        PjUnifiedHead head;
        if (!pjt_unified_head(t, PJ_HEAD_FLAT32, &head)) PJ_FAIL();
        op = head.opcode; np = head.bytes; o16 = head.op_override;
        repz = head.repz; repne = head.repne;
        segpref = head.segpref; es_override = head.es_override;
        w = head.operand_width;
    }
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
    /* r71: F2 on MOVS/STOS behaves exactly like F3 (Watcom's memcpy at
     * 100DFC98 uses F2 A5 / F2 A4); only CMPS/SCAS care which one it is. */
    if (repne && (op == 0xA4u || op == 0xA5u || op == 0xAAu || op == 0xABu)) repne = 0u;
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
        /* r71: 32-bit INC/DEC set ARM N/Z/V too (fused with a following
         * JNZ/JS/JL... that does not read CF) */
        if (op < 0x48u) pj_dpi(&t->e, 8u, w == 4u, R3, R2, 1u); else pj_dpi(&t->e, 13u, w == 4u, R3, R2, 1u);
        pjt_putr(t, op & 7u, w, R3);
        pjt_lazy(t, op < 0x48u ? pj_k_inc[w] : pj_k_dec[w], R2, R3);
        t->fuse_at = w == 4u ? t->e.p : NULL;
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
#if B86_PJ_V57_PM32_COVERAGE
    /* 8F /0 with mod=11: POP r16/r32. Reuse the existing PM32 checked
     * stack read, then increment ESP before writing the destination.
     * This order is essential for POP ESP and 66 POP SP. 8F memory forms
     * are refused: POP [ESP+disp] calculates its address after the pop. */
    case 0x8F:
        if (!((np == 1u && !o16) || (np == 2u && o16)) ||
            t->pos >= t->n || (t->b[t->pos] & 0xF8u) != 0xC0u ||
            !pjt_modrm(t, &m) || m.mod != 3u || m.reg != 0u) PJ_FAIL();
        pj_mov(&t->e, R1, GR(4));
        pjt_mem(t, w, 0);
        pj_dpi(&t->e, 8u, 0, GR(4), GR(4), w);
        if (w == 4u) pj_ldst(&t->e, PJ_LDR, GR(m.rm), R0, 0u);
        else { pjt_ld(t, R1, R0, 2u); pjt_putr(t, m.rm, 2u, R1); }
        return 1;
#endif /* V57 native POP r/m register */
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        const unsigned ww = (op & 1u) ? w : 1u;
        if (!pjt_modrm(t, &m)) PJ_FAIL();
        if (op <= 0x89u) {
            if (m.mod == 3u) { pjt_unified_regmov(t, op, (m.mod<<6u)|(m.reg<<3u)|m.rm, ww); return 1; }
            pjt_mem(t, ww, 1);
            if (ww == 4u) pj_ldst(&t->e, PJ_STR, GR(m.reg), R0, 0u);
            else { pjt_getr(t, R1, m.reg, ww); pjt_st(t, R1, R0, ww); }
        } else {
            if (m.mod == 3u) { pjt_unified_regmov(t, op, (m.mod<<6u)|(m.reg<<3u)|m.rm, ww); return 1; }
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
    case 0xFA: case 0xFB:
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pjt_call(t, op == 0xFAu ? (const void *)b86_pj_fast_cli : (const void *)b86_pj_fast_sti);
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
#if B86_PJ_V50_NATIVE_COVERAGE
        if (op == 0xFCu || op == 0xFDu) {
            /* CLD/STD change ONLY DF, which is never covered by lazy arithmetic
             * records. Do not materialize those records or return to C. */
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDR, R1, CREG, (unsigned)offsetof(B86Cpu, flags));
            pj_dpi(&t->e, op == 0xFCu ? 1u : 2u, 0, R1, R1, B86_DF);
            pj_ldst(&t->e, PJ_STR, R1, CREG, (unsigned)offsetof(B86Cpu, flags));
            t->fuse_at = NULL;
            return 1;
        }
#endif
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
#if B86_PJ_V57_PM32_COVERAGE
    /* E0/E1/E2: LOOPNE/LOOPE/LOOP in the flat 32-bit-address contract.
     * Unlike DEC ECX, LOOP does not modify guest EFLAGS. The generated
     * Thumb-2 subtracts without S-bit and uses a native CMP solely for the
     * branch. The pre-LOOP guest ZF comes from the existing lazy flag path.
     * ECX==0 after decrement takes the fallthrough before inspecting ZF.
     * 67 address-size overrides use CX, not ECX: refuse those for now. */
    case 0xE0: case 0xE1: case 0xE2: {
        int zf_arm;
        if (np != 1u || !pjt_u8(t, &imm)) PJ_FAIL();
        imm = (uint32_t)(int32_t)(int8_t)imm;
        pj_dpi(&t->e, 13u, 0, GR(1), GR(1), 1u); /* SUB ECX,#1; S=0 */
        pj_dpi(&t->e, 13u, 1, PC, GR(1), 0u);     /* CMP ECX,#0 (ARM only) */
        pjt_exit_cond(t, 0 /* EQ, count exhausted */, PJ_NEXT(), t->ninsn);
        if (op == 0xE2u) {
            pjt_exit(t, PJ_NEXT() + imm, t->ninsn, 0);
        } else {
            /* b86_pj_cond is called only if the lazy x86 ZF producer cannot
             * be reconstructed by generated Thumb-2; never interpret LOOP. */
            zf_arm = pjt_flags_for(t, 4u /* ZF==1 */);
            pjt_exit_cond(t, op == 0xE1u ? zf_arm : (zf_arm ^ 1),
                          PJ_NEXT() + imm, t->ninsn);
            pjt_exit(t, PJ_NEXT(), t->ninsn, 0);
        }
        return 2;
    }
#endif /* V57 native LOOP/LOOPE/LOOPNE */
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
    if (x->dyn == 0) {                                                /* r71: 14-byte static exit */
        { uint8_t *bl = t->e.p; pje32(&t->e, 0xF000u, 0xD000u);       /* BL exit_s */
          pj_patch_b(bl, j->exit_s); bl[3] |= 0x40u; }
        pje16(&t->e, x->ip & 0xFFFFu); pje16(&t->e, x->ip >> 16);
        pje16(&t->e, x->n);
        pje32(&t->e, 0xBF00u, 0xBF00u);                               /* +10: B.W target once linked */
        (void)tail;
        return;
    }
#if B86_PJ_V54_NATIVE_CONTROL
    /* dyn=3: PM16 computed EIP already in r1. The native v55 route
     * probes the CURRENT CS-context's PM16 table, never the flat PM32
     * table. On any miss/interrupt it falls through the original tail_z
     * path so C can validate/translate safely. */
    if (x->dyn == 3) {
        pj_movw(&t->e, R0, x->n);
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
        pj_patch_b(pj_b(&t->e, -1), j->dyn16);
#else
        pj_patch_b(pj_b(&t->e, -1), tail_z);
#endif
        return;
    }
#endif
    if (x->dyn == 2) pj_imm32(&t->e, R1, x->ip);
    pj_movw(&t->e, R0, x->n);
    pj_patch_b(pj_b(&t->e, -1), x->dyn == 1 ? j->dyn : tail_z);   /* EIP in r1: look the target up natively */
}

/* Shared code: tails and the native lookup for dynamic exits (RET, indirect
 * JMP/CALL): r1 = target EIP, r0 = retired in the leaving block. A hit
 * enters the target's chain entry with r3 = r0; a miss returns to C. */
/* V25: experimental fast native store for paged extended guest DATA RAM.
 * This counter is diagnostic and does not change the guest CPU layout. */
/* V30N LEAN NATIVE FASTWRITE: avoid an expensive volatile SRAM
 * read-modify-write on every native store.  A/B mode=0 restores the exact
 * old emitted counter and its telemetry; default=1 preserves all original
 * fault/SMC/TLB/codemap gates and removes ONLY diagnostic instructions.
 * No writes are skipped.  No guest behavior changes. */
#ifndef B86_PJ_V30N_LEAN_FASTWRITE
#define B86_PJ_V30N_LEAN_FASTWRITE 1
#endif
#if B86_PJ_V30N_LEAN_FASTWRITE != 0 && B86_PJ_V30N_LEAN_FASTWRITE != 1
#error B86_PJ_V30N_LEAN_FASTWRITE must be 0 or 1
#endif
#ifndef BB_PJ_V25_FASTWRITE
#define BB_PJ_V25_FASTWRITE 1
#endif
#if BB_PJ_V25_FASTWRITE && !B86_PJ_V30N_LEAN_FASTWRITE
static volatile uint32_t pj_v25_fastwrite_hits;
#endif
static B86_PJ_COLD void pj_emit_shared_at(B86Pj *j, uint8_t *base, uint8_t *end);
static B86_PJ_COLD void pj_emit_shared(B86Pj *j) { pj_emit_shared_at(j, j->code, j->code + j->cap); }
static B86_PJ_COLD void pj_emit_shared_at(B86Pj *j, uint8_t *base, uint8_t *end)
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
/* BB_PJ_DIRECT_WRITE_V2: route the three native write stubs straight to checked C slow path. */
#ifndef BB_PJ_MEMWRITE_DIRECT_SLOW_V2
#define BB_PJ_MEMWRITE_DIRECT_SLOW_V2 1
#endif
/* BB_PJ_V4_READ_GATE: read stubs are trusted, writes always bypass this path.
 * No runtime state change; the control mode preserves the exact V3 emitter. */
#ifndef BB_PJ_MEMREAD_STUB_ONLY_V4
#define BB_PJ_MEMREAD_STUB_ONLY_V4 1
#endif
/* BB_PJ_MEMSIZE_TRUSTED_STUBS_V1 is defined later in this function,
 * immediately before the read gate. If disabled, its existing fully-checked
 * control path is selected automatically, and the V4 shortcut is not emitted. */
#if BB_PJ_MEMREAD_STUB_ONLY_V4 && !BB_PJ_MEMWRITE_DIRECT_SLOW_V2
#error "V4 requires V2 direct-write routing; V1-disabled builds retain full checks"
#endif
#if BB_PJ_MEMWRITE_DIRECT_SLOW_V2
    uint8_t *mem_slow = NULL, *write_slow = NULL;
/* BB_PJ_SHARED_MEMTRAMP_FAST is defaulted further down this function.
 * Do not test it here: at this point an undefined macro evaluates to 0,
 * despite the later native-store block being enabled. */
#if BB_PJ_V25_FASTWRITE && !defined(B86_PJ_VERIFY)
    uint8_t *write_fast = NULL;
#endif
#endif
    unsigned hb = 0u;
    while ((1u << hb) < B86_PJ_HASH) ++hb;
    memset(t, 0, sizeof *t);
    t->e.p = base; t->e.end = end;
    pjt_tail(t, &j->tail_z, &j->tail);
    /* r71: BB_PJ_SHORT_STUB_V1. Static exits are 14 bytes:
     *   +0 BL exit_s | BL chk (linked)   +4 .word eip   +8 .hword n   +10 B.W target
     * LR = stub+5 inside both routines. exit_s: r1 = eip, r0 = n, B tail (tail
     * records LR so C can link the stub). chk: budget/IRQ checks, count n,
     * continue at +10; on refusal leave through tail_z with r1/r0 as above. */
    j->exit_s = t->e.p;
    pje32(&t->e, 0xF850u | LR, (R1 << 12) | 0xC01u);       /* LDR r1, [lr, #-1] */
    pj_ldst(&t->e, PJ_LDRH, R0, LR, 3u);
    pj_patch_b(pj_b(&t->e, -1), j->tail);
    j->chk = t->e.p;
    {
        uint8_t *f1, *f2;
        pje32(&t->e, 0xF850u | LR, (R1 << 12) | 0xC01u);   /* LDR r1, [lr, #-1] (only used on refusal) */
        pj_ldst(&t->e, PJ_LDRH, R0, LR, 3u);
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
        pj_dpi(&t->e, 8u, 0, LR, LR, 6u);                  /* -> stub+10 | 1 */
        pje16(&t->e, 0x4700u | (LR << 3));                  /* BX lr */
        pj_patch_b(f1, j->tail_z); pj_patch_b(f2, j->tail_z);
    }
    {   /* r71: lazy-flag record writers; r2 = a, r3 = res */
        static const uint32_t ks[12] = { LZ_ADD8, LZ_ADD16, LZ_ADD32, LZ_SUB8, LZ_SUB16, LZ_SUB32,
                                         LZ_LOG8, LZ_LOG16, LZ_LOG32, LZ_INC32, LZ_DEC32, LZ_INC16 };
        unsigned k, clr;
        for (k = 0; k < 12u; ++k) for (clr = 0; clr < 2u; ++clr) {
            const int inc = k >= 9u, log = k >= 6u && k < 9u;
            if (inc && clr) { j->lzh[k * 2u + clr] = NULL; continue; }
            j->lzh[k * 2u + clr] = t->e.p;
            pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
            if (inc) {
                pje32(&t->e, 0xE9C0u | (unsigned)CREG, ((unsigned)R2 << 12) | ((unsigned)R3 << 8) | ((unsigned)offsetof(B86Cpu, lz_ia) >> 2));
                pj_movw(&t->e, R2, ks[k]);
                pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_ikind));
            } else {
                if (log) pj_ldst(&t->e, PJ_STR, R3, CREG, (unsigned)offsetof(B86Cpu, lz_res));
                else pje32(&t->e, 0xE9C0u | (unsigned)CREG, ((unsigned)R2 << 12) | ((unsigned)R3 << 8) | ((unsigned)offsetof(B86Cpu, lz_a) >> 2));
                pj_movw(&t->e, R2, ks[k]);
                pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_kind));
                if (clr) { pj_movw(&t->e, R2, LZ_NONE); pj_ldst(&t->e, PJ_STR, R2, CREG, (unsigned)offsetof(B86Cpu, lz_ikind)); }
            }
            pje16(&t->e, 0x4700u | (LR << 3));               /* BX lr */
        }
    }
    /* BB_PJ_SHARED_MEMTRAMP_FAST: one native read-TLB fast path per JIT,
     * NOT one per translated instruction. R1=linear, R2=size|write<<8,
     * LR addresses the two caller literal words (EIP and retired).
     * Preserve R1 and R2 for the exact original C slow path. No writes
     * take the fast path; all guest writes retain SMC protection.
     * [sp+4] and [sp+16] belong to callers; never use them as scratch.
     * Fail closed for unusual widths, paging off, crossings, TLB misses,
     * and out-of-RAM addresses. No new B86Cpu/B86Pj fields. */
/* BB_PJ_MEMFAST_V1: fixed-size emit stubs provide only 1/2/4 reads and 0x101/0x102/0x104 writes. */
#ifndef BB_PJ_MEMSIZE_TRUSTED_STUBS_V1
#define BB_PJ_MEMSIZE_TRUSTED_STUBS_V1 1
#endif
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
#if BB_PJ_MEMSIZE_TRUSTED_STUBS_V1
            /* All native callers enter through memt[k] stubs which load
             * R2 from the six compile-time constants 1,2,4,0x101,0x102,
             * 0x104. No direct callers enter memtramp. Thus a single
             * unsigned R2>4 check admits all reads and rejects all writes.
             * The original size, crossing, TLB and RAM checks remain in
             * the #else control and the common paths below unchanged. */
#if BB_PJ_MEMREAD_STUB_ONLY_V4 && BB_PJ_MEMWRITE_DIRECT_SLOW_V2
            /* V4: only the 1-,2-,4-byte READ stubs reach this address.
             * V2's three WRITE stubs branch to write_slow, which retains
             * the complete C path.  All page/TLB/bounds guards below stay.
             * No R2 comparison or branch is emitted in the enabled mode. */
#else
            pj_dpi(&t->e, 13u, 1, PC, R2, 4u);
            miss[nm++] = pj_b(&t->e, 8 /* HI: all write stubs */);
#endif
#else
            pj_dpi(&t->e, 13u, 1, PC, R2, 1u);
            miss[nm++] = pj_b(&t->e, 3 /* LO: invalid size */);
            pj_dpi(&t->e, 13u, 1, PC, R2, 4u);
            miss[nm++] = pj_b(&t->e, 8 /* HI: write/invalid size */);
            pj_dpi(&t->e, 13u, 1, PC, R2, 3u);
            miss[nm++] = pj_b(&t->e, 0 /* EQ: invalid size */);
#endif
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
            pj_dpi(&t->e, 8u, 0, LR, LR, 6u);
            pje16(&t->e, 0x4700u | (LR << 3));
            pje16(&t->e, 0xBF00u);
            /* Every rejected access resumes the unmodified C slow path. */
            for (i = 0u; i < nm; ++i) pj_patch_b(miss[i], t->e.p);
        }
#endif
#if BB_PJ_MEMWRITE_DIRECT_SLOW_V2 && BB_PJ_SHARED_MEMTRAMP_FAST
        /* All read/TLB refusals jump here, retaining the entry LR saved above. */
        mem_slow = t->e.p;
#endif
        pj_ldst(&t->e, PJ_LDR, R0, SP, 0u);
        pj_call(&t->e, (const void *)b86_pj_mem);
        pj_ldst(&t->e, PJ_LDR, LR, SP, 12u);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        fail = pj_b(&t->e, 0 /* EQ */);
        pj_dpi(&t->e, 8u, 0, LR, LR, 6u);
        pje16(&t->e, 0x4700u | (LR << 3));                  /* BX lr: continue after the literal */
        pje16(&t->e, 0xBF00u);
        pj_patch_b(fail, t->e.p);
        pj_dpi(&t->e, 13u, 0, LR, LR, 1u);
        pj_ldst(&t->e, PJ_LDR, R1, LR, 0u);
        pj_ldst(&t->e, PJ_LDRH, R0, LR, 4u);
        pj_patch_b(pj_b(&t->e, -1), j->tail_z);
    }
#if BB_PJ_MEMWRITE_DIRECT_SLOW_V2 && BB_PJ_SHARED_MEMTRAMP_FAST
    /* Branch-local shim: the original memtramp saved LR at [sp+12]
     * before checking pg and the read-size gate.  Writes skip those
     * checks; we still save the exact same LR and enter the identical
     * b86_pj_mem slow path, with R1 and R2 untouched. No ABI changes. */
    write_slow = t->e.p;
    pj_ldst(&t->e, PJ_STR, LR, SP, 12u);
    pj_patch_b(pj_b(&t->e, -1), mem_slow);
#endif
#if BB_PJ_V25_FASTWRITE && BB_PJ_SHARED_MEMTRAMP_FAST && BB_PJ_MEMWRITE_DIRECT_SLOW_V2 && !defined(B86_PJ_VERIFY)
    /* Conservative native store: only present, same-page TLB hits that
     * provably cannot modify either PM native code or RM translated code.
     * For extended physical RAM smc() ignores the RM code map. For lower
     * RAM two exact 64-byte codemap-line tests are required. Any possible
     * code-page touch uses the exact original checked C helper.
     *
     * The PJ hashed code-page filter is only a NEGATIVE test: collisions
     * force slow C; it is re-read at every store, never hoisted.
     *
     * State at entry: r1=linear, r2=0x100|{1,2,4}, LR=literal pointer,
     * r4-r11=guest regs. Scratch r0,r3,r12,lr only. Leaves R1/R2 intact.
     * On success returns pointer in r0 and resumes *after* the 6-byte
     * literal, as the established memtramp does. No side effects on reject.
     * Only the three fixed-size write stubs target this entry. */
    write_fast = t->e.p;
    {
        uint8_t *miss[14]; unsigned nm = 0u, k;
        pj_ldst(&t->e, PJ_STR, LR, SP, 12u); /* identical slow-path LR save */
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pj_ldst(&t->e, PJ_LDRB, R0, CREG, (unsigned)offsetof(B86Cpu, pg));
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        miss[nm++] = pj_b(&t->e, 0); /* EQ: no paging */
        /* Write width is from trusted memt[3..5] stubs, low 8 bits. */
        pj_ubfx(&t->e, R3, R2, 0u, 8u);
        pj_ubfx(&t->e, R0, R1, 0u, 12u);
        pj_dpr(&t->e, 8u, 0, R0, R0, R3, 0u, 0u);
        pj_dpi(&t->e, 13u, 1, PC, R0, 4096u);
        miss[nm++] = pj_b(&t->e, 8); /* HI: crosses page */
        pj_ubfx(&t->e, R3, R1, 12u, 6u); /* 64-slot TLB index */
        pj_shift(&t->e, LR, R1, 1u, 12u);
        pj_shift(&t->e, LR, LR, 0u, 12u);
        pj_dpi(&t->e, 2u, 0, LR, LR, 1u);
        pjt_addimm(t, R0, CREG, (uint32_t)offsetof(B86Cpu, tlb_tag));
        pje32(&t->e, 0xF850u | R0, (R0 << 12) | (2u << 4) | R3);
        pj_dpr(&t->e, 13u, 1, PC, R0, LR, 0u, 0u);
        miss[nm++] = pj_b(&t->e, 1); /* NE: TLB miss */
        pjt_addimm(t, R0, CREG, (uint32_t)offsetof(B86Cpu, tlb_phys));
        pje32(&t->e, 0xF850u | R0, (R0 << 12) | (2u << 4) | R3);
        pj_ubfx(&t->e, R3, R1, 0u, 12u);
        pj_dpr(&t->e, 2u, 0, R0, R0, R3, 0u, 0u);
        pj_imm32(&t->e, LR, B86_RAM_BYTES);
        pj_ubfx(&t->e, R3, R2, 0u, 8u);
        pj_dpr(&t->e, 13u, 0, LR, LR, R3, 0u, 0u);
        pj_dpr(&t->e, 13u, 1, PC, R0, LR, 0u, 0u);
        miss[nm++] = pj_b(&t->e, 8); /* HI: outside guest RAM */
        /* For LOW physical memory the real-mode code cache might see a
         * store through smc(): bypass only when both covered 64-byte
         * codemap lines are zero (or the hook / codemap is absent).
         * Extended RAM never enters the RM codemap path in smc(). */
        pj_imm32(&t->e, LR, B86_MEM_BYTES);
        pj_dpr(&t->e, 13u, 1, PC, R0, LR, 0u, 0u);
        {
            uint8_t *skip_rm = pj_b(&t->e, 2); /* HS: extended RAM */
            /* Never dereference past the low-window code-map boundary. */
            pj_ubfx(&t->e, R3, R2, 0u, 8u);
            pj_dpr(&t->e, 8u, 0, R3, R0, R3, 0u, 0u);
            pj_dpi(&t->e, 13u, 0, R3, R3, 1u);
            pj_dpr(&t->e, 13u, 1, PC, R3, LR, 0u, 0u);
            miss[nm++] = pj_b(&t->e, 2); /* HS: last byte at low-window edge */
            pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, smc_hook));
            pj_dpi(&t->e, 13u, 1, PC, LR, 0u);
            { uint8_t *no_hook = pj_b(&t->e, 0);
              pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, codemap));
              pj_dpi(&t->e, 13u, 1, PC, LR, 0u);
              { uint8_t *no_map = pj_b(&t->e, 0);
                pj_shift(&t->e, R3, R0, 1u, B86_LINE_SHIFT);
                pj_dpr(&t->e, 8u, 0, R3, LR, R3, 0u, 0u);
                pj_ldst(&t->e, PJ_LDRB, R3, R3, 0u);
                pj_dpi(&t->e, 13u, 1, PC, R3, 0u);
                miss[nm++] = pj_b(&t->e, 1); /* NE: first code line */
                pj_ubfx(&t->e, R3, R2, 0u, 8u);
                pj_dpr(&t->e, 8u, 0, R3, R0, R3, 0u, 0u);
                pj_dpi(&t->e, 13u, 0, R3, R3, 1u);
                pj_shift(&t->e, R3, R3, 1u, B86_LINE_SHIFT);
                pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, codemap));
                pj_dpr(&t->e, 8u, 0, R3, LR, R3, 0u, 0u);
                pj_ldst(&t->e, PJ_LDRB, R3, R3, 0u);
                pj_dpi(&t->e, 13u, 1, PC, R3, 0u);
                miss[nm++] = pj_b(&t->e, 1); /* NE: last code line */
                pj_patch_b(no_map, t->e.p);
              }
              pj_patch_b(no_hook, t->e.p);
            }
            pj_patch_b(skip_rm, t->e.p);
        }
        /* PJ exact SMC tracking: a definite ZERO in the 4096-bit linear
         * code-page filter is enough to rule out any PJ invalidation.
         * A set bit (including collision) must use C and pj_cps_has(). */
        pj_shift(&t->e, R3, R1, 1u, 12u); /* linear page */
        pj_imm32(&t->e, LR, 0x9E3779B1u);
        pje32(&t->e, 0xFB00u | R3, 0xF000u | (R3 << 8) | LR); /* mul r3,r3,lr */
        pj_shift(&t->e, R3, R3, 1u, 20u); /* PJ_PGH(page): 0..4095 */
        pj_ubfx(&t->e, LR, R3, 5u, 7u);  /* word index */
        pj_ldst(&t->e, PJ_LDR, CREG, CREG, (unsigned)offsetof(B86Cpu, pj));
        /* Do not use pjt_addimm() for this large struct offset: its
         * general fallback uses R0, which still holds the physical addr.
         * Split into nonflag ADDW chunks without any scratch register. */
        { uint32_t left = (uint32_t)offsetof(B86Pj, cpg);
          while (left) {
              const uint32_t part = left > 4095u ? 4095u : left;
              pj_imm12w_v5(&t->e, 0, CREG, CREG, part);
              left -= part;
          }
        }
        pje32(&t->e, 0xF850u | CREG, (CREG << 12) | (2u << 4) | LR);
        pj_ubfx(&t->e, LR, R3, 0u, 5u); /* bit index */
        pje32(&t->e, 0xFA20u | CREG, 0xF000u | (CREG << 8) | LR); /* lsr.w r12,r12,lr */
        pj_dpi(&t->e, 0u, 1, CREG, CREG, 1u); /* ands r12,r12,#1 */
        miss[nm++] = pj_b(&t->e, 1); /* NE: possible code page */
#if B86_PJ_V33_PHYSDIRTY
        /* V33: the native fast store deliberately bypasses b86_pj_mem/smc.
         * Record the already-checked physical page, BEFORE the store pointer
         * is returned. The write-stub rejects page crossings, so marking
         * exactly one 4 KiB physical page is sufficient. R0 still holds
         * physical address; R1/R2 and the [sp+12] return slot are untouched.
         * No page mark authorizes skipping exact x86 code comparison. */
        pj_shift(&t->e, R3, R0, 1u, 12u);
        pj_imm32(&t->e, LR, (uint32_t)(uintptr_t)b86_v33_dirty);
        pj_dpr(&t->e, 8u, 0, LR, LR, R3, 0u, 0u);
        pj_imm32(&t->e, R3, 1u);
        pj_ldst(&t->e, PJ_STRB, R3, LR, 0u);
#if B86_PJ_V37_EPOCH_LEDGER
        /* BB_V37_EPOCH_LEDGER_V1: same physical frame, current epoch token.
         * R0 and R1/R2 remain intact; no calls and no native-entry guards.
         * This precedes the guest write; the original v33 cumulative mark
         * is still set independently. */
        pj_shift(&t->e, R3, R0, 1u, 12u);
        pj_imm32(&t->e, LR, (uint32_t)(uintptr_t)b86_v37_page_epoch);
        pj_dpr(&t->e, 8u, 0, LR, LR, R3, 0u, 0u);
        pj_imm32(&t->e, R3, (uint32_t)(uintptr_t)&b86_v37_token);
        pj_ldst(&t->e, PJ_LDRB, R3, R3, 0u);
        pj_ldst(&t->e, PJ_STRB, R3, LR, 0u);
#endif
#endif
        /* Exactly match b86_host_phys(c, phys): lower guest RAM may be
         * remapped through c->pt[ ((uintptr_t)(c->mem+phys)>>12)&511 ].
         * Above B86_MEM_BYTES no pt delta is applied. */
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
#if defined(B86_PAGED) && B86_RAM_BYTES > B86_MEM_BYTES
        pj_mov(&t->e, R3, R0); /* preserve physical address for region check */
#endif
        pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, mem));
        pj_dpr(&t->e, 8u, 0, R0, R0, LR, 0u, 0u);
#ifdef B86_PAGED
#if B86_RAM_BYTES > B86_MEM_BYTES
        pj_imm32(&t->e, LR, B86_MEM_BYTES);
        pj_dpr(&t->e, 13u, 1, PC, R3, LR, 0u, 0u);
        { uint8_t *skip_pt = pj_b(&t->e, 2); /* HS: extended */
#endif
        pj_ubfx(&t->e, R3, R0, 12u, 9u);
        pj_ldst(&t->e, PJ_LDR, LR, CREG, (unsigned)offsetof(B86Cpu, pt));
        pj_dpr(&t->e, 8u, 0, LR, LR, R3, 0u, 2u);
        pj_ldst(&t->e, PJ_LDR, LR, LR, 0u);
        pj_dpr(&t->e, 8u, 0, R0, R0, LR, 0u, 0u);
#if B86_RAM_BYTES > B86_MEM_BYTES
        pj_patch_b(skip_pt, t->e.p);
        }
#endif
#endif
        /* When lean=1 the successful native data-store path jumps
         * directly to the same LR restore and BX return as before.
         * Diagnostic mode=0 preserves all original emitted instructions. */
#if !B86_PJ_V30N_LEAN_FASTWRITE
        pj_imm32(&t->e, R3, (uint32_t)(uintptr_t)&pj_v25_fastwrite_hits);
        pj_ldst(&t->e, PJ_LDR, LR, R3, 0u);
        pj_dpi(&t->e, 8u, 0, LR, LR, 1u);
        pj_ldst(&t->e, PJ_STR, LR, R3, 0u);
#endif
        pj_ldst(&t->e, PJ_LDR, LR, SP, 12u);
        pj_dpi(&t->e, 8u, 0, LR, LR, 6u);
        pje16(&t->e, 0x4700u | (LR << 3)); /* bx lr */
        pje16(&t->e, 0xBF00u);
        for (k=0u;k<nm;++k) pj_patch_b(miss[k], mem_slow);
    }
#endif /* V25 safe extended-memory fast writes */
    {   /* r71: entries; index = size slot (1,2,4) + 3*write */
        static const uint16_t sw[6] = { 1u, 2u, 4u, 0x101u, 0x102u, 0x104u };
        unsigned k;
        for (k = 0; k < 6u; ++k) {
            j->memt[k] = t->e.p;
            pj_movw(&t->e, R2, sw[k]);
#if BB_PJ_MEMWRITE_DIRECT_SLOW_V2 && BB_PJ_SHARED_MEMTRAMP_FAST
#if BB_PJ_V25_FASTWRITE && !defined(B86_PJ_VERIFY)
            pj_patch_b(pj_b(&t->e, -1), k >= 3u ? write_fast : j->memtramp);
#else
            pj_patch_b(pj_b(&t->e, -1), k >= 3u ? write_slow : j->memtramp);
#endif
#else
            pj_patch_b(pj_b(&t->e, -1), j->memtramp);
#endif
        }
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
#if B86_PJ_V45_DYN2
    /* A first-way hit skips the second probe. R1 (target EIP), R0 (retired
     * count), R12 (tag) and the native stack frame are unchanged. R3 is
     * intentionally updated to the successful slot for the ent[] load.
     * A secondary miss takes the ORIGINAL tail_z/C path below. */
    {
        uint8_t *hit1 = pj_b(&t->e, 0 /* EQ */);
        pj_dpi(&t->e, 4u, 0, R3, R3, 1u); /* EOR.W r3,r3,#1: adjacent slot */
        pje32(&t->e, 0xF850u | R2, (LR << 12) | (2u << 4) | R3);
        pj_dpr(&t->e, 13u, 1, PC, LR, R12, 0u, 0u);
        miss1 = pj_b(&t->e, 1 /* NE */);
        pj_patch_b(hit1, t->e.p);
    }
#else
    miss1 = pj_b(&t->e, 1 /* NE */);
#endif
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
#if B86_PJ_V55_PM16_DYN && BB_PJ_PM16_REG_TIER
    /* V55: native PM16 target chaining. PM16 TAG/ENT pairs are indexed
     * within the current (CS base,limit,kind) context, NOT j->tag/ent.
     * C alone selects/allocates pm16_cur before native entry. The context
     * is stable until the native run returns to C. All SMC/IRQ/epoch guards
     * stay enabled; a hash hit is never a substitute for C admission. */
    j->dyn16 = t->e.p;               /* r1 = 16-bit target, r0 = retired */
    {
        unsigned hb16 = 0u, cs_shift = 0u;
        uint8_t *miss_tag, *miss_ent, *f_irq, *f_budget;
        while ((1u << hb16) < PJ_PM16_ENT) ++hb16;
        while ((1u << cs_shift) < 2u * PJ_PM16_ENT * sizeof(uint32_t)) ++cs_shift;
        /* This emitted path is only valid for the compile-time power-of-two
         * PM16 table geometry. Never wrap an invalid context into table 0. */
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, pj));
        pj_ldst(&t->e, PJ_LDR, R3, R2, (unsigned)offsetof(B86Pj, pm16_active));
        pj_dpi(&t->e, 13u, 1, PC, R3, 1u);
        f_irq = pj_b(&t->e, 1 /* NE: not in PM16 */);
        pj_ldst(&t->e, PJ_LDR, R3, R2, (unsigned)offsetof(B86Pj, pm16_cur));
        pj_dpi(&t->e, 13u, 1, PC, R3, PJ_PM16_CTX);
        f_budget = pj_b(&t->e, 2 /* HS: invalid context */);
        pj_shift(&t->e, R3, R3, 0u, cs_shift);
        pj_imm32(&t->e, R2, (uint32_t)(uintptr_t)pj_pm16_tag(j, 0u));
        pj_dpr(&t->e, 8u, 0, R2, R2, R3, 0u, 0u);
        pj_movw(&t->e, R3, 0x79B1u);
        pj_movt(&t->e, R3, 0x9E37u);
        pje32(&t->e, 0xFB00u | R1, 0xF000u | (R3 << 8) | R3); /* hash MUL */
        pj_ubfx(&t->e, R3, R3, 16u, hb16);
        pje32(&t->e, 0xF850u | R2, (LR << 12) | (2u << 4) | R3); /* TAG[hash] */
        pj_dpi(&t->e, 2u, 0, R12, R1, 1u); /* target|1 */
        pj_dpr(&t->e, 13u, 1, PC, LR, R12, 0u, 0u);
        miss_tag = pj_b(&t->e, 1 /* NE: C lookup/validation */);
        pjt_addimm(t, R2, R2, (uint32_t)(PJ_PM16_ENT * sizeof(uint32_t)));
        pje32(&t->e, 0xF850u | R2, (LR << 12) | (2u << 4) | R3); /* ENT[hash] */
        pj_dpi(&t->e, 13u, 1, PC, LR, 0u);
        miss_ent = pj_b(&t->e, 0 /* EQ: no translated block */);
        pj_imm32(&t->e, R12, (uint32_t)(uintptr_t)j->code);
        pj_dpr(&t->e, 8u, 0, LR, LR, R12, 0u, 0u); /* code + ent = Thumb body | 1 */
        /* Same IRQ, stop, budget, and retired-count logic as flat dyn. */
        pj_ldst(&t->e, PJ_LDR, CREG, SP, 0u);
        pj_ldst(&t->e, PJ_LDR, R2, CREG, (unsigned)offsetof(B86Cpu, irq));
        pj_ldst(&t->e, PJ_LDR, R3, CREG, (unsigned)offsetof(B86Cpu, pj_stop));
        pj_dpr(&t->e, 2u, 1, R2, R2, R3, 0u, 0u);
        { uint8_t *f = pj_b(&t->e, 1 /* NE */);
          pj_ldst(&t->e, PJ_LDR, R2, SP, 8u);
          pj_dpi(&t->e, 13u, 1, PC, R2, B86_PJ_CHAIN);
          { uint8_t *g = pj_b(&t->e, 2 /* HS */);
            pj_dpr(&t->e, 8u, 0, R2, R2, R0, 0u, 0u);
            pj_ldst(&t->e, PJ_STR, R2, SP, 8u);
            pje16(&t->e, 0x4700u | (LR << 3)); /* BX translated block */
            pj_patch_b(g, j->tail_z);
          }
          pj_patch_b(f, j->tail_z);
        }
        pj_patch_b(f_irq, j->tail_z);
        pj_patch_b(f_budget, j->tail_z);
        pj_patch_b(miss_tag, j->tail_z);
        pj_patch_b(miss_ent, j->tail_z);
    }
#endif /* V55 native PM16 dynamic chaining */
    j->used = (uint32_t)(t->e.p - j->code);
    j->used = (j->used + 3u) & ~3u;
    j->shared_end = j->used;                      /* r71: ring halves follow the shared code */
    j->half = ((j->cap - j->shared_end) / 2u) & ~3u;
    j->cur_half = 0u; j->half_fill[0] = j->half_fill[1] = 0u;
    j->nlinks = 0u;
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
    memset(pj_cpsv(j), 0, 2u * B86_PJ_CPS * sizeof(uint32_t) + PJ_PM16_BYTES);   /* r71: code-page set, block lists, PM16 contexts */
    ++j->gen;
    memset(j->es_skip_eip, 0, sizeof j->es_skip_eip);
    j->used = 0u;
    j->v28_paged_seen = 0u;
    j->v30c_parked = 0u;
    j->flush_pending = 0;
    c->pj_stop = 0u;
    if (B86_PJ_PCS) memset(pj_gidx(j), 0, PJ_GIDX_BYTES);           /* r72f */
    pj_emit_shared(j);
    if (j->hot) { j->hot_used = j->hot_start; j->nredir = 0u; }
    ++j->flushes;
#if B86_PJ_V31_LAZY_EPOCH
    /* A flush destroys all cached code and therefore needs no lazy fence. */
    pj_v31_active = 0u;
    pj_v31_memo_reset(j);
#endif
}

/* V27: invoked from the *shared* 16-bit JIT dispatcher on real-mode
 * entry, not from a protected-mode instruction or the hot native emitter.
 * The pending bit coalesces arbitrarily many host slices of real-mode code. */
void b86_pj_mark_rm_epoch(B86Cpu *c)
{
    B86Pj *j = c ? b86_pj_state(c) : NULL;
    if (!j) return;
    if (!j->v27_rm_pending) {
        ++j->v27_rm_episodes;
#if B86_PJ_V33_PHYSDIRTY && B86_PJ_V37_EPOCH_LEDGER
        b86_v42_closing_kind = B86_V42_CLOSE_PM;
        b86_v37_rotate(c); /* PM window closes; real-mode writes start anew. */
#endif
    }
    j->v27_rm_pending = 1u;
}

/* V29: Record the physical source page (and its paging mode) at native
 * translation time. For an original 160-byte-or-smaller block, at most two
 * physical pages can contribute instruction bytes. Each block tail is:
 *     [physical id 0, physical id 1 or ~0] [V28 exact x86 bytes].
 * Bit 31 of physical id encodes whether PG was active when translating.
 * Only a matching current mapping AND byte-for-byte identity can reuse it.
 * The identifiers are NOT checksums or probabilistic page hashes.
 * This is a cold entry/epoch check, never a per-x86-opcode hot-path check. */
#define PJ_V29_PAGED 0x80000000u
#define PJ_V29_ABSENT 0xFFFFFFFFu
static B86_PJ_COLD int pj_v29_phys_id(B86Cpu *c, uint32_t lin, uint32_t *id)
{
    uint32_t phys;
    if (c->pg) {
        if (!b86_pg_walk(c, lin, &phys)) return 0;
    } else phys = lin;
    if (phys >= B86_RAM_BYTES) return 0;
    *id = (phys >> 12) | (c->pg ? PJ_V29_PAGED : 0u);
    return 1;
}
/* V30G FASTWALK V1: 16-entry scan-local successful page-ID memoization.
 * No guest execution or code writes occur during this single reconciliation
 * walk. We discard the table upon return, never across RM epochs, CR3 changes,
 * INVLPG, or SMC. Failure results are never cached.  Exact x86 source bytes
 * are ALWAYS rechecked; this is not a hash-based validity shortcut.
 * Macro=0 restores original V30F scan code and serves as A/B control. */
#ifndef B86_PJ_V30G_FAST_WALK
#define B86_PJ_V30G_FAST_WALK 1
#endif
#if B86_PJ_V30G_FAST_WALK
#define PJ_V30G_MAPS 16u
typedef struct { uint32_t page_plus_one, id; } PjV30gMap;
static B86_PJ_COLD int pj_v30g_phys_id(B86Cpu *c, uint32_t lin,
                                      uint32_t *id, PjV30gMap *map)
{
    const uint32_t page = lin >> 12;
    PjV30gMap *e = &map[page & (PJ_V30G_MAPS - 1u)];
    if (e->page_plus_one == page + 1u) { *id = e->id; return 1; }
    if (!pj_v29_phys_id(c, lin, id)) return 0;
    e->page_plus_one = page + 1u;
    e->id = *id;
    return 1;
}
/* V30L: exact, bounds-preserving scan comparison, no page-journal claims.
 * A valid block is still compared byte-for-byte against current mapped RAM
 * at every reconciliation.  Mode 0 is the original libc memcmp control;
 * mode 1 compares 32-bit chunks using memcpy to avoid unaligned C lvalues,
 * then checks every remaining byte.  No hash, no skipped checks. */
#ifndef B86_PJ_V30L_EXACT_COMPARE
#define B86_PJ_V30L_EXACT_COMPARE 0
#endif
#if B86_PJ_V30L_EXACT_COMPARE != 0 && B86_PJ_V30L_EXACT_COMPARE != 1
#error B86_PJ_V30L_EXACT_COMPARE must be 0 or 1
#endif
#if B86_PJ_V30L_EXACT_COMPARE
static B86_PJ_COLD int pj_v30l_equal(const uint8_t *a,
                                    const uint8_t *b, uint32_t n)
{
    while (n >= 4u) {
        uint32_t x, y;
        memcpy(&x, a, 4u);
        memcpy(&y, b, 4u);
        if (x != y) return 0;
        a += 4u; b += 4u; n -= 4u;
    }
    while (n) {
        if (*a++ != *b++) return 0;
        --n;
    }
    return 1;
}
#endif
/* Check identical bytes with contiguous host spans. b86_host_phys() may
 * remap at a *host*-aligned 4K boundary through c->pt; a guest 4K boundary
 * need not be host-aligned. Respect both, and B86_MEM_BYTES, so each memcmp
 * sees precisely the bytes the V29 per-byte loop would have read. */
static B86_PJ_COLD int pj_v30g_source_equal(B86Cpu *c, uint32_t lin,
                               uint32_t id0, uint32_t id1,
                               const uint8_t *saved, uint32_t n)
{
    uint32_t k = 0u;
    const uint32_t first = 4096u - (lin & 0xFFFu);
    while (k < n) {
        const uint32_t frame = ((k < first ? id0 : id1) & ~PJ_V29_PAGED);
        const uint32_t phys = (frame << 12) | ((lin + k) & 0xFFFu);
        const uintptr_t hostaddr = (uintptr_t)(c->mem + phys);
        uint32_t span = n - k;
        const uint32_t guestroom = 4096u - (phys & 0xFFFu);
        const uint32_t hostroom = 4096u - (uint32_t)(hostaddr & 0xFFFu);
        if (phys >= B86_RAM_BYTES) return 0;
        if (span > B86_RAM_BYTES - phys) span = B86_RAM_BYTES - phys;
        if (span > guestroom) span = guestroom;
        if (span > hostroom) span = hostroom;
#if B86_RAM_BYTES > B86_MEM_BYTES
        if (phys < B86_MEM_BYTES && span > B86_MEM_BYTES - phys)
            span = B86_MEM_BYTES - phys;
#endif
        if (!span) return 0;
#if B86_PJ_V30L_EXACT_COMPARE
        if (!pj_v30l_equal(b86_host_phys(c, phys), saved + k, span)) return 0;
#else
        if (memcmp(b86_host_phys(c, phys), saved + k, span)) return 0;
#endif
        k += span;
    }
    return 1;
}
#endif
/* Only run once at each RM epoch / TLB epoch / PG mode or CR3 change.
 * A corrupt header or invalid physical provenance fails CLOSED to V27 flush. */
static B86_PJ_COLD int pj_v29_walk(B86Cpu *c, B86Pj *j,
                                  uint32_t lo, uint32_t hi)
{
    uint32_t off = lo;
#if B86_PJ_V30G_FAST_WALK
    PjV30gMap page_map[PJ_V30G_MAPS] = {{0u, 0u}};
#endif
    while (off < hi) {
        uint32_t *hd, size, n, sn, lin, id0, id1, old0, old1;
        const uint8_t *saved, *pin;
        int same = 1;
        if (hi - off < PJ_HDR_BYTES) return 0;
        hd = pj_hdr(j, off);
        size = hd[5]; n = hd[1] & 0x3FFFFFFFu;
        if (size < PJ_HDR_BYTES || size > hi - off || !n || n > 192u)
            return 0;
        if (!(hd[1] & 0x80000000u)) {
            sn = (n + 3u) & ~3u;
            lin = hd[0];
            if (size < PJ_HDR_BYTES + 16u + 8u + sn ||
                n > UINT32_MAX - lin)
                return 0;
            pin = j->code + off + size - sn - 8u;
            saved = pin + 8u;
            memcpy(&old0, pin, 4u); memcpy(&old1, pin + 4u, 4u);
            if (old0 == PJ_V29_ABSENT ||
                (old0 & ~(uint32_t)PJ_V29_PAGED) >= (B86_RAM_BYTES >> 12))
                return 0;
            /* If current virtual-to-physical identity differs, native code
             * is killed; the interpreter will refetch with current mapping. */
#if B86_PJ_V30G_FAST_WALK
            if (!pj_v30g_phys_id(c, lin, &id0, page_map)) same = 0;
#else
            if (!pj_v29_phys_id(c, lin, &id0)) same = 0;
#endif
            else if (id0 != old0) same = 0;
            if (((lin & 0xFFFu) + n) > 4096u) {
                const uint32_t lin2 = (lin & ~0xFFFu) + 4096u;
                if (old1 == PJ_V29_ABSENT ||
                    (old1 & ~(uint32_t)PJ_V29_PAGED) >= (B86_RAM_BYTES >> 12))
                    return 0;
#if B86_PJ_V30G_FAST_WALK
                if (!pj_v30g_phys_id(c, lin2, &id1, page_map)) same = 0;
#else
                if (!pj_v29_phys_id(c, lin2, &id1)) same = 0;
#endif
                else if (id1 != old1) same = 0;
            } else {
                if (old1 != PJ_V29_ABSENT) return 0;
                id1 = PJ_V29_ABSENT;
            }
            if (same) {
#if B86_PJ_V30G_FAST_WALK
                if (!pj_v30g_source_equal(c, lin, id0, id1, saved, n))
                    same = 0;
#else
                uint32_t k;
                const uint32_t first = 4096u - (lin & 0xFFFu);
                for (k = 0u; k < n; ++k) {
                    const uint32_t frame = (k < first ? id0 : id1) & ~PJ_V29_PAGED;
                    const uint32_t address = (frame << 12) | ((lin + k) & 0xFFFu);
                    if (address >= B86_RAM_BYTES ||
                        *b86_host_phys(c, address) != saved[k]) { same = 0; break; }
                }
#endif
            }
            ++j->v28_checks;
            if (!c->pg) ++j->v30_pg0_checks;
            if (old0 & PJ_V29_PAGED) ++j->v29_paged_checks;
            if (!same) {
                pj_kill_block(c, j, off);
                ++j->v28_kills;
                if (!c->pg) ++j->v30_pg0_kills;
                if (old0 & PJ_V29_PAGED) ++j->v29_paged_kills;
            } else {
                ++j->v28_keeps;
                if (!c->pg) ++j->v30_pg0_keeps;
            }
        }
        off += size;
    }
    return off == hi;
}

#if B86_PJ_V31_LAZY_EPOCH
/* Defined next to the existing V12 exact Thumb link-shape checker. */
static B86_PJ_COLD int pj_v31_unlink_all(B86Pj *j);
/* Check the block chosen by the ORIGINAL page-list order, never an older
 * duplicate picked from a separate cache. All executable entry paths use
 * this wrapper after the first lazy epoch. */
static B86_PJ_COLD uint32_t pj_v31_find(B86Cpu *c, B86Pj *j,
                                       uint32_t lin, uint32_t eip,
                                       uint32_t pm16)
{
    unsigned k;
    for (k = 0u; k < 128u; ++k) {
        const uint32_t ent = pj_find_block(j, lin, eip, pm16);
        uint32_t off, size, *hd;
        if (!ent || !pj_v31_active) return ent;
        if (ent <= PJ_HDR_BYTES) goto invalid;
        off = ent - 1u - PJ_HDR_BYTES;
        /* The original walker has already located the page-list entry;
         * only dereference offsets that are inside an allocated half or
         * the live hot tier, including the COMPLETE header. */
        {
            unsigned h, resident = 0u;
            if (pj_in_hot(j, off)) {
                if (j->hot_used >= PJ_HDR_BYTES && off >= j->hot_start && off <= j->hot_used - PJ_HDR_BYTES)
                    resident = 1u;
            } else for (h = 0u; h < 2u; ++h) {
                const uint32_t lo = j->shared_end + j->half * h;
                const uint32_t hi = h == j->cur_half ? j->used :
                                    (j->half_fill[h] ? j->half_fill[h] : lo);
                if (off >= lo && hi >= PJ_HDR_BYTES && off <= hi - PJ_HDR_BYTES)
                    resident = 1u;
            }
            if (!resident) goto invalid;
        }
        hd = pj_hdr(j, off);
        size = hd[5];
        if (size < PJ_HDR_BYTES + 16u || size > 0x10000u ||
            off > UINT32_MAX - size) goto invalid;
        if (pj_in_hot(j, off)) {
            if (off + size > j->hot_used) goto invalid;
        } else {
            unsigned h, contained = 0u;
            for (h = 0u; h < 2u; ++h) {
                const uint32_t lo = j->shared_end + j->half * h;
                const uint32_t hi = h == j->cur_half ? j->used :
                                    (j->half_fill[h] ? j->half_fill[h] : lo);
                if (off >= lo && off + size <= hi) contained = 1u;
            }
            if (!contained) goto invalid;
        }
        /* A valid memo entry is scoped to this RM epoch + code-cache
         * generation; dead entries still cannot be executed. */
        pj_v31_memo_sync(j);
        if (pj_v31_memo[(off >> 2) & (PJ_V31_MEMO_N - 1u)] == off + 1u &&
#if B86_PJ_V47_EPOCH_MEMO
            pj_v47_memo_tag[(off >> 2) & (PJ_V31_MEMO_N - 1u)] == pj_v47_memo_epoch &&
#endif
            !(hd[1] & 0x80000000u)) {
            ++pj_v31_memo_hits;
            return ent;
        }
        ++pj_v31_exact;
        if (!pj_v29_walk(c, j, off, off + size)) goto invalid;
        if (!(hd[1] & 0x80000000u)) {
            pj_v31_memo[(off >> 2) & (PJ_V31_MEMO_N - 1u)] = off + 1u;
#if B86_PJ_V47_EPOCH_MEMO
            pj_v47_memo_tag[(off >> 2) & (PJ_V31_MEMO_N - 1u)] = pj_v47_memo_epoch;
#endif
            return ent;
        }
        ++pj_v31_kills; /* stale version killed; search next live copy */
    }
invalid:
    /* An invalid header, incomplete region, or cyclic/duplicate list is
     * NOT permission to execute old code. Drop everything and retranslate. */
    ++pj_v31_fallbacks;
    b86_pj_flush(c);
    return 0u;
}
#endif

static B86_PJ_COLD void b86_pj_v27_reconcile(B86Cpu *c, B86Pj *j)
{
    uint32_t h;
    if (!j || !j->v27_rm_pending) return;
    j->v27_rm_pending = 0u;
#if B86_PJ_V33_PHYSDIRTY && B86_PJ_V37_EPOCH_LEDGER
    b86_v42_closing_kind = B86_V42_CLOSE_RM;
    b86_v37_rotate(c); /* RM window closes before PM code can be reused. */
#endif
    /* Observe all current mapping state. A full TLB flush covers CR3,
     * paging enable/disable and INVLPG implementations that flush the TLB. */
    j->v29_map_observed = 1u;
    j->v29_seen_pg = (uint32_t)c->pg;
    j->v29_seen_cr3 = c->cr3;
    j->v29_seen_tlb_flush = b86_pg_stat_flush;
    if (!(j->used > j->shared_end ||
          (j->hot && j->hot_used > j->hot_start))) {
        j->v30c_parked = 0u;
        return;
    }
    /* V30C: PG-off is an *inactive* period for saved PG-on translations,
     * not proof their physical source changed. Do not kill or admit them
     * under the wrong paging identity. b86_pj_try() deoptimizes all PJ
     * execution while parked. On PG-on reentry we validate all original
     * physical page identities and every source byte before native reuse.
     * This also covers any intervening real-mode writes without claiming a
     * complete universal write journal. */
    if (!c->pg && j->v28_paged_seen) {
        if (!j->v30c_parked) ++j->v30c_parks;
        j->v30c_parked = 1u;
        return;
    }
#if B86_PJ_V31_LAZY_EPOCH
    /* Native linked exits and the direct C / generated-dyn entry tables
     * must no longer reach unvalidated code. Unlink EVERY logged exit first;
     * an inconsistent log fails to the existing full flush. */
    if (!pj_v31_unlink_all(j)) goto full_flush;
    memset(j->tag, 0, sizeof j->tag);
    memset(j->ent, 0, sizeof j->ent);
    {
        unsigned ctx;
        for (ctx = 0; ctx < PJ_PM16_CTX; ++ctx)
            memset(pj_pm16_tag(j, ctx), 0, 2u * PJ_PM16_ENT * sizeof(uint32_t));
    }
    c->pj_exit = 0u;
    pj_v31_active = 1u;
    pj_v31_memo_reset(j);
    ++pj_v31_epochs;
    if (j->v30c_parked) {
        j->v30c_parked = 0u;
        ++j->v30c_resumes;
    }
    return;
#endif
    /* V30A: Every live block carries its translation-time PG mode, exact
     * physical source-page IDs, and all original instruction bytes.  A
     * PG-on block encountered with PG-off cannot match its recorded ID,
     * so pj_v29_walk kills just that block, without discarding unrelated
     * PG-off translations.  The same exact-byte validation covers code
     * overwritten by a DOS real-mode loader.  A malformed block header
     * still fails closed to the original V27 full flush below.
     * Do NOT bypass this walk on any RM epoch: no universal write journal
     * exists yet for all interpreted / native store paths. */
    for (h = 0u; h < 2u; ++h) {
        const uint32_t lo = j->shared_end + j->half * h;
        const uint32_t hi = h == j->cur_half ? j->used :
                            (j->half_fill[h] ? j->half_fill[h] : lo);
        if (hi < lo || hi > lo + j->half ||
            !pj_v29_walk(c, j, lo, hi)) goto full_flush;
    }
    if (j->hot && !pj_v29_walk(c, j, j->hot_start, j->hot_used))
        goto full_flush;
    if (j->v30c_parked) {
        j->v30c_parked = 0u;
        ++j->v30c_resumes;
    }
    return;
full_flush:
    ++j->v28_fallbacks;
    b86_pj_flush(c);
    ++j->v27_barriers;
#if BB_PJ_PM16_REG_TIER
    j->pm16_active = 0u;
#endif
    j->es_mode = 0u;
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
/* V21 shared execution backend: PM16 reads use the same native memory
 * trampoline as flat32, guarded by hidden segment-cache state at runtime.
 * This is the first shared native backend step, NOT a combined 8086/486
 * decoder, and its speed advantage must be measured on hardware. */
#ifndef BB_PJ_V21_PM16_INLINE_READ
#define BB_PJ_V21_PM16_INLINE_READ 1u
#endif
/* BB_PJ_PM16_PERF_GATE_V1: identical firmware except PM16 admission gate. */
#define BB_PJ_PM16_PERF_GATE 1
/* BB_PJ_PM16_TRUTHDIAG_V1: read-only counters; DEBUG BUILD ONLY. */
#ifndef BB_PJ_PM16_TRUTHDIAG
#define BB_PJ_PM16_TRUTHDIAG 1
#endif
/* V11 only adds counters to translation and logs to existing cold audits. */
#ifndef BB_PJ_LINK_AUDIT_BYTES_V11
#define BB_PJ_LINK_AUDIT_BYTES_V11 1
#endif
#if BB_PJ_PM16_TRUTHDIAG
#include <stdio.h>
static struct {
    uint32_t emit_attempt, emit_ok, emit_fail;
    uint32_t pref66, pref66_mov, pref66_stack, pref66_0f, pref66_other;
    uint32_t read_attempt, read_success, write_attempt, write_success;
    uint32_t v9_movext16, v9_movext32, v10_setcc;
    uint32_t v14_push32_native, v14_pop32_native;
    uint32_t v14_stack_attempt, v14_stack_success;
    uint32_t v143_unaligned_admitted; /* checked, single-page dword; executed via bytes */
    uint32_t v15_pm16_near_jcc_native; /* translation admissions, NOT execution count */
    uint32_t v16_movext_mem_native, v16_memread_attempt, v16_memread_ok;
    uint32_t v17_push_gs_native, v17_pop_gs_native;
    uint32_t v17_stack_attempt, v17_stack_ok, v17_seg_attempt, v17_seg_ok;
    uint32_t v18_bt_native, v18_bt_runtime, v18_bt_mem_refused;
    uint32_t v18_bt_modify_refused, v18_bt_other_refused;
    /* V18.1 read-only PM16 0F BA decline taxonomy, no JIT or CPU state. */
    uint32_t v181_ba_stops, v181_ba_no_prefix, v181_ba_pfx66;
    uint32_t v181_ba_pfx67, v181_ba_pfx_seg, v181_ba_pfx_lockrep;
    uint32_t v181_ba_pfx_multiple, v181_ba_truncated;
    uint32_t v181_ba_bt_reg, v181_ba_bt_mem;
    uint32_t v181_ba_modify_reg, v181_ba_modify_mem;
    uint32_t v181_ba_other_reg, v181_ba_other_mem;
    /* V18.2 PM16 66 0F BA /4: register-only BT r32,imm8. */
    uint32_t v182_bt32_native, v182_bt32_runtime;
    /* V18.3: 66 0F BA /5-/7, register-only modifying bit operations. */
    uint32_t v183_bts_native, v183_btr_native, v183_btc_native;
    uint32_t v183_bts_runtime, v183_btr_runtime, v183_btc_runtime;
    /* V18.4: read-only taxonomy of 0F 00/01/02/20/22 translation stops. */
    uint32_t v184_total[5], v184_pfx66[5], v184_pfx67[5];
    uint32_t v184_pfxseg[5], v184_pfxlockrep[5], v184_pfxmulti[5];
    uint32_t v184_regop[5], v184_memop[5], v184_trunc[5];
    uint32_t v184_group00[8], v184_group01[8];
    uint32_t v184_cr20[8], v184_cr22[8];
    /* V18.5: native PM16 unprefixed 0F 01 /4, register-only SMSW. */
    uint32_t v185_smsw_native, v185_smsw_runtime;
    /* V19: one batch covering safe PM16 0F 00 /0,/1 and 0F 20 CR reads. */
    uint32_t v19_sldt_native, v19_str_native, v19_crread_native;
    uint32_t v19_sldt_runtime, v19_str_runtime, v19_crread_runtime;
    uint32_t v19_cr_priv_refused;
    /* V20: guarded 66 89/8B r32 <-> [EA16] single-page dword MOV. */
    uint32_t v20_mov32_read_native, v20_mov32_write_native;
    /* V21: translation admissions to shared native PM16 read backend. */
    uint32_t v21_pm16_read_native;
    uint32_t v20_mov32_read_runtime, v20_mov32_write_runtime;
    uint32_t v20_mov32_refused, v20_mov32_unaligned;
    /* V14.2 instrumentation ONLY; fast-path decisions and stores unchanged. */
    uint32_t v142_bad_ss, v142_wrap, v142_bounds, v142_overflow;
    uint32_t v142_alignment, v142_protection, v142_mapping;
#if BB_PJ_LINK_AUDIT_BYTES_V11
    uint32_t v11_66_push, v11_66_pop, v11_66_stack_other;
#endif
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

/* V16: protected-mode register-destination MOVZX/MOVSX memory-source tier.
 * The original PM16 read helper remains unchanged. This helper performs the
 * full PM2 access check BEFORE returning a pointer, rejecting any ambiguous
 * descriptor, wrap, permission, page crossing, or out-of-RAM case. A native
 * call only reads after success and never modifies flags or memory. */
#ifndef BB_PJ_PM16_MOVEXT_MEM_V16
#define BB_PJ_PM16_MOVEXT_MEM_V16 1
#endif
#if BB_PJ_PM16_MOVEXT_MEM_V16 && BB_PJ_PM16_MOV_READ
static B86_PM_HOT uint8_t *b86_pj_pm16_movext_ptr(B86Cpu *c,
                                                     uint32_t off, uint32_t desc)
{
    const unsigned seg = (desc >> 8) & 7u, width = desc & 0xFFu;
    const B86PM1Cache *s;
    uint64_t linear;
    uint32_t checked_lin = 0u;
    B86PM2Fault fault;
    uint8_t *p;
    BB_PM16_DIAG_INC(v16_memread_attempt);
    if ((width != 1u && width != 2u) ||
        (seg != B86_DS && seg != B86_SS) || off > 0xFFFFu) return NULL;
    s = &c->pm386.seg[seg];
    /* Strict expand-up readable DATA segment subset. Any valid descriptor
     * outside it is left to the interpreter, not speculatively accepted. */
    if (!s->valid || s->reserved || (s->access & 0x1Au) != 0x12u ||
        (s->access & 0x04u) || off > s->limit ||
        (uint32_t)(width - 1u) > s->limit - off) return NULL;
    linear = (uint64_t)s->base + (uint64_t)off;
    if (linear + width - 1u > UINT32_MAX) return NULL;
    if (b86_pm2_check_access(c, seg, off, width, B86_PM1_READ,
                            &checked_lin, &fault) != B86_PM2_OK ||
        checked_lin != (uint32_t)linear) return NULL;
    /* Both bytes must reside in one linear and one physical page. A 16-bit
     * unaligned source is read as TWO BYTE loads in the generated code. */
    p = b86_pj_mem(c, (uint32_t)linear, width);
    if (p) BB_PM16_DIAG_INC(v16_memread_ok);
    return p;
}
#endif /* BB_PJ_PM16_MOVEXT_MEM_V16 && BB_PJ_PM16_MOV_READ */

/* V14: native 66 PUSH/POP r32 in PM16, guarded at execution time.
 * The helper returns a checked host pointer ONLY.  It never commits a
 * guest GPR, stack pointer, or store on a refusal.  The native caller
 * completes exactly one aligned dword access and then updates registers.
 * SS.B (cache flags bit 2) selects 16- versus 32-bit stack addressing;
 * 66 selects operand width independently.  No wrap/cross-boundary fast path.
 */
#ifndef BB_PJ_PM16_STACK32_V14
#define BB_PJ_PM16_STACK32_V14 1
#endif
#ifndef BB_PJ_PM16_STACK_REJECT_DIAG_V142
#define BB_PJ_PM16_STACK_REJECT_DIAG_V142 1
#endif
#if BB_PJ_PM16_STACK32_V14
#if BB_PJ_PM16_STACK_REJECT_DIAG_V142 && BB_PJ_PM16_TRUTHDIAG
#define PJ_V142_REFUSE(reason) do { BB_PM16_DIAG_INC(reason); return NULL; } while (0)
#else
#define PJ_V142_REFUSE(reason) do { return NULL; } while (0)
#endif
static B86_PM_HOT uint8_t *b86_pj_pm16_stack32_ptr(B86Cpu *c,
                                                     uint32_t esp, uint32_t is_push)
{
    const B86PM1Cache *s = &c->pm386.seg[B86_SS];
    uint32_t off;
    uint64_t lin;
    uint32_t checked_lin = 0u;
    B86PM2Fault fault;
    uint8_t *p;
    BB_PM16_DIAG_INC(v14_stack_attempt);
    /* This is an intentionally strict subset of usable SS descriptors:
     * present, S=1, writable data, expand-up; no reserved descriptor.
     * Other valid SS types and any cache ambiguity go to the interpreter. */
    if (!s->valid || s->reserved || (s->access & 0x9Au) != 0x92u ||
        (s->access & 0x04u) || is_push > 1u) PJ_V142_REFUSE(v142_bad_ss);
    if (s->flags & 4u) {
        off = is_push ? esp - 4u : esp;
    } else {
        /* SP wraps at 16 bits independently of the 32-bit operand width.
         * Refuse a dword crossing 64 KiB even if the cached limit is larger. */
        off = (is_push ? esp - 4u : esp) & 0xFFFFu;
        if (off > 0xFFFCu) PJ_V142_REFUSE(v142_wrap);
    }
    if (off > s->limit || s->limit - off < 3u) PJ_V142_REFUSE(v142_bounds);
    lin = (uint64_t)s->base + (uint64_t)off;
    if (lin > UINT32_MAX - 3u) PJ_V142_REFUSE(v142_overflow);
    /* Refuse cross-page dwords; unaligned but in-page dwords are
     * handled with four byte accesses (NEVER an unaligned ARM word). */
    if (((uint32_t)lin & 0xFFFu) > 0xFFCu) PJ_V142_REFUSE(v142_alignment);
    /* The full protected-mode access checker is the authority for SS
     * selector/privilege/segment AND paged read-versus-write permission.
     * It is already used by PJ's existing guarded ES memory tier.
     * On any disagreement, defer to the interpreter without a guest store. */
    if (b86_pm2_check_access(c, B86_SS, off, 4u,
            is_push ? B86_PM1_WRITE : B86_PM1_READ,
            &checked_lin, &fault) != B86_PM2_OK || checked_lin != (uint32_t)lin)
        PJ_V142_REFUSE(v142_protection);
    /* Retain V13's existing paging, physical RAM and SMC checking.
     * For PUSH, SMC notification happens before the write as elsewhere in PJ;
     * caller exits its block immediately after the successful store. */
    p = b86_pj_mem(c, (uint32_t)lin, 4u | (is_push ? 0x100u : 0u));
    if (p) {
        BB_PM16_DIAG_INC(v14_stack_success);
        if (lin & 3u) BB_PM16_DIAG_INC(v143_unaligned_admitted);
    } else PJ_V142_REFUSE(v142_mapping);
    return p;
}
#undef PJ_V142_REFUSE
#endif /* BB_PJ_PM16_STACK32_V14 */

/* V17: unprefixed PM16 0F A8/A9, default 16-bit PUSH/POP GS.  The
 * 16-bit operand width is INDEPENDENT of the stack address width (SS.B).
 * Validate both SS permission and paging before exposing any host bytes.
 * A failed guard is an interpreter retry, not a guest architectural fault. */
#ifndef BB_PJ_PM16_GS_STACK_V17
#define BB_PJ_PM16_GS_STACK_V17 1
#endif
#if BB_PJ_PM16_GS_STACK_V17
static B86_PM_HOT uint8_t *b86_pj_pm16_gs16_ptr(B86Cpu *c,
                                                   uint32_t esp, uint32_t push)
{
    const B86PM1Cache *s = &c->pm386.seg[B86_SS];
    uint32_t off, checked_lin = 0u;
    uint64_t linear;
    B86PM2Fault fault;
    uint8_t *p;
    BB_PM16_DIAG_INC(v17_stack_attempt);
    if (push > 1u || !s->valid || s->reserved ||
        (s->access & 0x9Au) != 0x92u || (s->access & 0x04u)) return NULL;
    if (s->flags & 4u) {
        off = push ? esp - 2u : esp;
    } else {
        off = (push ? esp - 2u : esp) & 0xFFFFu;
        /* Unlike an ARM halfword access, guest SP can be odd. Only
         * refuse a 64-KiB boundary crossing, not an odd stack offset. */
        if (off > 0xFFFEu) return NULL;
    }
    if (off > s->limit || s->limit - off < 1u) return NULL;
    linear = (uint64_t)s->base + (uint64_t)off;
    if (linear > UINT32_MAX - 1u || ((uint32_t)linear & 0xFFFu) > 0xFFEu)
        return NULL;
    if (b86_pm2_check_access(c, B86_SS, off, 2u,
            push ? B86_PM1_WRITE : B86_PM1_READ,
            &checked_lin, &fault) != B86_PM2_OK || checked_lin != (uint32_t)linear)
        return NULL;
    /* Reuse V13 paging and SMC protocol; 2x byte op avoids unaligned ARM
     * halfword loads/stores. All failures leave ESP and segment unchanged. */
    p = b86_pj_mem(c, (uint32_t)linear, 2u | (push ? 0x100u : 0u));
    if (p) BB_PM16_DIAG_INC(v17_stack_ok);
    return p;
}
/* Existing PM26-compatible descriptor plan + commit.  Returning zero
 * leaves native ESP untouched, so C interpreter can perform its own fault.
 * Caller must exit the current native block after a successful load. */
static B86_PM_HOT uint32_t b86_pj_pm16_gs16_commit(B86Cpu *c, uint32_t selector)
{
    uint32_t ok;
    BB_PM16_DIAG_INC(v17_seg_attempt);
    ok = b86_pj_loadseg(c, B86_GS, selector & 0xFFFFu);
    if (ok) BB_PM16_DIAG_INC(v17_seg_ok);
    return ok;
}
#endif /* BB_PJ_PM16_GS_STACK_V17 */

/* V18: narrowly scoped unprefixed PM16 0F BA /4 BT r16,imm8.
 * Only register form: no paged memory, guest write, segment override or
 * prefix; /5 BTS, /6 BTR, /7 BTC and memory BT stay in C. BT defines CF
 * alone; always materialize pending PJ flags first so OF/SF/ZF/AF/PF
 * survive and no stale lazy producer can override the newly written CF. */
#ifndef BB_PJ_PM16_BT_IMM_V18
#define BB_PJ_PM16_BT_IMM_V18 1u
#endif
#if BB_PJ_PM16_BT_IMM_V18
static B86_PM_HOT void b86_pj_pm16_bt16_cf(B86Cpu *c,
                                            uint32_t value, uint32_t bit)
{
    BB_PM16_DIAG_INC(v18_bt_runtime);
    b86_flags_materialize(c);
    c->flags = (c->flags & ~(uint32_t)B86_CF) |
               ((value >> (bit & 15u)) & 1u);
}
#endif /* BB_PJ_PM16_BT_IMM_V18 */

/* V18.2: 66h in PM16 selects BT r32,imm8. Keep the already tested V18
 * BT r16 path unchanged. Only CF is defined; preserve all remaining EFLAGS,
 * including lazily produced flags, via the existing materializer. */
#ifndef BB_PJ_PM16_BT32_IMM_V182
#define BB_PJ_PM16_BT32_IMM_V182 1u
#endif
#if BB_PJ_PM16_BT32_IMM_V182
static B86_PM_HOT void b86_pj_pm16_bt32_cf(B86Cpu *c,
                                            uint32_t value, uint32_t bit)
{
    BB_PM16_DIAG_INC(v182_bt32_runtime);
    b86_flags_materialize(c);
    c->flags = (c->flags & ~(uint32_t)B86_CF) |
               ((value >> (bit & 31u)) & 1u);
}
#endif /* BB_PJ_PM16_BT32_IMM_V182 */

/* V18.3: register-only 66 0F BA /5-/7 (BTS/BTR/BTC r32,imm8).
 * Materialize pending flags; CF receives ORIGINAL bit. Return new guest
 * register value to the native code, which commits it after the helper call.
 * Modifying memory forms are deliberately excluded. */
#ifndef BB_PJ_PM16_BITMOD32_IMM_V183
#define BB_PJ_PM16_BITMOD32_IMM_V183 1u
#endif
#if BB_PJ_PM16_BITMOD32_IMM_V183 && BB_PJ_PM16_BT32_IMM_V182
static B86_PM_HOT uint32_t b86_pj_pm16_bitmodify32(B86Cpu *c,
                                                      uint32_t value,
                                                      uint32_t spec)
{
    const uint32_t bit = spec & 31u;
    const uint32_t form = (spec >> 8u) & 7u;
    const uint32_t mask = 1u << bit;
    b86_flags_materialize(c);
    c->flags = (c->flags & ~(uint32_t)B86_CF) | ((value >> bit) & 1u);
    if (form == 5u) {
        BB_PM16_DIAG_INC(v183_bts_runtime);
        return value | mask;
    }
    if (form == 6u) {
        BB_PM16_DIAG_INC(v183_btr_runtime);
        return value & ~mask;
    }
    if (form == 7u) {
        BB_PM16_DIAG_INC(v183_btc_runtime);
        return value ^ mask;
    }
    return value; /* Never admitted by translator; defensive fallback. */
}
#endif /* BB_PJ_PM16_BITMOD32_IMM_V183 */

/* V18.5: register-form SMSW is unprivileged and reads the LOW 16 bits
 * of CR0, exposed through the existing B86Cpu.msw representation.  It
 * changes neither machine status nor flags; no descriptor or guest memory
 * access is performed.  Runtime accessor avoids stale translation-time CR0.
 * Exact decode acceptance lives in pjt_insn_pm16 below. */
#ifndef BB_PJ_PM16_SMSW_REG_V185
#define BB_PJ_PM16_SMSW_REG_V185 1u
#endif
#if BB_PJ_PM16_SMSW_REG_V185 && BB_PJ_PM16_TRUTHDIAG
static B86_PM_HOT uint32_t b86_pj_pm16_smsw16(const B86Cpu *c)
{
    BB_PM16_DIAG_INC(v185_smsw_runtime);
    return (uint32_t)c->msw & 0xFFFFu;
}
#elif BB_PJ_PM16_SMSW_REG_V185
static B86_PM_HOT uint32_t b86_pj_pm16_smsw16(const B86Cpu *c)
{
    return (uint32_t)c->msw & 0xFFFFu;
}
#endif /* BB_PJ_PM16_SMSW_REG_V185 */

/* V19 grouped safe 0F reads. Neither helper modifies guest flags or state.
 * System-selector reads are architecturally unprivileged on 80386.
 * MOV r32,CRn is CPL0-only: recheck at EXECUTION time and deopt before
 * changing the GPR if CPL has changed. Never compile 0F 22 writes here. */
#ifndef BB_PJ_V19_READ_SELECTORS
#define BB_PJ_V19_READ_SELECTORS 1u
#endif
#ifndef BB_PJ_V19_READ_CR
#define BB_PJ_V19_READ_CR 1u
#endif
#if BB_PJ_V19_READ_SELECTORS
static B86_PM_HOT uint32_t b86_pj_v19_selector(B86Cpu *c, uint32_t op)
{
    if (op == 0u) { BB_PM16_DIAG_INC(v19_sldt_runtime); return c->pm386.ldtr_selector; }
    BB_PM16_DIAG_INC(v19_str_runtime);
    return c->tr_sel;
}
#endif
#if BB_PJ_V19_READ_CR
/* AAPCS32: uint64_t return in r0 (value) and r1 (valid/privilege).
 * All failure cases return zero in r1 so the translator exits to the
 * original instruction, preserving the interpreter's #GP / #UD path.
 * The interpreter represents CR0.PG separately as c->pg. */
static B86_PM_HOT uint64_t b86_pj_v19_cr_read(B86Cpu *c, uint32_t cr)
{
    uint32_t v;
    if ((c->seg[B86_CS] & 3u) != 0u) {
        BB_PM16_DIAG_INC(v19_cr_priv_refused);
        return 0u;
    }
    switch (cr) {
    case 0u: v = (uint32_t)c->msw | ((uint32_t)!!c->pg << 31u); break;
    case 2u: v = c->cr2; break;
    case 3u: v = c->cr3; break;
    default: return 0u;
    }
    BB_PM16_DIAG_INC(v19_crread_runtime);
    return (UINT64_C(1) << 32u) | (uint64_t)v;
}
#endif

/* V20 PM16 66 MOV32 memory tier. One helper for register destination
 * read and register source write; exact DS/SS EA16 only, no extra prefixes.
 * Return (success << 32) | read32, AAPCS32 r1:r0; a failed guard returns 0
 * BEFORE any architectural GPR/memory/flag change, then C re-executes.
 * For stores the existing b86_pj_mem() performs SMC notification first.
 * All dwords use byte accesses, so unaligned Cortex-M33 loads never occur. */
#ifndef BB_PJ_V20_MOV32_MEM
#define BB_PJ_V20_MOV32_MEM 0u
#endif
#if BB_PJ_V20_MOV32_MEM
static B86_PM_HOT uint64_t b86_pj_v20_mov32_mem(B86Cpu *c,
             uint32_t off, uint32_t spec, uint32_t value)
{
    const unsigned seg = (spec >> 8u) & 7u;
    const unsigned write = (spec >> 15u) & 1u;
    const B86PM1Cache *s;
    uint32_t lin_checked = 0u, linear;
    uint64_t lin64;
    B86PM2Fault fault;
    uint8_t *p;
    if ((spec & ~(0x8000u | 0x700u)) != 0u ||
        (seg != B86_DS && seg != B86_SS) || off > 0xFFFCu) goto refused;
    s = &c->pm386.seg[seg];
    if (!s->valid || s->reserved || (s->access & 0x1Au) != 0x12u ||
        (s->access & 0x04u) || off > s->limit || s->limit - off < 3u)
        goto refused;
    lin64 = (uint64_t)s->base + (uint64_t)off;
    if (lin64 > UINT32_MAX - 3u) goto refused;
    linear = (uint32_t)lin64;
    if ((linear & 0xFFFu) > 0xFFCu) goto refused;
    if (b86_pm2_check_access(c, seg, off, 4u,
            write ? B86_PM1_WRITE : B86_PM1_READ,
            &lin_checked, &fault) != B86_PM2_OK || lin_checked != linear)
        goto refused;
    p = b86_pj_mem(c, linear, 4u | (write ? 0x100u : 0u));
    if (!p) goto refused;
    if (linear & 3u) BB_PM16_DIAG_INC(v20_mov32_unaligned);
    if (write) {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8u);
        p[2] = (uint8_t)(value >> 16u);
        p[3] = (uint8_t)(value >> 24u);
        BB_PM16_DIAG_INC(v20_mov32_write_runtime);
        return UINT64_C(1) << 32u;
    }
    BB_PM16_DIAG_INC(v20_mov32_read_runtime);
    value = (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
            ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
    return (UINT64_C(1) << 32u) | (uint64_t)value;
refused:
    BB_PM16_DIAG_INC(v20_mov32_refused);
    return 0u;
}
#endif /* BB_PJ_V20_MOV32_MEM */

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
/* V10: 386 PM16 SETcc register-only; no memory/segment access.
 * 66h does not change SETcc's byte operand size. */
#ifndef BB_PJ_PM16_SETCC_V10
#define BB_PJ_PM16_SETCC_V10 1
#endif
#if BB_PJ_PM16_SETCC_V10
static void pjt_pm16_setcc_reg(PjT *t, unsigned cc, unsigned rm)
{
    const int arm = pjt_flags_for(t, cc);
    uint8_t *skip;
    pj_movw(&t->e, R1, 1u);
    skip = pjt_bfwd(t, arm);
    pj_movw(&t->e, R1, 0u);
    pjt_here(t, skip);
    pjt_put8(t, rm, R1);
}
#endif /* BB_PJ_PM16_SETCC_V10 */
/* V18.4: declined instruction classifier only. Does not fetch guest memory,
 * modify flags/registers, generate code, or change any accept/refuse decision.
 * It records one event only on the EXISTING r==0 translator stop path. */
#ifndef BB_PJ_V184_STOP_CLASSIFY
#define BB_PJ_V184_STOP_CLASSIFY 1u
#endif
#if BB_PJ_V184_STOP_CLASSIFY && BB_PJ_PM16_TRUTHDIAG
static B86_PJ_COLD void pj_v184_note_0f_stop(const uint8_t *bytes,
                                                unsigned n, unsigned start,
                                                unsigned op_at)
{
    static const uint8_t wanted[5] = { 0x00u, 0x01u, 0x02u, 0x20u, 0x22u };
    unsigned i, kind, pfx=0u, mask=0u, mr, ext;
    if (op_at >= n || op_at + 1u >= n || bytes[op_at] != 0x0Fu) return;
    for (kind=0u; kind<5u && wanted[kind]!=bytes[op_at+1u]; ++kind) { }
    if (kind==5u) return;
    ++bb_pm16_diag.v184_total[kind];
    for (i=start; i<op_at; ++i) {
        const unsigned b=bytes[i];
        ++pfx;
        if (b==0x66u) mask|=1u;
        else if (b==0x67u) mask|=2u;
        else if (b==0x26u || b==0x2Eu || b==0x36u || b==0x3Eu ||
                 b==0x64u || b==0x65u) mask|=4u;
        else if (b==0xF0u || b==0xF2u || b==0xF3u) mask|=8u;
    }
    if (mask&1u) ++bb_pm16_diag.v184_pfx66[kind];
    if (mask&2u) ++bb_pm16_diag.v184_pfx67[kind];
    if (mask&4u) ++bb_pm16_diag.v184_pfxseg[kind];
    if (mask&8u) ++bb_pm16_diag.v184_pfxlockrep[kind];
    if (pfx>1u) ++bb_pm16_diag.v184_pfxmulti[kind];
    if (op_at+2u >= n) { ++bb_pm16_diag.v184_trunc[kind]; return; }
    mr=bytes[op_at+2u]; ext=(mr>>3u)&7u;
    if ((mr>>6u)==3u) ++bb_pm16_diag.v184_regop[kind];
    else ++bb_pm16_diag.v184_memop[kind];
    if (kind==0u) ++bb_pm16_diag.v184_group00[ext];
    if (kind==1u) ++bb_pm16_diag.v184_group01[ext];
    if (kind==3u) ++bb_pm16_diag.v184_cr20[ext];
    if (kind==4u) ++bb_pm16_diag.v184_cr22[ext];
}
#endif /* BB_PJ_V184_STOP_CLASSIFY */

/* V18.1 translation-stop-only 0F BA classifier. Reads fetched bytes only.
 * Unchanged guest execution, native emission, code cache and JIT state.
 * Categories are disjoint by ModRM.mod and ModRM.reg; prefix masks overlap. */
#ifndef BB_PJ_V181_BA_STOP_CLASSIFY
#define BB_PJ_V181_BA_STOP_CLASSIFY 1u
#endif
#if BB_PJ_V181_BA_STOP_CLASSIFY && BB_PJ_PM16_TRUTHDIAG
static B86_PJ_COLD void pj_v181_note_ba_stop(const uint8_t *bytes,
                                              unsigned n, unsigned start, unsigned op_at)
{
    unsigned i, mask = 0u, pfx = 0u, mr, reg;
    if (op_at >= n || bytes[op_at] != 0x0Fu ||
        op_at + 1u >= n || bytes[op_at + 1u] != 0xBAu) return;
    ++bb_pm16_diag.v181_ba_stops;
    for (i = start; i < op_at; ++i) {
        ++pfx;
        if (bytes[i] == 0x66u) mask |= 1u;
        else if (bytes[i] == 0x67u) mask |= 2u;
        else if (bytes[i] == 0x26u || bytes[i] == 0x2Eu ||
                 bytes[i] == 0x36u || bytes[i] == 0x3Eu ||
                 bytes[i] == 0x64u || bytes[i] == 0x65u) mask |= 4u;
        else if (bytes[i] == 0xF0u || bytes[i] == 0xF2u ||
                 bytes[i] == 0xF3u) mask |= 8u;
    }
    if (!pfx) ++bb_pm16_diag.v181_ba_no_prefix;
    if (mask & 1u) ++bb_pm16_diag.v181_ba_pfx66;
    if (mask & 2u) ++bb_pm16_diag.v181_ba_pfx67;
    if (mask & 4u) ++bb_pm16_diag.v181_ba_pfx_seg;
    if (mask & 8u) ++bb_pm16_diag.v181_ba_pfx_lockrep;
    if (pfx > 1u) ++bb_pm16_diag.v181_ba_pfx_multiple;
    if (op_at + 2u >= n) {
        ++bb_pm16_diag.v181_ba_truncated;
        return;
    }
    mr=bytes[op_at + 2u];
    reg=(mr >> 3) & 7u;
    if (reg == 4u) {
        if ((mr >> 6) == 3u) ++bb_pm16_diag.v181_ba_bt_reg;
        else ++bb_pm16_diag.v181_ba_bt_mem;
    } else if (reg >= 5u) {
        if ((mr >> 6) == 3u) ++bb_pm16_diag.v181_ba_modify_reg;
        else ++bb_pm16_diag.v181_ba_modify_mem;
    } else {
        if ((mr >> 6) == 3u) ++bb_pm16_diag.v181_ba_other_reg;
        else ++bb_pm16_diag.v181_ba_other_mem;
    }
}
#endif /* V18.1 classifier */

#if B86_PJ_V54_NATIVE_CONTROL && BB_PJ_PM16_GS_STACK_V17
/* V54 shared PM16 checked stack-pointer transition after a successful
 * 16-bit operand access. SS.B selects full ESP or low-SP16 wrapping.
 * No flags or other guest registers are modified. */
static void pjt_v54_sp_step(PjT *t, int is_push)
{
    pjt_c(t);
    pj_ldst(&t->e, PJ_LDRB, R2, CREG,
            (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
    pj_dpi(&t->e, 0u, 1, PC, R2, 4u); /* TST SS.B */
    {
        uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B=1 */);
        pjt_addimm(t, R2, GR(4), is_push ? (uint32_t)-2 : 2u);
        pj_bfi(&t->e, GR(4), R2, 0u, 16u);
        {
            uint8_t *done = pjt_bfwd(t, -1);
            pjt_here(t, addr32);
            pjt_addimm(t, GR(4), GR(4), is_push ? (uint32_t)-2 : 2u);
            pjt_here(t, done);
        }
    }
    t->fuse_at = NULL;
}
/* Any odd pointer is safe: guest words are always read/stored as bytes. */
static void pjt_v54_store16(PjT *t) /* host ptr=r0; value=r1 */
{
    pj_ldst(&t->e, PJ_STRB, R1, R0, 0u);
    pj_shift(&t->e, R2, R1, 1u, 8u);
    pj_ldst(&t->e, PJ_STRB, R2, R0, 1u);
}
static void pjt_v54_read16(PjT *t) /* host ptr=r0; result=r1 */
{
    pj_ldst(&t->e, PJ_LDRB, R1, R0, 0u);
    pj_ldst(&t->e, PJ_LDRB, R2, R0, 1u);
    pj_dpr(&t->e, 2u, 0, R1, R1, R2, 0u, 8u);
}
/* Guard first; any rejection returns the original x86 instruction to the
 * interpreter with no guest memory/register/ESP commit. */
static void pjt_v54_stack_check(PjT *t, int push)
{
    pjt_c(t);
    pj_mov(&t->e, R0, CREG);
    pj_mov(&t->e, R1, GR(4));
    pj_movw(&t->e, R2, push ? 1u : 0u);
    pjt_call(t, (const void *)b86_pj_pm16_gs16_ptr);
    pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
    pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
}
#endif /* V54 guarded 16-bit stack emission */
#if B86_PJ_V56_STACK_CHAIN && B86_PJ_V54_NATIVE_CONTROL && BB_PJ_PM16_GS_STACK_V17
/* RET imm16 adjusts the address-size-selected stack pointer AFTER popping
 * the target. No guest memory access is involved in the additional adjust.
 * Unlike a raw 16-bit add to ESP, SS.B=0 wraps only SP; SS.B=1 uses ESP. */
static void pjt_v56_sp_add(PjT *t, uint32_t bytes)
{
    pjt_c(t);
    pj_ldst(&t->e, PJ_LDRB, R2, CREG,
            (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
    pj_dpi(&t->e, 0u, 1, PC, R2, 4u);
    {
        uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B=1 */);
        pjt_addimm(t, R2, GR(4), bytes);
        pj_bfi(&t->e, GR(4), R2, 0u, 16u);
        {
            uint8_t *done = pjt_bfwd(t, -1);
            pjt_here(t, addr32);
            pjt_addimm(t, GR(4), GR(4), bytes);
            pjt_here(t, done);
        }
    }
    t->fuse_at = NULL;
}
#endif /* V56 checked PM16 stack additions */

static B86_PJ_COLD int pjt_insn_pm16(PjT *t)
{
    uint32_t op, modrm, imm;
    const unsigned start = t->pos, ex0 = t->nex;
    uint8_t *const p0 = t->e.p;
    const int lk0 = t->last_kind, ik0 = t->ikind_clear, cr0 = t->creg_ok;
    uint8_t *const fz0 = t->fuse_at;
    const unsigned base_ip = t->ip0;
    unsigned w, alu, form, a, b;
#define PJ16_FAIL() do { BB_PM16_DIAG_INC(emit_fail); t->pos=start; --t->ninsn; t->nex=ex0; \
    t->e.p=p0; t->last_kind=lk0; t->ikind_clear=ik0; t->creg_ok=cr0; t->fuse_at=fz0; return 0; } while(0)
#define PJ16_NEXT() ((base_ip + (t->pos - start)) & 0xFFFFu)
    BB_PM16_DIAG_INC(emit_attempt);
    ++t->ninsn;
    {   /* Preserve PM16's exact one-byte admission, including 66-prefixed
         * specialized handlers and their rollback behavior. */
        PjUnifiedHead head;
        if (!pjt_unified_head(t, PJ_HEAD_PM16, &head)) PJ16_FAIL();
        op = head.opcode;
    }
    /* Count the opcode AFTER 66; these are decoder translation attempts,
     * NOT executed-instruction counts. Keep separate from runtime helpers. */
    if (op == 0x66u) {
        const unsigned x = t->pos < t->n ? t->b[t->pos] : 0xFFu;
        BB_PM16_DIAG_INC(pref66);
#if BB_PJ_LINK_AUDIT_BYTES_V11
        if (x >= 0x50u && x <= 0x57u) BB_PM16_DIAG_INC(v11_66_push);
        else if (x >= 0x58u && x <= 0x5Fu) BB_PM16_DIAG_INC(v11_66_pop);
        else if (x == 0x68u || x == 0x6Au || x == 0x9Cu || x == 0x9Du)
            BB_PM16_DIAG_INC(v11_66_stack_other);
#endif
        if (x >= 0x88u && x <= 0x8Bu) BB_PM16_DIAG_INC(pref66_mov);
        else if (x >= 0x50u && x <= 0x5Fu) BB_PM16_DIAG_INC(pref66_stack);
        else if (x == 0x0Fu) BB_PM16_DIAG_INC(pref66_0f);
        else BB_PM16_DIAG_INC(pref66_other);
    }

/* V9: 16-bit protected-mode, register-only MOVZX/MOVSX.
 * Memory operands and invalid width combinations remain in the interpreter.
 * These instructions do not change x86 flags; source is captured before dest. */
#ifndef BB_PJ_PM16_MOVEXT_V9
#define BB_PJ_PM16_MOVEXT_V9 1
#endif
#if BB_PJ_PM16_OP66REG
    /* In 16-bit code, 66 changes operand size to 32. This opt-in path
     * accepts only forms with ModRM.mod=3 (registers), or imm32 to GPR.
     * On any decoding failure PJ16_FAIL completely discards emitted code. */
    if (op == 0x66u) {
        uint32_t op66, mr, imm66;
        unsigned a66, b66, alu66, form66;
        if (!pjt_u8(t, &op66)) PJ16_FAIL();
#if BB_PJ_PM16_MOVEXT_V9
        /* 66 0F B6/B7/BE/BF: operand override selects r32 destination.
         * Source is independently byte/word-sized; ModRM.mod=3 only. */
        if (op66 == 0x0Fu) {
            uint32_t op2, mr;
            if (!pjt_u8(t, &op2)) PJ16_FAIL();
#if BB_PJ_PM16_BT32_IMM_V182
            /* Exclusive admission: 66 0F BA /4, ModRM.mod=3, imm8.
             * No 67/segment/LOCK/REP or multi-prefix forms can enter this
             * 66-prefixed PM16 decoder. /5-/7 and all memory forms still
             * decline unchanged. Consume complete instruction before emit. */
            if (op2 == 0xBAu) {
                uint32_t bit32;
                if (!pjt_u8(t, &mr)) PJ16_FAIL();
#if BB_PJ_PM16_BITMOD32_IMM_V183
                /* The prior /4 BT path below is unchanged. Handle only
                 * /5-/7 with ModRM.mod=3, after fetching full imm8.
                 * Misdecoded or truncated forms still PJ16_FAIL to C. */
                if ((mr >> 6) == 3u && ((mr >> 3) & 7u) >= 5u) {
                    uint32_t form183 = (mr >> 3) & 7u;
                    if (!pjt_u8(t, &bit32)) PJ16_FAIL();
                    pjt_getr(t, R1, mr & 7u, 4u);
                    pj_movw(&t->e, R2, (form183 << 8u) | (bit32 & 31u));
                    pjt_c(t);
                    pj_mov(&t->e, R0, CREG);
                    pjt_call(t, (const void *)b86_pj_pm16_bitmodify32);
                    /* AAPCS: helper returns new r32 in r0; guest GPRs
                     * r4-r11 survive the call. No CREG usage until exit. */
                    pjt_putr(t, mr & 7u, 4u, R0);
                    t->last_kind = -1;
                    t->ikind_clear = 1;
                    t->fuse_at = NULL;
                    pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
                    if (form183 == 5u) BB_PM16_DIAG_INC(v183_bts_native);
                    else if (form183 == 6u) BB_PM16_DIAG_INC(v183_btr_native);
                    else BB_PM16_DIAG_INC(v183_btc_native);
                    do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
                }
#endif /* BB_PJ_PM16_BITMOD32_IMM_V183 */
                if (((mr >> 3) & 7u) != 4u || (mr >> 6) != 3u)
                    PJ16_FAIL();
                if (!pjt_u8(t, &bit32)) PJ16_FAIL();
                pjt_getr(t, R1, mr & 7u, 4u);
                pj_movw(&t->e, R2, bit32 & 31u);
                pjt_c(t);
                pj_mov(&t->e, R0, CREG);
                pjt_call(t, (const void *)b86_pj_pm16_bt32_cf);
                t->last_kind = -1;
                t->ikind_clear = 1;
                t->fuse_at = NULL;
                pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
                BB_PM16_DIAG_INC(v182_bt32_native);
                do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
            }
#endif /* BB_PJ_PM16_BT32_IMM_V182 */
#if BB_PJ_PM16_SETCC_V10
            if (op2 >= 0x90u && op2 <= 0x9Fu) {
                if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
                pjt_pm16_setcc_reg(t, op2 & 15u, mr & 7u);
                BB_PM16_DIAG_INC(v10_setcc);
                do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
            }
#endif /* BB_PJ_PM16_SETCC_V10 */
            if (op2 != 0xB6u && op2 != 0xB7u &&
                op2 != 0xBEu && op2 != 0xBFu) PJ16_FAIL();
#if BB_PJ_PM16_MOVEXT_MEM_V16 && BB_PJ_PM16_MOV_READ
            /* 66 0F B6/B7/BE/BF /r: MOVZX/MOVSX r32,[EA16].
             * Unprefixed (or 67/segment override) forms are not admitted.
             * Read only, all 8 destinations and all 16-bit EA base forms.
             * Runtime segment, privilege and page checks run before a load.
             * The original register-only V9 path remains unchanged. */
            if (t->pos < t->n && (t->b[t->pos] >> 6) != 3u) {
                const unsigned width16 = (op2 & 1u) ? 2u : 1u;
                unsigned dst16, seg16;
                if (!pjt_u8(t, &mr)) PJ16_FAIL();
                dst16 = (mr >> 3u) & 7u;
                if (!pjt_unified_ea16(t, mr, &seg16)) PJ16_FAIL();
                pjt_c(t);
                pj_mov(&t->e, R0, CREG);
                pj_movw(&t->e, R2, width16 | (seg16 << 8));
                pjt_call(t, (const void *)b86_pj_pm16_movext_ptr);
                pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
                /* On any check refusal, original guest registers/flags
                 * remain intact; retry in the protected-mode interpreter. */
                pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
                pj_ldst(&t->e, PJ_LDRB, R1, R0, 0u);
                if (width16 == 2u) {
                    /* 2x LDRB is ARM-alignment safe for odd guest addresses. */
                    pj_ldst(&t->e, PJ_LDRB, R2, R0, 1u);
                    pj_dpr(&t->e, 2u, 0, R1, R1, R2, 0u, 8u);
                }
                if (op2 == 0xBEu) pj_ext(&t->e, 0xFA4Fu, R1, R1);
                if (op2 == 0xBFu) pj_ext(&t->e, 0xFA0Fu, R1, R1);
                pjt_putr(t, dst16, 4u, R1);
                BB_PM16_DIAG_INC(v16_movext_mem_native);
                do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
            }
#endif /* BB_PJ_PM16_MOVEXT_MEM_V16 */
            if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
            pjt_getr(t, R1, mr & 7u, (op2 & 1u) ? 2u : 1u);
            if (op2 == 0xBEu) pj_ext(&t->e, 0xFA4Fu, R1, R1);
            if (op2 == 0xBFu) pj_ext(&t->e, 0xFA0Fu, R1, R1);
            pjt_putr(t, (mr >> 3) & 7u, 4u, R1);
            BB_PM16_DIAG_INC(v9_movext32);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif /* BB_PJ_PM16_MOVEXT_V9 */
#if BB_PJ_PM16_STACK32_V14
        /* 66 50..5F: 32-bit data operand on PM16 code, with independently
         * selected SS.B stack address width.  All eight guest registers.
         * Host r4..r11 are the live guest registers; helper reads only
         * the cached SS descriptor and paging.  No GPR is changed before
         * a checked pointer is returned.  On failure, replay in C at ip0. */
        if (op66 >= 0x50u && op66 <= 0x5Fu) {
            const unsigned reg32 = op66 & 7u;
            const int push32 = op66 < 0x58u;
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);                 /* c */
            pj_mov(&t->e, R1, GR(4));                /* original full ESP */
            pj_movw(&t->e, R2, push32 ? 1u : 0u);
            pjt_call(t, (const void *)b86_pj_pm16_stack32_ptr);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);    /* CMP pointer,#0 */
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            /* V14.3: most PM16 stacks are NOT dword-aligned.  Keep the
             * original word instruction when aligned.  Otherwise use only
             * LDRB/STRB on an already validated, single-page host pointer;
             * ARM unaligned word accesses are NEVER issued.  No access or
             * guest-state commit occurs on a refused pointer. */
            if (push32) {
                uint8_t *unaligned, *done;
                /* PUSH ESP stores the OLD ESP, not the decremented value. */
                pj_mov(&t->e, R1, GR(reg32));
                pj_dpi(&t->e, 0u, 1, PC, R0, 3u); /* TST host pointer,#3 */
                unaligned = pjt_bfwd(t, 1 /* NE */);
                pjt_st(t, R1, R0, 4u);
                done = pjt_bfwd(t, -1);
                pjt_here(t, unaligned);
                pj_ldst(&t->e, PJ_STRB, R1, R0, 0u);
                pj_shift(&t->e, R2, R1, 1u, 8u);
                pj_ldst(&t->e, PJ_STRB, R2, R0, 1u);
                pj_shift(&t->e, R2, R1, 1u, 16u);
                pj_ldst(&t->e, PJ_STRB, R2, R0, 2u);
                pj_shift(&t->e, R2, R1, 1u, 24u);
                pj_ldst(&t->e, PJ_STRB, R2, R0, 3u);
                pjt_here(t, done);
            } else {
                uint8_t *unaligned, *done;
                /* POP ESP: read before increment, then override ESP last. */
                pj_dpi(&t->e, 0u, 1, PC, R0, 3u);
                unaligned = pjt_bfwd(t, 1 /* NE */);
                pjt_ld(t, R1, R0, 4u);
                done = pjt_bfwd(t, -1);
                pjt_here(t, unaligned);
                pj_ldst(&t->e, PJ_LDRB, R1, R0, 0u);
                pj_ldst(&t->e, PJ_LDRB, R2, R0, 1u);
                pj_dpr(&t->e, 2u, 0, R1, R1, R2, 0u, 8u);
                pj_ldst(&t->e, PJ_LDRB, R2, R0, 2u);
                pj_dpr(&t->e, 2u, 0, R1, R1, R2, 0u, 16u);
                pj_ldst(&t->e, PJ_LDRB, R2, R0, 3u);
                pj_dpr(&t->e, 2u, 0, R1, R1, R2, 0u, 24u);
                pjt_here(t, done);
            }
            /* Determine address-size from SS.B using the SAME cached SS
             * descriptor examined by the pointer helper.  No helper call
             * occurs after the native memory access; R1 stays live for POP. */
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDRB, R2, CREG,
                    (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
            /* flags & 4: SS.B=1 means full ESP, otherwise 16-bit SP. */
            pj_dpi(&t->e, 0u, 1, PC, R2, 4u);    /* TST r2,#4 */
            {
                uint8_t *skip16 = pjt_bfwd(t, 1 /* NE, B=1 */);
                /* SP16: retain upper half of ESP even when low SP wraps. */
                pjt_addimm(t, R2, GR(4), push32 ? (uint32_t)-4 : 4u);
                pj_bfi(&t->e, GR(4), R2, 0u, 16u);
                {
                    uint8_t *skip32 = pjt_bfwd(t, -1 /* unconditional */);
                    pjt_here(t, skip16);
                    pjt_addimm(t, GR(4), GR(4), push32 ? (uint32_t)-4 : 4u);
                    pjt_here(t, skip32);
                }
            }
            if (!push32) pj_mov(&t->e, GR(reg32), R1);
            /* PUSH can invalidate native code via the shared SMC path.
             * Both forms exit the block while the old lazy flags survive. */
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
            if (push32) BB_PM16_DIAG_INC(v14_push32_native);
            else BB_PM16_DIAG_INC(v14_pop32_native);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#endif /* BB_PJ_PM16_STACK32_V14 */
#if B86_PJ_V50_NATIVE_COVERAGE
        /* Operand-size override: CWDE and CDQ; no x86 flags or memory. */
        if (op66 == 0x98u) {
            pj_ext(&t->e, 0xFA0Fu, R1, GR(0)); /* SXTH EAX low 16 */
            pjt_putr(t, 0u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 == 0x99u) {
            pj_shift(&t->e, R1, GR(0), 2u, 31u); /* CDQ: EDX=EAX>>31 arithmetic */
            pjt_putr(t, 2u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif
#if B86_PJ_V51_NATIVE_COVERAGE
        /* 66 8D /r: LEA r32,m16.  The EA16 emitter computes the wrapped
         * 16-bit effective OFFSET without touching a guest descriptor or
         * memory. A register-only ModRM (mod=3) is invalid and refused. */
        if (op66 == 0x8Du) {
            PjUnifiedModRM lea;
            unsigned unused_seg;
            if (!pjt_unified_modrm(t, &lea) || lea.mod == 3u ||
                !pjt_unified_ea16(t, lea.raw, &unused_seg)) PJ16_FAIL();
            pjt_putr(t, lea.reg, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif
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
#if BB_PJ_V20_MOV32_MEM
        /* 66 89 /r store and 66 8B /r load, ModRM.mod != 3.
         * Address-size remains 16 bits; BP-based forms default to SS,
         * otherwise DS. Other prefixes never enter this 66 decoder.
         * Every refusal exits to the original instruction, never #UD. */
        if ((op66 == 0x89u || op66 == 0x8Bu) &&
            t->pos < t->n && (t->b[t->pos] >> 6u) != 3u) {
            unsigned reg20, seg20;
            const unsigned write20 = op66 == 0x89u;
            if (!pjt_u8(t, &mr)) PJ16_FAIL();
            reg20 = (mr >> 3u) & 7u;
            if (!pjt_unified_ea16(t, mr, &seg20)) PJ16_FAIL();
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_movw(&t->e, R2, (seg20 << 8u) | (write20 << 15u));
            if (write20) pjt_getr(t, R3, reg20, 4u);
            else pj_movw(&t->e, R3, 0u);
            pjt_call(t, (const void *)b86_pj_v20_mov32_mem);
            pj_dpi(&t->e, 13u, 1, PC, R1, 0u); /* CMP r1(success),#0 */
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            /* Helper call clobbers ARM NZCV. Never fuse a Jcc through it. */
            t->last_kind = -1; t->ikind_clear = 1; t->fuse_at = NULL;
            if (write20) {
                /* The C helper completed the store + SMC notification.
                 * Honor pj_stop and dispatch before further guest code. */
                pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
                BB_PM16_DIAG_INC(v20_mov32_write_native);
                do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
            }
            pjt_putr(t, reg20, 4u, R0);
            BB_PM16_DIAG_INC(v20_mov32_read_native);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif /* BB_PJ_V20_MOV32_MEM */
        if (op66 == 0x89u || op66 == 0x8Bu) {
            if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
            pjt_unified_regmov(t, op66, mr, 4u);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (op66 < 0x40u && ((op66 & 7u) == 1u || (op66 & 7u) == 3u)) {
            alu66 = op66 >> 3; form66 = op66 & 7u;
            if (alu66 != 0u && alu66 != 1u && alu66 != 4u &&
                alu66 != 5u && alu66 != 6u && alu66 != 7u) PJ16_FAIL();
            if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
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
            if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
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
                if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
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
            if (!pjt_unified_modrm_regonly(t, &mr) || ((mr >> 3) & 7u)) PJ16_FAIL();
            if (!pjt_imm(t, 4u, &imm66)) PJ16_FAIL();
            pj_imm32(&t->e, R1, imm66); pjt_putr(t, mr & 7u, 4u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        PJ16_FAIL();
    }
#endif /* BB_PJ_PM16_OP66REG */
#if BB_PJ_PM16_SMSW_REG_V185
    /* PM16 default operand size: 0F 01 /4, SMSW r/m16, REGISTERS only.
     * 66/67/segment/LOCK/REP cannot enter this path because op==0F.
     * /6 LMSW and every descriptor/memory form stay in the interpreter.
     * Read msw per execution, preserve the upper 16 bits of destination
     * GPR and all EFLAGS (including pending lazy flags). Finish in an
     * unlinked exit: the helper call clobbers ARM condition flags and a
     * subsequent fused Jcc must not reuse those condition flags. */
    if (op == 0x0Fu && t->pos + 1u < t->n &&
        t->b[t->pos] == 0x01u) {
        uint32_t op2, mr;
        if (!pjt_u8(t, &op2) || !pjt_u8(t, &mr)) PJ16_FAIL();
        if (((mr >> 3) & 7u) != 4u || (mr >> 6) != 3u) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pjt_call(t, (const void *)b86_pj_pm16_smsw16);
        pjt_putr(t, mr & 7u, 2u, R0);
        t->last_kind = -1;
        t->ikind_clear = 1;
        t->fuse_at = NULL;
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        BB_PM16_DIAG_INC(v185_smsw_native);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* BB_PJ_PM16_SMSW_REG_V185 */
    /* V19: consolidated read-only 0F PM16 group. Exact unprefixed 0F
     * with register ModRM only; all memory, multi-prefix, write-side,
     * descriptor-checking, TLB-mutating, and task-changing instructions
     * decline to the existing C interpreter without any state change. */
#if BB_PJ_V19_READ_SELECTORS || BB_PJ_V19_READ_CR
    if (op == 0x0Fu && t->pos + 1u < t->n &&
        (t->b[t->pos] == 0x00u || t->b[t->pos] == 0x20u)) {
        uint32_t op2, mr, ext;
        if (!pjt_u8(t, &op2) || !pjt_u8(t, &mr)) PJ16_FAIL();
        ext = (mr >> 3u) & 7u;
        if ((mr >> 6u) != 3u) PJ16_FAIL();
#if BB_PJ_V19_READ_SELECTORS
        if (op2 == 0x00u && (ext == 0u || ext == 1u)) {
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_movw(&t->e, R1, ext);
            pjt_call(t, (const void *)b86_pj_v19_selector);
            pjt_putr(t, mr & 7u, 2u, R0);
            t->last_kind = -1; t->ikind_clear = 1; t->fuse_at = NULL;
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
            if (ext == 0u) BB_PM16_DIAG_INC(v19_sldt_native);
            else BB_PM16_DIAG_INC(v19_str_native);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#endif
#if BB_PJ_V19_READ_CR
        if (op2 == 0x20u && (ext == 0u || ext == 2u || ext == 3u)) {
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_movw(&t->e, R1, ext);
            pjt_call(t, (const void *)b86_pj_v19_cr_read);
            /* Inspect high 32 bits BEFORE committing destination. */
            pj_dpi(&t->e, 13u, 1, PC, R1, 0u); /* CMP validity,#0 */
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            pjt_putr(t, mr & 7u, 4u, R0);
            t->last_kind = -1; t->ikind_clear = 1; t->fuse_at = NULL;
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
            BB_PM16_DIAG_INC(v19_crread_native);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#endif
        PJ16_FAIL();
    }
#endif /* V19 combined 0F read-only group */
    /* Other prefixes and unsupported 66 forms decline naturally. */
#ifndef BB_PJ_V171_PUSH
#define BB_PJ_V171_PUSH 1u
#endif
#ifndef BB_PJ_V171_POP
#define BB_PJ_V171_POP 1u
#endif
#if B86_PJ_V51_NATIVE_COVERAGE && BB_PJ_PM16_GS_STACK_V17
    /* 0F A0/A1: unprefixed 16-bit PUSH/POP FS.  Same protected SS
     * pointer admission as the existing proven GS path, including odd
     * stack addresses, permission/page checks and SMC notification.  POP
     * validates and commits the FS descriptor before advancing SP. */
    if (op == 0x0Fu && t->pos < t->n &&
        (t->b[t->pos] == 0xA0u || t->b[t->pos] == 0xA1u)) {
        const int push_fs = t->b[t->pos] == 0xA0u;
        uint32_t op2;
        if (!pjt_u8(t, &op2)) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_mov(&t->e, R1, GR(4));
        pj_movw(&t->e, R2, push_fs ? 1u : 0u);
        pjt_call(t, (const void *)b86_pj_pm16_gs16_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        if (push_fs) {
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDRH, R1, CREG,
                    (unsigned)offsetof(B86Cpu, seg386));
            pj_ldst(&t->e, PJ_STRB, R1, R0, 0u);
            pj_shift(&t->e, R2, R1, 1u, 8u);
            pj_ldst(&t->e, PJ_STRB, R2, R0, 1u);
        } else {
            pj_ldst(&t->e, PJ_LDRB, R2, R0, 0u);
            pj_ldst(&t->e, PJ_LDRB, R1, R0, 1u);
            pj_dpr(&t->e, 2u, 0, R2, R2, R1, 0u, 8u);
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_movw(&t->e, R1, B86_FS);
            pjt_call(t, (const void *)b86_pj_loadseg);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        }
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDRB, R2, CREG,
                (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
        pj_dpi(&t->e, 0u, 1, PC, R2, 4u);
        {
            uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B */);
            pjt_addimm(t, R2, GR(4), push_fs ? (uint32_t)-2 : 2u);
            pj_bfi(&t->e, GR(4), R2, 0u, 16u);
            {
                uint8_t *done = pjt_bfwd(t, -1);
                pjt_here(t, addr32);
                pjt_addimm(t, GR(4), GR(4), push_fs ? (uint32_t)-2 : 2u);
                pjt_here(t, done);
            }
        }
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* V51 FS stack */
#if BB_PJ_PM16_GS_STACK_V17
    /* Default PM16 0F A8/A9: PUSH GS / POP GS, 16-bit operand.
     * Only this exact unprefixed form; 66h, LOCK/REP and segment/address
     * overrides remain in the interpreter. No writable guest state is
     * changed before pointer checks. A POP selector is validated and
     * committed by the existing PM26 loader before ESP is advanced. */
    if (op == 0x0Fu && t->pos < t->n &&
        ((BB_PJ_V171_PUSH && t->b[t->pos] == 0xA8u) ||
         (BB_PJ_V171_POP  && t->b[t->pos] == 0xA9u))) {
        uint32_t op2;
        const int push_gs = t->b[t->pos] == 0xA8u;
        if (!pjt_u8(t, &op2)) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_mov(&t->e, R1, GR(4)); /* old full ESP */
        pj_movw(&t->e, R2, push_gs ? 1u : 0u);
        pjt_call(t, (const void *)b86_pj_pm16_gs16_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u); /* CMP ptr,#0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        if (push_gs) {
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDRH, R1, CREG,
                    (unsigned)(offsetof(B86Cpu, seg386) +
                    (B86_GS - B86_FS) * sizeof(t->c->seg386[0])));
            pj_ldst(&t->e, PJ_STRB, R1, R0, 0u);
            pj_shift(&t->e, R2, R1, 1u, 8u);
            pj_ldst(&t->e, PJ_STRB, R2, R0, 1u);
        } else {
            /* Read all data first; even odd pointers use only byte reads. */
            pj_ldst(&t->e, PJ_LDRB, R2, R0, 0u);
            pj_ldst(&t->e, PJ_LDRB, R1, R0, 1u);
            pj_dpr(&t->e, 2u, 0, R2, R2, R1, 0u, 8u);
            pjt_c(t);
            pj_mov(&t->e, R0, CREG);
            pj_mov(&t->e, R1, R2);
            pjt_call(t, (const void *)b86_pj_pm16_gs16_commit);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        }
        /* Both operations commit SP/ESP only after successful memory and
         * (POP) descriptor access. SS.B=0 preserves the top 16 bits. */
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDRB, R2, CREG,
                (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
        pj_dpi(&t->e, 0u, 1, PC, R2, 4u);
        {
            uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B=1 */);
            pjt_addimm(t, R2, GR(4), push_gs ? (uint32_t)-2 : 2u);
            pj_bfi(&t->e, GR(4), R2, 0u, 16u);
            {
                uint8_t *done = pjt_bfwd(t, -1);
                pjt_here(t, addr32);
                pjt_addimm(t, GR(4), GR(4), push_gs ? (uint32_t)-2 : 2u);
                pjt_here(t, done);
            }
        }
        /* POP changes GS hidden descriptor state. PUSH may trigger SMC.
         * Both leave through an unlinked PM16 dispatcher exit. */
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        if (push_gs) BB_PM16_DIAG_INC(v17_push_gs_native);
        else BB_PM16_DIAG_INC(v17_pop_gs_native);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* BB_PJ_PM16_GS_STACK_V17 */
#if BB_PJ_PM16_BT_IMM_V18
    /* Decode only 0F BA /4, ModRM.mod=3, imm8 (BT r16, imm8).
     * The instruction has NO 66/67/LOCK/REP or segment prefix: the PM16
     * translator sees only an initial 0F; everything else is a fallback.
     * Register bit index is reduced mod 16, as required by x86. C helper
     * materializes any prior lazy flags and replaces CF only.
     * Deliberately end the native block unlinked so the next instruction
     * sees canonical flags and no remembered ARM/lazy flag producer. */
    if (op == 0x0Fu && t->pos < t->n && t->b[t->pos] == 0xBAu) {
        uint32_t op2, mr, bit;
        if (!pjt_u8(t, &op2) || !pjt_u8(t, &mr) ||
            !pjt_u8(t, &bit)) PJ16_FAIL();
        if (((mr >> 3) & 7u) != 4u) {
            if (((mr >> 3) & 7u) >= 5u && ((mr >> 3) & 7u) <= 7u)
                BB_PM16_DIAG_INC(v18_bt_modify_refused);
            else BB_PM16_DIAG_INC(v18_bt_other_refused);
            PJ16_FAIL();
        }
        if ((mr >> 6) != 3u) {
            BB_PM16_DIAG_INC(v18_bt_mem_refused);
            PJ16_FAIL();
        }
        pjt_getr(t, R1, mr & 7u, 2u);
        pj_movw(&t->e, R2, bit & 15u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pjt_call(t, (const void *)b86_pj_pm16_bt16_cf);
        /* No subsequent fused branch may use the flags before BT. */
        t->last_kind = -1;
        t->ikind_clear = 1;
        t->fuse_at = NULL;
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        BB_PM16_DIAG_INC(v18_bt_native);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* BB_PJ_PM16_BT_IMM_V18 */
#if BB_PJ_PM16_MOVEXT_V9
    /* Default PM16 destination is r16, so only byte-source B6/BE qualify.
     * B7/BF would require a 16-bit source and 16-bit dest: leave for #UD. */
    if (op == 0x0Fu) {
        uint32_t op2, mr;
        if (!pjt_u8(t, &op2)) PJ16_FAIL();
#if BB_PJ_PM16_JCC0F_V15
        /* 0F 80..8F rel16, default 16-bit operand size and CS.
         * Instruction is fully fetched/validated by pj_fetch_pm16.
         * Compute the 16-bit truncated target; taken and fallthrough use
         * unlinked dyn=2 exits, including for Jcc back-edges.  No chain
         * source is created in this nonflat segment context.  Flags do
         * not change.  The interpreter handles prefixes and faults. */
        if (op2 >= 0x80u && op2 <= 0x8Fu) {
            uint32_t delta16;
            int armcc;
            if (!pjt_imm(t, 2u, &delta16)) PJ16_FAIL();
            armcc = pjt_flags_for(t, op2 & 15u);
            pjt_exit_stay(t, armcc,
                          (PJ16_NEXT() + (uint32_t)(int32_t)(int16_t)delta16) & 0xFFFFu,
                          t->ninsn);
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
            BB_PM16_DIAG_INC(v15_pm16_near_jcc_native);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#endif
#if BB_PJ_PM16_SETCC_V10
        if (op2 >= 0x90u && op2 <= 0x9Fu) {
            if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
            pjt_pm16_setcc_reg(t, op2 & 15u, mr & 7u);
            BB_PM16_DIAG_INC(v10_setcc);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif /* BB_PJ_PM16_SETCC_V10 */
        if (op2 != 0xB6u && op2 != 0xBEu) PJ16_FAIL();
        if (!pjt_unified_modrm_regonly(t, &mr)) PJ16_FAIL();
        pjt_getr(t, R1, mr & 7u, 1u);
        if (op2 == 0xBEu) pj_ext(&t->e, 0xFA4Fu, R1, R1);
        pjt_putr(t, (mr >> 3) & 7u, 2u, R1);
        BB_PM16_DIAG_INC(v9_movext16);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#endif /* BB_PJ_PM16_MOVEXT_V9 */
#if B86_PJ_V50_NATIVE_COVERAGE
    /* Unprefixed MOV r/m16,Sreg, register destination. No descriptor load:
     * source is the live selector, including FS/GS, not the hidden base.
     * Memory ModRM and invalid /6,/7 still go to the PM interpreter. */
    if (op == 0x8Cu) {
        const unsigned sr = t->pos < t->n ? (t->b[t->pos] >> 3u) & 7u : 7u;
        if (!pjt_unified_modrm_regonly(t, &modrm) || sr > B86_GS) PJ16_FAIL();
        pjt_c(t);
        if (sr <= B86_DS) {
            pj_ldst(&t->e, PJ_LDR, R1, CREG,
                    (unsigned)offsetof(B86Cpu, seg) + 4u * sr);
            pj_ext(&t->e, 0xFA1Fu, R1, R1); /* UXTH */
        } else {
            pj_ldst(&t->e, PJ_LDRH, R1, CREG,
                    (unsigned)offsetof(B86Cpu, seg386) +
                    2u * (sr - B86_FS));
        }
        pjt_putr(t, modrm & 7u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* CBW/CWD: preserve high GPR halves and ALL guest flags. */
    if (op == 0x98u) {
        pj_ext(&t->e, 0xFA4Fu, R1, GR(0)); /* SXTB AL -> AX */
        pjt_putr(t, 0u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    if (op == 0x99u) {
        pj_ext(&t->e, 0xFA0Fu, R1, GR(0)); /* SXTH AX */
        pj_shift(&t->e, R1, R1, 2u, 31u); /* CWD: DX=sign(AX) */
        pjt_putr(t, 2u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* CLD/STD update only DF; keep all pending arithmetic lazy flags. */
    if (op == 0xFCu || op == 0xFDu) {
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDR, R1, CREG, (unsigned)offsetof(B86Cpu, flags));
        pj_dpi(&t->e, op == 0xFCu ? 1u : 2u, 0, R1, R1, B86_DF);
        pj_ldst(&t->e, PJ_STR, R1, CREG, (unsigned)offsetof(B86Cpu, flags));
        t->fuse_at = NULL;
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#if BB_PJ_PM16_GS_STACK_V17
    /* PUSH ES/CS/SS/DS (06/0E/16/1E): translate the *common checked*
     * 16-bit PM stack case instead of interpreting it. The existing V17
     * 2-byte SS.B-aware pointer helper checks descriptors, privilege, paging,
     * crossing, and SMC before any write. A failed guard replays in C.
     * Successful PUSH exits unlinked because a stack store can modify code. */
    if (op == 0x06u || op == 0x0Eu || op == 0x16u || op == 0x1Eu) {
        const unsigned sr = (op >> 3u) & 3u; /* 06 ES, 0E CS, 16 SS, 1E DS */
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_mov(&t->e, R1, GR(4));
        pj_movw(&t->e, R2, 1u);
        pjt_call(t, (const void *)b86_pj_pm16_gs16_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ: interpreter retry */, t->ip0, t->ninsn - 1u);
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDR, R1, CREG,
                (unsigned)offsetof(B86Cpu, seg) + 4u * sr);
        pj_ldst(&t->e, PJ_STRB, R1, R0, 0u);
        pj_shift(&t->e, R2, R1, 1u, 8u);
        pj_ldst(&t->e, PJ_STRB, R2, R0, 1u);
        pj_ldst(&t->e, PJ_LDRB, R2, CREG,
                (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
        pj_dpi(&t->e, 0u, 1, PC, R2, 4u);
        {
            uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B */);
            pjt_addimm(t, R2, GR(4), (uint32_t)-2);
            pj_bfi(&t->e, GR(4), R2, 0u, 16u);
            {
                uint8_t *done = pjt_bfwd(t, -1);
                pjt_here(t, addr32);
                pjt_addimm(t, GR(4), GR(4), (uint32_t)-2);
                pjt_here(t, done);
            }
        }
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* BB_PJ_PM16_GS_STACK_V17 */
#endif /* B86_PJ_V50_NATIVE_COVERAGE */
#if B86_PJ_V51_NATIVE_COVERAGE && BB_PJ_PM16_GS_STACK_V17
    /* 07 POP ES / 1F POP DS. The 16-bit SS.B-aware memory admission is
     * shared with the GS stack path.  Read first, then validate/commit the
     * target segment before touching SP; failure retries in the interpreter
     * without a partial stack/selector update.  A changed hidden descriptor
     * always exits to the checked dispatcher rather than remaining chained. */
    if (op == 0x07u || op == 0x1Fu) {
        const unsigned sr = op == 0x07u ? B86_ES : B86_DS;
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_mov(&t->e, R1, GR(4));
        pj_movw(&t->e, R2, 0u);
        pjt_call(t, (const void *)b86_pj_pm16_gs16_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pj_ldst(&t->e, PJ_LDRB, R2, R0, 0u);
        pj_ldst(&t->e, PJ_LDRB, R1, R0, 1u);
        pj_dpr(&t->e, 2u, 0, R2, R2, R1, 0u, 8u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, sr);
        pjt_call(t, (const void *)b86_pj_loadseg);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pjt_c(t);
        pj_ldst(&t->e, PJ_LDRB, R2, CREG,
                (unsigned)offsetof(B86Cpu, pm386.seg[B86_SS].flags));
        pj_dpi(&t->e, 0u, 1, PC, R2, 4u);
        {
            uint8_t *addr32 = pjt_bfwd(t, 1 /* NE: SS.B */);
            pjt_addimm(t, R2, GR(4), 2u);
            pj_bfi(&t->e, GR(4), R2, 0u, 16u);
            {
                uint8_t *done = pjt_bfwd(t, -1);
                pjt_here(t, addr32);
                pjt_addimm(t, GR(4), GR(4), 2u);
                pjt_here(t, done);
            }
        }
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* V51 ES/DS POP */
#if B86_PJ_V51_NATIVE_COVERAGE
    /* 8D /r LEA r16,m16: pure Thumb-2 EA calculation, including mod=00
     * [disp16] and 16-bit wrap. No descriptor lookup or memory access. */
    if (op == 0x8Du) {
        PjUnifiedModRM lea;
        unsigned unused_seg;
        if (!pjt_unified_modrm(t, &lea) || lea.mod == 3u ||
            !pjt_unified_ea16(t, lea.raw, &unused_seg)) PJ16_FAIL();
        pjt_putr(t, lea.reg, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#endif
#if B86_PJ_V53_NATIVE_SEGMEM && BB_PJ_PM16_MOV_READ
    /* PM16 8E /r, memory-source MOV Sreg,[EA16]: the existing checked
     * DS/SS pointer validates the access before reading its selector.
     * The existing segment planner atomically validates/commits it. The
     * guest remains unchanged on refusal; MOV SS/CS intentionally decline. */
    if (op == 0x8Eu && t->pos < t->n &&
        (t->b[t->pos] >> 6u) != 3u) {
        PjUnifiedModRM m53;
        unsigned seg53;
        if (!pjt_unified_modrm(t, &m53) || m53.mod == 3u ||
            (m53.reg != B86_ES && m53.reg != B86_DS &&
             m53.reg != B86_FS && m53.reg != B86_GS) ||
            !pjt_unified_ea16(t, m53.raw, &seg53)) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, 2u | (seg53 << 8u));
        pjt_call(t, (const void *)b86_pj_pm16_read_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ: read fault/retry */, t->ip0,
                      t->ninsn - 1u);
        pj_ldst(&t->e, PJ_LDRH, R2, R0, 0u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, m53.reg);
        pjt_call(t, (const void *)b86_pj_loadseg);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ: segment-load fault/retry */, t->ip0,
                      t->ninsn - 1u);
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* V53 checked memory-source segment load */
#if B86_PJ_V53_NATIVE_SEGMEM && BB_PJ_PM16_MOV_WRITE
    /* PM16 26 88/89 memory-destination MOV ES:[EA16],reg8/16.
     * Avoid interpreter and C store helper for the common guarded case.
     * Native memt write gate preserves exact SMC/paging/MMIO checks.
     * End this block after every successful store, never chain across it. */
    if (op == 0x26u && t->pos + 1u < t->n &&
        (t->b[t->pos] == 0x88u || t->b[t->pos] == 0x89u) &&
        (t->b[t->pos + 1u] >> 6u) != 3u) {
        uint32_t store_op53;
        PjUnifiedModRM m53;
        unsigned ignored_seg53;
        if (!pjt_u8(t, &store_op53) ||
            !pjt_unified_modrm(t, &m53) || m53.mod == 3u ||
            !pjt_unified_ea16(t, m53.raw, &ignored_seg53)) PJ16_FAIL();
        {
            const unsigned width53 = store_op53 == 0x89u ? 2u : 1u;
            pjt_v53_es_write_guard(t, width53);
            pjt_mem(t, width53, 1);
            pjt_getr(t, R1, m53.reg, width53);
            pjt_st(t, R1, R0, width53);
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        }
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* V53 native ES stores */
#if B86_PJ_V52_NATIVE_SEGMOV_ES
    /* PM16 8C /r, mod=3: MOV r16,Sreg. All six real segment
     * selectors are readable; mod=3 never needs a memory access.
     * Leave operand-size/extra-prefix and memory forms to the old C path.
     * No guest flags, descriptors or caches are changed. */
    if (op == 0x8Cu) {
        PjUnifiedModRM sm;
        if (!pjt_unified_modrm(t, &sm) || sm.mod != 3u || sm.reg > 5u)
            PJ16_FAIL();
        pjt_c(t);
        if (sm.reg <= B86_DS) {
            pj_ldst(&t->e, PJ_LDR, R2, CREG,
                    (unsigned)offsetof(B86Cpu, seg) + 4u * sm.reg);
        } else {
            pj_ldst(&t->e, PJ_LDRH, R2, CREG,
                    (unsigned)offsetof(B86Cpu, seg386) + 2u * (sm.reg - 4u));
        }
        pjt_putr(t, sm.rm, 2u, R2);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* PM16 8E /r, mod=3: MOV Sreg,r16. The existing PM26-compatible
     * b86_pj_loadseg performs descriptor planning, privilege checks and
     * atomic selector/cache commit. A failure retries at original EIP.
     * CS is #UD and SS needs interrupt-shadow handling: both stay in C.
     * A successful load always exits to recheck hidden segment state. */
    if (op == 0x8Eu) {
        PjUnifiedModRM sm;
        if (!pjt_unified_modrm(t, &sm) || sm.mod != 3u ||
            (sm.reg != B86_ES && sm.reg != B86_DS &&
             sm.reg != B86_FS && sm.reg != B86_GS)) PJ16_FAIL();
        pjt_getr(t, R2, sm.rm, 2u);
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R1, sm.reg);
        pjt_call(t, (const void *)b86_pj_loadseg);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ: descriptor fault / interpreter retry */,
                      t->ip0, t->ninsn - 1u);
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
    /* PM16 26 8A/8B: ES override, byte/word MOV r,[EA16]. Decode the
     * complete instruction; 8/16-bit register-only 88..8B and LEA ignore
     * segment override and stay entirely native. For a memory source the
     * SAME V21 hidden-descriptor guard and native TLB are emitted with ES
     * selected explicitly. No C helper on the successful memory-read path.
     * Unsupported ES stores, multi-prefixes and bad operands refuse safely. */
    if (op == 0x26u) {
        uint32_t esop;
        PjUnifiedModRM esmr;
        unsigned default_seg;
        if (!pjt_u8(t, &esop)) PJ16_FAIL();
        if (esop == 0x8Du) {
            if (!pjt_unified_modrm(t, &esmr) || esmr.mod == 3u ||
                !pjt_unified_ea16(t, esmr.raw, &default_seg)) PJ16_FAIL();
            pjt_putr(t, esmr.reg, 2u, R1);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (esop < 0x88u || esop > 0x8Bu ||
            !pjt_unified_modrm(t, &esmr)) PJ16_FAIL();
        if (esmr.mod == 3u) {
            pjt_unified_regmov(t, esop, esmr.raw,
                               (esop & 1u) ? 2u : 1u);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#if BB_PJ_PM16_MOV_READ && BB_PJ_V21_PM16_INLINE_READ
        if (esop == 0x8Au || esop == 0x8Bu) {
            const unsigned width = esop == 0x8Bu ? 2u : 1u;
            const unsigned sc = (unsigned)offsetof(B86Cpu, pm386) +
                 (unsigned)offsetof(B86PM1State, seg) +
                 B86_ES * (unsigned)sizeof(B86PM1Cache);
            if (!pjt_unified_ea16(t, esmr.raw, &default_seg)) PJ16_FAIL();
            pjt_c(t);
            /* Runtime cache validity and conservative readable data
             * descriptor checks; original EA16 is still in R1. */
            pj_ldst(&t->e, PJ_LDRB, R0, CREG,
                    sc + (unsigned)offsetof(B86PM1Cache, valid));
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG,
                    sc + (unsigned)offsetof(B86PM1Cache, reserved));
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 1 /* NE */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG,
                    sc + (unsigned)offsetof(B86PM1Cache, access));
            pj_dpi(&t->e, 0u, 0, R0, R0, 0x1Eu);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0x12u);
            pjt_exit_stay(t, 1 /* NE */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDR, R3, CREG,
                    sc + (unsigned)offsetof(B86PM1Cache, limit));
            pjt_addimm(t, R2, R1, width - 1u);
            pj_dpr(&t->e, 13u, 1, PC, R2, R3, 0u, 0u);
            pjt_exit_stay(t, 8 /* HI */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDR, R0, CREG,
                    sc + (unsigned)offsetof(B86PM1Cache, base));
            pj_dpr(&t->e, 8u, 1, R1, R1, R0, 0u, 0u);
            pjt_exit_stay(t, 2 /* CS: overflow */, t->ip0, t->ninsn - 1u);
            if (width == 2u) {
                pj_dpi(&t->e, 0u, 1, R0, R1, 1u);
                pjt_exit_stay(t, 1 /* NE: odd linear addr */,
                              t->ip0, t->ninsn - 1u);
            }
            pjt_mem(t, width, 0);
            pjt_ld(t, R1, R0, width);
            pjt_putr(t, esmr.reg, width, R1);
            t->fuse_at = NULL;
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
#endif /* checked native ES read */
        PJ16_FAIL();
    }
#endif /* B86_PJ_V52_NATIVE_SEGMOV_ES */
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
        const uint32_t width = op == 0x89u ? 2u : 1u;
        PjUnifiedModRM operand;
        unsigned seg;
        if (!pjt_unified_modrm(t, &operand)) PJ16_FAIL();
        if (!pjt_unified_ea16(t, operand.raw, &seg)) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, width | (seg << 8));
        pjt_getr(t, R3, operand.reg, width);
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
        const uint32_t width = op == 0x8Bu ? 2u : 1u;
        PjUnifiedModRM operand;
        unsigned seg;
        if (!pjt_unified_modrm(t, &operand)) PJ16_FAIL();
        if (!pjt_unified_ea16(t, operand.raw, &seg)) PJ16_FAIL();
#if BB_PJ_V21_PM16_INLINE_READ
        /* Runtime hidden-descriptor guard, not a compile-time assumption.
         * Original EA16 lives in R1. R4-R11 (guest GPRs) are untouched.
         * Conservative native admission exactly matches the old helper's
         * allowed data/segment/alignment subset. No guest state changes
         * before a failing guard exits to the original instruction. */
        {
            const unsigned sc21 = (unsigned)offsetof(B86Cpu, pm386) +
                 (unsigned)offsetof(B86PM1State, seg) +
                 seg * (unsigned)sizeof(B86PM1Cache);
            const unsigned v21 = sc21 + (unsigned)offsetof(B86PM1Cache, valid);
            const unsigned res21 = sc21 + (unsigned)offsetof(B86PM1Cache, reserved);
            const unsigned acc21 = sc21 + (unsigned)offsetof(B86PM1Cache, access);
            const unsigned lim21 = sc21 + (unsigned)offsetof(B86PM1Cache, limit);
            const unsigned bas21 = sc21 + (unsigned)offsetof(B86PM1Cache, base);
            pjt_c(t);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG, v21);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 0 /* EQ: !valid */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG, res21);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
            pjt_exit_stay(t, 1 /* NE: reserved */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDRB, R0, CREG, acc21);
            pj_dpi(&t->e, 0u, 0, R0, R0, 0x1Eu);
            pj_dpi(&t->e, 13u, 1, PC, R0, 0x12u);
            pjt_exit_stay(t, 1 /* NE: bad data permissions */, t->ip0, t->ninsn - 1u);
            /* EA16 is zero-extended already; compare end offset without
             * wrapping at 16 bits. Width=1 or 2. */
            pj_ldst(&t->e, PJ_LDR, R3, CREG, lim21);
            pjt_addimm(t, R2, R1, width - 1u);
            pj_dpr(&t->e, 13u, 1, PC, R2, R3, 0u, 0u);
            pjt_exit_stay(t, 8 /* HI: outside segment limit */, t->ip0, t->ninsn - 1u);
            pj_ldst(&t->e, PJ_LDR, R0, CREG, bas21);
            /* ADDS sets C on 32-bit overflow; do not allow wrapped base. */
            pj_dpr(&t->e, 8u, 1, R1, R1, R0, 0u, 0u);
            pjt_exit_stay(t, 2 /* CS: base+EA overflow */, t->ip0, t->ninsn - 1u);
            if (width == 2u) {
                pj_dpi(&t->e, 0u, 1, R0, R1, 1u);
                pjt_exit_stay(t, 1 /* NE: halfword unaligned */, t->ip0, t->ninsn - 1u);
            }
            /* The same native TLB, physical memory, mapped-page and
             * page-crossing backend used by the flat32 translator.
             * Memory failure deopts before touching the destination. */
            pjt_mem(t, width, 0);
            pjt_ld(t, R1, R0, width);
            pjt_putr(t, operand.reg, width, R1);
            t->fuse_at = NULL; /* guard CMP/ANDS invalidated ARM NZCV */
            BB_PM16_DIAG_INC(v21_pm16_read_native);
        }
#else
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, width | (seg << 8));
        pjt_call(t, (const void *)b86_pj_pm16_read_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);  /* CMP pointer, #0 */
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pjt_ld(t, R1, R0, width);
        pjt_putr(t, operand.reg, width, R1);
#endif /* BB_PJ_V21_PM16_INLINE_READ */
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#endif /* BB_PJ_PM16_MOV_READ */
    /* Exact register-only MOV (including 8-bit AH/BH etc). */
    if (op >= 0x88u && op <= 0x8Bu) {
        if (!pjt_unified_modrm_regonly(t, &modrm)) PJ16_FAIL();
        w = (op & 1u) ? 2u : 1u;
        pjt_unified_regmov(t, op, modrm, w);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
    /* ADD/OR/AND/SUB/XOR/CMP r8/r16, r8/r16: ModRM.mod=3 only. */
    if (op < 0x40u && (op & 7u) < 4u) {
        alu = op >> 3; form = op & 7u;
        if (alu != 0u && alu != 1u && alu != 4u &&
            alu != 5u && alu != 6u && alu != 7u) PJ16_FAIL();
        if (!pjt_unified_modrm_regonly(t, &modrm)) PJ16_FAIL();
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
        if (!pjt_unified_modrm_regonly(t, &modrm)) PJ16_FAIL();
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
        if (!pjt_unified_modrm_regonly(t, &modrm)) PJ16_FAIL();
        w = (op & 1u) ? 2u : 1u;
        pjt_getr(t, R1, modrm & 7u, w);
        pjt_getr(t, R0, (modrm >> 3) & 7u, w);
        if (!pjt_alu(t, 4u, w, R3, R1, R0)) PJ16_FAIL();
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#if B86_PJ_V56_STACK_CHAIN && B86_PJ_V54_NATIVE_CONTROL && BB_PJ_PM16_GS_STACK_V17
    /* 68 iw / 6A ib: PM16 PUSH imm16 and sign-extended PUSH imm8.
     * Successful writes may overlap executable memory, so terminate the
     * translated block. The original SS guard runs before any write. */
    if (op == 0x68u || op == 0x6Au) {
        if (!pjt_imm(t, op == 0x68u ? 2u : 1u, &imm)) PJ16_FAIL();
        if (op == 0x6Au) imm = (uint32_t)(int32_t)(int8_t)imm;
        pjt_v54_stack_check(t, 1);
        pj_movw(&t->e, R1, imm & 0xFFFFu);
        pjt_v54_store16(t);
        pjt_v54_sp_step(t, 1);
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
    /* 8F /0, ModRM.mod=3: POP r16. The checked stack read happens
     * before SP or destination changes. POP SP's loaded value replaces
     * the post-incremented SP, as required on 286/386. Other /r and
     * memory-destination forms continue through the interpreter. */
    if (op == 0x8Fu && t->pos < t->n &&
        ((t->b[t->pos] >> 3u) & 7u) == 0u &&
        (t->b[t->pos] >> 6u) == 3u) {
        if (!pjt_unified_modrm_regonly(t, &modrm)) PJ16_FAIL();
        pjt_v54_stack_check(t, 0);
        pjt_v54_read16(t);
        pjt_v54_sp_step(t, 0);
        pjt_putr(t, modrm & 7u, 2u, R1);
        do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
    }
#if BB_PJ_PM16_MOV_READ
    /* FF /6, memory operand: read source from the effective data segment
     * before checking/writing SS. Preserve it across the checked stack
     * helper's AAPCS call; a refused read/write replays in the interpreter
     * without committing ESP or guest data. */
    if (op == 0xFFu && t->pos < t->n &&
        ((t->b[t->pos] >> 3u) & 7u) == 6u &&
        (t->b[t->pos] >> 6u) != 3u) {
        PjUnifiedModRM ff56;
        unsigned seg56 = B86_DS;
        if (!pjt_unified_modrm(t, &ff56) || ff56.reg != 6u ||
            ff56.mod == 3u || !pjt_unified_ea16(t, ff56.raw, &seg56)) PJ16_FAIL();
        pjt_c(t);
        pj_mov(&t->e, R0, CREG);
        pj_movw(&t->e, R2, 2u | (seg56 << 8u));
        pjt_call(t, (const void *)b86_pj_pm16_read_ptr);
        pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
        pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
        pjt_v54_read16(t);
        pj_ldst(&t->e, PJ_STR, R1, SP, 4u);
        pjt_v54_stack_check(t, 1);
        pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
        pjt_v54_store16(t);
        pjt_v54_sp_step(t, 1);
        pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* PM16 MOV READ */
    /* C2 iw: near RET with immediate stack cleanup. The target is
     * read from the checked old SS:SP before the stack is adjusted. */
    if (op == 0xC2u) {
        uint32_t cleanup56;
        if (!pjt_imm(t, 2u, &cleanup56)) PJ16_FAIL();
        pjt_v54_stack_check(t, 0);
        pjt_v54_read16(t);
        pj_ldst(&t->e, PJ_STR, R1, SP, 4u);
        pjt_v54_sp_step(t, 0);
        pjt_v56_sp_add(t, cleanup56);
        pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
        pjt_exit(t, 0u, t->ninsn, 3);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif /* V56 PM16 stack-coverage additions */
#if B86_PJ_V54_NATIVE_CONTROL
    /* PM16 FF register forms: /0 INC, /1 DEC, /4 near JMP. The
     * architectural IP is 16-bit and the high halves of guest GPRs stay
     * intact. /3,/5 far transfers and /7 undefined still decline. */
    if (op == 0xFFu) {
        PjUnifiedModRM ff54;
        unsigned seg54 = B86_DS;
        if (!pjt_unified_modrm(t, &ff54)) PJ16_FAIL();
        if ((ff54.reg == 0u || ff54.reg == 1u) && ff54.mod == 3u) {
            pjt_getr(t, R2, ff54.rm, 2u);
            if (ff54.reg == 0u) pj_dpi(&t->e, 8u, 0, R3, R2, 1u);
            else pj_dpi(&t->e, 13u, 0, R3, R2, 1u);
            pjt_putr(t, ff54.rm, 2u, R3);
            pjt_lazy(t, ff54.reg == 0u ? pj_k_inc[2] : pj_k_dec[2], R2, R3);
            do { BB_PM16_DIAG_INC(emit_ok); return 1; } while (0);
        }
        if (ff54.reg == 4u || ff54.reg == 2u) {
            /* The indirect near target is read BEFORE a CALL pushes the
             * return IP, even for CALL [ESP/BP-relative memory]. */
            if (ff54.mod == 3u) {
                pjt_getr(t, R1, ff54.rm, 2u);
            } else {
#if BB_PJ_PM16_MOV_READ
                if (!pjt_unified_ea16(t, ff54.raw, &seg54)) PJ16_FAIL();
                pjt_c(t);
                pj_mov(&t->e, R0, CREG);
                pj_movw(&t->e, R2, 2u | (seg54 << 8u));
                pjt_call(t, (const void *)b86_pj_pm16_read_ptr);
                pj_dpi(&t->e, 13u, 1, PC, R0, 0u);
                pjt_exit_stay(t, 0 /* EQ */, t->ip0, t->ninsn - 1u);
                pjt_v54_read16(t);
#else
                PJ16_FAIL();
#endif
            }
            if (ff54.reg == 2u) {
#if BB_PJ_PM16_GS_STACK_V17
                /* Preserve target across the AAPCS C stack guard call.
                 * [sp+4] is the translator's dedicated scratch slot. */
                pj_ldst(&t->e, PJ_STR, R1, SP, 4u);
                pjt_v54_stack_check(t, 1);
                pj_movw(&t->e, R1, PJ16_NEXT());
                pjt_v54_store16(t);
                pjt_v54_sp_step(t, 1);
                pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
#else
                PJ16_FAIL();
#endif
            }
            /* dyn=3 returns to the checked PM16 C dispatcher with the
             * computed 16-bit target. PM32 j->dyn is NOT PM16-safe. */
            pjt_exit(t, 0u, t->ninsn, 3);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#if BB_PJ_PM16_GS_STACK_V17
        if (ff54.reg == 6u && ff54.mod == 3u) {
            /* PUSH r/m16 register: read source BEFORE SP is changed;
             * PUSH SP stores the original SP (286+ behavior). */
            pjt_v54_stack_check(t, 1);
            pjt_getr(t, R1, ff54.rm, 2u);
            pjt_v54_store16(t);
            pjt_v54_sp_step(t, 1);
            /* Potential SMC stack store: never execute another stale block. */
            pjt_exit(t, PJ16_NEXT(), t->ninsn, 2);
            do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
        }
#endif
        PJ16_FAIL();
    }
#if BB_PJ_PM16_GS_STACK_V17
    /* PM16 near relative CALL: checked 2-byte SS store, then native SP
     * commit and unlinked transfer to the calculated 16-bit target. */
    if (op == 0xE8u) {
        uint32_t disp54;
        uint32_t target54;
        if (!pjt_imm(t, 2u, &disp54)) PJ16_FAIL();
        target54 = (PJ16_NEXT() + (uint32_t)(int32_t)(int16_t)disp54) & 0xFFFFu;
        pjt_v54_stack_check(t, 1);
        pj_movw(&t->e, R1, PJ16_NEXT());
        pjt_v54_store16(t);
        pjt_v54_sp_step(t, 1);
#if B86_PJ_V56_STACK_CHAIN && B86_PJ_V55_PM16_DYN
        /* V56: direct CALL can chain in the active PM16 CS context on
         * an admitted cache hit. No flat32 table lookup is ever used. */
        pj_movw(&t->e, R1, target54);
        pjt_exit(t, 0u, t->ninsn, 3);
#else
        /* Keep static target unlinked: original v55 behavior. */
        pjt_exit(t, target54, t->ninsn, 2);
#endif
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
    /* PM16 near RET: checked SS read, then native SP commit. The return
     * EIP remains in r1 and must take the PM16 checked dynamic exit. */
    if (op == 0xC3u) {
        pjt_v54_stack_check(t, 0);
        pjt_v54_read16(t);
        pj_ldst(&t->e, PJ_STR, R1, SP, 4u);
        pjt_v54_sp_step(t, 0);
        pj_ldst(&t->e, PJ_LDR, R1, SP, 4u);
        pjt_exit(t, 0u, t->ninsn, 3);
        do { BB_PM16_DIAG_INC(emit_ok); return 2; } while (0);
    }
#endif
#endif /* V54 native PM16 control transfers */
    /* All control transfers return through the unlinked C dispatcher. */
    if (op == 0xEBu || op == 0xE9u) {
        if (op == 0xEBu) {
            if (!pjt_u8(t, &imm)) PJ16_FAIL();
            imm = (uint32_t)(int32_t)(int8_t)imm;
        } else {
            if (!pjt_imm(t, 2u, &imm)) PJ16_FAIL();
            imm = (uint32_t)(int32_t)(int16_t)imm;
        }
#if B86_PJ_V56_STACK_CHAIN && B86_PJ_V55_PM16_DYN
        /* Relative near and short JMP now probe native PM16 context
         * before returning to C, exactly like v55's computed transfers. */
        pj_movw(&t->e, R1, (PJ16_NEXT() + imm) & 0xFFFFu);
        pjt_exit(t, 0u, t->ninsn, 3);
#else
        pjt_exit(t, (PJ16_NEXT() + imm) & 0xFFFFu, t->ninsn, 2);
#endif
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

/* V15: small fixed-mode histogram, sampled on each translator refusal.
 * The original stop_0f[256] aggregate and all native dispatch stay intact. */
static B86_PJ_COLD void pj_v15_note_0f_stop(B86Pj *j, unsigned opcode,
                                            unsigned prefix_count)
{
    static const uint8_t ops[16] = {
        0x00u, 0x01u, 0x02u, 0x20u, 0x22u, 0x83u, 0x84u, 0x85u,
        0x86u, 0xA8u, 0xA9u, 0xB6u, 0xB7u, 0xBAu, 0xBDu, 0xBFu
    };
    unsigned k, mode = j->pm16_active ? 0u : 1u;
    ++j->v15_0f_total[mode];
    if (prefix_count) ++j->v15_0f_prefixed[mode];
    for (k = 0u; k < 16u; ++k)
        if (ops[k] == opcode) { ++j->v15_0f_stop[mode][k]; break; }
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

/* r71: rebuild the code-page filter, exact set and per-page lists from the
 * live blocks of one half (the other half was just emptied). */
static B86_PJ_COLD void pj_rebuild_walk(B86Pj *j, uint32_t lo, uint32_t hi);
static B86_PJ_COLD void pj_rebuild_pages(B86Pj *j, uint32_t lo, uint32_t hi)
{
    memset(j->cpg, 0, sizeof j->cpg);
    memset(pj_cpsv(j), 0, 2u * B86_PJ_CPS * sizeof(uint32_t));
    pj_rebuild_walk(j, lo, hi);
    if (j->hot) pj_rebuild_walk(j, j->hot_start, j->hot_used);     /* r72: SRAM blocks stay tracked */
}
static B86_PJ_COLD void pj_rebuild_walk(B86Pj *j, uint32_t lo, uint32_t hi)
{
    uint32_t off = lo, *heads = pj_heads(j);
    while (off < hi) {
        if (hi - off < PJ_HDR_BYTES) break;
        uint32_t *hd = pj_hdr(j, off);
        const uint32_t size = hd[5];
        if (size < PJ_HDR_BYTES || off + size > hi) break;
        if (!(hd[1] & 0x80000000u)) {
            const uint32_t pga = hd[0] >> 12, pgb = (hd[0] + (hd[1] & 0x3FFFFFFFu) - 1u) >> 12;
            uint32_t pg;
            int sa, sb;
            for (pg = pga; ; ++pg) { const uint32_t h = PJ_PGH(pg); j->cpg[h >> 5] |= 1u << (h & 31u); if (pg == pgb) break; }
            if (hd[6]) { const uint32_t h = PJ_PGH(hd[6]); j->cpg[h >> 5] |= 1u << (h & 31u); (void)pj_cps_add(j, hd[6]); }
            sa = pj_cps_add(j, pga);
            sb = pgb != pga ? pj_cps_add(j, pgb) : sa;
            if (sa && sb) {
                hd[2] = heads[sa - 1]; heads[sa - 1] = off + 1u;
                if (pgb != pga) { hd[3] = heads[sb - 1]; heads[sb - 1] = off + 1u; } else hd[3] = 0u;
            } else hd[1] |= 0x80000000u;           /* cannot track it: treat as dead */
        }
        off += size;
    }
}
/* r73h: integrity audit after every eviction and tier reset (rare events).
 * Checks that every logged chained jump, every hash entry and every SRAM
 * redirect still points at a live block of the right EIP. Prints a summary
 * line and the first few violations. Read-only: changes nothing. */
static unsigned long pj_aud_runs, pj_aud_bad_total;
static int pj_aud_live(const B86Pj *j, uint32_t off)
{
    unsigned h;
    if (pj_in_hot(j, off)) return off >= j->hot_start && off < j->hot_used;
    for (h = 0u; h < 2u; ++h) {
        const uint32_t lo = j->shared_end + j->half * h;
        const uint32_t hi = h == j->cur_half ? j->used : (j->half_fill[h] ? j->half_fill[h] : lo);
        if (off >= lo && off < hi) return 1;
    }
    return 0;
}
static uint32_t pj_aud_bw(const uint8_t *at)       /* decode B.W/BL T4 target as code offset */
{
    const uint32_t h1 = (uint32_t)at[0] | ((uint32_t)at[1] << 8), h2 = (uint32_t)at[2] | ((uint32_t)at[3] << 8);
    const uint32_t s = (h1 >> 10) & 1u, j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
    const uint32_t i1 = (~(j1 ^ s)) & 1u, i2 = (~(j2 ^ s)) & 1u;
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FFu) << 12) | ((h2 & 0x7FFu) << 1);
    if (s) imm |= 0xFE000000u;
    return (uint32_t)(uintptr_t)(at + 4) + imm;
}
static int pj_aud_dup(const uint32_t *rd, uint32_t n, uint32_t i)   /* r73j: another entry for the same X? */
{
    uint32_t k;
    for (k = 0; k < n; ++k) if (k != i && rd[4u * k] == rd[4u * i]) return (int)k + 1;
    return 0;
}
static B86_PJ_COLD void pj_audit(B86Pj *j, const char *where)
{
    uint32_t i, k, *lk = pj_links(j);
    unsigned long badl = 0, bade = 0, badr = 0, shown = 0;
    const uint32_t base = (uint32_t)(uintptr_t)j->code;
    for (i = 0; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i], to = lk[2u * i + 1u];
        const uint32_t tgt = pj_aud_bw(j->code + so + 10u) - base;
        const int ok_src = pj_aud_live(j, so), ok_to = pj_aud_live(j, to);
        if (!ok_src || !ok_to || tgt != to) {
            ++badl;
            if (shown++ < 4u) printf("[bb386-audit]   link #%lu so=%08lX to=%08lX bw=%08lX src-live=%d to-live=%d\n",
                                     (unsigned long)i, (unsigned long)so, (unsigned long)to, (unsigned long)tgt, ok_src, ok_to);
#if BB_PJ_LINK_AUDIT_BYTES_V11
            /* Cold path only, and only for the first four existing audit
             * failures. No branch repair, cache write or extra hot-path work. */
            if (shown <= 4u) {
                const uint8_t *s = j->code + so;
                const uint8_t *b = s + 10u;
                const uint32_t h1 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
                const uint32_t h2 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
                const unsigned bw_form = ((h1 & 0xF800u) == 0xF000u &&
                                          (h2 & 0xD000u) == 0x9000u);
                printf("[bb386-v11-link] i=%lu so=%08lX to=%08lX got=%08lX "
                       "delta=%ld form-bw=%u stub=%02X%02X%02X%02X "
                       "branch=%02X%02X%02X%02X\n",
                       (unsigned long)i, (unsigned long)so, (unsigned long)to,
                       (unsigned long)tgt, (long)(int32_t)(tgt - to), bw_form,
                       (unsigned)s[0], (unsigned)s[1], (unsigned)s[2], (unsigned)s[3],
                       (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
            }
#endif
        }
    }
    for (k = 0; k < B86_PJ_HASH; ++k) {
        uint32_t e, *hd;
        if (!j->ent[k]) continue;
        e = j->ent[k] - 1u;
        if (!pj_aud_live(j, e) || e < PJ_HDR_BYTES) { ++bade;
            if (shown++ < 8u) printf("[bb386-audit]   ent[%lu] tag=%08lX off=%08lX NOT LIVE\n", (unsigned long)k, (unsigned long)j->tag[k], (unsigned long)e);
            continue; }
        hd = pj_hdr(j, e - PJ_HDR_BYTES);
        if ((hd[4] | 1u) != j->tag[k]) { ++bade;
            if (shown++ < 8u) printf("[bb386-audit]   ent[%lu] tag=%08lX off=%08lX hdr-eip=%08lX dead=%lu\n", (unsigned long)k,
                                     (unsigned long)j->tag[k], (unsigned long)e, (unsigned long)hd[4], (unsigned long)(hd[1] >> 31)); }
    }
    if (j->hot) {
        const uint32_t *rd = pj_redir(j);
        for (i = 0; i < j->nredir; ++i) {
            const uint32_t off = rd[4u * i], hent = rd[4u * i + 1u] - 1u;
            const uint8_t *code = j->code + off + PJ_HDR_BYTES;
            uint32_t w;
            memcpy(&w, code + 4, 4);
            if (!pj_aud_live(j, off) || !pj_aud_live(j, hent) || code[0] != 0xDFu || code[1] != 0xF8u ||
                w != (((uint32_t)(uintptr_t)(j->code + hent)) | 1u) || pj_hdr(j, off)[4] != pj_hdr(j, hent - PJ_HDR_BYTES)[4]) {
                ++badr;
                if (shown++ < 12u) printf("[bb386-audit]   redir #%lu off=%08lX hot=%08lX live=%d/%d op=%02X%02X w=%08lX want=%08lX x-eip=%08lX h-eip=%08lX x-hd1=%08lX h-hd1=%08lX dup=%d\n", (unsigned long)i,
                                          (unsigned long)off, (unsigned long)hent, pj_aud_live(j, off), pj_aud_live(j, hent), code[0], code[1],
                                          (unsigned long)w, (unsigned long)(((uint32_t)(uintptr_t)(j->code + hent)) | 1u),
                                          (unsigned long)pj_hdr(j, off)[4], (unsigned long)pj_hdr(j, hent - PJ_HDR_BYTES)[4],
                                          (unsigned long)pj_hdr(j, off)[1], (unsigned long)pj_hdr(j, hent - PJ_HDR_BYTES)[1], pj_aud_dup(rd, j->nredir, i));
            }
        }
    }
    ++pj_aud_runs; pj_aud_bad_total += badl + bade + badr;
    printf("[bb386-audit] %s #%lu links=%lu bad-links=%lu bad-ent=%lu redirs=%lu bad-redir=%lu (total-bad=%lu) cur=%u used=%08lX fill=%08lX/%08lX hot=%08lX..%08lX\n",
           where, pj_aud_runs, (unsigned long)j->nlinks, badl, bade, (unsigned long)j->nredir, badr, pj_aud_bad_total,
           j->cur_half, (unsigned long)j->used, (unsigned long)j->half_fill[0], (unsigned long)j->half_fill[1],
           (unsigned long)j->hot_start, (unsigned long)j->hot_used);
}
/* V12: eviction must not write through a stale link-log entry.
 * Validate BOTH Thumb branch shapes and the exact recorded target. This
 * check and its counter run on the cold eviction path, not in native code. */
#ifndef BB_PJ_LINK_SAFE_EVICT_V12
#define BB_PJ_LINK_SAFE_EVICT_V12 1
#endif
#if BB_PJ_LINK_SAFE_EVICT_V12
static uint32_t pj_aud_bw(const uint8_t *at); /* existing audit decoder below */
static unsigned long pj_v12_stale_link_drops;
static int pj_v12_link_intact(const B86Pj *j, uint32_t so, uint32_t to)
{
    const uint8_t *const stub = j->code + so;
    const uint8_t *const branch = stub + 10u;
    const uint32_t bl1 = (uint32_t)stub[0] | ((uint32_t)stub[1] << 8);
    const uint32_t bl2 = (uint32_t)stub[2] | ((uint32_t)stub[3] << 8);
    const uint32_t bw1 = (uint32_t)branch[0] | ((uint32_t)branch[1] << 8);
    const uint32_t bw2 = (uint32_t)branch[2] | ((uint32_t)branch[3] << 8);
    /* r71 static linked exit: BL chk at +0, B.W target at +10. */
    if ((bl1 & 0xF800u) != 0xF000u || (bl2 & 0xD000u) != 0xD000u ||
        (bw1 & 0xF800u) != 0xF000u || (bw2 & 0xD000u) != 0x9000u)
        return 0;
    return pj_aud_bw(branch) == (uint32_t)(uintptr_t)(j->code + to);
}
#endif
/* v48: one-pass verified native-link unchaining. Any malformed link
 * returns failure to the *existing* v27 reconciliation full-flush, which
 * retires the entire code cache before any further native execution.
 * Each record is still bounds/live/code-shape/target checked BEFORE its
 * individual 4-byte exit instruction is rewritten. */
#ifndef B86_PJ_V48_UNLINKPASS
#define B86_PJ_V48_UNLINKPASS 1
#endif
#if B86_PJ_V48_UNLINKPASS != 0 && B86_PJ_V48_UNLINKPASS != 1
#error B86_PJ_V48_UNLINKPASS must be 0 or 1
#endif
#if B86_PJ_V31_LAZY_EPOCH
/* A conservative epoch fence: no direct B.W connection can cross the
 * validation boundary. Decode each currently logged stub with the SAME
 * exact code-form and target checker used during ordinary half eviction.
 * Never patch an unverified address; a failure forces a full cache flush. */
static B86_PJ_COLD int pj_v31_unlink_all(B86Pj *j)
{
    uint32_t i, *lk = pj_links(j);
    /* Corrupt metadata must never index beyond the bounded link log. */
    if (j->nlinks > B86_PJ_LINKS) return 0;
#if B86_PJ_V48_UNLINKPASS
    /* Validating and rewriting one link at a time avoids a second full
     * scan of the PSRAM-resident link ledger. A later failed record forces
     * a complete cache flush in b86_pj_v27_reconcile() before any PJ reentry;
     * partially unlinked stubs therefore cannot execute after failure. */
    for (i = 0u; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i], to = lk[2u * i + 1u];
        uint8_t *stub;
        if (so > UINT32_MAX - 13u ||
            !pj_aud_live(j, so) || !pj_aud_live(j, so + 13u) ||
            !pj_aud_live(j, to) || !pj_v12_link_intact(j, so, to))
            return 0; /* caller full-flushes the *entire* PJ code cache */
        stub = j->code + so;
#else
    /* Original two-phase control: validate all records, then rewrite all. */
    /* Preflight every record BEFORE modifying any executable code. */
    for (i = 0u; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i], to = lk[2u * i + 1u];
        if (so > UINT32_MAX - 13u ||
            !pj_aud_live(j, so) || !pj_aud_live(j, so + 13u) ||
            !pj_aud_live(j, to) || !pj_v12_link_intact(j, so, to))
            return 0;
    }
    for (i = 0u; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i];
        uint8_t *stub = j->code + so;
#endif
        stub[0] = 0x00u; stub[1] = 0xF0u;
        stub[2] = 0x00u; stub[3] = 0x90u;
        pj_patch_b(stub, pj_in_hot(j, so) ? j->hsh.exit_s : j->exit_s);
        stub[3] |= 0x40u; /* BL exit_s, identical to eviction unchaining */
#if defined(__linux__)
        __builtin___clear_cache((char *)stub, (char *)stub + 14);
#endif
    }
#if !defined(__linux__)
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    pj_v31_unlinked += j->nlinks;
    j->nlinks = 0u;
    return 1;
}
#endif
/* r71: the current half is full: empty the other one and continue there. */
static B86_PJ_COLD void pj_evict_switch(B86Cpu *c, B86Pj *j)
{
    const unsigned other = j->cur_half ^ 1u;
    const uint32_t lo = j->shared_end + j->half * other, hi = lo + j->half;
    uint32_t *lk = pj_links(j), i, n = 0u, k;
    /* links: drop those living in the evicted half, undo those pointing into it */
    for (i = 0; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i], to = lk[2u * i + 1u];
        if (so >= lo && so < hi) continue;
#if BB_PJ_LINK_SAFE_EVICT_V12
        /* A surviving address range is NOT proof that this is still its
         * original linked exit stub. Reused code may contain unrelated
         * Thumb instructions: never overwrite those on unchain. Drop only
         * stale log metadata; leave executable bytes untouched. */
        if (!pj_v12_link_intact(j, so, to)) {
            ++pj_v12_stale_link_drops;
            continue;
        }
#endif
        if (to >= lo && to < hi) {
            uint8_t *stub = j->code + so;
            stub[0] = 0x00u; stub[1] = 0xF0u; stub[2] = 0x00u; stub[3] = 0x90u;
            pj_patch_b(stub, j->exit_s); stub[3] |= 0x40u;              /* BL exit_s again */
#if defined(__linux__)
            __builtin___clear_cache((char *)stub, (char *)stub + 14);
#endif
            ++j->unlinks;
            continue;
        }
        lk[2u * n] = so; lk[2u * n + 1u] = to; ++n;
    }
    j->nlinks = n;
#if BB_PJ_LINK_SAFE_EVICT_V12
    /* Cold-path count is cumulative; the unmodified audit still runs below. */
    if (pj_v12_stale_link_drops)
        printf("[bb386-v12-linkgc] stale-dropped-total=%lu (no writes to stale sources)\n",
               pj_v12_stale_link_drops);
#endif
#if !defined(__linux__)
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    /* table entries into the evicted half */
    for (k = 0; k < B86_PJ_HASH; ++k) if (j->ent[k] && j->ent[k] - 1u >= lo && j->ent[k] - 1u < hi) { j->ent[k] = 0u; j->tag[k] = 0u; }
    for (k = 0; k < PJ_PM16_CTX * PJ_PM16_ENT; ++k) {
        uint32_t *tg = pj_pm16_tag(j, k / PJ_PM16_ENT), *en = pj_pm16_ent(j, k / PJ_PM16_ENT);
        const uint32_t e = en[k % PJ_PM16_ENT];
        if (e && e - 1u >= lo && e - 1u < hi) { en[k % PJ_PM16_ENT] = 0u; tg[k % PJ_PM16_ENT] = 0u; }
    }
    memset(j->es_skip_eip, 0, sizeof j->es_skip_eip);
    if (B86_PJ_PCS) {   uint32_t *gi = pj_gidx(j), g;                 /* r72f: granule index entries into the evicted half */
        for (g = lo >> j->gshift; g <= (hi - 1u) >> j->gshift && g < PJ_GIDX_N; ++g)
            if (gi[g] && gi[g] - 1u >= lo && gi[g] - 1u < hi) gi[g] = 0u;
    }
    if (j->hot) {                                   /* r72: trampolines in the evicted half are gone */
        uint32_t *rd = pj_redir(j), m = 0u;
        for (i = 0; i < j->nredir; ++i) if (!(rd[4u * i] >= lo && rd[4u * i] < hi)) {
            rd[4u * m] = rd[4u * i]; rd[4u * m + 1u] = rd[4u * i + 1u];
            rd[4u * m + 2u] = rd[4u * i + 2u]; rd[4u * m + 3u] = rd[4u * i + 3u]; ++m;
        }
        j->nredir = m;
    }
    /* survivors: the current half */
    {
        const uint32_t clo = j->shared_end + j->half * j->cur_half;
        pj_rebuild_pages(j, clo, j->used);
    }
    j->half_fill[other] = 0u;
    j->cur_half = other;
    j->used = lo;
    ++j->evictions; ++j->gen;
    b86_v30f_note_eviction(&v30f_admit, (uint32_t)j->blocks);
    c->pj_stop = 1u;
    /* V30D: report only early and every 16th eviction. This counts total
     * translations and declines generated between PREVIOUS samples. */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    if (j->evictions <= 4u || (j->evictions & 15u) == 0u) {
        printf("[bb386-v30d-residency] evict=%lu since=%lu blocks=%lu declines=%lu half=%lu fills=%lu,%lu hotresets=%lu gen=%lu\n",
               j->evictions, j->evictions - v30d_last_sample_evicts,
               j->blocks - v30d_last_sample_blocks,
               j->declines - v30d_last_sample_declines,
               (unsigned long)j->cur_half,
               (unsigned long)j->half_fill[0], (unsigned long)j->half_fill[1],
               j->hot_resets, j->gen);
        v30d_last_sample_evicts = j->evictions;
        v30d_last_sample_blocks = j->blocks;
        v30d_last_sample_declines = j->declines;
    }
#endif
    pj_audit(j, "evict");
}
static B86_PJ_COLD uint32_t b86_pj_translate(B86Cpu *c, uint32_t eip);
/* V13: cold hot-reset link log cleanup. The V12 eviction guard protects
 * writes at eviction time. Prune malformed records immediately after the
 * hot-reset restores PSRAM trampolines, before the integrity audit. This
 * changes metadata ONLY; it neither patches code nor changes translations. */
#ifndef BB_PJ_LINK_PRUNE_HOTRESET_V13
#define BB_PJ_LINK_PRUNE_HOTRESET_V13 1
#endif
#if BB_PJ_LINK_PRUNE_HOTRESET_V13
static unsigned long pj_v13_hotreset_stale_total;
static void pj_v13_prune_links(B86Pj *j)
{
    uint32_t *const lk = pj_links(j);
    uint32_t i, n = 0u, dropped = 0u;
    for (i = 0u; i < j->nlinks; ++i) {
        const uint32_t so = lk[2u * i], to = lk[2u * i + 1u];
        /* Validate bounds BEFORE V12 reads the 14-byte exit stub. */
        if (so >= j->shared_end && so <= j->cap - 14u &&
            to < j->cap && pj_aud_live(j, so) && pj_aud_live(j, to) &&
            pj_v12_link_intact(j, so, to)) {
            lk[2u * n] = so;
            lk[2u * n + 1u] = to;
            ++n;
        } else ++dropped;
    }
    j->nlinks = n;
    pj_v13_hotreset_stale_total += dropped;
    if (dropped)
        printf("[bb386-v13-linkgc] hotreset-stale-dropped=%lu total=%lu (no code writes)\n",
               (unsigned long)dropped, pj_v13_hotreset_stale_total);
}
#endif
/* r72: empty the SRAM tier: trampolines become exits to C, entries into
 * the tier are dropped from the tables, links inside it are forgotten. */
static unsigned long pj_hr_redirs, pj_hr_restored, pj_hr_dead, pj_hr_short, pj_hr_bad;   /* r73g, r73j */
static unsigned long pj_redir_overlap, pj_link_tramp;   /* r73k */
static B86_PJ_COLD void pj_hot_reset(B86Cpu *c, B86Pj *j)
{
    uint32_t i, k, n = 0u, *rd = pj_redir(j), *lk = pj_links(j);
    pj_hr_redirs += j->nredir;
    for (i = 0; i < j->nredir; ++i) {
        /* r72c: the PSRAM copy is still a correct translation unless its
         * SRAM copy was killed by a code write: put its first 8 bytes back
         * and make it live again (no retranslation after a tier reset) */
        const uint32_t off = rd[4u * i], hot_off = rd[4u * i + 1u] - 1u - PJ_HDR_BYTES;
        uint8_t *code = j->code + off + PJ_HDR_BYTES;
        /* r73g: the PSRAM copy X was dead while redirected, so SMC kills only
         * reached the SRAM copy H. X is still valid only if H covered every
         * guest byte X was translated from (same start, same physical page,
         * at least the same length). A shorter H let a store into X's tail go
         * unnoticed; reviving X then ran stale code. */
        const uint32_t *hx = pj_hdr(j, off), *hh = pj_hdr(j, hot_off);
        const int h_alive = !(hh[1] & 0x80000000u);
        const int covered = hh[0] == hx[0] && hh[4] == hx[4] && hh[6] == hx[6] &&
                            (hh[1] & 0x3FFFFFFFu) >= (hx[1] & 0x3FFFFFFFu);
        /* r73j: X must still hold exactly the trampoline this entry wrote
         * (LDR.W pc,[pc,#0] + this H's address). If anything rewrote X since,
         * the saved 8 bytes no longer belong to the code around them, and
         * putting them back would splice two different translations together. */
        uint32_t w;
        int intact;
        memcpy(&w, code + 4, 4);
        intact = code[0] == 0xDFu && code[1] == 0xF8u && code[2] == 0x00u && code[3] == 0xF0u &&
                 w == (((uint32_t)(uintptr_t)(j->code + rd[4u * i + 1u] - 1u)) | 1u) &&
                 (hx[1] & 0x80000000u);
        if (!intact) {
            if (pj_hr_bad++ < 16u)
                printf("[bb386-hotreset]   BAD redir #%lu off=%08lX hot=%08lX op=%02X%02X%02X%02X w=%08lX want=%08lX x-eip=%08lX h-eip=%08lX x-hd1=%08lX h-hd1=%08lX\n",
                       (unsigned long)i, (unsigned long)off, (unsigned long)(rd[4u * i + 1u] - 1u), code[0], code[1], code[2], code[3],
                       (unsigned long)w, (unsigned long)(((uint32_t)(uintptr_t)(j->code + rd[4u * i + 1u] - 1u)) | 1u),
                       (unsigned long)hx[4], (unsigned long)hh[4], (unsigned long)hx[1], (unsigned long)hh[1]);
        }
        if (!h_alive) ++pj_hr_dead; else if (!covered) ++pj_hr_short;
        if (h_alive && covered && intact) {
            ++pj_hr_restored;
            memcpy(code, &rd[4u * i + 2u], 8u);
            pj_hdr(j, off)[1] &= ~0x80000000u;
            pj_hdr(j, off)[7] = 0u;
        } else {
            PjE e;
            e.p = code; e.end = code + 16; e.bad = 0;
            pj_movw(&e, R1, pj_hdr(j, off)[4] & 0xFFFFu); pj_movt(&e, R1, pj_hdr(j, off)[4] >> 16);
            pj_movw(&e, R0, 0u);
            pj_patch_b(pj_b(&e, -1), j->tail_z);
        }
#if defined(__linux__)
        __builtin___clear_cache((char *)code, (char *)code + 16);
#endif
    }
    /* r72d: raise the bar only when the tier refilled quickly (thrash);
     * a slow refill means it is simply full of genuinely hot code */
    if (j->promotions - j->promo_at_reset < 512u) { if (j->hot_after < 256u) j->hot_after *= 2u; }
    else if (j->hot_after > PJ_HOT_AFTER) j->hot_after /= 2u;
    j->promo_at_reset = j->promotions;
    j->nredir = 0u;
    for (k = 0; k < B86_PJ_HASH; ++k) if (j->ent[k] && pj_in_hot(j, j->ent[k] - 1u)) { j->ent[k] = 0u; j->tag[k] = 0u; }
    for (i = 0; i < j->nlinks; ++i) {
        if (pj_in_hot(j, lk[2u * i]) || pj_in_hot(j, lk[2u * i + 1u])) continue;
        lk[2u * n] = lk[2u * i]; lk[2u * n + 1u] = lk[2u * i + 1u]; ++n;
    }
    j->nlinks = n;
#if BB_PJ_LINK_PRUNE_HOTRESET_V13
    pj_v13_prune_links(j);
#endif
    j->hot_used = j->hot_start;
    pj_rebuild_pages(j, j->shared_end + j->half * j->cur_half, j->used);
    if (j->half_fill[j->cur_half ^ 1u]) pj_rebuild_walk(j, j->shared_end + j->half * (j->cur_half ^ 1u), j->half_fill[j->cur_half ^ 1u]);
#if !defined(__linux__)
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    ++j->hot_resets; ++j->gen;
    c->pj_stop = 1u;
    printf("[bb386-hotreset] #%lu redirects=%lu restored=%lu stubbed-dead=%lu stubbed-short=%lu stubbed-bad=%lu refused-overlap=%lu link-tramp=%lu (totals) evictions=%lu\n",
           j->hot_resets, pj_hr_redirs, pj_hr_restored, pj_hr_dead, pj_hr_short, pj_hr_bad, pj_redir_overlap, pj_link_tramp, j->evictions);
    pj_audit(j, "hotreset");
}
/* r72: translate eip into the SRAM tier; returns its entry (0 = refused) */
static B86_PJ_COLD uint32_t pj_hot_translate(B86Cpu *c, B86Pj *j, uint32_t eip)
{
    const uint32_t used0 = j->used;
    const unsigned long f0 = j->flushes;
    struct PjSh mainsh;
    uint32_t e;
    if (j->hot_base + j->hot_cap - j->hot_used < 4096u || j->nredir >= j->maxredir) pj_hot_reset(c, j);
    pj_sh_get(j, &mainsh);
    pj_sh_set(j, &j->hsh);                           /* emit against the SRAM copies of the routines */
    j->tr_hot = 1u; j->used = j->hot_used;
    e = b86_pj_translate(c, eip);
    j->tr_hot = 0u;
    if (j->flushes != f0) return 0u;                 /* a full flush re-emitted the main routines and reset the tier */
    j->hot_used = j->used; j->used = used0;
    pj_sh_set(j, &mainsh);
    if (!e) ++j->hot_refused;
    return e;
}
/* r72: PSRAM block at off becomes LDR PC,[PC,#0]; .word hot entry */
/* V30J_LINKFILTER_BEGIN: bit-exact, conservative link-overlap prefilter.
 * The original 14-byte chain stub protects against overwriting the first
 * eight bytes of a PSRAM block during a hot-tier redirect. Every NEW link is
 * marked in a monotonic 16-byte-bin bitset. Bits are NEVER cleared; stale
 * bits produce only false positives (an ordinary exact scan). No zero bit
 * can occur for a live link intersecting the proposed 8-byte redirect.
 * This remains safe across ring evictions, hot resets, separate B86Pj state
 * objects, and all other link compaction. It does not alter linked code.
 * A/B mode 0 retains the original linear guard without the 8 KiB bitset. */
#ifndef B86_PJ_V30J_LINK_FILTER
#define B86_PJ_V30J_LINK_FILTER 1
#endif
#if B86_PJ_V30J_LINK_FILTER != 0 && B86_PJ_V30J_LINK_FILTER != 1
#error B86_PJ_V30J_LINK_FILTER must be 0 or 1
#endif
#if B86_PJ_V30J_LINK_FILTER
/* V30J_LINKFILTER_HELPER_BEGIN */
#define PJ_V30J_BITS 65536u
static uint8_t pj_v30j_bits[PJ_V30J_BITS / 8u];
static unsigned long pj_v30j_marks, pj_v30j_skips, pj_v30j_scans;
static inline unsigned pj_v30j_slot(uint32_t bin)
{
    return (unsigned)((bin * 2654435761u) >> 16);
}
static inline void pj_v30j_mark_bin(uint32_t bin)
{
    const unsigned bit = pj_v30j_slot(bin);
    pj_v30j_bits[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}
static inline int pj_v30j_has_bin(uint32_t bin)
{
    const unsigned bit = pj_v30j_slot(bin);
    return (pj_v30j_bits[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0;
}
static inline void pj_v30j_mark_link(uint32_t stub_off)
{
    /* A linked exit covers [stub_off, stub_off+14). A 14-byte span
     * intersects at most two 16-byte bins. */
    const uint32_t first = stub_off >> 4;
    const uint32_t last = (stub_off + 13u) >> 4;
    pj_v30j_mark_bin(first);
    if (last != first) pj_v30j_mark_bin(last);
    ++pj_v30j_marks;
}
static inline int pj_v30j_possible_overlap(uint32_t code_off)
{
    /* Redirect overwrites [code_off, code_off+8). It intersects at most
     * two 16-byte bins. A hash collision can only cause a false positive.
     * Return TRUE if either bin is marked, not if both are marked. */
    const uint32_t first = code_off >> 4;
    const uint32_t last = (code_off + 7u) >> 4;
    return pj_v30j_has_bin(first) ||
           (last != first && pj_v30j_has_bin(last));
}
/* V30J_LINKFILTER_HELPER_END */
#endif
/* V30J_LINKFILTER_END */

static B86_PJ_COLD int pj_redirect(B86Pj *j, uint32_t off, uint32_t hot_ent)
{
    uint8_t *code = j->code + off + PJ_HDR_BYTES;
    /* r73k: the trampoline overwrites code+0..7. In a tiny block an exit stub
     * starts inside that range (e.g. at code+4), and if that stub is a logged
     * chain link, eviction later unlinks it by writing BL exit_s over the
     * trampoline's address word (seen as w=F7xxFBxx). Never trampoline over a
     * logged link stub. */
    {
        const uint32_t c0 = off + PJ_HDR_BYTES;
        int need_exact_scan = 1;
#if B86_PJ_V30J_LINK_FILTER
        if (!pj_v30j_possible_overlap(c0)) {
            ++pj_v30j_skips;
            need_exact_scan = 0;
        } else {
            ++pj_v30j_scans;
        }
#endif
        if (need_exact_scan) {
            const uint32_t *lk = pj_links(j);
            uint32_t i;
            for (i = 0; i < j->nlinks; ++i) {
                const uint32_t so = lk[2u * i];
                if (so + 14u > c0 && so < c0 + 8u) { ++pj_redir_overlap; return 0; }
            }
        }
    }
    if (code[0] == 0xDFu && code[1] == 0xF8u && code[2] == 0x00u && code[3] == 0xF0u) {   /* r73j */
        static unsigned long twice;
        if (twice++ < 8u) printf("[bb386-redir] already redirected off=%08lX hd1=%08lX (skipped)\n",
                                 (unsigned long)off, (unsigned long)pj_hdr(j, off)[1]);
        return 0;
    }
    const uint32_t tgt = (uint32_t)(uintptr_t)(j->code + hot_ent - 1u) | 1u;
    {   uint32_t *rd = pj_redir(j) + 4u * j->nredir;
        rd[0] = off; rd[1] = hot_ent; memcpy(&rd[2], code, 8u); }
    code[0] = 0xDFu; code[1] = 0xF8u; code[2] = 0x00u; code[3] = 0xF0u;      /* LDR.W pc, [pc, #0] */
    memcpy(code + 4, &tgt, 4);
#if defined(__linux__)
    __builtin___clear_cache((char *)code, (char *)code + 8);
#else
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
    pj_hdr(j, off)[1] |= 0x80000000u;              /* dead: SMC and re-find use the SRAM copy */
    ++j->nredir;
    return 1;
}
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
    {   /* r71: ring eviction instead of a full flush */
        const uint32_t lim = j->shared_end + j->half * (j->cur_half + 1u);
        if (j->tr_hot) { if (j->hot_base + j->hot_cap - j->used < 4096u) return 0u; }   /* r72: caller resets the tier */
        else if (lim - j->used < 4096u) pj_evict_switch(c, j);
    }
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
    t.e.p = j->code + start_off + PJ_HDR_BYTES;
    t.e.end = j->tr_hot ? j->code + j->hot_base + j->hot_cap
                        : j->code + j->shared_end + j->half * (j->cur_half + 1u);       /* r71: stay inside the half */
    while (t.ninsn < B86_PJ_MAXI && t.pos + 16u <= t.n) {
#if BB_PJ_PM16_REG_TIER
        t.ip0 = j->pm16_active ? ((eip + t.pos) & 0xFFFFu) : eip + t.pos;
#ifdef B86_PJ_CPROF
        {   uint8_t *p0 = t.e.p; const unsigned pos0 = t.pos; unsigned k = pos0, op;
            r = j->pm16_active ? pjt_insn_pm16(&t) : pjt_insn(&t);
            while (k < t.n && (t.b[k] == 0x66u || t.b[k] == 0x67u || t.b[k] == 0xF2u || t.b[k] == 0xF3u || t.b[k] == 0x26u ||
                   t.b[k] == 0x2Eu || t.b[k] == 0x36u || t.b[k] == 0x3Eu || t.b[k] == 0x64u || t.b[k] == 0x65u)) ++k;
            op = k < t.n ? (t.b[k] == 0x0Fu && k + 1u < t.n ? 0x100u + t.b[k + 1u] : t.b[k]) : 0u;
            if (r != 0) { extern unsigned long b86_pj_op_bytes[512], b86_pj_op_count[512];
                b86_pj_op_bytes[op] += (unsigned long)(t.e.p - p0); ++b86_pj_op_count[op]; }
        }
#else
        r = j->pm16_active ? pjt_insn_pm16(&t) : pjt_insn(&t);
#endif
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
                if (b == 0x0Fu && k + 1u < t.n) {
                    ++j->stop_0f[t.b[k + 1u]];
#if BB_PJ_PM16_REG_TIER
                    pj_v15_note_0f_stop(j, t.b[k + 1u], pfx);
#if BB_PJ_V181_BA_STOP_CLASSIFY && BB_PJ_PM16_TRUTHDIAG
                    if (j->pm16_active && t.b[k + 1u] == 0xBAu)
                        pj_v181_note_ba_stop(t.b, t.n, t.pos, k);
#endif
#if BB_PJ_V184_STOP_CLASSIFY && BB_PJ_PM16_TRUTHDIAG
                    if (j->pm16_active)
                        pj_v184_note_0f_stop(t.b, t.n, t.pos, k);
#endif
#endif
                }
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
#ifdef B86_PJ_CPROF
    { extern unsigned long b86_pj_stub_bytes, b86_pj_blocks_x, b86_pj_insns_x; uint8_t *p0 = t.e.p;
      for (i = 0; i < t.nex; ++i) pjt_stub(&t, &t.ex[i], j);
      b86_pj_stub_bytes += (unsigned long)(t.e.p - p0); ++b86_pj_blocks_x; b86_pj_insns_x += t.ninsn; }
#else
    for (i = 0; i < t.nex; ++i) pjt_stub(&t, &t.ex[i], j);
#endif
    if (t.e.bad) return 0u;
    while (t.e.p < j->code + start_off + PJ_HDR_BYTES + 16u) pje16(&t.e, 0xBF00u);   /* room for a kill stub */
    if (t.e.bad) return 0u;
    /* V29: add two exact physical page identities BEFORE the V28 bytes.
     * Each source range is <=160 bytes, so at most two 4 KiB pages.
     * Store only after checking available code-cache space. */
    {
        const uint32_t sn = (t.pos + 3u) & ~3u;
#if BB_PJ_PM16_REG_TIER
        const uint32_t lin0 = (j->pm16_active ? j->pm16_base : 0u) + eip;
#else
        const uint32_t lin0 = eip;
#endif
        const uintptr_t p = (uintptr_t)t.e.p;
        const uintptr_t aligned = (p + 3u) & ~(uintptr_t)3u;
        uint32_t p0, p1 = PJ_V29_ABSENT;
        if (!t.pos || t.pos > sizeof bytes ||
            lin0 > UINT32_MAX - (t.pos - 1u) ||
            aligned < p || (uintptr_t)t.e.end < aligned ||
            (uintptr_t)t.e.end - aligned < sn + 8u ||
            !pj_v29_phys_id(c, lin0, &p0)) return 0u;
        if (((lin0 & 0xFFFu) + t.pos) > 4096u) {
            const uint32_t lin2 = (lin0 & ~0xFFFu) + 4096u;
            if (!pj_v29_phys_id(c, lin2, &p1)) return 0u;
        }
        t.e.p = (uint8_t *)aligned;
        memcpy(t.e.p, &p0, 4u); memcpy(t.e.p + 4u, &p1, 4u);
        t.e.p += 8u;
        memcpy(t.e.p, t.b, t.pos);
        if (sn > t.pos) memset(t.e.p + t.pos, 0, sn - t.pos);
        t.e.p += sn;
    }
    if (c->pg) j->v28_paged_seen = 1u;
    {   /* r71: linear code pages -> filter, exact set, per-page block lists;
         * physical code pages -> filter + exact set (Stage4U alias tracking) */
#if BB_PJ_PM16_REG_TIER
        const uint32_t lin0 = (j->pm16_active ? j->pm16_base : 0u) + eip;
#else
        const uint32_t lin0 = eip;
#endif
        const uint32_t pga = lin0 >> 12, pgb = (lin0 + t.pos - 1u) >> 12;
        uint32_t *hd = pj_hdr(j, start_off), *heads = pj_heads(j), pg;
        int sa, sb;
        for (pg = pga; ; ++pg) {
            uint32_t phys, pkey, ph;
            const uint32_t h = PJ_PGH(pg);
            j->cpg[h >> 5] |= 1u << (h & 31u);
            if (c->pg) { if (!b86_pg_walk(c, pg << 12, &phys)) { b86_pj_flush(c); return 0u; } }
            else phys = pg << 12;
            if (phys >= B86_RAM_BYTES) { b86_pj_flush(c); return 0u; }
            pkey = 0x40000000u | (phys >> 12);
            ph = PJ_PGH(pkey);
            j->cpg[ph >> 5] |= 1u << (ph & 31u);
            if (!pj_cps_add(j, pkey)) { ++j->profile_cps_refusals; b86_pj_flush(c); ++j->flush_cps; return 0u; }
            if (pg == pgb) break;
        }
        sa = pj_cps_add(j, pga);
        sb = pgb != pga ? pj_cps_add(j, pgb) : sa;
        if (!sa || !sb) { ++j->profile_cps_refusals; b86_pj_flush(c); ++j->flush_cps; return 0u; }
        hd[0] = lin0; hd[4] = eip;
        hd[6] = 0u;
        if (c->pg) { uint32_t ph0; if (b86_pg_walk(c, pga << 12, &ph0)) hd[6] = 0x40000000u | (ph0 >> 12); }
        else hd[6] = 0x40000000u | pga;
#if BB_PJ_PM16_REG_TIER
        hd[1] = t.pos | (j->pm16_active ? 0x40000000u : 0u);  /* bit30: PM16 block */
#else
        hd[1] = t.pos;
#endif
        hd[2] = heads[sa - 1]; heads[sa - 1] = start_off + 1u;
        if (pgb != pga) { hd[3] = heads[sb - 1]; heads[sb - 1] = start_off + 1u; } else hd[3] = 0u;
    }
    j->used = ((uint32_t)(t.e.p - j->code) + 3u) & ~3u;
    pj_hdr(j, start_off)[5] = j->used - start_off;    /* r71: walkable block chain */
    if (!j->tr_hot) j->half_fill[j->cur_half] = j->used;
    pj_hdr(j, start_off)[7] = 0u;
    if (B86_PJ_PCS && !j->tr_hot) { uint32_t *gi = pj_gidx(j) + (start_off >> j->gshift); if (!*gi) *gi = start_off + 1u; }   /* r72f */
#if defined(__linux__)
    __builtin___clear_cache((char *)j->code + start_off, (char *)t.e.p);
#else
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");      /* Cortex-M: no I-cache on SRAM */
#endif
    ++j->blocks;
    return start_off + PJ_HDR_BYTES + 1u;
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
/* BB_V30D_RESIDENCY_ADMISSION_V1: optional TWO-TOUCH admission on genuine
 * missing native translations, active only after 32 ring evictions. This
 * must never invalidate or execute code: a defer returns to the fully
 * checked interpreter; the next identical miss in the SAME cache generation
 * is eligible for ordinary compilation. A collision only changes speed.
 * Keep this table OUTSIDE the 8KiB PJ state (which is near capacity). */
#ifndef BB_V30D_ADMISSION
#define BB_V30D_ADMISSION 0
#endif
#if BB_V30D_ADMISSION && B86_V30F_MODE != 0
#error "Disable BB_V30D_ADMISSION when using v30f"
#endif
#if BB_V30D_ADMISSION
#define V30D_ADMIT_SLOTS 128u
static uint32_t v30d_admit_key[V30D_ADMIT_SLOTS];
static uint32_t v30d_admit_gen[V30D_ADMIT_SLOTS];
static unsigned long v30d_defers, v30d_admits, v30d_collisions;
#endif
static B86_PJ_COLD int pj_v30d_defer(B86Cpu *c, B86Pj *j, uint32_t eip)
{
#if BB_V30D_ADMISSION
    uint32_t lin, mix, slot, sig;
    if (!c || !j || j->evictions < 32u || j->tr_hot) return 0;
#if BB_PJ_PM16_REG_TIER
    lin = (j->pm16_active ? j->pm16_base : 0u) + eip;
    mix = j->pm16_active ? (j->pm16_kind ^ j->pm16_limit ^ 0x16B86u) : 0x32B86u;
#else
    lin = eip; mix = 0x32B86u;
#endif
    /* This signature is only a conservative PERFORMANCE hint. Exact code,
     * page identity and descriptor checks remain V30C's responsibility. */
    sig = (lin * 2654435761u) ^ (mix * 2246822519u) ^ c->cr3
          ^ (c->pg ? 0x9E3779B9u : 0u);
    slot = (sig ^ (sig >> 16)) & (V30D_ADMIT_SLOTS - 1u);
    if (v30d_admit_gen[slot] == (uint32_t)j->gen &&
        v30d_admit_key[slot] == sig) {
        ++v30d_admits;
        return 0;
    }
    if (v30d_admit_gen[slot] == (uint32_t)j->gen) ++v30d_collisions;
    v30d_admit_gen[slot] = (uint32_t)j->gen;
    v30d_admit_key[slot] = sig;
    ++v30d_defers;
    return 1;
#else
    (void)c; (void)j; (void)eip;
    return 0;
#endif
}
/* V30O bounded two-touch doorkeeper for genuine missing native blocks.
 * The old guarded admission begins after 32 cache evictions, when the game
 * is already experiencing catastrophic late cache churn. V30O arms after
 * eight evictions and remembers first sightings in an 8KiB Bloom filter.
 * False positives only compile MORE native code; false negatives are
 * impossible before reset (both bits stay set until the eviction changes).
 * Every deferred instruction follows the ordinary checked C interpreter.
 * A unique-miss dominated window suspends filtering until next eviction,
 * avoiding prolonged slow-interpretation under a large cold working set.
 * The original V30F policy remains byte-for-byte present for A/B mode=0.
 * The bloom is SRAM BSS; PJ arena and PSRAM code-cache offsets unchanged. */
#ifndef B86_PJ_V30O_DOORKEEPER
#define B86_PJ_V30O_DOORKEEPER 1
#endif
#if B86_PJ_V30O_DOORKEEPER != 0 && B86_PJ_V30O_DOORKEEPER != 1
#error B86_PJ_V30O_DOORKEEPER must be 0 or 1
#endif
#define PJ_V30O_ARM_EVICTIONS 8u
#define PJ_V30O_PROBE_LIMIT 512u
#define PJ_V30O_MIN_REVISITS 48u
#define PJ_V30O_BLOOM_BYTES 8192u
#if B86_PJ_V30O_DOORKEEPER
static uint8_t pj_v30o_bits[PJ_V30O_BLOOM_BYTES];
static uint32_t pj_v30o_epoch = UINT32_MAX;
static uint32_t pj_v30o_probe, pj_v30o_repeat, pj_v30o_suspended;
static unsigned long pj_v30o_resets, pj_v30o_first, pj_v30o_hits;
static unsigned long pj_v30o_defers, pj_v30o_bypassed, pj_v30o_escapes;
/* Returns 1 to defer translation to the ORIGINAL interpreter. */
static B86_PJ_COLD int pj_v30o_defer_key(uint64_t key, uint32_t eviction)
{
    uint32_t x, h1, h2, b1, b2;
    uint8_t m1, m2;
    int seen;
    if (pj_v30o_epoch != eviction) {
        memset(pj_v30o_bits, 0, sizeof pj_v30o_bits);
        pj_v30o_epoch = eviction;
        pj_v30o_probe = 0u;
        pj_v30o_repeat = 0u;
        pj_v30o_suspended = 0u;
        ++pj_v30o_resets;
    }
    if (pj_v30o_suspended) {
        ++pj_v30o_bypassed;
        return 0;
    }
    /* Do not hash or probe SRAM at all when the admission filter is
     * suspended: the unique-working-set case must stay inexpensive. */
    x = b86_v30f_hash(key);
    h1 = x & (PJ_V30O_BLOOM_BYTES * 8u - 1u);
    h2 = ((x >> 16) ^ (x * 0x9E3779B9u)) &
         (PJ_V30O_BLOOM_BYTES * 8u - 1u);
    b1 = h1 >> 3; b2 = h2 >> 3;
    m1 = (uint8_t)(1u << (h1 & 7u));
    m2 = (uint8_t)(1u << (h2 & 7u));
    seen = (pj_v30o_bits[b1] & m1) && (pj_v30o_bits[b2] & m2);
    /* Read both before updating either: even coincident bits cannot make
     * a first occurrence look like a second. */
    pj_v30o_bits[b1] |= m1;
    pj_v30o_bits[b2] |= m2;
    ++pj_v30o_probe;
    if (seen) { ++pj_v30o_repeat; ++pj_v30o_hits; }
    else { ++pj_v30o_first; ++pj_v30o_defers; }
    if (pj_v30o_probe >= PJ_V30O_PROBE_LIMIT &&
        pj_v30o_repeat < PJ_V30O_MIN_REVISITS) {
        pj_v30o_suspended = 1u;
        ++pj_v30o_escapes;
    }
    return !seen;
}
#endif
/* BB_V30F_ADAPTIVE_INTEGRATION_V1: only a cold cache MISS that was already
 * eligible for the old V30D filter may be deferred. A defer executes the
 * checked interpreter. Never modify the tag/entry tables on a defer.
 * The candidate cache is heuristic; physical-page/exact-byte/SMC validation
 * continues in the existing PJ path and is NOT bypassed by a permit. */
static B86_PJ_COLD int pj_v30f_defer(B86Cpu *c, B86Pj *j, uint32_t eip)
{
    uint32_t lin, mix, generation;
    uint64_t key;
    /* Keep the legacy function referenced in all builds for clean warnings. */
    if (B86_V30F_MODE == 0) return pj_v30d_defer(c, j, eip);
    if (!c || !j || j->tr_hot) return 0;
#if BB_PJ_PM16_REG_TIER
    lin = (j->pm16_active ? j->pm16_base : 0u) + eip;
    mix = j->pm16_active ? (j->pm16_kind ^ j->pm16_limit ^ 0x16B86u) : 0x32B86u;
#else
    lin = eip; mix = 0x32B86u;
#endif
    mix ^= (uint32_t)c->cr3 * 2246822519u;
    mix ^= c->pg ? 0x9E3779B9u : 0u;
    /* Cache generation changes on every routine ring eviction; that is NOT
     * code-mapping invalidation. Keep sightings across ring replacements.
     * Existing precise-SMC validation still controls native execution.
     * These fields change for whole flush / address-space transitions. */
    generation = (uint32_t)j->flushes ^ ((uint32_t)j->v29_map_changes * 0x9E3779B9u)
               ^ ((uint32_t)j->v27_barriers * 0x85EBCA6Bu);
    key = ((uint64_t)lin << 32) | (uint64_t)mix;
#if B86_PJ_V30O_DOORKEEPER
    /* Never filter startup: reserve cold filtering for the verified
     * eviction-churn phase. Original V30F remains an A/B fallback. */
    if (j->evictions >= PJ_V30O_ARM_EVICTIONS)
        return pj_v30o_defer_key(key, (uint32_t)j->evictions);
#endif
    return !b86_v30f_allow(&v30f_admit, key, generation);
}
/* Always-on cold telemetry; no eviction policy changes. */

static B86_PJ_COLD void b86_pj_link(B86Cpu *c)
{
    B86Pj *j = b86_pj_state(c);
    uint8_t *stub = (uint8_t *)((c->pj_exit & ~(uintptr_t)1u) - 4u);    /* r71: LR after BL exit_s at +0 */
    const unsigned long gen = j->flushes + j->gen;
    uint32_t h = ((c->ip * 2654435761u) >> 16) & (B86_PJ_HASH - 1u), ent;
    c->pj_exit = 0u;
#if B86_PJ_V45_DYN2
    /* Linker uses precisely the same flat PM32 pair as native dyn and C.
     * Its chosen entry is still found/validated through pj_v31_find(). */
    if (j->tag[h] != (c->ip | 1u) && j->tag[h ^ 1u] == (c->ip | 1u)) h ^= 1u;
    if (j->tag[h] != (c->ip | 1u) && j->tag[h] && !j->tag[h ^ 1u]) h ^= 1u;
#endif
    if (j->flush_pending) return;
    if (!pj_in_hot(j, (uint32_t)(stub - j->code)) && (stub < j->code + j->shared_end || stub + 14u > j->code + j->cap)) return;
    if (j->nlinks >= B86_PJ_LINKS) return;            /* r71: log full: stop linking until the next eviction */
    if (j->tag[h] != (c->ip | 1u)) {
        uint32_t e =
#if B86_PJ_V31_LAZY_EPOCH
            pj_v31_find(c, j, c->ip, c->ip, 0u);
#else
            pj_find_block(j, c->ip, c->ip, 0u);
#endif
        if (e) ++j->refinds;
        else {
            if (pj_v30f_defer(c, j, c->ip)) return;
            e = b86_pj_translate(c, c->ip);
        }
        j->tag[h] = c->ip | 1u;
        j->ent[h] = e;
        if (!e) ++j->declines;
    }
    ent = j->ent[h];
    if (!ent || j->flushes + j->gen != gen) return;  /* translation flushed/evicted: stub may be gone */
    if (pj_in_hot(j, (uint32_t)(stub - j->code)) != pj_in_hot(j, ent - 1u)) return;   /* r72: B.W cannot reach */
    {   /* r73k: never patch a stub that a redirect trampoline (LDR.W pc,[pc,#0]; .word) overlaps */
        int k;
        for (k = -6; k <= 0; k += 2) {
            const uint8_t *t = stub + k;
            if (t >= j->code && t[0] == 0xDFu && t[1] == 0xF8u && t[2] == 0x00u && t[3] == 0xF0u) { ++pj_link_tramp; return; }
        }
    }
    {
        /* r71: +10: B.W target body first, then +0: BL chk */
        stub[10] = 0x00u; stub[11] = 0xF0u; stub[12] = 0x00u; stub[13] = 0x90u;
        pj_patch_b(stub + 10, j->code + ent - 1u);
        stub[0] = 0x00u; stub[1] = 0xF0u; stub[2] = 0x00u; stub[3] = 0x90u;
        pj_patch_b(stub, pj_in_hot(j, (uint32_t)(stub - j->code)) ? j->hsh.chk : j->chk); stub[3] |= 0x40u;   /* B.W -> BL; r72: the stub's own region */
        ++j->links;
        { uint32_t *lk = pj_links(j); const uint32_t so = (uint32_t)(stub - j->code); lk[2u * j->nlinks] = so; lk[2u * j->nlinks + 1u] = ent - 1u;
#if B86_PJ_V30J_LINK_FILTER
          pj_v30j_mark_link(so); /* before record becomes visible */
#endif
          ++j->nlinks; }
#if defined(__linux__)
        __builtin___clear_cache((char *)stub, (char *)stub + 14);
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

/* r72f: PC-sample promotion. Dispatch counts only see blocks entered from
 * C; a loop chained inside PSRAM is entered once and then runs for millions
 * of instructions without being counted. The host samples core 0's PC from a
 * timer interrupt (b86_pj_pc_sample); samples that land in a PSRAM block are
 * mapped to it here, and a block that collects PJ_PCS_HOT samples within one
 * epoch (2048 samples) is moved to the SRAM tier. hd[7]: bits 0-19 dispatch
 * count, 20-27 sample count, 28-31 epoch of the sample count. */
#ifndef PJ_PCS_HOT
#define PJ_PCS_HOT 3u
#endif
static B86_PM_HOT uint32_t pj_pcs_block(const B86Pj *j, uint32_t pcoff)
{
    const uint32_t *gi = pj_gidx(j);
    uint32_t g = pcoff >> j->gshift, k, off = 0u;
    if (g >= PJ_GIDX_N) return 0u;
    for (k = 0u; k < 8u && k <= g; ++k) {
        const uint32_t v = gi[g - k];
        if (v && v - 1u <= pcoff) { off = v - 1u; break; }
    }
    if (!off) return 0u;
    for (k = 0u; k < 64u; ++k) {
        const uint32_t sz = pj_hdr(j, off)[5];
        if (sz < PJ_HDR_BYTES || sz > 0x10000u || off + sz > j->cap) return 0u;
        if (pcoff < off + sz) return pcoff >= off + PJ_HDR_BYTES ? off + 1u : 0u;
        off += sz;
    }
    return 0u;
}
static B86_PJ_COLD void pj_pcs_promote(B86Cpu *c, B86Pj *j, uint32_t off)
{
    const uint32_t ent = off + PJ_HDR_BYTES + 1u, lin = pj_hdr(j, off)[0], eip = pj_hdr(j, off)[4];
    uint32_t e2, k;
    if (pj_find_block(j, lin, eip, 0u) != ent) { pj_hdr(j, off)[7] &= 0xF00FFFFFu; return; }   /* not a live block start */
    e2 = pj_hot_translate(c, j, eip);
    if (e2 && !(pj_hdr(j, off)[1] & 0x80000000u) && pj_hdr(j, off)[4] == eip && pj_hdr(j, off)[0] == lin &&
        pj_redirect(j, off, e2)) {   /* (the new SRAM copy now heads the page list); r73k: may refuse */
        for (k = 0; k < B86_PJ_HASH; ++k) if (j->ent[k] == ent) j->ent[k] = e2;
        ++j->promotions; ++pj_pcs_cls[7];
    } else pj_hdr(j, off)[7] &= 0xF00FFFFFu;
}
static B86_PM_HOT void pj_pcs_drain(B86Cpu *c, B86Pj *j)
{
    const uint32_t w = pj_pcs_w;
    uint32_t r = pj_pcs_r;
    int ok = pj_flat(c) && pj_pcs_promote_on;          /* r73b: no promotion, no header writes */
#if BB_PJ_PM16_REG_TIER
    if (j->pm16_active) ok = 0;
#endif
    /* the sample bar follows the adaptive dispatch bar: a thrashing tier raises both */
    const uint32_t pcs_thr = PJ_PCS_HOT * j->hot_after / PJ_HOT_AFTER < 255u ? PJ_PCS_HOT * j->hot_after / PJ_HOT_AFTER : 255u;
    pj_pcs_sh = j->shared_end;
    if (w - r > PJ_PCS_RING) r = w - PJ_PCS_RING;
    for (; ok && r != w; ++r) {
        const uint32_t pcoff = pj_pcs_ring[r & (PJ_PCS_RING - 1u)] - (uint32_t)(uintptr_t)j->code;
        uint32_t off, *hd, v, ep;
        if (pcoff < j->shared_end || pcoff >= j->cap) continue;
        off = pj_pcs_block(j, pcoff);
        if (!off) continue;
        hd = pj_hdr(j, --off);
        if (hd[1] & 0xC0000000u) continue;           /* dead / redirected, or PM16 */
        /* r73b: the granule walk can reach stale headers left in the code
         * buffer by an earlier generation; only a block the page lists know
         * is real. Writing hd[7] at a wrong offset corrupted native code. */
        if (pj_find_block(j, hd[0], hd[4], 0u) != off + PJ_HDR_BYTES + 1u) continue;
        ep = (j->pcs_n++ >> 11) & 15u;
        v = hd[7];
        if ((v >> 28) != ep) v = (v & 0xFFFFFu) | (ep << 28);
        v += 1u << 20;
        hd[7] = v;
        if (pj_pcs_promote_on && ((v >> 20) & 0xFFu) >= pcs_thr) {
            pj_pcs_r = r + 1u;
            pj_pcs_promote(c, j, off);
        }
    }
    pj_pcs_r = w;
}
/* Run one native block at EIP if possible. Returns retired instructions (0 = none). */
static B86_PM_HOT unsigned b86_pj_try(B86Cpu *c)
{
    B86Pj *j = b86_pj_state(c);
    uint32_t h, ent;
    unsigned n;
    /* V29: one shared mapping epoch for both PM16 and PM32 native blocks.
     * Only an actual PG/CR3/TLB epoch change incurs a cold reconciliation. */
    if (j && j->v29_map_observed && !j->v27_rm_pending &&
        (j->v29_seen_pg != (uint32_t)c->pg ||
         (c->pg && j->v29_seen_cr3 != c->cr3) ||
         j->v29_seen_tlb_flush != b86_pg_stat_flush)) {
        j->v27_rm_pending = 1u;
        ++j->v29_map_changes;
    }
    /* A PG-off admission cannot use PG-on cached code, even before the
     * first mapping epoch was sampled. Arm a fence if needed. */
    if (j && !c->pg && j->v28_paged_seen && !j->v30c_parked)
        j->v27_rm_pending = 1u;
    if (j && j->v27_rm_pending) b86_pj_v27_reconcile(c, j);
    /* V30C: never dispatch even a direct native tag while paging is off
     * and paged code is parked. C/interpreter executes this PM16 interval;
     * PG-on reentry above validates dormant code before enabling PJ. */
    if (j && j->v30c_parked && !c->pg) {
        ++j->v30c_deopts;
        return 0u;
    }
    if (B86_PJ_PCS && j && j->hot && pj_pcs_r != pj_pcs_w) pj_pcs_drain(c, j);   /* r72f */
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
                /* r71: switch to (or create) this segment's block table */
                unsigned k, hit = PJ_PM16_CTX;
                for (k = 0u; k < PJ_PM16_CTX; ++k) {
                    const uint32_t *d = pj_pm16_desc(j, k);
                    if (d[3] && d[0] == cs->base && d[1] == cs->limit && d[2] == kind) { hit = k; break; }
                }
                if (hit == PJ_PM16_CTX) {
                    uint32_t *d;
                    hit = j->pm16_rr++ % PJ_PM16_CTX;
                    d = pj_pm16_desc(j, hit);
                    d[0] = cs->base; d[1] = cs->limit; d[2] = kind; d[3] = 1u;
                    memset(pj_pm16_tag(j, hit), 0, 2u * PJ_PM16_ENT * sizeof(uint32_t));
                    ++j->flush_pm16;                          /* r71: counts context (re)allocations */
                }
                j->pm16_cur = hit;
                j->es_mode = 0u;
            }
            j->pm16_active = 1u;
            j->pm16_base = cs->base;
            j->pm16_limit = cs->limit;
            j->pm16_kind = kind;
        } else if (j->pm16_active) {                    /* r71: flat blocks stay valid */
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
    c->pj_stop = 0u;                                  /* r71: kills since the last run are done */
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
    {
#if BB_PJ_PM16_REG_TIER
    uint32_t *const tagt = j->pm16_active ? pj_pm16_tag(j, j->pm16_cur) : j->tag;
    uint32_t *const entt = j->pm16_active ? pj_pm16_ent(j, j->pm16_cur) : j->ent;
    h = j->pm16_active ? ((c->ip * 2654435761u) >> 16) & (PJ_PM16_ENT - 1u)
                       : ((c->ip * 2654435761u) >> 16) & (B86_PJ_HASH - 1u);
#else
    uint32_t *const tagt = j->tag, *const entt = j->ent;
    h = ((c->ip * 2654435761u) >> 16) & (B86_PJ_HASH - 1u);
#endif
#if B86_PJ_V45_DYN2
    /* Flat PM32 C and emitted native dynamic exits must resolve the same
     * two slots. Never use this for PM16: it owns separate context tables.
     * Epoch reconciliation still zeros both ways before any native reuse. */
    if (!j->pm16_active && tagt[h] != (c->ip | 1u) &&
        tagt[h ^ 1u] == (c->ip | 1u)) h ^= 1u;
#endif
    if (tagt[h] != (c->ip | 1u)) {
#if B86_PJ_V45_DYN2
        /* Prefer an empty neighbor rather than evicting a live translation.
         * A nonempty pair retains the original primary-victim policy.
         * The second way has no permission to bypass lazy v31 validation. */
        if (!j->pm16_active && tagt[h] && !tagt[h ^ 1u]) h ^= 1u;
#endif
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

        {   /* r71: reuse a live copy if one exists; otherwise translate (a
             * capacity flush inside translate clears every table first) */
#if BB_PJ_PM16_REG_TIER
            const uint32_t pm16 = j->pm16_active ? 1u : 0u;
            const uint32_t lin = (pm16 ? j->pm16_base : 0u) + c->ip;
#else
            const uint32_t pm16 = 0u, lin = c->ip;
#endif
            uint32_t e =
#if B86_PJ_V31_LAZY_EPOCH
                pj_v31_find(c, j, lin, c->ip, pm16);
#else
                pj_find_block(j, lin, c->ip, pm16);
#endif
            if (e) ++j->refinds;
            else {
                /* Never cache a negative entry for a temporary defer. */
                if (pj_v30f_defer(c, j, c->ip)) return 0u;
                e = b86_pj_translate(c, c->ip);
            }
            tagt[h] = c->ip | 1u;
            entt[h] = e;
        }
        if (!entt[h]) ++j->declines;
/* BB_PJ_PM16_NEGCACHE_V1: record only a failed PM16 translation.
         * It is safe to remember this across code and descriptor changes:
         * later hits always interpret instead of executing old native code. */
        if (!entt[h] && j->pm16_active) {
            const uint32_t pm16_lin = j->pm16_base + c->ip;
            uint32_t *const pm16_neg =
                (uint32_t *)(void *)(j->code + j->cap);
            pm16_neg[(pm16_lin * 2654435761u) >> 23] = pm16_lin + 1u;
            ++j->pm16_neg_inserts;
        }

    }
    ent = entt[h];
    }
    if (!ent) return 0u;
    /* r71: BB_PJ_FETCH_PAGE_V1. Real 386 code fetch walks the code page:
     * it faults when DOS/4GW has paged it out and sets the Accessed bit the
     * VMM uses to pick eviction victims. Translated code never re-reads its
     * bytes, so check the entry page here (a TLB hit almost always): a
     * missing page goes to the interpreter, which raises the #PF and lets
     * the VMM page it back in; a present page gets its A bit as on hardware. */
    if (c->pg) {
        uint32_t ph;
#if BB_PJ_PM16_REG_TIER
        const uint32_t lip = (j->pm16_active ? j->pm16_base : 0u) + c->ip;
#else
        const uint32_t lip = c->ip;
#endif
        /* V30I: a valid (already accessed) TLB page needs no second walk.
         * On miss use the unchanged walker, including all #PF/A-bit effects. */
        if (pj_v30i_entry_tlb_hit(c, lip)) {
            ++j->v30i_tlb_hits;
        } else {
            ++j->v30i_tlb_walks;
            if (!b86_pg_walk(c, lip, &ph)) { ++j->fetch_refusals; return 0u; }
        }
    }
#ifdef B86_PJ_VERIFY
    uint32_t pr[8], pip = c->ip, pfl = c->flags, plz[7];
    uint64_t pic = c->icount;
    /* r71: blocks may load DS/ES; the replay must start from the same
     * segment state (this produced a false mismatch at 00202A53). */
    uint32_t pseg[4]; uint16_t pseg386[2]; B86PM1Cache ppm[6]; const uint32_t pcr2 = c->cr2;
    memcpy(pseg, c->seg, sizeof pseg); memcpy(pseg386, c->seg386, sizeof pseg386); memcpy(ppm, c->pm386.seg, sizeof ppm);
    memcpy(pr, c->r, sizeof pr);
    plz[0] = c->lz_kind; plz[1] = c->lz_a; plz[2] = c->lz_res; plz[3] = c->lz_b; plz[4] = c->lz_ikind; plz[5] = c->lz_ia; plz[6] = c->lz_ires;
    pj_nvl = 0u;
#endif
    if (j->hot && !pj_in_hot(j, ent - 1u)
#if BB_PJ_PM16_REG_TIER
        && !j->pm16_active
#endif
        ) {                                          /* r72: count dispatches; promote hot blocks to SRAM */
        const uint32_t off = ent - 1u - PJ_HDR_BYTES;
        uint32_t *hd = pj_hdr(j, off);
        /* r72d: every 64K dispatches let the threshold sink back while the
         * tier has room (resets during loading must not freeze it high) */
        if (!(++j->hot_tick & 0xFFFFu) && j->hot_after > PJ_HOT_AFTER &&
            j->hot_used - j->hot_start < (j->hot_cap - (j->hot_start - j->hot_base)) / 2u) j->hot_after /= 2u;
        if (!(hd[1] & 0x80000000u) && (++hd[7] & 0xFFFFFu) >= j->hot_after) {
            hd[7] = 0u;                              /* r72d: a refused promotion backs off */
            const uint32_t e2 = pj_hot_translate(c, j, c->ip);
            if (e2 && !(hd[1] & 0x80000000u) && pj_hdr(j, off)[4] == c->ip && pj_redirect(j, off, e2)) {   /* r73k: may refuse */
                j->tag[h] = c->ip | 1u; j->ent[h] = e2; ent = e2;
                ++j->promotions;
            }
        }
    }
    c->pj_exit = 0u;
    {
        const unsigned hot = pj_in_hot(j, ent - 1u) != 0;
#if B86_PJ_V30K_XIP_SPLIT
        uint32_t a0 = 0u, h0 = 0u, t0 = 0u;
        if (j->xacc) {                               /* r72e: XIP split */
            a0 = *j->xacc; h0 = *j->xhit; t0 = *j->xus;
            if (j->xlt) {
                if (a0 >= j->xla && h0 >= j->xlh && a0 - j->xla >= h0 - j->xlh) {
                    j->xs[8]++; j->xs[9] += (a0 - j->xla) - (h0 - j->xlh); j->xs[10] += t0 - j->xlt;
                } else j->xs[11]++;
            }
        }
#endif /* B86_PJ_V30K_XIP_SPLIT: pre-entry hardware reads */
        n = ((unsigned (*)(B86Cpu *, uintptr_t))(void *)(uintptr_t)((uintptr_t)(hot ? j->hsh.entry : j->entry) | 1u))(c, (uintptr_t)(j->code + ent - 1u) | 1u);
#if B86_PJ_V30K_XIP_SPLIT
        if (j->xacc) {
            const uint32_t a1 = *j->xacc, h1 = *j->xhit, t1 = *j->xus;
            if (a1 >= a0 && h1 >= h0 && a1 - a0 >= h1 - h0) {
                unsigned long *x = j->xs + (hot ? 0u : 4u);
                x[0]++; x[1] += n; x[2] += (a1 - a0) - (h1 - h0); x[3] += t1 - t0;
            } else j->xs[11]++;
            j->xla = a1; j->xlh = h1; j->xlt = t1 ? t1 : 1u;
        }
#endif /* B86_PJ_V30K_XIP_SPLIT: post-entry hardware reads */
    }
#ifdef B86_PJ_VERIFY
    if (n) {
        uint32_t nr[8], nip = c->ip, nfl;
        static uint8_t nw[1024][4];
        unsigned k, bad = 0u;
        b86_flags_materialize(c); nfl = c->flags;
        memcpy(nr, c->r, sizeof nr);
        for (k = 0; k < pj_nvl; ++k) memcpy(nw[k], pj_vl[k].p, pj_vl[k].n);
        for (k = pj_nvl; k-- > 0u;) memcpy(pj_vl[k].p, pj_vl[k].old, pj_vl[k].n);
        {   uint32_t nseg[4]; uint16_t nseg386[2]; B86PM1Cache npm[6];
            memcpy(nseg, c->seg, sizeof nseg); memcpy(nseg386, c->seg386, sizeof nseg386); memcpy(npm, c->pm386.seg, sizeof npm);
            memcpy(c->seg, pseg, sizeof pseg); memcpy(c->seg386, pseg386, sizeof pseg386); memcpy(c->pm386.seg, ppm, sizeof ppm);
            memcpy(c->r, pr, sizeof pr); c->ip = pip; c->flags = pfl;
            c->lz_kind = plz[0]; c->lz_a = plz[1]; c->lz_res = plz[2]; c->lz_b = plz[3]; c->lz_ikind = plz[4]; c->lz_ia = plz[5]; c->lz_ires = plz[6];
            c->icount = pic; c->pj = NULL;
            /* r71: replay until n instructions RETIRED; REP chunks take
             * several interpreter steps per instruction */
            for (k = 0; c->icount - pic < n && k < 64u * n + 4096u; ++k) if (b86_step(c) != B86_OK) { bad |= 32u; break; }
            c->pj = j;
            if (memcmp(nseg, c->seg, sizeof nseg) || memcmp(nseg386, c->seg386, sizeof nseg386)) bad |= 16u;
        }
        if (0) {
        memcpy(c->r, pr, sizeof pr); c->ip = pip; c->flags = pfl;
        c->lz_kind = plz[0]; c->lz_a = plz[1]; c->lz_res = plz[2]; c->lz_b = plz[3]; c->lz_ikind = plz[4]; c->lz_ia = plz[5]; c->lz_ires = plz[6];
        c->icount = pic;
        c->pj = NULL;
        for (k = 0; k < n; ++k) if (b86_step(c) != B86_OK) { bad |= 32u; break; }
        c->pj = j;
        }
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
        if (bad && c->pg && c->cr2 != pcr2 && pj_cps_has(j, c->cr2 >> 12)) {
            /* r71: the replay took #PF fetching from a translated code page
             * that DOS/4GW had paged out; chained native code does not fetch */
            static unsigned long fetchpf; ++fetchpf; bad = 0u;
            if (fetchpf <= 3u) fprintf(stderr, "[pj-verify] replay fetch #PF at %08X (code page paged out); not a mismatch\n", c->cr2);
        }
        if (bad && ++b86_pj_vfy_bad <= 30u) {
            fprintf(stderr, "[pj-verify] MISMATCH%s%s%s%s%s block %08X n=%u\n   native:", (bad & 1u) ? " regs" : "", (bad & 2u) ? " eip" : "",
                    (bad & 4u) ? " flags" : "", (bad & 8u) ? " memory" : "", (bad & 32u) ? " interp-stop" : (bad & 16u) ? " segments" : "", pip, n);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", nr[k]);
            fprintf(stderr, " eip=%08X fl=%08X\n   interp:", nip, nfl);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", c->r[k]);
            fprintf(stderr, " eip=%08X fl=%08X\n   pre   :", c->ip, c->flags);
            for (k = 0; k < 8; ++k) fprintf(stderr, " %08X", pr[k]);
            fprintf(stderr, " fl=%08X\n", pfl);
            {   /* r71: the code that ran, from the block start */
                uint8_t cb[48]; unsigned q, nb = pj_fetch(c, pip, cb, sizeof cb);
                fprintf(stderr, "   code  :");
                for (q = 0; q < nb; ++q) fprintf(stderr, " %02X", cb[q]);
                fprintf(stderr, "\n");
            }
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
    if (!c || !state || state_bytes < sizeof(B86Pj) || !code || code_bytes < 16384u + PJ_TAIL_BYTES || (((uintptr_t)code) & 3u)) return 0;
    memset(j, 0, sizeof *j);
    j->code = (uint8_t *)code; j->cap = (uint32_t)(code_bytes - PJ_TAIL_BYTES);
    memset(j->code + j->cap, 0, PJ_TAIL_BYTES); /* negative table, code-page set, block lists: outside emitter bounds */
    {   /* 8-byte aligned candidate table in additional PSRAM tail. */
        const uintptr_t base = (uintptr_t)(j->code + j->cap + PJ_TAIL_V30F_BASE);
        b86_v30f_slot *slots = (b86_v30f_slot *)(void *)((base + 7u) & ~(uintptr_t)7u);
        if (!b86_v30f_init(&v30f_admit, slots, PJ_TAIL_V30F_SLOTS,
                           (b86_v30f_mode)B86_V30F_MODE)) return 0;
    }
    for (j->gshift = 6u; (j->cap >> j->gshift) >= PJ_GIDX_N; ++j->gshift) {}
    c->pj = j;
    pj_emit_shared(j);
    pj_pcs_lo = (uint32_t)(uintptr_t)j->code; pj_pcs_hi = pj_pcs_lo + j->cap; pj_pcs_sh = j->shared_end;
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
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    /* The serial logger only sees changes; do not spam on every stats read.
     * rm-episodes counts coalesced RM phases, barriers counts real flushes. */
    if (j->v27_reported != j->v27_barriers) {
        B86Pj *mut = (B86Pj *)(void *)j;
        printf("[bb386-v27-coherence] rm-episodes=%lu barriers=%lu pending=%lu\n",
               j->v27_rm_episodes,j->v27_barriers,
               (unsigned long)j->v27_rm_pending);
        mut->v27_reported = j->v27_barriers;
    }
#endif
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    if (j->v28_reported_checks != j->v28_checks) {
        B86Pj *mut = (B86Pj *)(void *)j;
        printf("[bb386-v28-exact] checked=%lu killed=%lu kept=%lu paged-fallback=%lu\n",
               j->v28_checks, j->v28_kills, j->v28_keeps, j->v28_fallbacks);
        mut->v28_reported_checks = j->v28_checks;
    }
    if (j->v29_reported_checks != j->v29_paged_checks ||
        j->v29_reported_changes != j->v29_map_changes) {
        B86Pj *mut = (B86Pj *)(void *)j;
        printf("[bb386-v29-physical] mapped-checks=%lu mapped-kills=%lu map-changes=%lu fallback=%lu\n",
               j->v29_paged_checks, j->v29_paged_kills,
               j->v29_map_changes, j->v28_fallbacks);
        mut->v29_reported_checks = j->v29_paged_checks;
        mut->v29_reported_changes = j->v29_map_changes;
    }
#if B86_PJ_V30J_LINK_FILTER
    printf("[bb386-v30j-linkfilter] mode=1 marks=%lu skipped=%lu exact-scans=%lu overlap-refusals=%lu\n",
           pj_v30j_marks, pj_v30j_skips, pj_v30j_scans, pj_redir_overlap);
#else
    printf("[bb386-v30j-linkfilter] mode=0 (original exact scan) overlap-refusals=%lu\n",
           pj_redir_overlap);
#endif
#if B86_PJ_V30G_FAST_WALK
    printf("[bb386-v30l-exactcmp] mode=%u exact bytes preserved; no journal bypass\n", (unsigned)B86_PJ_V30L_EXACT_COMPARE);
#endif
#if B86_PJ_V30M_OBSERVE
    printf("[bb386-v30m-pagejournal] mode=1 pages=%lu/%lu rm-stores=%lu pm-stores=%lu pte-writes=%lu unknown=%lu wraps=%lu exact-bypass=0\n",
           b86_v30m_unique, (unsigned long)B86_V30M_NPAGES,
           b86_v30m_rm_stores, b86_v30m_pm_stores,
           b86_v30m_pte_writes, b86_v30m_unknown,
           b86_v30m_generation_wraps);
#else
    printf("[bb386-v30m-pagejournal] mode=0 instrumentation disabled; exact-bypass=0\n");
#endif
#if B86_PJ_V31_LAZY_EPOCH
#if B86_PJ_V33_PHYSDIRTY
    {
        unsigned long pages = 0u;
        uint32_t v33p;
        for (v33p = 0u; v33p < B86_V33_NPAGES; ++v33p)
            pages += b86_v33_dirty[v33p] != 0u;
        printf("[bb386-v33-physdirty] mode=1 pages=%lu/%lu interp-slow=%lu pte=%lu unknown=%lu native-fast=marked exact-bypass=0 host-covered=0 authoritative=0\n",
               pages, (unsigned long)B86_V33_NPAGES, b86_v33_cpu,
               b86_v33_pte, b86_v33_unknown);
        printf("[bb386-v34-host] mode=1 calls=%lu exact-bypass=0 host-covered=0 authoritative=0\n", b86_v34_host_calls);
#if B86_PJ_V37_EPOCH_LEDGER
        {
            unsigned long current_pages = 0u;
            uint32_t k;
            for (k = 0u; k < B86_V33_NPAGES; ++k)
                current_pages += (b86_v37_page_epoch[k] == b86_v37_token);
            printf("[bb386-v37-epoch] mode=1 windows=%lu last=%lu current=%lu peak=%lu pages-sum=%lu empty=%lu unknown-windows=%lu wraps=%lu token=%u exact-bypass=0 authoritative=0\n",
                b86_v37_windows, b86_v37_last_pages, current_pages,
                b86_v37_pages_peak, b86_v37_pages_sum,
                b86_v37_empty_windows, b86_v37_unsafe_windows,
                b86_v37_wraps, (unsigned)b86_v37_token);
#if B86_PJ_V38_SPOT_ORACLE
            printf("[bb386-v38-oracle] mode=1 windows=%lu checked-pages=%lu changed=%lu covered=%lu missed=%lu missed-windows=%lu alias-skips=%lu incomplete=%lu exact-bypass=0 authoritative=0\n",
                   b86_v38_windows, b86_v38_compared_pages,
                   b86_v38_changed, b86_v38_covered, b86_v38_missed,
                   b86_v38_missed_windows, b86_v38_alias_skips,
                   b86_v38_incomplete_pages);
#endif
#if B86_PJ_V39_HASH_ORACLE
            printf("[bb386-v39-fullhash] mode=1 windows=%lu checked-pages=%lu changed=%lu covered=%lu missed=%lu missed-windows=%lu alias-skips=%lu incomplete=%lu first-missed-page=%lu first-missed-token=%u exact-bypass=0 authoritative=0\n",
                b86_v39_windows, b86_v39_compared, b86_v39_changed,
                b86_v39_covered, b86_v39_missed, b86_v39_missed_windows,
                b86_v39_alias, b86_v39_incomplete,
                (unsigned long)b86_v39_first_missed_page,
                b86_v39_first_missed_token);
#if B86_PJ_V40_MISSLOC
            /* Emit each missed-write event only once, outside the hot path. */
            if (b86_v40_total - b86_v40_printed > B86_V40_EVENT_CAP) {
                b86_v40_lost += b86_v40_total - b86_v40_printed - B86_V40_EVENT_CAP;
                b86_v40_printed = b86_v40_total - B86_V40_EVENT_CAP;
            }
            while (b86_v40_printed < b86_v40_total) {
                const B86V40MissEvent *e = &b86_v40_events[b86_v40_printed % B86_V40_EVENT_CAP];
                unsigned first = 0u;
                while (first < B86_V40_CHUNKS_PER_PAGE && !(e->chunk_mask & (1u << first)))
                    ++first;
                printf("[bb386-v40-missloc] event=%lu window=%lu page=%lu phys=%08lX closing-token=%u observed-stamp=%u first-chunk=%u first-offset=%u mask=%04X closing-kind=%u\n",
                    b86_v40_printed + 1u, (unsigned long)e->window,
                    (unsigned long)e->page, (unsigned long)(e->page << 12u),
                    (unsigned)e->closing_token, (unsigned)e->observed_stamp,
                    first, first * 256u, (unsigned)e->chunk_mask, (unsigned)e->closing_kind);
                ++b86_v40_printed;
            }
            printf("[bb386-v40-missloc] mode=1 events=%lu lost=%lu exact-bypass=0 authoritative=0\n",
                b86_v40_total, b86_v40_lost);
            printf("[bb386-v42-mode] mode=1 rm-windows=%lu pm-windows=%lu unknown-windows=%lu rm-checked=%lu pm-checked=%lu unknown-checked=%lu rm-changed=%lu pm-changed=%lu unknown-changed=%lu rm-missed=%lu pm-missed=%lu unknown-missed=%lu exact-bypass=0 authoritative=0\n",
                b86_v42_rm_windows, b86_v42_pm_windows, b86_v42_other_windows,
                b86_v42_rm_checked, b86_v42_pm_checked, b86_v42_other_checked,
                b86_v42_rm_changed, b86_v42_pm_changed, b86_v42_other_changed,
                b86_v42_rm_missed, b86_v42_pm_missed, b86_v42_other_missed);
            printf("[bb386-v41-phase] mode=1 closing-before-token=1 baseline-after-token=1 exact-bypass=0 authoritative=0\n");
#endif
#endif
        }
#endif
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
        {
            uint32_t md_bulk[4] = {0u, 0u, 0u, 0u};
            extern void md_v35_bulk_stats(uint32_t out[4]);
            md_v35_bulk_stats(md_bulk);
            printf("[bb386-v35-md-bulk] mode=1 direct=%lu wrapped=%lu notify=%lu bytes=%lu exact-bypass=0 authoritative=0\n",
                   (unsigned long)md_bulk[0], (unsigned long)md_bulk[1],
                   (unsigned long)md_bulk[2], (unsigned long)md_bulk[3]);
            /* BB_V36_MD_SCALAR_DIRTY_V1: counts canonical microDOS scalar guest stores. */
            {
                uint32_t md_scalar[4] = {0u, 0u, 0u, 0u};
                extern void md_v36_scalar_stats(uint32_t out[4]);
                md_v36_scalar_stats(md_scalar);
                printf("[bb386-v36-md-scalar] mode=1 byte=%lu word=%lu wrapped=%lu notify=%lu exact-bypass=0 authoritative=0\n",
                       (unsigned long)md_scalar[0], (unsigned long)md_scalar[1],
                       (unsigned long)md_scalar[2], (unsigned long)md_scalar[3]);
            }
        }
#endif
    }
#else
    printf("[bb386-v33-physdirty] mode=0 exact-bypass=0 authoritative=0\n");
#endif
    printf("[bb386-v31-lazy] mode=1 epochs=%lu unlinked=%lu exact-on-use=%lu memo-hits=%lu killed=%lu fallbacks=%lu active=%u\n",
           pj_v31_epochs, pj_v31_unlinked, pj_v31_exact,
           pj_v31_memo_hits, pj_v31_kills, pj_v31_fallbacks,
           (unsigned)pj_v31_active);
#else
    printf("[bb386-v31-lazy] mode=0 exact full-scan control\n");
#endif
    printf("[bb386-v30i-entrytlb] mode=%u hits=%lu walks=%lu refusals=%lu\n",
           (unsigned)B86_PJ_V30I_ENTRY_TLB,
           j->v30i_tlb_hits, j->v30i_tlb_walks, j->fetch_refusals);
    if (j->v30_reported_checks != j->v30_pg0_checks) {
        B86Pj *mut = (B86Pj *)(void *)j;
        printf("[bb386-v30-selective] nonpaged-checks=%lu nonpaged-kills=%lu nonpaged-kept=%lu full-fallback=%lu\n",
               j->v30_pg0_checks, j->v30_pg0_kills,
               j->v30_pg0_keeps, j->v28_fallbacks);
        mut->v30_reported_checks = j->v30_pg0_checks;
    }
    if (j->v30c_reported_parks != j->v30c_parks ||
        j->v30c_reported_resumes != j->v30c_resumes) {
        B86Pj *mut = (B86Pj *)(void *)j;
        printf("[bb386-v30c-park] parks=%lu resumes=%lu pm-pg0-deopts=%lu parked=%lu full-fallback=%lu\n",
               j->v30c_parks, j->v30c_resumes, j->v30c_deopts,
               (unsigned long)j->v30c_parked, j->v28_fallbacks);
        mut->v30c_reported_parks = j->v30c_parks;
        mut->v30c_reported_resumes = j->v30c_resumes;
    }
/* V30O telemetry prints on existing manual statistics requests only. */
#if B86_PJ_V30O_DOORKEEPER
    printf("[bb386-v30o-doorkeeper] mode=1 arm=%u epoch=%lu reset=%lu probe=%lu repeat=%lu first=%lu admits=%lu defers=%lu bypass=%lu escapes=%lu suspended=%lu\n",
           (unsigned)PJ_V30O_ARM_EVICTIONS,
           (unsigned long)pj_v30o_epoch, pj_v30o_resets,
           (unsigned long)pj_v30o_probe, (unsigned long)pj_v30o_repeat,
           pj_v30o_first, pj_v30o_hits, pj_v30o_defers,
           pj_v30o_bypassed, pj_v30o_escapes,
           (unsigned long)pj_v30o_suspended);
#else
    printf("[bb386-v30o-doorkeeper] mode=0 original V30F admission\n");
#endif
/* BB_V30F_ADAPTIVE_INTEGRATION_V1: read-only cold telemetry. */
    printf("[bb386-v30f-admit] mode=%u hits=%u pressure=%u up=%lu down=%lu first=%lu revisit=%lu replaced=%lu stale=%lu denied=%lu permit=%lu prearm=%lu evictions=%lu blocks=%lu last-bpe=%lu\n",
           (unsigned)v30f_admit.mode,
           (unsigned)b86_v30f_required_hits(&v30f_admit),
           (unsigned)v30f_admit.pressure,
           (unsigned long)v30f_admit.pressure_up, (unsigned long)v30f_admit.pressure_down,
           (unsigned long)v30f_admit.first, (unsigned long)v30f_admit.revisits,
           (unsigned long)v30f_admit.replaced, (unsigned long)v30f_admit.stale,
           (unsigned long)v30f_admit.denied, (unsigned long)v30f_admit.permitted,
           (unsigned long)v30f_admit.prearm_permits,
           (unsigned long)j->evictions, (unsigned long)j->blocks,
           (unsigned long)v30f_admit.last_blocks_per_evict);
    if (v30f_admit.mode == B86_V30F_GUARDED) {
        printf("[bb386-v30f-guard] bypass=%u entries=%lu exits=%lu direct=%lu badwindows=%u window=%lu first=%lu replaced=%lu revisit=%lu permit=%lu resume-evict=%lu\n",
               (unsigned)v30f_admit.guard_suspended,
               (unsigned long)v30f_admit.guard_entries,
               (unsigned long)v30f_admit.guard_exits,
               (unsigned long)v30f_admit.guard_bypassed,
               (unsigned)v30f_admit.guard_bad_windows,
               (unsigned long)v30f_admit.guard_window_total,
               (unsigned long)v30f_admit.guard_window_first,
               (unsigned long)v30f_admit.guard_window_replaced,
               (unsigned long)v30f_admit.guard_window_revisits,
               (unsigned long)v30f_admit.guard_window_permitted,
               (unsigned long)v30f_admit.guard_resume_eviction);
    }
#if BB_V30D_ADMISSION
    {   /* Reporting is read-only; no new per-block or per-iteration checks. */
        printf("[bb386-v30d-admit] enabled=1 deferred=%lu admitted=%lu collisions=%lu evictions=%lu blocks=%lu\n",
               v30d_defers, v30d_admits, v30d_collisions, j->evictions, j->blocks);
    }
#else
    printf("[bb386-v30d-admit] enabled=0 evictions=%lu blocks=%lu\n", j->evictions, j->blocks);
#endif
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
    printf("[bb386-pm16-native] v9-movext16=%lu v9-movext32=%lu v10-setcc=%lu\n",
           (unsigned long)bb_pm16_diag.v9_movext16,
           (unsigned long)bb_pm16_diag.v9_movext32,
           (unsigned long)bb_pm16_diag.v10_setcc);
    printf("[bb386-v14-pm16-stack32] push-native=%lu pop-native=%lu access-attempt=%lu access-ok=%lu\n",
           (unsigned long)bb_pm16_diag.v14_push32_native,
           (unsigned long)bb_pm16_diag.v14_pop32_native,
           (unsigned long)bb_pm16_diag.v14_stack_attempt,
           (unsigned long)bb_pm16_diag.v14_stack_success);
    printf("[bb386-v143-stack-unaligned] admitted=%lu\n",
           (unsigned long)bb_pm16_diag.v143_unaligned_admitted);
#if BB_PJ_PM16_TRUTHDIAG
    printf("[bb386-v17-pm16-gs-stack] push-native=%lu pop-native=%lu stack-attempt=%lu stack-ok=%lu seg-attempt=%lu seg-ok=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v17_push_gs_native,
           (unsigned long)bb_pm16_diag.v17_pop_gs_native,
           (unsigned long)bb_pm16_diag.v17_stack_attempt,
           (unsigned long)bb_pm16_diag.v17_stack_ok,
           (unsigned long)bb_pm16_diag.v17_seg_attempt,
           (unsigned long)bb_pm16_diag.v17_seg_ok,
           (unsigned)BB_PJ_PM16_GS_STACK_V17);
    printf("[bb386-v174-gs-mode] native-push=%u native-pop=%u (both enabled requires combined Pico validation)\n",
           (unsigned)BB_PJ_V171_PUSH, (unsigned)BB_PJ_V171_POP);
    printf("[bb386-v18-pm16-btimm] native-reg=%lu runtime=%lu memory-refused=%lu modifying-refused=%lu other-refused=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v18_bt_native,
           (unsigned long)bb_pm16_diag.v18_bt_runtime,
           (unsigned long)bb_pm16_diag.v18_bt_mem_refused,
           (unsigned long)bb_pm16_diag.v18_bt_modify_refused,
           (unsigned long)bb_pm16_diag.v18_bt_other_refused,
           (unsigned)BB_PJ_PM16_BT_IMM_V18);
    printf("[bb386-v182-pm16-bt32] native-reg=%lu runtime=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v182_bt32_native,
           (unsigned long)bb_pm16_diag.v182_bt32_runtime,
           (unsigned)BB_PJ_PM16_BT32_IMM_V182);
#if BB_PJ_V184_STOP_CLASSIFY
    {
        static const unsigned char code[5] = { 0x00,0x01,0x02,0x20,0x22 };
        unsigned k;
        for (k=0u; k<5u; ++k)
            printf("[bb386-v184-0fstop] op=%02X total=%lu pfx66=%lu pfx67=%lu pfxseg=%lu pfxlockrep=%lu pfxmulti=%lu reg=%lu mem=%lu truncated=%lu\n",
                   (unsigned)code[k], (unsigned long)bb_pm16_diag.v184_total[k],
                   (unsigned long)bb_pm16_diag.v184_pfx66[k],
                   (unsigned long)bb_pm16_diag.v184_pfx67[k],
                   (unsigned long)bb_pm16_diag.v184_pfxseg[k],
                   (unsigned long)bb_pm16_diag.v184_pfxlockrep[k],
                   (unsigned long)bb_pm16_diag.v184_pfxmulti[k],
                   (unsigned long)bb_pm16_diag.v184_regop[k],
                   (unsigned long)bb_pm16_diag.v184_memop[k],
                   (unsigned long)bb_pm16_diag.v184_trunc[k]);
        for (k=0u; k<8u; ++k)
            printf("[bb386-v184-modrm] ext=%u op00=%lu op01=%lu cr20=%lu cr22=%lu\n",
                   k, (unsigned long)bb_pm16_diag.v184_group00[k],
                   (unsigned long)bb_pm16_diag.v184_group01[k],
                   (unsigned long)bb_pm16_diag.v184_cr20[k],
                   (unsigned long)bb_pm16_diag.v184_cr22[k]);
    }
#endif /* BB_PJ_V184_STOP_CLASSIFY */
    printf("[bb386-v22-consolidation] shared-ea16=3 consumers shared-regmov=pm16+flat32 source-contract=1\n");
    printf("[bb386-v23-unified-head] flat32=prefix4 pm16=single-byte policies=2 source-contract=1\n");
    printf("[bb386-v24-unified-modrm] shared=flat32+pm16 ea16-consumers=4 source-contract=1\n");
#if BB_PJ_V25_FASTWRITE && !defined(B86_PJ_VERIFY)
#if B86_PJ_V30N_LEAN_FASTWRITE
    printf("[bb386-v25-fastwrite] enabled=1 native-hits=unavailable gate=tlb+cpg+rm-codemap\n");
#else
    printf("[bb386-v25-fastwrite] enabled=1 native-hits=%lu gate=tlb+cpg+rm-codemap\n",
           (unsigned long)pj_v25_fastwrite_hits);
#endif
#else
    printf("[bb386-v25-fastwrite] enabled=0 native-hits=0 gate=control\n");
#endif
    printf("[bb386-v30n-lean] fastwrite-counter=%u pagejournal=%u exactcmp=%u\n",
           (unsigned)B86_PJ_V30N_LEAN_FASTWRITE,
           (unsigned)B86_PJ_V30M_OBSERVE,
           (unsigned)B86_PJ_V30L_EXACT_COMPARE);
    printf("[bb386-v21-unified-pm16-read] native-translations=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v21_pm16_read_native,
           (unsigned)BB_PJ_V21_PM16_INLINE_READ);
    printf("[bb386-v20-pm16-mov32mem] read-native=%lu write-native=%lu read-runtime=%lu write-runtime=%lu refused=%lu unaligned=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v20_mov32_read_native,
           (unsigned long)bb_pm16_diag.v20_mov32_write_native,
           (unsigned long)bb_pm16_diag.v20_mov32_read_runtime,
           (unsigned long)bb_pm16_diag.v20_mov32_write_runtime,
           (unsigned long)bb_pm16_diag.v20_mov32_refused,
           (unsigned long)bb_pm16_diag.v20_mov32_unaligned,
           (unsigned)BB_PJ_V20_MOV32_MEM);
    printf("[bb386-v19-0f-read-batch] sldt-native=%lu str-native=%lu cr-native=%lu sldt-runtime=%lu str-runtime=%lu cr-runtime=%lu cr-priv-refused=%lu selectors-enabled=%u cr-enabled=%u\n",
           (unsigned long)bb_pm16_diag.v19_sldt_native,
           (unsigned long)bb_pm16_diag.v19_str_native,
           (unsigned long)bb_pm16_diag.v19_crread_native,
           (unsigned long)bb_pm16_diag.v19_sldt_runtime,
           (unsigned long)bb_pm16_diag.v19_str_runtime,
           (unsigned long)bb_pm16_diag.v19_crread_runtime,
           (unsigned long)bb_pm16_diag.v19_cr_priv_refused,
           (unsigned)BB_PJ_V19_READ_SELECTORS,
           (unsigned)BB_PJ_V19_READ_CR);
    printf("[bb386-v185-pm16-smsw] native-reg=%lu runtime=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v185_smsw_native,
           (unsigned long)bb_pm16_diag.v185_smsw_runtime,
           (unsigned)BB_PJ_PM16_SMSW_REG_V185);
    printf("[bb386-v183-pm16-bitmodify32] bts-native=%lu btr-native=%lu btc-native=%lu bts-runtime=%lu btr-runtime=%lu btc-runtime=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v183_bts_native,
           (unsigned long)bb_pm16_diag.v183_btr_native,
           (unsigned long)bb_pm16_diag.v183_btc_native,
           (unsigned long)bb_pm16_diag.v183_bts_runtime,
           (unsigned long)bb_pm16_diag.v183_btr_runtime,
           (unsigned long)bb_pm16_diag.v183_btc_runtime,
           (unsigned)BB_PJ_PM16_BITMOD32_IMM_V183);
#if BB_PJ_V181_BA_STOP_CLASSIFY
    printf("[bb386-v181-ba-stops] total=%lu no-prefix=%lu pfx66=%lu pfx67=%lu pfxseg=%lu pfxlockrep=%lu pfxmulti=%lu truncated=%lu\n",
           (unsigned long)bb_pm16_diag.v181_ba_stops,
           (unsigned long)bb_pm16_diag.v181_ba_no_prefix,
           (unsigned long)bb_pm16_diag.v181_ba_pfx66,
           (unsigned long)bb_pm16_diag.v181_ba_pfx67,
           (unsigned long)bb_pm16_diag.v181_ba_pfx_seg,
           (unsigned long)bb_pm16_diag.v181_ba_pfx_lockrep,
           (unsigned long)bb_pm16_diag.v181_ba_pfx_multiple,
           (unsigned long)bb_pm16_diag.v181_ba_truncated);
    printf("[bb386-v181-ba-forms] bt-reg=%lu bt-mem=%lu modify-reg=%lu modify-mem=%lu other-reg=%lu other-mem=%lu\n",
           (unsigned long)bb_pm16_diag.v181_ba_bt_reg,
           (unsigned long)bb_pm16_diag.v181_ba_bt_mem,
           (unsigned long)bb_pm16_diag.v181_ba_modify_reg,
           (unsigned long)bb_pm16_diag.v181_ba_modify_mem,
           (unsigned long)bb_pm16_diag.v181_ba_other_reg,
           (unsigned long)bb_pm16_diag.v181_ba_other_mem);
#endif
    printf("[bb386-v16-pm16-movext-mem] native-translations=%lu runtime-attempt=%lu runtime-ok=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v16_movext_mem_native,
           (unsigned long)bb_pm16_diag.v16_memread_attempt,
           (unsigned long)bb_pm16_diag.v16_memread_ok,
           (unsigned)BB_PJ_PM16_MOVEXT_MEM_V16);
#endif
    printf("[bb386-v15-pm16-nearjcc] native-translations=%lu enabled=%u\n",
           (unsigned long)bb_pm16_diag.v15_pm16_near_jcc_native,
           (unsigned)BB_PJ_PM16_JCC0F_V15);
#if BB_PJ_PM16_STACK_REJECT_DIAG_V142
    printf("[bb386-v142-stack-refuse] ss=%lu wrap=%lu bounds=%lu overflow=%lu align=%lu protection=%lu mapping=%lu\n",
           (unsigned long)bb_pm16_diag.v142_bad_ss,
           (unsigned long)bb_pm16_diag.v142_wrap,
           (unsigned long)bb_pm16_diag.v142_bounds,
           (unsigned long)bb_pm16_diag.v142_overflow,
           (unsigned long)bb_pm16_diag.v142_alignment,
           (unsigned long)bb_pm16_diag.v142_protection,
           (unsigned long)bb_pm16_diag.v142_mapping);
#endif
#if BB_PJ_LINK_AUDIT_BYTES_V11
    printf("[bb386-v11-pm16-66] push-reg=%lu pop-reg=%lu other-stack=%lu\n",
           (unsigned long)bb_pm16_diag.v11_66_push,
           (unsigned long)bb_pm16_diag.v11_66_pop,
           (unsigned long)bb_pm16_diag.v11_66_stack_other);
#endif
    /* Translation-stop histogram only.  The existing blitzBUS [bb386-0f]
     * c-executed counter is a DIFFERENT metric and is not mode-separated. */
    if (j) {
        static const char *const names[2] = { "pm16", "flat32" };
        unsigned m;
        for (m = 0; m < 2u; ++m) {
            printf("[bb386-v15-0f-stops] mode=%s total=%lu prefixed=%lu 00=%lu 01=%lu 02=%lu 20=%lu 22=%lu 83=%lu 84=%lu 85=%lu 86=%lu A8=%lu A9=%lu B6=%lu B7=%lu BA=%lu BD=%lu BF=%lu\n",
                names[m], (unsigned long)j->v15_0f_total[m],
                (unsigned long)j->v15_0f_prefixed[m],
                (unsigned long)j->v15_0f_stop[m][0], (unsigned long)j->v15_0f_stop[m][1],
                (unsigned long)j->v15_0f_stop[m][2], (unsigned long)j->v15_0f_stop[m][3],
                (unsigned long)j->v15_0f_stop[m][4], (unsigned long)j->v15_0f_stop[m][5],
                (unsigned long)j->v15_0f_stop[m][6], (unsigned long)j->v15_0f_stop[m][7],
                (unsigned long)j->v15_0f_stop[m][8], (unsigned long)j->v15_0f_stop[m][9],
                (unsigned long)j->v15_0f_stop[m][10], (unsigned long)j->v15_0f_stop[m][11],
                (unsigned long)j->v15_0f_stop[m][12], (unsigned long)j->v15_0f_stop[m][13],
                (unsigned long)j->v15_0f_stop[m][14], (unsigned long)j->v15_0f_stop[m][15]);
        }
    }
#endif

}
unsigned b86_pj_tlb_mode(void) { return BB_PJ_TLB_MODE; }


/* r71: flush causes and precise-invalidation counters */
void b86_pj_flush_causes(const B86Cpu *c, unsigned long out[10])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    out[0] = j ? j->flushes : 0; out[1] = j ? j->flush_smc : 0; out[2] = j ? j->flush_full : 0;
    out[3] = j ? j->flush_pm16 : 0; out[4] = j ? j->flush_cps : 0;
    out[5] = j ? j->smc_events : 0; out[6] = j ? j->smc_kills : 0;
    out[7] = j ? j->refinds : 0;
    out[8] = j ? j->evictions : 0; out[9] = j ? j->unlinks : 0;
}

#ifdef B86_PJ_CPROF
unsigned long b86_pj_op_bytes[512], b86_pj_op_count[512], b86_pj_stub_bytes, b86_pj_blocks_x, b86_pj_insns_x;
unsigned long b86_pm_c_op_exec[512];   /* harness profile storage */
#endif

/* r72: attach an SRAM tier (call after b86_pj_attach). bytes >= 32 KiB. */
int b86_pj_attach_hot(B86Cpu *c, void *buf, size_t bytes)
{
    B86Pj *j = c ? (B86Pj *)c->pj : NULL;
    uint32_t sv_used, sv_end, sv_half, sv_cur, sv_f0, sv_f1, sv_nl;
    if (!j || !buf || bytes < 32768u || (((uintptr_t)buf) & 3u)) return 0;
    j->hot = (uint8_t *)buf;
    j->maxredir = 512u;                              /* 16-byte entries: 8 KiB at the end of the tier */
    j->hot_after = PJ_HOT_AFTER;
    j->hot_cap = (uint32_t)bytes - j->maxredir * 16u;
    j->hot_base = (uint32_t)((uintptr_t)j->hot - (uintptr_t)j->code);
    sv_used = j->used; sv_end = j->shared_end; sv_half = j->half; sv_cur = j->cur_half;
    sv_f0 = j->half_fill[0]; sv_f1 = j->half_fill[1]; sv_nl = j->nlinks;
    {   struct PjSh mainsh;
        pj_sh_get(j, &mainsh);
        pj_emit_shared_at(j, j->hot, j->hot + j->hot_cap);
        j->hot_start = j->used;                      /* offset of the end of the SRAM shared code */
        pj_sh_get(j, &j->hsh);                       /* SRAM routines */
        pj_sh_set(j, &mainsh);
    }
    j->used = sv_used; j->shared_end = sv_end; j->half = sv_half; j->cur_half = sv_cur;
    j->half_fill[0] = sv_f0; j->half_fill[1] = sv_f1; j->nlinks = sv_nl;
    j->hot_used = j->hot_start; j->nredir = 0u;
#ifdef B86_PJ_RANGE_CHECK
    fprintf(stderr, "[pj] main: tail_z+%X tail+%X exit_s+%X chk+%X memtramp+%X entry+%X dyn+%X lzh0+%X memt0+%X\n",
        (unsigned)(j->tail_z-j->code),(unsigned)(j->tail-j->code),(unsigned)(j->exit_s-j->code),(unsigned)(j->chk-j->code),
        (unsigned)(j->memtramp-j->code),(unsigned)(j->entry-j->code),(unsigned)(j->dyn-j->code),(unsigned)(j->lzh[0]-j->code),(unsigned)(j->memt[0]-j->code));
    fprintf(stderr, "[pj] hot: tail_z+%X exit_s+%X chk+%X memtramp+%X entry+%X dyn+%X lzh0+%X memt0+%X start+%X\n",
        (unsigned)(j->hsh.tail_z-j->hot),(unsigned)(j->hsh.exit_s-j->hot),(unsigned)(j->hsh.chk-j->hot),
        (unsigned)(j->hsh.memtramp-j->hot),(unsigned)(j->hsh.entry-j->hot),(unsigned)(j->hsh.dyn-j->hot),(unsigned)(j->hsh.lzh[0]-j->hot),(unsigned)(j->hsh.memt[0]-j->hot),(unsigned)(j->hot_start-j->hot_base));
#endif
    /* BB_PJ_SHARED_ATLAS_V1: read-only emitter boundary report, attach only. */
    printf("[bb386-shared-layout] tier=psram base=%08lX shared-end=%lu tail_z=%lu tail=%lu exit_s=%lu chk=%lu lzh0=%lu memtramp=%lu memt0=%lu entry=%lu dyn=%lu\n",
        (unsigned long)(uintptr_t)j->code, (unsigned long)j->shared_end,
        (unsigned long)(uintptr_t)(j->tail_z - j->code),
        (unsigned long)(uintptr_t)(j->tail - j->code),
        (unsigned long)(uintptr_t)(j->exit_s - j->code),
        (unsigned long)(uintptr_t)(j->chk - j->code),
        (unsigned long)(uintptr_t)(j->lzh[0] - j->code),
        (unsigned long)(uintptr_t)(j->memtramp - j->code),
        (unsigned long)(uintptr_t)(j->memt[0] - j->code),
        (unsigned long)(uintptr_t)(j->entry - j->code),
        (unsigned long)(uintptr_t)(j->dyn - j->code));
    printf("[bb386-shared-layout] tier=sram base=%08lX shared-end=%lu tail_z=%lu tail=%lu exit_s=%lu chk=%lu lzh0=%lu memtramp=%lu memt0=%lu entry=%lu dyn=%lu\n",
        (unsigned long)(uintptr_t)j->hot, (unsigned long)(j->hot_start - j->hot_base),
        (unsigned long)(uintptr_t)(j->hsh.tail_z - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.tail - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.exit_s - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.chk - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.lzh[0] - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.memtramp - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.memt[0] - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.entry - j->hot),
        (unsigned long)(uintptr_t)(j->hsh.dyn - j->hot));
    fflush(stdout);
    /* BB_PJ_SHARED_ATLAS_V1_END */
    /* BB_PJ_V3_MEMCODE_CAPTURE: boot-only Thumb2 byte dump; no codegen change. */
#ifndef BB_PJ_V3_MEMCODE_CAPTURE
#define BB_PJ_V3_MEMCODE_CAPTURE 1
#endif
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE && BB_PJ_V3_MEMCODE_CAPTURE
    /* PJ shared routines are fully emitted by this point, separately into
     * PSRAM and SRAM.  Do not disassemble on device or mutate emitted code.
     * Range is exactly [memtramp, entry), including the V2 write shim and
     * all six memory-entry stubs.  Output is intentionally one-shot. */
    {
        unsigned bbv3_tier;
        for (bbv3_tier = 0u; bbv3_tier < 2u; ++bbv3_tier) {
            const char *name = bbv3_tier ? "sram" : "psram";
            const uint8_t *start = bbv3_tier ? j->hsh.memtramp : j->memtramp;
            const uint8_t *stub = bbv3_tier ? j->hsh.memt[0] : j->memt[0];
            const uint8_t *end = bbv3_tier ? j->hsh.entry : j->entry;
            const uintptr_t a = (uintptr_t)start, b = (uintptr_t)end, m = (uintptr_t)stub;
            if (!start || !stub || !end || !(a < m && m < b) || b - a > 512u) {
                printf("[bb386-v3-error] tier=%s invalid-memory-code-range\n", name);
                continue;
            }
            printf("[bb386-v3-blob] tier=%s start=%08lX len=%lu memt0=%08lX entry=%08lX\n",
                name, (unsigned long)a, (unsigned long)(b - a),
                (unsigned long)m, (unsigned long)b);
            {
                uint32_t k;
                for (k = 0u; k < (uint32_t)(b - a); k += 32u) {
                    uint32_t q, n = (uint32_t)(b - a) - k;
                    if (n > 32u) n = 32u;
                    printf("[bb386-v3-hex] tier=%s off=%lu data=", name, (unsigned long)k);
                    for (q = 0u; q < n; ++q) printf("%02X", (unsigned)start[k + q]);
                    putchar('\n');
                }
            }
        }
        fflush(stdout);
    }
#endif
    pj_pcs_hlo = (uint32_t)(uintptr_t)j->hot; pj_pcs_hhi = pj_pcs_hlo + j->hot_cap;   /* r72f */
    return 1;
}
/* r72e: host passes &xip_ctrl_hw->ctr_acc, &xip_ctrl_hw->ctr_hit and a
 * free-running microsecond counter (e.g. &timer_hw->timerawl); NULLs disable. */
void b86_pj_set_xip_counters(B86Cpu *c, const volatile uint32_t *acc, const volatile uint32_t *hit, const volatile uint32_t *us)
{
    B86Pj *j = c ? (B86Pj *)c->pj : NULL;
    if (!j) return;
    j->xacc = (acc && hit && us) ? acc : NULL; j->xhit = hit; j->xus = us; j->xlt = 0u;
#if B86_PJ_V30K_XIP_SPLIT
    printf("[bb386-v30k-xip] mode=1 detailed XIP split enabled (baseline overhead)\n");
#else
    printf("[bb386-v30k-xip] mode=0 native-entry XIP timing removed; bb386-xip counters unavailable\n");
#endif
}
/* out: [0..3] SRAM-tier runs, insns, XIP misses, us; [4..7] the same for PSRAM
 * runs; [8..10] gaps between runs (count, misses, us); [11] samples dropped
 * because a counter was cleared or saturated meanwhile. Cumulative. */
void b86_pj_xip_split(const B86Cpu *c, unsigned long out[12])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    unsigned k;
    for (k = 0u; k < 12u; ++k) out[k] = j ? j->xs[k] : 0u;
}
void b86_pj_hot_stats(const B86Cpu *c, unsigned long out[6])
{
    const B86Pj *j = c ? (const B86Pj *)c->pj : NULL;
    out[0] = j ? j->promotions : 0; out[1] = j ? j->hot_resets : 0; out[2] = j ? j->hot_refused : 0;
    out[3] = j && j->hot ? j->hot_used - j->hot_start : 0; out[4] = j && j->hot ? j->hot_base + j->hot_cap - j->hot_start : 0;
    out[5] = j ? j->nredir : 0;
}

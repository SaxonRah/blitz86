/* blitz86 — 8086 real-mode CPU core: reference interpreter + dynamic
   binary translator to AArch64 (Pi Zero 2 W) and Thumb-2 (RP2350).

   B86Cpu is shared by the interpreter, the C runtime and generated code.
   Generated code addresses its fields with offsetof(), so the layout may
   change freely; it is never hard-coded in an emitter. */
#ifndef B86_H
#define B86_H

#include <stddef.h>
#include <stdint.h>
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
#include "x87_state.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Place blitz86 code in RAM where flash executes through a shared cache
   (RP2350: the 16 KiB XIP cache also fronts PSRAM guest memory). */
#if defined(B86_RAM_FUNCS) && B86_RAM_FUNCS
#define B86_HOT __attribute__((section(".time_critical.blitz86")))
#else
#define B86_HOT
#endif
/* r69: protected-mode hot path (PM40, fetch window, page walk, PM dispatch).
   In SRAM with B86_PM_RAM=1 (about 12 KiB; on blitzBUS lower BB_CODE_KB to
   make room). Otherwise it runs from flash like the rest of the PM stages. */
#if defined(B86_RAM_FUNCS) && B86_RAM_FUNCS && defined(B86_PM_RAM) && B86_PM_RAM
#define B86_PM_HOT __attribute__((section(".time_critical.blitz86pm")))
#else
#define B86_PM_HOT
#endif
/* Translator / emitter code: runs only while translating, so it stays in
   flash (B86_COLD_RAM=1 puts it back in RAM). */
#if defined(B86_RAM_FUNCS) && B86_RAM_FUNCS && defined(B86_COLD_RAM) && B86_COLD_RAM
#define B86_COLD B86_HOT
#else
#define B86_COLD
#endif

/* Guest memory: 1 MiB + the 64 KiB HMA (FFFF:0010..FFFF:FFFF) + slack so a
   word access at the very top never runs off the end. With A20 enabled
   (the JIT's model) seg*16+off is never wrapped, so every segment is a flat
   64 KiB window starting at mem + seg*16. */
#define B86_MEM_BYTES   (0x110000u + 0x20u)
#define B86_LINE_SHIFT  6u                         /* SMC line = 64 bytes */
/* r63: total guest RAM. B86_MEM_BYTES above stays the *low window*: real
   mode (1 MiB + HMA), everything the JIT translates, the SMC line table,
   the page-delta map and the translation shadow. Extended memory above it
   (B86_MEM_BYTES..B86_RAM_BYTES) is reached only by protected-mode code
   through the interpreter; it is never translated, mapped or SMC-tracked.
   The guest buffer passed to b86_init must hold B86_RAM_BYTES. */
#ifndef B86_RAM_BYTES
#define B86_RAM_BYTES   B86_MEM_BYTES
#endif
#if B86_RAM_BYTES < B86_MEM_BYTES
#error "B86_RAM_BYTES must be >= B86_MEM_BYTES"
#endif
#define B86_LINES       ((B86_MEM_BYTES >> B86_LINE_SHIFT) + 1u)

enum { B86_AX, B86_CX, B86_DX, B86_BX, B86_SP, B86_BP, B86_SI, B86_DI };
enum { B86_ES, B86_CS, B86_SS, B86_DS };
/* BB_386_RM3_V1: extra segment selectors; do not renumber the native four. */
enum { B86_FS = 4, B86_GS = 5 };

enum {
    B86_CF = 0x0001, B86_PF = 0x0004, B86_AF = 0x0010, B86_ZF = 0x0040,
    B86_SF = 0x0080, B86_TF = 0x0100, B86_IF = 0x0200, B86_DF = 0x0400,
    B86_OF = 0x0800,
    B86_ARITH = B86_CF | B86_PF | B86_AF | B86_ZF | B86_SF | B86_OF
};

/* Lazy flags. When lz_kind != LZ_NONE the six arithmetic flags are derived
   from (lz_a, lz_res) instead of `flags`; for INC/DEC, CF still comes from
   `flags`. The second operand is implied: b = res - a (ADD/INC) or
   b = a - res (SUB/DEC), exact because lazily recorded producers never
   have a carry-in (ADC/SBB materialize eagerly). Values may carry garbage
   above the operand width. Two stores per producer instead of three.
   An INC/DEC overlay record (lz_ikind) may sit on top of the main record. */
enum {
    LZ_NONE = 0,
    LZ_ADD8, LZ_ADD16, LZ_SUB8, LZ_SUB16,
    LZ_LOG8, LZ_LOG16, LZ_INC8, LZ_INC16, LZ_DEC8, LZ_DEC16,
    LZ_SHL8, LZ_SHL16, LZ_SHR8, LZ_SHR16, LZ_SAR8, LZ_SAR16,   /* count 1 */
    LZ_ADC8, LZ_ADC16, LZ_SBB8, LZ_SBB16,                       /* explicit lz_b */
    LZ_SZPC8, LZ_SZPC16,  /* SF/ZF/PF from lz_res, CF/OF bits in lz_b, AF=0
                             (a shift by 1 followed by RCL/RCR by 1) */
    LZ_ADD32, LZ_SUB32, LZ_LOG32, LZ_INC32, LZ_DEC32,   /* r56: 386 32-bit records */
    LZ_SHL8A, LZ_SHL16A, LZ_SHR8A, LZ_SHR16A, LZ_SAR8A, LZ_SAR16A, /* r59: 386 shift by 1, AF kept from flags */
    LZ_SZPC32,            /* r70: 32-bit SF/ZF/PF from lz_res, CF/OF bits in lz_b, AF=0 */
    LZ_ADC32, LZ_SBB32,   /* r70: 32-bit ADC/SBB, explicit lz_b */
    LZ_COUNT
};

/* Return codes from b86_step / b86_run. */
enum { B86_OK = 0, B86_HALT = 1, B86_EXIT = 2, B86_BUDGET = 3, B86_TRAP = 4, B86_UNSUPPORTED = 5, B86_SHUTDOWN = 6 };

struct B86Cpu;
typedef uint8_t (*B86In8)(struct B86Cpu *, uint16_t port);
typedef void    (*B86Out8)(struct B86Cpu *, uint16_t port, uint8_t v);
/* Return nonzero if the host handled the software interrupt (HLE). */
typedef int     (*B86IntHook)(struct B86Cpu *, uint8_t vector);
typedef void    (*B86SmcHook)(struct B86Cpu *, uint32_t lin, uint32_t len);
/* Called for every guest byte range a new translation depends on. */
typedef void    (*B86CodeHook)(struct B86Cpu *, uint32_t lin, uint32_t len);

enum { B86_CPU_8086 = 0, B86_CPU_186 = 1, B86_CPU_286 = 2, B86_CPU_386 = 3 }; /* BB_386_RM0_V1: 386 real mode */   /* B86Cpu.model (286: real mode) */

/* BB_386_STAGE5C5C_V1: appended shadow caches, no native-hot offsets changed. */
typedef struct B86PM1Cache {
    uint32_t base, limit;
    uint16_t selector;
    uint8_t access, flags, valid, reserved;
} B86PM1Cache;
typedef struct B86PM1State {
    B86PM1Cache seg[6]; /* ES,CS,SS,DS,FS,GS; CS not activated in 5C5C */
    uint32_t ldtr_base, ldtr_limit;
    uint16_t ldtr_selector;
    uint8_t ldtr_valid, reserved;
} B86PM1State;

typedef struct B86Cpu {
    /* --- hot block: generated code touches these ------------------------ */
    uint32_t r[8];          /* GPRs; only bits 0..15 are meaningful        */
    uint32_t lz_kind;
    uint32_t lz_a;
    uint32_t lz_res;
    uint32_t lz_b;          /* ADC/SBB only: second operand (carry-in unknown) */
    uint32_t lz_ikind;      /* INC/DEC overlay: OSZAP from (lz_ia, lz_ires), */
    uint32_t lz_ia;         /* CF still from the main record / flags. Lets */
    uint32_t lz_ires;       /* INC/DEC record lazily without touching CF.   */
    uint32_t flags;         /* authoritative except lazily-covered bits    */
    uint32_t ip;
    uint32_t seg[4];        /* ES CS SS DS                                  */
    volatile uint32_t irq;  /* host sets nonzero to force a return to C    */
    uint32_t scratch;       /* spill slot for generated code               */
    uint8_t *segp[4];       /* mem + seg*16, kept in sync with seg[]       */
    uint8_t *mem;
    uint8_t *codemap;       /* NULL, or one byte per 64-byte line          */
    uint8_t *codemap_host;  /* codemap biased so [hostaddr >> 6] indexes it */
    void    *fast;          /* JIT direct-mapped (key -> host) table        */
    uintptr_t patch;        /* JIT: branch site to patch on a chain exit    */
    uint32_t retired;       /* JIT debug: guest insns retired by last block  */
    uint32_t icnt;          /* JIT: retired guest insns (wraps; enable with
                               b86_jit_set_count_retired, read deltas)      */
    uint32_t trap_cs;       /* b86_jit_run returns B86_TRAP before running
                               any code whose CS equals this (0xFFFFFFFF: off) */
    uint32_t pad1;
    uint32_t rep_tmp[2];    /* JIT: scratch for the inline REP MOVS/STOS path */
    uint32_t model;         /* CPU model: B86_CPU_8086 (0, default), _186, _286 */
    uint32_t idt_base, gdt_base;    /* 286: LIDT/LGDT (24-bit bases); real-mode IVT = idt_base */
    uint16_t idt_limit, gdt_limit;
    uint16_t msw;                   /* 286 machine status word (PE, MP, EM, TS) */

    /* --- configuration ----------------------------------------------------- */
    uint32_t amask;         /* 0xFFFFF = 8086 1 MiB wrap; 0x1FFFFF = A20 on */
    uint32_t exact_wrap;    /* 1: word at offset FFFF wraps inside segment  */

    /* --- host hooks ----------------------------------------------------------- */
    B86In8 in8;
    B86Out8 out8;
    B86IntHook int_hook;
    B86SmcHook smc_hook;    /* called by the interpreter for stores to code */
    B86CodeHook code_hook;  /* optional: translated ranges (embedder write filters) */
    void *user;
    struct B86Jit *jit;

    uint64_t icount;        /* instructions retired by the interpreter      */
    uint16_t step_oip;      /* last b86_step: IP of its opcode byte (after prefixes) */
    uint8_t  step_op;       /* last b86_step: the opcode                    */
#ifdef B86_PAGED
    /* Page deltas (Thumb-2 + C paths; -DB86_PAGED). A guest access to the
       nominal host address h = mem + linear really goes to h + pt[(h >> 12) & 511].
       0 for normal pages; nonzero for 4 KiB pages moved elsewhere (SRAM)
       with b86_map_range. `pt` points at pt_store, or (JIT) at the first
       2 KiB of the SMC line table, which generated code reaches through r9.
       Stack (SS) accesses in generated code are NOT translated: the embedder
       must never map pages inside the current SS window. */
    int32_t *pt;
    uint32_t pt_active_pages; /* page-delta specialization guard; maintained by map/unmap */
    int32_t pt_store[512];
    /* bytes stored by REP STOS/MOVS per 4 KiB guest page since the embedder
       last cleared it: a cheap signal for which pages to move to SRAM */
    uint32_t rep_page_bytes[B86_MEM_BYTES >> 12];
#endif
    /* BB_386_RM3_V1: appended only; old generated-code offsets unchanged. */
    uint16_t seg386[2];   /* FS and GS selectors, real mode */
    uint32_t shutdown386; /* BB_386_STAGE5C4A_V1: sticky 386 shutdown until b86_init */
    B86PM1State pm386; /* Not active in interpreter/JIT until PE is implemented. */
    /* r55 (386 JIT tier 1): while b86_jit_run executes 386 real-mode code
       natively, generated code treats r[] as 16-bit registers and may leave
       anything in bits 16-31. The authoritative upper halves of EAX..EDI
       live here; they are merged back before any interpreter step and when
       b86_jit_run returns, so outside b86_jit_run r[] is always exact. */
    uint32_t rhi[8];
    /* r65 (B86_PG_V1): 80386 paging, appended only. pg mirrors CR0.PG and is
       only ever set while CR0.PE=1. The 64-entry TLB caches linear page ->
       physical page (tag = linear page | 1); it is flushed by CR3 writes and
       CR0.PG changes, as on a 386 (no INVLPG, no global pages, no WP). A
       translation miss sets pf_pending/pf_lin and returns a scratch page; the
       protected-mode step rolls the instruction back and delivers #PF(14). */
    uint32_t cr2, cr3;
    uint8_t  pg, pf_pending, pf_err, pg_pad;
    uint32_t pf_lin;
    uint32_t tlb_tag[64], tlb_phys[64];
    /* r65: task register (LTR/STR). No hardware task switching. */
    uint32_t tr_base, tr_limit;
    uint16_t tr_sel, tr_pad;
    /* r66: protected-mode instruction-fetch window, valid only inside one
       b86_step: up to 16 bytes at fb_cs:fb_ip, limit- and page-checked. */
    uint32_t fb_ip;
    uint16_t fb_cs;
    uint8_t  fb_len, fb_pad;
    uint8_t  fb[16];
    /* r68: arithmetic flags the last PM40 instruction left undefined */
    uint32_t pm40_undef;
    /* r70: protected-mode native translator state (b86_pj_attach), or NULL */
    void *pj;
    uintptr_t pj_exit;      /* r70: address after the static exit a block left by (0: none) */
    uint32_t pj_stop;       /* r70: nonzero stops block chaining (code page written) */
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
    B86X87 x87; /* Stage4B: experimental register-only PM x87 state */
#endif
} B86Cpu;

/* r65: translate through the guest page tables (interp.c). */
uint8_t *b86_pg_host(B86Cpu *c, uint32_t lin);

/* Host pointer for a guest linear address (honours mapped pages). */
#ifdef B86_PAGED
static inline __attribute__((always_inline)) uint8_t *b86_host_phys(const B86Cpu *c, uint32_t lin)
{
    uint8_t *p = c->mem + lin;
#if B86_RAM_BYTES > B86_MEM_BYTES
    if (lin >= B86_MEM_BYTES) return p;           /* r63: extended memory: never mapped */
#endif
    return p + c->pt[((uintptr_t)p >> 12) & 511u];
}
#else
static inline __attribute__((always_inline)) uint8_t *b86_host_phys(const B86Cpu *c, uint32_t lin) { return c->mem + lin; }
#endif
/* Guest linear address -> host pointer. With CR0.PG clear (always in real
   mode, and in the JIT) linear == physical; otherwise b86_pg_host walks the
   guest page tables. */
static inline __attribute__((always_inline)) uint8_t *b86_host(const B86Cpu *c, uint32_t lin)
{
    if (__builtin_expect(c->pg != 0, 0)) return b86_pg_host((B86Cpu *)c, lin);
    return b86_host_phys(c, lin);
}

/* ---- core / interpreter --------------------------------------------------- */
void     b86_init(B86Cpu *c, uint8_t *mem);           /* mem: B86_MEM_BYTES  */
void     b86_set_seg(B86Cpu *c, int s, uint16_t v);
uint16_t b86_get_flags(B86Cpu *c);                    /* materializes lazy  */
void     b86_set_flags(B86Cpu *c, uint16_t f);
void     b86_flags_materialize(B86Cpu *c);
int      b86_step(B86Cpu *c);                         /* one instruction    */
int      b86_run_interp(B86Cpu *c, uint64_t max_insns);
void     b86_interrupt(B86Cpu *c, uint8_t vector);    /* real-mode INT n    */
/* r67: deliver an external (hardware) interrupt between instructions, in
   real or protected mode. Honours EFLAGS.IF and the MOV SS/POP SS shadow;
   protected mode goes through the IDT (interrupt/trap gate or task gate).
   Returns 1 when delivered, 0 when the CPU cannot take it now (retry later).
   The caller owns the PIC (IRR/ISR) and the vector number. */
int      b86_irq(B86Cpu *c, uint8_t vector);
/* r70: native translation of flat 32-bit protected-mode code (Thumb-2 hosts).
   state: b86_pj_state_bytes() bytes; code: executable buffer (>= 16 KiB). */
size_t   b86_pj_state_bytes(void);
int      b86_pj_attach(B86Cpu *c, void *state, size_t state_bytes, void *code, size_t code_bytes);
void     b86_pj_stats(const B86Cpu *c, unsigned long out[8]);
void     b86_pj_stop_ops(const B86Cpu *c, unsigned long out[256]);
extern const uint8_t b86_parity[256];

static inline uint16_t b86_r16(const B86Cpu *c, int r) { return (uint16_t)c->r[r]; }

/* ---- JIT -------------------------------------------------------------------- */
/* Paged guest memory (-DB86_PAGED, Thumb-2 backend). Move the 4 KiB pages
   [lin, lin+len) to `frame` (len bytes, contiguous: consecutive pages stay
   consecutive, so accesses straddling two pages *inside* the range are
   exact; a 16-bit access straddling the range's first or last byte is not).
   The guest buffer must be 4 KiB aligned. Copies the current contents in.
   Fails: -1 unsupported/misaligned/already mapped, -2 the frame's SMC-table
   index range collides with guest lines, -3 the range holds translated code. Code in mapped pages is always
   interpreted (stores there are not SMC-checked). Unmap copies back. */
int      b86_map_range(B86Cpu *c, uint32_t lin, uint32_t len, uint8_t *frame);
void     b86_unmap_range(B86Cpu *c, uint32_t lin, uint32_t len);
int      b86_page_mapped(const B86Cpu *c, uint32_t lin);
typedef struct B86JitStats {
    uint64_t blocks, guest_insns, host_bytes, helper_insns;
    uint64_t chains, lookups, flushes, smc_hits, smc_invalidations;
    uint64_t dispatches;
    /* BB_CHAIN_EXIT_SPLIT_V2: cumulative return-reason counters. */
    uint64_t exit_dynamic, exit_native_miss, exit_direct_lookup;
    uint64_t exit_step_return, exit_ds_guard, exit_dead_stub; /* BB_EXIT_HOTSPOT_V1 */
    uint64_t translate_us;  /* time in translation (needs -DB86_NOW=fn returning us) */
    uint64_t fast_dispatches; /* dispatcher entries resolved by the SRAM fast table */
    /* runtime C round trips from translated code */
    uint64_t rt_step, rt_cond, rt_flags, rt_light, rt_smc, rt_rep;
    uint64_t translate_misses;  /* cache misses while translating (needs -DB86_MISSES=fn) */
    /* r52g, -DB86_MPROF (needs B86_MISSES, B86_NOW; B86_XACCESS optional):
       cache misses / accesses / microseconds per runtime category:
       0 cold interpretation, 1 step helper, 2 REP helper, 3 cond/flags,
       4 SMC helper, 5 light helpers (rot1/mulflags) */
    uint64_t mp_miss[6], mp_acc[6], mp_us[6];
    uint64_t smc_bucket_visits;       /* total page-chain entries visited */
    uint64_t smc_dead_skips;          /* dead entries discovered and unlinked */
    uint64_t smc_bucket_max_depth;    /* longest page traversal */
    uint64_t smc_bucket_compactions;  /* traversals that unlinked entries */
    uint64_t smc_bucket_unlinks;      /* dead and newly invalidated entries */
    uint64_t joins;             /* superblocks ended at already-translated code */
    uint64_t inner_branches;    /* forward Jcc lowered as in-block branches */
    uint64_t splits;            /* blocks killed so a new entry inside them joins */
    uint64_t cold_runs, cold_insns;   /* interpreted before an entry became hot */
    uint64_t evictions, evicted_blocks, unchains;  /* r52 region eviction */
    uint64_t limit_retries;     /* r52c: oversized attempts retried without a flush */
    uint64_t ds_baked, ds_miss; /* r52n: blocks with DS-absolute baking / guard misses */
} B86JitStats;

/* Code buffer must be executable (and, on Pico, in SRAM). */
/* Guest memory (b86_init) must be 64-byte aligned; returns NULL otherwise. */
struct B86Jit *b86_jit_create(B86Cpu *c, void *code_buf, size_t code_size);
/* Same, with the per-store / per-indirect-jump tables placed in `hot`
   (>= b86_jit_hot_bytes(); use SRAM on RP2350). Other metadata uses calloc. */
struct B86Jit *b86_jit_create_ex(B86Cpu *c, void *code_buf, size_t code_size,
                                 void *hot, size_t hot_size);
size_t   b86_jit_hot_bytes(void);
void     b86_jit_destroy(struct B86Jit *j);
/* Run until HLT, an exit request, or roughly max_insns guest instructions. */
int      b86_jit_run(B86Cpu *c, uint64_t max_insns);
void     b86_jit_flush(struct B86Jit *j);
/* Host code wrote guest memory [lin, lin+len) behind the JIT's back (disk
   DMA, another interpreter): drop translations that depend on it. */
void     b86_jit_invalidate(struct B86Jit *j, uint32_t lin, uint32_t len);
void     b86_jit_flush_causes(const struct B86Jit *j, unsigned long out[3]);   /* r73 */
/* Count retired guest instructions exactly in cpu->icnt (one add per block
   exit, not per instruction). Takes effect for code translated afterwards;
   call before running or follow with b86_jit_flush. */
void     b86_jit_set_count_retired(struct B86Jit *j, int on);
/* Optional shadow (B86_MEM_BYTES) of translated guest bytes; enables
   byte-exact b86_jit_sync_external. Set before translating anything. */
void     b86_jit_set_shadow(struct B86Jit *j, uint8_t *shadow);
/* B86_SPARSE_SHADOW_V1: optional, bounded PSRAM snapshots of translated
   64-byte guest lines. index has B86_LINES zero-initialized uint16_t entries;
   slot_storage has slot_count * (1u << B86_LINE_SHIFT) bytes. The JIT clears
   index on attachment; storage is caller-owned. A full pool conservatively
   invalidates unsnapshotted code lines, never executes stale translations.
   Configure before translating any code, not during guest execution. */
void     b86_jit_set_sparse_shadow(struct B86Jit *j, uint16_t *index,
                                   uint8_t *slot_storage, uint32_t slot_count);
/* Memory in [lin, lin+len) may have been written by someone else: kill only
   translations whose bytes actually changed. Returns lines that differed. */
uint32_t b86_jit_sync_external(struct B86Jit *j, uint32_t lin, uint32_t len);
/* r56: the n most frequent forms the interpreter still steps in 386 real
   mode since the last call, e.g. "66F7:1234 0FA4:99" (then cleared). */
int b86_jit_386_top(struct B86Jit *j, char *buf, int len, int n);
const B86JitStats *b86_jit_stats(struct B86Jit *j);
/* v49: map a sampled Thumb-2 PC to the containing live translated block. */
int b86_jit_pc_lookup(struct B86Jit *j, uintptr_t pc, uint32_t *key, uint32_t *offset);
/* v48 sampled native dispatch profiler (diagnostic, approximate). */
void b86_jit_hot_report(struct B86Jit *j);

/* Testing: translate at most this many instructions per block (0 = default). */
void     b86_jit_set_max_block(struct B86Jit *j, unsigned n);
/* Testing: 0 disables successor flag lookahead (every flag live at exits). */
void     b86_jit_set_lookahead(struct B86Jit *j, int on);
/* Testing: one guest instruction per block, no lookahead, no fast-table
   entry, so b86_jit_run(c, 1) executes exactly one instruction. */
void     b86_jit_set_single_step(struct B86Jit *j, int on);
/* Testing: never use the fast table, so each b86_jit_run(c,1) is one block. */
void     b86_jit_set_no_fast(struct B86Jit *j, int on);
void     b86_jit_set_no_chain(struct B86Jit *j, int on);   /* testing */
void     b86_jit_set_no_spec(struct B86Jit *j, int on);    /* testing */
void     b86_jit_set_no_join(struct B86Jit *j, int on);    /* testing: no superblock joins */
/* Translate an entry point only after it has been dispatched n times within
   a decay window (n <= 255; 0 = translate at once, the default). Until then
   the dispatcher interprets it. Keeps rarely-run code out of a small code
   buffer (RP2350). */
void     b86_jit_set_hot_threshold(struct B86Jit *j, unsigned n);
int      b86_jit_evict_enabled(struct B86Jit *j);   /* r52: 1 = region eviction, 0 = whole-cache flushes */
/* Testing: exits record cpu->retired (takes effect after a flush). */
void     b86_jit_set_count_exits(struct B86Jit *j, int on);

#ifdef __cplusplus
}
#endif
#endif

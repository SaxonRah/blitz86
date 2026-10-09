/* blitz86 — 8086 real-mode CPU core: reference interpreter + dynamic
   binary translator to AArch64 (Pi Zero 2 W) and Thumb-2 (RP2350).

   B86Cpu is shared by the interpreter, the C runtime and generated code.
   Generated code addresses its fields with offsetof(), so the layout may
   change freely; it is never hard-coded in an emitter. */
#ifndef B86_H
#define B86_H

#include <stddef.h>
#include <stdint.h>

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
#define B86_LINES       ((B86_MEM_BYTES >> B86_LINE_SHIFT) + 1u)

enum { B86_AX, B86_CX, B86_DX, B86_BX, B86_SP, B86_BP, B86_SI, B86_DI };
enum { B86_ES, B86_CS, B86_SS, B86_DS };

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
    LZ_COUNT
};

/* Return codes from b86_step / b86_run. */
enum { B86_OK = 0, B86_HALT = 1, B86_EXIT = 2, B86_BUDGET = 3, B86_TRAP = 4 };

struct B86Cpu;
typedef uint8_t (*B86In8)(struct B86Cpu *, uint16_t port);
typedef void    (*B86Out8)(struct B86Cpu *, uint16_t port, uint8_t v);
/* Return nonzero if the host handled the software interrupt (HLE). */
typedef int     (*B86IntHook)(struct B86Cpu *, uint8_t vector);
typedef void    (*B86SmcHook)(struct B86Cpu *, uint32_t lin, uint32_t len);
/* Called for every guest byte range a new translation depends on. */
typedef void    (*B86CodeHook)(struct B86Cpu *, uint32_t lin, uint32_t len);

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
#ifdef B86_PAGED
    /* Page deltas (Thumb-2 + C paths; -DB86_PAGED). A guest access to the
       nominal host address h = mem + linear really goes to h + pt[(h >> 12) & 511].
       0 for normal pages; nonzero for 4 KiB pages moved elsewhere (SRAM)
       with b86_map_range. `pt` points at pt_store, or (JIT) at the first
       2 KiB of the SMC line table, which generated code reaches through r9.
       Stack (SS) accesses in generated code are NOT translated: the embedder
       must never map pages inside the current SS window. */
    int32_t *pt;
    int32_t pt_store[512];
    /* bytes stored by REP STOS/MOVS per 4 KiB guest page since the embedder
       last cleared it: a cheap signal for which pages to move to SRAM */
    uint32_t rep_page_bytes[B86_MEM_BYTES >> 12];
#endif
} B86Cpu;

/* Host pointer for a guest linear address (honours mapped pages). */
#ifdef B86_PAGED
static inline uint8_t *b86_host(const B86Cpu *c, uint32_t lin)
{
    uint8_t *p = c->mem + lin;
    return p + c->pt[((uintptr_t)p >> 12) & 511u];
}
#else
static inline uint8_t *b86_host(const B86Cpu *c, uint32_t lin) { return c->mem + lin; }
#endif

/* ---- core / interpreter --------------------------------------------------- */
void     b86_init(B86Cpu *c, uint8_t *mem);           /* mem: B86_MEM_BYTES  */
void     b86_set_seg(B86Cpu *c, int s, uint16_t v);
uint16_t b86_get_flags(B86Cpu *c);                    /* materializes lazy  */
void     b86_set_flags(B86Cpu *c, uint16_t f);
void     b86_flags_materialize(B86Cpu *c);
int      b86_step(B86Cpu *c);                         /* one instruction    */
int      b86_run_interp(B86Cpu *c, uint64_t max_insns);
void     b86_interrupt(B86Cpu *c, uint8_t vector);    /* real-mode INT n    */
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
    uint64_t translate_us;  /* time in translation (needs -DB86_NOW=fn returning us) */
    uint64_t fast_dispatches; /* dispatcher entries resolved by the SRAM fast table */
    /* runtime C round trips from translated code */
    uint64_t rt_step, rt_cond, rt_flags, rt_light, rt_smc, rt_rep;
    uint64_t translate_misses;  /* cache misses while translating (needs -DB86_MISSES=fn) */
    uint64_t smc_bucket_visits;       /* total page-chain entries visited */
    uint64_t smc_dead_skips;          /* dead entries discovered and unlinked */
    uint64_t smc_bucket_max_depth;    /* longest page traversal */
    uint64_t smc_bucket_compactions;  /* traversals that unlinked entries */
    uint64_t smc_bucket_unlinks;      /* dead and newly invalidated entries */
    uint64_t joins;             /* superblocks ended at already-translated code */
    uint64_t inner_branches;    /* forward Jcc lowered as in-block branches */
    uint64_t splits;            /* blocks killed so a new entry inside them joins */
    uint64_t cold_runs, cold_insns;   /* interpreted before an entry became hot */
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
/* Count retired guest instructions exactly in cpu->icnt (one add per block
   exit, not per instruction). Takes effect for code translated afterwards;
   call before running or follow with b86_jit_flush. */
void     b86_jit_set_count_retired(struct B86Jit *j, int on);
/* Optional shadow (B86_MEM_BYTES) of translated guest bytes; enables
   byte-exact b86_jit_sync_external. Set before translating anything. */
void     b86_jit_set_shadow(struct B86Jit *j, uint8_t *shadow);
/* Memory in [lin, lin+len) may have been written by someone else: kill only
   translations whose bytes actually changed. Returns lines that differed. */
uint32_t b86_jit_sync_external(struct B86Jit *j, uint32_t lin, uint32_t len);
const B86JitStats *b86_jit_stats(struct B86Jit *j);
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
/* Testing: exits record cpu->retired (takes effect after a flush). */
void     b86_jit_set_count_exits(struct B86Jit *j, int on);

#ifdef __cplusplus
}
#endif
#endif

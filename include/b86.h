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
enum { B86_OK = 0, B86_HALT = 1, B86_EXIT = 2, B86_BUDGET = 3 };

struct B86Cpu;
typedef uint8_t (*B86In8)(struct B86Cpu *, uint16_t port);
typedef void    (*B86Out8)(struct B86Cpu *, uint16_t port, uint8_t v);
/* Return nonzero if the host handled the software interrupt (HLE). */
typedef int     (*B86IntHook)(struct B86Cpu *, uint8_t vector);
typedef void    (*B86SmcHook)(struct B86Cpu *, uint32_t lin, uint32_t len);

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
    uint32_t pad1;

    /* --- configuration ----------------------------------------------------- */
    uint32_t amask;         /* 0xFFFFF = 8086 1 MiB wrap; 0x1FFFFF = A20 on */
    uint32_t exact_wrap;    /* 1: word at offset FFFF wraps inside segment  */

    /* --- host hooks ----------------------------------------------------------- */
    B86In8 in8;
    B86Out8 out8;
    B86IntHook int_hook;
    B86SmcHook smc_hook;    /* called by the interpreter for stores to code */
    void *user;
    struct B86Jit *jit;

    uint64_t icount;        /* instructions retired by the interpreter      */
} B86Cpu;

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
typedef struct B86JitStats {
    uint64_t blocks, guest_insns, host_bytes, helper_insns;
    uint64_t chains, lookups, flushes, smc_hits, smc_invalidations;
    uint64_t dispatches;
} B86JitStats;

/* Code buffer must be executable (and, on Pico, in SRAM). */
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
/* Testing: exits record cpu->retired (takes effect after a flush). */
void     b86_jit_set_count_exits(struct B86Jit *j, int on);

#ifdef __cplusplus
}
#endif
#endif

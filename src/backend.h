/* Backend contract. The frontend (jit.c) owns x86 semantics; a backend only
 * knows how to encode a small set of operations for its ISA.
 *
 * Virtual registers
 *   0..7   guest AX CX DX BX SP BP SI DI, resident in host registers for the
 *          whole lifetime of translated code. Bits 16..31 are garbage: every
 *          consumer that cares (addresses, compares, shifts right) masks or
 *          shifts. This removes a UXTH after almost every ALU op.
 *   V_T0, V_T1   scratch. V_T2 exists only if be_has_t2 (AArch64).
 *
 * Clobber rules (the frontend relies on these):
 *   - be_ea: dst must be V_T0; may clobber V_T1 for ES/CS (uncached bases).
 *   - be_store with check: may clobber V_T1 (Thumb-2 SMC-check temp) and
 *     NZCV when be_store_clobbers_nzcv. Without check: clobbers nothing.
 *   - be_cbz16 / be_cbnz16: may clobber V_T1 and NZCV.
 *   - be_stctx_imm: may clobber V_T1.
 *   - be_call_*: clobber V_T0, V_T1, (V_T2) and NZCV.
 *   - everything else writes only its destination and, for *_sh ops, NZCV.
 *
 * NZCV: x86 flag producers that feed an in-block branch compute ARM flags
 * on operands shifted to the top of the register (sh = 16 words, 24 bytes),
 * so N/Z/V equal SF/ZF/OF exactly and C equals CF (FM_ADD) or !CF (FM_SUB).
 */
#ifndef B86_BACKEND_H
#define B86_BACKEND_H
#include "b86.h"
#include <stdint.h>
#include <stddef.h>

enum { V_T0 = 8, V_T1 = 9, V_T2 = 10 };
enum { AOP_ADD, AOP_SUB, AOP_AND, AOP_ORR, AOP_EOR };
enum { FM_SUB = 1, FM_ADD = 2 };            /* NZCV interpretation       */
enum { AC_EQ, AC_NE, AC_CS, AC_CC, AC_MI, AC_PL, AC_VS, AC_VC,
       AC_HI, AC_LS, AC_GE, AC_LT, AC_GT, AC_LE, AC_AL };

/* Exit reasons returned by the enter trampoline. */
enum { XR_LOOKUP = 0, XR_CHAIN = 1, XR_HALT = 2, XR_IRQ = 3 };

typedef struct Emit {
    uint8_t *base;      /* start of code buffer */
    uint8_t *p, *end;
    int overflow;
    B86Cpu *cpu;
    uint32_t blk;       /* index of block being translated */
    /* shared stubs (absolute positions inside the code buffer) */
    uint8_t *x_epilogue, *x_lookup, *x_miss, *x_chain, *x_dynexit, *x_ipexit, *x_irq;
    /* out-of-line SMC slow paths pending for the current block */
    struct { uint8_t *site, *resume; uint16_t next_ip; uint8_t len; uint32_t retire; } slow[64];
    int nslow;
    int count_exits;    /* debug: exits store `retire` into cpu->retired */
    uint32_t retire;
} Emit;

extern const int be_has_t2;
extern const int be_store_clobbers_nzcv;
extern const int be_logic_mode;             /* FM_* produced by be_test_res */

/* Direct-mapped table the shared lookup stub probes for indirect jumps. */
#define B86_FASTN 4096u
typedef struct B86Fast { uint32_t key; uintptr_t host; } B86Fast;

/* Trampolines, emitted once at the start of the buffer. Returns the
   address of `enter(ctx, host)`, which runs until an exit and returns an
   exit reason (XR_*). */
void *be_emit_runtime(Emit *e);
void  be_flush_icache(void *start, size_t len);
/* Rewrite the branch at `site` (from a chain exit) to go to `target`. */
void  be_patch_branch(uint8_t *site, uint8_t *target);
/* Make block entry `entry` branch to `stub` (block invalidated). */
void  be_kill_entry(uint8_t *entry, uint8_t *stub);
uintptr_t be_code_ptr(uint8_t *p);       /* value to store in the fast table */

/* Data movement and ALU (no flags) */
void be_mov(Emit *e, int d, int s);
void be_movi(Emit *e, int d, uint32_t imm);
void be_op(Emit *e, int aop, int d, int a, int b);
void be_opi(Emit *e, int aop, int d, int a, uint32_t imm16);  /* low 16 bits exact */
void be_mvn(Emit *e, int d, int s);
void be_neg(Emit *e, int d, int s);
void be_lsl(Emit *e, int d, int s, unsigned n);
void be_ubfx(Emit *e, int d, int s, unsigned lsb, unsigned w);
void be_sbfx(Emit *e, int d, int s, unsigned lsb, unsigned w);
void be_bfi(Emit *e, int d, int s, unsigned lsb, unsigned w);
void be_sxtb(Emit *e, int d, int s);

/* NZCV producers (operands hold values in their low bits) */
int  be_addsub_sh(Emit *e, int sub, int x, int d, int a, int b, int sh);    /* d = a +/- b */
int  be_addsubi_sh(Emit *e, int sub, int x, int d, int a, uint32_t imm, int sh, int tmp);
int  be_cmp_sh(Emit *e, int x, int a, int b, int sh);
int  be_cmpi_sh(Emit *e, int x, int a, uint32_t imm, int sh, int tmp);
int  be_neg_sh(Emit *e, int x, int d, int a, int sh);
int  be_test_res(Emit *e, int x, int r, int sh);    /* flags of a logic result */
int  be_imm_ok_sh(uint32_t imm, int sh);            /* encodable without tmp   */

/* Guest memory. EA = segbase + (uint16)(b1 + b2 + disp); b1/b2 may be -1. */
void be_ea(Emit *e, int d, int seg, int b1, int b2, int32_t disp);
void be_ea_off(Emit *e, int d, int b1, int b2, int32_t disp);   /* LEA */
void be_load(Emit *e, int w16, int d, int addr);
/* check: emit SMC line check. next_ip is where to resume if this store
   invalidated the running block. */
void be_store(Emit *e, int w16, int v, int addr, int check, uint16_t next_ip);
void be_set_seg(Emit *e, int s, int v);

/* ctx access */
void be_ldctx(Emit *e, int d, unsigned off);
void be_stctx(Emit *e, int v, unsigned off);
void be_stctx_imm(Emit *e, uint32_t imm, unsigned off);

/* Control flow. Positions are code addresses inside the buffer. */
uint8_t *be_jcc(Emit *e, int ac);           /* conditional, bind later */
uint8_t *be_jmp(Emit *e);                   /* unconditional, bind later */
uint8_t *be_cbnz(Emit *e, int r);           /* branch if r != 0 */
uint8_t *be_cbz16(Emit *e, int r);          /* branch if (uint16)r == 0 */
uint8_t *be_cbnz16(Emit *e, int r);         /* branch if (uint16)r != 0 */
void     be_bind(Emit *e, uint8_t *site, uint8_t *target);

/* Exits */
void be_exit_chain(Emit *e, uint16_t target_ip, int poll);  /* patchable */
void be_exit_ip_reg(Emit *e, int r);        /* near indirect: ip in r     */
void be_exit_ip_imm(Emit *e, uint16_t ip, int reason);
void be_exit_dyn(Emit *e);                  /* ctx->ip already valid      */

/* Helpers into C. All sync guest registers through ctx as needed. */
void be_call_step(Emit *e, uint16_t ip, uint16_t next); /* interp one insn; exits if control changed */
void be_call_cond(Emit *e, int cc);         /* result (0/1) -> V_T0       */
void be_call_flags(Emit *e);                /* materialize lazy flags     */
void be_finish_block(Emit *e);              /* emit pending slow paths    */

/* Runtime helpers called from generated code (defined in jit.c). */
uint32_t b86h_step(B86Cpu *c, uint32_t ip_next, uint32_t blk); /* ip | next<<16 */
uint32_t b86h_cond(B86Cpu *c, uint32_t cc);
void     b86h_flags(B86Cpu *c);
uint32_t b86h_smc(B86Cpu *c, uint8_t *host, uint32_t len, uint32_t blk);

#endif

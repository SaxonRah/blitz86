/* Reference 8086 interpreter.
 *
 * Two jobs: (1) the oracle every translated instruction is checked against,
 * validated itself against SingleStepTests physical-8086 vectors; (2) the
 * cold path the JIT calls for instructions it does not lower natively.
 * It is written for clarity and exactness, not speed. Flags are eager here;
 * lazy state from generated code is materialized on entry. */
#include "b86.h"
#include <string.h>
/* 5C5E: forward declaration for CR0 dispatch before PM3 implementation. */
#include "b86_pm3.h"
/* 5C5F protected fetch/decode API */
#include "b86_pm4.h"
/* 5C5G: arithmetic and relative control flow */
#include "b86_pm5.h"
/* 5C5H: protected ModR/M register and memory execution */
#include "b86_pm6.h"
/* Stage5C5I: protected CPL0 near stack/flow */
#include "b86_pm7.h"
/* Stage5C5J: protected indirect near ModR/M flow */
#include "b86_pm8.h"
/* Stage5C5K: protected selectors, LEA and XCHG */
#include "b86_pm9.h"
/* Stage5C5L: protected direct far transfers */
#include "b86_pm10.h"
/* Stage5C5M: SS load shadow */
#include "b86_pm11.h"
/* Stage5C5N: software protected IDT */
#include "b86_pm12.h"
/* Stage5C5O: CPU faults with IDT */
#include "b86_pm13.h"
/* Stage5C5P: contributory faults + DF */
#include "b86_pm14.h"
/* Stage5C5Q: #DE / #BR */
#include "b86_pm15.h"
#include "b86_pm16.h"
#include "b86_pm17.h"
#include "b86_pm18.h"
#include "b86_pm19.h"
#include "b86_pm20.h"

const uint8_t b86_parity[256] = {
#define P2(n) n, n ^ 4, n ^ 4, n
#define P4(n) P2(n), P2(n ^ 4), P2(n ^ 4), P2(n)
#define P6(n) P4(n), P4(n ^ 4), P4(n ^ 4), P4(n)
    P6(4), P6(0), P6(0), P6(4)
#undef P2
#undef P4
#undef P6
};

/* ------------------------------------------------------------------------ */
/* State helpers                                                            */
/* ------------------------------------------------------------------------ */

B86_HOT void b86_set_seg(B86Cpu *c, int s, uint16_t v)
{
    c->seg[s] = v;
    c->segp[s] = c->mem + ((uint32_t)v << 4);
}

B86_HOT void b86_init(B86Cpu *c, uint8_t *mem)
{
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->amask = 0x1FFFFFu;
    c->flags = 0xF002u;
    c->trap_cs = 0xFFFFFFFFu;
    c->idt_limit = 0x03FFu;                         /* 286 reset state */
#ifdef B86_PAGED
    c->pt = c->pt_store;
#endif
    for (int s = 0; s < 4; ++s) b86_set_seg(c, s, 0);
}

/* (q) kind -> class (0 add, 1 sub, 2 logic, 3 shl, 4 shr, 5 sar, 6 inc,
   7 dec, 8 adc, 9 sbb, 10 szpc) and width */
static const uint8_t lz_class[LZ_COUNT] = {
    [LZ_ADD8] = 0, [LZ_ADD16] = 0, [LZ_SUB8] = 1, [LZ_SUB16] = 1,
    [LZ_LOG8] = 2, [LZ_LOG16] = 2, [LZ_INC8] = 6, [LZ_INC16] = 6, [LZ_DEC8] = 7, [LZ_DEC16] = 7,
    [LZ_SHL8] = 3, [LZ_SHL16] = 3, [LZ_SHR8] = 4, [LZ_SHR16] = 4, [LZ_SAR8] = 5, [LZ_SAR16] = 5,
    [LZ_ADC8] = 8, [LZ_ADC16] = 8, [LZ_SBB8] = 9, [LZ_SBB16] = 9, [LZ_SZPC8] = 10, [LZ_SZPC16] = 10,
};
static const uint8_t lz_w16[LZ_COUNT] = {
    [LZ_ADD16] = 1, [LZ_SUB16] = 1, [LZ_LOG16] = 1, [LZ_INC16] = 1, [LZ_DEC16] = 1,
    [LZ_SHL16] = 1, [LZ_SHR16] = 1, [LZ_SAR16] = 1, [LZ_ADC16] = 1, [LZ_SBB16] = 1, [LZ_SZPC16] = 1,
};

B86_HOT static uint32_t lazy_eval(uint32_t k, uint32_t a0, uint32_t r0, uint32_t b0, uint32_t f)
{
    unsigned cls = lz_class[k], w16 = lz_w16[k];
    uint32_t m = w16 ? 0xFFFFu : 0xFFu, sb = w16 ? 0x8000u : 0x80u;
    uint32_t a = a0 & m, r = r0 & m, b;
    uint32_t zsp = (r ? 0u : (uint32_t)B86_ZF) | ((w16 ? (r >> 8) : r) & 0x80u) | b86_parity[r & 0xFFu];
    uint32_t out;                                   /* CF | AF | OF bits */
    switch (cls) {
    case 0: case 6: case 8:                         /* add, inc, adc */
        b = cls == 8 ? (b0 & m) : ((r - a) & m);
        out = ((((a & b) | ((a | b) & ~r)) & sb) ? (uint32_t)B86_CF : 0u)
            | (((a ^ r) & (b ^ r) & sb) ? (uint32_t)B86_OF : 0u)
            | ((a ^ b ^ r) & (uint32_t)B86_AF);
        break;
    case 1: case 7: case 9:                         /* sub, dec, sbb */
        b = cls == 9 ? (b0 & m) : ((a - r) & m);
        out = ((((~a & b) | (~(a ^ b) & r)) & sb) ? (uint32_t)B86_CF : 0u)
            | (((a ^ b) & (a ^ r) & sb) ? (uint32_t)B86_OF : 0u)
            | ((a ^ b ^ r) & (uint32_t)B86_AF);
        break;
    case 3: { uint32_t cf = (a & sb) != 0;          /* shifts by 1 (8086 semantics) */
        out = (cf ? (uint32_t)B86_CF : 0u) | ((((r & sb) != 0) ^ cf) ? (uint32_t)B86_OF : 0u); break; }
    case 4: out = ((a & 1u) ? (uint32_t)B86_CF : 0u) | ((a & sb) ? (uint32_t)B86_OF : 0u); break;
    case 5: out = (a & 1u) ? (uint32_t)B86_CF : 0u; break;
    case 10: out = b0 & (uint32_t)(B86_CF | B86_OF); break;
    default: out = 0; break;                        /* logic */
    }
    if (cls == 6 || cls == 7) out = (out & ~(uint32_t)B86_CF) | (f & (uint32_t)B86_CF);   /* INC/DEC keep CF */
    return (f & ~(uint32_t)B86_ARITH) | out | zsp;
}

B86_HOT void b86_flags_materialize(B86Cpu *c)
{
    if (c->lz_kind != LZ_NONE) c->flags = lazy_eval(c->lz_kind, c->lz_a, c->lz_res, c->lz_b, c->flags);
    if (c->lz_ikind != LZ_NONE) c->flags = lazy_eval(c->lz_ikind, c->lz_ia, c->lz_ires, 0, c->flags);
    c->lz_kind = LZ_NONE;
    c->lz_ikind = LZ_NONE;
}

B86_HOT uint16_t b86_get_flags(B86Cpu *c)
{
    b86_flags_materialize(c);
    /* Stage 5C1: real-mode 386 FLAGS has IOPL and NT as well as the
     * 8086 arithmetic and control bits. Earlier CPU models are unchanged. */
    if (c->model >= B86_CPU_386)
        return (uint16_t)((c->flags & 0x7FD5u) | 0x0002u);
    return (uint16_t)((c->flags & 0x0FD5u) | (c->model >= B86_CPU_286 ? 0x0002u : 0xF002u));
}

B86_HOT void b86_set_flags(B86Cpu *c, uint16_t f)
{
    c->lz_kind = LZ_NONE;
    c->lz_ikind = LZ_NONE;
    if (c->model >= B86_CPU_386) {
        c->flags = (c->flags & 0xFFFF0000u) | (f & 0x7FD5u) | 0x0002u;
        return;
    }
    c->flags = (f & 0x0FD5u) | 0xF002u;
}

/* ------------------------------------------------------------------------ */
/* Memory                                                                   */
/* ------------------------------------------------------------------------ */

B86_HOT static inline __attribute__((always_inline)) uint32_t lin(const B86Cpu *c, int s, uint32_t off)
{
    return ((c->seg[s] << 4) + (off & 0xFFFFu)) & c->amask;
}

B86_HOT static inline void smc(B86Cpu *c, uint32_t a, uint32_t n)
{
    if (c->codemap && (c->codemap[a >> B86_LINE_SHIFT] |
                       c->codemap[(a + n - 1) >> B86_LINE_SHIFT]) && c->smc_hook)
        c->smc_hook(c, a, n);
}

#ifdef B86_MEMTRACE
extern void b86_memtrace(uint32_t a, int write);
#define MT(a, w) b86_memtrace((a), (w))
#else
#define MT(a, w) ((void)0)
#endif
B86_HOT static inline uint8_t rd8(B86Cpu *c, int s, uint32_t off) { uint32_t a = lin(c, s, off); if (s != B86_CS) MT(a, 0); return *b86_host(c, a); }
B86_HOT static inline void wr8(B86Cpu *c, int s, uint32_t off, uint8_t v)
{
    uint32_t a = lin(c, s, off);
    MT(a, 1);
    *b86_host(c, a) = v;
    smc(c, a, 1);
}
B86_HOT static inline uint16_t rd16(B86Cpu *c, int s, uint32_t off)
{
    if (c->exact_wrap)
        return (uint16_t)(rd8(c, s, off) | (rd8(c, s, off + 1) << 8));
    uint32_t a = lin(c, s, off);
    if (s != B86_CS) MT(a, 0);
    const uint8_t *h = b86_host(c, a);
    if ((a & 0xFFFu) != 0xFFFu) return (uint16_t)(h[0] | (h[1] << 8));   /* same 4 KiB page */
    return (uint16_t)(h[0] | (*b86_host(c, a + 1) << 8));
}
B86_HOT static inline void wr16(B86Cpu *c, int s, uint32_t off, uint16_t v)
{
    if (c->exact_wrap) { wr8(c, s, off, (uint8_t)v); wr8(c, s, off + 1, (uint8_t)(v >> 8)); return; }
    uint32_t a = lin(c, s, off);
    MT(a, 1);
    uint8_t *h = b86_host(c, a);
    h[0] = (uint8_t)v;
    if ((a & 0xFFFu) != 0xFFFu) h[1] = (uint8_t)(v >> 8);              /* same 4 KiB page */
    else *b86_host(c, a + 1) = (uint8_t)(v >> 8);
    smc(c, a, 2);
}

/* ------------------------------------------------------------------------ */
/* Per-instruction decode state                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    B86Cpu *c;
    uint16_t ip;        /* running fetch pointer */
    int seg;            /* override or -1 */
    int rep;            /* 0, 0xF2, 0xF3 */
    /* ModR/M */
    uint8_t mod, reg, rm;
    int ea_seg;
    uint16_t ea_off;
    const uint8_t *fp;          /* fetch window: host bytes at CS:fip0 */
    uint16_t fip0, favail;
} St;
/* BB_386_STAGE5C4C_V1: 386-only bridge to the existing bounded #UD path.
 * Keep the earlier 186/286 b86_interrupt behavior unchanged. */
static __attribute__((noinline, cold, section(".text.bb386_flash")))
int b86_rm5c2_fault(St *s, unsigned vector);

/* divide error: the 8086 pushes the next IP, the 286 the faulting instruction */
#define DE_IP(s) ((s)->c->model >= B86_CPU_286 ? (s)->fip0 : (s)->ip)

/* (r51c) instruction fetch from a host window set up once per instruction
   (CS:IP up to the end of its 4 KiB page / the segment / the A20 wrap),
   instead of lin()+page-table per byte */
B86_HOT static uint8_t f8_slow(St *s) { uint8_t v = rd8(s->c, B86_CS, s->ip); s->ip++; return v; }
B86_HOT static inline __attribute__((always_inline)) uint8_t f8(St *s)
{
    uint32_t i = (uint16_t)(s->ip - s->fip0);
    if (__builtin_expect(i < s->favail, 1)) { s->ip++; return s->fp[i]; }
    return f8_slow(s);
}
B86_HOT static inline uint16_t f16(St *s) { uint16_t lo = f8(s); return (uint16_t)(lo | (f8(s) << 8)); }

B86_HOT static void modrm(St *s)
{
    uint8_t m = f8(s);
    s->mod = m >> 6; s->reg = (m >> 3) & 7; s->rm = m & 7;
    /* register forms of memory-only instructions (LDS/LES/LEA/far FF) are
       undefined on the 8086; give them a deterministic DS:0 */
    s->ea_seg = s->seg >= 0 ? s->seg : B86_DS;
    s->ea_off = 0;
    if (s->mod == 3) return;
    B86Cpu *c = s->c;
    uint16_t off = 0; int dseg = B86_DS;
    switch (s->rm) {
    case 0: off = (uint16_t)(c->r[B86_BX] + c->r[B86_SI]); break;
    case 1: off = (uint16_t)(c->r[B86_BX] + c->r[B86_DI]); break;
    case 2: off = (uint16_t)(c->r[B86_BP] + c->r[B86_SI]); dseg = B86_SS; break;
    case 3: off = (uint16_t)(c->r[B86_BP] + c->r[B86_DI]); dseg = B86_SS; break;
    case 4: off = (uint16_t)c->r[B86_SI]; break;
    case 5: off = (uint16_t)c->r[B86_DI]; break;
    case 6: if (s->mod == 0) off = 0; else { off = (uint16_t)c->r[B86_BP]; dseg = B86_SS; } break;
    case 7: off = (uint16_t)c->r[B86_BX]; break;
    }
    if (s->mod == 0 && s->rm == 6) off = f16(s);
    else if (s->mod == 1) off = (uint16_t)(off + (int8_t)f8(s));
    else if (s->mod == 2) off = (uint16_t)(off + f16(s));
    s->ea_off = off;
    s->ea_seg = s->seg >= 0 ? s->seg : dseg;
}

B86_HOT static inline __attribute__((always_inline)) uint8_t g8(B86Cpu *c, int r) { return (uint8_t)(r < 4 ? c->r[r] : c->r[r - 4] >> 8); }
B86_HOT static inline __attribute__((always_inline)) void p8(B86Cpu *c, int r, uint8_t v)
{
    if (r < 4) c->r[r] = (c->r[r] & 0xFF00u) | v;
    else c->r[r - 4] = (c->r[r - 4] & 0x00FFu) | ((uint32_t)v << 8);
}
B86_HOT static inline __attribute__((always_inline)) uint16_t g16(B86Cpu *c, int r) { return (uint16_t)c->r[r]; }
B86_HOT static inline __attribute__((always_inline)) void p16(B86Cpu *c, int r, uint16_t v) { c->r[r] = v; }

B86_HOT static inline uint32_t rm_get(St *s, int w)
{
    if (s->mod == 3) return w ? g16(s->c, s->rm) : g8(s->c, s->rm);
    return w ? rd16(s->c, s->ea_seg, s->ea_off) : rd8(s->c, s->ea_seg, s->ea_off);
}
B86_HOT static inline void rm_put(St *s, int w, uint32_t v)
{
    if (s->mod == 3) { if (w) p16(s->c, s->rm, (uint16_t)v); else p8(s->c, s->rm, (uint8_t)v); return; }
    if (w) wr16(s->c, s->ea_seg, s->ea_off, (uint16_t)v); else wr8(s->c, s->ea_seg, s->ea_off, (uint8_t)v);
}
B86_HOT static inline uint32_t reg_get(St *s, int w) { return w ? g16(s->c, s->reg) : g8(s->c, s->reg); }
B86_HOT static inline void reg_put(St *s, int w, uint32_t v)
{
    if (w) p16(s->c, s->reg, (uint16_t)v); else p8(s->c, s->reg, (uint8_t)v);
}

B86_HOT static inline void push(B86Cpu *c, uint16_t v)
{
    c->r[B86_SP] = (uint16_t)(c->r[B86_SP] - 2);
    wr16(c, B86_SS, c->r[B86_SP], v);
}
B86_HOT static inline uint16_t pop(B86Cpu *c)
{
    uint16_t v = rd16(c, B86_SS, c->r[B86_SP]);
    c->r[B86_SP] = (uint16_t)(c->r[B86_SP] + 2);
    return v;
}

/* ------------------------------------------------------------------------ */
/* Flags and ALU                                                            */
/* ------------------------------------------------------------------------ */

#define FL (c->flags)
B86_HOT static inline __attribute__((always_inline)) void setf(B86Cpu *c, uint32_t bit, int on) { if (on) FL |= bit; else FL &= ~bit; }

/* (p) branch-light flag computation: all six arithmetic flags in one
   expression (CF 0x001, PF 0x004, AF 0x010, ZF 0x040, SF 0x080, OF 0x800) */
B86_HOT static inline __attribute__((always_inline)) uint32_t zsp_bits(int w, uint32_t r)    /* r already masked */
{
    uint32_t sf = (w ? (r >> 8) : r) & 0x80u;                /* SF is bit 7 */
    return (r ? 0u : (uint32_t)B86_ZF) | sf | b86_parity[r & 0xFFu];
}
B86_HOT static inline void szp(B86Cpu *c, int w, uint32_t r)
{
    r &= w ? 0xFFFFu : 0xFFu;
    FL = (FL & ~(uint32_t)(B86_SF | B86_ZF | B86_PF)) | zsp_bits(w, r);
}

B86_HOT static uint32_t add_f(B86Cpu *c, int w, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu;                        /* OF: sign bit 7/15 -> bit 11 */
    uint32_t full = a + b + cin, r = full & m;
    uint32_t f = (full > m ? (uint32_t)B86_CF : 0u)
               | ((w ? (((a ^ r) & (b ^ r)) >> 4) : (((a ^ r) & (b ^ r)) << 4)) & (uint32_t)B86_OF)
               | ((a ^ b ^ r) & (uint32_t)B86_AF)
               | zsp_bits(w, r);
    FL = (FL & ~(uint32_t)B86_ARITH) | f;
    return r;
}
B86_HOT static uint32_t sub_f(B86Cpu *c, int w, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu;
    uint32_t full = a - b - cin, r = full & m;
    uint32_t f = ((full & ~m) ? (uint32_t)B86_CF : 0u)
               | ((w ? (((a ^ b) & (a ^ r)) >> 4) : (((a ^ b) & (a ^ r)) << 4)) & (uint32_t)B86_OF)
               | ((a ^ b ^ r) & (uint32_t)B86_AF)
               | zsp_bits(w, r);
    FL = (FL & ~(uint32_t)B86_ARITH) | f;
    return r;
}
B86_HOT static uint32_t log_f(B86Cpu *c, int w, uint32_t r)
{
    r &= w ? 0xFFFFu : 0xFFu;
    FL = (FL & ~(uint32_t)B86_ARITH) | zsp_bits(w, r);
    return r;
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns result; CMP
   result is not written by callers. */
B86_HOT static uint32_t alu(B86Cpu *c, int op, int w, uint32_t a, uint32_t b)
{
    switch (op) {
    case 0: return add_f(c, w, a, b, 0);
    case 1: return log_f(c, w, a | b);
    case 2: return add_f(c, w, a, b, FL & B86_CF);
    case 3: return sub_f(c, w, a, b, FL & B86_CF);
    case 4: return log_f(c, w, a & b);
    case 5: case 7: return sub_f(c, w, a, b, 0);
    default: return log_f(c, w, a ^ b);
    }
}

B86_HOT static uint32_t incdec(B86Cpu *c, int w, uint32_t a, int dec)
{
    uint32_t cf = FL & B86_CF;
    uint32_t r = dec ? sub_f(c, w, a, 1, 0) : add_f(c, w, a, 1, 0);
    FL = (FL & ~(uint32_t)B86_CF) | cf;
    return r;
}

/* Shifts/rotates, 8086 semantics: count is not masked. */
B86_HOT static uint32_t shift(B86Cpu *c, int op, int w, uint32_t v, unsigned n)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu, sb = w ? 0x8000u : 0x80u;
    unsigned bits = w ? 16 : 8;
    if (n == 0) return v;
    uint32_t cf = FL & B86_CF ? 1 : 0, of = 0;
    switch (op) {
    case 0: /* ROL */
        for (unsigned i = 0; i < n; ++i) { cf = (v & sb) ? 1 : 0; v = ((v << 1) | cf) & m; }
        of = ((v & sb) ? 1 : 0) ^ cf;
        break;
    case 1: /* ROR */
        for (unsigned i = 0; i < n; ++i) { cf = v & 1; v = (v >> 1) | (cf ? sb : 0); }
        of = ((v ^ (v << 1)) & sb) ? 1 : 0;
        break;
    case 2: /* RCL */
        for (unsigned i = 0; i < n; ++i) { uint32_t o = (v & sb) ? 1 : 0; v = ((v << 1) | cf) & m; cf = o; }
        of = ((v & sb) ? 1 : 0) ^ cf;
        break;
    case 3: /* RCR */
        for (unsigned i = 0; i < n; ++i) { uint32_t o = v & 1; v = (v >> 1) | (cf ? sb : 0); cf = o; }
        of = ((v ^ (v << 1)) & sb) ? 1 : 0;
        break;
    case 4: /* SHL */
    case 6: /* SETMO slot behaves as SHL here (undocumented, not validated) */
        for (unsigned i = 0; i < n; ++i) { cf = (v & sb) ? 1 : 0; v = (v << 1) & m; }
        of = ((v & sb) ? 1 : 0) ^ cf;
        break;
    case 5: /* SHR */
        for (unsigned i = 0; i < n; ++i) { of = (v & sb) ? 1 : 0; cf = v & 1; v >>= 1; }
        break;
    case 7: /* SAR */
        for (unsigned i = 0; i < n; ++i) { cf = v & 1; v = (v >> 1) | (v & sb); }
        of = 0;
        break;
    }
    (void)bits;
    setf(c, B86_CF, (int)cf);
    setf(c, B86_OF, (int)of);
    if (op >= 4) { szp(c, w, v); FL &= ~(uint32_t)B86_AF; }
    return v;
}

B86_HOT static int cond(B86Cpu *c, int cc)
{
    uint32_t f = FL;
    int r;
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

B86_HOT void b86_interrupt(B86Cpu *c, uint8_t v)
{
    b86_flags_materialize(c);
    if (c->int_hook && c->int_hook(c, v)) return;
    push(c, (uint16_t)((c->flags & 0x0FD5u) | (c->model >= B86_CPU_286 ? 0x0002u : 0xF002u)));
    FL &= ~(uint32_t)(B86_IF | B86_TF);
    push(c, (uint16_t)c->seg[B86_CS]);
    push(c, (uint16_t)c->ip);
    uint32_t vec = (uint32_t)v * 4u;
    if (c->model >= B86_CPU_286) vec = (c->idt_base + vec) & c->amask;   /* LIDT moves the real-mode IVT */
    c->ip = *b86_host(c, vec) | (*b86_host(c, vec + 1) << 8);
    b86_set_seg(c, B86_CS, (uint16_t)(*b86_host(c, vec + 2) | (*b86_host(c, vec + 3) << 8)));
}

/* ------------------------------------------------------------------------ */
/* String instructions                                                      */
/* ------------------------------------------------------------------------ */

B86_HOT static void string_op(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    int w = op & 1;
    int16_t d = (int16_t)((FL & B86_DF) ? -(1 << w) : (1 << w));
    int sseg = s->seg >= 0 ? s->seg : B86_DS;
    int rep = s->rep;
    for (;;) {
        if (rep && (uint16_t)c->r[B86_CX] == 0) break;
        uint16_t si = (uint16_t)c->r[B86_SI], di = (uint16_t)c->r[B86_DI];
        int stop = 0;
        switch (op & 0xFE) {
        case 0xA4: /* MOVS */
            if (w) wr16(c, B86_ES, di, rd16(c, sseg, si)); else wr8(c, B86_ES, di, rd8(c, sseg, si));
            c->r[B86_SI] = (uint16_t)(si + d); c->r[B86_DI] = (uint16_t)(di + d);
            break;
        case 0xA6: { /* CMPS */
            uint32_t a = w ? rd16(c, sseg, si) : rd8(c, sseg, si);
            uint32_t b = w ? rd16(c, B86_ES, di) : rd8(c, B86_ES, di);
            sub_f(c, w, a, b, 0);
            c->r[B86_SI] = (uint16_t)(si + d); c->r[B86_DI] = (uint16_t)(di + d);
            stop = 1;
            break; }
        case 0xAA: /* STOS */
            if (w) wr16(c, B86_ES, di, g16(c, B86_AX)); else wr8(c, B86_ES, di, g8(c, 0));
            c->r[B86_DI] = (uint16_t)(di + d);
            break;
        case 0xAC: /* LODS */
            if (w) p16(c, B86_AX, rd16(c, sseg, si)); else p8(c, 0, rd8(c, sseg, si));
            c->r[B86_SI] = (uint16_t)(si + d);
            break;
        case 0xAE: { /* SCAS */
            uint32_t b = w ? rd16(c, B86_ES, di) : rd8(c, B86_ES, di);
            sub_f(c, w, w ? g16(c, B86_AX) : g8(c, 0), b, 0);
            c->r[B86_DI] = (uint16_t)(di + d);
            stop = 1;
            break; }
        }
        if (!rep) break;
        c->r[B86_CX] = (uint16_t)(c->r[B86_CX] - 1);
        if (stop) {
            int zf = (FL & B86_ZF) != 0;
            if (rep == 0xF3 && !zf) break;
            if (rep == 0xF2 && zf) break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* MUL / DIV                                                                */
/* ------------------------------------------------------------------------ */

B86_HOT static int group3(St *s, int w)
{
    B86Cpu *c = s->c;
    uint32_t v = rm_get(s, w);
    switch (s->reg) {
    case 0: case 1: log_f(c, w, v & (w ? f16(s) : f8(s))); break;
    case 2: rm_put(s, w, ~v); break;
    case 3: rm_put(s, w, sub_f(c, w, 0, v, 0)); break;
    case 4: /* MUL */
        if (!w) {
            uint32_t r = (uint32_t)g8(c, 0) * v;
            p16(c, B86_AX, (uint16_t)r);
            setf(c, B86_CF | B86_OF, (r >> 8) != 0);
            szp(c, 0, r >> 8);
            setf(c, B86_ZF, 0);
        } else {
            uint32_t r = (uint32_t)g16(c, B86_AX) * v;
            p16(c, B86_AX, (uint16_t)r); p16(c, B86_DX, (uint16_t)(r >> 16));
            setf(c, B86_CF | B86_OF, (r >> 16) != 0);
            szp(c, 1, r >> 16);
            setf(c, B86_ZF, 0);
        }
        break;
    case 5: /* IMUL */
        if (!w) {
            int32_t r = (int32_t)(int8_t)g8(c, 0) * (int8_t)v;
            p16(c, B86_AX, (uint16_t)r);
            setf(c, B86_CF | B86_OF, r != (int8_t)r);
            szp(c, 0, (uint32_t)r >> 8);
            setf(c, B86_ZF, 0);
        } else {
            int32_t r = (int32_t)(int16_t)g16(c, B86_AX) * (int16_t)v;
            p16(c, B86_AX, (uint16_t)r); p16(c, B86_DX, (uint16_t)((uint32_t)r >> 16));
            setf(c, B86_CF | B86_OF, r != (int16_t)r);
            szp(c, 1, (uint32_t)r >> 16);
            setf(c, B86_ZF, 0);
        }
        break;
    case 6: /* DIV */
        if (!w) {
            uint32_t n = g16(c, B86_AX);
            if (v == 0 || n / v > 0xFF) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            p8(c, 0, (uint8_t)(n / v)); p8(c, 4, (uint8_t)(n % v));
        } else {
            uint32_t n = ((uint32_t)g16(c, B86_DX) << 16) | g16(c, B86_AX);
            if (v == 0 || n / v > 0xFFFF) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            p16(c, B86_AX, (uint16_t)(n / v)); p16(c, B86_DX, (uint16_t)(n % v));
        }
        break;
    case 7: /* IDIV */
        if (!w) {
            int32_t n = (int16_t)g16(c, B86_AX), d = (int8_t)v;
            if (d == 0) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            int32_t q = n / d, r = n % d;
            if (q < (s->c->model >= B86_CPU_286 ? -128 : -127) || q > 127) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            if (s->rep) q = -q;
            p8(c, 0, (uint8_t)q); p8(c, 4, (uint8_t)r);
        } else {
            int32_t n = (int32_t)(((uint32_t)g16(c, B86_DX) << 16) | g16(c, B86_AX)), d = (int16_t)v;
            if (d == 0) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            if (n == INT32_MIN && d == -1) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            int32_t q = n / d, r = n % d;
            if (q < (s->c->model >= B86_CPU_286 ? -32768 : -32767) || q > 32767) { if (c->model >= B86_CPU_386)
                return b86_rm5c2_fault(s, 0u) + 1;
            c->ip = DE_IP(s); b86_interrupt(c, 0); return 1; }
            if (s->rep) q = -q;
            p16(c, B86_AX, (uint16_t)q); p16(c, B86_DX, (uint16_t)r);
        }
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* BCD                                                                      */
/* ------------------------------------------------------------------------ */

static __attribute__((noinline, cold, section(".text.bb386_flash"))) void daa_das(B86Cpu *c, int sub)
{
    uint8_t al = g8(c, 0), old = al;
    int oaf = (FL & B86_AF) != 0, ocf = (FL & B86_CF) != 0, af = 0, cf = 0;
    if ((al & 0x0F) > 9 || oaf) {
        if (c->model >= B86_CPU_286 && (sub ? al < 6 : al > 0xF9)) cf = 1;   /* 286: borrow/carry of the +-6 */
        al = (uint8_t)(sub ? al - 6 : al + 6); af = 1;
    }
    if (ocf || (c->model >= B86_CPU_286 ? old > 0x99 : (old > 0x9F || (old > 0x99 && !oaf)))) { al = (uint8_t)(sub ? al - 0x60 : al + 0x60); cf = 1; }
    setf(c, B86_AF, af); setf(c, B86_CF, cf);
    p8(c, 0, al);
    szp(c, 0, al);
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) void aaa_aas(B86Cpu *c, int sub)
{
    uint8_t al = g8(c, 0), ah = g8(c, 4);
    if ((al & 0x0F) > 9 || (FL & B86_AF)) {
        if (c->model >= B86_CPU_286) {              /* 286: AX +/- 6 carries into AH */
            uint16_t ax = (uint16_t)(al | ah << 8);
            ax = (uint16_t)(sub ? ax - 6 : ax + 6);
            al = (uint8_t)ax; ah = (uint8_t)(ax >> 8);
        } else al = (uint8_t)(sub ? al - 6 : al + 6);
        ah = (uint8_t)(sub ? ah - 1 : ah + 1);
        FL |= B86_AF | B86_CF;
    } else FL &= ~(uint32_t)(B86_AF | B86_CF);
    p8(c, 0, al & 0x0F); p8(c, 4, ah);
}

/* ------------------------------------------------------------------------ */
/* The step function                                                        */
/* ------------------------------------------------------------------------ */

/* 80286 real mode: 0F xx. Protected-mode-only forms (and LMSW setting PE,
   which this emulator does not support) raise #UD (INT 6). */
static __attribute__((noinline, cold, section(".text.bb386_flash"))) int step0f(St *s)
{
    B86Cpu *c = s->c;
    uint8_t op2 = f8(s);
    if (op2 == 0x01) {
        modrm(s);
        int reg = s->reg;
        if (reg <= 3) {                             /* SGDT SIDT LGDT LIDT: memory only */
            if (s->mod == 3) goto ud;
            /* BB_386_STAGE5C4E_V1: all SIX bytes must fit before any write
             * or descriptor read. A 16-bit EA does not wrap mid-operand. */
            if (c->model >= B86_CPU_386 && s->ea_off > 0xFFFAu)
                return b86_rm5c2_fault(s, s->ea_seg == B86_SS ? 12u : 13u);
            uint16_t o = s->ea_off; int sg = s->ea_seg;
            uint32_t *base = (reg & 1) ? &c->idt_base : &c->gdt_base;
            uint16_t *lim = (reg & 1) ? &c->idt_limit : &c->gdt_limit;
            if (reg < 2) {
                wr16(c, sg, o, *lim);
                wr16(c, sg, (uint16_t)(o + 2), (uint16_t)*base);
                wr8(c, sg, (uint16_t)(o + 4), (uint8_t)(*base >> 16));
                wr8(c, sg, (uint16_t)(o + 5), 0xFF);          /* 286 stores FFh in the 6th byte */
            } else {
                *lim = rd16(c, sg, o);
                *base = rd16(c, sg, (uint16_t)(o + 2)) | (uint32_t)rd8(c, sg, (uint16_t)(o + 4)) << 16;
            }
        } else if (reg == 4) {                      /* SMSW */
            if (c->model >= B86_CPU_386 && s->mod != 3 && s->ea_off > 0xFFFEu)
                return b86_rm5c2_fault(s, s->ea_seg == B86_SS ? 12u : 13u);
            rm_put(s, 1, (uint16_t)(0xFFF0u | c->msw));
        } else if (reg == 6) {                      /* LMSW: PE can be set, not cleared */
            if (c->model >= B86_CPU_386 && s->mod != 3 && s->ea_off > 0xFFFEu)
                return b86_rm5c2_fault(s, s->ea_seg == B86_SS ? 12u : 13u);
            uint16_t v = (uint16_t)rm_get(s, 1);
            /* Setting PE requests a mode transition this engine cannot perform.
             * Do not fabricate #UD for an architecturally valid 386 LMSW. */
            if (v & 1u) {
                if (c->model >= B86_CPU_386) {
                    c->ip = s->fip0;
                    return B86_UNSUPPORTED;
                }
                goto ud;
            }
            c->msw = (uint16_t)((c->msw & 1u) | (v & 0x0Eu));
        } else goto ud;
        c->ip = s->ip;
        return B86_OK;
    }
    if (op2 == 0x06) { c->msw &= (uint16_t)~8u; c->ip = s->ip; return B86_OK; }   /* CLTS */
ud:
    if (c->model >= B86_CPU_386) return b86_rm5c2_fault(s, 6u);
    c->ip = s->fip0;
    b86_interrupt(c, 6);
    return B86_OK;
}

/* 80186/80188 additions (model >= B86_CPU_186). Returns -1 when op is not
   one of them (the 8086 table handles it), else the b86_step result. */
B86_HOT static int step186(St *s, uint8_t op, int w)
{
    B86Cpu *c = s->c;
    switch (op) {
    case 0x60: {                                    /* PUSHA */
        uint16_t sp = (uint16_t)c->r[B86_SP];
        push(c, g16(c, B86_AX)); push(c, g16(c, B86_CX)); push(c, g16(c, B86_DX)); push(c, g16(c, B86_BX));
        push(c, sp); push(c, g16(c, B86_BP)); push(c, g16(c, B86_SI)); push(c, g16(c, B86_DI));
        break; }
    case 0x61:                                      /* POPA (the SP slot is skipped) */
        p16(c, B86_DI, pop(c)); p16(c, B86_SI, pop(c)); p16(c, B86_BP, pop(c)); (void)pop(c);
        p16(c, B86_BX, pop(c)); p16(c, B86_DX, pop(c)); p16(c, B86_CX, pop(c)); p16(c, B86_AX, pop(c));
        break;
    case 0x62: {                                    /* BOUND r16, m16&16 -> INT 5 */
        modrm(s);
        if (s->mod == 3) {
            if (c->model >= B86_CPU_386) return b86_rm5c2_fault(s, 6u);
            c->ip = s->fip0;
            b86_interrupt(c, 6);
            return B86_OK;
        }
        /* A boundary fault takes priority over the BOUND range comparison.
         * The m16&16 pair is ONE four-byte memory operand. */
        if (c->model >= B86_CPU_386 && s->ea_off > 0xFFFCu)
            return b86_rm5c2_fault(s, s->ea_seg == B86_SS ? 12u : 13u);
        int16_t v = (int16_t)reg_get(s, 1);
        int16_t lo = (int16_t)rd16(c, s->ea_seg, s->ea_off), hi = (int16_t)rd16(c, s->ea_seg, (uint16_t)(s->ea_off + 2));
        if (v < lo || v > hi) {
            if (c->model >= B86_CPU_386) return b86_rm5c2_fault(s, 5u);
            c->ip = s->fip0; b86_interrupt(c, 5); return B86_OK;
        }   /* IP of the first prefix */
        break; }
    case 0x68: push(c, f16(s)); break;              /* PUSH imm16 */
    case 0x6A: push(c, (uint16_t)(int16_t)(int8_t)f8(s)); break;   /* PUSH imm8 (sign-extended) */
    case 0x69: case 0x6B: {                         /* IMUL r16, r/m16, imm */
        modrm(s);
        int32_t a = (int16_t)rm_get(s, 1);
        int32_t b = op == 0x69 ? (int16_t)f16(s) : (int8_t)f8(s);
        int32_t r = a * b;
        reg_put(s, 1, (uint16_t)r);
        setf(c, B86_CF | B86_OF, r != (int16_t)r);
        szp(c, 1, (uint16_t)r);
        break; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {   /* INS / OUTS */
        int16_t d = (int16_t)((FL & B86_DF) ? -(1 << w) : (1 << w));
        int sseg = s->seg >= 0 ? s->seg : B86_DS;
        uint16_t port = g16(c, B86_DX);
        for (;;) {
            if (s->rep && (uint16_t)c->r[B86_CX] == 0) break;
            if (op < 0x6E) {
                uint16_t di = (uint16_t)c->r[B86_DI];
                uint8_t lo = c->in8 ? c->in8(c, port) : 0xFF;
                if (w) wr16(c, B86_ES, di, (uint16_t)(lo | (c->in8 ? c->in8(c, (uint16_t)(port + 1)) : 0xFF) << 8));
                else wr8(c, B86_ES, di, lo);
                c->r[B86_DI] = (uint16_t)(di + d);
            } else {
                uint16_t si = (uint16_t)c->r[B86_SI];
                uint16_t v = w ? rd16(c, sseg, si) : rd8(c, sseg, si);
                if (c->out8) { c->out8(c, port, (uint8_t)v); if (w) c->out8(c, (uint16_t)(port + 1), (uint8_t)(v >> 8)); }
                c->r[B86_SI] = (uint16_t)(si + d);
            }
            if (!s->rep) break;
            c->r[B86_CX] = (uint16_t)(c->r[B86_CX] - 1);
        }
        break; }
    case 0xC0: case 0xC1: {                         /* shift/rotate r/m, imm8 */
        modrm(s);
        uint32_t v = rm_get(s, w);
        unsigned n = f8(s) & 31u;
        uint32_t r = shift(c, s->reg, w, v, n);
        if (n) rm_put(s, w, r);
        break; }
    case 0xC8: {                                    /* ENTER size, level */
        uint16_t size = f16(s);
        unsigned level = f8(s) & 31u;
        push(c, g16(c, B86_BP));
        uint16_t frame = (uint16_t)c->r[B86_SP];
        if (level) {
            for (unsigned i = 1; i < level; ++i) {
                p16(c, B86_BP, (uint16_t)(g16(c, B86_BP) - 2));
                push(c, rd16(c, B86_SS, g16(c, B86_BP)));
            }
            push(c, frame);
        }
        p16(c, B86_BP, frame);
        c->r[B86_SP] = (uint16_t)(c->r[B86_SP] - size);
        break; }
    case 0x0F:                                      /* 286: two-byte opcodes */
        if (c->model < B86_CPU_286) { c->ip = s->fip0; b86_interrupt(c, 6); return B86_OK; }   /* 186: #UD */
        return step0f(s);
    case 0xC9:                                      /* LEAVE */
        c->r[B86_SP] = (uint16_t)c->r[B86_BP];
        p16(c, B86_BP, pop(c));
        break;
    default:
        return -1;
    }
    c->ip = s->ip;
    return B86_OK;
}

/* BB_386_RM0_V1: 386 66h operand path stays in flash. */
#include "i386_rm0.inc"
/* BB_386_RM1_V1: 67h address-size interpreter, flash only. */
/* BB_386_RM3_V1: forward selector accessor for RM1 memory helpers. */
static uint32_t b86_rm3_segvalue(B86Cpu *c, int which);
#include "i386_rm1.inc"
/* BB_386_RM2_V1: 80386 real-mode extended 0F decoder, flash-only. */
#include "i386_rm2.inc"
/* BB_386_RM3_V1: FS/GS and 32-bit near flow, XIP flash. */
#include "i386_rm3.inc"
/* BB_386_PM0_V1: GDT descriptor helpers in flash, no PE switch. */
#include "i386_pm0.inc"
/* BB_386_STAGE5C5C_V1: shadow cache + checked address helpers, inert in RM. */
#include "i386_pm1.inc"
/* BB_386_STAGE5C5D_V1: transactional protected selector loaders, PE blocked. */
#include "i386_pm2.inc"
/* BB_386_STAGE5C5E_V1: first PE transition, one far JMP, then fail-closed. */
#include "i386_pm3.inc"
/* 5C5F: bounded post-far-JMP descriptor-backed instruction execution */
#include "i386_pm4.inc"
/* 5C5G cold protected-mode ALU/branch subset */
#include "i386_pm5.inc"
/* 5C5H: CPL0 descriptor-backed ModR/M */
#include "i386_pm6.inc"
/* Stage5C5I: protected CPL0 near stack/flow */
#include "i386_pm7.inc"
/* Stage5C5J: protected indirect near ModR/M flow */
#include "i386_pm8.inc"
/* Stage5C5K: protected selectors, LEA and XCHG */
#include "i386_pm9.inc"
/* Stage5C5L: protected direct far transfers */
#include "i386_pm10.inc"
/* Stage5C5M: SS load shadow */
#include "i386_pm11.inc"
/* Stage5C5N: software protected IDT */
#include "i386_pm12.inc"
/* Stage5C5O: CPU faults with IDT */
#include "i386_pm13.inc"
/* Stage5C5P: contributory faults + DF */
#include "i386_pm14.inc"
/* Stage5C5Q: #DE / #BR */
#include "i386_pm15.inc"
#include "i386_pm16.inc"
#include "i386_pm17.inc"
#include "i386_pm18.inc"
#include "i386_pm19.inc"
#include "i386_pm20.inc"

/* BB_386_STAGE5C3C_V1: model-386-only legacy word memory limit preflight. */
/* The 8086/186/286 interpreter and JIT memory primitives are intentionally
 * unchanged. This runs only for unprefixed-operand-size, 16-bit EA 386 paths
 * that fall through the 386-specific RM3/RM2/RM1/RM0 decoders.
 * A shadow St decodes ModRM without committing IP or any guest state.
 * Check whole memory operands before legacy code performs *any* reads/writes.
 * This is not the string/REP, I/O string, instruction-fetch, or stack pass. */
static __attribute__((noinline, cold, section(".text.bb386_flash")))
int b86_rm5c3c_preflight16(St *s, uint8_t op)
{
    St shadow = *s;
    B86Cpu *c = s->c;
    unsigned bytes = 0;
    uint32_t off;
    int seg;
    int use_modrm = 0;
    (void)c;

    if (op >= 0xA0u && op <= 0xA3u) {
        off = f16(&shadow);
        seg = s->seg >= 0 ? s->seg : B86_DS;
        bytes = (op & 1u) ? 2u : 1u;
    } else {
        if ((op < 0x40u && (op & 7u) <= 3u) ||
            op == 0x80u || op == 0x81u || op == 0x83u ||
            (op >= 0x84u && op <= 0x8Bu) || op == 0x8Fu ||
            op == 0x62u || op == 0x69u || op == 0x6Bu ||
            op == 0xC4u || op == 0xC5u || op == 0xC6u || op == 0xC7u ||
            op == 0xF6u || op == 0xF7u || op == 0xFEu || op == 0xFFu)
            use_modrm = 1;
        if (!use_modrm) return -1;

        modrm(&shadow);
        /* A register-only operand or invalid encoding never accesses data
         * memory. Let the original interpreter handle register and #UD cases. */
        if (shadow.mod == 3u) return -1;
        if ((op == 0xC6u || op == 0xC7u || op == 0x8Fu) && shadow.reg) return -1;
        if ((op == 0xF6u || op == 0xF7u) && shadow.reg == 1u) return -1;
        if (op == 0xFEu && shadow.reg > 1u) return -1;
        if (op == 0xFFu && shadow.reg == 7u) return -1;
        off = shadow.ea_off;
        seg = shadow.ea_seg;
        if (op == 0x62u || op == 0xC4u || op == 0xC5u ||
            (op == 0xFFu && (shadow.reg == 3u || shadow.reg == 5u)))
            bytes = 4u; /* two 16-bit words: BOUND, LES/LDS, far pointer */
        else if (op == 0x80u || op == 0x84u || op == 0x86u ||
                 op == 0x88u || op == 0x8Au || op == 0xC6u ||
                 op == 0xF6u || op == 0xFEu ||
                 (op < 0x40u && !(op & 1u)))
            bytes = 1u;
        else
            bytes = 2u;
    }
    if (off <= 0x10000u - bytes) return -1;
    return b86_rm5c2_fault(s, seg == B86_SS ? 12u : 13u);
}

/* BB_386_STAGE5C3D_V1: real-mode 386 string/REP engine. */
/* Only invoked by model 386; the legacy 8086/286 and JIT paths remain intact.
 * 67h selects ESI, EDI, ECX; otherwise SI, DI, CX. 66h selects DWORD
 * elements for non-byte string opcodes, without changing stack size.
 * Preflight BOTH memory operands before the first read, write, or port I/O
 * of each iteration. Completed REP iterations remain committed on #GP/#SS. */
static __attribute__((noinline, cold, section(".text.bb386_flash")))
int b86_rm5c3d_string(St *s, uint8_t op, int operand32, int address32)
{
    B86Cpu *c = s->c;
    const int repeated = s->rep == 0xF2 || s->rep == 0xF3;
    const unsigned bytes = (op & 1u) ? (operand32 ? 4u : 2u) : 1u;
    const unsigned width = bytes == 4u ? 2u : bytes == 2u ? 1u : 0u;
    const int uses_src = op == 0xA4 || op == 0xA5 ||
                         op == 0xA6 || op == 0xA7 ||
                         op == 0xAC || op == 0xAD ||
                         op == 0x6E || op == 0x6F;
    const int uses_dst = op == 0xA4 || op == 0xA5 ||
                         op == 0xA6 || op == 0xA7 ||
                         op == 0xAA || op == 0xAB ||
                         op == 0xAE || op == 0xAF ||
                         op == 0x6C || op == 0x6D;
    const int source_seg = s->seg >= 0 ? s->seg : B86_DS;
    const uint16_t port = (uint16_t)c->r[B86_DX];
    const uint32_t delta = (c->flags & B86_DF) ? (uint32_t)(0u - bytes) : bytes;
    uint32_t remaining = repeated ? (address32 ? c->r[B86_CX] : (uint16_t)c->r[B86_CX]) : 1u;

    /* REP with zero count does no data/I/O access, even if SI/DI are invalid. */
    while (remaining) {
        const uint32_t si = address32 ? c->r[B86_SI] : (uint16_t)c->r[B86_SI];
        const uint32_t di = address32 ? c->r[B86_DI] : (uint16_t)c->r[B86_DI];
        B86RM1EA src, dst;
        uint32_t a=0u, b=0u;
        unsigned i;
        memset(&src, 0, sizeof src);
        memset(&dst, 0, sizeof dst);
        src.seg = source_seg; src.off = si;
        dst.seg = B86_ES; dst.off = di;
        if (uses_src && !b86_rm1_memok(&src, bytes))
            return b86_rm5c2_fault(s, source_seg == B86_SS ? 12u : 13u);
        if (uses_dst && !b86_rm1_memok(&dst, bytes))
            return b86_rm5c2_fault(s, 13u); /* string destination is always ES */

        switch (op) {
        case 0xA4: case 0xA5: /* MOVS */
            a = b86_rm1_read(c, &src, bytes);
            b86_rm1_write(c, &dst, bytes, a);
            break;
        case 0xA6: case 0xA7: /* CMPS: source - ES:destination */
            a = b86_rm1_read(c, &src, bytes);
            b = b86_rm1_read(c, &dst, bytes);
            if (bytes == 4u) (void)b86_rm0_alu(c, 5, a, b);
            else (void)sub_f(c, bytes == 2u, a, b, 0u);
            break;
        case 0xAA: case 0xAB: /* STOS */
            b86_rm1_write(c, &dst, bytes, b86_rm1_regget(c, B86_AX, width));
            break;
        case 0xAC: case 0xAD: /* LODS */
            b86_rm1_regput(c, B86_AX, width, b86_rm1_read(c, &src, bytes));
            break;
        case 0xAE: case 0xAF: /* SCAS: accumulator - ES:destination */
            b = b86_rm1_read(c, &dst, bytes);
            a = b86_rm1_regget(c, B86_AX, width);
            if (bytes == 4u) (void)b86_rm0_alu(c, 5, a, b);
            else (void)sub_f(c, bytes == 2u, a, b, 0u);
            break;
        case 0x6C: case 0x6D: /* INS; destination preflight before port read */
            for (i=0; i<bytes; ++i)
                a |= (uint32_t)(c->in8 ? c->in8(c, (uint16_t)(port+i)) : 0xFFu) << (8u*i);
            b86_rm1_write(c, &dst, bytes, a);
            break;
        case 0x6E: case 0x6F: /* OUTS; source preflight before port write */
            a = b86_rm1_read(c, &src, bytes);
            if (c->out8) for (i=0; i<bytes; ++i)
                c->out8(c, (uint16_t)(port+i), (uint8_t)(a >> (8u*i)));
            break;
        default: /* dispatch guarantees only supported string opcodes */
            c->ip = s->fip0;
            return B86_UNSUPPORTED;
        }

        /* Index updates commit after the completed iteration, even on a
         * subsequent fault. 16-bit addressing preserves the upper half. */
        if (uses_src) {
            if (address32) c->r[B86_SI] += delta;
            else c->r[B86_SI] = (c->r[B86_SI] & 0xFFFF0000u) |
                 (uint16_t)((uint16_t)c->r[B86_SI] + delta);
        }
        if (uses_dst) {
            if (address32) c->r[B86_DI] += delta;
            else c->r[B86_DI] = (c->r[B86_DI] & 0xFFFF0000u) |
                 (uint16_t)((uint16_t)c->r[B86_DI] + delta);
        }
        --remaining;
        if (repeated) {
            if (address32) c->r[B86_CX] = remaining;
            else c->r[B86_CX] = (c->r[B86_CX] & 0xFFFF0000u) | (uint16_t)remaining;
        }
        if (!repeated) break;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            const int equal = (c->flags & B86_ZF) != 0;
            if (s->rep == 0xF3 && !equal) break;
            if (s->rep == 0xF2 && equal) break;
        }
        /* Permit a host interrupt request between completed REP iterations.
         * Prefix IP and remaining ECX/CX make the instruction restartable. */
        if (remaining && c->irq) { c->ip = s->fip0; return B86_OK; }
    }
    c->ip = s->ip;
    return B86_OK;
}

B86_HOT static int b86_step_internal(B86Cpu *c)
{
    St st, *s = &st;
    if (c->lz_kind != LZ_NONE || c->lz_ikind != LZ_NONE) b86_flags_materialize(c);   /* usually none pending */
    s->c = c; s->ip = (uint16_t)c->ip; s->seg = -1; s->rep = 0;
    {   /* fetch window (f8): never crosses the 4 KiB mapping granule, the
           64 KiB segment end, or the A20 wrap; 16 bytes covers any 8086 insn */
        uint32_t a0 = lin(c, B86_CS, s->ip);
        uint32_t n = 4096u - (a0 & 4095u), t;
        t = 0x10000u - s->ip; if (t < n) n = t;
        t = c->amask + 1u - a0; if (t < n) n = t;
        s->fp = b86_host(c, a0); s->fip0 = s->ip; s->favail = (uint16_t)(n < 16u ? n : 16u);
    }
    uint8_t op;
    int operand32 = 0, address32 = 0;  /* BB_386_RM0_V1 */
    for (;;) {
        op = f8(s);
        if (c->model >= B86_CPU_386 && op == 0x66) { operand32 = 1; continue; }
        if (c->model >= B86_CPU_386 && op == 0x67) { address32 = 1; continue; }
        if (c->model >= B86_CPU_386 && (op == 0x64 || op == 0x65)) {
            s->seg = op == 0x64 ? B86_FS : B86_GS; continue;
        }
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) { s->seg = (op >> 3) & 3; continue; }
        if (op == 0xF2 || op == 0xF3) { s->rep = op; continue; }
        if (op == 0xF0 || op == 0xF1) continue;
        break;
    }
    c->step_op = op; c->step_oip = (uint16_t)(s->ip - 1u);   /* for cold runs (jit.c) */
    c->icount++;
    int w = op & 1;
    if (c->model >= B86_CPU_386) {
        if ((op >= 0xA4 && op <= 0xA7) ||
            (op >= 0xAA && op <= 0xAF) ||
            (op >= 0x6C && op <= 0x6F))
            return b86_rm5c3d_string(s, op, operand32, address32);
        /* BB_386_RM3_V1: intercept only new 386 encodings. */
        { int r3 = b86_rm3_dispatch(s, op, operand32, address32);
          if (r3 >= 0) return r3; }
        /* RM2: 386-only 0F opcodes. The 286 0F 01/06 cases stay on
           their existing descriptor/MSW/CLTS interpreter path. */
        if (op == 0x0F) {
            uint8_t op2 = rd8(c, B86_CS, s->ip);
            if (op2 != 0x01 && op2 != 0x06)
                return b86_rm2_0f(s, operand32, address32);
        }
        if (address32) return b86_rm1_67(s, op, operand32); /* 67h with optional 66h */
        if (op == 0x63 || op == 0x64 || op == 0x65 ||
            (op == 0x0F && !operand32 &&
             rd8(c, B86_CS, s->ip) != 0x01 && rd8(c, B86_CS, s->ip) != 0x06)) {
            c->ip = s->fip0; return B86_UNSUPPORTED;
        }
        if (operand32) return b86_rm0_66(s, op);
        /* Existing preflighted 386 decoders already returned above. */
        { int fault = b86_rm5c3c_preflight16(s, op);
          if (fault >= 0) return fault; }
    }

    if (op < 0x40 && (op & 7) < 6) {      /* ALU r/m,r  r,r/m  acc,imm */
        int aop = op >> 3;
        switch (op & 7) {
        case 0: case 1: { modrm(s); uint32_t r = alu(c, aop, w, rm_get(s, w), reg_get(s, w)); if (aop != 7) rm_put(s, w, r); break; }
        case 2: case 3: { modrm(s); uint32_t r = alu(c, aop, w, reg_get(s, w), rm_get(s, w)); if (aop != 7) reg_put(s, w, r); break; }
        case 4: { uint32_t r = alu(c, aop, 0, g8(c, 0), f8(s)); if (aop != 7) p8(c, 0, (uint8_t)r); break; }
        case 5: { uint32_t r = alu(c, aop, 1, g16(c, 0), f16(s)); if (aop != 7) p16(c, 0, (uint16_t)r); break; }
        }
        c->ip = s->ip;
        return B86_OK;
    }

    if (c->model >= B86_CPU_286 && (op == 0x8C || op == 0x8E || op == 0x8D || op == 0xC4 || op == 0xC5 ||
                                     op == 0x8F || op == 0xC6 || op == 0xC7 || op == 0xFF || op == 0xFE)) {
        uint8_t m = *b86_host(c, lin(c, B86_CS, s->ip));
        int mod = m >> 6, reg = (m >> 3) & 7, ud;
        switch (op) {
        case 0x8C: ud = reg > 3; break;
        case 0x8E: ud = reg > 3 || reg == B86_CS; break;
        case 0x8D: case 0xC4: case 0xC5: ud = mod == 3; break;
        case 0x8F: case 0xC6: case 0xC7: ud = reg != 0; break;
        case 0xFE: ud = reg > 1; break;
        default:   ud = reg == 7 || ((reg == 3 || reg == 5) && mod == 3); break;   /* FF */
        }
        if (ud) {
            if (c->model >= B86_CPU_386) return b86_rm5c2_fault(s, 6u);
            c->ip = s->fip0;
            b86_interrupt(c, 6);
            return B86_OK;
        }
    }
    if (c->model >= B86_CPU_186) {
        int r = step186(s, op, w);
        if (r >= 0) return r;
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: push(c, (uint16_t)c->seg[op >> 3]); break;
    case 0x07: case 0x0F: case 0x17: case 0x1F: b86_set_seg(c, op >> 3, pop(c)); break;
    case 0x27: daa_das(c, 0); break;
    case 0x2F: daa_das(c, 1); break;
    case 0x37: aaa_aas(c, 0); break;
    case 0x3F: aaa_aas(c, 1); break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        p16(c, op & 7, (uint16_t)incdec(c, 1, g16(c, op & 7), 0)); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        p16(c, op & 7, (uint16_t)incdec(c, 1, g16(c, op & 7), 1)); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
        push(c, g16(c, op & 7)); break;
    case 0x54: /* 8086 PUSH SP pushes the decremented value; 286 the old one */
        if (c->model >= B86_CPU_286) { push(c, (uint16_t)c->r[B86_SP]); break; }
        c->r[B86_SP] = (uint16_t)(c->r[B86_SP] - 2);
        wr16(c, B86_SS, c->r[B86_SP], (uint16_t)c->r[B86_SP]);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint16_t v = pop(c); p16(c, op & 7, v); break; }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)f8(s);
        if (cond(c, op & 15)) s->ip = (uint16_t)(s->ip + d);
        break; }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        modrm(s);
        uint32_t a = rm_get(s, w);
        uint32_t b = (op == 0x81) ? f16(s) : (op == 0x83) ? (uint16_t)(int8_t)f8(s) : f8(s);
        uint32_t r = alu(c, s->reg, w, a, b);
        if (s->reg != 7) rm_put(s, w, r);
        break; }
    case 0x84: case 0x85: modrm(s); log_f(c, w, rm_get(s, w) & reg_get(s, w)); break;
    case 0x86: case 0x87: { modrm(s); uint32_t a = rm_get(s, w), b = reg_get(s, w); rm_put(s, w, b); reg_put(s, w, a); break; }
    case 0x88: case 0x89: modrm(s); rm_put(s, w, reg_get(s, w)); break;
    case 0x8A: case 0x8B: modrm(s); reg_put(s, w, rm_get(s, w)); break;
    case 0x8C: modrm(s); rm_put(s, 1, c->seg[s->reg & 3]); break;
    case 0x8D: modrm(s);
        if (s->mod != 3) p16(c, s->reg, s->ea_off);
        break;
    case 0x8E: modrm(s); b86_set_seg(c, s->reg & 3, (uint16_t)rm_get(s, 1)); break;
    case 0x8F: { modrm(s); uint16_t v = pop(c);
        if (s->mod != 3) { /* EA uses SP after the pop on 8086 */ }
        rm_put(s, 1, v); break; }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = g16(c, 0); p16(c, 0, g16(c, op & 7)); p16(c, op & 7, t); break; }
    case 0x98: p16(c, 0, (uint16_t)(int8_t)g8(c, 0)); break;
    case 0x99: p16(c, B86_DX, (g16(c, 0) & 0x8000) ? 0xFFFF : 0); break;
    case 0x9A: { uint16_t ip = f16(s), cs = f16(s);
        push(c, (uint16_t)c->seg[B86_CS]); push(c, s->ip);
        b86_set_seg(c, B86_CS, cs); s->ip = ip; break; }
    case 0x9B: break;
    case 0x9C: push(c, (uint16_t)((FL & 0x0FD5u) | (c->model >= B86_CPU_286 ? 0x0002u : 0xF002u))); break;
    case 0x9D: FL = (pop(c) & 0x0FD5u) | 0xF002u; break;
    case 0x9E: FL = (FL & ~0xFFu) | (g8(c, 4) & 0xD5u) | 2u; break;
    case 0x9F: p8(c, 4, (uint8_t)((FL & 0xD5u) | 2u)); break;
    case 0xA0: case 0xA1: { uint16_t o = f16(s); int sg = s->seg >= 0 ? s->seg : B86_DS;
        if (w) p16(c, 0, rd16(c, sg, o)); else p8(c, 0, rd8(c, sg, o)); break; }
    case 0xA2: case 0xA3: { uint16_t o = f16(s); int sg = s->seg >= 0 ? s->seg : B86_DS;
        if (w) wr16(c, sg, o, g16(c, 0)); else wr8(c, sg, o, g8(c, 0)); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(s, op); break;
    case 0xA8: log_f(c, 0, g8(c, 0) & f8(s)); break;
    case 0xA9: log_f(c, 1, g16(c, 0) & f16(s)); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        p8(c, op & 7, f8(s)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        p16(c, op & 7, f16(s)); break;
    case 0xC0: case 0xC2: { uint16_t n = f16(s); s->ip = pop(c); c->r[B86_SP] = (uint16_t)(c->r[B86_SP] + n); break; }
    case 0xC1: case 0xC3: s->ip = pop(c); break;
    case 0xC4: case 0xC5: modrm(s);
        p16(c, s->reg, rd16(c, s->ea_seg, s->ea_off));
        b86_set_seg(c, op == 0xC4 ? B86_ES : B86_DS, rd16(c, s->ea_seg, (uint16_t)(s->ea_off + 2)));
        break;
    case 0xC6: case 0xC7: modrm(s); rm_put(s, w, w ? f16(s) : f8(s)); break;
    case 0xC8: case 0xCA: { uint16_t n = f16(s); s->ip = pop(c); b86_set_seg(c, B86_CS, pop(c));
        c->r[B86_SP] = (uint16_t)(c->r[B86_SP] + n); break; }
    case 0xC9: case 0xCB: s->ip = pop(c); b86_set_seg(c, B86_CS, pop(c)); break;
    case 0xCC: c->ip = s->ip; b86_interrupt(c, 3); return B86_OK;
    case 0xCD: { uint8_t v = f8(s); c->ip = s->ip; b86_interrupt(c, v); return B86_OK; }
    case 0xCE: c->ip = s->ip; if (FL & B86_OF) b86_interrupt(c, 4); return B86_OK;
    case 0xCF: s->ip = pop(c); b86_set_seg(c, B86_CS, pop(c)); FL = (pop(c) & 0x0FD5u) | 0xF002u; break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        modrm(s);
        unsigned n = (op & 2) ? g8(c, 1) : 1;
        if (c->model >= B86_CPU_186) n &= 31u;          /* 80186+: count masked to 5 bits */
        uint32_t v = rm_get(s, w);
        uint32_t r = shift(c, s->reg, w, v, n);
        if (n) rm_put(s, w, r);
        break; }
    case 0xD4: { uint8_t b = f8(s), al = g8(c, 0);
        if (!b) {
            /* BB_386_STAGE5C4D_V1: #DE is fault-atomic. AAM 0 must not change FLAGS. */
            if (c->model >= B86_CPU_386) return b86_rm5c2_fault(s, 0u);
            szp(c, 0, 0); c->ip = DE_IP(s); b86_interrupt(c, 0); return B86_OK;
        }
        p8(c, 4, al / b); p8(c, 0, al % b); szp(c, 0, al % b); break; }
    case 0xD5: { uint8_t b = f8(s);
        uint32_t r = add_f(c, 0, g8(c, 0), (uint8_t)(g8(c, 4) * b), 0);
        p16(c, 0, (uint16_t)(r & 0xFF)); break; }
    case 0xD6: p8(c, 0, (FL & B86_CF) ? 0xFF : 0x00); break;
    case 0xD7: { int sg = s->seg >= 0 ? s->seg : B86_DS;
        p8(c, 0, rd8(c, sg, (uint16_t)(g16(c, B86_BX) + g8(c, 0)))); break; }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(s); break; /* ESC: no FPU */
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t d = (int8_t)f8(s);
        c->r[B86_CX] = (uint16_t)(c->r[B86_CX] - 1);
        int t = (uint16_t)c->r[B86_CX] != 0;
        if (op == 0xE0) t = t && !(FL & B86_ZF);
        if (op == 0xE1) t = t && (FL & B86_ZF);
        if (t) s->ip = (uint16_t)(s->ip + d);
        break; }
    case 0xE3: { int8_t d = (int8_t)f8(s); if ((uint16_t)c->r[B86_CX] == 0) s->ip = (uint16_t)(s->ip + d); break; }
    case 0xE4: case 0xE5: { uint8_t p = f8(s);
        uint8_t lo = c->in8 ? c->in8(c, p) : 0xFF;
        if (w) p16(c, 0, (uint16_t)(lo | ((c->in8 ? c->in8(c, (uint16_t)(p + 1)) : 0xFF) << 8)));
        else p8(c, 0, lo);
        break; }
    case 0xE6: case 0xE7: { uint8_t p = f8(s);
        if (c->out8) { c->out8(c, p, g8(c, 0)); if (w) c->out8(c, (uint16_t)(p + 1), g8(c, 4)); }
        break; }
    case 0xE8: { int16_t d = (int16_t)f16(s); push(c, s->ip); s->ip = (uint16_t)(s->ip + d); break; }
    case 0xE9: { int16_t d = (int16_t)f16(s); s->ip = (uint16_t)(s->ip + d); break; }
    case 0xEA: { uint16_t ip = f16(s), cs = f16(s); b86_set_seg(c, B86_CS, cs); s->ip = ip; break; }
    case 0xEB: { int8_t d = (int8_t)f8(s); s->ip = (uint16_t)(s->ip + d); break; }
    case 0xEC: case 0xED: { uint16_t p = g16(c, B86_DX);
        uint8_t lo = c->in8 ? c->in8(c, p) : 0xFF;
        if (w) p16(c, 0, (uint16_t)(lo | ((c->in8 ? c->in8(c, (uint16_t)(p + 1)) : 0xFF) << 8)));
        else p8(c, 0, lo);
        break; }
    case 0xEE: case 0xEF: { uint16_t p = g16(c, B86_DX);
        if (c->out8) { c->out8(c, p, g8(c, 0)); if (w) c->out8(c, (uint16_t)(p + 1), g8(c, 4)); }
        break; }
    case 0xF4: c->ip = s->ip; return B86_HALT;
    case 0xF5: FL ^= B86_CF; break;
    case 0xF6: case 0xF7: {
        modrm(s);
        int gr = group3(s, w);
        if (gr) return c->model >= B86_CPU_386 ? gr - 1 : B86_OK;
        break; }
    case 0xF8: FL &= ~(uint32_t)B86_CF; break;
    case 0xF9: FL |= B86_CF; break;
    case 0xFA: FL &= ~(uint32_t)B86_IF; break;
    case 0xFB: FL |= B86_IF; break;
    case 0xFC: FL &= ~(uint32_t)B86_DF; break;
    case 0xFD: FL |= B86_DF; break;
    case 0xFE: modrm(s);
        if (s->reg < 2) rm_put(s, 0, incdec(c, 0, rm_get(s, 0), s->reg));
        break;
    case 0xFF: modrm(s);
        switch (s->reg) {
        case 0: case 1: rm_put(s, 1, incdec(c, 1, rm_get(s, 1), s->reg)); break;
        case 2: { uint16_t t = (uint16_t)rm_get(s, 1); push(c, s->ip); s->ip = t; break; }
        case 3: { uint16_t ip = rd16(c, s->ea_seg, s->ea_off), cs = rd16(c, s->ea_seg, (uint16_t)(s->ea_off + 2));
            push(c, (uint16_t)c->seg[B86_CS]); push(c, s->ip); b86_set_seg(c, B86_CS, cs); s->ip = ip; break; }
        case 4: s->ip = (uint16_t)rm_get(s, 1); break;
        case 5: { uint16_t ip = rd16(c, s->ea_seg, s->ea_off), cs = rd16(c, s->ea_seg, (uint16_t)(s->ea_off + 2));
            b86_set_seg(c, B86_CS, cs); s->ip = ip; break; }
        default: /* PUSH r/m; PUSH SP pushes the decremented SP */
            if (s->mod == 3 && s->rm == B86_SP && c->model < B86_CPU_286) {
                c->r[B86_SP] = (uint16_t)(c->r[B86_SP] - 2);
                wr16(c, B86_SS, c->r[B86_SP], (uint16_t)c->r[B86_SP]);
            } else push(c, (uint16_t)rm_get(s, 1));
            break;
        }
        break;
    default: break;
    }
    c->ip = s->ip;
    return B86_OK;
}

/* BB_386_RM0_V1: on a 386, 16-bit instructions must preserve the
   upper halves of EAX..EDI, even when the legacy 286 interpreter assigns
   directly to 16-bit register slots.  The wrapper is never used by 286.
   A 66h instruction writes the entire 32-bit destination, so skip merge. */
/* BB_386_RM2_FLASH_SRAM_V1: 386-only upper-register preservation in XIP flash. */
static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_386_step_flash(B86Cpu *c)
{
    uint32_t hi[8];
    for (unsigned i = 0; i < 8; ++i) hi[i] = c->r[i] & 0xFFFF0000u;
    uint16_t ip = (uint16_t)c->ip;
    int operand32 = 0, address32 = 0;
    uint8_t final_op = 0;
    for (unsigned k = 0; k < 15; ++k) {
        uint8_t op = rd8(c, B86_CS, ip++);
        if (op == 0x66) { operand32 = 1; continue; }
        if (op == 0x67) { address32 = 1; continue; }
        if (op == 0x64 || op == 0x65 || op == 0x26 || op == 0x2E || op == 0x36 ||
            op == 0x3E || op == 0xF0 || op == 0xF1 || op == 0xF2 || op == 0xF3)
            continue;
        final_op = op;
        break;
    }
    /* BB_386_STAGE5C5B_V2_IMPLICIT_R32: 0F 20/22 use r32 even without 66h.
     * Inspect extension before execution; exception entry can change CS. */
    const int implicit_control32 = final_op == 0x0Fu &&
        (rd8(c, B86_CS, ip) == 0x20u || rd8(c, B86_CS, ip) == 0x22u);
    int rc = b86_step_internal(c);
    /* 67h string instructions update full ESI/EDI/ECX even with the default
     * 16-bit operand size. Keep their updated high halves, but preserve the
     * usual legacy word-register masking for all other 386 instructions. */
    const int wide_index = address32 &&
        ((final_op >= 0xA4 && final_op <= 0xA7) ||
         (final_op >= 0xAA && final_op <= 0xAF) ||
         (final_op >= 0x6C && final_op <= 0x6F));
    if (!operand32 && !implicit_control32) for (unsigned i = 0; i < 8; ++i) {
        if (wide_index && (i == B86_CX || i == B86_SI || i == B86_DI)) continue;
        c->r[i] = (c->r[i] & 0xFFFFu) | hi[i];
    }
    return rc;
}

B86_HOT int b86_step(B86Cpu *c)
{
    /* BB_386_STAGE5C4A_V1: shutdown precedes fetch and all guest side effects. */
    if (c->model >= B86_CPU_386 && c->shutdown386) return B86_SHUTDOWN;
    /* 5C5F: always use descriptor-backed protected execution while PE=1.
     * PM4 also delegates the initial single far-JMP transition to PM3. */
    if (c->model >= B86_CPU_386 && (c->msw & 1u)) {
        /* SS interrupt/debug shadow is retired by one completed *following*
         * protected instruction. The loader arms it with value 2. */
        uint8_t before = c->pm386.seg[B86_SS].reserved;
        int rc = b86_pm20_step(c);
        if (rc == B86_OK || rc == B86_HALT) {
            if (c->pm386.seg[B86_SS].reserved == B86_PM11_SHADOW_ARMED)
                c->pm386.seg[B86_SS].reserved = B86_PM11_SHADOW_NEXT;
            else if (before == B86_PM11_SHADOW_NEXT &&
                     c->pm386.seg[B86_SS].reserved == B86_PM11_SHADOW_NEXT)
                c->pm386.seg[B86_SS].reserved = 0u;
        }
        return rc;
    }
    if (c->model < B86_CPU_386) return b86_step_internal(c);
    return b86_386_step_flash(c);
}

B86_HOT int b86_run_interp(B86Cpu *c, uint64_t max)
{
    if (c->model >= B86_CPU_386 && c->shutdown386) return B86_SHUTDOWN;
    for (uint64_t i = 0; i < max; ++i) {
        int r = b86_step(c);
        if (r != B86_OK) return r;
        if (c->irq) return B86_EXIT;
    }
    return B86_BUDGET;
}

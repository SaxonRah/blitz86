/* Reference 8086 interpreter.
 *
 * Two jobs: (1) the oracle every translated instruction is checked against,
 * validated itself against SingleStepTests physical-8086 vectors; (2) the
 * cold path the JIT calls for instructions it does not lower natively.
 * It is written for clarity and exactness, not speed. Flags are eager here;
 * lazy state from generated code is materialized on entry. */
#include "b86.h"
#include <string.h>

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

void b86_set_seg(B86Cpu *c, int s, uint16_t v)
{
    c->seg[s] = v;
    c->segp[s] = c->mem + ((uint32_t)v << 4);
}

void b86_init(B86Cpu *c, uint8_t *mem)
{
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->amask = 0x1FFFFFu;
    c->flags = 0xF002u;
    c->trap_cs = 0xFFFFFFFFu;
    for (int s = 0; s < 4; ++s) b86_set_seg(c, s, 0);
}

static uint32_t lazy_eval(uint32_t k, uint32_t a0, uint32_t r0, uint32_t b0, uint32_t f)
{
    int w16 = (k == LZ_ADD16 || k == LZ_SUB16 || k == LZ_LOG16 ||
               k == LZ_INC16 || k == LZ_DEC16 || k == LZ_SHL16 || k == LZ_SHR16 || k == LZ_SAR16 ||
               k == LZ_ADC16 || k == LZ_SBB16);
    uint32_t m = w16 ? 0xFFFFu : 0xFFu, sb = w16 ? 0x8000u : 0x80u;
    uint32_t a = a0 & m, r = r0 & m, b;
    if (k >= LZ_SHL8 && k <= LZ_SAR16) {          /* shifts by 1 (8086 semantics) */
        uint32_t cf, of;
        if (k <= LZ_SHL16) { cf = (a & sb) != 0; of = ((r & sb) != 0) ^ cf; }
        else if (k <= LZ_SHR16) { cf = a & 1; of = (a & sb) != 0; }
        else { cf = a & 1; of = 0; }
        f &= ~(uint32_t)B86_ARITH;
        if (cf) f |= B86_CF;
        if (of) f |= B86_OF;
        if (r == 0) f |= B86_ZF;
        if (r & sb) f |= B86_SF;
        return f | b86_parity[r & 0xFFu];
    }
    if (k == LZ_ADD8 || k == LZ_ADD16 || k == LZ_INC8 || k == LZ_INC16) b = (r - a) & m;
    else if (k >= LZ_ADC8) b = b0 & m;
    else b = (a - r) & m;
    uint32_t cf = 0, of = 0, af = 0;
    switch (k) {
    case LZ_ADD8: case LZ_ADD16: case LZ_INC8: case LZ_INC16: case LZ_ADC8: case LZ_ADC16:
        cf = ((a & b) | ((a | b) & ~r)) & sb;
        of = ((a ^ r) & (b ^ r)) & sb;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case LZ_SUB8: case LZ_SUB16: case LZ_DEC8: case LZ_DEC16: case LZ_SBB8: case LZ_SBB16:
        cf = ((~a & b) | (~(a ^ b) & r)) & sb;
        of = ((a ^ b) & (a ^ r)) & sb;
        af = (a ^ b ^ r) & 0x10u;
        break;
    default: break; /* logic: CF = OF = AF = 0 */
    }
    uint32_t keep_cf = f & B86_CF;
    f &= ~(uint32_t)B86_ARITH;
    if (k == LZ_INC8 || k == LZ_INC16 || k == LZ_DEC8 || k == LZ_DEC16) f |= keep_cf;
    else if (cf) f |= B86_CF;
    if (of) f |= B86_OF;
    if (af) f |= B86_AF;
    if (r == 0) f |= B86_ZF;
    if (r & sb) f |= B86_SF;
    f |= b86_parity[r & 0xFFu];
    return f;
}

void b86_flags_materialize(B86Cpu *c)
{
    if (c->lz_kind != LZ_NONE) c->flags = lazy_eval(c->lz_kind, c->lz_a, c->lz_res, c->lz_b, c->flags);
    if (c->lz_ikind != LZ_NONE) c->flags = lazy_eval(c->lz_ikind, c->lz_ia, c->lz_ires, 0, c->flags);
    c->lz_kind = LZ_NONE;
    c->lz_ikind = LZ_NONE;
}

uint16_t b86_get_flags(B86Cpu *c)
{
    b86_flags_materialize(c);
    return (uint16_t)((c->flags & 0x0FD5u) | 0xF002u);
}

void b86_set_flags(B86Cpu *c, uint16_t f)
{
    c->lz_kind = LZ_NONE;
    c->lz_ikind = LZ_NONE;
    c->flags = (f & 0x0FD5u) | 0xF002u;
}

/* ------------------------------------------------------------------------ */
/* Memory                                                                   */
/* ------------------------------------------------------------------------ */

static inline uint32_t lin(const B86Cpu *c, int s, uint32_t off)
{
    return ((c->seg[s] << 4) + (off & 0xFFFFu)) & c->amask;
}

static inline void smc(B86Cpu *c, uint32_t a, uint32_t n)
{
    if (c->codemap && (c->codemap[a >> B86_LINE_SHIFT] |
                       c->codemap[(a + n - 1) >> B86_LINE_SHIFT]) && c->smc_hook)
        c->smc_hook(c, a, n);
}

static inline uint8_t rd8(B86Cpu *c, int s, uint32_t off) { return c->mem[lin(c, s, off)]; }
static inline void wr8(B86Cpu *c, int s, uint32_t off, uint8_t v)
{
    uint32_t a = lin(c, s, off);
    c->mem[a] = v;
    smc(c, a, 1);
}
static inline uint16_t rd16(B86Cpu *c, int s, uint32_t off)
{
    if (c->exact_wrap)
        return (uint16_t)(rd8(c, s, off) | (rd8(c, s, off + 1) << 8));
    uint32_t a = lin(c, s, off);
    return (uint16_t)(c->mem[a] | (c->mem[a + 1] << 8));
}
static inline void wr16(B86Cpu *c, int s, uint32_t off, uint16_t v)
{
    if (c->exact_wrap) { wr8(c, s, off, (uint8_t)v); wr8(c, s, off + 1, (uint8_t)(v >> 8)); return; }
    uint32_t a = lin(c, s, off);
    c->mem[a] = (uint8_t)v;
    c->mem[a + 1] = (uint8_t)(v >> 8);
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
} St;

static inline uint8_t f8(St *s) { uint8_t v = rd8(s->c, B86_CS, s->ip); s->ip++; return v; }
static inline uint16_t f16(St *s) { uint16_t lo = f8(s); return (uint16_t)(lo | (f8(s) << 8)); }

static void modrm(St *s)
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

static inline uint8_t g8(B86Cpu *c, int r) { return (uint8_t)(r < 4 ? c->r[r] : c->r[r - 4] >> 8); }
static inline void p8(B86Cpu *c, int r, uint8_t v)
{
    if (r < 4) c->r[r] = (c->r[r] & 0xFF00u) | v;
    else c->r[r - 4] = (c->r[r - 4] & 0x00FFu) | ((uint32_t)v << 8);
}
static inline uint16_t g16(B86Cpu *c, int r) { return (uint16_t)c->r[r]; }
static inline void p16(B86Cpu *c, int r, uint16_t v) { c->r[r] = v; }

static inline uint32_t rm_get(St *s, int w)
{
    if (s->mod == 3) return w ? g16(s->c, s->rm) : g8(s->c, s->rm);
    return w ? rd16(s->c, s->ea_seg, s->ea_off) : rd8(s->c, s->ea_seg, s->ea_off);
}
static inline void rm_put(St *s, int w, uint32_t v)
{
    if (s->mod == 3) { if (w) p16(s->c, s->rm, (uint16_t)v); else p8(s->c, s->rm, (uint8_t)v); return; }
    if (w) wr16(s->c, s->ea_seg, s->ea_off, (uint16_t)v); else wr8(s->c, s->ea_seg, s->ea_off, (uint8_t)v);
}
static inline uint32_t reg_get(St *s, int w) { return w ? g16(s->c, s->reg) : g8(s->c, s->reg); }
static inline void reg_put(St *s, int w, uint32_t v)
{
    if (w) p16(s->c, s->reg, (uint16_t)v); else p8(s->c, s->reg, (uint8_t)v);
}

static inline void push(B86Cpu *c, uint16_t v)
{
    c->r[B86_SP] = (uint16_t)(c->r[B86_SP] - 2);
    wr16(c, B86_SS, c->r[B86_SP], v);
}
static inline uint16_t pop(B86Cpu *c)
{
    uint16_t v = rd16(c, B86_SS, c->r[B86_SP]);
    c->r[B86_SP] = (uint16_t)(c->r[B86_SP] + 2);
    return v;
}

/* ------------------------------------------------------------------------ */
/* Flags and ALU                                                            */
/* ------------------------------------------------------------------------ */

#define FL (c->flags)
static inline void setf(B86Cpu *c, uint32_t bit, int on) { if (on) FL |= bit; else FL &= ~bit; }

static inline void szp(B86Cpu *c, int w, uint32_t r)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu;
    r &= m;
    FL &= ~(uint32_t)(B86_SF | B86_ZF | B86_PF);
    if (!r) FL |= B86_ZF;
    if (r & (w ? 0x8000u : 0x80u)) FL |= B86_SF;
    FL |= b86_parity[r & 0xFF];
}

static uint32_t add_f(B86Cpu *c, int w, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu, sb = w ? 0x8000u : 0x80u;
    uint32_t r = a + b + cin;
    setf(c, B86_CF, (r & ~m) != 0);
    r &= m;
    setf(c, B86_OF, ((a ^ r) & (b ^ r) & sb) != 0);
    setf(c, B86_AF, ((a ^ b ^ r) & 0x10u) != 0);
    szp(c, w, r);
    return r;
}
static uint32_t sub_f(B86Cpu *c, int w, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu, sb = w ? 0x8000u : 0x80u;
    uint32_t r = a - b - cin;
    setf(c, B86_CF, (r & ~m) != 0);
    r &= m;
    setf(c, B86_OF, ((a ^ b) & (a ^ r) & sb) != 0);
    setf(c, B86_AF, ((a ^ b ^ r) & 0x10u) != 0);
    szp(c, w, r);
    return r;
}
static uint32_t log_f(B86Cpu *c, int w, uint32_t r)
{
    FL &= ~(uint32_t)(B86_CF | B86_OF | B86_AF);
    szp(c, w, r);
    return r & (w ? 0xFFFFu : 0xFFu);
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns result; CMP
   result is not written by callers. */
static uint32_t alu(B86Cpu *c, int op, int w, uint32_t a, uint32_t b)
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

static uint32_t incdec(B86Cpu *c, int w, uint32_t a, int dec)
{
    uint32_t cf = FL & B86_CF;
    uint32_t r = dec ? sub_f(c, w, a, 1, 0) : add_f(c, w, a, 1, 0);
    FL = (FL & ~(uint32_t)B86_CF) | cf;
    return r;
}

/* Shifts/rotates, 8086 semantics: count is not masked. */
static uint32_t shift(B86Cpu *c, int op, int w, uint32_t v, unsigned n)
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

static int cond(B86Cpu *c, int cc)
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

void b86_interrupt(B86Cpu *c, uint8_t v)
{
    b86_flags_materialize(c);
    if (c->int_hook && c->int_hook(c, v)) return;
    push(c, (uint16_t)((c->flags & 0x0FD5u) | 0xF002u));
    FL &= ~(uint32_t)(B86_IF | B86_TF);
    push(c, (uint16_t)c->seg[B86_CS]);
    push(c, (uint16_t)c->ip);
    uint32_t vec = (uint32_t)v * 4u;
    c->ip = c->mem[vec] | (c->mem[vec + 1] << 8);
    b86_set_seg(c, B86_CS, (uint16_t)(c->mem[vec + 2] | (c->mem[vec + 3] << 8)));
}

/* ------------------------------------------------------------------------ */
/* String instructions                                                      */
/* ------------------------------------------------------------------------ */

static void string_op(St *s, uint8_t op)
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

static int group3(St *s, int w)
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
            if (v == 0 || n / v > 0xFF) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            p8(c, 0, (uint8_t)(n / v)); p8(c, 4, (uint8_t)(n % v));
        } else {
            uint32_t n = ((uint32_t)g16(c, B86_DX) << 16) | g16(c, B86_AX);
            if (v == 0 || n / v > 0xFFFF) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            p16(c, B86_AX, (uint16_t)(n / v)); p16(c, B86_DX, (uint16_t)(n % v));
        }
        break;
    case 7: /* IDIV */
        if (!w) {
            int32_t n = (int16_t)g16(c, B86_AX), d = (int8_t)v;
            if (d == 0) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            int32_t q = n / d, r = n % d;
            if (q <= -128 || q > 127) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            if (s->rep) q = -q;
            p8(c, 0, (uint8_t)q); p8(c, 4, (uint8_t)r);
        } else {
            int32_t n = (int32_t)(((uint32_t)g16(c, B86_DX) << 16) | g16(c, B86_AX)), d = (int16_t)v;
            if (d == 0) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            if (n == INT32_MIN && d == -1) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
            int32_t q = n / d, r = n % d;
            if (q <= -32768 || q > 32767) { c->ip = s->ip; b86_interrupt(c, 0); return 1; }
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

static void daa_das(B86Cpu *c, int sub)
{
    uint8_t al = g8(c, 0), old = al;
    int oaf = (FL & B86_AF) != 0, ocf = (FL & B86_CF) != 0, af = 0, cf = 0;
    if ((al & 0x0F) > 9 || oaf) { al = (uint8_t)(sub ? al - 6 : al + 6); af = 1; }
    if (ocf || old > 0x9F || (old > 0x99 && !oaf)) { al = (uint8_t)(sub ? al - 0x60 : al + 0x60); cf = 1; }
    setf(c, B86_AF, af); setf(c, B86_CF, cf);
    p8(c, 0, al);
    szp(c, 0, al);
}

static void aaa_aas(B86Cpu *c, int sub)
{
    uint8_t al = g8(c, 0), ah = g8(c, 4);
    if ((al & 0x0F) > 9 || (FL & B86_AF)) {
        al = (uint8_t)(sub ? al - 6 : al + 6);
        ah = (uint8_t)(sub ? ah - 1 : ah + 1);
        FL |= B86_AF | B86_CF;
    } else FL &= ~(uint32_t)(B86_AF | B86_CF);
    p8(c, 0, al & 0x0F); p8(c, 4, ah);
}

/* ------------------------------------------------------------------------ */
/* The step function                                                        */
/* ------------------------------------------------------------------------ */

int b86_step(B86Cpu *c)
{
    St st, *s = &st;
    b86_flags_materialize(c);
    s->c = c; s->ip = (uint16_t)c->ip; s->seg = -1; s->rep = 0;
    uint8_t op;
    for (;;) {
        op = f8(s);
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) { s->seg = (op >> 3) & 3; continue; }
        if (op == 0xF2 || op == 0xF3) { s->rep = op; continue; }
        if (op == 0xF0 || op == 0xF1) continue;
        break;
    }
    c->icount++;
    int w = op & 1;

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
    case 0x54: /* 8086 PUSH SP pushes the decremented value */
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
    case 0x9C: push(c, (uint16_t)((FL & 0x0FD5u) | 0xF002u)); break;
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
        uint32_t v = rm_get(s, w);
        uint32_t r = shift(c, s->reg, w, v, n);
        if (n) rm_put(s, w, r);
        break; }
    case 0xD4: { uint8_t b = f8(s), al = g8(c, 0);
        if (!b) { szp(c, 0, 0); c->ip = s->ip; b86_interrupt(c, 0); return B86_OK; }
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
    case 0xF6: case 0xF7: modrm(s); if (group3(s, w)) return B86_OK; break;
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
            if (s->mod == 3 && s->rm == B86_SP) {
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

int b86_run_interp(B86Cpu *c, uint64_t max)
{
    for (uint64_t i = 0; i < max; ++i) {
        int r = b86_step(c);
        if (r != B86_OK) return r;
        if (c->irq) return B86_EXIT;
    }
    return B86_BUDGET;
}

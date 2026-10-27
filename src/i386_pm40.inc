#include "b86_div.h"   /* r73i */
/* B86_PM40_V1 (r68): fast path for 32-bit protected-mode code.
 *
 * One decoder and one set of instruction semantics for the instructions that
 * dominate compiled 32-bit DOS/4GW programs (Watcom C: Descent). It is the
 * front end of the protected-mode JIT: the native translator emits code for
 * exactly what this file accepts, and this file is its reference and fallback.
 *
 * Rules
 *  - Runs only with CR0.PE=1, a valid 32-bit CS (D=1), no PE transition in
 *    progress, EFLAGS.VM=0, and the instruction fully inside the per-step
 *    fetch window (c->fb).
 *  - Every check happens before any state changes. Anything it does not
 *    handle, or any access that would fault (segment limit/type, page not
 *    present, page-crossing operand, divide error, I/O not permitted),
 *    returns -1 and the original stage chain executes the instruction and
 *    raises the exception exactly as before.
 *  - Arithmetic flags are computed exactly; flags the 386 leaves undefined
 *    are reported in c->pm40_undef so a verifier can ignore them.
 *  - REP MOVS/STOS run at most B86_PM40_REP elements per step and leave EIP
 *    on the instruction while ECX != 0 (restartable, as on the CPU).
 */
#ifndef B86_PM40_FLASH
#define B86_PM40_FLASH __attribute__((noinline)) B86_PM_HOT
#endif
#ifndef B86_PM40_REP
#define B86_PM40_REP 4096u
#endif

unsigned long b86_pm40_stat_ok, b86_pm40_stat_decline;
#ifdef B86_PM40_STATS
unsigned long b86_pm40_decl[512];   /* by opcode; 0x100+xx for 0F xx */
#endif

typedef struct {
    const uint8_t *b;
    unsigned n, pos;
    unsigned o16, a16, seg, rep, segov;   /* o16/a16: effective 16-bit operand/address size */
    /* ModRM */
    unsigned mod, reg, rm;
    uint32_t ea;            /* effective offset (memory forms) */
    unsigned easeg;
} P40;

static inline int p40_u8(P40 *d, uint32_t *v) { if (d->pos >= d->n) return 0; *v = d->b[d->pos++]; return 1; }
static inline int p40_u16(P40 *d, uint32_t *v)
{ if (d->pos + 2u > d->n) return 0; *v = (uint32_t)d->b[d->pos] | ((uint32_t)d->b[d->pos + 1u] << 8); d->pos += 2u; return 1; }
static inline int p40_u32(P40 *d, uint32_t *v)
{
    if (d->pos + 4u > d->n) return 0;
    *v = (uint32_t)d->b[d->pos] | ((uint32_t)d->b[d->pos + 1u] << 8) |
         ((uint32_t)d->b[d->pos + 2u] << 16) | ((uint32_t)d->b[d->pos + 3u] << 24);
    d->pos += 4u; return 1;
}
static inline int p40_imm(P40 *d, unsigned w, uint32_t *v)
{ return w == 1u ? p40_u8(d, v) : w == 2u ? p40_u16(d, v) : p40_u32(d, v); }

/* ModRM/SIB for 32-bit addressing, or 16-bit forms when d->a16. */
static B86_PM_HOT int p40_modrm(const B86Cpu *c, P40 *d)
{
    uint32_t m, sib, disp = 0u;
    if (!p40_u8(d, &m)) return 0;
    d->mod = m >> 6; d->reg = (m >> 3) & 7u; d->rm = m & 7u;
    if (d->mod == 3u) return 1;
    d->easeg = B86_DS;
    if (d->a16) {                              /* r68b: 16-bit addressing */
        static const uint8_t b1[8] = { 3, 3, 5, 5, 6, 7, 5, 3 }, b2[8] = { 6, 7, 6, 7, 8, 8, 8, 8 };
        if (d->mod == 0u && d->rm == 6u) { if (!p40_u16(d, &disp)) return 0; d->ea = disp; }
        else {
            d->ea = c->r[b1[d->rm]] + (b2[d->rm] < 8u ? c->r[b2[d->rm]] : 0u);
            if (d->rm == 2u || d->rm == 3u || d->rm == 6u) d->easeg = B86_SS;
            if (d->mod == 1u) { if (!p40_u8(d, &disp)) return 0; d->ea += (uint32_t)(int32_t)(int8_t)disp; }
            else if (d->mod == 2u) { if (!p40_u16(d, &disp)) return 0; d->ea += disp; }
        }
        d->ea &= 0xFFFFu;
        if (d->segov) d->easeg = d->seg;
        return 1;
    }
    if (d->rm == 4u) {
        unsigned base, idx, sc;
        if (!p40_u8(d, &sib)) return 0;
        sc = sib >> 6; idx = (sib >> 3) & 7u; base = sib & 7u;
        d->ea = idx != 4u ? c->r[idx] << sc : 0u;
        if (base == 5u && d->mod == 0u) { if (!p40_u32(d, &disp)) return 0; }
        else { d->ea += c->r[base]; if (base == 4u || base == 5u) d->easeg = B86_SS; }
    } else if (d->rm == 5u && d->mod == 0u) {
        if (!p40_u32(d, &disp)) return 0;
        d->ea = 0u;
    } else {
        d->ea = c->r[d->rm];
        if (d->rm == 5u) d->easeg = B86_SS;
    }
    if (d->mod == 1u) { if (!p40_u8(d, &disp)) return 0; disp = (uint32_t)(int32_t)(int8_t)disp; }
    else if (d->mod == 2u) { if (!p40_u32(d, &disp)) return 0; }
    d->ea += disp;
    if (d->segov) d->easeg = d->seg;
    return 1;
}

/* Host pointer for seg:off, size bytes, or NULL to decline (fault path). */
static B86_PM_HOT uint8_t *p40_ptr(B86Cpu *c, unsigned seg, uint32_t off, unsigned size, int write, uint32_t *lin_out)
{
    const B86PM1Cache *s = &c->pm386.seg[seg];
    uint32_t lin, phys;
    if (!s->valid || !(s->access & 0x80u) || !(s->access & 0x10u)) return NULL;
    if (s->access & 8u) {                      /* code segment */
        if (write || !(s->access & 2u)) return NULL;
    } else {
        if (write && !(s->access & 2u)) return NULL;
        if (s->access & 4u) return NULL;       /* expand-down: leave to the stages */
    }
    if (s->limit != 0xFFFFFFFFu && (off > s->limit || s->limit - off < size - 1u)) return NULL;
    lin = s->base + off;
    if ((lin & 0xFFFu) + size > 0x1000u) return NULL;
    if (c->pg) { if (!b86_pg_walk(c, lin, &phys)) return NULL; }
    else phys = lin;
    if (phys > B86_RAM_BYTES - size) return NULL;
    *lin_out = lin;
    return b86_host_phys(c, phys);
}
static inline uint32_t p40_ld(const uint8_t *p, unsigned w)
{
    return w == 1u ? p[0] : w == 2u ? (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         : (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#ifdef B86_PM40_VERIFY
static struct { uint8_t *p; uint8_t w, old[4], nw[4]; } p40_log[1u << 16];
static unsigned p40_nlog, p40_logging, p40_short;
#endif
static inline void p40_st(B86Cpu *c, uint8_t *p, uint32_t lin, uint32_t v, unsigned w)
{
#ifdef B86_PM40_VERIFY
    if (p40_logging && p40_nlog < (1u << 16)) {
        p40_log[p40_nlog].p = p; p40_log[p40_nlog].w = (uint8_t)w;
        for (unsigned k = 0; k < w; ++k) { p40_log[p40_nlog].old[k] = p[k]; p40_log[p40_nlog].nw[k] = (uint8_t)(v >> (8u * k)); }
        ++p40_nlog;
    }
#endif
    p[0] = (uint8_t)v;
    if (w > 1u) { p[1] = (uint8_t)(v >> 8); if (w > 2u) { p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); } }
    smc(c, lin, w);
}

/* registers by width (8-bit: AL CL DL BL AH CH DH BH) */
static inline uint32_t p40_getr(const B86Cpu *c, unsigned r, unsigned w)
{
    if (w == 4u) return c->r[r];
    if (w == 2u) return c->r[r] & 0xFFFFu;
    return r < 4u ? c->r[r] & 0xFFu : (c->r[r - 4u] >> 8) & 0xFFu;
}
static inline void p40_setr(B86Cpu *c, unsigned r, unsigned w, uint32_t v)
{
    if (w == 4u) c->r[r] = v;
    else if (w == 2u) c->r[r] = (c->r[r] & 0xFFFF0000u) | (v & 0xFFFFu);
    else if (r < 4u) c->r[r] = (c->r[r] & 0xFFFFFF00u) | (v & 0xFFu);
    else c->r[r - 4u] = (c->r[r - 4u] & 0xFFFF00FFu) | ((v & 0xFFu) << 8);
}

static inline uint32_t p40_mask(unsigned w) { return w == 4u ? 0xFFFFFFFFu : (1u << (w * 8u)) - 1u; }
static inline uint32_t p40_msb(unsigned w) { return 1u << (w * 8u - 1u); }
static inline uint32_t p40_szp(uint32_t r, unsigned w)
{
    return (r ? 0u : (uint32_t)B86_ZF) | ((r & p40_msb(w)) ? (uint32_t)B86_SF : 0u) | b86_parity[r & 0xFFu];
}
static inline void p40_setflags(B86Cpu *c, uint32_t f) { c->flags = (c->flags & ~(uint32_t)B86_ARITH) | f; }

/* ALU: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns result. */
static B86_PM_HOT uint32_t p40_alu(B86Cpu *c, unsigned op, uint32_t a, uint32_t b, unsigned w)
{
    const uint32_t m = p40_mask(w), msb = p40_msb(w);
    uint32_t r, f;
    uint64_t wide;
    a &= m; b &= m;
    switch (op) {
    case 0: case 2: {
        const uint32_t cin = op == 2u ? (c->flags & B86_CF) : 0u;
        wide = (uint64_t)a + b + cin; r = (uint32_t)wide & m;
        f = (wide > m ? (uint32_t)B86_CF : 0u) | (((a ^ r) & (b ^ r) & msb) ? (uint32_t)B86_OF : 0u) |
            ((a ^ b ^ r) & 0x10u);
        break; }
    case 3: case 5: case 7: {
        const uint32_t bin = op == 3u ? (c->flags & B86_CF) : 0u;
        r = (a - b - bin) & m;
        f = ((uint64_t)a < (uint64_t)b + bin ? (uint32_t)B86_CF : 0u) |
            (((a ^ b) & (a ^ r) & msb) ? (uint32_t)B86_OF : 0u) | ((a ^ b ^ r) & 0x10u);
        break; }
    case 1: r = a | b; f = 0u; c->pm40_undef |= B86_AF; break;
    case 4: r = a & b; f = 0u; c->pm40_undef |= B86_AF; break;
    default: r = a ^ b; f = 0u; c->pm40_undef |= B86_AF; break;
    }
    p40_setflags(c, f | p40_szp(r, w));
    return r;
}

static B86_PM_HOT int p40_cond(uint32_t f, unsigned cc)
{
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
    return (cc & 1u) ? !r : r;
}

/* Shifts/rotates: 0 ROL 1 ROR 4/6 SHL 5 SHR 7 SAR (2/3 RCL/RCR decline). */
static B86_PM_HOT int p40_shift(B86Cpu *c, unsigned op, uint32_t a, unsigned n, unsigned w, uint32_t *res)
{
    const unsigned bits = w * 8u;
    const uint32_t m = p40_mask(w), msb = p40_msb(w);
    uint32_t r, f = c->flags & B86_ARITH;
    a &= m;
    n &= 31u;
    if (op == 2u || op == 3u) return 0;
    if (n == 0u) { *res = a; return 1; }
    switch (op) {
    case 0: {
        const unsigned k = n % bits;
        r = k ? ((a << k) | (a >> (bits - k))) & m : a;
        f = (f & ~(uint32_t)(B86_CF | B86_OF)) | ((r & 1u) ? (uint32_t)B86_CF : 0u);
        if (((r & msb) != 0) != ((r & 1u) != 0)) f |= B86_OF;
        if (n != 1u) c->pm40_undef |= B86_OF;
        break; }
    case 1: {
        const unsigned k = n % bits;
        r = k ? ((a >> k) | (a << (bits - k))) & m : a;
        f = (f & ~(uint32_t)(B86_CF | B86_OF)) | ((r & msb) ? (uint32_t)B86_CF : 0u);
        if (((r & msb) != 0) != ((r & (msb >> 1)) != 0)) f |= B86_OF;
        if (n != 1u) c->pm40_undef |= B86_OF;
        break; }
    case 4: case 6: {
        const uint32_t cf = n <= bits ? (uint32_t)((((uint64_t)a) << (n - 1u)) & msb) != 0u : 0u;
        r = n < 32u ? (a << n) & m : 0u;
        f = p40_szp(r, w) | (cf ? (uint32_t)B86_CF : 0u);
        if (((r & msb) != 0) != (cf != 0)) f |= B86_OF;
        c->pm40_undef |= B86_AF | (n != 1u ? (uint32_t)B86_OF : 0u) | (n > bits ? (uint32_t)B86_CF : 0u);
        break; }
    case 5: {
        const uint32_t cf = n <= bits ? (a >> (n - 1u)) & 1u : 0u;
        r = n < 32u ? a >> n : 0u;
        f = p40_szp(r, w) | (cf ? (uint32_t)B86_CF : 0u) | ((a & msb) ? (uint32_t)B86_OF : 0u);
        c->pm40_undef |= B86_AF | (n != 1u ? (uint32_t)B86_OF : 0u) | (n > bits ? (uint32_t)B86_CF : 0u);
        break; }
    default: {   /* SAR */
        const int32_t sa = w == 4u ? (int32_t)a : w == 2u ? (int32_t)(int16_t)a : (int32_t)(int8_t)a;
        const unsigned k = n > 31u ? 31u : n;
        r = (uint32_t)(sa >> k) & m;
        f = p40_szp(r, w) | (((uint32_t)(sa >> (k - 1u)) & 1u) ? (uint32_t)B86_CF : 0u);
        c->pm40_undef |= B86_AF | (n > bits ? (uint32_t)B86_CF : 0u);
        break; }
    }
    p40_setflags(c, f);
    *res = r;
    return 1;
}

/* r/m operand helpers (memory pointer fetched once, checked, before effects) */
typedef struct { uint8_t *p; uint32_t lin; } P40M;
static B86_PM_HOT int p40_rm_ptr(B86Cpu *c, const P40 *d, unsigned w, int write, P40M *m)
{
    if (d->mod == 3u) { m->p = NULL; return 1; }
    m->p = p40_ptr(c, d->easeg, d->ea, w, write, &m->lin);
    return m->p != NULL;
}
static inline uint32_t p40_rm_get(const B86Cpu *c, const P40 *d, const P40M *m, unsigned w)
{ return m->p ? p40_ld(m->p, w) : p40_getr(c, d->rm, w); }
static inline void p40_rm_put(B86Cpu *c, const P40 *d, const P40M *m, unsigned w, uint32_t v)
{ if (m->p) p40_st(c, m->p, m->lin, v, w); else p40_setr(c, d->rm, w, v); }

/* stack (SS must be a 32-bit stack) */
/* stack: 32-bit ESP when SS.B=1, else 16-bit SP (upper ESP preserved) */
static B86_PM_HOT uint8_t *p40_push_ptr(B86Cpu *c, unsigned w, uint32_t *nsp, uint32_t *lin)
{
    if (c->pm386.seg[B86_SS].flags & 4u) { *nsp = c->r[B86_SP] - w; return p40_ptr(c, B86_SS, *nsp, w, 1, lin); }
    *nsp = (c->r[B86_SP] & 0xFFFF0000u) | ((c->r[B86_SP] - w) & 0xFFFFu);
    return p40_ptr(c, B86_SS, *nsp & 0xFFFFu, w, 1, lin);
}
static B86_PM_HOT uint8_t *p40_pop_ptr(B86Cpu *c, unsigned w, uint32_t *lin)
{
    const int s32 = (c->pm386.seg[B86_SS].flags & 4u) != 0u;
    return p40_ptr(c, B86_SS, s32 ? c->r[B86_SP] : (c->r[B86_SP] & 0xFFFFu), w, 0, lin);
}
static inline void p40_sp_add(B86Cpu *c, uint32_t n)
{
    if (c->pm386.seg[B86_SS].flags & 4u) c->r[B86_SP] += n;
    else c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | ((c->r[B86_SP] + n) & 0xFFFFu);
}

/* segment registers (selectors) and checked loads through the PM2 planner,
 * exactly as the stage chain does them (CPL0 only, like PM9/PM26). */
static inline uint32_t p40_sel(const B86Cpu *c, unsigned s) { return (s <= B86_DS ? c->seg[s] : c->seg386[s - B86_FS]) & 0xFFFFu; }
static int p40_plan(const B86Cpu *c, unsigned s, uint16_t sel, B86PM2Plan *plan)
{
    B86PM2Fault fault;
    if (c->seg[B86_CS] & 3u) return 0;
    return b86_pm2_plan_segment(c, s, sel, 0u, 0u, plan, &fault) == B86_PM2_OK;
}
#ifdef B86_PM40_STATS
static unsigned p40_first_op(const P40 *d)
{
    unsigned i = 0;
    while (i < d->n && (d->b[i] == 0x66 || d->b[i] == 0x67 || d->b[i] == 0xF0 || d->b[i] == 0xF2 || d->b[i] == 0xF3 ||
           d->b[i] == 0x26 || d->b[i] == 0x2E || d->b[i] == 0x36 || d->b[i] == 0x3E || d->b[i] == 0x64 || d->b[i] == 0x65)) ++i;
    if (i >= d->n) return 0;
    if (d->b[i] == 0x0F && i + 1 < d->n) return 0x100u + d->b[i + 1];
    return d->b[i];
}
#endif
static B86_PM40_FLASH int b86_pm40_step(B86Cpu *c)
{
    P40 d;
    P40M mm;
    uint32_t op, v, imm, lin, nsp, a, b;
    unsigned w, npref = 0u;
    const B86PM1Cache *cs = &c->pm386.seg[B86_CS];

    const unsigned code32 = (cs->flags & 4u) != 0u;
    if (!c->fb_len || c->fb_ip != c->ip || c->fb_cs != (uint16_t)c->seg[B86_CS] ||
        cs->reserved || (c->flags & 0x20000u) || (!code32 && c->ip > 0xFFFFu))
        return -1;
    memset(&d, 0, sizeof d);
    d.b = c->fb; d.n = c->fb_len;
    d.o16 = d.a16 = !code32;
    c->pm40_undef = 0u;
    for (;;) {
        if (!p40_u8(&d, &op)) return -1;
        if (++npref > 5u) return -1;
        switch (op) {
        case 0x66: d.o16 = !code32; d.o16 ^= 1u; continue;
        case 0x67: d.a16 = !code32; d.a16 ^= 1u; continue;
        case 0xF0: continue;
        case 0xF2: case 0xF3: d.rep = op; continue;
        case 0x26: d.seg = B86_ES; d.segov = 1u; continue;
        case 0x2E: d.seg = B86_CS; d.segov = 1u; continue;
        case 0x36: d.seg = B86_SS; d.segov = 1u; continue;
        case 0x3E: d.seg = B86_DS; d.segov = 1u; continue;
        case 0x64: d.seg = B86_FS; d.segov = 1u; continue;
        case 0x65: d.seg = B86_GS; d.segov = 1u; continue;
        default: break;
        }
        break;
    }
    w = d.o16 ? 2u : 4u;

#define NEXTIP() (code32 ? c->ip + d.pos : (c->ip + d.pos) & 0xFFFFu)
#define JUMP(t) do { c->ip = (t); ++c->icount; ++b86_pm40_stat_ok; return B86_OK; } while (0)
#define DONE() JUMP(NEXTIP())
#ifdef B86_PM40_STATS
#define NO() do { ++b86_pm40_stat_decline; ++b86_pm40_decl[(d.b[d.pos ? 0 : 0] == 0x0F ? 0 : 0) + p40_first_op(&d)]; return -1; } while (0)
#else
#define NO() do { ++b86_pm40_stat_decline; return -1; } while (0)
#endif

    /* ---- ALU 00-3F (op r/m,r / r,r/m / acc,imm) --------------------------- */
    if (op < 0x40u && (op & 7u) < 6u) {
        const unsigned alu = op >> 3, form = op & 7u;
        const unsigned ww = (form & 1u) ? w : 1u;
        if (form < 4u) {
            if (!p40_modrm(c, &d)) NO();
            if (!p40_rm_ptr(c, &d, ww, form < 2u && alu != 7u, &mm)) NO();
            if (form < 2u) {             /* r/m op= reg */
                v = p40_alu(c, alu, p40_rm_get(c, &d, &mm, ww), p40_getr(c, d.reg, ww), ww);
                if (alu != 7u) p40_rm_put(c, &d, &mm, ww, v);
            } else {                     /* reg op= r/m */
                v = p40_alu(c, alu, p40_getr(c, d.reg, ww), p40_rm_get(c, &d, &mm, ww), ww);
                if (alu != 7u) p40_setr(c, d.reg, ww, v);
            }
        } else {
            if (!p40_imm(&d, ww, &imm)) NO();
            v = p40_alu(c, alu, p40_getr(c, 0u, ww), imm, ww);
            if (alu != 7u) p40_setr(c, 0u, ww, v);
        }
        DONE();
    }
    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        const uint32_t cf = c->flags & B86_CF;
        v = p40_alu(c, op < 0x48u ? 0u : 5u, p40_getr(c, op & 7u, w), 1u, w);
        c->flags = (c->flags & ~(uint32_t)B86_CF) | cf;
        p40_setr(c, op & 7u, w, v);
        DONE(); }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        uint8_t *p = p40_push_ptr(c, w, &nsp, &lin);
        if (!p) NO();
        v = p40_getr(c, op & 7u, w);           /* PUSH ESP pushes the old value */
        p40_st(c, p, lin, v, w); c->r[B86_SP] = nsp;
        DONE(); }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint8_t *p = p40_pop_ptr(c, w, &lin);
        if (!p) NO();
        v = p40_ld(p, w);
        p40_sp_add(c, w);
        p40_setr(c, op & 7u, w, v);            /* POP ESP: the popped value wins */
        DONE(); }
    case 0x68: case 0x6A: {
        uint8_t *p;
        if (op == 0x68u) { if (!p40_imm(&d, w, &imm)) NO(); }
        else { if (!p40_u8(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
        p40_st(c, p, lin, imm, w); c->r[B86_SP] = nsp;
        DONE(); }
    case 0x69: case 0x6B: break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        if (!p40_u8(&d, &imm)) NO();
        v = c->ip + d.pos;
        if (p40_cond(c->flags, op & 15u)) v += (uint32_t)(int32_t)(int8_t)imm;
        if (d.o16) v &= 0xFFFFu;
        if (v > cs->limit) NO();
        JUMP(v);
    case 0x80: case 0x81: case 0x83: {
        const unsigned ww = op == 0x80u ? 1u : w;
        if (!p40_modrm(c, &d)) NO();
        if (op == 0x83u) { if (!p40_u8(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        else if (!p40_imm(&d, ww, &imm)) NO();
        if (!p40_rm_ptr(c, &d, ww, d.reg != 7u, &mm)) NO();
        v = p40_alu(c, d.reg, p40_rm_get(c, &d, &mm, ww), imm, ww);
        if (d.reg != 7u) p40_rm_put(c, &d, &mm, ww, v);
        DONE(); }
    case 0x84: case 0x85: {
        const unsigned ww = op == 0x84u ? 1u : w;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, ww, 0, &mm)) NO();
        (void)p40_alu(c, 4u, p40_rm_get(c, &d, &mm, ww), p40_getr(c, d.reg, ww), ww);
        DONE(); }
    case 0x86: case 0x87: {
        const unsigned ww = op == 0x86u ? 1u : w;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, ww, 1, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, ww); b = p40_getr(c, d.reg, ww);
        p40_rm_put(c, &d, &mm, ww, b); p40_setr(c, d.reg, ww, a);
        DONE(); }
    case 0x88: case 0x89: {
        const unsigned ww = op == 0x88u ? 1u : w;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, ww, 1, &mm)) NO();
        p40_rm_put(c, &d, &mm, ww, p40_getr(c, d.reg, ww));
        DONE(); }
    case 0x8A: case 0x8B: {
        const unsigned ww = op == 0x8Au ? 1u : w;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, ww, 0, &mm)) NO();
        p40_setr(c, d.reg, ww, p40_rm_get(c, &d, &mm, ww));
        DONE(); }
    case 0x8D:
        if (!p40_modrm(c, &d) || d.mod == 3u) NO();
        p40_setr(c, d.reg, w, d.ea);
        DONE();
    case 0x90: DONE();
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        a = p40_getr(c, 0u, w); p40_setr(c, 0u, w, p40_getr(c, op & 7u, w)); p40_setr(c, op & 7u, w, a);
        DONE();
    case 0x98:
        if (d.o16) p40_setr(c, 0u, 2u, (uint32_t)(int32_t)(int8_t)c->r[0]);
        else c->r[0] = (uint32_t)(int32_t)(int16_t)c->r[0];
        DONE();
    case 0x99:
        if (d.o16) p40_setr(c, 2u, 2u, (c->r[0] & 0x8000u) ? 0xFFFFu : 0u);
        else c->r[2] = (c->r[0] & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        DONE();
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        const unsigned ww = (op & 1u) ? w : 1u;
        uint8_t *p;
        if (!(d.a16 ? p40_u16(&d, &imm) : p40_u32(&d, &imm))) NO();
        if (!(p = p40_ptr(c, d.segov ? d.seg : B86_DS, imm, ww, op >= 0xA2u, &lin))) NO();
        if (op < 0xA2u) p40_setr(c, 0u, ww, p40_ld(p, ww));
        else p40_st(c, p, lin, p40_getr(c, 0u, ww), ww);
        DONE(); }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: {
        /* MOVS / STOS / LODS (REP for MOVS and STOS only) */
        const unsigned ww = (op & 1u) ? w : 1u;
        const unsigned kind = op <= 0xA5u ? 0u : op <= 0xABu ? 1u : 2u;
        const uint32_t step = (c->flags & B86_DF) ? (uint32_t)0u - ww : ww;
        const unsigned sseg = d.segov ? d.seg : B86_DS;
        const uint32_t am = d.a16 ? 0xFFFFu : 0xFFFFFFFFu;      /* SI/DI/CX vs ESI/EDI/ECX */
        unsigned done = 0u;
        int rep = d.rep != 0u;
#define P40_ADV(R_, n) (c->r[R_] = (c->r[R_] & ~am) | ((c->r[R_] + (n)) & am))
        if (d.rep == 0xF2u || (rep && kind == 2u)) NO();
        if (rep && (c->r[B86_CX] & am) == 0u) DONE();
        do {
            uint8_t *dp = NULL, *sp = NULL;
            uint32_t dl = 0u, sl = 0u;
            if (kind != 1u && !(sp = p40_ptr(c, sseg, c->r[B86_SI] & am, ww, 0, &sl))) break;
            if (kind != 2u && !(dp = p40_ptr(c, B86_ES, c->r[B86_DI] & am, ww, 1, &dl))) break;
            if (kind == 0u) { p40_st(c, dp, dl, p40_ld(sp, ww), ww); P40_ADV(B86_SI, step); P40_ADV(B86_DI, step); }
            else if (kind == 1u) { p40_st(c, dp, dl, p40_getr(c, 0u, ww), ww); P40_ADV(B86_DI, step); }
            else { p40_setr(c, 0u, ww, p40_ld(sp, ww)); P40_ADV(B86_SI, step); }
            ++done;
            if (rep) P40_ADV(B86_CX, 0xFFFFFFFFu);
        } while (rep && (c->r[B86_CX] & am) && done < B86_PM40_REP);
#undef P40_ADV
        if (!done) NO();
        if (rep && (c->r[B86_CX] & am)) {      /* resume here; retires when done */
#ifdef B86_PM40_VERIFY
            if (done < B86_PM40_REP) p40_short = 1u;   /* stopped early before a page/limit edge */
#endif
            ++b86_pm40_stat_ok; return B86_OK;
        }
        DONE(); }
    case 0xA8: case 0xA9: {
        const unsigned ww = op == 0xA8u ? 1u : w;
        if (!p40_imm(&d, ww, &imm)) NO();
        (void)p40_alu(c, 4u, p40_getr(c, 0u, ww), imm, ww);
        DONE(); }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        if (!p40_u8(&d, &imm)) NO();
        p40_setr(c, op & 7u, 1u, imm);
        DONE();
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (!p40_imm(&d, w, &imm)) NO();
        p40_setr(c, op & 7u, w, imm);
        DONE();
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const unsigned ww = (op & 1u) ? w : 1u;
        unsigned n;
        if (!p40_modrm(c, &d)) NO();
        if (op <= 0xC1u) { if (!p40_u8(&d, &imm)) NO(); n = imm; }
        else n = op <= 0xD1u ? 1u : (c->r[B86_CX] & 0xFFu);
        if (d.reg == 2u || d.reg == 3u) NO();
        if (!p40_rm_ptr(c, &d, ww, 1, &mm)) NO();
        if (!p40_shift(c, d.reg, p40_rm_get(c, &d, &mm, ww), n, ww, &v)) NO();
        if (n & 31u) p40_rm_put(c, &d, &mm, ww, v);
        DONE(); }
    case 0xC2: case 0xC3: {
        uint8_t *p;
        if (op == 0xC2u && !p40_u16(&d, &imm)) NO();
        if (!(p = p40_pop_ptr(c, w, &lin))) NO();
        v = p40_ld(p, w);
        if (v > cs->limit) NO();
        p40_sp_add(c, w + (op == 0xC2u ? imm : 0u));
        JUMP(v); }
    case 0xC6: case 0xC7: {
        const unsigned ww = op == 0xC6u ? 1u : w;
        if (!p40_modrm(c, &d) || d.reg != 0u || !p40_imm(&d, ww, &imm)) NO();
        if (!p40_rm_ptr(c, &d, ww, 1, &mm)) NO();
        p40_rm_put(c, &d, &mm, ww, imm);
        DONE(); }
    case 0xC9: {                                /* LEAVE (32-bit) */
        uint32_t sl;
        uint8_t *p;
        const int s32 = (c->pm386.seg[B86_SS].flags & 4u) != 0u;
        if (!(p = p40_ptr(c, B86_SS, s32 ? c->r[B86_BP] : (c->r[B86_BP] & 0xFFFFu), w, 0, &sl))) NO();
        v = p40_ld(p, w);
        if (s32) c->r[B86_SP] = c->r[B86_BP];
        else c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (c->r[B86_BP] & 0xFFFFu);
        p40_sp_add(c, w);
        p40_setr(c, B86_BP, w, v);
        DONE(); }
    case 0xE4: case 0xE6: case 0xEC: case 0xEE: {   /* byte IN/OUT, CPL <= IOPL */
        unsigned port;
#ifdef B86_PM40_VERIFY
        NO();                                    /* port side effects cannot be replayed */
#endif
        if ((c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) NO();
        if (op <= 0xE6u) { if (!p40_u8(&d, &imm)) NO(); port = imm; }
        else port = c->r[B86_DX] & 0xFFFFu;
        if (op == 0xE4u || op == 0xECu) {
            if (!c->in8) NO();
            p40_setr(c, 0u, 1u, c->in8(c, (uint16_t)port));
        } else { if (!c->out8) NO(); c->out8(c, (uint16_t)port, (uint8_t)c->r[0]); }
        DONE(); }
    case 0xE8: case 0xE9: case 0xEB: {
        if (op == 0xEBu) { if (!p40_u8(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        else if (d.o16) { if (!p40_u16(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int16_t)imm; }
        else if (!p40_u32(&d, &imm)) NO();
        v = c->ip + d.pos + imm;                /* target */
        if (d.o16) v &= 0xFFFFu;
        if (v > cs->limit) NO();
        if (op == 0xE8u) {
            uint8_t *p = p40_push_ptr(c, w, &nsp, &lin);
            if (!p) NO();
            p40_st(c, p, lin, NEXTIP(), w); c->r[B86_SP] = nsp;
        }
        JUMP(v); }
    case 0xF5: c->flags ^= B86_CF; DONE();
    case 0xF8: c->flags &= ~(uint32_t)B86_CF; DONE();
    case 0xF9: c->flags |= B86_CF; DONE();
    case 0xFC: c->flags &= ~(uint32_t)B86_DF; DONE();
    case 0xFD: c->flags |= B86_DF; DONE();
    case 0xF6: case 0xF7: {
        const unsigned ww = op == 0xF6u ? 1u : w;
        const uint32_t m = p40_mask(ww);
        if (!p40_modrm(c, &d)) NO();
        if (d.reg == 1u) NO();
        if (d.reg == 0u && !p40_imm(&d, ww, &imm)) NO();
        if (!p40_rm_ptr(c, &d, ww, d.reg == 2u || d.reg == 3u, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, ww);
        switch (d.reg) {
        case 0: (void)p40_alu(c, 4u, a, imm, ww); break;
        case 2: p40_rm_put(c, &d, &mm, ww, ~a & m); break;
        case 3: v = p40_alu(c, 5u, 0u, a, ww); p40_rm_put(c, &d, &mm, ww, v); break;
        case 4: case 5: {
            uint32_t lo, hi, f;
            int ovf;
            if (d.reg == 4u) {
                const uint64_t pr = (uint64_t)p40_getr(c, 0u, ww) * a;
                lo = (uint32_t)pr & m; hi = (uint32_t)(pr >> (ww * 8u)) & m; ovf = hi != 0u;
            } else {
                const int64_t sa = ww == 4u ? (int32_t)a : ww == 2u ? (int16_t)a : (int8_t)a;
                const uint32_t acc = p40_getr(c, 0u, ww);
                const int64_t sb = ww == 4u ? (int32_t)acc : ww == 2u ? (int16_t)acc : (int8_t)acc;
                const int64_t pr = sa * sb;
                const int64_t lo_s = ww == 4u ? (int32_t)pr : ww == 2u ? (int16_t)pr : (int8_t)pr;
                lo = (uint32_t)pr & m; hi = (uint32_t)((uint64_t)pr >> (ww * 8u)) & m; ovf = lo_s != pr;
            }
            if (ww == 1u) p40_setr(c, 0u, 2u, lo | (hi << 8));
            else { p40_setr(c, 0u, ww, lo); p40_setr(c, 2u, ww, hi); }
            f = ovf ? (uint32_t)(B86_CF | B86_OF) : 0u;
            p40_setflags(c, f | p40_szp(lo, ww));
            c->pm40_undef |= B86_SF | B86_ZF | B86_AF | B86_PF;
            break; }
        default: {                               /* DIV / IDIV */
            if (!(a & m)) NO();
            {   /* r73i: hardware UDIV/SDIV + divlu instead of libgcc 64-bit division */
                uint32_t q, r;
                const uint32_t dlo = ww == 1u ? c->r[0] : p40_getr(c, 0u, ww), dhi = ww == 1u ? 0u : p40_getr(c, 2u, ww);
                if (!b86_divx(dlo, dhi, a, ww, d.reg == 7u, &q, &r)) NO();
                if (ww == 1u) p40_setr(c, 0u, 2u, (q & 0xFFu) | ((r & 0xFFu) << 8));
                else { p40_setr(c, 0u, ww, q); p40_setr(c, 2u, ww, r); }
            }
            c->pm40_undef |= B86_ARITH;
            break; }
        }
        DONE(); }
    case 0xFE: case 0xFF: {
        const unsigned ww = op == 0xFEu ? 1u : w;
        if (!p40_modrm(c, &d)) NO();
        if (d.reg <= 1u) {
            const uint32_t cf = c->flags & B86_CF;
            if (!p40_rm_ptr(c, &d, ww, 1, &mm)) NO();
            v = p40_alu(c, d.reg ? 5u : 0u, p40_rm_get(c, &d, &mm, ww), 1u, ww);
            c->flags = (c->flags & ~(uint32_t)B86_CF) | cf;
            p40_rm_put(c, &d, &mm, ww, v);
            DONE();
        }
        if (op == 0xFEu) NO();
        if (d.reg == 2u || d.reg == 4u || d.reg == 6u) {
            uint8_t *p;
            if (!p40_rm_ptr(c, &d, w, 0, &mm)) NO();
            v = p40_rm_get(c, &d, &mm, w);
            if (d.reg == 4u) { if (v > cs->limit) NO(); JUMP(v); }
            if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
            if (d.reg == 6u) { p40_st(c, p, lin, v, w); c->r[B86_SP] = nsp; DONE(); }
            if (v > cs->limit) NO();
            p40_st(c, p, lin, NEXTIP(), w); c->r[B86_SP] = nsp;
            JUMP(v);
        }
        NO(); }
    case 0x06: case 0x0E: case 0x16: case 0x1E: {     /* PUSH Sreg (zero-extended) */
        uint8_t *p;
        if (c->seg[B86_CS] & 3u) NO();
        if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
        p40_st(c, p, lin, p40_sel(c, op == 0x06u ? B86_ES : op == 0x0Eu ? B86_CS : op == 0x16u ? B86_SS : B86_DS), w);
        c->r[B86_SP] = nsp;
        DONE(); }
    case 0x07: case 0x1F: {                           /* POP ES/DS */
        B86PM2Plan plan;
        uint8_t *p = p40_pop_ptr(c, w, &lin);
        if (!p || !p40_plan(c, op == 0x07u ? B86_ES : B86_DS, (uint16_t)p40_ld(p, 2u), &plan)) NO();
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) NO();
        p40_sp_add(c, w);
        DONE(); }
    case 0x60: {                                      /* PUSHA/PUSHAD */
        uint8_t *ps[8]; uint32_t ls[8], sp0 = c->r[B86_SP], k;
        const int s32 = (c->pm386.seg[B86_SS].flags & 4u) != 0u;
        for (k = 0; k < 8u; ++k) {
            const uint32_t a = sp0 - w * (k + 1u);
            if (!(ps[k] = p40_ptr(c, B86_SS, s32 ? a : (a & 0xFFFFu), w, 1, &ls[k]))) NO();
        }
        for (k = 0; k < 8u; ++k) p40_st(c, ps[k], ls[k], k == 4u ? (w == 4u ? sp0 : sp0 & 0xFFFFu) : p40_getr(c, k, w), w);
        p40_sp_add(c, 0u - 8u * w);
        DONE(); }
    case 0x61: {                                      /* POPA/POPAD (SP not loaded) */
        uint8_t *ps[8]; uint32_t ls[8], sp0 = c->r[B86_SP], k;
        const int s32 = (c->pm386.seg[B86_SS].flags & 4u) != 0u;
        for (k = 0; k < 8u; ++k) {
            const uint32_t a = sp0 + w * k;
            if (!(ps[k] = p40_ptr(c, B86_SS, s32 ? a : (a & 0xFFFFu), w, 0, &ls[k]))) NO();
        }
        for (k = 0; k < 8u; ++k) if (k != 3u) p40_setr(c, 7u - k, w, p40_ld(ps[k], w));
        p40_sp_add(c, 8u * w);
        DONE(); }
    case 0x8C: {                                      /* MOV r/m, Sreg */
        if (!p40_modrm(c, &d) || d.reg > 5u) NO();
        if (d.mod == 3u) { p40_setr(c, d.rm, w, p40_sel(c, d.reg)); DONE(); }
        if (!p40_rm_ptr(c, &d, 2u, 1, &mm)) NO();
        p40_st(c, mm.p, mm.lin, p40_sel(c, d.reg), 2u);
        DONE(); }
    case 0x8E: {                                      /* MOV Sreg, r/m (not CS/SS) */
        B86PM2Plan plan;
        if (!p40_modrm(c, &d) || d.reg > 5u || d.reg == B86_CS || d.reg == B86_SS) NO();
        if (d.mod == 3u && d.segov) NO();
        if (!p40_rm_ptr(c, &d, 2u, 0, &mm)) NO();
        if (!p40_plan(c, d.reg, (uint16_t)p40_rm_get(c, &d, &mm, 2u), &plan)) NO();
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) NO();
        DONE(); }
    case 0x8F: {                                      /* POP r/m */
        uint8_t *p;
        if (!p40_modrm(c, &d) || d.reg != 0u) NO();
        if (d.mod != 3u && !d.a16 && (d.rm == 4u || d.easeg == B86_SS)) NO();   /* ESP-relative: leave to the chain */
        if (!(p = p40_pop_ptr(c, w, &lin))) NO();
        v = p40_ld(p, w);
        if (!p40_rm_ptr(c, &d, w, 1, &mm)) NO();
        p40_sp_add(c, w);
        p40_rm_put(c, &d, &mm, w, v);
        DONE(); }
    case 0x9C: {                                      /* PUSHF/PUSHFD (VM, RF read as 0) */
        uint8_t *p;
        if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
        p40_st(c, p, lin, (c->flags | 2u) & 0x0004FFD7u & ~0x30000u, w);
        c->r[B86_SP] = nsp;
        DONE(); }
    case 0xC4: case 0xC5: {                           /* LES / LDS */
        B86PM2Plan plan;
        uint8_t *p, *ps;
        uint32_t ls;
        if (!p40_modrm(c, &d) || d.mod == 3u) NO();
        if (!(p = p40_ptr(c, d.easeg, d.ea, w, 0, &lin))) NO();
        if (!(ps = p40_ptr(c, d.easeg, (d.ea + w) & (d.a16 ? 0xFFFFu : 0xFFFFFFFFu), 2u, 0, &ls))) NO();
        if (!p40_plan(c, op == 0xC4u ? B86_ES : B86_DS, (uint16_t)p40_ld(ps, 2u), &plan)) NO();
        v = p40_ld(p, w);
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) NO();
        p40_setr(c, d.reg, w, v);
        DONE(); }
    case 0xC8: {                                      /* ENTER imm16, 0 */
        uint8_t *p;
        uint32_t frame, sz;
        if (!p40_u16(&d, &sz) || !p40_u8(&d, &imm) || (imm & 31u)) NO();
        if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
        p40_st(c, p, lin, p40_getr(c, B86_BP, w), w);
        c->r[B86_SP] = nsp;
        frame = c->r[B86_SP];
        p40_setr(c, B86_BP, w, frame);
        p40_sp_add(c, 0u - sz);
        DONE(); }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {     /* LOOPNZ/LOOPZ/LOOP/JCXZ */
        const uint32_t am = d.a16 ? 0xFFFFu : 0xFFFFFFFFu;
        int take;
        if (!p40_u8(&d, &imm)) NO();
        v = c->ip + d.pos + (uint32_t)(int32_t)(int8_t)imm;
        if (d.o16) v &= 0xFFFFu;
        if (op == 0xE3u) take = (c->r[B86_CX] & am) == 0u;
        else {
            const uint32_t n = (c->r[B86_CX] - 1u) & am;
            take = n != 0u && (op == 0xE2u || (op == 0xE1u ? (c->flags & B86_ZF) != 0u : (c->flags & B86_ZF) == 0u));
            if (take && v > cs->limit) NO();
            c->r[B86_CX] = (c->r[B86_CX] & ~am) | n;
        }
        if (take) { if (v > cs->limit) NO(); JUMP(v); }
        DONE(); }
    case 0xFA: case 0xFB:                             /* CLI / STI (CPL <= IOPL) */
        if ((c->seg[B86_CS] & 3u) > ((c->flags >> 12) & 3u)) NO();
        if (op == 0xFAu) c->flags &= ~(uint32_t)B86_IF; else c->flags |= B86_IF;
        DONE();
    case 0x0F: break;
    default: NO();
    }

    /* ---- IMUL r, r/m, imm ---------------------------------------------- */
    if (op == 0x69u || op == 0x6Bu) {
        int64_t pr, lo_s;
        if (!p40_modrm(c, &d)) NO();
        if (op == 0x6Bu) { if (!p40_u8(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int8_t)imm; }
        else if (!p40_imm(&d, w, &imm)) NO();
        if (!p40_rm_ptr(c, &d, w, 0, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, w);
        pr = (int64_t)(w == 4u ? (int32_t)a : (int16_t)a) * (int64_t)(w == 4u ? (int32_t)imm : (int16_t)imm);
        lo_s = w == 4u ? (int32_t)pr : (int16_t)pr;
        p40_setr(c, d.reg, w, (uint32_t)pr);
        p40_setflags(c, (lo_s != pr ? (uint32_t)(B86_CF | B86_OF) : 0u) | p40_szp((uint32_t)pr & p40_mask(w), w));
        c->pm40_undef |= B86_SF | B86_ZF | B86_AF | B86_PF;
        DONE();
    }

    /* ---- 0F xx ---------------------------------------------------------- */
    if (!p40_u8(&d, &op)) NO();
    if (op >= 0x80u && op <= 0x8Fu) {
        if (d.o16) { if (!p40_u16(&d, &imm)) NO(); imm = (uint32_t)(int32_t)(int16_t)imm; }
        else if (!p40_u32(&d, &imm)) NO();
        v = c->ip + d.pos;
        if (p40_cond(c->flags, op & 15u)) v += imm;
        if (d.o16) v &= 0xFFFFu;
        if (v > cs->limit) NO();
        JUMP(v);
    }
    if (op >= 0x90u && op <= 0x9Fu) {
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, 1u, 1, &mm)) NO();
        p40_rm_put(c, &d, &mm, 1u, (uint32_t)p40_cond(c->flags, op & 15u));
        DONE();
    }
    switch (op) {
    case 0xA0: case 0xA8: {                            /* PUSH FS/GS */
        uint8_t *p;
        if (c->seg[B86_CS] & 3u) NO();
        if (!(p = p40_push_ptr(c, w, &nsp, &lin))) NO();
        p40_st(c, p, lin, p40_sel(c, op == 0xA0u ? B86_FS : B86_GS), w);
        c->r[B86_SP] = nsp;
        DONE(); }
    case 0xA1: case 0xA9: {                            /* POP FS/GS */
        B86PM2Plan plan;
        uint8_t *p = p40_pop_ptr(c, w, &lin);
        if (!p || !p40_plan(c, op == 0xA1u ? B86_FS : B86_GS, (uint16_t)p40_ld(p, 2u), &plan)) NO();
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) NO();
        p40_sp_add(c, w);
        DONE(); }
    case 0xB4: case 0xB5: {                            /* LFS / LGS */
        B86PM2Plan plan;
        uint8_t *p, *ps;
        uint32_t ls;
        if (!p40_modrm(c, &d) || d.mod == 3u) NO();
        if (!(p = p40_ptr(c, d.easeg, d.ea, w, 0, &lin))) NO();
        if (!(ps = p40_ptr(c, d.easeg, (d.ea + w) & (d.a16 ? 0xFFFFu : 0xFFFFFFFFu), 2u, 0, &ls))) NO();
        if (!p40_plan(c, op == 0xB4u ? B86_FS : B86_GS, (uint16_t)p40_ld(ps, 2u), &plan)) NO();
        v = p40_ld(p, w);
        if (b86_pm2_commit_segment(c, &plan, 0u) != B86_PM2_OK) NO();
        p40_setr(c, d.reg, w, v);
        DONE(); }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {   /* BT/BTS/BTR/BTC */
        const unsigned bits = w * 8u;
        unsigned kind, bit;
        uint32_t ea0;
        if (!p40_modrm(c, &d)) NO();
        if (op == 0xBAu) {
            if (d.reg < 4u || !p40_u8(&d, &imm)) NO();
            kind = d.reg - 4u; bit = imm & (bits - 1u);
        } else {
            const uint32_t off = p40_getr(c, d.reg, w);
            kind = op == 0xA3u ? 0u : op == 0xABu ? 1u : op == 0xB3u ? 2u : 3u;
            if (d.mod == 3u) bit = off & (bits - 1u);
            else {                                    /* bit string: signed offset moves the operand */
                const int32_t so = w == 4u ? (int32_t)off : (int16_t)off;
                ea0 = d.ea + (uint32_t)((so >> (w == 4u ? 5 : 4)) * (int32_t)w);
                d.ea = d.a16 ? (ea0 & 0xFFFFu) : ea0;
                bit = (uint32_t)so & (bits - 1u);
            }
        }
        if (!p40_rm_ptr(c, &d, w, kind != 0u, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, w);
        c->flags = (c->flags & ~(uint32_t)B86_CF) | ((a >> bit) & 1u);
        c->pm40_undef |= B86_OF | B86_SF | B86_ZF | B86_AF | B86_PF;
        if (kind) {
            const uint32_t m1 = 1u << bit;
            p40_rm_put(c, &d, &mm, w, kind == 1u ? a | m1 : kind == 2u ? a & ~m1 : a ^ m1);
        }
        DONE(); }
    case 0xAF: {
        int64_t pr, lo_s;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, w, 0, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, w); b = p40_getr(c, d.reg, w);
        pr = (int64_t)(w == 4u ? (int32_t)a : (int16_t)a) * (int64_t)(w == 4u ? (int32_t)b : (int16_t)b);
        lo_s = w == 4u ? (int32_t)pr : (int16_t)pr;
        p40_setr(c, d.reg, w, (uint32_t)pr);
        p40_setflags(c, (lo_s != pr ? (uint32_t)(B86_CF | B86_OF) : 0u) | p40_szp((uint32_t)pr & p40_mask(w), w));
        c->pm40_undef |= B86_SF | B86_ZF | B86_AF | B86_PF;
        DONE(); }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        const unsigned sw = (op & 1u) ? 2u : 1u;
        if (!p40_modrm(c, &d) || !p40_rm_ptr(c, &d, sw, 0, &mm)) NO();
        a = p40_rm_get(c, &d, &mm, sw);
        if (op >= 0xBEu) a = sw == 1u ? (uint32_t)(int32_t)(int8_t)a : (uint32_t)(int32_t)(int16_t)a;
        p40_setr(c, d.reg, w, a);
        DONE(); }
    default: NO();
    }
#undef DONE
#undef NO
#undef JUMP
#undef NEXTIP
}

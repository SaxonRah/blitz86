/* Thumb-2 backend (RP2350 / Cortex-M33; also runs on ARMv7-A for testing).
 *
 * Register map (resident across chained blocks):
 *   r0..r7  AX CX DX BX SP BP SI DI
 *   r8      B86Cpu *
 *   r9      codemap biased by host address >> 6
 *   r10     DS base pointer   r11  SS base pointer   (ES/CS loaded on use)
 *   r12     V_T0              lr   V_T1               (no V_T2)
 * Only 32-bit encodings with S=0 are used for non-flag work, so NZCV
 * survives ordinary ALU ops. Stores with an SMC check clobber NZCV; the
 * frontend knows (be_store_clobbers_nzcv). Exit reason travels in r12.
 */
#include "backend.h"
#include <string.h>

const int be_has_t2 = 0;
const int be_store_clobbers_nzcv = 1;
const int be_logic_mode = FM_SUB;      /* LSL x,r,#sh ; CMP x,#0 : C=1 V=0 */

#define OFF(f) ((unsigned)offsetof(B86Cpu, f))
#define R_CTX 8
#define R_CM 9
#define R_DS 10
#define R_SS 11
#define R12 12
#define LR 14
#define PC 15

static int hr(int v) { return v < 8 ? v : (v == V_T0 ? R12 : LR); }

static void put16(Emit *e, uint32_t hw)
{
    if (e->p + 2 > e->end) { e->overflow = 1; return; }
    e->p[0] = (uint8_t)hw; e->p[1] = (uint8_t)(hw >> 8);
    e->p += 2;
}
static void put32(Emit *e, uint32_t h1, uint32_t h2) { put16(e, h1); put16(e, h2); }

/* ---- immediates --------------------------------------------------------- */
static int modimm(uint32_t v, uint32_t *enc)
{
    uint32_t b = v & 0xFF;
    if (v < 256) { *enc = v; return 1; }
    if (b && v == (b | b << 16)) { *enc = 0x100 | b; return 1; }
    uint32_t c = (v >> 8) & 0xFF;
    if (c && v == (c << 8 | c << 24)) { *enc = 0x200 | c; return 1; }
    if (v == b * 0x01010101u) { *enc = 0x300 | b; return 1; }
    for (unsigned rot = 8; rot < 32; ++rot) {
        uint32_t u = (v << rot) | (v >> (32 - rot));
        if (u >= 0x80 && u <= 0xFF) { *enc = rot << 7 | (u & 0x7F); return 1; }
    }
    return 0;
}

enum { DP_AND = 0, DP_BIC = 1, DP_ORR = 2, DP_ORN = 3, DP_EOR = 4, DP_ADD = 8, DP_SUB = 13, DP_RSB = 14 };

static void dp_imm(Emit *e, int op, int s, int d, int n, uint32_t enc)
{
    put32(e, 0xF000u | (enc >> 11 & 1) << 10 | (uint32_t)op << 5 | (uint32_t)s << 4 | (uint32_t)n,
             (enc >> 8 & 7) << 12 | (uint32_t)d << 8 | (enc & 0xFF));
}
static void dp_reg(Emit *e, int op, int s, int d, int n, int m, int type, int amt)
{
    put32(e, 0xEA00u | (uint32_t)op << 5 | (uint32_t)s << 4 | (uint32_t)n,
             (uint32_t)(amt >> 2 & 7) << 12 | (uint32_t)d << 8 | (uint32_t)(amt & 3) << 6 | (uint32_t)type << 4 | (uint32_t)m);
}
static void movw(Emit *e, int d, uint32_t v)
{
    v &= 0xFFFF;
    put32(e, 0xF240u | (v >> 11 & 1) << 10 | (v >> 12), (v >> 8 & 7) << 12 | (uint32_t)d << 8 | (v & 0xFF));
}
static void movt(Emit *e, int d, uint32_t v)
{
    v &= 0xFFFF;
    put32(e, 0xF2C0u | (v >> 11 & 1) << 10 | (v >> 12), (v >> 8 & 7) << 12 | (uint32_t)d << 8 | (v & 0xFF));
}
static void imm32(Emit *e, int d, uint32_t v) { movw(e, d, v); if (v >> 16) movt(e, d, v >> 16); }
static void addw(Emit *e, int d, int n, uint32_t v, int sub)
{
    put32(e, (sub ? 0xF2A0u : 0xF200u) | (v >> 11 & 1) << 10 | (uint32_t)n, (v >> 8 & 7) << 12 | (uint32_t)d << 8 | (v & 0xFF));
}
static void mov(Emit *e, int d, int s) { if (d != s) put32(e, 0xEA4Fu, (uint32_t)d << 8 | (uint32_t)s); }
static void shift_imm(Emit *e, int s_flag, int d, int m, int type, unsigned n)
{
    put32(e, s_flag ? 0xEA5Fu : 0xEA4Fu, (n >> 2 & 7) << 12 | (uint32_t)d << 8 | (n & 3) << 6 | (uint32_t)type << 4 | (uint32_t)m);
}
static void bitfield(Emit *e, uint32_t h1, int d, int n, unsigned lsb, unsigned last)
{
    put32(e, h1 | (uint32_t)n, (lsb >> 2 & 7) << 12 | (uint32_t)d << 8 | (lsb & 3) << 6 | last);
}
static void ldst(Emit *e, uint32_t h1, int t, int n, unsigned off) { put32(e, h1 | (uint32_t)n, (uint32_t)t << 12 | (off & 0xFFF)); }
#define LDRW 0xF8D0u
#define STRW 0xF8C0u
#define LDRH 0xF8B0u
#define STRH 0xF8A0u
#define LDRB 0xF890u
#define STRB 0xF880u
static void cmp_imm0(Emit *e, int n) { dp_imm(e, DP_SUB, 1, PC, n, 0); }

/* ---- branches ------------------------------------------------------------ */
static void enc_b(uint8_t *site, uint8_t *target)
{
    int32_t off = (int32_t)(target - (site + 4));
    uint32_t S = (uint32_t)(off >> 24) & 1, I1 = (uint32_t)(off >> 23) & 1, I2 = (uint32_t)(off >> 22) & 1;
    uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
    uint32_t h1 = 0xF000u | S << 10 | ((uint32_t)(off >> 12) & 0x3FF);
    uint32_t h2 = 0x9000u | J1 << 13 | J2 << 11 | ((uint32_t)(off >> 1) & 0x7FF);
    site[0] = (uint8_t)h1; site[1] = (uint8_t)(h1 >> 8); site[2] = (uint8_t)h2; site[3] = (uint8_t)(h2 >> 8);
}
static void enc_bcc(uint8_t *site, uint8_t *target, uint32_t cond)
{
    int32_t off = (int32_t)(target - (site + 4));
    if (off < -(1 << 20) || off >= (1 << 20)) __builtin_trap();
    uint32_t S = (uint32_t)(off >> 20) & 1, J2 = (uint32_t)(off >> 19) & 1, J1 = (uint32_t)(off >> 18) & 1;
    uint32_t h1 = 0xF000u | S << 10 | cond << 6 | ((uint32_t)(off >> 12) & 0x3F);
    uint32_t h2 = 0x8000u | J1 << 13 | J2 << 11 | ((uint32_t)(off >> 1) & 0x7FF);
    site[0] = (uint8_t)h1; site[1] = (uint8_t)(h1 >> 8); site[2] = (uint8_t)h2; site[3] = (uint8_t)(h2 >> 8);
}
static uint8_t *emit_b(Emit *e, uint8_t *target)
{
    uint8_t *s = e->p;
    if (e->p + 4 > e->end) { e->overflow = 1; return s; }
    enc_b(s, target ? target : s + 4); e->p += 4; return s;
}
static uint8_t *emit_bcc(Emit *e, int cond, uint8_t *target)
{
    uint8_t *s = e->p;
    if (e->p + 4 > e->end) { e->overflow = 1; return s; }
    enc_bcc(s, target ? target : s + 4, (uint32_t)cond); e->p += 4; return s;
}

void be_bind(Emit *e, uint8_t *site, uint8_t *target)
{
    (void)e;
    if (!site) return;
    uint32_t h1 = site[0] | site[1] << 8, h2 = site[2] | site[3] << 8;
    if ((h2 & 0xD000u) == 0x9000u) enc_b(site, target);
    else enc_bcc(site, target, (h1 >> 6) & 0xF);
}

void be_flush_icache(void *start, size_t len)
{
#if defined(__linux__)
    __builtin___clear_cache((char *)start, (char *)start + len);
#else
    (void)start; (void)len;
    __asm__ volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
}
void be_patch_branch(uint8_t *site, uint8_t *target) { enc_b(site, target); be_flush_icache(site, 4); }
void be_kill_entry(uint8_t *entry, uint8_t *stub) { be_patch_branch(entry, stub); }
uintptr_t be_code_ptr(uint8_t *p) { return (uintptr_t)p | 1u; }

/* ---- runtime -------------------------------------------------------------- */

static void call_abs(Emit *e, void *fn)
{
    imm32(e, R12, (uint32_t)(uintptr_t)fn);
    put16(e, 0x4780u | R12 << 3);                    /* BLX r12 */
}
static void reload_segs(Emit *e)
{
    ldst(e, LDRW, R_DS, R_CTX, OFF(segp[B86_DS]));
    ldst(e, LDRW, R_SS, R_CTX, OFF(segp[B86_SS]));
}
static void retire_mark(Emit *e, uint32_t n)
{
    if (!e->count_exits) return;
    movw(e, LR, n);
    ldst(e, STRW, LR, R_CTX, OFF(retired));
}

void *be_emit_runtime(Emit *e)
{
    if ((uintptr_t)e->p & 2) put16(e, 0xBF00);
    uint8_t *enter = e->p;
    put32(e, 0xE92Du, 0x4FF8u);                      /* PUSH {r3-r11, lr} */
    mov(e, R_CTX, 0);
    mov(e, R12, 1);
    ldst(e, LDRW, R_CM, R_CTX, OFF(codemap_host));
    reload_segs(e);
    put32(e, 0xE890u | R_CTX, 0x00FFu);              /* LDM r8, {r0-r7} */
    put16(e, 0x4700u | R12 << 3);                    /* BX r12 */

    e->x_epilogue = e->p;
    put32(e, 0xE880u | R_CTX, 0x00FFu);              /* STM r8, {r0-r7} */
    mov(e, 0, R12);
    put32(e, 0xE8BDu, 0x8FF8u);                      /* POP {r3-r11, pc} */

    e->x_ipexit = e->p;                              /* r12 = ip, lr = reason */
    put32(e, 0xFA1Fu, 0xF080u | R12 << 8 | R12);     /* UXTH r12, r12 */
    ldst(e, STRW, R12, R_CTX, OFF(ip));
    mov(e, R12, LR);
    emit_b(e, e->x_epilogue);

    e->x_dynexit = e->p;
    movw(e, R12, XR_LOOKUP);
    emit_b(e, e->x_epilogue);
    e->x_miss = e->x_dynexit;                        /* lookup stores ip first */

    e->x_chain = e->p;                               /* r12 = ip, lr = site */
    ldst(e, STRW, LR, R_CTX, OFF(patch));
    movw(e, LR, XR_CHAIN);
    emit_b(e, e->x_ipexit);

    e->x_irq = e->p;                                 /* r12 = ip */
    movw(e, LR, XR_IRQ);
    emit_b(e, e->x_ipexit);

    e->x_lookup = e->p;                              /* r12 = ip */
    put32(e, 0xFA1Fu, 0xF080u | R12 << 8 | R12);     /* UXTH r12, r12 */
    ldst(e, STRW, R12, R_CTX, OFF(ip));
    ldst(e, LDRW, LR, R_CTX, OFF(irq));
    cmp_imm0(e, LR);
    emit_bcc(e, AC_NE, e->x_irq);
    ldst(e, LDRW, LR, R_CTX, OFF(seg[B86_CS]));
    dp_reg(e, DP_ORR, 0, R12, R12, LR, 0, 16);       /* key */
    put16(e, 0xB401u);                               /* PUSH {r0} */
    dp_reg(e, DP_EOR, 0, 0, R12, R12, 1, 16);        /* r0 = key ^ key>>16 */
    bitfield(e, 0xF3C0u, 0, 0, 0, 11);               /* UBFX r0, r0, #0, #12 */
    ldst(e, LDRW, LR, R_CTX, OFF(fast));
    dp_reg(e, DP_ADD, 0, LR, LR, 0, 0, 3);           /* lr += idx*8 */
    ldst(e, LDRW, 0, LR, 0);
    put32(e, 0xEBB0u | 0, 0x0F00u | R12);            /* CMP r0, r12 */
    put16(e, 0xBC01u);                               /* POP {r0} */
    emit_bcc(e, AC_NE, e->x_miss);
    ldst(e, LDRW, LR, LR, 4);
    put16(e, 0x4700u | LR << 3);                     /* BX lr */
    return (void *)((uintptr_t)enter | 1u);
}

/* ---- ALU ------------------------------------------------------------------ */

void be_mov(Emit *e, int d, int s) { mov(e, hr(d), hr(s)); }
void be_movi(Emit *e, int d, uint32_t imm) { imm32(e, hr(d), imm); }

static int dp_of(int aop)
{
    switch (aop) { case AOP_ADD: return DP_ADD; case AOP_SUB: return DP_SUB; case AOP_AND: return DP_AND;
                   case AOP_ORR: return DP_ORR; default: return DP_EOR; }
}
void be_op(Emit *e, int aop, int d, int a, int b) { dp_reg(e, dp_of(aop), 0, hr(d), hr(a), hr(b), 0, 0); }

static void opi_raw(Emit *e, int aop, int d, int a, uint32_t imm)
{
    uint32_t enc;
    imm &= 0xFFFF;
    if (aop == AOP_ADD || aop == AOP_SUB) {
        int sub = aop == AOP_SUB;
        uint32_t neg = (0x10000 - imm) & 0xFFFF;
        if (imm == 0) { mov(e, d, a); return; }
        if (imm < 4096) { addw(e, d, a, imm, sub); return; }
        if (neg < 4096) { addw(e, d, a, neg, !sub); return; }
        if (modimm(imm, &enc)) { dp_imm(e, sub ? DP_SUB : DP_ADD, 0, d, a, enc); return; }
        addw(e, d, a, imm & 0xFFF, sub);
        modimm(imm & 0xF000, &enc);
        dp_imm(e, sub ? DP_SUB : DP_ADD, 0, d, d, enc);
        return;
    }
    if (aop == AOP_AND) {
        if (modimm(imm, &enc) || modimm(imm | 0xFFFF0000u, &enc)) { dp_imm(e, DP_AND, 0, d, a, enc); return; }
        uint32_t lo = ~imm & 0xFF, hi = ~imm & 0xFF00;
        int src = a;
        if (lo) { modimm(lo, &enc); dp_imm(e, DP_BIC, 0, d, src, enc); src = d; }
        if (hi) { modimm(hi, &enc); dp_imm(e, DP_BIC, 0, d, src, enc); src = d; }
        if (src != d) mov(e, d, a);
        return;
    }
    int op = aop == AOP_ORR ? DP_ORR : DP_EOR;
    if (imm == 0) { mov(e, d, a); return; }
    if (modimm(imm, &enc)) { dp_imm(e, op, 0, d, a, enc); return; }
    modimm(imm & 0xFF, &enc); dp_imm(e, op, 0, d, a, enc);
    modimm(imm & 0xFF00, &enc); dp_imm(e, op, 0, d, d, enc);
}
void be_opi(Emit *e, int aop, int d, int a, uint32_t imm) { opi_raw(e, aop, hr(d), hr(a), imm); }

void be_mvn(Emit *e, int d, int s) { put32(e, 0xEA6Fu, (uint32_t)hr(d) << 8 | (uint32_t)hr(s)); }
void be_neg(Emit *e, int d, int s) { dp_imm(e, DP_RSB, 0, hr(d), hr(s), 0); }
void be_lsl(Emit *e, int d, int s, unsigned n) { shift_imm(e, 0, hr(d), hr(s), 0, n); }
void be_ubfx(Emit *e, int d, int s, unsigned lsb, unsigned w) { bitfield(e, 0xF3C0u, hr(d), hr(s), lsb, w - 1); }
void be_sbfx(Emit *e, int d, int s, unsigned lsb, unsigned w) { bitfield(e, 0xF340u, hr(d), hr(s), lsb, w - 1); }
void be_bfi(Emit *e, int d, int s, unsigned lsb, unsigned w) { bitfield(e, 0xF360u, hr(d), hr(s), lsb, lsb + w - 1); }
void be_sxtb(Emit *e, int d, int s) { put32(e, 0xFA4Fu, 0xF080u | (uint32_t)hr(d) << 8 | (uint32_t)hr(s)); }

/* ---- NZCV producers ------------------------------------------------------------ */

static uint32_t mask_sh(uint32_t imm, int sh) { return (imm & (sh == 16 ? 0xFFFFu : 0xFFu)) << sh; }
int be_imm_ok_sh(uint32_t imm, int sh) { uint32_t enc; return modimm(mask_sh(imm, sh), &enc); }

int be_addsub_sh(Emit *e, int sub, int x, int d, int a, int b, int sh)
{
    shift_imm(e, 0, hr(x), hr(a), 0, (unsigned)sh);
    dp_reg(e, sub ? DP_SUB : DP_ADD, 1, hr(x), hr(x), hr(b), 0, sh);
    shift_imm(e, 0, hr(d), hr(x), 1, (unsigned)sh);
    return sub ? FM_SUB : FM_ADD;
}

int be_addsubi_sh(Emit *e, int sub, int x, int d, int a, uint32_t imm, int sh, int tmp)
{
    uint32_t v = mask_sh(imm, sh), enc;
    shift_imm(e, 0, hr(x), hr(a), 0, (unsigned)sh);
    if (modimm(v, &enc)) dp_imm(e, sub ? DP_SUB : DP_ADD, 1, hr(x), hr(x), enc);
    else { imm32(e, hr(tmp), v); dp_reg(e, sub ? DP_SUB : DP_ADD, 1, hr(x), hr(x), hr(tmp), 0, 0); }
    shift_imm(e, 0, hr(d), hr(x), 1, (unsigned)sh);
    return sub ? FM_SUB : FM_ADD;
}

int be_cmp_sh(Emit *e, int x, int a, int b, int sh)
{
    shift_imm(e, 0, hr(x), hr(a), 0, (unsigned)sh);
    dp_reg(e, DP_SUB, 1, PC, hr(x), hr(b), 0, sh);
    return FM_SUB;
}

int be_cmpi_sh(Emit *e, int x, int a, uint32_t imm, int sh, int tmp)
{
    uint32_t v = mask_sh(imm, sh), enc;
    shift_imm(e, 0, hr(x), hr(a), 0, (unsigned)sh);
    if (modimm(v, &enc)) dp_imm(e, DP_SUB, 1, PC, hr(x), enc);
    else { imm32(e, hr(tmp), v); dp_reg(e, DP_SUB, 1, PC, hr(x), hr(tmp), 0, 0); }
    return FM_SUB;
}

int be_neg_sh(Emit *e, int x, int d, int a, int sh)
{
    shift_imm(e, 0, hr(x), hr(a), 0, (unsigned)sh);
    dp_imm(e, DP_RSB, 1, hr(x), hr(x), 0);
    shift_imm(e, 0, hr(d), hr(x), 1, (unsigned)sh);
    return FM_SUB;
}

int be_test_res(Emit *e, int x, int r, int sh)
{
    shift_imm(e, 0, hr(x), hr(r), 0, (unsigned)sh);
    cmp_imm0(e, hr(x));
    return FM_SUB;
}

/* ---- memory ----------------------------------------------------------------------- */

static int segreg(Emit *e, int seg)
{
    if (seg == B86_DS) return R_DS;
    if (seg == B86_SS) return R_SS;
    ldst(e, LDRW, LR, R_CTX, OFF(segp[seg]));
    return LR;
}

void be_ea(Emit *e, int d, int seg, int b1, int b2, int32_t disp)
{
    int dd = hr(d);
    int sr = segreg(e, seg);
    if (b1 < 0 && b2 < 0) {
        movw(e, dd, (uint32_t)disp);
        dp_reg(e, DP_ADD, 0, dd, sr, dd, 0, 0);
        return;
    }
    int src = hr(b1);
    if (b2 >= 0) { dp_reg(e, DP_ADD, 0, dd, hr(b1), hr(b2), 0, 0); src = dd; }
    if (disp) { opi_raw(e, AOP_ADD, dd, src, (uint32_t)disp); src = dd; }
    put32(e, 0xFA1Fu, 0xF080u | (uint32_t)dd << 8 | (uint32_t)src);   /* UXTH */
    dp_reg(e, DP_ADD, 0, dd, sr, dd, 0, 0);
}

void be_ea_off(Emit *e, int d, int b1, int b2, int32_t disp)
{
    int dd = hr(d);
    if (b1 < 0 && b2 < 0) { movw(e, dd, (uint32_t)disp); return; }
    if (b2 >= 0) dp_reg(e, DP_ADD, 0, dd, hr(b1), hr(b2), 0, 0);
    else mov(e, dd, hr(b1));
    if (disp) opi_raw(e, AOP_ADD, dd, dd, (uint32_t)disp);
}

void be_load(Emit *e, int w16, int d, int addr) { ldst(e, w16 ? LDRH : LDRB, hr(d), hr(addr), 0); }

void be_store(Emit *e, int w16, int v, int addr, int check, uint16_t next_ip)
{
    ldst(e, w16 ? STRH : STRB, hr(v), hr(addr), 0);
    if (!check) return;
    shift_imm(e, 0, LR, hr(addr), 1, 6);                         /* lr = addr >> 6 */
    put32(e, 0xF810u | R_CM, (uint32_t)LR << 12 | LR);           /* LDRB lr, [r9, lr] */
    cmp_imm0(e, LR);
    if (e->nslow >= 64) { e->overflow = 1; return; }
    e->slow[e->nslow].site = emit_bcc(e, AC_NE, NULL);
    e->slow[e->nslow].resume = e->p;
    e->slow[e->nslow].next_ip = next_ip;
    e->slow[e->nslow].len = (uint8_t)(w16 ? 2 : 1);
    e->slow[e->nslow].retire = e->retire;
    e->nslow++;
}

void be_set_seg(Emit *e, int s, int v)
{
    put32(e, 0xFA1Fu, 0xF080u | LR << 8 | (uint32_t)hr(v));     /* UXTH lr, v */
    ldst(e, STRW, LR, R_CTX, OFF(seg[s]));
    ldst(e, LDRW, R12, R_CTX, OFF(mem));
    dp_reg(e, DP_ADD, 0, R12, R12, LR, 0, 4);
    ldst(e, STRW, R12, R_CTX, OFF(segp[s]));
    if (s == B86_DS) mov(e, R_DS, R12);
    else if (s == B86_SS) mov(e, R_SS, R12);
}

void be_ldctx(Emit *e, int d, unsigned off) { ldst(e, LDRW, hr(d), R_CTX, off); }
void be_stctx(Emit *e, int v, unsigned off) { ldst(e, STRW, hr(v), R_CTX, off); }
void be_stctx_imm(Emit *e, uint32_t imm, unsigned off) { imm32(e, LR, imm); ldst(e, STRW, LR, R_CTX, off); }

/* ---- control flow -------------------------------------------------------------------- */

uint8_t *be_jcc(Emit *e, int ac) { return emit_bcc(e, ac, NULL); }
uint8_t *be_jmp(Emit *e) { return emit_b(e, NULL); }
uint8_t *be_cbnz(Emit *e, int r) { cmp_imm0(e, hr(r)); return emit_bcc(e, AC_NE, NULL); }
uint8_t *be_cbz16(Emit *e, int r) { shift_imm(e, 1, LR, hr(r), 0, 16); return emit_bcc(e, AC_EQ, NULL); }
uint8_t *be_cbnz16(Emit *e, int r) { shift_imm(e, 1, LR, hr(r), 0, 16); return emit_bcc(e, AC_NE, NULL); }

void be_exit_chain(Emit *e, uint16_t target_ip, int poll)
{
    uint8_t *irq = NULL;
    if (poll) {
        ldst(e, LDRW, R12, R_CTX, OFF(irq));
        cmp_imm0(e, R12);
        irq = emit_bcc(e, AC_NE, NULL);
    }
    uint8_t *site = emit_b(e, NULL);                             /* patchable */
    retire_mark(e, e->retire);
    movw(e, R12, target_ip);
    imm32(e, LR, (uint32_t)(uintptr_t)site);
    emit_b(e, e->x_chain);
    if (poll) {
        be_bind(e, irq, e->p);
        retire_mark(e, e->retire);
        movw(e, R12, target_ip);
        emit_b(e, e->x_irq);
    }
}

void be_exit_ip_reg(Emit *e, int r) { retire_mark(e, e->retire); mov(e, R12, hr(r)); emit_b(e, e->x_lookup); }
void be_exit_ip_imm(Emit *e, uint16_t ip, int reason)
{
    retire_mark(e, e->retire);
    movw(e, R12, ip);
    movw(e, LR, (uint32_t)reason);
    emit_b(e, e->x_ipexit);
}
void be_exit_dyn(Emit *e) { retire_mark(e, e->retire); emit_b(e, e->x_dynexit); }

/* ---- helper calls ------------------------------------------------------------------------ */

void be_call_step(Emit *e, uint16_t ip, uint16_t next)
{
    retire_mark(e, e->retire);
    put32(e, 0xE880u | R_CTX, 0x00FFu);                          /* STM r8, {r0-r7} */
    mov(e, 0, R_CTX);
    imm32(e, 1, (uint32_t)ip | (uint32_t)next << 16);
    imm32(e, 2, e->blk);
    call_abs(e, (void *)b86h_step);
    mov(e, R12, 0);
    put32(e, 0xE890u | R_CTX, 0x00FFu);                          /* LDM r8, {r0-r7} */
    reload_segs(e);
    cmp_imm0(e, R12);
    uint8_t *s = emit_bcc(e, AC_EQ, NULL);
    emit_b(e, e->x_dynexit);
    be_bind(e, s, e->p);
}

void be_call_cond(Emit *e, int cc)
{
    put32(e, 0xE92Du, 0x000Fu);                                  /* PUSH {r0-r3} */
    mov(e, 0, R_CTX);
    movw(e, 1, (uint32_t)cc);
    call_abs(e, (void *)b86h_cond);
    mov(e, R12, 0);
    put32(e, 0xE8BDu, 0x000Fu);                                  /* POP {r0-r3} */
}

void be_call_flags(Emit *e)
{
    put32(e, 0xE92Du, 0x000Fu);
    mov(e, 0, R_CTX);
    call_abs(e, (void *)b86h_flags);
    put32(e, 0xE8BDu, 0x000Fu);
}

void be_finish_block(Emit *e)
{
    for (int i = 0; i < e->nslow; ++i) {
        be_bind(e, e->slow[i].site, e->p);
        put32(e, 0xE92Du, 0x500Fu);                              /* PUSH {r0-r3, r12, lr} */
        mov(e, 1, R12);
        mov(e, 0, R_CTX);
        movw(e, 2, e->slow[i].len);
        imm32(e, 3, e->blk);
        call_abs(e, (void *)b86h_smc);
        cmp_imm0(e, 0);
        put32(e, 0xE8BDu, 0x500Fu);                              /* POP {r0-r3, r12, lr} */
        uint8_t *ex = emit_bcc(e, AC_NE, NULL);
        emit_b(e, e->slow[i].resume);
        be_bind(e, ex, e->p);
        retire_mark(e, e->slow[i].retire);
        movw(e, R12, e->slow[i].next_ip);
        movw(e, LR, XR_LOOKUP);
        emit_b(e, e->x_ipexit);
    }
    e->nslow = 0;
}

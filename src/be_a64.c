/* AArch64 backend (Raspberry Pi Zero 2 W, Cortex-A53).
 *
 * Register map (resident across chained blocks):
 *   w19..w26  AX CX DX BX SP BP SI DI   (callee-saved: survive C helpers)
 *   x27       B86Cpu *
 *   x28       codemap biased by host address >> 6
 *   x29       DS base pointer (mem + DS*16)
 *   x14, x15  SS / ES base pointers (reloaded after helper calls)
 *   w9..w11   V_T0..V_T2 ; w16, w17 backend-internal ; w0 = exit reason
 * Stores keep NZCV intact (the SMC slow path saves and restores it).
 */
#include "backend.h"
#include <string.h>

const int be_has_t2 = 1;
const int be_store_clobbers_nzcv = 0;
const int be_logic_mode = FM_ADD;      /* CMN wzr, r, LSL #sh: C=0 V=0 */

#define OFF(f) ((unsigned)offsetof(B86Cpu, f))
#define XZR 31
#define XSP 31
#define R_CTX 27
#define R_CM 28
#define R_DS 29
#define R_SS 14
#define R_ES 15
#define I0 16
#define I1 17

static int hr(int v)
{
    if (v < 8) return 19 + v;
    return 9 + (v - V_T0);
}

static void put(Emit *e, uint32_t ins)
{
    if (e->p + 4 > e->end) { e->overflow = 1; return; }
    memcpy(e->p, &ins, 4);
    e->p += 4;
}

/* ---- encoders ---------------------------------------------------------- */
static uint32_t movz(int d, uint32_t imm, int hw) { return 0x52800000u | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)d; }
static uint32_t movk(int d, uint32_t imm, int hw) { return 0x72800000u | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)d; }
static uint32_t movzx(int d, uint32_t imm, int hw) { return 0xD2800000u | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)d; }
static uint32_t movkx(int d, uint32_t imm, int hw) { return 0xF2800000u | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)d; }
/* shifted-register ALU: base | shift<<22 | m<<16 | imm6<<10 | n<<5 | d */
static uint32_t alur(uint32_t base, int d, int n, int m, int sht, int amt)
{
    return base | (uint32_t)sht << 22 | (uint32_t)m << 16 | (uint32_t)amt << 10 | (uint32_t)n << 5 | (uint32_t)d;
}
#define W_ADD  0x0B000000u
#define W_ADDS 0x2B000000u
#define W_SUB  0x4B000000u
#define W_SUBS 0x6B000000u
#define W_AND  0x0A000000u
#define W_ORR  0x2A000000u
#define W_EOR  0x4A000000u
#define W_ORN  0x2A200000u
#define X_ADD  0x8B000000u
static uint32_t addi(int d, int n, uint32_t imm12, int lsl12) { return 0x11000000u | (uint32_t)lsl12 << 22 | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t subi(int d, int n, uint32_t imm12, int lsl12) { return 0x51000000u | (uint32_t)lsl12 << 22 | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t ubfm(int d, int n, int immr, int imms) { return 0x53000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t sbfm(int d, int n, int immr, int imms) { return 0x13000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t bfm(int d, int n, int immr, int imms) { return 0x33000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
/* ADD Xd, Xn, Wm, UXTH|UXTW #lsl */
static uint32_t addx_ext(int d, int n, int m, int opt, int lsl) { return 0x8B200000u | (uint32_t)m << 16 | (uint32_t)opt << 13 | (uint32_t)lsl << 10 | (uint32_t)n << 5 | (uint32_t)d; }
#define EXT_UXTH 1
#define EXT_UXTW 2
static uint32_t ldst(uint32_t base, int t, int n, unsigned off, int scale) { return base | (uint32_t)(off >> scale) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
#define STRW  0xB9000000u
#define LDRW  0xB9400000u
#define STRX  0xF9000000u
#define LDRX  0xF9400000u
#define STRH  0x79000000u
#define LDRH  0x79400000u
#define STRB  0x39000000u
#define LDRB  0x39400000u
static uint32_t ldrb_reg(int t, int n, int m) { return 0x38606800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t stp_w(int t1, int t2, int n, int off) { return 0x29000000u | (uint32_t)((off / 4) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t ldp_w(int t1, int t2, int n, int off) { return 0x29400000u | (uint32_t)((off / 4) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t stp_x(int t1, int t2, int n, int off) { return 0xA9000000u | (uint32_t)((off / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t ldp_x(int t1, int t2, int n, int off) { return 0xA9400000u | (uint32_t)((off / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t stp_x_pre(int t1, int t2, int n, int off) { return 0xA9800000u | (uint32_t)((off / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t ldp_x_post(int t1, int t2, int n, int off) { return 0xA8C00000u | (uint32_t)((off / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static uint32_t br(int n) { return 0xD61F0000u | (uint32_t)n << 5; }
static uint32_t blr(int n) { return 0xD63F0000u | (uint32_t)n << 5; }
#define RET 0xD65F03C0u

static void mov(Emit *e, int d, int s) { if (d != s) put(e, alur(W_ORR, d, XZR, s, 0, 0)); }
static void movx(Emit *e, int d, int s) { put(e, 0xAA0003E0u | (uint32_t)s << 16 | (uint32_t)d); }
static void imm32(Emit *e, int d, uint32_t v)
{
    put(e, movz(d, v & 0xFFFF, 0));
    if (v >> 16) put(e, movk(d, v >> 16, 1));
}
static void imm64(Emit *e, int d, uint64_t v)
{
    put(e, movzx(d, (uint32_t)(v & 0xFFFF), 0));
    for (int i = 1; i < 4; ++i) if ((v >> (16 * i)) & 0xFFFF) put(e, movkx(d, (uint32_t)(v >> (16 * i)) & 0xFFFF, i));
}

/* branches */
static uint32_t b_to(uint8_t *at, uint8_t *to) { return 0x14000000u | ((uint32_t)((to - at) >> 2) & 0x3FFFFFF); }
static void bl_abs(Emit *e, uint8_t *to) { if (e->p + 4 <= e->end) put(e, 0x94000000u | ((uint32_t)((to - e->p) >> 2) & 0x3FFFFFF)); else e->overflow = 1; }
static void b_abs(Emit *e, uint8_t *to) { if (e->p + 4 <= e->end) put(e, b_to(e->p, to)); else e->overflow = 1; }

void be_bind(Emit *e, uint8_t *site, uint8_t *target)
{
    (void)e;
    if (!site) return;
    uint32_t ins;
    memcpy(&ins, site, 4);
    int32_t off = (int32_t)((target - site) >> 2);
    if ((ins & 0xFC000000u) == 0x14000000u) ins = 0x14000000u | ((uint32_t)off & 0x3FFFFFF);
    else {                                                              /* B.cond / CBZ / CBNZ */
        if (off < -(1 << 18) || off >= (1 << 18)) __builtin_trap();
        ins = (ins & 0xFF00001Fu) | ((uint32_t)off & 0x7FFFF) << 5;
    }
    memcpy(site, &ins, 4);
}

void be_patch_branch(uint8_t *site, uint8_t *target)
{
    uint32_t ins = b_to(site, target);
    memcpy(site, &ins, 4);
    be_flush_icache(site, 4);
}

void be_kill_entry(uint8_t *entry, uint8_t *stub) { be_patch_branch(entry, stub); }
uintptr_t be_code_ptr(uint8_t *p) { return (uintptr_t)p; }

void be_flush_icache(void *start, size_t len)
{
    __builtin___clear_cache((char *)start, (char *)start + len);
}

/* ---- runtime trampolines ------------------------------------------------ */

static void reload_segs(Emit *e)
{
    put(e, ldst(LDRX, R_DS, R_CTX, OFF(segp[B86_DS]), 3));
    put(e, ldst(LDRX, R_SS, R_CTX, OFF(segp[B86_SS]), 3));
    put(e, ldst(LDRX, R_ES, R_CTX, OFF(segp[B86_ES]), 3));
}
static void save_gprs(Emit *e)
{
    for (int i = 0; i < 8; i += 2) put(e, stp_w(19 + i, 20 + i, R_CTX, OFF(r[0]) + 4 * i));
}
static void load_gprs(Emit *e)
{
    for (int i = 0; i < 8; i += 2) put(e, ldp_w(19 + i, 20 + i, R_CTX, OFF(r[0]) + 4 * i));
}

static void call_abs(Emit *e, void *fn);

void *be_emit_runtime(Emit *e)
{
    /* enter(ctx=x0, host=x1) */
    uint8_t *enter = e->p;
    put(e, stp_x_pre(29, 30, XSP, -96));
    put(e, stp_x(19, 20, XSP, 16));
    put(e, stp_x(21, 22, XSP, 32));
    put(e, stp_x(23, 24, XSP, 48));
    put(e, stp_x(25, 26, XSP, 64));
    put(e, stp_x(27, 28, XSP, 80));
    movx(e, R_CTX, 0);
    load_gprs(e);
    put(e, ldst(LDRX, R_CM, R_CTX, OFF(codemap_host), 3));
    reload_segs(e);
    put(e, br(1));

    /* epilogue: w0 = reason */
    e->x_epilogue = e->p;
    save_gprs(e);
    put(e, ldp_x(19, 20, XSP, 16));
    put(e, ldp_x(21, 22, XSP, 32));
    put(e, ldp_x(23, 24, XSP, 48));
    put(e, ldp_x(25, 26, XSP, 64));
    put(e, ldp_x(27, 28, XSP, 80));
    put(e, ldp_x_post(29, 30, XSP, 96));
    put(e, RET);

    /* ipexit: w9 = ip, w0 = reason */
    e->x_ipexit = e->p;
    put(e, ubfm(I0, 9, 0, 15));
    put(e, ldst(STRW, I0, R_CTX, OFF(ip), 2));
    b_abs(e, e->x_epilogue);

    /* dynexit: ctx->ip valid */
    e->x_dynexit = e->p;
    put(e, movz(0, XR_LOOKUP, 0));
    b_abs(e, e->x_epilogue);

    /* chain: w9 = ip, x16 = patch site */
    e->x_chain = e->p;
    put(e, ldst(STRX, I0, R_CTX, OFF(patch), 3));
    put(e, movz(0, XR_CHAIN, 0));
    b_abs(e, e->x_ipexit);

    /* irq: w9 = ip */
    e->x_irq = e->p;
    put(e, movz(0, XR_IRQ, 0));
    b_abs(e, e->x_ipexit);

    /* miss: w9 = key (cs<<16|ip) */
    e->x_miss = e->p;
    put(e, movz(0, XR_LOOKUP, 0));
    b_abs(e, e->x_ipexit);

    /* retfill: w9 = ip, x16 = site */
    e->x_retfill = e->p;
    put(e, ldst(STRX, I0, R_CTX, OFF(patch), 3));
    put(e, movz(0, XR_RETFILL, 0));
    b_abs(e, e->x_ipexit);

    /* ---- shared cold paths: `BL stub` + data words, read through x30 ---- */

    /* chain request: [BL x_chreq][target ip]; the BL is the patch site */
    e->x_chreq = e->p;
    put(e, ldst(LDRW, 9, 30, 0, 2));
    put(e, subi(I0, 30, 4, 0) | 0x80000000u);                /* SUB x16, x30, #4 */
    b_abs(e, e->x_chain);

    /* SMC slow path: x9 = host address; [BL][w0 next|len<<16|noexit<<24][w1 blk|retire<<20] */
    e->x_smc = e->p;
    put(e, 0xD100C3FFu);                                      /* SUB sp, sp, #48 */
    put(e, stp_x(9, 10, XSP, 0));
    put(e, 0xD53B4200u | I1);                                 /* MRS x17, NZCV */
    put(e, stp_x(11, I1, XSP, 16));
    put(e, stp_x(30, 30, XSP, 32));
    movx(e, 1, 9);
    put(e, ldst(LDRW, 2, 30, 0, 2));
    put(e, ubfm(2, 2, 16, 31));                               /* len | noexit<<8 */
    put(e, ldst(LDRW, 3, 30, 4, 2));
    put(e, ubfm(3, 3, 0, 19));                                /* blk */
    movx(e, 0, R_CTX);
    call_abs(e, (void *)b86h_smc);
    put(e, ldp_x(30, I0, XSP, 32));
    put(e, ldp_x(11, I1, XSP, 16));
    put(e, 0xD51B4200u | I1);                                 /* MSR NZCV, x17 */
    put(e, ldp_x(9, 10, XSP, 0));
    put(e, 0x9100C3FFu);                                      /* ADD sp, sp, #48 */
    reload_segs(e);
    uint8_t *sx = e->p; put(e, 0x35000000u | 0);              /* CBNZ w0, exit */
    put(e, addi(30, 30, 8, 0) | 0x80000000u);                 /* ADD x30, x30, #8 */
    put(e, RET);
    be_bind(e, sx, e->p);
    put(e, ldst(LDRW, I1, 30, 4, 2));
    put(e, ubfm(I1, I1, 20, 31));                             /* retire */
    put(e, ldst(STRW, I1, R_CTX, OFF(retired), 2));
    put(e, ldst(LDRW, I0, R_CTX, OFF(icnt), 2));
    put(e, alur(W_ADD, I0, I0, I1, 0, 0));
    put(e, ldst(STRW, I0, R_CTX, OFF(icnt), 2));
    put(e, ldst(LDRH, 9, 30, 0, 1));                          /* next ip */
    put(e, movz(0, XR_LOOKUP, 0));
    b_abs(e, e->x_ipexit);

    /* interpreter call: [BL][w0 ip|next<<16][w1 blk][w2 retire][x? fn as 2 words] */
    e->x_step = e->p;
    save_gprs(e);
    movx(e, 19, 30);                                          /* guest regs are in ctx now */
    movx(e, 0, R_CTX);
    put(e, ldst(LDRW, 1, 19, 0, 2));
    put(e, ldst(LDRW, 2, 19, 4, 2));
    put(e, ldst(LDRW, I0, 19, 12, 2));                        /* fn low */
    put(e, ldst(LDRW, I1, 19, 16, 2));                        /* fn high */
    put(e, 0xAA000000u | (uint32_t)I1 << 16 | 32u << 10 | (uint32_t)I0 << 5 | I0);   /* ORR x16, x16, x17, LSL #32 */
    put(e, blr(I0));
    movx(e, 30, 19);
    load_gprs(e);
    reload_segs(e);
    uint8_t *tx = e->p; put(e, 0x35000000u | 0);              /* CBNZ w0, exit */
    put(e, addi(30, 30, 20, 0) | 0x80000000u);                /* ADD x30, x30, #20 */
    put(e, RET);
    be_bind(e, tx, e->p);
    put(e, ldst(LDRW, I1, 30, 8, 2));                         /* retire */
    put(e, ldst(STRW, I1, R_CTX, OFF(retired), 2));
    put(e, ldst(LDRW, I0, R_CTX, OFF(icnt), 2));
    put(e, alur(W_ADD, I0, I0, I1, 0, 0));
    put(e, ldst(STRW, I0, R_CTX, OFF(icnt), 2));
    b_abs(e, e->x_dynexit);

    /* lookup: w9 = ip (garbage above bit 15) */
    e->x_lookup = e->p;
    put(e, ubfm(9, 9, 0, 15));
    put(e, ldst(LDRW, I0, R_CTX, OFF(irq), 2));
    uint8_t *cb = e->p; put(e, 0x35000000u | I0);           /* CBNZ w16, irq */
    put(e, ldst(LDRW, I0, R_CTX, OFF(seg[B86_CS]), 2));
    put(e, alur(W_ORR, 9, 9, I0, 0, 16));                   /* key */
    put(e, alur(W_EOR, I0, 9, 9, 1, 16));                   /* key ^ key>>16 */
    put(e, ubfm(I0, I0, 0, B86_FAST_BITS - 1));             /* & (FASTN-1) */
    put(e, ldst(LDRX, I1, R_CTX, OFF(fast), 3));
    put(e, alur(X_ADD, I1, I1, I0, 0, 4));                  /* + idx*16 */
    put(e, ldst(LDRW, I0, I1, 0, 2));
    put(e, alur(W_SUBS, XZR, I0, 9, 0, 0));
    uint8_t *bne = e->p; put(e, 0x54000000u | AC_NE);
    put(e, ldst(LDRX, I1, I1, 8, 3));
    put(e, br(I1));
    be_bind(e, cb, e->x_irq);
    be_bind(e, bne, e->x_miss);
    return enter;
}

/* ---- data movement / ALU ------------------------------------------------- */

void be_mov(Emit *e, int d, int s) { mov(e, hr(d), hr(s)); }
void be_movi(Emit *e, int d, uint32_t imm) { imm32(e, hr(d), imm); }

static uint32_t aop_base(int aop)
{
    switch (aop) { case AOP_ADD: return W_ADD; case AOP_SUB: return W_SUB; case AOP_AND: return W_AND;
                   case AOP_ORR: return W_ORR; default: return W_EOR; }
}
void be_op(Emit *e, int aop, int d, int a, int b) { put(e, alur(aop_base(aop), hr(d), hr(a), hr(b), 0, 0)); }
void be_op_lsl(Emit *e, int aop, int d, int a, int b, unsigned sh) { put(e, alur(aop_base(aop), hr(d), hr(a), hr(b), 0, (int)sh)); }

void be_opi(Emit *e, int aop, int d, int a, uint32_t imm)
{
    imm &= 0xFFFF;
    if (aop == AOP_ADD || aop == AOP_SUB) {
        uint32_t neg = (0x10000 - imm) & 0xFFFF;
        int add = aop == AOP_ADD;
        if (imm == 0) { mov(e, hr(d), hr(a)); return; }
        if (imm < 4096) { put(e, add ? addi(hr(d), hr(a), imm, 0) : subi(hr(d), hr(a), imm, 0)); return; }
        if (neg < 4096) { put(e, add ? subi(hr(d), hr(a), neg, 0) : addi(hr(d), hr(a), neg, 0)); return; }
        if ((imm & 0xFFF) == 0) { put(e, add ? addi(hr(d), hr(a), imm >> 12, 1) : subi(hr(d), hr(a), imm >> 12, 1)); return; }
    }
    imm32(e, I0, imm);
    put(e, alur(aop_base(aop), hr(d), hr(a), I0, 0, 0));
}

void be_mvn(Emit *e, int d, int s) { put(e, alur(W_ORN, hr(d), XZR, hr(s), 0, 0)); }
void be_neg(Emit *e, int d, int s) { put(e, alur(W_SUB, hr(d), XZR, hr(s), 0, 0)); }
void be_lsl(Emit *e, int d, int s, unsigned n) { put(e, ubfm(hr(d), hr(s), (int)((32 - n) & 31), (int)(31 - n))); }
void be_ubfx(Emit *e, int d, int s, unsigned lsb, unsigned w) { put(e, ubfm(hr(d), hr(s), (int)lsb, (int)(lsb + w - 1))); }
void be_sbfx(Emit *e, int d, int s, unsigned lsb, unsigned w) { put(e, sbfm(hr(d), hr(s), (int)lsb, (int)(lsb + w - 1))); }
void be_bfi(Emit *e, int d, int s, unsigned lsb, unsigned w) { put(e, bfm(hr(d), hr(s), (int)((32 - lsb) & 31), (int)(w - 1))); }
void be_sxtb(Emit *e, int d, int s) { put(e, sbfm(hr(d), hr(s), 0, 7)); }

void be_mul(Emit *e, int d, int a, int b) { put(e, 0x1B007C00u | (uint32_t)hr(b) << 16 | (uint32_t)hr(a) << 5 | (uint32_t)hr(d)); }
void be_shift_reg(Emit *e, int type, int d, int a, int cnt)
{
    /* A64 variable shifts use count mod 32; x86 counts are unmasked bytes,
       so clamp the count to 31 (enough to clear/sign-fill a 16-bit value). */
    put(e, ubfm(I1, hr(cnt), 0, 7));                                  /* UXTB w17, cnt */
    put(e, 0x71000000u | 31u << 10 | (uint32_t)I1 << 5 | 31u);        /* CMP w17, #31 */
    put(e, movz(I0, 31, 0));
    put(e, 0x1A800000u | (uint32_t)I0 << 16 | 0x9u << 12 | (uint32_t)I1 << 5 | (uint32_t)I1); /* CSEL w17, w17, w16, LS */
    static const uint32_t op[3] = { 0x1AC02000u, 0x1AC02400u, 0x1AC02800u };    /* LSLV LSRV ASRV */
    put(e, op[type] | (uint32_t)I1 << 16 | (uint32_t)hr(a) << 5 | (uint32_t)hr(d));
}

void be_get_carry(Emit *e, int d, int ac)
{
    /* CSET d, ac  ==  CSINC d, wzr, wzr, invert(ac) */
    put(e, 0x1A9F07E0u | (uint32_t)(ac ^ 1) << 12 | (uint32_t)hr(d));
}

/* ---- NZCV producers -------------------------------------------------------- */

int be_imm_ok_sh(uint32_t imm, int sh) { (void)imm; (void)sh; return 1; }

int be_addsub_sh(Emit *e, int sub, int x, int d, int a, int b, int sh)
{
    put(e, ubfm(hr(x), hr(a), 32 - sh, 31 - sh));                         /* x = a << sh */
    put(e, alur(sub ? W_SUBS : W_ADDS, hr(x), hr(x), hr(b), 0, sh));      /* x op= b << sh */
    put(e, ubfm(hr(d), hr(x), sh, 31));                                   /* d = x >> sh */
    return sub ? FM_SUB : FM_ADD;
}

static void imm_sh(Emit *e, int r, uint32_t imm, int sh)
{
    uint32_t v = (imm << sh);
    put(e, movz(r, v >> 16, 1));
}

int be_addsubi_sh(Emit *e, int sub, int x, int d, int a, uint32_t imm, int sh, int tmp)
{
    (void)tmp;
    put(e, ubfm(hr(x), hr(a), 32 - sh, 31 - sh));
    imm_sh(e, I0, imm & (sh == 16 ? 0xFFFF : 0xFF), sh);
    put(e, alur(sub ? W_SUBS : W_ADDS, hr(x), hr(x), I0, 0, 0));
    put(e, ubfm(hr(d), hr(x), sh, 31));
    return sub ? FM_SUB : FM_ADD;
}

int be_cmp_sh(Emit *e, int x, int a, int b, int sh)
{
    put(e, ubfm(hr(x), hr(a), 32 - sh, 31 - sh));
    put(e, alur(W_SUBS, XZR, hr(x), hr(b), 0, sh));
    return FM_SUB;
}

int be_cmpi_sh(Emit *e, int x, int a, uint32_t imm, int sh, int tmp)
{
    (void)tmp;
    put(e, ubfm(hr(x), hr(a), 32 - sh, 31 - sh));
    imm_sh(e, I0, imm & (sh == 16 ? 0xFFFF : 0xFF), sh);
    put(e, alur(W_SUBS, XZR, hr(x), I0, 0, 0));
    return FM_SUB;
}

int be_neg_sh(Emit *e, int x, int d, int a, int sh)
{
    put(e, alur(W_SUBS, hr(x), XZR, hr(a), 0, sh));                     /* x = 0 - (a<<sh) */
    put(e, ubfm(hr(d), hr(x), sh, 31));
    return FM_SUB;
}

void be_test_nz(Emit *e, int x, int r, int sh) { be_test_res(e, x, r, sh); }

int be_test_res(Emit *e, int x, int r, int sh)
{
    (void)x;
    put(e, alur(W_ADDS, XZR, XZR, hr(r), 0, sh));                       /* CMN wzr, r, LSL sh */
    return FM_ADD;
}

/* ---- memory ------------------------------------------------------------------ */

static int segreg(Emit *e, int seg)
{
    if (seg == B86_DS) return R_DS;
    if (seg == B86_SS) return R_SS;
    if (seg == B86_ES) return R_ES;
    put(e, ldst(LDRX, I1, R_CTX, OFF(segp[seg]), 3));
    return I1;
}

static void add_disp(Emit *e, int d, int s, int32_t disp)
{
    uint32_t imm = (uint32_t)disp & 0xFFFF;
    uint32_t neg = (0x10000 - imm) & 0xFFFF;
    if (imm < 4096) put(e, addi(d, s, imm, 0));
    else if (neg < 4096) put(e, subi(d, s, neg, 0));
    else { imm32(e, I0, imm); put(e, alur(W_ADD, d, s, I0, 0, 0)); }
}

void be_ea(Emit *e, int d, int seg, int b1, int b2, int32_t disp)
{
    int sr = segreg(e, seg);
    int dd = hr(d);
    int off;
    if (b1 < 0 && b2 < 0) {
        imm32(e, dd, (uint32_t)disp & 0xFFFF);
        put(e, addx_ext(dd, sr, dd, EXT_UXTW, 0));
        return;
    }
    if (b2 >= 0) { put(e, alur(W_ADD, dd, hr(b1), hr(b2), 0, 0)); off = dd; }
    else off = hr(b1);
    if (disp) { add_disp(e, dd, off, disp); off = dd; }
    put(e, addx_ext(dd, sr, off, EXT_UXTH, 0));
}

void be_ea_off(Emit *e, int d, int b1, int b2, int32_t disp)
{
    int dd = hr(d);
    if (b1 < 0 && b2 < 0) { imm32(e, dd, (uint32_t)disp & 0xFFFF); return; }
    if (b2 >= 0) put(e, alur(W_ADD, dd, hr(b1), hr(b2), 0, 0));
    else mov(e, dd, hr(b1));
    if (disp) add_disp(e, dd, dd, disp);
}

void be_load(Emit *e, int w16, int d, int addr)
{
    put(e, ldst(w16 ? LDRH : LDRB, hr(d), hr(addr), 0, 0));
}

void be_store(Emit *e, int w16, int v, int addr, int check, uint16_t next_ip)
{
    put(e, ldst(w16 ? STRH : STRB, hr(v), hr(addr), 0, 0));
    if (!check) return;
    put(e, 0xD3400000u | 6u << 16 | 63u << 10 | (uint32_t)hr(addr) << 5 | I0); /* LSR x16, xaddr, #6 */
    put(e, ldrb_reg(I0, R_CM, I0));
    if (e->nslow >= 64) { e->overflow = 1; return; }
    e->slow[e->nslow].site = e->p;
    put(e, 0x35000000u | I0);                                  /* CBNZ w16, slow */
    e->slow[e->nslow].resume = e->p;
    e->slow[e->nslow].next_ip = next_ip;
    e->slow[e->nslow].len = (uint8_t)(w16 ? 2 : 1);
    e->slow[e->nslow].retire = e->retire;
    e->slow[e->nslow].noexit = (uint8_t)(check == 2);
    e->nslow++;
}

void be_set_seg(Emit *e, int s, int v)
{
    put(e, ubfm(I1, hr(v), 0, 15));                             /* clear upper garbage in seg */
    put(e, ldst(STRW, I1, R_CTX, OFF(seg[s]), 2));
    put(e, ldst(LDRX, I0, R_CTX, OFF(mem), 3));
    put(e, addx_ext(I0, I0, I1, EXT_UXTW, 4));                  /* mem + (v<<4) */
    put(e, ldst(STRX, I0, R_CTX, OFF(segp[s]), 3));
    if (s == B86_DS) movx(e, R_DS, I0);
    else if (s == B86_SS) movx(e, R_SS, I0);
    else if (s == B86_ES) movx(e, R_ES, I0);
}

/* ---- ctx ------------------------------------------------------------------------ */

void be_ldctx(Emit *e, int d, unsigned off) { put(e, ldst(LDRW, hr(d), R_CTX, off, 2)); }
void be_stctx(Emit *e, int v, unsigned off) { put(e, ldst(STRW, hr(v), R_CTX, off, 2)); }
void be_stctx_imm(Emit *e, uint32_t imm, unsigned off)
{
    if (imm == 0) { put(e, ldst(STRW, XZR, R_CTX, off, 2)); return; }
    imm32(e, I0, imm);
    put(e, ldst(STRW, I0, R_CTX, off, 2));
}

/* ---- control flow ------------------------------------------------------------------ */

void be_count(Emit *e, uint32_t n)
{
    if (!e->count_ret || e->no_count || n == 0) return;
    put(e, ldst(LDRW, I1, R_CTX, OFF(icnt), 2));
    put(e, addi(I1, I1, n & 0xFFF, 0));
    put(e, ldst(STRW, I1, R_CTX, OFF(icnt), 2));
}

static void retire_mark(Emit *e, uint32_t n)
{
    if (!e->count_exits) return;
    put(e, movz(I1, n, 0));
    put(e, ldst(STRW, I1, R_CTX, OFF(retired), 2));
}

uint8_t *be_jcc(Emit *e, int ac) { uint8_t *s = e->p; put(e, 0x54000000u | (uint32_t)ac); return s; }
uint8_t *be_jmp(Emit *e) { uint8_t *s = e->p; put(e, 0x14000000u); return s; }
uint8_t *be_cbz(Emit *e, int r) { uint8_t *s = e->p; put(e, 0x34000000u | (uint32_t)hr(r)); return s; }
uint8_t *be_cbnz(Emit *e, int r) { uint8_t *s = e->p; put(e, 0x35000000u | (uint32_t)hr(r)); return s; }
uint8_t *be_cbz16(Emit *e, int r)
{
    put(e, ubfm(I1, hr(r), 0, 15));
    uint8_t *s = e->p; put(e, 0x34000000u | I1); return s;
}
uint8_t *be_cbnz16(Emit *e, int r)
{
    put(e, ubfm(I1, hr(r), 0, 15));
    uint8_t *s = e->p; put(e, 0x35000000u | I1); return s;
}

void be_exit_chain(Emit *e, uint16_t target_ip, int poll)
{
    uint8_t *irq = NULL;
    be_count(e, e->retire);
    if (poll) {
        put(e, ldst(LDRW, I0, R_CTX, OFF(irq), 2));
        irq = e->p; put(e, 0x35000000u | I0);
    }
    if (!e->count_exits) {                                      /* compact: BL + data */
        bl_abs(e, e->x_chreq);                                  /* patch site */
        put(e, target_ip);
    } else {
        uint8_t *site = e->p;
        put(e, b_to(e->p, e->p + 4));                           /* patchable: B next */
        retire_mark(e, e->retire);
        put(e, movz(9, target_ip, 0));
        imm64(e, I0, (uint64_t)(uintptr_t)site);
        b_abs(e, e->x_chain);
    }
    if (poll) {
        be_bind(e, irq, e->p);
        retire_mark(e, e->retire);
        put(e, movz(9, target_ip, 0));
        b_abs(e, e->x_irq);
    }
}

void be_exit_ip_reg(Emit *e, int r) { be_count(e, e->retire); retire_mark(e, e->retire); mov(e, 9, hr(r)); b_abs(e, e->x_lookup); }
void be_exit_ip_imm(Emit *e, uint16_t ip, int reason)
{
    be_count(e, e->retire);
    retire_mark(e, e->retire);
    put(e, movz(9, ip, 0));
    put(e, movz(0, (uint32_t)reason, 0));
    b_abs(e, e->x_ipexit);
}
void be_exit_dyn(Emit *e) { be_count(e, e->retire); retire_mark(e, e->retire); b_abs(e, e->x_dynexit); }

/* +0 MOVZ w16,#ip  +4 CMP w9,w16  +8 B.NE miss  +12 LDR w16,[irq]  +16 CBNZ w16,irq
   +20 B hit */
void be_ret_cache(Emit *e, uint8_t **site, uint8_t **bne, uint8_t **birq, uint8_t **bhit)
{
    be_count(e, e->retire);
    *site = e->p;
    put(e, movz(I0, 0, 0));
    put(e, alur(W_SUBS, XZR, 9, I0, 0, 0));
    *bne = e->p; put(e, 0x54000000u | AC_NE);
    put(e, ldst(LDRW, I0, R_CTX, OFF(irq), 2));
    *birq = e->p; put(e, 0x35000000u | I0);
    *bhit = e->p; put(e, 0x14000000u);
}
uint8_t *be_ret_hit_branch(uint8_t *site) { return site + 20; }
void be_exit_irq_ip(Emit *e) { retire_mark(e, e->retire); b_abs(e, e->x_irq); }
void be_ret_fill(Emit *e, uint16_t ret_ip, uint8_t *site)
{
    retire_mark(e, e->retire);
    put(e, movz(I1, ret_ip, 0));
    put(e, ldst(STRW, I1, R_CTX, OFF(scratch), 2));
    imm64(e, I0, (uint64_t)(uintptr_t)site);
    b_abs(e, e->x_retfill);
}

/* plain layout: core, +24 hop: B lookup, +28 irqhop: B irq, +32 fill */
void be_exit_ret(Emit *e, uint16_t ret_ip)
{
    uint8_t *site, *bne, *birq, *b;
    retire_mark(e, e->retire);
    be_ret_cache(e, &site, &bne, &birq, &b);
    b_abs(e, e->x_lookup);                                       /* +24 */
    be_bind(e, birq, e->p);
    b_abs(e, e->x_irq);                                          /* +28 */
    be_bind(e, bne, e->p);
    be_bind(e, b, e->p);
    be_ret_fill(e, ret_ip, site);
}

void be_patch_ret(uint8_t *site, uint16_t ip, uint8_t *target, uint8_t *miss)
{
    uint32_t ins = movz(I0, ip, 0);
    memcpy(site, &ins, 4);
    uint8_t *m = miss ? miss : site + 24;
    ins = 0x54000000u | (((uint32_t)(m - (site + 8)) >> 2) & 0x7FFFF) << 5 | AC_NE;
    memcpy(site + 8, &ins, 4);
    ins = b_to(site + 20, target);
    memcpy(site + 20, &ins, 4);
    be_flush_icache(site, 24);
}

/* ---- helper calls ---------------------------------------------------------------------- */

static void call_abs(Emit *e, void *fn)
{
    imm64(e, I0, (uint64_t)(uintptr_t)fn);
    put(e, blr(I0));
}

void be_call_step(Emit *e, uint16_t ip, uint16_t next)
{
    be_call_helper(e, (void *)b86h_step, (uint32_t)ip | (uint32_t)next << 16);
}

void be_call_helper(Emit *e, void *fn, uint32_t arg)
{
    retire_mark(e, e->retire);
    bl_abs(e, e->x_step);
    put(e, arg);
    put(e, e->blk);
    put(e, e->retire);
    put(e, (uint32_t)(uintptr_t)fn);
    put(e, (uint32_t)((uint64_t)(uintptr_t)fn >> 32));
}

void be_call_cond(Emit *e, int cc)
{
    movx(e, 0, R_CTX);
    put(e, movz(1, (uint32_t)cc, 0));
    call_abs(e, (void *)b86h_cond);
    mov(e, 9, 0);
    reload_segs(e);
}

void be_call_flags(Emit *e)
{
    movx(e, 0, R_CTX);
    call_abs(e, (void *)b86h_flags);
    reload_segs(e);
}

void be_finish_block(Emit *e)
{
    for (int i = 0; i < e->nslow; ++i) {
        be_bind(e, e->slow[i].site, e->p);
        bl_abs(e, e->x_smc);
        put(e, (uint32_t)e->slow[i].next_ip | (uint32_t)e->slow[i].len << 16 | (uint32_t)e->slow[i].noexit << 24);
        put(e, e->blk | e->slow[i].retire << 20);
        b_abs(e, e->slow[i].resume);                      /* x_smc returns here */
    }
    e->nslow = 0;
}

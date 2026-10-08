/* Differential fuzzer: random structured 8086 programs run on the reference
 * interpreter and on the translator (normal mode: superblocks, flag
 * liveness, NZCV fusion, lazy flags, chaining, fast lookup, SMC). Final
 * registers, all flags (including "undefined" ones) and the whole 1 MiB
 * image must match exactly.
 *
 * Programs contain forward branches of every condition, counted loops
 * (LOOP, DEC/JNZ, LOOPZ/LOOPNZ, JCXZ), near/indirect/far calls, software
 * interrupts with IRET, divide faults through INT 0, string ops with REP,
 * segment loads, PUSHF/POPF, BCD, shifts/rotates by 1 and CL, and
 * deliberate self-modifying stores into upcoming instructions.
 *
 *   fuzz [iterations] [seed]
 */
#include "b86.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODESEG 0x1000u
#define INTSEG  0x2000u
#define FARSEG  0x2800u
#define DSEG    0x3000u
#define ESEG    0x4000u
#define SSEG    0x5000u
#define XSEG    0x6000u

static uint8_t memA[B86_MEM_BYTES] __attribute__((aligned(64)));
static uint8_t memB[B86_MEM_BYTES] __attribute__((aligned(64)));
static uint8_t init_img[B86_MEM_BYTES];

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint32_t rn(uint32_t n) { return rnd() % n; }

/* ---- code emission ------------------------------------------------------ */
static uint8_t *code;      /* points into init_img at CS:0 */
static uint32_t pc;        /* offset within code segment */
static void B(uint8_t v) { code[pc++] = v; }
static void W(uint16_t v) { B((uint8_t)v); B((uint8_t)(v >> 8)); }

/* forbidden destination registers (bit per reg16) */
static unsigned forbid;
#define F_SP (1u << B86_SP)
#define F_CX (1u << B86_CX)

static int r16_ok(int r) { return !(forbid & (1u << r)); }
static int r8_ok(int r) { return !(forbid & (1u << (r & 3))); }

/* modrm with optional memory; returns via *rmreg the register for mod=3 */
static void modrm(int reg, int allow_mem, int *is_mem, int *rmreg)
{
    int mod = allow_mem ? (int)rn(4) : 3;
    int rm = (int)rn(8);
    *is_mem = mod != 3; *rmreg = rm;
    B((uint8_t)(mod << 6 | reg << 3 | rm));
    if (mod == 0 && rm == 6) W((uint16_t)rnd());
    else if (mod == 1) B((uint8_t)rnd());
    else if (mod == 2) W((uint16_t)rnd());
}

static void seg_prefix_data(void)
{
    static const uint8_t p[3] = { 0x26, 0x36, 0x3E };
    if (rn(6) == 0) B(p[rn(3)]);
}

/* pick a register index valid as destination */
static int pick16(void) { for (;;) { int r = (int)rn(8); if (r16_ok(r)) return r; } }
static int pick8(void) { for (;;) { int r = (int)rn(8); if (r8_ok(r)) return r; } }

/* one random straight-line instruction (no control flow) */
static void gen_simple(void)
{
    for (;;) {
        uint32_t save = pc;
        int k = (int)rn(30);
        int mem, rmr;
        switch (k) {
        case 0: case 1: case 2: case 3: case 4: case 5: { /* ALU r/m,r  r,r/m */
            int aop = (int)rn(8), dir = (int)rn(2), w = (int)rn(2);
            int reg = w ? pick16() : pick8();
            seg_prefix_data();
            B((uint8_t)(aop << 3 | dir << 1 | w));
            modrm(reg, 1, &mem, &rmr);
            if (dir == 0 && !mem && aop != 7 && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            return; }
        case 6: { /* ALU acc, imm */
            int aop = (int)rn(8), w = (int)rn(2);
            if (!r16_ok(0)) { pc = save; continue; }
            B((uint8_t)(aop << 3 | 4 | w)); if (w) W((uint16_t)rnd()); else B((uint8_t)rnd());
            return; }
        case 7: case 8: { /* grp1 */
            static const uint8_t ops[4] = { 0x80, 0x81, 0x82, 0x83 };
            uint8_t op = ops[rn(4)];
            seg_prefix_data();
            B(op);
            int reg = (int)rn(8);
            modrm(reg, 1, &mem, &rmr);
            if (!mem && reg != 7 && !((op & 1) ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            if (op == 0x81) W((uint16_t)rnd()); else B((uint8_t)(rn(3) ? rn(8) : rnd()));
            return; }
        case 9: { /* INC/DEC r16 */
            int r = pick16(); B((uint8_t)((rn(2) ? 0x40 : 0x48) | r)); return; }
        case 10: { /* FE/FF inc/dec, F6/F7 test/not/neg/mul/imul/div/idiv */
            int which = (int)rn(3);
            int w = (int)rn(2);
            seg_prefix_data();
            if (which == 0) { B((uint8_t)(0xFE | w)); modrm((int)rn(2), 1, &mem, &rmr); }
            else {
                int reg = (int)rn(8);
                if (reg >= 4 && (forbid & ((1u << B86_AX) | (1u << B86_DX)))) { pc = save; continue; }
                B((uint8_t)(0xF6 | w)); modrm(reg, 1, &mem, &rmr);
                if (reg < 2) { if (w) W((uint16_t)rnd()); else B((uint8_t)rnd()); }
                if (reg >= 4) return;
            }
            if (!mem && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            return; }
        case 11: case 12: { /* MOV forms */
            int f = (int)rn(4), w = (int)rn(2);
            if (f == 0) { int r = w ? pick16() : pick8(); B((uint8_t)((w ? 0xB8 : 0xB0) | r)); if (w) W((uint16_t)rnd()); else B((uint8_t)rnd()); return; }
            if (f == 1) { /* mov reg, r/m (CS override allowed: read) */
                int r = w ? pick16() : pick8();
                if (rn(8) == 0) B(0x2E); else seg_prefix_data();
                B((uint8_t)(0x8A | w)); modrm(r, 1, &mem, &rmr); return; }
            if (f == 2) { seg_prefix_data(); B((uint8_t)(0x88 | w)); modrm((int)rn(8), 1, &mem, &rmr);
                if (!mem && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; } return; }
            seg_prefix_data(); B((uint8_t)(0xC6 | w)); modrm(0, 1, &mem, &rmr);
            if (!mem && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            if (w) W((uint16_t)rnd()); else B((uint8_t)rnd());
            return; }
        case 13: { /* TEST / XCHG */
            int w = (int)rn(2), x = (int)rn(2);
            int r = w ? pick16() : pick8();
            seg_prefix_data();
            B((uint8_t)((x ? 0x86 : 0x84) | w)); modrm(r, 1, &mem, &rmr);
            if (x && !mem && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            return; }
        case 14: { /* LEA / CBW / CWD / XCHG AX,r / LAHF / SAHF / flags ops / SALC */
            int f = (int)rn(8);
            if (f == 0) { int r = pick16(); B(0x8D); B((uint8_t)((rn(3)) << 6 | r << 3 | rn(8))); if (code[pc - 1] >> 6 == 1) B((uint8_t)rnd()); else if (code[pc - 1] >> 6 == 2 || (code[pc - 1] & 0xC7) == 6) W((uint16_t)rnd()); return; }
            if (f == 1) { if (!r16_ok(0)) { pc = save; continue; } B(0x98); return; }
            if (f == 2) { if (!r16_ok(2)) { pc = save; continue; } B(0x99); return; }
            if (f == 3) { int r = pick16(); if (!r16_ok(0)) { pc = save; continue; } B((uint8_t)(0x90 | r)); return; }
            if (f == 4) { if (!r16_ok(0)) { pc = save; continue; } B(0x9F); return; }
            if (f == 5) { B(0x9E); return; }
            if (f == 6) { static const uint8_t fo[6] = { 0xF5, 0xF8, 0xF9, 0xFC, 0xFD, 0xFC }; B(fo[rn(6)]); return; }
            if (!r16_ok(0)) { pc = save; continue; }
            B(0xD6); return; }
        case 15: { /* shifts/rotates by 1 or CL */
            int w = (int)rn(2), cl = (int)rn(3) == 0;
            seg_prefix_data();
            B((uint8_t)(0xD0 | cl << 1 | w)); modrm((int)rn(8), 1, &mem, &rmr);
            if (!mem && !(w ? r16_ok(rmr) : r8_ok(rmr))) { pc = save; continue; }
            return; }
        case 16: { /* BCD / AAM / AAD / XLAT */
            if (!r16_ok(0)) { pc = save; continue; }
            static const uint8_t bo[5] = { 0x27, 0x2F, 0x37, 0x3F, 0xD7 };
            int f = (int)rn(7);
            if (f < 5) B(bo[f]);
            else if (f == 5) { B(0xD4); B((uint8_t)(rn(10) == 0 ? 0 : rn(20) + 1)); }
            else { B(0xD5); B((uint8_t)rnd()); }
            return; }
        case 17: { /* PUSH x ; <simple> ; POP y */
            if (!r16_ok(pick16())) { pc = save; continue; }
            int r = (int)rn(8);
            if (rn(3) == 0) B((uint8_t)(0x06 | (rn(4) << 3))); else B((uint8_t)(0x50 | r));
            gen_simple();
            int d = pick16();
            if (rn(4) == 0) { B(0x8F); B((uint8_t)(0xC0 | d)); } else B((uint8_t)(0x58 | d));
            return; }
        case 18: { /* PUSHF/POPF pair, or PUSH r / POPF */
            if (rn(2)) { B(0x9C); B(0x9D); }
            else { B((uint8_t)(0x50 | rn(8))); B(0x9D); }
            return; }
        case 19: { /* segment load: mov ax?,seg ; mov ds/es,reg */
            static const uint16_t segs[4] = { DSEG, ESEG, XSEG, DSEG + 0x123 };
            int r = pick16();
            B((uint8_t)(0xB8 | r)); W(segs[rn(4)]);
            B(0x8E); B((uint8_t)(0xC0 | (rn(2) ? 0 : 3) << 3 | r));
            return; }
        case 20: { /* string op, maybe REP with small CX */
            static const uint8_t so[10] = { 0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
            uint8_t op = so[rn(10)];
            if ((op == 0xAC || op == 0xAD) && !r16_ok(0)) { pc = save; continue; }
            if (!r16_ok(B86_SI) || !r16_ok(B86_DI)) { pc = save; continue; }
            int rep = (int)rn(3);
            if (rep) { if (!r16_ok(B86_CX)) { pc = save; continue; } B(0xB9); W((uint16_t)rn(40)); B(rep == 1 ? 0xF3 : 0xF2); }
            seg_prefix_data();
            B(op);
            return; }
        case 21: { /* moffs */
            int w = (int)rn(2), st = (int)rn(2);
            if (!st && !r16_ok(0)) { pc = save; continue; }
            seg_prefix_data();
            B((uint8_t)(0xA0 | st << 1 | w)); W((uint16_t)rnd());
            return; }
        case 22: { /* MOV r/m16, sreg ; MOV sreg (SS/DS/ES) from r/m: limited */
            int r = pick16();
            B(0x8C); B((uint8_t)(0xC0 | rn(4) << 3 | r));
            return; }
        case 23: { /* INT 60h (handler in another segment), INTO */
            if (rn(2)) { B(0xCD); B(0x60); } else B(0xCE);
            return; }
        default: { /* compare + use of flags later: CMP r,r / r,imm / OR r,r */
            int w = (int)rn(2);
            int f = (int)rn(3);
            if (f == 0) { B((uint8_t)(0x38 | w)); B((uint8_t)(0xC0 | rn(64))); }
            else if (f == 1) { B((uint8_t)(0x3C | w)); if (w) W((uint16_t)(rn(4) ? rn(300) : rnd())); else B((uint8_t)rnd()); }
            else { B((uint8_t)(0x08 | w)); int r = (int)rn(8); B((uint8_t)(0xC0 | r << 3 | r)); }
            return; }
        }
    }
}

static void gen_block(int depth, int n);

/* forward conditional over a sub-block */
static void gen_fwd_branch(int depth)
{
    int kind = (int)rn(4);
    uint32_t at;
    if (kind == 0) { B((uint8_t)(rn(2) ? 0x70 | rn(16) : 0x60 | rn(16))); }
    else if (kind == 1) { B(0xE3); }                               /* JCXZ */
    else if (kind == 2) { B(0xEB); }                               /* JMP short */
    else { B((uint8_t)(0x70 | rn(16))); }
    at = pc; B(0);
    gen_block(depth + 1, (int)rn(6) + 1);
    uint32_t d = pc - (at + 1);
    if (d > 127) { pc = at + 1; gen_simple(); d = pc - (at + 1); if (d > 127) d = 0; }
    code[at] = (uint8_t)d;
}

static void gen_loop(int depth)
{
    if (forbid & F_CX) { gen_simple(); return; }
    unsigned saved = forbid;
    int kind = (int)rn(4);
    B(0xB9); W((uint16_t)(rn(12) + 1));        /* mov cx, n */
    uint32_t top = pc;
    forbid |= F_CX;
    gen_block(depth + 1, (int)rn(8) + 1);
    forbid = saved;
    int32_t d;
    switch (kind) {
    case 0: B(0xE2); d = (int32_t)top - (int32_t)(pc + 1); break;        /* LOOP */
    case 1: B(0x49); B(0x75); d = (int32_t)top - (int32_t)(pc + 1); break; /* DEC CX; JNZ */
    case 2: B(0xE1); d = (int32_t)top - (int32_t)(pc + 1); break;        /* LOOPZ */
    default: B(0xE0); d = (int32_t)top - (int32_t)(pc + 1); break;       /* LOOPNZ */
    }
    if (d < -128) { code[pc - 1] = 0x90; code[pc - 2] = 0x90; if (kind == 1) code[pc - 3] = 0x90; B(0x90); return; }
    B((uint8_t)d);
}

static uint32_t subs[8]; static int nsubs;

static void gen_call(void)
{
    if (!nsubs) { gen_simple(); return; }
    uint32_t t = subs[rn((uint32_t)nsubs)];
    int k = (int)rn(4);
    if (k == 0 && !(forbid & (1u << B86_SI))) { B(0xBE); W((uint16_t)t); B(0xFF); B(0xD6); }  /* mov si,t ; call si */
    else if (k == 1) { B(0x9A); W(0); W(FARSEG); }                                            /* call far */
    else { B(0xE8); W((uint16_t)(t - (pc + 2))); }                                            /* call near */
}

/* self-modifying store into the imm8 of an upcoming MOV AL/BL,imm8 */
static void gen_smc(void)
{
    if (!r16_ok(B86_BX) || !r16_ok(B86_AX)) { gen_simple(); return; }
    B(0xB0); B((uint8_t)rnd());                      /* mov al, x */
    B(0x2E); B(0xA2); uint32_t fix = pc; W(0);       /* mov [cs:target], al */
    gen_simple();
    uint32_t target = pc + 1;
    B(0xB3); B(0x11);                                /* mov bl, 11 (patched) */
    code[fix] = (uint8_t)target; code[fix + 1] = (uint8_t)(target >> 8);
}

static void gen_block(int depth, int n)
{
    for (int i = 0; i < n; ++i) {
        int k = (int)rn(20);
        if (depth < 3 && k == 0) gen_fwd_branch(depth);
        else if (depth < 2 && k == 1) gen_loop(depth);
        else if (depth < 3 && k == 2) gen_call();
        else if (k == 3) gen_smc();
        else if (k == 4 && depth < 3) gen_fwd_branch(depth);
        else gen_simple();
    }
}

static void build(uint8_t *img)
{
    memset(img, 0, B86_MEM_BYTES);
    for (uint32_t a = 0; a < 0x100000; a += 4) { uint32_t v = rnd(); memcpy(img + a, &v, 4); }
    memset(img, 0, 0x400);
    /* IVT: 0 and 4 and 60h -> handlers */
    uint16_t ivt[256][2];
    memset(ivt, 0, sizeof ivt);
    code = img + (INTSEG << 4);
    pc = 0;
    /* handler for INT 0 / INT 4 / INT 60h: a few simple ops + IRET */
    forbid = F_SP | F_CX;   /* handlers/subroutines may run inside loops */
    uint32_t h0 = pc; nsubs = 0; gen_block(3, 3); B(0xCF);
    for (int v = 0; v < 256; ++v) { ivt[v][0] = (uint16_t)h0; ivt[v][1] = INTSEG; }
    memcpy(img, ivt, sizeof ivt);
    /* far subroutine */
    code = img + (FARSEG << 4); pc = 0;
    gen_block(3, (int)rn(5) + 1); B(0xCB);
    /* main code */
    code = img + (CODESEG << 4); pc = 0;
    /* subroutines first (jumped over) */
    B(0xE9); uint32_t jfix = pc; W(0);
    nsubs = 0;
    for (int s = 0; s < 4; ++s) {
        subs[nsubs++] = pc;
        gen_block(3, (int)rn(6) + 1);
        if (rn(3) == 0) { B(0xC2); W(0); } else B(0xC3);
    }
    forbid = F_SP;
    uint16_t rel = (uint16_t)(pc - (jfix + 2));
    code[jfix] = (uint8_t)rel; code[jfix + 1] = (uint8_t)(rel >> 8);
    gen_block(0, 120);
    B(0xF4);
}

static void init_cpu(B86Cpu *c, uint8_t *mem, uint64_t seed)
{
    b86_init(c, mem);
    uint64_t s = rs; rs = seed * 0x9E3779B97F4A7C15ull + 1;
    for (int i = 0; i < 8; ++i) c->r[i] = (uint16_t)rnd();
    c->r[B86_SP] = (uint16_t)(0x8000 + (rnd() & 0x3FFE));
    rs = s;
    b86_set_seg(c, B86_CS, CODESEG); b86_set_seg(c, B86_DS, DSEG);
    b86_set_seg(c, B86_ES, ESEG); b86_set_seg(c, B86_SS, SSEG);
    c->ip = 0;
    b86_set_flags(c, 0xF002 | (uint16_t)(seed & 0x8D5));
}

uint8_t fuzz_lastb[48]; uint16_t fuzz_lastip, fuzz_lastfl;
static B86Cpu *alarm_cpu;
static void on_alarm(int sig) { (void)sig; if (alarm_cpu) alarm_cpu->irq = 1; }

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200;
    uint64_t seed0 = argc > 2 ? strtoull(argv[2], 0, 0) : 1;
    void *buf = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    B86Cpu a, b;
    long bad = 0, skipped = 0;
    uint64_t total_insns = 0;
    signal(SIGALRM, on_alarm);
    B86Cpu tmp;
    b86_init(&tmp, memB);
    struct B86Jit *j = b86_jit_create(&tmp, buf, 8 << 20);
    for (long it = 0; it < iters; ++it) {
        uint64_t seed = seed0 + (uint64_t)it;
        rs = seed * 0x2545F4914F6CDD1Dull | 1;
        build(init_img);
        /* interpreter */
        memcpy(memA, init_img, B86_MEM_BYTES);
        init_cpu(&a, memA, seed);
        int ra = b86_run_interp(&a, 3000000);
        if (ra != B86_HALT) { skipped++; continue; }
        total_insns += a.icount;
        /* translator */
        memcpy(memB, init_img, B86_MEM_BYTES);
        init_cpu(&b, memB, seed);
        b.jit = j; b.codemap = tmp.codemap; b.codemap_host = tmp.codemap_host;
        b.fast = tmp.fast; b.smc_hook = tmp.smc_hook;
        /* the JIT keeps its cpu pointer; copy state into it */
        tmp = b;
        b86_jit_flush(j);
        b86_jit_set_no_chain(j, getenv("FUZZ_NOCHAIN") != NULL);
        b86_jit_set_no_spec(j, getenv("FUZZ_NOSPEC") != NULL);
        if (getenv("FUZZ_NOFAST")) b86_jit_set_no_fast(j, 1);
        if (getenv("FUZZ_TRACE")) {
            /* lockstep: one JIT block, then interpreter up to the same CS:IP */
            static uint8_t snap[B86_MEM_BYTES];
            B86Cpu ref; memcpy(snap, init_img, B86_MEM_BYTES);
            init_cpu(&ref, snap, seed);
            b86_jit_set_no_fast(j, 1);
            b86_jit_set_count_exits(j, 1);
            b86_jit_flush(j);
            long blkn;
            uint64_t ref_steps = 0;
            for (blkn = 0; blkn < 400000; ++blkn) {
                uint16_t ip0 = (uint16_t)tmp.ip; uint32_t cs0 = tmp.seg[B86_CS];
                uint8_t cur[48]; memcpy(cur, memB + (cs0 << 4) + ip0, 48);
                uint16_t pre[8]; for (int i = 0; i < 8; ++i) pre[i] = (uint16_t)tmp.r[i];
                uint16_t pref = b86_get_flags(&ref);
                uint16_t jfl = b86_get_flags(&tmp);
                static uint8_t prevb[48]; static uint16_t previp, prevfl, prevjfl;
                if (getenv("FUZZ_SHOW")) { fprintf(stderr, "blk %04X:%04X\n", cs0, ip0); }
                int r = b86_jit_run(&tmp, 1);
                int steps = 0, rr = B86_OK;
                for (uint32_t q = 0; q < tmp.retired && rr == B86_OK; ++q) { rr = b86_step(&ref); steps++; }
                ref_steps += (uint64_t)steps;
                int diff = ref.seg[B86_CS] != tmp.seg[B86_CS] || (uint16_t)ref.ip != (uint16_t)tmp.ip;
                for (int i = 0; i < 8; ++i) if ((uint16_t)ref.r[i] != (uint16_t)tmp.r[i]) diff = 1;
                long md = -1;
                for (long k = 0; k < (long)B86_MEM_BYTES; ++k) if (snap[k] != memB[k]) { md = k; diff = 1; break; }
                if (diff) {
                    printf("diverge in block %04X:%04X -> %04X (ref %04X, %d steps):", cs0, ip0, (uint16_t)tmp.ip, (uint16_t)ref.ip, steps);
                    for (int i = 0; i < 8; ++i) if ((uint16_t)ref.r[i] != (uint16_t)tmp.r[i]) printf(" r%d %04X/%04X", i, (uint16_t)ref.r[i], (uint16_t)tmp.r[i]);
                    if (md >= 0) printf(" mem[%05lX] %02X/%02X", md, snap[md], memB[md]);
                    printf("\n  bytes:");
                    for (int k = 0; k < 48; ++k) printf(" %02X", cur[k]);
                    printf("\n  entry regs:");
                    for (int i = 0; i < 8; ++i) printf(" %04X", pre[i]);
                    printf(" flags ref %04X jit %04X ds %04X es %04X\n", pref, jfl, ref.seg[B86_DS], ref.seg[B86_ES]);
                    printf("  prev block %04X (flags ref %04X jit %04X):", previp, prevfl, prevjfl);
                    for (int k = 0; k < 48; ++k) printf(" %02X", prevb[k]);
                    printf("\n");
                    break;
                }
                memcpy(prevb, cur, 48); previp = ip0; prevfl = pref; prevjfl = jfl;
                { extern uint8_t fuzz_lastb[48]; extern uint16_t fuzz_lastip, fuzz_lastfl;
                  memcpy(fuzz_lastb, cur, 48); fuzz_lastip = ip0; fuzz_lastfl = pref; }
                if (r == B86_HALT || rr != B86_OK) break;
            }
            printf("trace flags ref %04X jit %04X\n", b86_get_flags(&ref), b86_get_flags(&tmp));
            {
                extern uint8_t fuzz_lastb[48]; extern uint16_t fuzz_lastip, fuzz_lastfl;
                printf("last block %04X entry flags %04X:", fuzz_lastip, fuzz_lastfl);
                for (int k = 0; k < 48; ++k) printf(" %02X", fuzz_lastb[k]);
                printf("\n");
            }
            printf("trace end: %ld blocks, %llu ref steps, jit ip %04X:%04X ref ip %04X:%04X retired(last)=%u\n",
                   blkn, (unsigned long long)ref_steps, tmp.seg[B86_CS], (uint16_t)tmp.ip, ref.seg[B86_CS], (uint16_t)ref.ip, tmp.retired);
            b86_jit_set_no_fast(j, 0);
            b86_jit_set_count_exits(j, 0);
            continue;
        }
        alarm_cpu = &tmp; alarm(getenv("FUZZ_ALARM") ? (unsigned)atoi(getenv("FUZZ_ALARM")) : 20u);
        int rb = b86_jit_run(&tmp, ~0ull);
        alarm(0); alarm_cpu = NULL;
        b = tmp;
        int ok = rb == B86_HALT;
        for (int i = 0; i < 8; ++i) if ((uint16_t)a.r[i] != (uint16_t)b.r[i]) ok = 0;
        for (int i = 0; i < 4; ++i) if (a.seg[i] != b.seg[i]) ok = 0;
        if ((uint16_t)a.ip != (uint16_t)b.ip) ok = 0;
        uint16_t fa = b86_get_flags(&a), fb = b86_get_flags(&b);
        if (fa != fb) ok = 0;
        long mdiff = -1;
        for (long k = 0; k < (long)B86_MEM_BYTES; ++k) if (memA[k] != memB[k]) { mdiff = k; ok = 0; break; }
        if (!ok) {
            bad++;
            printf("seed %llu: MISMATCH rb=%d ip %04X/%04X flags %04X/%04X", (unsigned long long)seed, rb, (uint16_t)a.ip, (uint16_t)b.ip, fa, fb);
            for (int i = 0; i < 8; ++i) if ((uint16_t)a.r[i] != (uint16_t)b.r[i]) printf(" r%d %04X/%04X", i, (uint16_t)a.r[i], (uint16_t)b.r[i]);
            if (mdiff >= 0) printf(" mem[%05lX] %02X/%02X", mdiff, memA[mdiff], memB[mdiff]);
            printf("\n");
            if (bad > 10) break;
        }
    }
    const B86JitStats *s = b86_jit_stats(j);
    printf("fuzz: %ld programs, %ld mismatches, %ld skipped (no HLT), %llu guest insns interpreted\n",
           iters, bad, skipped, (unsigned long long)total_insns);
    printf("jit: blocks %llu  insns %llu  helper %llu  chains %llu  smc hits %llu  invalidations %llu  bytes/insn %.1f\n",
           (unsigned long long)s->blocks, (unsigned long long)s->guest_insns, (unsigned long long)s->helper_insns,
           (unsigned long long)s->chains, (unsigned long long)s->smc_hits, (unsigned long long)s->smc_invalidations,
           s->guest_insns ? (double)s->host_bytes / (double)s->guest_insns : 0.0);
    return bad != 0;
}

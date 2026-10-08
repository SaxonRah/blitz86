/* Hot-loop kernels. Each runs once on the interpreter (to count guest
 * instructions) and once on the translator. On hardware, time the JIT run;
 * under qemu, count executed host instructions in the code buffer with
 *   qemu-<arch> -one-insn-per-tb -d exec,nochain -dfilter <buf range> bench K N
 *
 *   bench <kernel> [iterations]     kernel: 0..N-1, or "list"
 */
#include "b86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <time.h>
#endif

#define CODE 0x1000u
#define DATA 0x3000u

static uint8_t mem[B86_MEM_BYTES] __attribute__((aligned(64)));

typedef struct { const char *name; const char *desc; const uint8_t *code; size_t len; } Kernel;

/* every kernel: CX = iteration count on entry, ends with HLT */
static const uint8_t k_regmix[] = {          /* L: add ax,bx ; xor dx,ax ; sub si,dx ; inc di ; loop L */
    0x01, 0xD8, 0x31, 0xC2, 0x29, 0xD6, 0x47, 0xE2, 0xF7, 0xF4 };
static const uint8_t k_cmpbr[] = {           /* L: cmp ax,100 ; jl S ; sub ax,100 ; S: add ax,7 ; dec cx ; jnz L */
    0x3D, 0x64, 0x00, 0x7C, 0x03, 0x2D, 0x64, 0x00, 0x05, 0x07, 0x00, 0x49, 0x75, 0xF2, 0xF4 };
static const uint8_t k_strlen[] = {          /* mov si,0 ; L: mov al,[si] ; inc si ; or al,al ; jnz L ; ... outer loop */
    0x31, 0xF6,                               /* O: xor si,si */
    0x8A, 0x04, 0x46, 0x08, 0xC0, 0x75, 0xF9, /* L: mov al,[si]; inc si; or al,al; jnz L */
    0xE2, 0xF5, 0xF4 };                       /* loop O ; hlt */
static const uint8_t k_memrmw[] = {          /* xor si,si ; L: add [bx+si+4],ax ; add si,2 ; dec cx ; jnz L */
    0x31, 0xF6, 0x01, 0x40, 0x04, 0x83, 0xC6, 0x02, 0x49, 0x75, 0xF7, 0xF4 };
static const uint8_t k_call[] = {            /* L: call F ; dec cx ; jnz L ; hlt ; F: add ax,bx ; ret */
    0xE8, 0x04, 0x00, 0x49, 0x75, 0xFA, 0xF4, 0x01, 0xD8, 0xC3 };
static const uint8_t k_stack[] = {           /* L: push ax ; push bx ; pop ax ; pop bx ; loop L */
    0x50, 0x53, 0x58, 0x5B, 0xE2, 0xFA, 0xF4 };
static const uint8_t k_lodsto[] = {          /* cld ; L: lodsb ; stosb ; loop L  (non-REP string ops) */
    0xFC, 0xAC, 0xAA, 0xE2, 0xFC, 0xF4 };
static const uint8_t k_repmov[] = {          /* cld ; O: mov dx,cx ; mov cx,64 ; xor si,si ; xor di,di ; rep movsw ; mov cx,dx ; loop O */
    0xFC, 0x89, 0xCA, 0xB9, 0x40, 0x00, 0x31, 0xF6, 0x31, 0xFF, 0xF3, 0xA5, 0x89, 0xD1, 0xE2, 0xF1, 0xF4 };
static const uint8_t k_shift[] = {           /* L: shl ax,1 ; adc dx,0 ; shr bx,1 ; loop L  (flag-consuming shift) */
    0xD1, 0xE0, 0x83, 0xD2, 0x00, 0xD1, 0xEB, 0xE2, 0xF7, 0xF4 };
static const uint8_t k_mul[] = {             /* L: mov ax,cx ; mul bx ; add si,ax ; loop L */
    0x89, 0xC8, 0xF7, 0xE3, 0x01, 0xC6, 0xE2, 0xF8, 0xF4 };

static const Kernel K[] = {
    { "regmix",  "register ALU + LOOP",                 k_regmix, sizeof k_regmix },
    { "cmpbr",   "CMP/JL/SUB/ADD + DEC/JNZ",           k_cmpbr,  sizeof k_cmpbr },
    { "strlen",  "byte scan MOV/INC/OR/JNZ",           k_strlen, sizeof k_strlen },
    { "memrmw",  "ADD [bx+si+4],ax + DEC/JNZ",         k_memrmw, sizeof k_memrmw },
    { "call",    "near CALL/RET per iteration",        k_call,   sizeof k_call },
    { "stack",   "PUSH/POP",                           k_stack,  sizeof k_stack },
    { "lodsto",  "LODSB/STOSB (non-REP)",              k_lodsto, sizeof k_lodsto },
    { "repmov",  "REP MOVSW x64",                      k_repmov, sizeof k_repmov },
    { "shift",   "SHL/ADC/SHR (flags live)",           k_shift,  sizeof k_shift },
    { "mul",     "MUL r16",                            k_mul,    sizeof k_mul },
};
#define NK (int)(sizeof K / sizeof K[0])

static void setup(B86Cpu *c, int k, unsigned iters)
{
    memset(mem, 0, B86_MEM_BYTES);
    memcpy(mem + (CODE << 4), K[k].code, K[k].len);
    for (unsigned i = 0; i < 0x8000; ++i) mem[(DATA << 4) + i] = (uint8_t)((i + 1) % 61 ? 1 + i % 7 : 0);
    b86_init(c, mem);
    b86_set_seg(c, B86_CS, CODE); b86_set_seg(c, B86_DS, DATA);
    b86_set_seg(c, B86_ES, DATA + 0x800); b86_set_seg(c, B86_SS, DATA + 0x1000);
    c->r[B86_SP] = 0xFFF0; c->r[B86_CX] = iters; c->r[B86_BX] = 0x1234; c->r[B86_AX] = 5;
    c->ip = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (int k = 0; k < NK; ++k) printf("%d %-8s %s\n", k, K[k].name, K[k].desc);
        return 0;
    }
    int k = atoi(argv[1]);
    unsigned iters = argc > 2 ? (unsigned)atoi(argv[2]) : 1000;
    B86Cpu c;
    setup(&c, k, iters);
    int noref = getenv("BENCH_NOREF") != NULL;   /* for host-instruction counting */
    if (!noref) b86_run_interp(&c, 100000000);
    uint64_t guest = c.icount;
    uint16_t ax = (uint16_t)c.r[0], dx = (uint16_t)c.r[2], si = (uint16_t)c.r[6];

    size_t sz = 1 << 20;
    void *buf = mmap((void *)0x20000000, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (buf == MAP_FAILED)
        buf = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    setup(&c, k, iters);
    struct B86Jit *j = b86_jit_create(&c, buf, sz);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = b86_jit_run(&c, ~0ull);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    int ok = r == B86_HALT && (noref || ((uint16_t)c.r[0] == ax && (uint16_t)c.r[2] == dx && (uint16_t)c.r[6] == si));
    const B86JitStats *st = b86_jit_stats(j);
    printf("%-8s guest=%llu %s blocks=%llu helper_sites=%llu dispatches=%llu buf=%p-%p %.3fs\n",
           K[k].name, (unsigned long long)guest, ok ? "OK" : "MISMATCH",
           (unsigned long long)st->blocks, (unsigned long long)st->helper_insns,
           (unsigned long long)st->dispatches, buf, (void *)((char *)buf + sz), s);
    return !ok;
}

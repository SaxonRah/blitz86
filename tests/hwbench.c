/* Mirrors microDOS pico/pi0w blitz86 JIT drivers (oracle compare, cold +
   warm passes on a live JIT with cpu = initial) so their workloads and
   logic can be validated under qemu before flashing. */
#include "b86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static uint8_t guest[B86_MEM_BYTES] __attribute__((aligned(64)));
static uint8_t hotbuf[128u * 1024u] __attribute__((aligned(64)));
static B86Cpu cpu;
static const uint8_t wl_loop[]={0xB9,0,0x80,0x49,0x75,0xFD,0xF4};
static const uint8_t wl_regmix[]={0xB8,1,0,0xBB,3,0,0xB9,0,0x80,0x31,0xD2,0x03,0xC3,0x33,0xD8,0x03,0xD0,0x49,0x75,0xF7,0xF4};
static const uint8_t wl_callmix[]={0xB9,0,0x80,0xBB,0,0,0xE8,4,0,0x49,0x75,0xFA,0xF4,0x83,0xC3,3,0xC3};
/* blitz86 kernels (all data kept away from the code at 0x100):
   memrmw  : mov cx,8000h; mov bx,4000h; xor si,si; L: add [bx+si+4],ax; add si,2; and si,3FFEh; dec cx; jnz L
   stack   : mov cx,8000h; L: push ax; push bx; pop ax; pop bx; loop L
   lodsto  : mov cx,4000h; mov si,4000h; mov di,8000h; cld; L: lodsb; stosb; loop L
   repmov  : mov dx,200h; cld; O: mov cx,800h; mov si,4000h; mov di,8000h; rep movsw; dec dx; jnz O
   shiftadc: mov cx,8000h; L: shl ax,1; adc dx,0; shr bx,1; loop L
   mul     : mov cx,8000h; mov bx,7; L: mov ax,cx; mul bx; add si,ax; loop L                */
static const uint8_t wl_memrmw[]={0xB9,0,0x80,0xBB,0,0x40,0x31,0xF6,0x01,0x40,0x04,0x83,0xC6,0x02,0x81,0xE6,0xFE,0x3F,0x49,0x75,0xF3,0xF4};
static const uint8_t wl_stack[]={0xB9,0,0x80,0x50,0x53,0x58,0x5B,0xE2,0xFA,0xF4};
static const uint8_t wl_lodsto[]={0xB9,0,0x40,0xBE,0,0x40,0xBF,0,0x80,0xFC,0xAC,0xAA,0xE2,0xFC,0xF4};
static const uint8_t wl_repmov[]={0xBA,0,0x02,0xFC,0xB9,0,0x08,0xBE,0,0x40,0xBF,0,0x80,0xF3,0xA5,0x4A,0x75,0xF2,0xF4};
static const uint8_t wl_shiftadc[]={0xB9,0,0x80,0xD1,0xE0,0x83,0xD2,0x00,0xD1,0xEB,0xE2,0xF7,0xF4};
static const uint8_t wl_mul[]={0xB9,0,0x80,0xBB,7,0,0x89,0xC8,0xF7,0xE3,0x01,0xC6,0xE2,0xF8,0xF4};
struct Work { const char *name; const uint8_t *bytes; unsigned size; };
static const struct Work workloads[]={
 {"loop",wl_loop,sizeof wl_loop},{"regmix",wl_regmix,sizeof wl_regmix},
 {"callmix",wl_callmix,sizeof wl_callmix},{"memrmw",wl_memrmw,sizeof wl_memrmw},
 {"stack",wl_stack,sizeof wl_stack},{"lodsto",wl_lodsto,sizeof wl_lodsto},
 {"repmov",wl_repmov,sizeof wl_repmov},{"shiftadc",wl_shiftadc,sizeof wl_shiftadc},
 {"mul",wl_mul,sizeof wl_mul}
};
static void reset_cpu(const struct Work *w)
{
    memset(guest, 0, sizeof guest); memcpy(guest + 0x100, w->bytes, w->size); b86_init(&cpu, guest);
    for (unsigned i = 0; i < 4; i++) b86_set_seg(&cpu, (int)i, 0);
    cpu.ip = 0x100; cpu.r[B86_SP] = 0xFFFEu;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv)
{
    size_t csz = 96u * 1024u;
    uint8_t *codebuf = mmap(NULL, csz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int only = argc > 1 ? atoi(argv[1]) : -1, fails = 0;
    for (unsigned w = 0; w < sizeof workloads / sizeof workloads[0]; w++) {
        if (only >= 0 && (int)w != only) continue;
        const struct Work *p = &workloads[w];
        reset_cpu(p);
        if (b86_run_interp(&cpu, 20000000) != B86_HALT) { printf("%s: oracle did not halt\n", p->name); return 1; }
        uint64_t retired = cpu.icount;
        B86Cpu oracle = cpu; static uint8_t omem[B86_MEM_BYTES]; memcpy(omem, guest, sizeof guest);
        reset_cpu(p);
        struct B86Jit *j = b86_jit_create_ex(&cpu, codebuf, csz, hotbuf, sizeof hotbuf);
        b86_jit_set_max_block(j, 48);
        B86Cpu initial = cpu;
        double best = 1e9; int ok = 1;
        for (unsigned pass = 0; pass < 4; pass++) {
            memset(guest, 0, sizeof guest); memcpy(guest + 0x100, p->bytes, p->size);
            cpu = initial;
            double t0 = now(); int rc = b86_jit_run(&cpu, 500000); double t = now() - t0;
            if (t < best) best = t;
            int same = rc == B86_HALT && !memcmp(guest, omem, sizeof guest) && (uint16_t)cpu.ip == (uint16_t)oracle.ip &&
                       b86_get_flags(&cpu) == b86_get_flags(&oracle);
            for (int i = 0; i < 8; i++) if ((uint16_t)cpu.r[i] != (uint16_t)oracle.r[i]) same = 0;
            ok &= same;
        }
        const B86JitStats *st = b86_jit_stats(j);
        printf("%-9s %s retired=%llu blocks=%llu chains=%llu dispatches=%llu helpers=%llu (qemu time %.4fs)\n", p->name, ok ? "PASS" : "FAIL",
               (unsigned long long)retired, (unsigned long long)st->blocks, (unsigned long long)st->chains,
               (unsigned long long)st->dispatches, (unsigned long long)st->helper_insns, best);
        fails += !ok;
        b86_jit_destroy(j);
    }
    return fails != 0;
}

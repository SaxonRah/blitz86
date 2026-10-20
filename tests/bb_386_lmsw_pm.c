/* r60: LMSW AX (PE=1) + far JMP enters protected mode; then 32-bit flat code
 * (DOS/4GW-style) stores through a 4 GiB data segment and halts.
 * gcc -O2 -Iinclude src/interp.c tests/bb_386_lmsw_pm.c -o build/bb_386_lmsw_pm */
#include "b86.h"
#include <stdio.h>
#include <string.h>
static uint8_t mem[B86_MEM_BYTES] __attribute__((aligned(64)));
static B86Cpu c;
int main(int argc, char **argv)
{
    int use_cr0 = argc > 1;
    static const uint8_t gdt[] = {
        0,0,0,0,0,0,0,0,
        0xFF,0xFF,0,0,0,0x9A,0xCF,0,      /* 08: code32 flat */
        0xFF,0xFF,0,0,0,0x92,0xCF,0,      /* 10: data32 flat */
    };
    uint8_t p[64]; unsigned n = 0;
    memcpy(mem + 0x1000, gdt, sizeof gdt);
    /* gdtr at 0x1100: limit 23, base 0x1000 */
    mem[0x1100]=23; mem[0x1101]=0; mem[0x1102]=0x00; mem[0x1103]=0x10; mem[0x1104]=0; mem[0x1105]=0;
    p[n++]=0xFA;                                   /* cli */
    p[n++]=0x0F; p[n++]=0x01; p[n++]=0x16; p[n++]=0x00; p[n++]=0x11; /* lgdt [1100] */
    if (use_cr0) { p[n++]=0x66; p[n++]=0xB8; p[n++]=1; p[n++]=0; p[n++]=0; p[n++]=0;  /* mov eax,1 */
                   p[n++]=0x0F; p[n++]=0x22; p[n++]=0xC0; }                         /* mov cr0,eax */
    else { p[n++]=0xB8; p[n++]=0xF1; p[n++]=0xFF;   /* mov ax,FFF1 (as DOS/4GW) */
           p[n++]=0x0F; p[n++]=0x01; p[n++]=0xF0; } /* lmsw ax */
    /* jmp far 0008:target (16-bit form, real-mode operand size) */
    unsigned tgt = 0x7C00 + n + 5;
    p[n++]=0xEA; p[n++]=(uint8_t)tgt; p[n++]=(uint8_t)(tgt>>8); p[n++]=0x08; p[n++]=0x00;
    /* 32-bit code now */
    p[n++]=0x66; p[n++]=0xB8; p[n++]=0x10; p[n++]=0x00;          /* mov ax,10h (16-bit in USE32) */
    p[n++]=0x8E; p[n++]=0xD8;                                    /* mov ds,ax */
    p[n++]=0xDB; p[n++]=0xE3;                                    /* fninit (no FPU: no-op) */
    p[n++]=0xDD; p[n++]=0x3D; p[n++]=0x00; p[n++]=0x06; p[n++]=0x10; p[n++]=0x00; /* fnstsw [100600h] */
    p[n++]=0x9B;                                                 /* wait */
    p[n++]=0xC6; p[n++]=0x05; p[n++]=0x00; p[n++]=0x05; p[n++]=0x10; p[n++]=0x00; p[n++]=0x42; /* mov byte [100500h],42h */
    p[n++]=0xF4;                                                 /* hlt */
    memcpy(mem + 0x7C00, p, n);
    mem[0x100600] = 0x5A;
    b86_init(&c, mem);
    c.model = B86_CPU_386;
    c.seg[B86_CS] = 0; c.ip = 0x7C00; c.seg[B86_SS]=0; c.r[B86_SP]=0x7000;
    int rc = B86_OK, steps = 0;
    while (steps < 100 && (rc = b86_step(&c)) == B86_OK) steps++;
    printf("%s: rc=%d steps=%d msw=%X mem[100500]=%02X ip=%X\n", use_cr0 ? "MOV CR0" : "LMSW",
           rc, steps, c.msw, mem[0x100500], (unsigned)c.ip);
    return !(rc == B86_HALT && (c.msw & 1) && mem[0x100500] == 0x42 && mem[0x100600] == 0x5A);
}

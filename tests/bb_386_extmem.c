/* r63: protected-mode access to extended memory above the low window.
 * gcc -O2 -Iinclude -DB86_RAM_BYTES=0x400000u [-DB86_PAGED=1] src/interp.c tests/bb_386_extmem.c */
#include "b86.h"
#include <stdio.h>
#include <string.h>
static uint8_t mem[B86_RAM_BYTES] __attribute__((aligned(4096)));
static B86Cpu c;
int main(void)
{
    static const uint8_t gdt[] = {0,0,0,0,0,0,0,0, 0xFF,0xFF,0,0,0,0x9A,0xCF,0, 0xFF,0xFF,0,0,0,0x92,0xCF,0};
    uint8_t p[64]; unsigned n = 0;
    memcpy(mem + 0x1000, gdt, sizeof gdt);
    mem[0x1100]=23; mem[0x1102]=0x00; mem[0x1103]=0x10;
    p[n++]=0xFA; p[n++]=0x0F; p[n++]=0x01; p[n++]=0x16; p[n++]=0x00; p[n++]=0x11;      /* cli; lgdt [1100] */
    p[n++]=0xB8; p[n++]=0x01; p[n++]=0x00; p[n++]=0x0F; p[n++]=0x01; p[n++]=0xF0;      /* mov ax,1; lmsw ax */
    unsigned tgt = 0x7C00 + n + 5;
    p[n++]=0xEA; p[n++]=(uint8_t)tgt; p[n++]=(uint8_t)(tgt>>8); p[n++]=8; p[n++]=0;   /* jmp 08:tgt */
    p[n++]=0x66; p[n++]=0xB8; p[n++]=0x10; p[n++]=0x00; p[n++]=0x8E; p[n++]=0xD8;     /* mov ax,10h; mov ds,ax */
    p[n++]=0xB8; p[n++]=0x78; p[n++]=0x56; p[n++]=0x34; p[n++]=0x12;                    /* mov eax,12345678h */
    p[n++]=0xA3; p[n++]=0xF0; p[n++]=0xFF; p[n++]=0x3F; p[n++]=0x00;                    /* mov [3FFFF0h],eax */
    p[n++]=0x8B; p[n++]=0x1D; p[n++]=0xF0; p[n++]=0xFF; p[n++]=0x3F; p[n++]=0x00;      /* mov ebx,[3FFFF0h] */
    p[n++]=0x01; p[n++]=0x1D; p[n++]=0x00; p[n++]=0x00; p[n++]=0x20; p[n++]=0x00;      /* add [200000h],ebx */
    p[n++]=0xF4;
    memcpy(mem + 0x7C00, p, n);
    b86_init(&c, mem);
    c.model = B86_CPU_386; c.ip = 0x7C00; c.r[B86_SP] = 0x7000;
    int rc, k = 0;
    while (k < 100 && (rc = b86_step(&c)) == B86_OK) k++;
    uint32_t a, b;
    memcpy(&a, mem + 0x3FFFF0, 4); memcpy(&b, mem + 0x200000, 4);
    printf("RAM=%uKiB rc=%d steps=%d [3FFFF0]=%08X [200000]=%08X ebx=%08X\n",
           (unsigned)(B86_RAM_BYTES/1024), rc, k, a, b, c.r[B86_BX]);
    return !(rc == B86_HALT && a == 0x12345678u && b == 0x12345678u && c.r[B86_BX] == 0x12345678u);
}

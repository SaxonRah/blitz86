/* BB_386_RM1_V1: 386 real-mode address-size tests.
 * cc -O2 -Wall -Wextra -Iinclude src/interp.c tests/bb_386_rm1_smoke.c -o build/bb_386_rm1_smoke
 * Standalone reference-interpreter test: no JIT or DOS runtime required.
 */
#include "b86.h"
#include <stdio.h>
#include <string.h>

static uint8_t mem[B86_MEM_BYTES];
static B86Cpu cpu;
static unsigned errors;
static void ck(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) { ++errors; printf("FAIL %-32s got=%08X expected=%08X\n", what, (unsigned)got, (unsigned)want); }
}
static void setup(const uint8_t *code, unsigned n)
{
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10000u, code, n);
    b86_init(&cpu, mem);
    cpu.model = B86_CPU_386;
    b86_set_seg(&cpu, B86_CS, 0x1000);
    b86_set_seg(&cpu, B86_DS, 0x3000);
    b86_set_seg(&cpu, B86_SS, 0x5000);
    cpu.r[B86_SP] = 0x0000A100;
}
static void run(const char *label, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        int rc = b86_step(&cpu);
        if (rc != B86_OK) {
            ++errors;
            printf("FAIL %-32s step=%u status=%d ip=%04X\n", label, i, rc, (unsigned)cpu.ip);
            break;
        }
    }
}
static void dat32(unsigned seg, unsigned off, uint32_t v)
{
    unsigned p = (seg << 4) + off;
    for (int i=0;i<4;++i) mem[p+i]=(uint8_t)(v>>(8*i));
}
static uint32_t out32(unsigned seg, unsigned off)
{
    unsigned p=(seg << 4)+off;
    return (uint32_t)mem[p] | (uint32_t)mem[p+1]<<8 | (uint32_t)mem[p+2]<<16 | (uint32_t)mem[p+3]<<24;
}
int main(void)
{
    /* 67 66 8B 44 8B FC: mov eax,[ebx+ecx*4-4] */
    static const uint8_t sib[]={0x67,0x66,0x8B,0x44,0x8B,0xFC};
    setup(sib,sizeof sib);
    cpu.r[B86_BX]=0x120u; cpu.r[B86_CX]=3;
    dat32(0x3000,0x128,0xFEDCBA98u);
    run("SIB index*4 disp8",1);
    ck("SIB read eax",cpu.r[B86_AX],0xFEDCBA98u);
    ck("SIB instruction length",cpu.ip,sizeof sib);

    /* ESP base means SS; override 3E means DS, even with a SIB byte. */
    static const uint8_t stackbase[]={
        0x67,0x66,0x8B,0x04,0x24,  /* mov eax,[esp] */
        0x3E,0x67,0x66,0x8B,0x04,0x24
    };
    setup(stackbase,sizeof stackbase);
    cpu.r[B86_SP]=0xA0;
    dat32(0x5000,0xA0,0x11223344u);
    dat32(0x3000,0xA0,0x55667788u);
    run("SIB ESP SS",1);
    ck("ESP uses SS",cpu.r[B86_AX],0x11223344u);
    run("SIB override DS",1);
    ck("ESP DS override",cpu.r[B86_AX],0x55667788u);

    /* EBP with mod=1 is SS; no-base SIB encoding index EBP uses DS. */
    static const uint8_t ebp[]={
        0x67,0x66,0x8B,0x45,0xFE,        /* [ebp-2] SS */
        0x67,0x66,0x8B,0x04,0x6D,0x08,0,0,0 /* [ebp*2 +8] DS */
    };
    setup(ebp,sizeof ebp);
    cpu.r[B86_BP]=0x100;
    dat32(0x5000,0xFE,0xDEADBEEFu);
    dat32(0x3000,0x208,0xCAFEBABEu);
    run("EBP SS",1);
    ck("EBP defaults SS",cpu.r[B86_AX],0xDEADBEEFu);
    run("SIB no base",1);
    ck("SIB no base defaults DS",cpu.r[B86_AX],0xCAFEBABEu);

    /* Mod=0 r/m=5 is disp32, and 16-bit operation preserves EAX.high */
    static const uint8_t disp[]={0x67,0x8B,0x05,0x34,0x12,0,0};
    setup(disp,sizeof disp);
    cpu.r[B86_AX]=0xABCD0000u;
    dat32(0x3000,0x1234,0xEEDD3344u);
    run("disp32 16-bit",1);
    ck("67 mov ax source",cpu.r[B86_AX],0xABCD3344u);

    /* 32-bit arithmetic address: base FFFFFFF0 + disp8 +32 wraps to 0x10. */
    static const uint8_t wrap[]={0x67,0x66,0x8B,0x43,0x20};
    setup(wrap,sizeof wrap);
    cpu.r[B86_BX]=0xFFFFFFF0u;
    dat32(0x3000,0x10,0x1234CDEFu);
    run("32-bit EA wrap",1);
    ck("wrap result",cpu.r[B86_AX],0x1234CDEFu);

    /* LEA does not access memory or check segment limits.  EAX includes the
       entire 32-bit EA, and the 16-bit destination variant writes AX only. */
    static const uint8_t lea[]={
       0x67,0x66,0x8D,0x43,0x10, /* lea eax,[ebx+16] */
       0x67,0x8D,0x43,0x10       /* lea ax,[ebx+16] */
    };
    setup(lea,sizeof lea);
    cpu.r[B86_BX]=0x1234FFF0u;
    run("LEA full32",1);
    ck("LEA32 no limit",cpu.r[B86_AX],0x12350000u);
    run("LEA 16 retain high",1);
    ck("LEA16 high preserved",cpu.r[B86_AX],0x12350000u);

    /* MOFFS 67 66 A3 stores 32 bits at 32-bit immediate offset. */
    static const uint8_t moffs[]={0x66,0x67,0xA3,0x20,0x01,0,0};
    setup(moffs,sizeof moffs);
    cpu.r[B86_AX]=0x87654321u;
    run("Moffs32 store",1);
    ck("Moffs32 bytes",out32(0x3000,0x120),0x87654321u);

    /* Byte-register writes MUST preserve EAX.high even with redundant 66h. */
    static const uint8_t byteop[]={0x66,0x67,0x8A,0x05,0x25,0,0,0};
    setup(byteop,sizeof byteop);
    cpu.r[B86_AX]=0xDEADBEEFu;
    mem[0x30000+0x25]=0x34;
    run("Byte + redundant 66",1);
    ck("AL preserves upper bits",cpu.r[B86_AX],0xDEADBE34u);

    /* Signed group 83 /7 compare in 32-bit EA + DWORD operand mode. */
    static const uint8_t cmp[]={0x67,0x66,0x83,0x3D,0x40,0,0,0,0xFF};
    setup(cmp,sizeof cmp);
    dat32(0x3000,0x40,0xFFFFFFFFu);
    run("CMP imm8 -1",1);
    ck("CMP EQ",(cpu.flags & B86_ZF)!=0,1);
    ck("CMP no memory write",out32(0x3000,0x40),0xFFFFFFFFu);

    /* A 32-bit offset outside the real-mode 64KiB segment limit MUST NOT
       quietly truncate to 16-bit and access an unrelated memory location. */
    static const uint8_t bad[]={0x67,0x66,0x8B,0x05,0x00,0x00,0x01,0x00};
    setup(bad,sizeof bad);
    dat32(0x3000,0x0,0xFEEDBABEu);
    ck("segment limit explicit status",b86_step(&cpu),B86_UNSUPPORTED);
    ck("unsupported restores IP",cpu.ip,0);
    ck("unsupported preserves EAX",cpu.r[B86_AX],0);

    /* Existing 286 interpreter must not interpret 67h as address prefix. */
    static const uint8_t old[]={0xB8,0x12,0x34};
    setup(old,sizeof old);
    cpu.model=B86_CPU_286;
    cpu.r[B86_AX]=0xCCDDFFFFu;
    run("286 old path",1);
    ck("286 old MOV",cpu.r[B86_AX],0x3412u);

    if (errors) printf("BB386 RM1 FAIL %u\n",errors);
    else puts("BB386 RM1 PASS — 67h/66h+67h SIB, segments, wrap, LEA, flags, limits, 286 isolation");
    return errors ? 1 : 0;
}

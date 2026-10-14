/* RM2 hosted interpreter validation: no DOS or JIT required. */
#include "b86.h"
#include <stdio.h>
#include <string.h>
static B86Cpu cpu;
static uint8_t mem[B86_MEM_BYTES];
static unsigned errors, tests;
static void eq(const char *what, uint32_t got, uint32_t want)
{
    ++tests;
    if (got != want) {
        ++errors;
        printf("FAIL %-32s got=%08X expected=%08X\n", what, (unsigned)got, (unsigned)want);
    }
}
static void setup(const uint8_t *code, size_t n)
{
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10000, code, n);
    b86_init(&cpu, mem);
    cpu.model = B86_CPU_386;
    b86_set_seg(&cpu, B86_CS, 0x1000);
    b86_set_seg(&cpu, B86_DS, 0x3000);
    b86_set_seg(&cpu, B86_SS, 0x5000);
}
static void step(const char *what)
{
    int rc = b86_step(&cpu);
    eq(what, (unsigned)rc, B86_OK);
}
static void put32(unsigned seg, unsigned off, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) mem[(seg << 4) + off + i] = (uint8_t)(v >> (8*i));
}
static uint32_t get32(unsigned seg, unsigned off)
{
    uint32_t out=0;
    for (unsigned i=0;i<4;++i) out|=(uint32_t)mem[(seg<<4)+off+i]<<(8*i);
    return out;
}
int main(void)
{
    { /* SETNE AH despite redundant 66; byte destination must preserve EAX.high */
        static const uint8_t p[]={0x66,0x0F,0x95,0xC4};
        setup(p,sizeof p); cpu.r[B86_AX]=0xABCD2345;
        cpu.flags &= ~B86_ZF;
        step("SETNE AH"); eq("AH set, preserve EAX",cpu.r[B86_AX],0xABCD0145);
        eq("SETNE leaves flags",cpu.flags & B86_ZF,0);
    }
    { /* 0F B6 C4 MOVZX AX,AH */
        static const uint8_t p[]={0x0F,0xB6,0xC4};
        setup(p,sizeof p); cpu.r[B86_AX]=0xABCDFF10;
        step("MOVZX AX,AH"); eq("MOVZX upper EAX",cpu.r[B86_AX],0xABCD00FF);
    }
    { /* 66 0F BE C4 MOVSX EAX,AH */
        static const uint8_t p[]={0x66,0x0F,0xBE,0xC4};
        setup(p,sizeof p); cpu.r[B86_AX]=0xABCDFF10;
        step("MOVSX EAX,AH"); eq("MOVSX sign extension",cpu.r[B86_AX],0xFFFFFFFF);
    }
    { /* 67 66 0F B7 44 8B FC: SIB indexed MOVZX EAX, WORD [EBX+ECX*4-4] */
        static const uint8_t p[]={0x67,0x66,0x0F,0xB7,0x44,0x8B,0xFC};
        setup(p,sizeof p); cpu.r[B86_BX]=0x120;cpu.r[B86_CX]=3;
        put32(0x3000,0x128,0x89ABCDEF);
        step("MOVZX 66+67 SIB"); eq("MOVZX SIB value",cpu.r[B86_AX],0xCDEF);
    }
    { /* 0F B6 06 34 12: MOVZX AX,BYTE [1234] */
        static const uint8_t p[]={0x0F,0xB6,0x06,0x34,0x12};
        setup(p,sizeof p);cpu.r[B86_AX]=0xBEEF0000;
        mem[0x30000+0x1234]=0x77;
        step("MOVZX disp16");eq("MOVZX preserves high",cpu.r[B86_AX],0xBEEF0077);
    }
    { /* BSF EAX,EBX with zero preserves destination and sets ZF */
        static const uint8_t p[]={0x66,0x0F,0xBC,0xC3};
        setup(p,sizeof p);cpu.r[B86_AX]=0xD00D1234;cpu.r[B86_BX]=0;
        step("BSF zero");eq("BSF dest unchanged",cpu.r[B86_AX],0xD00D1234);
        eq("BSF ZF",!!(cpu.flags&B86_ZF),1);
    }
    { /* BSR EAX,EBX from bit 31 */
        static const uint8_t p[]={0x66,0x0F,0xBD,0xC3};
        setup(p,sizeof p);cpu.r[B86_BX]=0x80000001;
        step("BSR EAX EBX");eq("BSR index",cpu.r[B86_AX],31);
        eq("BSR clears ZF",!!(cpu.flags&B86_ZF),0);
    }
    { /* 66 0F A3 C8 BT EAX,ECX bit 31 */
        static const uint8_t p[]={0x66,0x0F,0xA3,0xC8};
        setup(p,sizeof p);cpu.r[B86_AX]=0x80000000;cpu.r[B86_CX]=31;
        step("BT reg bit31");eq("BT CF",!!(cpu.flags&B86_CF),1);
        eq("BT nonmodifying",cpu.r[B86_AX],0x80000000);
    }
    { /* 66 0F AB 0E 40 00 BTS [0040],ECX with -1 bit index accesses [003C] bit31 */
        static const uint8_t p[]={0x66,0x0F,0xAB,0x0E,0x40,0x00};
        setup(p,sizeof p);cpu.r[B86_CX]=0xFFFFFFFF;
        step("BTS signed bit idx");eq("BTS modified previous DWORD",get32(0x3000,0x3c),0x80000000);
        eq("BTS CF old",!!(cpu.flags&B86_CF),0);
    }
    { /* 67 66 0F B3 4C 24 FC: BTR [ESP-4],ECX; default SS */
        static const uint8_t p[]={0x67,0x66,0x0F,0xB3,0x4C,0x24,0xFC};
        setup(p,sizeof p);cpu.r[B86_SP]=0x40;cpu.r[B86_CX]=0;
        put32(0x5000,0x3c,1);put32(0x3000,0x3c,1);
        step("BTR 67 SS base");eq("BTR SS updated",get32(0x5000,0x3c),0);
        eq("BTR DS untouched",get32(0x3000,0x3c),1);
        eq("BTR CF",!!(cpu.flags&B86_CF),1);
    }
    { /* 66 0F BA 3E 40 00 23: BTC [0040], imm8(35), targets [0044],bit3 */
        static const uint8_t p[]={0x66,0x0F,0xBA,0x3E,0x40,0x00,0x23};
        setup(p,sizeof p);step("BTC imm8 bit35");
        eq("BTC adjacent dword",get32(0x3000,0x44),8);
        eq("BTC original dword",get32(0x3000,0x40),0);
    }
    { /* 66 0F A4 C8 01 SHLD EAX,ECX,1  old EAX=0x80000001 */
        static const uint8_t p[]={0x66,0x0F,0xA4,0xC8,0x01};
        setup(p,sizeof p);cpu.r[B86_AX]=0x80000001;cpu.r[B86_CX]=1;
        step("SHLD EAX 1");eq("SHLD value",cpu.r[B86_AX],2);
        eq("SHLD carry",!!(cpu.flags&B86_CF),1);
        eq("SHLD overflow",!!(cpu.flags&B86_OF),1);
    }
    { /* 66 0F AC C8 01 SHRD EAX,ECX,1 => 0x80000000 */
        static const uint8_t p[]={0x66,0x0F,0xAC,0xC8,0x01};
        setup(p,sizeof p);cpu.r[B86_AX]=1;cpu.r[B86_CX]=1;
        step("SHRD EAX 1");eq("SHRD value",cpu.r[B86_AX],0x80000000);
        eq("SHRD carry",!!(cpu.flags&B86_CF),1);
        eq("SHRD overflow",!!(cpu.flags&B86_OF),1);
        eq("SHRD sign",!!(cpu.flags&B86_SF),1);
    }
    { /* 0F A5 C8 SHLD AX,CX,CL count=16, avoids host undefined shift */
        static const uint8_t p[]={0x0F,0xA5,0xC8};
        setup(p,sizeof p);cpu.r[B86_AX]=0xCAFE1234;cpu.r[B86_CX]=0xBEEF0010;
        step("SHLD16 count16");eq("SHLD16 result",cpu.r[B86_AX],0xCAFE0010);
    }
    { /* 66 0F AF C3 IMUL EAX,EBX flags */
        static const uint8_t p[]={0x66,0x0F,0xAF,0xC3};
        setup(p,sizeof p);cpu.r[B86_AX]=0x40000000;cpu.r[B86_BX]=4;
        step("IMUL overflow");eq("IMUL result",cpu.r[B86_AX],0);
        eq("IMUL overflow CF",!!(cpu.flags&B86_CF),1);
        eq("IMUL overflow OF",!!(cpu.flags&B86_OF),1);
    }
    { /* 0F 84 +4 near16 JZ from IP 4 -> target 8 */
        static const uint8_t p[]={0x0F,0x84,0x04,0x00,0x90,0x90,0x90,0x90,0x90};
        setup(p,sizeof p);cpu.flags|=B86_ZF;
        step("JZ rel16 taken");eq("JZ target",cpu.ip,8);
    }
    { /* 66 0F 85 03 00 00 00 JNZ near32 */
        static const uint8_t p[]={0x66,0x0F,0x85,0x03,0,0,0,0x90,0x90,0x90,0x90};
        setup(p,sizeof p);cpu.flags&=~B86_ZF;
        step("JNZ rel32 taken");eq("JNZ32 target",cpu.ip,10);
    }
    { /* 386 unsupported opcode 0F 31 must be explicit, IP unchanged */
        static const uint8_t p[]={0x0F,0x31};
        setup(p,sizeof p);eq("RDTS C unsupported",b86_step(&cpu),B86_UNSUPPORTED);
        eq("unsupported rewinds IP",cpu.ip,0);
    }
    { /* Real-mode 64KiB segment limit, no accidental access on new 0F form */
        static const uint8_t p[]={0x67,0x66,0x0F,0xB7,0x05,0,0,1,0};
        setup(p,sizeof p);cpu.r[B86_AX]=0x44556677;
        eq("386 segment fault placeholder",b86_step(&cpu),B86_UNSUPPORTED);
        eq("segment fault IP",cpu.ip,0);
        eq("segment fault preserves EAX",cpu.r[B86_AX],0x44556677);
    }
    { /* 286 model path remains independent */
        static const uint8_t p[]={0xB8,0x34,0x12};
        setup(p,sizeof p);cpu.model=B86_CPU_286;
        step("286 MOV AX immediate");eq("286 original semantics",cpu.r[B86_AX],0x1234);
    }
    printf("BB386 RM2 %s: %u assertions, %u failed\n",errors?"FAIL":"PASS",tests,errors);
    return errors ? 1 : 0;
}

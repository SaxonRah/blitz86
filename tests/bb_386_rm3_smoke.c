/* BB_386_RM3_V1 hosted interpreter smoke test.
 * Compile with: gcc -O2 -Wall -Wextra -Iinclude src/interp.c tests/bb_386_rm3_smoke.c -o build/bb_386_rm3_smoke
 */
#include "b86.h"
#include <stdio.h>
#include <string.h>

static B86Cpu cpu;
static uint8_t mem[B86_MEM_BYTES];
static unsigned tests, failed;
static void check(const char *name, uint32_t value, uint32_t want)
{
    ++tests;
    if (value != want) {
        ++failed;
        printf("FAIL %-38s got=%08X expected=%08X\n", name, (unsigned)value, (unsigned)want);
    }
}
static void setup(const uint8_t *ins, unsigned bytes, unsigned model)
{
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10000u, ins, bytes);
    b86_init(&cpu, mem);
    cpu.model = model;
    b86_set_seg(&cpu, B86_CS, 0x1000);
    b86_set_seg(&cpu, B86_DS, 0x3000);
    b86_set_seg(&cpu, B86_SS, 0x5000);
    cpu.r[B86_SP] = 0x12340200u;
}
static void put(unsigned seg, unsigned off, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        mem[(seg << 4) + off + i] = (uint8_t)(v >> (8u * i));
}
static uint32_t get(unsigned seg, unsigned off, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint32_t)mem[(seg << 4) + off + i] << (8u * i);
    return v;
}
static void good(const char *name) { check(name, b86_step(&cpu), B86_OK); }
int main(void)
{
    {
        /* MOV FS,AX; MOV BX,FS, preserve EBX.high */
        static const uint8_t p[] = {0x8E,0xE0,0x8C,0xE3};
        setup(p,sizeof p,B86_CPU_386);
        cpu.r[B86_AX] = 0xABCD6000u;
        cpu.r[B86_BX] = 0xBEEF0001u;
        good("MOV FS,AX");check("FS loaded", cpu.seg386[0], 0x6000);
        good("MOV BX,FS");check("BX value and high", cpu.r[B86_BX], 0xBEEF6000u);
    }
    {
        /* GS selector and 32-bit destination zero extension */
        static const uint8_t p[] = {0x8E,0xE8,0x66,0x8C,0xEB};
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_AX] = 0x7000;
        good("MOV GS,AX");check("GS loaded",cpu.seg386[1],0x7000);
        good("MOV EBX,GS");check("EBX zeroextends selector",cpu.r[B86_BX],0x7000);
    }
    {
        /* FS: 16-bit EA, DWORD operand */
        static const uint8_t p[] = {0x64,0x66,0x8B,0x06,0x40,0x00};
        setup(p,sizeof p,B86_CPU_386);cpu.seg386[0]=0x6000;
        put(0x6000,0x40,0x87654321u,4);put(0x3000,0x40,0xDEADBEEFu,4);
        good("FS 66 MOV EAX,[40]");check("FS DWORD used",cpu.r[B86_AX],0x87654321u);
        check("DS stays unmodified",get(0x3000,0x40,4),0xDEADBEEFu);
    }
    {
        /* GS: combined 67/66 + SIB index */
        static const uint8_t p[] = {0x65,0x67,0x66,0x8B,0x44,0x8B,0xFC};
        setup(p,sizeof p,B86_CPU_386);cpu.seg386[1]=0x7000;
        cpu.r[B86_BX]=0x100;cpu.r[B86_CX]=3;
        put(0x7000,0x108,0x0BADF00Du,4);
        good("GS 66+67 SIB");check("GS SIB value",cpu.r[B86_AX],0x0BADF00Du);
    }
    {
        /* FS: one-byte memory read and write */
        static const uint8_t p[] = {0x64,0x8A,0x06,0x50,0x00,0x64,0x88,0x06,0x51,0x00};
        setup(p,sizeof p,B86_CPU_386);cpu.seg386[0]=0x6000;
        put(0x6000,0x50,0xA5,1);cpu.r[B86_AX]=0xBEEF0033;
        good("FS MOV AL,[50]");check("FS byte preserved EAX",cpu.r[B86_AX],0xBEEF00A5u);
        good("FS MOV [51],AL");check("FS write",get(0x6000,0x51,1),0xA5);
    }
    {
        /* 0F B4 /r: LFS AX,[0040], memory holds offset16 then FS. */
        static const uint8_t p[] = {0x0F,0xB4,0x06,0x40,0x00};
        setup(p,sizeof p,B86_CPU_386);
        put(0x3000,0x40,0xABCD,2);put(0x3000,0x42,0x6000,2);
        cpu.r[B86_AX]=0xDEAD0000;
        good("LFS AX,m16:16");
        check("LFS AX preserved high",cpu.r[B86_AX],0xDEADABCDu);
        check("LFS selector",cpu.seg386[0],0x6000);
    }
    {
        /* 66 67 0F B5 /r LGS EAX,m16:32 with SIB no index. */
        static const uint8_t p[] = {0x66,0x67,0x0F,0xB5,0x44,0x24,0x00};
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_SP]=0x100;
        put(0x5000,0x100,0xF00DBAADu,4);
        put(0x5000,0x104,0x7000u,2);
        good("LGS EAX,m16:32 SIB");
        check("LGS value",cpu.r[B86_AX],0xF00DBAADu);
        check("LGS GS",cpu.seg386[1],0x7000);
    }
    {
        /* PUSH FS; POP GS; both 16-bit stack operations */
        static const uint8_t p[] = {0x0F,0xA0,0x0F,0xA9};
        setup(p,sizeof p,B86_CPU_386);cpu.seg386[0]=0x6000;
        good("PUSH FS");check("PUSH FS SP",cpu.r[B86_SP],0x123401FEu);
        check("PUSH FS selector",get(0x5000,0x1FE,2),0x6000);
        good("POP GS");check("POP GS selector",cpu.seg386[1],0x6000);
        check("POP GS SP",cpu.r[B86_SP],0x12340200u);
    }
    {
        /* 66 PUSH GS; 66 POP FS; DWORD stack width */
        static const uint8_t p[] = {0x66,0x0F,0xA8,0x66,0x0F,0xA1};
        setup(p,sizeof p,B86_CPU_386);cpu.seg386[1]=0x7000;
        good("PUSH GS DWORD");check("PUSH GS SP",cpu.r[B86_SP],0x123401FCu);
        check("PUSH GS data",get(0x5000,0x1FC,4),0x7000);
        good("POP FS DWORD");check("POP FS selector",cpu.seg386[0],0x7000);
        check("POP FS SP",cpu.r[B86_SP],0x12340200u);
    }
    {
        /* CALL rel32 to 9; RET32 back to 6 */
        static const uint8_t p[] = {0x66,0xE8,0x03,0,0,0,0x90,0x90,0x90,0x66,0xC3};
        setup(p,sizeof p,B86_CPU_386);
        good("CALL rel32");check("CALL target",cpu.ip,9);
        check("CALL DWORD return",get(0x5000,0x1FC,4),6);
        check("CALL SP",cpu.r[B86_SP],0x123401FC);
        good("RET32");check("RET target",cpu.ip,6);
        check("RET SP",cpu.r[B86_SP],0x12340200);
    }
    {
        static const uint8_t p[] = {0x66,0xE9,0x02,0,0,0};
        setup(p,sizeof p,B86_CPU_386);
        good("JMP rel32");check("JMP target",cpu.ip,8);
        check("JMP SP unchanged",cpu.r[B86_SP],0x12340200);
    }
    {
        /* Near indirect CALL EAX / RET, 32-bit operand */
        static const uint8_t p[] = {0x66,0xFF,0xD0,0x90,0x66,0xC3};
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_AX]=4;
        good("CALL EAX");check("CALL EAX target",cpu.ip,4);
        check("CALL EAX return",get(0x5000,0x1FC,4),3);
        good("RET after CALL EAX");check("return after indirect",cpu.ip,3);
    }
    {
        static const uint8_t p[] = {0x66,0xFF,0xF0};  /* PUSH EAX */
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_AX]=0xFEEDFACE;
        good("PUSH EAX r/m32");check("PUSH EAX value",get(0x5000,0x1FC,4),0xFEEDFACE);
    }
    {
        /* RET32 imm16 pops EIP, then adds extra eight bytes. */
        static const uint8_t p[] = {0x66,0xC2,0x08,0};
        setup(p,sizeof p,B86_CPU_386);put(0x5000,0x200,0x33,4);
        good("RET32 imm16");check("RET32 imm target",cpu.ip,0x33);
        check("RET32 imm SP",cpu.r[B86_SP],0x1234020C);
    }
    {
        /* PUSHAD stores original ESP; POPAD skips its ESP entry. */
        static const uint8_t p[] = {0x66,0x60,0x66,0x61};
        setup(p,sizeof p,B86_CPU_386);
        for(unsigned i=0;i<8;++i)cpu.r[i] = 0x11000000u+i;
        cpu.r[B86_SP] = 0x12340200;
        good("PUSHAD");check("PUSHAD SP",cpu.r[B86_SP],0x123401E0);
        check("PUSHAD stored ESP",get(0x5000,0x1EC,4),0x12340200);
        for(unsigned i=0;i<8;++i)if(i!=B86_SP)cpu.r[i]=0;
        good("POPAD");check("POPAD ESP skipped",cpu.r[B86_SP],0x12340200);
        check("POPAD EAX",cpu.r[B86_AX],0x11000000);
        check("POPAD EDI",cpu.r[B86_DI],0x11000007);
    }
    {
        static const uint8_t p[] = {0x66,0xC9};
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_BP]=0xBEEF0180;
        put(0x5000,0x180,0xAA11BB22,4);
        good("LEAVE32");check("LEAVE EBP",cpu.r[B86_BP],0xAA11BB22);
        check("LEAVE SP",cpu.r[B86_SP],0x12340184);
    }
    {
        /* Disallow a real-mode EIP past FFFF, without changing SP. */
        static const uint8_t p[] = {0x66,0xFF,0xD0};
        setup(p,sizeof p,B86_CPU_386);cpu.r[B86_AX]=0x10000;
        check("CS limit status", b86_step(&cpu), B86_UNSUPPORTED);
        check("CS limit leaves SP",cpu.r[B86_SP],0x12340200);
        check("CS limit rewinds IP",cpu.ip,0);
    }
    {
        /* unsupported FS-prefixed instruction must not silently use DS. */
        static const uint8_t p[] = {0x64,0xD7};
        setup(p,sizeof p,B86_CPU_386);check("FS unsupported",b86_step(&cpu),B86_UNSUPPORTED);
    }
    {
        /* 286 still does not recognize 386 FS/GS opcodes; model unaffected. */
        static const uint8_t p[] = {0x0F,0xA0};
        setup(p,sizeof p,B86_CPU_286);
        check("286 FS opcode raises int6",b86_step(&cpu),B86_OK);
    }
    if(failed) { printf("BB386 RM3 FAIL: %u of %u assertions\n", failed, tests); return 1; }
    printf("BB386 RM3 PASS: %u assertions, 0 failed\n", tests);
    return 0;
}

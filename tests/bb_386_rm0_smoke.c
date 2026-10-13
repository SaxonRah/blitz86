/* blitz86 386 real-mode milestone 0 — host interpreter smoke tests.
   Build (GCC/Clang):
      cc -O2 -Wall -Wextra -Iinclude src/interp.c tests/bb_386_rm0_smoke.c -o build/bb_386_rm0_smoke
   Run: build/bb_386_rm0_smoke
   The test deliberately does not invoke the JIT; 386 is interpreter-only.
*/
#include "b86.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
static uint8_t mem[B86_MEM_BYTES];
static B86Cpu cpu;

static void chk(const char *label, uint32_t got, uint32_t want)
{
    if (got != want) {
        ++failures;
        printf("FAIL %-28s got=%08X want=%08X\n", label, (unsigned)got, (unsigned)want);
    }
}
static void start(const uint8_t *code, unsigned count, uint32_t model)
{
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10000, code, count);
    b86_init(&cpu, mem);
    cpu.model = model;
    b86_set_seg(&cpu, B86_CS, 0x1000);
    b86_set_seg(&cpu, B86_DS, 0x3000);
    b86_set_seg(&cpu, B86_SS, 0x5000);
    cpu.r[B86_SP] = 0xAABBFFFEu;
}
static void steps(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        int rc = b86_step(&cpu);
        if (rc != B86_OK) {
            ++failures;
            printf("FAIL step %u status=%d ip=%04X\n", i, rc, (unsigned)cpu.ip);
            return;
        }
    }
}
static uint32_t dword(unsigned o)
{
    const uint8_t *p = mem + 0x30000 + o;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int main(void)
{
    static const uint8_t mix[] = {
        0x66,0xB8,0x78,0x56,0x34,0x12,          /* mov eax,12345678 */
        0xB8,0xCD,0xAB,                         /* mov ax,ABCD: EAX high stays 1234 */
        0x66,0x05,0x01,0x00,0x00,0x00,          /* add eax,1 */
        0x66,0x89,0x06,0x00,0x02,               /* mov [0200],eax */
        0x66,0x8B,0x1E,0x00,0x02,               /* mov ebx,[0200] */
        0x66,0x83,0xEB,0xFF,                    /* sub ebx,-1 */
        0x66,0x50,                              /* push eax */
        0x66,0x59,                              /* pop ecx */
        0xF4
    };
    start(mix, sizeof mix, B86_CPU_386);
    steps(8);
    chk("16-bit MOV preserves EAX high", cpu.r[B86_AX], 0x1234ABCEu);
    chk("66 MOV m32", dword(0x200), 0x1234ABCEu);
    chk("66 MOV EBX,m32; SUB -1", cpu.r[B86_BX], 0x1234ABCFu);
    chk("66 PUSH EAX; POP ECX", cpu.r[B86_CX], 0x1234ABCEu);
    chk("SP width16 preserved high", cpu.r[B86_SP], 0xAABBFFFEu);
    chk("SUB32 borrow", (cpu.flags & B86_CF) != 0, 1);

    static const uint8_t flags[] = {
        0x66,0xB8,0xFF,0xFF,0xFF,0x7F,         /* mov eax,7FFFFFFF */
        0x66,0x05,0x01,0x00,0x00,0x00,         /* add eax,1 */
        0x66,0x83,0xE8,0x01                    /* sub eax,1 */
    };
    start(flags, sizeof flags, B86_CPU_386);
    steps(2);
    chk("ADD32 signed overflow", (cpu.flags & B86_OF) != 0, 1);
    chk("ADD32 negative result", (cpu.flags & B86_SF) != 0, 1);
    chk("ADD32 no unsigned carry", (cpu.flags & B86_CF) != 0, 0);
    steps(1);
    chk("SUB32 result", cpu.r[B86_AX], 0x7FFFFFFFu);
    chk("SUB32 signed overflow", (cpu.flags & B86_OF) != 0, 1);

    static const uint8_t imm[] = {
        0x66,0xC7,0x06,0x04,0x02,0x44,0x33,0x22,0x11, /* mov dword [0204],11223344 */
        0x66,0xA1,0x04,0x02,                          /* mov eax,[0204] */
        0x66,0x6A,0xFE,                               /* push dword -2 */
        0x66,0x5A                                    /* pop edx */
    };
    start(imm, sizeof imm, B86_CPU_386);
    steps(4);
    chk("66 C7 store imm32", dword(0x204), 0x11223344u);
    chk("66 A1 load EAX", cpu.r[B86_AX], 0x11223344u);
    chk("66 PUSH imm8 sign-extend", cpu.r[B86_DX], 0xFFFFFFFEu);

    static const uint8_t unsupported67[] = {0x67,0x8B,0x00};
    start(unsupported67, sizeof unsupported67, B86_CPU_386);
    chk("67h 32-bit EA recognized", b86_step(&cpu), B86_OK);
    chk("67h advances instruction", cpu.ip, 3u);
    /* BB_386_RM1_V1: this was an unsupported case in RM0. */

    static const uint8_t unsupported66[] = {0x66,0x0F,0xB6,0xC0};
    start(unsupported66, sizeof unsupported66, B86_CPU_386);
    chk("missing 386 op explicit status", b86_step(&cpu), B86_UNSUPPORTED);

    static const uint8_t old[] = {0xB8,0x34,0x12};
    start(old, sizeof old, B86_CPU_286);
    cpu.r[B86_AX] = 0xABCD9999u;
    steps(1);
    chk("286 MOV AX unchanged", cpu.r[B86_AX], 0x1234u);

    if (failures) printf("BB386 RM0 FAIL %u\n", failures);
    else puts("BB386 RM0 PASS — 386/286 interpreter smoke tests");
    return failures ? 1 : 0;
}

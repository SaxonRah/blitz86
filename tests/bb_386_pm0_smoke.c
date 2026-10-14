/* BB_386_PM0_V1: host validation of descriptor decoding and GDTR bounds.
 * gcc -O2 -Wall -Wextra -Iinclude src/interp.c tests/bb_386_pm0_smoke.c -o build/bb_386_pm0_smoke
 */
#include "b86_pm0.h"
#include <stdio.h>
#include <string.h>
static B86Cpu c;
static uint8_t mem[B86_MEM_BYTES];
static unsigned total, bad;
static void eq(const char *name, uint32_t got, uint32_t want)
{
    ++total;
    if (got != want) {
        ++bad;
        printf("FAIL %-32s got=%08X expected=%08X\n",name,(unsigned)got,(unsigned)want);
    }
}
int main(void)
{
    B86PM0Desc d;
    static const uint8_t code32[8] = {0xFF,0xFF,0,0,0,0x9A,0xCF,0};
    static const uint8_t data32[8] = {0xFF,0xFF,0,0,0,0x92,0xCF,0};
    static const uint8_t notpresent[8] = {0xFF,0,0x34,0x12,0,0x12,0,0};
    b86_init(&c,mem);
    c.model = B86_CPU_386;
    c.gdt_base = 0x8000;
    c.gdt_limit = 31;
    memcpy(mem+0x8008,code32,8);
    memcpy(mem+0x8010,data32,8);
    memcpy(mem+0x8018,notpresent,8);
    eq("lookup code",b86_pm0_lookup_gdt(&c,8,&d),B86_PM0_OK);
    eq("code base",d.base,0);
    eq("code byte limit",d.limit,0xFFFFFFFFu);
    eq("code raw limit",d.raw_limit,0xFFFFFu);
    eq("code default32",d.default32,1);
    eq("code executable",d.executable,1);
    eq("code readable",d.read_write,1);
    eq("code DPL",d.dpl,0);
    eq("code present",d.present,1);
    eq("lookup data",b86_pm0_lookup_gdt(&c,0x13,&d),B86_PM0_OK);
    eq("data selector retains RPL",d.selector,0x13);
    eq("data executable",d.executable,0);
    eq("data writable",d.read_write,1);
    eq("not present",b86_pm0_lookup_gdt(&c,0x18,&d),B86_PM0_NOT_PRESENT);
    eq("missing present",d.present,0);
    eq("null selector",b86_pm0_lookup_gdt(&c,0,&d),B86_PM0_NULL);
    eq("LDT unimplemented",b86_pm0_lookup_gdt(&c,0x0C,&d),B86_PM0_LDT);
    eq("limit rejected",b86_pm0_lookup_gdt(&c,0x20,&d),B86_PM0_TABLE_LIMIT);
    c.gdt_limit = 10;
    eq("partial descriptor rejected",b86_pm0_lookup_gdt(&c,8,&d),B86_PM0_TABLE_LIMIT);
    c.gdt_limit = 0xFFFFu;
    c.gdt_base = B86_MEM_BYTES - 12;
    eq("physical out of range",b86_pm0_lookup_gdt(&c,8,&d),B86_PM0_MEM_LIMIT);
    eq("null ptr",b86_pm0_decode(NULL,8,&d),B86_PM0_BAD_ARGUMENT);
    printf("BB386 PM0 %s: %u checks, %u failed\n",bad?"FAIL":"PASS",total,bad);
    return bad != 0;
}

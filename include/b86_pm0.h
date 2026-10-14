/* blitz86 386 PM0: descriptor inspection only, NO protected-mode switch.
 * New state is deliberately not added to B86Cpu; the 286 Thumb-2 ABI and
 * 232 KiB native code cache are unchanged.
 */
#ifndef B86_PM0_H
#define B86_PM0_H
#include <stdint.h>
#include "b86.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct B86PM0Desc {
    uint32_t base;
    uint32_t limit;           /* expanded byte limit */
    uint32_t raw_limit;       /* 20-bit limit before G expansion */
    uint16_t selector;
    uint8_t access;          /* descriptor access byte */
    uint8_t flags;           /* descriptor flags: AVL/L/D-B/G in bits 0..3 */
    uint8_t dpl;
    uint8_t present;
    uint8_t code_data;       /* S bit */
    uint8_t executable;
    uint8_t conforming_expand;
    uint8_t read_write;
    uint8_t accessed;
    uint8_t default32;       /* D/B bit */
    uint8_t granularity;     /* G bit */
} B86PM0Desc;

enum { B86_PM0_OK=0, B86_PM0_NULL=1, B86_PM0_LDT=2,
       B86_PM0_TABLE_LIMIT=3, B86_PM0_MEM_LIMIT=4,
       B86_PM0_NOT_PRESENT=5, B86_PM0_BAD_ARGUMENT=6 };

/* Decode an eight-byte 386 segment descriptor. No state mutation. */
int b86_pm0_decode(const uint8_t bytes[8], uint16_t selector, B86PM0Desc *out);
/* Resolve a selector through the existing CPU's GDTR. TI=1 (LDT) unsupported.
 * This is NOT the protected-mode segment-register load/fault path. */
int b86_pm0_lookup_gdt(const B86Cpu *cpu, uint16_t selector, B86PM0Desc *out);
#ifdef __cplusplus
}
#endif
#endif

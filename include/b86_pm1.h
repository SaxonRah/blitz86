/* Stage 5C5C: 80386 descriptor-cache preparation, NOT a PE implementation.
 * These calls neither change the guest's segment registers nor enable PE.
 */
#ifndef B86_PM1_H
#define B86_PM1_H
#include "b86_pm0.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
 B86_PM1_OK=0, B86_PM1_ARGUMENT=1, B86_PM1_SELECTOR=2,
 B86_PM1_TABLE_LIMIT=3, B86_PM1_MEMORY_LIMIT=4,
 B86_PM1_NOT_PRESENT=5, B86_PM1_TYPE=6,
 B86_PM1_PRIVILEGE=7, B86_PM1_ACCESS=8,
 B86_PM1_SEGMENT_LIMIT=9, B86_PM1_NO_LDT=10,
 B86_PM1_UNSUPPORTED=11
};
enum { B86_PM1_READ=1, B86_PM1_WRITE=2, B86_PM1_EXEC=4 };
/* Loads the LDTR *shadow* from a system LDT descriptor in the GDT.
 * Does not execute LLDT or switch the CPU into protected mode. */
int b86_pm1_prepare_ldtr(B86Cpu *cpu, uint16_t selector);
/* Resolves GDT or prepared LDT; does not mutate guest memory or CPU. */
int b86_pm1_descriptor(const B86Cpu *cpu, uint16_t selector, B86PM0Desc *out);
/* Validates a data/stack selector and caches its hidden attributes.
 * Rejects CS (far control transfers require a separate future stage).
 * A null selector is accepted for DS,ES,FS,GS and makes them unusable.
 * cpl is the caller's current CPL, not a new machine-state setting. */
int b86_pm1_prepare_segment(B86Cpu *cpu, unsigned seg, uint16_t selector,
                            unsigned cpl);
/* Validates access, offset, width and the actual emulator memory bounds;
 * produces an address only on success. No memory reads or writes. */
int b86_pm1_translate(const B86Cpu *cpu, unsigned seg, uint32_t offset,
                       uint32_t width, unsigned access, uint32_t *linear);
#ifdef __cplusplus
}
#endif
#endif

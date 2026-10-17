/* Stage5C5F: first bounded 80386 protected-mode execution.
 * This is NOT full x86 protected mode: no paging, privilege transitions,
 * interrupt delivery, ModR/M memory operands, or instruction-set completion.
 * All unsupported cases return B86_UNSUPPORTED before architectural writes.
 */
#ifndef B86_PM4_H
#define B86_PM4_H
#include "b86_pm3.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One protected-mode step, including PM3's one-instruction transition.
 * Supported after far jump: 90, F4(CPL0), B0-BF, A0-A3; 66/67 and segment
 * overrides only where meaningful. Hidden CS/data cache is authoritative.
 * A failed step leaves EIP, registers and guest memory unchanged. */
int b86_pm4_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

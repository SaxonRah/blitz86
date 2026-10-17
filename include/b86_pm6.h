/* Stage5C5H: CPL0 protected-mode 80386 ModR/M MOV and arithmetic subset.
 * Cold interpreter, descriptor-backed memory; no paging or PM exception delivery.
 * Unsupported cases return B86_UNSUPPORTED without architectural writes.
 */
#ifndef B86_PM6_H
#define B86_PM6_H
#include "b86_pm5.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm6_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

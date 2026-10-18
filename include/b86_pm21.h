/* PM21 (development): CPL0 protected external interrupt vector injection. */
#ifndef B86_PM21_H
#define B86_PM21_H
#include "b86_pm20.h"
#define B86_PM21_STI_SHADOW 1u
#ifdef __cplusplus
extern "C" {
#endif
/* Called by the host AFTER PIC selects an 8-bit IRQ vector. Does not
 * inspect/acknowledge c->irq (host yield request). Returns B86_OK on
 * successful delivery, B86_UNSUPPORTED on a blocked/invalid IRQ, or
 * B86_SHUTDOWN for an already-shutdown processor. */
int b86_pm21_irq(B86Cpu *c, unsigned vector);
int b86_pm21_step(B86Cpu *c);
#ifdef __cplusplus
}
#endif
#endif

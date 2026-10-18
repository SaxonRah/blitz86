/* Stage5C5N: CPL0 32-bit protected IDT software INT and IRETD only. */
#ifndef B86_PM12_H
#define B86_PM12_H
#include "b86_pm11.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm12_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

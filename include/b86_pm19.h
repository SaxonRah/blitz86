/* PM19: protected-mode 80386 string engine + direction flag. */
#ifndef B86_PM19_H
#define B86_PM19_H
#include "b86_pm18.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm19_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif

#endif

/* 80386 protected-mode 0F integer opcode family; development milestone R1. */
#ifndef B86_PM16_H
#define B86_PM16_H
#include "b86_pm15.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm16_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

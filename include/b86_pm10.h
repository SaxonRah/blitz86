/* Stage5C5L: CPL0 protected direct far JMP/CALL and far RET.
 * No gates, privilege transitions, task switching, or exception delivery. */
#ifndef B86_PM10_H
#define B86_PM10_H
#include "b86_pm9.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm10_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

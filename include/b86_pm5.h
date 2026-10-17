/* Stage5C5G: bounded 80386 protected-mode arithmetic/relative branches.
 * PM4 remains the only handler for memory MOV and the initial PM3 far JMP.
 * Unsupported instructions return B86_UNSUPPORTED before committing state.
 */
#ifndef B86_PM5_H
#define B86_PM5_H
#include "b86_pm4.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm5_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

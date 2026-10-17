/* Stage5C5I: bounded CPL0 protected-mode stack and near control flow.
 * PUSH/POP r16/r32, PUSH imm8/imm16/imm32, CALL rel16/rel32,
 * RET near / RET near imm16. No far/privilege transitions, paging,
 * protected exceptions, stack switching, or general FF ModR/M forms.
 * All fetches/stack accesses/branch targets are validated before writes.
 */
#ifndef B86_PM7_H
#define B86_PM7_H
#include "b86_pm6.h"
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm7_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

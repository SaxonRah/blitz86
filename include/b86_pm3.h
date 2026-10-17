/* Stage5C5E: bounded 80386 CR0.PE entry + FIRST direct far JMP only.
 * No general protected execution, paging, descriptor A-bit writes or faults.
 * The initial real-mode CS cache is retained through PE activation;
 * a validated far JMP installs a PM2 code cache and then execution stops.
 */
#ifndef B86_PM3_H
#define B86_PM3_H
#include "b86_pm2.h"
#ifdef __cplusplus
extern "C" {
#endif
#define B86_PM3_TRANSITION_MARKER 0xE5u
/* In-flight transition lives in CS cache.reserved (the field is otherwise 0).
 * It is cleared by PM2's CS commit without extending B86Cpu/JIT ABI. */
enum { B86_PM3_OK=0, B86_PM3_UNSUPPORTED=1, B86_PM3_ARGUMENT=2 };
/* Enter PE at current CS:EIP, preserving the existing RM cache geometry.
 * Only CR0 bits PE MP EM TS ET (0..4) are representable. No mutation on fail. */
int b86_pm3_enter(B86Cpu *cpu, uint32_t requested_cr0);
/* Executes exactly one direct far JMP EA [ptr16:16 / 66 EA ptr16:32]
 * during the in-flight transition. No other PE instruction is supported.
 * Returns B86_UNSUPPORTED without changing CPU state if rejected. */
int b86_pm3_transition_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

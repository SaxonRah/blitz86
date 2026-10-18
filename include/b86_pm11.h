/* Stage5C5M: protected stack segment loads and one-instruction shadow. */
#ifndef B86_PM11_H
#define B86_PM11_H
#include "b86_pm10.h"
/* SS cache reserved byte: 2=just loaded; 1=mask delivery until one
 * following instruction completes. Host IRQ exit requests are distinct. */
#define B86_PM11_SHADOW_ARMED 2u
#define B86_PM11_SHADOW_NEXT  1u
#ifdef __cplusplus
extern "C" {
#endif
int b86_pm11_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

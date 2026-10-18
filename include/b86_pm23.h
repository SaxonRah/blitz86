/* PM23 development: protected IN/OUT plus minimal host-owned 8259/PIT0 ports. */
#ifndef B86_PM23_H
#define B86_PM23_H
#include "b86_pm22.h"
#ifdef __cplusplus
extern "C" {
#endif
/* State remains outside B86Cpu. The host may forward its existing in8/out8
 * hooks to port_in8/port_out8 using its own context/dispatch. */
typedef struct B86Pm23Hw {
    B86Pm22Pic pic;
    uint32_t pit_reload;        /* 1..65536, zero means disabled */
    uint32_t pit_phase;         /* input clocks accumulated, < reload */
    uint8_t pit_mode;          /* mode 2 / mode 3 supported */
    uint8_t pit_access;        /* 1 low; 2 high; 3 low then high */
    uint8_t pit_write_phase;   /* next byte in lobyte/hibyte sequence */
    uint8_t pit_low;
    uint8_t pic_init_step;     /* ICW2=1, ICW3=2, ICW4=3 */
    uint8_t pic_require_icw3;
    uint8_t pic_require_icw4;
    uint8_t pic_isr_read;      /* OCW3: read ISR rather than IRR */
    uint8_t pic_icw3;
    uint8_t pic_icw4;
} B86Pm23Hw;
int b86_pm23_hw_init(B86Pm23Hw *h, unsigned initial_base);
uint8_t b86_pm23_port_in8(B86Pm23Hw *h, unsigned port);
int b86_pm23_port_out8(B86Pm23Hw *h, unsigned port, unsigned value);
/* Advance by PIT INPUT CLOCKS (not CPU instructions). At most one pending
 * edge is latched in PIC IRR; missed ticks coalesce like PM22's edge model. */
int b86_pm23_advance(B86Pm23Hw *h,uint32_t clocks,uint32_t *edges);
int b86_pm23_poll(B86Cpu *cpu,B86Pm23Hw *h,unsigned *vector);
int b86_pm23_step(B86Cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif

/* M2 development: single 8259-style master PIC request/priority layer.
 * NOT port 0x20/0x21 register emulation or a cascaded slave PIC.
 * The host is responsible for PIT timing, register I/O and explicit EOI.
 * Keep this state OUTSIDE B86Cpu: no emulator ABI change. */
#ifndef B86_PM22_H
#define B86_PM22_H
#include "b86_pm21.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct B86Pm22Pic {
    uint8_t pending;       /* IRR: edge-latched IRQ0..7 */
    uint8_t in_service;    /* ISR: accepted, awaiting EOI */
    uint8_t mask;          /* IMR: 1 means masked */
    uint8_t vector_base;   /* 8-aligned remapped IDT base */
} B86Pm22Pic;
/* Returns B86_UNSUPPORTED unless vector_base is 8-aligned and <= 248. */
int b86_pm22_pic_init(B86Pm22Pic *pic, unsigned vector_base);
int b86_pm22_pic_raise(B86Pm22Pic *pic, unsigned irq);
int b86_pm22_pic_mask(B86Pm22Pic *pic, unsigned mask);
/* Specific EOI: a guest's OUT 20h, 60h|irq can map to this in future. */
int b86_pm22_pic_eoi(B86Pm22Pic *pic, unsigned irq);
/* Non-specific EOI: clears highest-priority currently serviced request. */
int b86_pm22_pic_eoi_highest(B86Pm22Pic *pic);
/* B86_OK delivered; B86_BUDGET means no eligible PIC request;
 * B86_UNSUPPORTED means a candidate was blocked/rejected by the CPU.
 * On any failure neither IRR nor ISR is consumed. */
int b86_pm22_pic_poll(B86Cpu *c, B86Pm22Pic *pic, unsigned *vector);
#ifdef __cplusplus
}
#endif
#endif

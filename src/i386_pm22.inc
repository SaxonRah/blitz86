/* M2: bounded fixed-priority 8259 master PIC => PM21 protected IDT.
 * This is the CPU/PIC host-side delivery boundary, not guest port I/O.
 * No automatic EOI, IRQ0 clock generation, or c->irq reinterpretation.
 * IRQ0 highest priority. While a request is in service, only strictly
 * higher-priority requests may nest; equal/lower pending bits remain set.
 */
#include "b86_pm22.h"
#ifndef B86_PM22_FLASH
#define B86_PM22_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM22_FLASH int b86_pm22_pic_init(B86Pm22Pic *p,unsigned base)
{
    if(!p || base>248u || (base&7u))return B86_UNSUPPORTED;
    p->pending=0;p->in_service=0;p->mask=0;p->vector_base=(uint8_t)base;
    return B86_OK;
}
B86_PM22_FLASH int b86_pm22_pic_raise(B86Pm22Pic *p,unsigned irq)
{
    if(!p || irq>=8u)return B86_UNSUPPORTED;
    p->pending=(uint8_t)(p->pending|(1u<<irq));return B86_OK;
}
B86_PM22_FLASH int b86_pm22_pic_mask(B86Pm22Pic *p,unsigned mask)
{
    if(!p||mask>255u)return B86_UNSUPPORTED;
    p->mask=(uint8_t)mask;return B86_OK;
}
B86_PM22_FLASH int b86_pm22_pic_eoi(B86Pm22Pic *p,unsigned irq)
{
    unsigned bit;
    if(!p||irq>=8u)return B86_UNSUPPORTED;
    bit=1u<<irq;
    if(!(p->in_service&bit))return B86_UNSUPPORTED;
    p->in_service=(uint8_t)(p->in_service&~bit);return B86_OK;
}
B86_PM22_FLASH int b86_pm22_pic_eoi_highest(B86Pm22Pic *p)
{
    unsigned i;
    if(!p)return B86_UNSUPPORTED;
    for(i=0;i<8;i++)if(p->in_service&(1u<<i))
        return b86_pm22_pic_eoi(p,i);
    return B86_UNSUPPORTED;
}
B86_PM22_FLASH int b86_pm22_pic_poll(B86Cpu *c,B86Pm22Pic *p,unsigned *vector)
{
    unsigned active=8u,irq,v;
    uint8_t eligible;
    int rc;
    if(!p||!c)return B86_UNSUPPORTED;
    for(irq=0u;irq<8u;irq++)if(p->in_service&(1u<<irq)){
        active=irq;break;
    }
    eligible=(uint8_t)(p->pending & (uint8_t)~p->mask);
    for(irq=0u;irq<active;irq++)if(eligible&(1u<<irq))break;
    if(irq>=active)return B86_BUDGET;
    v=(unsigned)p->vector_base+irq;
    rc=b86_pm21_irq(c,v);
    if(rc!=B86_OK)return rc;
    p->pending=(uint8_t)(p->pending & ~(1u<<irq));
    p->in_service=(uint8_t)(p->in_service|(1u<<irq));
    if(vector)*vector=v;
    return B86_OK;
}

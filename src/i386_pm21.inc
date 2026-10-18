/* PM21: explicit protected-mode maskable hardware IRQ delivery and CLI/STI.
 * The host must map PIC IRQ number to an IDT VECTOR. c->irq is a host-yield
 * notification, NOT an architectural IRQ number. No automatic PIC ack.
 * Same-CPL0 32-bit CS/SS and 32-bit IDT interrupt/trap gates only.
 * Reuse PM13 atomic frame preflight. No TSS, privilege changes, VM86, APIC,
 * PIC emulation, NMI, task gates, 16-bit gates or paging.
 * STI shadow is stored in the GS hidden-cache reserved byte (1=pending).
 */
#include "b86_pm21.h"
#ifndef B86_PM21_FLASH
#define B86_PM21_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM21_FLASH int b86_pm21_irq(B86Cpu *c,unsigned vector)
{
    uint64_t count;
    int rc;
    if(!c)return B86_UNSUPPORTED;
    if(c->model>=B86_CPU_386 && c->shutdown386)return B86_SHUTDOWN;
    if(!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||vector>255u||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !c->pm386.seg[B86_CS].valid||!(c->pm386.seg[B86_CS].access&8u)||
       !(c->pm386.seg[B86_CS].flags&4u)||(c->seg[B86_CS]&3u)||
       !c->pm386.seg[B86_SS].valid||
       !(c->flags&B86_IF)||
       c->pm386.seg[B86_SS].reserved||c->pm386.seg[B86_GS].reserved||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&(0x20000u|0x4000u)))return B86_UNSUPPORTED;
    count=c->icount;
    rc=b86_pm13_deliver(c,vector,0u,0u);
    /* An external interrupt is not a retired guest instruction. PM13
     * delivery increments icount for fault-instruction/INT accounting. */
    if(rc==B86_OK)c->icount=count;
    return rc;
}
B86_PM21_FLASH int b86_pm21_step(B86Cpu *c)
{
    uint8_t op,shadow;
    uint32_t start,ip;
    uint64_t before;
    int rc;
    if(!c || !c->mem || c->model!=B86_CPU_386||!(c->msw&1u)||
       c->shutdown386||c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !c->pm386.seg[B86_CS].valid||!(c->pm386.seg[B86_CS].access&8u)||
       !(c->pm386.seg[B86_CS].flags&4u)||(c->seg[B86_CS]&3u)||
       c->ip>UINT32_MAX-15u||c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&(0x20000u|0x4000u)))return b86_pm20_step(c);
    ip=start=c->ip;
    if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
    if(op==0xFAu||op==0xFBu){
        /* CPL0 only. Neither operation changes arithmetic status flags. */
        if(op==0xFAu){c->flags&=~(uint32_t)B86_IF;
                      c->pm386.seg[B86_GS].reserved=0u;}
        else{c->flags|=B86_IF;
             c->pm386.seg[B86_GS].reserved=B86_PM21_STI_SHADOW;}
        c->ip=ip;++c->icount;return B86_OK;
    }
    shadow=c->pm386.seg[B86_GS].reserved;
    before=c->icount;
    rc=b86_pm20_step(c);
    /* STI inhibits maskable IRQ through the following fully COMPLETED
     * instruction; partial REP chunks do not retire and must not clear it. */
    if(shadow==B86_PM21_STI_SHADOW &&
       (rc==B86_OK||rc==B86_HALT) && c->icount>before)
        c->pm386.seg[B86_GS].reserved=0u;
    return rc;
}

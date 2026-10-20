/* BB_386_STAGE5C5N_V1: first same-CPL protected IDT delivery subset.
 * Software INT n and INT3 through present 32-bit interrupt/trap gates,
 * plus IRETD. Requires CPL0, 32-bit CS and 32-bit gate. No 16-bit IDT
 * gates, task/privilege switches, error-code pushes, hardware IRQ or
 * automatic exception translation. Faults are fail-closed pending PM13.
 */
#include "b86_pm12.h"
#ifndef B86_PM12_FLASH
#define B86_PM12_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

static B86_PM12_FLASH uint32_t b86_pm12_load(const B86Cpu *c,uint32_t lin,unsigned n)
{
    uint32_t v=0;unsigned i;
    for(i=0;i<n;i++)v|=(uint32_t)(*b86_host(c,lin+i))<<(8u*i);
    return v;
}
static B86_PM12_FLASH void b86_pm12_store(B86Cpu *c,uint32_t lin,uint32_t v,unsigned n)
{
    unsigned i;
    for(i=0;i<n;i++)*b86_host(c,lin+i)=(uint8_t)(v>>(8u*i));
}
B86_PM12_FLASH int b86_pm12_step(B86Cpu *c)
{
    uint32_t start,ip,sp,newsp,slot0=0;
    uint32_t target=0,idt=0,oldip=0,flags=0;
    uint16_t selector=0;
    uint8_t op,b,attr=0;
    unsigned vec=0,gate=0,ss32=0;
    B86PM2Plan plan;
    B86PM2Fault fault;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm11_step(c);
    if(!c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || !(c->pm386.seg[B86_CS].flags&4u) ||
       c->ip>UINT32_MAX-15u)return B86_UNSUPPORTED;
    start=ip=c->ip;
    if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
    /* This stage explicitly admits no prefixes for these three forms.
     * Delegation lets the earlier interpreter handle its own opcodes. */
    if(op!=0xCDu && op!=0xCCu && op!=0xCFu)return b86_pm11_step(c);
    if(c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE ||
       (c->flags & (0x20000u | 0x4000u)))return B86_UNSUPPORTED;
    if(op==0xCDu){if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;vec=b;}
    else if(op==0xCCu)vec=3u;
    sp=c->r[B86_SP];ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    if(op!=0xCFu){
        /* Read IDT gate from linear, physically representable guest memory. */
        if((uint32_t)vec*8u+7u>c->idt_limit)return B86_UNSUPPORTED;
        if((uint64_t)c->idt_base+(uint64_t)vec*8u+8u>B86_RAM_BYTES)
            return B86_UNSUPPORTED;
        idt=c->idt_base+(uint32_t)vec*8u;
        attr=*b86_host(c,idt+5u);
        gate=attr&15u;
        if((gate!=0xEu && gate!=0xFu) || (attr&0xF0u)!=(uint8_t)0x80u ||
           *b86_host(c,idt+4u)!=0u)return B86_UNSUPPORTED;
        selector=(uint16_t)b86_pm12_load(c,idt+2u,2u);
        target=b86_pm12_load(c,idt,2u)|(b86_pm12_load(c,idt+6u,2u)<<16u);
        if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK ||
           !(plan.cache.flags&4u))return B86_UNSUPPORTED;
        oldip=ip;flags=c->flags|2u;
        /* Preflight full frame, including 16-bit SP wrap handling via SS.B.
         * Reject split/wrapped stack frames rather than partial stores. */
        newsp=b86_pm7_stack_add(sp,(int)ss32,(uint32_t)(0u-12u));
        if(b86_pm2_check_access(c,B86_SS,ss32?newsp:(newsp&0xFFFFu),
                                12u,B86_PM1_WRITE,&slot0,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        /* Plan is committed only after all gate/frame checks succeeded. */
        b86_pm12_store(c,slot0,oldip,4u);
        b86_pm12_store(c,slot0+4u,c->seg[B86_CS]&0xFFFFu,4u);
        b86_pm12_store(c,slot0+8u,flags,4u);
        smc(c,slot0,12u);
        c->r[B86_SP]=newsp;
        c->flags=flags & ~(uint32_t)(B86_TF|0x4000u|0x10000u);
        if(gate==0xEu)c->flags &= ~(uint32_t)B86_IF;
    }else{
        /* Same-level IRETD; VM86/task-return, trap-flag and IOPL changes
         * require machinery not yet present. Validate before popping. */
        if(b86_pm2_check_access(c,B86_SS,ss32?sp:(sp&0xFFFFu),
                                12u,B86_PM1_READ,&slot0,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        target=b86_pm12_load(c,slot0,4u);
        selector=(uint16_t)b86_pm12_load(c,slot0+4u,4u);
        flags=b86_pm12_load(c,slot0+8u,4u);
        if(flags & (B86_TF|0x3000u|0x4000u|0x10000u|0x20000u))
            return B86_UNSUPPORTED;
        if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK ||
           !(plan.cache.flags&4u))return B86_UNSUPPORTED;
        newsp=b86_pm7_stack_add(sp,(int)ss32,12u);
        c->r[B86_SP]=newsp;
        c->flags=flags|2u;
    }
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
    ++c->icount;
    return B86_OK;
}

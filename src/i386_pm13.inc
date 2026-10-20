/* BB_386_STAGE5C5O_V1: initial protected CPU exception delivery.
 * 0F 0B -> #UD(6) with saved FAULTING EIP, no error code.
 * MOV Sreg,r16 descriptor #GP(13), #NP(11), #SS(12) push error code.
 * MOV CS,r16 -> #UD(6). Only CPL0, 32-bit CS and IDT gate, 32-bit
 * stack frames, no privilege switches, nested escalation or external IRQ.
 * A failure to deliver remains B86_UNSUPPORTED, with guest state intact.
 */
#include "b86_pm13.h"
#ifndef B86_PM13_FLASH
#define B86_PM13_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
/* Unlike PM12 software INT, fault returns the address of its opcode.
 * Error-code exception pushes 16 bytes: error, EIP, CS, EFLAGS.
 */
static B86_PM13_FLASH int b86_pm13_deliver(B86Cpu *c,unsigned vector,
                                            unsigned error_valid,uint16_t error)
{
    uint32_t idt,target,slot,newsp,sp,flags;
    uint16_t selector;
    uint8_t attr;
    unsigned ss32,gate,width=error_valid?16u:12u;
    B86PM2Plan plan;
    B86PM2Fault fault;
    if(c->flags&(0x20000u|0x4000u))return B86_UNSUPPORTED;
    if((uint32_t)vector*8u+7u>c->idt_limit ||
       (uint64_t)c->idt_base+(uint64_t)vector*8u+8u>B86_RAM_BYTES)
        return B86_UNSUPPORTED;
    idt=c->idt_base+(uint32_t)vector*8u;
    attr=*b86_host(c,idt+5u);gate=attr&15u;
    if((gate!=0xEu&&gate!=0xFu) || (attr&0x90u)!=0x80u ||
       *b86_host(c,idt+4u)!=0u)return B86_UNSUPPORTED;
    selector=(uint16_t)b86_pm12_load(c,idt+2u,2u);
    target=b86_pm12_load(c,idt,2u)|(b86_pm12_load(c,idt+6u,2u)<<16u);
    if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK ||
       !(plan.cache.flags&4u))return B86_UNSUPPORTED;
    sp=c->r[B86_SP];ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    newsp=b86_pm7_stack_add(sp,(int)ss32,0u-width);
    if(b86_pm2_check_access(c,B86_SS,ss32?newsp:(newsp&0xFFFFu),
                            width,B86_PM1_WRITE,&slot,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    flags=c->flags|2u;
    if(error_valid){
        b86_pm12_store(c,slot,(uint32_t)error,4u);
        b86_pm12_store(c,slot+4u,c->ip,4u);
        b86_pm12_store(c,slot+8u,c->seg[B86_CS]&0xFFFFu,4u);
        b86_pm12_store(c,slot+12u,flags,4u);
    }else{
        b86_pm12_store(c,slot,c->ip,4u);
        b86_pm12_store(c,slot+4u,c->seg[B86_CS]&0xFFFFu,4u);
        b86_pm12_store(c,slot+8u,flags,4u);
    }
    smc(c,slot,width);
    c->r[B86_SP]=newsp;
    c->flags=flags & ~(uint32_t)(B86_TF|0x4000u|0x10000u);
    if(gate==0xEu)c->flags &= ~(uint32_t)B86_IF;
    /* PM2 commit only fails on programmer error; plan was validated above. */
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
    ++c->icount;
    return B86_OK;
}
B86_PM13_FLASH int b86_pm13_step(B86Cpu *c)
{
    uint32_t ip,start;
    uint8_t op,second;
    B86PM2Plan plan;
    B86PM2Fault fault;
    int rc;
    unsigned reg,selector;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm12_step(c);
    if(!c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || !(c->pm386.seg[B86_CS].flags&4u) ||
       c->ip>UINT32_MAX-15u)return B86_UNSUPPORTED;
    start=ip=c->ip;
    if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
    if(op!=0x0Fu && op!=0x8Eu)return b86_pm12_step(c);
    if(op==0x0Fu){
        if(!b86_pm4_byte(c,&ip,start,&second) || second!=0x0Bu)
            return b86_pm12_step(c);
        if(c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE ||
           (c->flags&(0x20000u|0x4000u)))return B86_UNSUPPORTED;
        return b86_pm13_deliver(c,6u,0u,0u);
    }
    if(!b86_pm4_byte(c,&ip,start,&second))return B86_UNSUPPORTED;
    /* Only register form: unhandled ModR/M and prefixed instructions still
     * use PM12->PM11->PM9's tested implementations. */
    if((second&0xC0u)!=0xC0u)return b86_pm12_step(c);
    reg=(second>>3u)&7u;
    if(reg==1u){ /* MOV CS is architecturally invalid. */
        if(c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE)return B86_UNSUPPORTED;
        return b86_pm13_deliver(c,6u,0u,0u);
    }
    if(reg==0u||reg==2u||reg==3u||reg==4u||reg==5u){
        selector=c->r[second&7u]&0xFFFFu;
        rc=b86_pm2_plan_segment(c,reg,(uint16_t)selector,0u,0u,&plan,&fault);
        if(rc==B86_PM2_FAULT && (fault.vector==B86_PM2_GP ||
           fault.vector==B86_PM2_NP || fault.vector==B86_PM2_SS)){
            if(c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE)return B86_UNSUPPORTED;
            return b86_pm13_deliver(c,fault.vector,1u,fault.error_code);
        }
    }
    return b86_pm12_step(c);
}

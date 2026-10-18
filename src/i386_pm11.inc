/* BB_386_STAGE5C5M_V1: protected MOV SS,r/m16 and POP SS.
 * CPL0 only, PE=1. CS checked by PM4 byte fetch. PM2 validates SS
 * descriptor before committing; stack reads are validated first.
 * Other protected interrupts, gates and MOV CS remain unsupported.
 * The SS hidden-cache reserved field encodes the pending interrupt/debug
 * shadow without changing the B86Cpu layout.
 */
#include "b86_pm11.h"
#ifndef B86_PM11_FLASH
#define B86_PM11_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM11_FLASH int b86_pm11_step(B86Cpu *c)
{
    B86PM6EA ea;
    B86PM2Plan plan;
    B86PM2Fault fault;
    uint32_t start,ip,oldsp,linear=0,selector=0,newsp=0;
    unsigned code32,op32,ad32,ss32,width,pref66=0,pref67=0,segpref=0,seg=B86_DS;
    uint8_t op;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm10_step(c);
    if(!c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32 && c->ip>0xFFFFu)return B86_UNSUPPORTED;
    start=ip=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            switch(op){
                case 0x26u:seg=B86_ES;break;case 0x2Eu:seg=B86_CS;break;
                case 0x36u:seg=B86_SS;break;case 0x3Eu:seg=B86_DS;break;
                case 0x64u:seg=B86_FS;break;default:seg=B86_GS;break;
            }
            continue;
        }
        break;
    }
    if(op!=0x8Eu && op!=0x17u)return b86_pm10_step(c);
    op32=code32^pref66;ad32=code32^pref67;
    if(op==0x8Eu){
        if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
        if(ea.reg!=B86_SS)return b86_pm10_step(c);
        if(segpref){if(ea.is_reg)return B86_UNSUPPORTED;ea.seg=seg;}
        if(ea.is_reg)selector=b86_pm6_reg(c,ea.rm,2u);
        else{
            if(b86_pm2_check_access(c,ea.seg,ea.off,2u,B86_PM1_READ,
                                    &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            selector=(uint32_t)*b86_host(c,linear) |
                     ((uint32_t)*b86_host(c,linear+1u)<<8u);
        }
    }else{
        if(segpref)return B86_UNSUPPORTED;
        width=op32?4u:2u;
        oldsp=c->r[B86_SP];ss32=!!(c->pm386.seg[B86_SS].flags&4u);
        if(b86_pm2_check_access(c,B86_SS,ss32?oldsp:(oldsp&0xFFFFu),
                                width,B86_PM1_READ,&linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        selector=(uint32_t)*b86_host(c,linear) |
                 ((uint32_t)*b86_host(c,linear+1u)<<8u);
        newsp=b86_pm7_stack_add(oldsp,(int)ss32,width);
    }
    if(b86_pm2_plan_segment(c,B86_SS,(uint16_t)selector,0u,0u,&plan,&fault)
       !=B86_PM2_OK)return B86_UNSUPPORTED;
    /* Code instruction fully fetched and source validated: one commit site. */
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
    if(op==0x17u)c->r[B86_SP]=newsp;
    c->pm386.seg[B86_SS].reserved=B86_PM11_SHADOW_ARMED;
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;
    return B86_OK;
}

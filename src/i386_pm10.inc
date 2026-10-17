/* BB_386_STAGE5C5L_V1: same-CPL protected far transfers.
 * EA ptr16:16/32, 9A ptr16:16/32, CB/CA far return [imm16].
 * PM2 rejects task/call gates and privilege transitions. All fetch,
 * descriptor, stack slots and guest memory validated before committing.
 * This stage deliberately does not handle MOV SS, protected IRQs or paging.
 */
#include "b86_pm10.h"
#ifndef B86_PM10_FLASH
#define B86_PM10_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

B86_PM10_FLASH int b86_pm10_step(B86Cpu *c)
{
    B86PM2Plan plan;
    B86PM2Fault fault;
    uint32_t start,ip,oldsp,sp1,sp2,linear1=0,linear2=0;
    uint32_t target=0,ret_ip=0,newsp=0,selector_slot=0,imm=0;
    uint16_t selector=0;
    unsigned op32,code32,ss32,width,k,pref66=0, pref67=0;
    uint8_t op,b;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 ||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm9_step(c);
    if(!c->pm386.seg[B86_CS].valid ||
       !(c->pm386.seg[B86_CS].access & 8u) ||
       (c->seg[B86_CS] & 3u) || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32 && c->ip>0xFFFFu)return B86_UNSUPPORTED;
    start=ip=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        break;
    }
    if(op!=0xEAu && op!=0x9Au && op!=0xCAu && op!=0xCBu)
        return b86_pm9_step(c);
    op32=code32 ^ pref66;
    width=op32?4u:2u;
    if(op==0xEAu || op==0x9Au){
        for(k=0;k<width;k++){
            if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
            target |= (uint32_t)b << (8u*k);
        }
        if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        selector=b;
        if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        selector |= (uint16_t)b<<8u;
    }else{
        if(op==0xCAu){
            if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
            imm=b;
            if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
            imm|=(uint32_t)b<<8u;
        }
    }
    if(pref67){ /* 67 has no effect on these instruction encodings. */ }
    ret_ip=code32?ip:(ip&0xFFFFu);
    if(op==0x9Au || op==0xCAu || op==0xCBu){
        B86PM2Fault sf;
        oldsp=c->r[B86_SP];
        ss32=!!(c->pm386.seg[B86_SS].flags&4u);
        if(op==0x9Au){
            /* Push old CS first, then return offset; RETF pops offset, CS. */
            sp1=b86_pm7_stack_add(oldsp,(int)ss32,0u-width);
            sp2=b86_pm7_stack_add(sp1,(int)ss32,0u-width);
            if(b86_pm2_check_access(c,B86_SS,ss32?sp1:(sp1&0xFFFFu),
                                    width,B86_PM1_WRITE,&linear1,&sf)!=B86_PM2_OK ||
               b86_pm2_check_access(c,B86_SS,ss32?sp2:(sp2&0xFFFFu),
                                    width,B86_PM1_WRITE,&linear2,&sf)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            newsp=sp2;
        }else{
            sp1=b86_pm7_stack_add(oldsp,(int)ss32,width);
            sp2=b86_pm7_stack_add(sp1,(int)ss32,width);
            if(b86_pm2_check_access(c,B86_SS,ss32?oldsp:(oldsp&0xFFFFu),
                                    width,B86_PM1_READ,&linear1,&sf)!=B86_PM2_OK ||
               b86_pm2_check_access(c,B86_SS,ss32?sp1:(sp1&0xFFFFu),
                                    width,B86_PM1_READ,&linear2,&sf)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            for(k=0;k<width;k++){
                target |= (uint32_t)*b86_host(c,linear1+k)<<(8u*k);
                selector_slot |= (uint32_t)*b86_host(c,linear2+k)<<(8u*k);
            }
            if(width==2u)target&=0xFFFFu;
            selector=(uint16_t)selector_slot;
            newsp=b86_pm7_stack_add(sp2,(int)ss32,imm);
        }
    }
    /* Same-level protected segment plan checks descriptor type, presence,
     * CPL/RPL, executable access, target limit, and physical bounds. */
    if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    /* Do not allow a protected branch to land in an unrepresentable 16-bit
     * code offset. PM2 has checked the *destination* descriptor. */
    if(!(plan.cache.flags&4u) && target>0xFFFFu)return B86_UNSUPPORTED;
    if(op==0x9Au){
        uint32_t old_selector=c->seg[B86_CS]&0xFFFFu;
        for(k=0;k<width;k++){
            *b86_host(c,linear1+k)=(uint8_t)(old_selector>>(8u*k));
            *b86_host(c,linear2+k)=(uint8_t)(ret_ip>>(8u*k));
        }
        smc(c,linear1,width);smc(c,linear2,width);
        c->r[B86_SP]=newsp;
    }else if(op==0xCAu || op==0xCBu){
        c->r[B86_SP]=newsp;
    }
    /* Plan was preflighted, so commit cannot fail for unmodified CPU. */
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    ++c->icount;
    return B86_OK;
}

/* BB_386_STAGE5C5Q_V1: CPL0 protected arithmetic faults and BOUND.
 * F6/F7 DIV/IDIV register or memory divisor zero -> vector 0 #DE.
 * 62 /r BOUND m16&16 or m32&32 signed out-of-range -> vector 5 #BR;
 * in-range BOUND retires with flags and operands unchanged.
 * 66/67 and one explicit segment override, full ModR/M/SIB preflight.
 * Normal division and unsupported faults defer to PM14 without execution.
 * No 64-bit guest arithmetic, page faults, or generalized nested faults.
 */
#include "b86_pm15.h"
#ifndef B86_PM15_FLASH
#define B86_PM15_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM15_FLASH int b86_pm15_step(B86Cpu *c)
{
    uint32_t ip,start,linear=0,divisor=0,lo=0,hi=0,index=0;
    uint8_t op;
    B86PM6EA ea;
    B86PM2Fault fault;
    unsigned seg=B86_DS,width,amount;
    int pref66=0,pref67=0,segpref=0,op32,ad32;
    if(c && c->model>=B86_CPU_386 && c->shutdown386)return B86_SHUTDOWN;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER ||
       !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || !(c->pm386.seg[B86_CS].flags&4u) ||
       c->ip>UINT32_MAX-15u || c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE ||
       (c->flags&0x20000u))return b86_pm14_step(c);
    start=ip=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return b86_pm14_step(c);pref66=1;continue;}
        if(op==0x67u){if(pref67)return b86_pm14_step(c);pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return b86_pm14_step(c);
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
    if(op!=0xF6u && op!=0xF7u && op!=0x62u)return b86_pm14_step(c);
    op32=(!!(c->pm386.seg[B86_CS].flags&4u))^pref66;
    ad32=(!!(c->pm386.seg[B86_CS].flags&4u))^pref67;
    /* Effective address parser performs bounded CS fetch before data loads. */
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    if(op==0x62u){
        if(ea.is_reg)return b86_pm14_step(c); /* invalid BOUND mod=11 unsupported */
        width=op32?4u:2u;amount=width*2u;
        if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,
                                amount,B86_PM1_READ,&linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        lo=b86_pm12_load(c,linear,width);
        hi=b86_pm12_load(c,linear+width,width);
        index=b86_pm6_reg(c,ea.reg,width);
        if(width==2u){
            if((int32_t)(int16_t)index<(int32_t)(int16_t)lo ||
               (int32_t)(int16_t)index>(int32_t)(int16_t)hi)
                return b86_pm13_deliver(c,5u,0u,0u);
        }else{
            if((int32_t)index<(int32_t)lo || (int32_t)index>(int32_t)hi)
                return b86_pm13_deliver(c,5u,0u,0u);
        }
        c->ip=ip;++c->icount;return B86_OK;
    }
    /* Only /6 DIV and /7 IDIV are handled: zero divisor faults before
     * any change to dividend, flags, EIP or register contents. */
    if(ea.reg!=6u && ea.reg!=7u)return b86_pm14_step(c);
    width=op==0xF6u?1u:(op32?4u:2u);
    if(ea.is_reg)divisor=b86_pm6_reg(c,ea.rm,width);
    else{
        if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,
                                width,B86_PM1_READ,&linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        divisor=b86_pm12_load(c,linear,width);
    }
    if(divisor==0u)return b86_pm13_deliver(c,0u,0u,0u);
    return b86_pm14_step(c);
}

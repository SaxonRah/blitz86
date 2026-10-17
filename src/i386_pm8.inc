/* BB_386_STAGE5C5J_V1: protected FF /2 /4 /6 and 8F /0 forms.
 * Existing PM6 ModR/M decoder resolves 16/32-bit SIB, displacement and
 * segment defaults. No new fields in hot B86Cpu / real-mode path.
 */
#include "b86_pm8.h"
#ifndef B86_PM8_FLASH
#define B86_PM8_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

B86_PM8_FLASH int b86_pm8_step(B86Cpu *c)
{
    uint32_t start,ip,modrm_pos,old_sp,new_sp=0u,retip=0u,operand=0u,linear=0u;
    uint32_t src_linear=0u,dst_linear=0u,target=0u;
    B86PM6EA ea;
    B86PM2Fault fault;
    unsigned kind,seg=B86_DS,op32,ad32,width,ss32,reg=0u;
    unsigned pref66=0u,pref67=0u,segpref=0u;
    uint8_t op,modrm=0u,sib=0u;
    int code32;
    if (!c || c->model!=B86_CPU_386 || !c->mem || !(c->msw&1u) ||
        c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm7_step(c);
    if (!c->pm386.seg[B86_CS].valid ||
        !(c->pm386.seg[B86_CS].access&8u) ||
        (c->seg[B86_CS]&3u)!=0u || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if (!code32 && c->ip>0xFFFFu)return B86_UNSUPPORTED;
    start=ip=c->ip;
    for (;;) {
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            switch(op){
            case 0x26:seg=B86_ES;break;
            case 0x2E:seg=B86_CS;break;
            case 0x36:seg=B86_SS;break;
            case 0x3E:seg=B86_DS;break;
            case 0x64:seg=B86_FS;break;
            default:seg=B86_GS;break;
            }
            continue;
        }
        break;
    }
    if(op!=0xFFu && op!=0x8Fu)return b86_pm7_step(c);
    /* The first ModR/M byte determines the instruction family. Obtain it
     * using the checked fetch helper, then let PM6 decode the entire EA. */
    modrm_pos=ip;
    {
        uint32_t preview=ip;
        if(!b86_pm4_byte(c,&preview,start,&modrm))return B86_UNSUPPORTED;
        reg=(modrm>>3)&7u;
    }
    if(op==0xFFu){
        if(reg==2u)kind=1u;        /* CALL r/m16/32 */
        else if(reg==4u)kind=2u;   /* JMP r/m16/32 */
        else if(reg==6u)kind=3u;   /* PUSH r/m16/32 */
        else return B86_UNSUPPORTED; /* far forms and INC/DEC not yet handled */
    }else{
        if(reg!=0u)return B86_UNSUPPORTED; /* POP r/m16/32 */
        kind=4u;
    }
    op32=(unsigned)(code32^(int)pref66);
    ad32=(unsigned)(code32^(int)pref67);
    width=op32?4u:2u;
    if(!code32 && op32 && (kind==1u||kind==2u))return B86_UNSUPPORTED;
    /* Indirect near target widths match the existing near CALL/RET subset. */
    if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
    if(ea.reg!=reg || (ea.is_reg && segpref))return B86_UNSUPPORTED;
    retip=code32?ip:(ip&0xFFFFu);
    ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    old_sp=c->r[B86_SP];
    if(kind==4u){
        /* POP r/m uses the old SS:SP as source and its post-increment value
         * for the destination address whenever 32-bit EA uses ESP as base.
         * Neither SP nor memory is modified during this preflight. */
        new_sp=b86_pm7_stack_add(old_sp,(int)ss32,width);
        if(!ea.is_reg && ad32 && (modrm&7u)==4u){
            uint32_t s_ip=modrm_pos+1u;
            if(!b86_pm4_byte(c,&s_ip,start,&sib))return B86_UNSUPPORTED;
            if((sib&7u)==4u)
                ea.off+=(uint32_t)(new_sp-old_sp);
        }
        if(b86_pm2_check_access(c,B86_SS,ss32?old_sp:(old_sp&0xFFFFu),
                                width,B86_PM1_READ,&src_linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        if(!ea.is_reg){
            if(segpref)ea.seg=seg;
            if(b86_pm2_check_access(c,ea.seg,ea.off,width,B86_PM1_WRITE,
                                    &dst_linear,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
        }
        for(unsigned i=0;i<width;i++)
            operand|=(uint32_t)(*b86_host(c,src_linear+i))<<(8u*i);
        /* Commit after every potentially failing read/write check. */
        c->r[B86_SP]=new_sp;
        if(ea.is_reg)b86_pm6_putreg(c,ea.rm,width,operand);
        else{
            for(unsigned i=0;i<width;i++)
                *b86_host(c,dst_linear+i)=(uint8_t)(operand>>(8u*i));
            smc(c,dst_linear,width);
        }
        c->ip=retip;++c->icount;return B86_OK;
    }
    if(ea.is_reg)operand=b86_pm6_reg(c,ea.rm,width);
    else{
        if(segpref)ea.seg=seg;
        if(b86_pm2_check_access(c,ea.seg,ea.off,width,B86_PM1_READ,
                                &src_linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        for(unsigned i=0;i<width;i++)
            operand|=(uint32_t)(*b86_host(c,src_linear+i))<<(8u*i);
    }
    if(kind==1u||kind==2u){
        target=width==2u?(operand&0xFFFFu):operand;
        if(!code32)target&=0xFFFFu;
        if(b86_pm2_check_access(c,B86_CS,target,1u,B86_PM1_EXEC,
                                &linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
    }
    if(kind==1u||kind==3u){
        new_sp=b86_pm7_stack_add(old_sp,(int)ss32,0u-width);
        if(b86_pm2_check_access(c,B86_SS,ss32?new_sp:(new_sp&0xFFFFu),
                                width,B86_PM1_WRITE,&dst_linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        if(kind==1u)operand=retip;
        for(unsigned i=0;i<width;i++)
            *b86_host(c,dst_linear+i)=(uint8_t)(operand>>(8u*i));
        smc(c,dst_linear,width);
        c->r[B86_SP]=new_sp;
    }
    c->ip=(kind==1u||kind==2u)?target:retip;
    ++c->icount;
    return B86_OK;
}

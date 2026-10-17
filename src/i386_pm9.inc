/* BB_386_STAGE5C5K_V1: descriptor-backed protected MOV Sreg, LEA and XCHG.
 * Supported opcodes: 8C, 8E (except loading CS/SS), 8D, 86, 87.
 * MOV SS is deliberately refused until the interrupt-shadow semantics are
 * implemented. No exception dispatch, paging or privilege switches here.
 * All decoder and access failure paths are atomic and fail closed.
 */
#include "b86_pm9.h"
#ifndef B86_PM9_FLASH
#define B86_PM9_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

B86_PM9_FLASH int b86_pm9_step(B86Cpu *c)
{
    B86PM6EA ea;
    B86PM2Fault fault;
    B86PM2Plan plan;
    uint32_t start,ip,addr=0u,old_value=0u,reg_value=0u,linear=0u;
    uint32_t result_width=0u,mem_width=0u,read_width=0u;
    unsigned seg=B86_DS,seg_index,code32,op32,ad32;
    unsigned pref66=0u,pref67=0u,segpref=0u;
    uint8_t op;
    if(!c || c->model!=B86_CPU_386 || !c->mem || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm8_step(c);
    if(!c->pm386.seg[B86_CS].valid ||
       !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u)!=0u || c->ip>UINT32_MAX-15u)
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
            segpref=1u;
            switch(op){
            case 0x26u:seg=B86_ES;break;
            case 0x2Eu:seg=B86_CS;break;
            case 0x36u:seg=B86_SS;break;
            case 0x3Eu:seg=B86_DS;break;
            case 0x64u:seg=B86_FS;break;
            default:seg=B86_GS;break;
            }
            continue;
        }
        break;
    }
    if(op!=0x8Cu && op!=0x8Eu && op!=0x8Du && op!=0x86u && op!=0x87u)
        return b86_pm8_step(c);
    op32=code32^pref66;
    ad32=code32^pref67;
    if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
    if(ea.is_reg && segpref)return B86_UNSUPPORTED;
    if(segpref)ea.seg=seg;
    result_width=op32?4u:2u;
    if(op==0x8Du){
        /* LEA requires a memory-form effective address, but never accesses
         * that address; segment limits and memory mappings are irrelevant. */
        if(ea.is_reg || segpref)return B86_UNSUPPORTED;
        addr=ad32?ea.off:(ea.off&0xFFFFu);
        b86_pm6_putreg(c,ea.reg,result_width,addr);
    }else if(op==0x8Cu || op==0x8Eu){
        /* ModR/M.reg is a SEGMENT register index, not a GPR index. */
        seg_index=ea.reg;
        if(seg_index>5u)return B86_UNSUPPORTED;
        if(op==0x8Eu){
            /* MOV CS is invalid; MOV SS needs an interrupt shadow. */
            if(seg_index==B86_CS || seg_index==B86_SS)return B86_UNSUPPORTED;
            if(ea.is_reg)old_value=b86_pm6_reg(c,ea.rm,2u);
            else{
                if(b86_pm2_check_access(c,ea.seg,ea.off,2u,B86_PM1_READ,
                                        &linear,&fault)!=B86_PM2_OK)
                    return B86_UNSUPPORTED;
                old_value=(uint32_t)*b86_host(c,linear) |
                         ((uint32_t)*b86_host(c,linear+1u)<<8u);
            }
            /* Validate descriptor before committing the visible selector.
             * PM2 plans keep the old data cache and selector untouched on
             * all selector/privilege/presence failures. */
            if(b86_pm2_plan_segment(c,seg_index,(uint16_t)old_value,0u,0u,
                                    &plan,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
        }else{
            /* Memory destination is always 16 bits. For 32-bit register
             * operands MOV r32,Sreg zero extends the selector. */
            if(seg_index<=B86_DS)old_value=c->seg[seg_index];
            else old_value=c->seg386[seg_index-B86_FS];
            if(ea.is_reg)b86_pm6_putreg(c,ea.rm,result_width,old_value);
            else{
                if(b86_pm2_check_access(c,ea.seg,ea.off,2u,B86_PM1_WRITE,
                                        &linear,&fault)!=B86_PM2_OK)
                    return B86_UNSUPPORTED;
                *b86_host(c,linear)=(uint8_t)old_value;
                *b86_host(c,linear+1u)=(uint8_t)(old_value>>8u);
                smc(c,linear,2u);
            }
        }
    }else{
        /* XCHG r/m8,r8 or r/m16/32,r16/32. Preflight read + write before
         * either operand changes. LOCK effects are not modelled. */
        mem_width=read_width=(op==0x86u)?1u:result_width;
        reg_value=b86_pm6_reg(c,ea.reg,read_width);
        if(ea.is_reg)old_value=b86_pm6_reg(c,ea.rm,read_width);
        else{
            if(b86_pm2_check_access(c,ea.seg,ea.off,mem_width,
                                    B86_PM1_READ|B86_PM1_WRITE,
                                    &linear,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            for(unsigned i=0;i<mem_width;i++)
                old_value|=(uint32_t)*b86_host(c,linear+i)<<(8u*i);
        }
        if(ea.is_reg)b86_pm6_putreg(c,ea.rm,read_width,reg_value);
        else{
            for(unsigned i=0;i<mem_width;i++)
                *b86_host(c,linear+i)=(uint8_t)(reg_value>>(8u*i));
            smc(c,linear,mem_width);
        }
        b86_pm6_putreg(c,ea.reg,read_width,old_value);
    }
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;
    return B86_OK;
}

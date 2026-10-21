/* B86_PM_UNIFIED_V8: 386 protected-mode conversions, multiplication and
 * far-pointer loads, shared by 16- and 32-bit code segments.
 * 0F B6/B7/BE/BF MOVZX/MOVSX; 0F AF and 69/6B signed IMUL;
 * C4/C5 LES/LDS, 0F B2/B4/B5 LSS/LFS/LGS.
 * Uses PM4 checked instruction fetch, PM6 16/32-bit addressing and register
 * access, PM2 segment access/descriptor transactional planning. Does not add
 * any CPU, SRAM or JIT state.  Returns -1 for unrelated opcode families.
 * Privilege changes, paging and task mode remain unsupported.
 */
#ifndef B86_PM28_FLASH
#define B86_PM28_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM28_FLASH uint32_t b86_pm28_read(const B86Cpu *c,uint32_t lin,unsigned width)
{
    uint32_t v=0u;
    for(unsigned i=0u;i<width;i++)v|=(uint32_t)*b86_host(c,lin+i)<<(i*8u);
    return v;
}
static B86_PM28_FLASH int64_t b86_pm28_signed(uint32_t v,unsigned width)
{
    return width==1u?(int64_t)(int8_t)v:
           width==2u?(int64_t)(int16_t)v:(int64_t)(int32_t)v;
}
static B86_PM28_FLASH int b86_pm28_step(B86Cpu *c)
{
    B86PM6EA ea;
    B86PM2Fault fault;
    B86PM2Plan plan;
    uint32_t start,ip,linear=0u,value=0u,imm=0u,target=0u;
    int pref66=0,pref67=0,segpref=0,code32,op32,ad32;
    unsigned seg=B86_DS,kind=0u,srcwidth=0u,w=0u,sreg=B86_DS;
    uint8_t op,ext=0u;
    if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
       c->shutdown386||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !c->pm386.seg[B86_CS].valid||
       !(c->pm386.seg[B86_CS].access&8u)||
       (c->seg[B86_CS]&3u)||c->ip>UINT32_MAX-15u||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&0x20000u))return -1;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32 && c->ip>0xFFFFu)return -1;
    ip=start=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            seg=op==0x26u?B86_ES:op==0x2Eu?B86_CS:
                op==0x36u?B86_SS:op==0x3Eu?B86_DS:
                op==0x64u?B86_FS:B86_GS;
            continue;
        }
        break;
    }
    if(op==0x0Fu){
        if(!b86_pm4_byte(c,&ip,start,&ext))return B86_UNSUPPORTED;
        if(ext==0xB6u||ext==0xB7u||ext==0xBEu||ext==0xBFu)kind=1u;
        else if(ext==0xAFu)kind=2u;
        else if(ext==0xB2u||ext==0xB4u||ext==0xB5u){
            kind=3u;
            sreg=ext==0xB2u?B86_SS:ext==0xB4u?B86_FS:B86_GS;
        }else return -1;
    }else if(op==0x69u||op==0x6Bu)kind=2u;
    else if(op==0xC4u||op==0xC5u){kind=3u;sreg=op==0xC4u?B86_ES:B86_DS;}
    else return -1;
    op32=code32^pref66;
    ad32=code32^pref67;
    w=op32?4u:2u;
    if(kind==1u){
        srcwidth=(ext==0xB6u||ext==0xBEu)?1u:2u;
        /* MOVZX/MOVSX r16,r/m16 is not an 80386 encoding. */
        if(srcwidth==2u && w==2u)return B86_UNSUPPORTED;
    }else srcwidth=w;
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    if(segpref&&!ea.is_reg)ea.seg=seg;
    if(kind==3u){
        uint16_t selector;
        if(ea.is_reg)return B86_UNSUPPORTED; /* far pointer must be m16:16/32 */
        if(b86_pm2_check_access(c,ea.seg,ea.off,w+2u,B86_PM1_READ,
                                &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
        target=b86_pm28_read(c,linear,w);
        selector=(uint16_t)b86_pm28_read(c,linear+w,2u);
        if(b86_pm2_plan_segment(c,sreg,selector,0u,0u,&plan,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        /* All memory/selector checks have passed; segment commit cannot
           fail for an unmodified CPU, so neither half is written on fault. */
        if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        b86_pm6_putreg(c,ea.reg,w,target);
        if(sreg==B86_SS)c->pm386.seg[B86_SS].reserved=B86_PM11_SHADOW_ARMED;
    }else{
        if(!ea.is_reg){
            if(b86_pm2_check_access(c,ea.seg,ea.off,srcwidth,B86_PM1_READ,
                                    &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            value=b86_pm28_read(c,linear,srcwidth);
        }else value=b86_pm6_reg(c,ea.rm,srcwidth);
        if(kind==1u){
            uint32_t result=(ext==0xBEu||ext==0xBFu)?
                (uint32_t)b86_pm28_signed(value,srcwidth):value;
            b86_pm6_putreg(c,ea.reg,w,result);
        }else{
            uint32_t factor=b86_pm6_reg(c,ea.reg,w);
            int64_t product;
            if(op==0x69u||op==0x6Bu){
                unsigned immsz=op==0x69u?w:1u;
                if(!b86_pm6_imm(c,&ip,start,immsz,&imm))return B86_UNSUPPORTED;
                factor=(uint32_t)b86_pm28_signed(imm,immsz);
            }
            product=b86_pm28_signed(factor,w)*b86_pm28_signed(value,w);
            uint32_t result=(uint32_t)product;
            if(w==2u)result&=0xFFFFu;
            int overflow=product!=b86_pm28_signed(result,w);
            b86_pm6_putreg(c,ea.reg,w,result);
            c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF))|
                     (overflow?(B86_CF|B86_OF):0u);
        }
    }
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;
    return B86_OK;
}

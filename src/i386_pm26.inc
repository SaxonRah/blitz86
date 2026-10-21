/* B86_PM_UNIFIED_V6: protected segment stacks and mixed-width IDT frames.
 * Runs before PM25; returns -1 for unrelated opcodes. CPL0, same-privilege,
 * 16/32-bit code and IDT interrupt/trap gates. No task gates, TSS stack
 * switching, paging, VM86, inter-privilege IRET, or blanket IRQ routing.
 * All protected memory writes are preflighted before guest-state commit.
 */
#ifndef B86_PM26_FLASH
#define B86_PM26_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM26_FLASH uint32_t b86_pm26_read(B86Cpu *c,uint32_t at,unsigned n)
{
    uint32_t v=0u;
    for(unsigned i=0;i<n;i++)v|=(uint32_t)(*b86_host(c,at+i))<<(8u*i);
    return v;
}
static B86_PM26_FLASH void b86_pm26_write(B86Cpu *c,uint32_t at,uint32_t v,unsigned n)
{
    for(unsigned i=0;i<n;i++)*b86_host(c,at+i)=(uint8_t)(v>>(8u*i));
}
static B86_PM26_FLASH int b86_pm26_segment_op(B86Cpu *c, unsigned reg,
                                                int ispop, unsigned w,
                                                uint32_t next)
{
    B86PM2Fault fault;
    B86PM2Plan plan;
    uint32_t sp=c->r[B86_SP],slot=0,val,newsp;
    int ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    if(reg>5u||(ispop && reg==B86_CS))return B86_UNSUPPORTED;
    newsp=b86_pm7_stack_add(sp,ss32,ispop?w:(uint32_t)(0u-w));
    if(b86_pm2_check_access(c,B86_SS,
            ss32?(ispop?sp:newsp):((ispop?sp:newsp)&0xFFFFu),
            w,ispop?B86_PM1_READ:B86_PM1_WRITE,&slot,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    if(ispop){
        val=b86_pm26_read(c,slot,w)&0xFFFFu;
        if(b86_pm2_plan_segment(c,reg,(uint16_t)val,0u,0u,&plan,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
        if(reg==B86_SS)c->pm386.seg[B86_SS].reserved=B86_PM11_SHADOW_ARMED;
    }else{
        val=(reg<=B86_DS?c->seg[reg]:c->seg386[reg-B86_FS])&0xFFFFu;
        b86_pm26_write(c,slot,val,w);
        smc(c,slot,w);
    }
    c->r[B86_SP]=newsp;
    c->ip=next;
    ++c->icount;
    return B86_OK;
}
static B86_PM26_FLASH int b86_pm26_interrupt(B86Cpu *c,unsigned vec,
                                               uint32_t return_ip)
{
    B86PM2Plan plan;
    B86PM2Fault fault;
    uint32_t gateat,target,slot=0,newsp,sp,oldflags;
    unsigned gate,w,ss32;
    uint8_t attr;
    uint16_t selector;
    if(vec>255u || (uint32_t)vec*8u+7u>c->idt_limit ||
       (!c->pg&&(uint64_t)c->idt_base+(uint64_t)vec*8u+8u>B86_RAM_BYTES))
        return B86_UNSUPPORTED;
    gateat=c->idt_base+vec*8u;
    attr=*b86_host(c,gateat+5u);
    gate=attr&15u;
    if(gate!=6u && gate!=7u && gate!=14u && gate!=15u)
        return B86_UNSUPPORTED;
    if(!(attr&0x80u) || (attr&0x10u) || *b86_host(c,gateat+4u))
        return B86_UNSUPPORTED;
    /* CPL0 only. All software INT gates still need DPL >= CPL. */
    selector=(uint16_t)b86_pm26_read(c,gateat+2u,2u);
    target=b86_pm26_read(c,gateat,2u);
    if(gate>=14u)target|=b86_pm26_read(c,gateat+6u,2u)<<16u;
    w=(gate>=14u)?4u:2u;
    if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    if(!(plan.cache.flags&4u) && target>0xFFFFu)return B86_UNSUPPORTED;
    sp=c->r[B86_SP];ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    newsp=b86_pm7_stack_add(sp,(int)ss32,(uint32_t)(0u-3u*w));
    if(b86_pm2_check_access(c,B86_SS,ss32?newsp:(newsp&0xFFFFu),
                            3u*w,B86_PM1_WRITE,&slot,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    oldflags=c->flags|2u;
    b86_pm26_write(c,slot,return_ip,w);
    b86_pm26_write(c,slot+w,c->seg[B86_CS]&0xFFFFu,w);
    b86_pm26_write(c,slot+2u*w,oldflags,w);
    smc(c,slot,3u*w);
    c->r[B86_SP]=newsp;
    c->flags=oldflags&~(uint32_t)(B86_TF|0x4000u|0x10000u);
    if(gate==6u||gate==14u)c->flags&=~(uint32_t)B86_IF;
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
    ++c->icount;
    return B86_OK;
}
static B86_PM26_FLASH int b86_pm26_iret(B86Cpu *c,unsigned w)
{
    B86PM2Plan plan;
    B86PM2Fault fault;
    uint32_t sp=c->r[B86_SP],slot=0,target,flags,newsp;
    unsigned ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    uint16_t selector;
    if(b86_pm2_check_access(c,B86_SS,ss32?sp:(sp&0xFFFFu),
                            3u*w,B86_PM1_READ,&slot,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    target=b86_pm26_read(c,slot,w);
    selector=(uint16_t)b86_pm26_read(c,slot+w,w);
    flags=b86_pm26_read(c,slot+2u*w,w);
    if(w==2u)flags=(c->flags&0xFFFF0000u)|(flags&0xFFFFu);
    /* No task returns, VM86, RF, IOPL changes or debug-trap emulation. */
    /* CPL0 may restore IOPL; RF is cleared by any completed instruction. */
    if(flags&(B86_TF|0x20000u))
        return B86_UNSUPPORTED;
    flags&=~0x10000u;
    if(b86_pm2_plan_segment(c,B86_CS,selector,0u,target,&plan,&fault)!=B86_PM2_OK)
        return B86_UNSUPPORTED;
    if(!(plan.cache.flags&4u)&&target>0xFFFFu)return B86_UNSUPPORTED;
    newsp=b86_pm7_stack_add(sp,(int)ss32,3u*w);
    if(b86_pm2_commit_segment(c,&plan,0u)!=B86_PM2_OK)return B86_UNSUPPORTED;
    c->r[B86_SP]=newsp;
    c->flags=flags|2u;
    ++c->icount;
    return B86_OK;
}
static B86_PM26_FLASH int b86_pm26_step(B86Cpu *c)
{
    uint32_t start,ip;
    unsigned reg=0u,code32,w,vec=0u;
    int pop=-1,p66=0,p67=0,segpref=0;
    uint8_t op,ext=0,b;
    if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
       c->shutdown386||c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !c->pm386.seg[B86_CS].valid||!(c->pm386.seg[B86_CS].access&8u)||
       (c->seg[B86_CS]&3u)||c->ip>UINT32_MAX-15u||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&0x20000u))return -1;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32&&c->ip>0xFFFFu)return -1;
    ip=start=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(p66)return B86_UNSUPPORTED;p66=1;continue;}
        if(op==0x67u){if(p67)return B86_UNSUPPORTED;p67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            continue;
        }
        break;
    }
    w=(code32^(unsigned)p66)?4u:2u;
    switch(op){
    case 0x06u:reg=B86_ES;pop=0;break;
    case 0x07u:reg=B86_ES;pop=1;break;
    case 0x0Eu:reg=B86_CS;pop=0;break;
    case 0x16u:reg=B86_SS;pop=0;break;
    case 0x17u:reg=B86_SS;pop=1;break;
    case 0x1Eu:reg=B86_DS;pop=0;break;
    case 0x1Fu:reg=B86_DS;pop=1;break;
    case 0x0Fu:
        if(!b86_pm4_byte(c,&ip,start,&ext))return B86_UNSUPPORTED;
        if(ext==0xA0u||ext==0xA1u){reg=B86_FS;pop=ext==0xA1u;}
        else if(ext==0xA8u||ext==0xA9u){reg=B86_GS;pop=ext==0xA9u;}
        break;
    }
    if(pop>=0){
        if(p67||segpref)return B86_UNSUPPORTED;
        return b86_pm26_segment_op(c,reg,pop,w,code32?ip:(ip&0xFFFFu));
    }
    if(op!=0xCDu && op!=0xCCu && op!=0xCEu && op!=0xCFu)return -1;
    /* Preserve the existing, tested PM12 32-bit INT/IRETD path. PM26
     * handles mixed-width overrides and the missing 16-bit gate path. */
    /* B86_PM31_V1: PM26 now owns every PM INT/INTO/INT3/IRET form. PM12
     * required a 32-bit gate target and 32-bit stack; DOS/4GW's INT 31h
     * enters a 16-bit kernel through a 386 gate from 32-bit code on a
     * 16-bit (B=0) stack, which this path already frames correctly. */
    if(p67||segpref)return B86_UNSUPPORTED;
    if(op==0xCFu)return b86_pm26_iret(c,w);
    if(op==0xCDu){
        if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        vec=b;
    }else if(op==0xCCu)vec=3u;
    else{
        vec=4u;
        if(!(c->flags&B86_OF)){
            c->ip=code32?ip:(ip&0xFFFFu);
            ++c->icount;
            return B86_OK;
        }
    }
    return b86_pm26_interrupt(c,vec,code32?ip:(ip&0xFFFFu));
}

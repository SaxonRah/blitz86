/* B86_PM_UNIFIED_V9: cold 80386 PM F6/F7 unary integer group and
 * checked CR0.PE=1 -> 0 real-mode far-jump handoff.
 * /0 TEST delegated to PM24. /1 reserved (delegated); /2 NOT, /3 NEG,
 * /4 MUL, /5 IMUL, /6 DIV, /7 IDIV support 8/16/32-bit operands.
 * Bad divide/overflow is fail-closed pending full #DE exception delivery.
 * No new B86Cpu fields or persistent RAM, no JIT changes.
 */
#ifndef B86_PM29_FLASH
#define B86_PM29_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM29_FLASH uint32_t b86_pm29_read(const B86Cpu *c,uint32_t lin,unsigned n)
{
    uint32_t v=0u;
    for(unsigned i=0;i<n;i++)v|=(uint32_t)*b86_host(c,lin+i)<<(8u*i);
    return v;
}
static B86_PM29_FLASH void b86_pm29_write(B86Cpu *c,uint32_t lin,unsigned n,uint32_t v)
{
    for(unsigned i=0;i<n;i++)*b86_host(c,lin+i)=(uint8_t)(v>>(8u*i));
    smc(c,lin,n);
}
static B86_PM29_FLASH unsigned b86_pm29_even(uint32_t x)
{
    x&=255u;x^=x>>4;x^=x>>2;x^=x>>1;
    return (x&1u)?0u:B86_PF;
}
static B86_PM29_FLASH int b86_pm29_step(B86Cpu *c)
{
    uint32_t start,ip,lin=0,operand,mask,sign,result=0,low=0,high=0,flags=0;
    uint64_t wide=0;
    int pref66=0,pref67=0,segpref=0,code32,op32,ad32;
    unsigned seg=B86_DS,w,sub,bits,need;
    uint8_t op;
    B86PM6EA ea;
    B86PM2Fault fault;
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
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            seg=op==0x26u?B86_ES:op==0x2Eu?B86_CS:op==0x36u?B86_SS:
                op==0x3Eu?B86_DS:op==0x64u?B86_FS:B86_GS;
            continue;
        }
        break;
    }
    if(op!=0xF6u&&op!=0xF7u)return -1;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    op32=code32^pref66;ad32=code32^pref67;
    w=op==0xF6u?1u:(op32?4u:2u);
    mask=w==1u?0xFFu:w==2u?0xFFFFu:UINT32_MAX;
    bits=8u*w;sign=1u<<(bits-1u);
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    sub=ea.reg;
    if(sub<2u)return -1; /* PM24 performs TEST /0, /1 is reserved. */
    if(segpref&&ea.is_reg)return B86_UNSUPPORTED;
    if(segpref)ea.seg=seg;
    need=(sub==2u||sub==3u)?(B86_PM1_READ|B86_PM1_WRITE):B86_PM1_READ;
    if(ea.is_reg)operand=b86_pm6_reg(c,ea.rm,w);
    else{
        if(b86_pm2_check_access(c,ea.seg,ea.off,w,need,&lin,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        operand=b86_pm29_read(c,lin,w);
    }
    operand&=mask;
    if(sub==2u)result=(~operand)&mask;
    else if(sub==3u){
        result=(0u-operand)&mask;
        flags=(operand?B86_CF:0u) | (operand==sign?B86_OF:0u) |
              (((operand^result)&0x10u)?B86_AF:0u) |
              (result==0u?B86_ZF:0u) | (result&sign?B86_SF:0u) |
              b86_pm29_even(result);
    }else if(sub==4u||sub==5u){
        int64_t signed_product=0;
        uint32_t ax=b86_pm6_reg(c,B86_AX,w);
        if(sub==4u){wide=(uint64_t)ax*(uint64_t)operand;}
        else{
            int64_t a=w==1u?(int8_t)ax:w==2u?(int16_t)ax:(int32_t)ax;
            int64_t b=w==1u?(int8_t)operand:w==2u?(int16_t)operand:(int32_t)operand;
            signed_product=a*b;wide=(uint64_t)signed_product;
        }
        low=(uint32_t)wide&mask;high=(uint32_t)(wide>>bits)&mask;
        if(sub==4u)flags=(high?B86_CF|B86_OF:0u);
        else{
            int64_t se=w==1u?(int8_t)low:w==2u?(int16_t)low:(int32_t)low;
            flags=(se!=signed_product)?(B86_CF|B86_OF):0u;
        }
    }else{
        /* Divide with a wide host accumulator. Check the entire result
         * before changing guest state. Zero/overflow awaits #DE delivery. */
        uint64_t dividend,quot,rem;
        if(operand==0u)return B86_UNSUPPORTED;
        if(w==1u)dividend=c->r[B86_AX]&0xFFFFu;
        else if(w==2u)dividend=((uint64_t)(c->r[B86_DX]&0xFFFFu)<<16u)|
                                  (c->r[B86_AX]&0xFFFFu);
        else dividend=((uint64_t)c->r[B86_DX]<<32u)|c->r[B86_AX];
        if(sub==6u){
            quot=dividend/(uint64_t)operand;rem=dividend%(uint64_t)operand;
            if(quot>mask)return B86_UNSUPPORTED;
        }else{
            int64_t d=w==1u?(int8_t)operand:w==2u?(int16_t)operand:(int32_t)operand;
            int64_t n=w==1u?(int16_t)(uint16_t)dividend:
                w==2u?(int32_t)(uint32_t)dividend:
                (int64_t)(int32_t)c->r[B86_DX]*4294967296LL+(int64_t)c->r[B86_AX];
            /* INT64_MIN / -1 would trap in the host before guest #DE. */
            if(n==INT64_MIN && d==-1)return B86_UNSUPPORTED;
            int64_t q=n/d,r=n%d;
            int64_t min=-(1LL<<(bits-1u)),max=(1LL<<(bits-1u))-1LL;
            if(q<min||q>max)return B86_UNSUPPORTED;
            quot=(uint64_t)q;rem=(uint64_t)r;
        }
        low=(uint32_t)quot&mask;high=(uint32_t)rem&mask;
    }
    /* Commit: reads/descriptor checks/divide checks are complete. */
    if(sub==2u||sub==3u){
        if(ea.is_reg)b86_pm6_putreg(c,ea.rm,w,result);
        else b86_pm29_write(c,lin,w,result);
        if(sub==3u)
            c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF|B86_AF|B86_ZF|B86_SF|B86_PF))|flags|2u;
    }else if(w==1u){
        c->r[B86_AX]=(c->r[B86_AX]&~0xFFFFu)|((high&0xFFu)<<8u)|(low&0xFFu);
    }else{
        b86_pm6_putreg(c,B86_AX,w,low);
        b86_pm6_putreg(c,B86_DX,w,high);
    }
    if(sub==4u||sub==5u)
        c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF))|flags|2u;
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;return B86_OK;
}

/* PE was explicitly cleared by MOV CR0,reg. On a real 386 the old CS
 * descriptor cache continues to select the instruction fetch until a far
 * control transfer. The ordinary real-mode decoder always uses CS<<4,
 * so finish exactly the far JMP using the *old* cached CS.base first.
 * All other instructions fail closed until more cache semantics exist.
 */
static B86_PM29_FLASH int b86_pm29_finish_pe_exit(B86Cpu *c)
{
    uint32_t start,ip,off=0u;
    uint16_t sel=0;
    unsigned code32,operand32,w;
    uint8_t op,b;
    if(!c||!c->mem||c->model!=B86_CPU_386||(c->msw&1u)||
       c->pm386.seg[B86_CS].reserved!=B86_PM29_EXIT_MARKER||
       !c->pm386.seg[B86_CS].valid||c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32&&c->ip>0xFFFFu)return B86_UNSUPPORTED;
    start=ip=c->ip;operand32=code32;
    if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
    if(op==0x66u){operand32=!operand32;
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
    }
    if(op!=0xEAu)return B86_UNSUPPORTED; /* no accidental old-CS fetch */
    w=operand32?4u:2u;
    for(unsigned i=0;i<w;i++){
        if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        off|=(uint32_t)b<<(i*8u);
    }
    if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
    sel=b;
    if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
    sel|=(uint16_t)b<<8u;
    /* The post-jump real-mode fetch is subject to the emulator's address
     * mask; refuse 32-bit EIP outside its current real-mode capability. */
    if(off>0xFFFFu ||
       (uint64_t)((uint32_t)sel<<4u)+(uint64_t)off>=B86_RAM_BYTES)
        return B86_UNSUPPORTED;
    b86_set_seg(c,B86_CS,sel);
    c->ip=off;
    c->pm386.seg[B86_CS].reserved=0u;
    ++c->icount;
    return B86_OK;
}

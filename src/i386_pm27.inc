/* B86_PM_UNIFIED_V7: descriptor queries and 80386 bit-operation families.
 * Protected CPL0 16/32-bit CS and operand/address overrides:
 * 0F 02/03 LAR/LSL; 0F A3/AB/B3/BB BT/BTS/BTR/BTC;
 * 0F BA /4-/7 immediate bit ops; 0F BC/BD BSF/BSR;
 * 0F A4/A5/AC/AD SHLD/SHRD.
 * Operands and descriptors are validated before guest-state updates.
 * Returns -1 for unrelated opcodes, preserving PM26->PM25->PM24 fallback.
 * No paging, VM86, TSS/task switches or ring transitions.
 */
#ifndef B86_PM27_FLASH
#define B86_PM27_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM27_FLASH unsigned b86_pm27_parity(uint32_t v)
{
    v&=0xFFu;v^=v>>4;v^=v>>2;v^=v>>1;
    return (v&1u)?0u:4u;
}
static B86_PM27_FLASH int b86_pm27_descriptor(B86Cpu *c,uint16_t sel,
                                               unsigned ext,unsigned w,
                                               uint32_t *result)
{
    B86PM0Desc d;
    int rc;
    unsigned typ;
    if(!(sel&0xFFF8u))return 0;
    rc=b86_pm1_descriptor(c,sel,&d);
    /* Descriptor not found is a ZF=0 probe, not an unsupported opcode.
       Physical memory errors fail closed rather than masking a bad address. */
    if(rc==B86_PM1_SELECTOR||rc==B86_PM1_TABLE_LIMIT||rc==B86_PM1_NO_LDT)
        return 0;
    if(rc!=B86_PM1_OK && rc!=B86_PM1_NOT_PRESENT)return -1;
    typ=d.access&15u;
    if(!d.code_data){
        /* LDT and TSS descriptors have sensible limit and rights outputs.
           Other system/gate types deliberately remain unsupported. */
        if(typ!=2u && typ!=1u && typ!=3u && typ!=9u && typ!=11u)
            return 0; /* invalid descriptor type: ZF=0, destination unchanged */
    }
    /* Conforming code is exempt from the ordinary CPL/RPL vs DPL test. */
    if(!(d.code_data && d.executable && d.conforming_expand) &&
       (d.dpl < (c->seg[B86_CS]&3u) || d.dpl < (sel&3u)))return 0;
    if(ext==2u)
        *result=((uint32_t)d.access<<8u) |
                ((w==4u)?((uint32_t)(d.flags&15u)<<20u):0u);
    else *result=d.limit;
    return 1;
}
static B86_PM27_FLASH uint32_t b86_pm27_load(B86Cpu*c,uint32_t lin,unsigned n)
{
    uint32_t v=0;
    for(unsigned i=0;i<n;i++)v|=(uint32_t)(*b86_host(c,lin+i))<<(8u*i);
    return v;
}
static B86_PM27_FLASH void b86_pm27_store(B86Cpu*c,uint32_t lin,unsigned n,uint32_t v)
{
    for(unsigned i=0;i<n;i++)*b86_host(c,lin+i)=(uint8_t)(v>>(8u*i));
    smc(c,lin,n);
}
static B86_PM27_FLASH int b86_pm27_step(B86Cpu*c)
{
    B86PM6EA ea;
    B86PM2Fault fault;
    uint32_t start,ip,linear=0u,selector=0u,result=0u,operand=0u,source=0u;
    uint32_t bit=0u,value=0u,cf=0u,oldflags;
    unsigned w=0u,code32=0u,op32=0u,ad32=0u,seg=B86_DS;
    unsigned width_bits=0u,kind=0u,count=0u,sub=0u;
    int pref66=0,pref67=0,segpref=0;
    uint8_t op,ext=0u,b;
    if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
       c->shutdown386||c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !c->pm386.seg[B86_CS].valid||!(c->pm386.seg[B86_CS].access&8u)||
       (c->seg[B86_CS]&3u)||c->ip>UINT32_MAX-15u||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&(0x20000u|0x4000u)))return -1;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32&&c->ip>0xFFFFu)return -1;
    start=ip=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
        if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            seg=op==0x26u?B86_ES:op==0x2Eu?B86_CS:
                op==0x36u?B86_SS:op==0x3Eu?B86_DS:op==0x64u?B86_FS:B86_GS;
            continue;
        }
        break;
    }
    if(op!=0x0Fu)return -1;
    if(!b86_pm4_byte(c,&ip,start,&ext))return B86_UNSUPPORTED;
    /* Decode whole 0F opcode families in one module. */
    if(ext==0x02u||ext==0x03u)kind=1u;
    else if(ext==0xA3u||ext==0xABu||ext==0xB3u||ext==0xBBu||ext==0xBAu)kind=2u;
    else if(ext==0xBCu||ext==0xBDu)kind=3u;
    else if(ext==0xA4u||ext==0xA5u||ext==0xACu||ext==0xADu)kind=4u;
    else return -1;
    op32=code32^(unsigned)pref66;
    ad32=code32^(unsigned)pref67;
    w=op32?4u:2u;
    width_bits=w*8u;
    if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
    if(segpref&&!ea.is_reg)ea.seg=seg;
    if(kind==1u){
        if(ea.is_reg)selector=b86_pm6_reg(c,ea.rm,2u);
        else{
            if(b86_pm2_check_access(c,ea.seg,ea.off,2u,B86_PM1_READ,
                                    &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            selector=b86_pm27_load(c,linear,2u);
        }
        int drc=b86_pm27_descriptor(c,(uint16_t)selector,ext,w,&result);
        if(drc<0)return B86_UNSUPPORTED;
        if(drc){
            b86_pm6_putreg(c,ea.reg,w,result);
            c->flags|=B86_ZF;
        }else c->flags&=~(uint32_t)B86_ZF;
    }else if(kind==3u){
        if(ea.is_reg)operand=b86_pm6_reg(c,ea.rm,w);
        else{
            if(b86_pm2_check_access(c,ea.seg,ea.off,w,B86_PM1_READ,
                                    &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            operand=b86_pm27_load(c,linear,w);
        }
        if(!operand)c->flags|=B86_ZF;
        else{
            c->flags&=~(uint32_t)B86_ZF;
            if(ext==0xBCu){
                for(bit=0u;bit<width_bits;bit++)if(operand&(1u<<bit))break;
            }else{
                for(bit=width_bits;bit>0u;bit--)if(operand&(1u<<(bit-1u)))break;
                bit--;
            }
            b86_pm6_putreg(c,ea.reg,w,bit);
        }
    }else if(kind==2u){
        /* BT/BTS/BTR/BTC: register index can select a *different* memory
           word, including negative offsets. Immediate form selects bits
           within the sized word. */
        int isimm=(ext==0xBAu);
        int64_t signed_bit=0;
        if(isimm){
            sub=ea.reg;
            if(sub<4u||sub>7u)return B86_UNSUPPORTED;
            if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
            bit=(unsigned)b & (width_bits-1u);
        }else{
            sub=ext==0xA3u?4u:ext==0xABu?5u:ext==0xB3u?6u:7u;
            value=b86_pm6_reg(c,ea.reg,w);
            signed_bit=w==2u?(int16_t)(uint16_t)value:(int32_t)value;
            bit=value&(width_bits-1u);
        }
        if(ea.is_reg)operand=b86_pm6_reg(c,ea.rm,w);
        else{
            if(!isimm){
                int64_t q=signed_bit/(int64_t)width_bits;
                if(signed_bit<0 && signed_bit%(int64_t)width_bits)q--;
                /* Effective-address-size wrap happens before segment check. */
                ea.off=(uint32_t)((int64_t)ea.off+q*(int64_t)w);
                if(!ad32)ea.off&=0xFFFFu;
            }
            if(b86_pm2_check_access(c,ea.seg,ea.off,w,
                         sub==4u?B86_PM1_READ:(B86_PM1_READ|B86_PM1_WRITE),
                         &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            operand=b86_pm27_load(c,linear,w);
        }
        cf=(operand>>bit)&1u;
        c->flags=(c->flags&~(uint32_t)B86_CF)|cf;
        if(sub!=4u){
            uint32_t mask=1u<<bit;
            result=sub==5u?operand|mask:sub==6u?operand&~mask:operand^mask;
            if(ea.is_reg)b86_pm6_putreg(c,ea.rm,w,result);
            else b86_pm27_store(c,linear,w,result);
        }
    }else{
        /* SHLD / SHRD: 386 double-precision shifts, count 1..operand bits.
           Counts greater than width on 16-bit operands are undefined on
           80386, so remain fail-closed. Count 0 preserves everything. */
        uint32_t mask=w==2u?0xFFFFu:UINT32_MAX,sign=1u<<(width_bits-1u);
        int left=ext==0xA4u||ext==0xA5u;
        source=b86_pm6_reg(c,ea.reg,w);
        if(ext==0xA4u||ext==0xACu){
            if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
            count=b&31u;
        }else count=c->r[B86_CX]&31u;
        if(count>width_bits)return B86_UNSUPPORTED;
        if(!count){c->ip=code32?ip:(ip&0xFFFFu);++c->icount;return B86_OK;}
        if(ea.is_reg)operand=b86_pm6_reg(c,ea.rm,w);
        else{
            if(b86_pm2_check_access(c,ea.seg,ea.off,w,
                          B86_PM1_READ|B86_PM1_WRITE,&linear,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            operand=b86_pm27_load(c,linear,w);
        }
        operand&=mask;source&=mask;
        if(left){
            result=((operand<<count)|
                   (count==width_bits?source:(source>>(width_bits-count))))&mask;
            cf=(operand>>(width_bits-count))&1u;
        }else{
            result=((operand>>count)|
                   (count==width_bits?source:(source<<(width_bits-count))))&mask;
            cf=(operand>>(count-1u))&1u;
        }
        oldflags=c->flags;
        c->flags=(oldflags&~(uint32_t)(B86_CF|B86_PF|B86_ZF|B86_SF))|
                 cf|b86_pm27_parity(result)|
                 ((result==0u)?B86_ZF:0u)|((result&sign)?B86_SF:0u);
        if(count==1u){
            c->flags&=~(uint32_t)B86_OF;
            if(left?((!!(result&sign))!=cf):((!!(operand&sign))!=(!!(result&sign))))
                c->flags|=B86_OF;
        }
        if(ea.is_reg)b86_pm6_putreg(c,ea.rm,w,result);
        else b86_pm27_store(c,linear,w,result);
    }
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;
    return B86_OK;
}

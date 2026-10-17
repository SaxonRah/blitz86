/* BB_386_STAGE5C5H_V1: protected-mode ModR/M 16/32 EA, SIB, MOV + ALU.
 * 88..8B, C6/C7 /0, 00..03 ADD, 28..2B SUB, 38..3B CMP.
 * CPL0 only; all instruction/data offsets validated before state changes.
 */
#include "b86_pm6.h"
#ifndef B86_PM6_FLASH
#define B86_PM6_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

typedef struct B86PM6EA {
    uint32_t off;
    unsigned seg;
    unsigned reg;
    unsigned rm;
    unsigned is_reg;
} B86PM6EA;

static B86_PM6_FLASH uint32_t b86_pm6_reg(const B86Cpu *c, unsigned reg, unsigned width)
{
    if (width==1u) {
        uint32_t x=c->r[reg&3u];
        return reg<4u ? (x&255u) : ((x>>8)&255u);
    }
    return width==2u ? (c->r[reg]&0xFFFFu) : c->r[reg];
}
static B86_PM6_FLASH void b86_pm6_putreg(B86Cpu *c, unsigned reg,
                                           unsigned width,uint32_t value)
{
    if (width==1u) {
        if (reg<4u) c->r[reg]=(c->r[reg]&~255u)|(value&255u);
        else c->r[reg-4u]=(c->r[reg-4u]&~0xFF00u)|((value&255u)<<8);
    } else if (width==2u) c->r[reg]=(c->r[reg]&0xFFFF0000u)|(value&0xFFFFu);
    else c->r[reg]=value;
}
static B86_PM6_FLASH int b86_pm6_imm(const B86Cpu *c,uint32_t *ip,
                     uint32_t start,unsigned width,uint32_t *value)
{
    uint32_t x=0;
    for (unsigned i=0;i<width;i++) {
        uint8_t b;
        if (!b86_pm4_byte(c,ip,start,&b)) return 0;
        x|=(uint32_t)b<<(i*8u);
    }
    *value=x;return 1;
}
/* Decode addressing without writing any guest state. Every SIB/disp byte
 * must be preflighted against CS before any data access is attempted.
 */
static B86_PM6_FLASH int b86_pm6_ea(const B86Cpu *c,uint32_t *ip,
           uint32_t start,int address32,B86PM6EA *ea)
{
    uint8_t modrm, sib=0;
    unsigned mod,rm,reg;
    uint32_t off=0,disp=0;
    unsigned disp_size=0;
    int stack=0;
    if (!b86_pm4_byte(c,ip,start,&modrm))return 0;
    mod=modrm>>6;reg=(modrm>>3)&7u;rm=modrm&7u;
    ea->reg=reg;ea->rm=rm;ea->is_reg=(mod==3u);
    ea->seg=B86_DS;ea->off=0;
    if(mod==3u)return 1;
    if(address32) {
        uint32_t base=0,index=0;
        if(rm==4u){
            unsigned scale,idx,bas;
            if(!b86_pm4_byte(c,ip,start,&sib))return 0;
            scale=sib>>6;idx=(sib>>3)&7u;bas=sib&7u;
            if(idx!=4u)index=c->r[idx]<<scale;
            if(bas==5u && mod==0u)disp_size=4u;
            else {base=c->r[bas];if(bas==B86_BP || bas==B86_SP)stack=1;}
        } else if(rm==5u && mod==0u)disp_size=4u;
        else {base=c->r[rm];if(rm==B86_BP || rm==B86_SP)stack=1;}
        if(mod==1u)disp_size=1u;
        if(mod==2u)disp_size=4u;
        if(disp_size && !b86_pm6_imm(c,ip,start,disp_size,&disp))return 0;
        if(disp_size==1u)disp=(uint32_t)(int32_t)(int8_t)disp;
        off=base+index+disp;
    } else {
        switch(rm){
        case 0:off=(uint16_t)c->r[B86_BX]+(uint16_t)c->r[B86_SI];break;
        case 1:off=(uint16_t)c->r[B86_BX]+(uint16_t)c->r[B86_DI];break;
        case 2:off=(uint16_t)c->r[B86_BP]+(uint16_t)c->r[B86_SI];stack=1;break;
        case 3:off=(uint16_t)c->r[B86_BP]+(uint16_t)c->r[B86_DI];stack=1;break;
        case 4:off=(uint16_t)c->r[B86_SI];break;
        case 5:off=(uint16_t)c->r[B86_DI];break;
        case 6:
            if(mod==0u)disp_size=2u;
            else {off=(uint16_t)c->r[B86_BP];stack=1;}
            break;
        default:off=(uint16_t)c->r[B86_BX];break;
        }
        if(mod==1u)disp_size=1u;
        if(mod==2u)disp_size=2u;
        if(disp_size && !b86_pm6_imm(c,ip,start,disp_size,&disp))return 0;
        if(disp_size==1u)disp=(uint32_t)(int32_t)(int8_t)disp;
        off=(off+disp)&0xFFFFu;
    }
    ea->seg=stack?B86_SS:B86_DS;
    ea->off=off;
    return 1;
}

B86_PM6_FLASH int b86_pm6_step(B86Cpu *c)
{
    B86PM6EA ea;
    B86PM2Fault fault;
    uint32_t start,ip,imm=0,linear=0,src=0,dst=0,result=0,newflags=0;
    unsigned width,kind=0,seg=B86_DS,reg=0,opreg=0;
    int pref66=0,pref67=0,segpref=0,op32,ad32,code32,has_mem;
    uint8_t op;
    if (!c || c->model!=B86_CPU_386 || !c->mem || !(c->msw&1u) ||
        c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm5_step(c);
    if (!c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
        (c->seg[B86_CS]&3u)!=0u || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if (!code32 && c->ip>0xFFFFu)return B86_UNSUPPORTED;
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
    if(op>=0x88u && op<=0x8Bu)kind=1u;/* MOV r/m <-> reg */
    else if(op==0xC6u||op==0xC7u)kind=2u;/* MOV r/m, imm */
    else if(op<=0x03u)kind=3u;/* ADD */
    else if(op>=0x28u&&op<=0x2Bu)kind=4u;/* SUB */
    else if(op>=0x38u&&op<=0x3Bu)kind=5u;/* CMP */
    if(!kind)return b86_pm5_step(c);
    if((kind>=3u) && (c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE))
        return B86_UNSUPPORTED;
    op32=code32^pref66;ad32=code32^pref67;
    width=(kind==2u)?(op==0xC6u?1u:(op32?4u:2u)):
                       ((op&1u)?(op32?4u:2u):1u);
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    if(kind==2u && ea.reg!=0u)return B86_UNSUPPORTED;
    if(kind==2u && !b86_pm6_imm(c,&ip,start,width,&imm))return B86_UNSUPPORTED;
    if(!code32)ip&=0xFFFFu;
    if(ea.is_reg && segpref)return B86_UNSUPPORTED;/* meaningless override */
    has_mem=!ea.is_reg;
    if(has_mem){
        unsigned access;
        if(segpref)ea.seg=seg;
        /* All non-CMP destination memory operations request WRITE, and
         * source memory operations request READ. Validate full operand. */
        access=(kind==2u || (kind==1u && !(op&2u)) ||
               (kind>=3u && !(op&2u) && kind!=5u))?B86_PM1_WRITE:B86_PM1_READ;
        if(b86_pm2_check_access(c,ea.seg,ea.off,width,access,
                                &linear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
    }
    reg=ea.reg;opreg=ea.rm;
    if(kind==2u){
        if(ea.is_reg)b86_pm6_putreg(c,opreg,width,imm);
        else {
            for(unsigned i=0;i<width;i++)*b86_host(c,linear+i)=(uint8_t)(imm>>(8u*i));
            smc(c,linear,width);
        }
    } else {
        /* r/m <-> reg direction bit 1: 0 => destination r/m, 1 => reg. */
        int reg_dest=(op&2u)!=0u;
        uint32_t rmv=0,regv=b86_pm6_reg(c,reg,width);
        if(ea.is_reg)rmv=b86_pm6_reg(c,opreg,width);
        else for(unsigned i=0;i<width;i++)rmv|=(uint32_t)(*b86_host(c,linear+i))<<(8u*i);
        dst=reg_dest?regv:rmv;
        src=reg_dest?rmv:regv;
        if(kind==1u)result=src;
        else newflags=b86_pm5_arith_flags(c->flags,dst,src,width,kind!=3u,&result);
        if(kind!=5u){
            if(reg_dest)b86_pm6_putreg(c,reg,width,result);
            else if(ea.is_reg)b86_pm6_putreg(c,opreg,width,result);
            else {
                for(unsigned i=0;i<width;i++)*b86_host(c,linear+i)=(uint8_t)(result>>(8u*i));
                smc(c,linear,width);
            }
        }
        if(kind>=3u)c->flags=newflags;
    }
    c->ip=ip;++c->icount;
    return B86_OK;
}

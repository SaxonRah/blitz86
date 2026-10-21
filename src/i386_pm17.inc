/* PM17: Protected-mode 69/6B IMUL immediate and F6/F7 /4..7 integer
 * multiply/divide; F6/F7 /6..7 divisor=0 goes to PM15 #DE.
 * 8/16/32-bit operations, 16/32 address modes, transactional faults.
 * No guest-CPU ABI or JIT changes. Software 64-bit division is permitted.
 */
#include "b86_pm17.h"
#ifndef B86_PM17_FLASH
#define B86_PM17_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM17_FLASH uint32_t b86_pm17_mask(unsigned n)
{ return n==4u?UINT32_MAX:n==2u?0xFFFFu:0xFFu; }
static B86_PM17_FLASH int64_t b86_pm17_sext(uint32_t v,unsigned n)
{ return n==1u?(int64_t)(int8_t)v:n==2u?(int64_t)(int16_t)v:(int64_t)(int32_t)v; }
static B86_PM17_FLASH uint32_t b86_pm17_getimm(B86Cpu*c,uint32_t*ip,uint32_t start,unsigned n,int*ok)
{
    uint32_t v=0; uint8_t b;
    for(unsigned i=0;i<n;i++) {
        if(!b86_pm4_byte(c,ip,start,&b)){*ok=0;return 0;}
        v|=(uint32_t)b<<(i*8u);
    }
    *ok=1;return v;
}
B86_PM17_FLASH int b86_pm17_step(B86Cpu*c)
{
    uint32_t ip,start,lin=0,src=0,imm=0,mask,result=0;
    uint8_t op;B86PM6EA ea;B86PM2Fault fault;
    unsigned seg=B86_DS,n,readn,group,bits;
    int p66=0,p67=0,segpref=0,op32,ad32,ok,ismem;
    if(c&&c->model>=B86_CPU_386&&c->shutdown386)return B86_SHUTDOWN;
    if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
       !c->pm386.seg[B86_CS].valid ||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !(c->pm386.seg[B86_CS].access&8u)||(c->seg[B86_CS]&3u)||
       !(c->pm386.seg[B86_CS].flags&4u)||c->ip>UINT32_MAX-15u||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&0x20000u))return b86_pm16_step(c);
    ip=start=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(p66)return B86_UNSUPPORTED;p66=1;continue;}
        if(op==0x67u){if(p67)return B86_UNSUPPORTED;p67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segpref)return B86_UNSUPPORTED;
            segpref=1;
            switch(op){
            case 0x26u:seg=B86_ES;break;case 0x2Eu:seg=B86_CS;break;
            case 0x36u:seg=B86_SS;break;case 0x3Eu:seg=B86_DS;break;
            case 0x64u:seg=B86_FS;break;default:seg=B86_GS;break;
            }continue;
        }break;
    }
    if(op!=0xF6u && op!=0xF7u && op!=0x69u && op!=0x6Bu)
        return b86_pm16_step(c);
    op32=1^p66;ad32=1^p67;
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    ismem=!ea.is_reg;
    if(!ismem&&segpref)return B86_UNSUPPORTED;
    if(op==0xF6u||op==0xF7u){
        group=ea.reg;
        if(group<4u)return b86_pm16_step(c);
    }else group=0u;
    n=(op==0xF6u)?1u:(op32?4u:2u);readn=n;
    bits=n*8u;mask=b86_pm17_mask(n);
    if(ismem){
        if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,readn,
                               B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        src=b86_pm12_load(c,lin,readn);
    }else src=b86_pm6_reg(c,ea.rm,readn);
    if(op==0x69u||op==0x6Bu){
        imm=b86_pm17_getimm(c,&ip,start,op==0x69u?n:1u,&ok);
        if(!ok)return B86_UNSUPPORTED;
        int64_t mul=b86_pm17_sext(imm,op==0x69u?n:1u);
        int64_t product=b86_pm17_sext(src,n)*mul;
        result=(uint32_t)product&mask;
        int ov=product!=b86_pm17_sext(result,n);
        b86_pm6_putreg(c,ea.reg,n,result);
        c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF))|
                (ov?(B86_CF|B86_OF):0u);
    }else if(group==4u||group==5u){
        /* MUL/IMUL: full-width product in AH:AL / DX:AX / EDX:EAX. */
        uint32_t ax=b86_pm6_reg(c,B86_AX,n);
        uint64_t product;
        if(group==4u)product=(uint64_t)ax*(uint64_t)src;
        else product=(uint64_t)(b86_pm17_sext(ax,n)*b86_pm17_sext(src,n));
        if(n==1u){
            result=(uint32_t)product&0xFFFFu;
            c->r[B86_AX]=(c->r[B86_AX]&0xFFFF0000u)|result;
        }else if(n==2u){
            result=(uint32_t)product&0xFFFFu;
            b86_pm6_putreg(c,B86_AX,2u,result);
            b86_pm6_putreg(c,B86_DX,2u,(uint32_t)(product>>16u));
        }else{
            result=(uint32_t)product;
            c->r[B86_AX]=result;
            c->r[B86_DX]=(uint32_t)(product>>32u);
        }
        int overflow=group==4u?
            (product>>(bits*2u==16u?8u:bits))!=0u:
            ((int64_t)product!=b86_pm17_sext((uint32_t)product&mask,n));
        c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF))|
                 (overflow?(B86_CF|B86_OF):0u);
    }else{
        /* DIV/IDIV: no architectural register writes until quotient fits. */
        if(!src)return b86_pm16_step(c); /* inherited PM15 #DE */
        uint32_t qlo,rem;
        int overflow=0;
        if(group==6u){
            uint64_t dividend;
            if(n==1u)dividend=c->r[B86_AX]&0xFFFFu;
            else if(n==2u)dividend=((uint64_t)(c->r[B86_DX]&0xFFFFu)<<16u)|
                                           (c->r[B86_AX]&0xFFFFu);
            else dividend=((uint64_t)c->r[B86_DX]<<32u)|c->r[B86_AX];
            uint64_t q=dividend/src, r=dividend%src;
            overflow=q>mask;qlo=(uint32_t)q;rem=(uint32_t)r;
        }else{
            int64_t dividend,divisor=b86_pm17_sext(src,n);
            if(n==1u)dividend=(int16_t)(c->r[B86_AX]&0xFFFFu);
            else if(n==2u)
                dividend=(int64_t)(int16_t)c->r[B86_DX]*65536LL+
                         (c->r[B86_AX]&0xFFFFu);
            else dividend=(int64_t)(int32_t)c->r[B86_DX]*4294967296LL+
                          c->r[B86_AX];
            if(dividend==INT64_MIN && divisor==-1LL)overflow=1;
            if(!overflow){
                int64_t q=dividend/divisor,r=dividend%divisor;
                int64_t minq=-(1LL<<(bits-1u)),maxq=(1LL<<(bits-1u))-1LL;
                overflow=q<minq||q>maxq;
                qlo=(uint32_t)q;rem=(uint32_t)r;
            }else{qlo=0;rem=0;}
        }
        if(overflow)return b86_pm13_deliver(c,0u,0u,0u);
        if(n==1u){
            result=(qlo&0xFFu)|((rem&0xFFu)<<8u);
            c->r[B86_AX]=(c->r[B86_AX]&0xFFFF0000u)|result;
        }else if(n==2u){
            b86_pm6_putreg(c,B86_AX,2u,qlo);
            b86_pm6_putreg(c,B86_DX,2u,rem);
        }else{c->r[B86_AX]=qlo;c->r[B86_DX]=rem;}
        /* 386 DIV flags undefined: preserve prior flags. */
    }
    c->ip=ip;++c->icount;return B86_OK;
}

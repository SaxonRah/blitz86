/* Protected CPL0 80386 0F integer family.
 * 0F AF IMUL; B6/B7/BE/BF MOVZX/MOVSX; BC/BD BSF/BSR;
 * A3/AB/B3/BB and BA /4..7 BT/BTS/BTR/BTC;
 * A4/A5/AC/AD SHLD/SHRD immediate/CL.
 * Register and descriptor-checked memory operands, 16/32 operand sizes,
 * 16/32 address modes (SIB included), bounded prefixes, atomic commits.
 * Earlier PM15 handles the remainder; no JIT or CPU-layout changes.
 */
#include "b86_pm16.h"
#ifndef B86_PM16_FLASH
#define B86_PM16_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM16_FLASH uint32_t b86_pm16_mask(unsigned n)
{ return n==4u?UINT32_MAX:0xFFFFu; }
static B86_PM16_FLASH unsigned b86_pm16_parity(uint32_t v)
{ unsigned n=0;v&=0xffu;while(v){n^=v&1u;v>>=1u;}return n==0u; }
static B86_PM16_FLASH void b86_pm16_store(B86Cpu *c,uint32_t at,uint32_t v,unsigned n)
{ for(unsigned i=0;i<n;i++)*b86_host(c,at+i)=(uint8_t)(v>>(8u*i));smc(c,at,n); }
B86_PM16_FLASH int b86_pm16_step(B86Cpu *c)
{
    uint32_t ip,start,lin=0,src=0,dst=0,result=0,idx=0,nextoff;
    uint8_t op,ext,im=0;
    B86PM6EA ea;
    B86PM2Fault fault;
    unsigned n,srcn,seg=B86_DS,bits,bit=0,count=0,action=0;
    int pref66=0,pref67=0,segpref=0,op32,ad32,ismem,change_mem=0;
    if(c&&c->model>=B86_CPU_386&&c->shutdown386)return B86_SHUTDOWN;
    if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
       !c->pm386.seg[B86_CS].valid ||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
       !(c->pm386.seg[B86_CS].access&8u)|| (c->seg[B86_CS]&3u)||
       !(c->pm386.seg[B86_CS].flags&4u)||c->ip>UINT32_MAX-15u||
       c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
       (c->flags&0x20000u))return b86_pm15_step(c);
    ip=start=c->ip;
    for(;;){
      if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
      if(op==0x66u){if(pref66)return B86_UNSUPPORTED;pref66=1;continue;}
      if(op==0x67u){if(pref67)return B86_UNSUPPORTED;pref67=1;continue;}
      if(op==0x26u||op==0x2eu||op==0x36u||op==0x3eu||op==0x64u||op==0x65u){
        if(segpref)return B86_UNSUPPORTED;
        segpref=1;
        switch(op){
        case 0x26u:seg=B86_ES;break;case 0x2eu:seg=B86_CS;break;
        case 0x36u:seg=B86_SS;break;case 0x3eu:seg=B86_DS;break;
        case 0x64u:seg=B86_FS;break;default:seg=B86_GS;break;
        }continue;
      }break;
    }
    if(op!=0x0fu)return b86_pm15_step(c);
    if(!b86_pm4_byte(c,&ip,start,&ext))return B86_UNSUPPORTED;
    if(!(ext==0xafu||ext==0xb6u||ext==0xb7u||ext==0xbeu||ext==0xbfu||
         ext==0xbcu||ext==0xbdu||ext==0xa3u||ext==0xabu||ext==0xb3u||
         ext==0xbbu||ext==0xbau||ext==0xa4u||ext==0xa5u||
         ext==0xacu||ext==0xadu))return b86_pm15_step(c);
    op32=1^pref66;ad32=1^pref67;n=op32?4u:2u;bits=n*8u;
    if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
    ismem=!ea.is_reg;
    if(!ismem&&segpref)return B86_UNSUPPORTED;
    if(ext==0xb6u||ext==0xb7u||ext==0xbeu||ext==0xbfu){
        srcn=(ext==0xb6u||ext==0xbeu)?1u:2u;
        if(ismem){
            if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,srcn,
                       B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            src=b86_pm12_load(c,lin,srcn);
        }else src=b86_pm6_reg(c,ea.rm,srcn);
        if(ext==0xbeu)result=(uint32_t)(int32_t)(int8_t)src;
        else if(ext==0xbfu)result=(uint32_t)(int32_t)(int16_t)src;
        else result=src;
        b86_pm6_putreg(c,ea.reg,n,result);
    }else if(ext==0xafu){
        if(ismem){
            if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,n,
                       B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            src=b86_pm12_load(c,lin,n);
        }else src=b86_pm6_reg(c,ea.rm,n);
        dst=b86_pm6_reg(c,ea.reg,n);
        int64_t a=(n==2u)?(int64_t)(int16_t)dst:(int64_t)(int32_t)dst;
        int64_t b=(n==2u)?(int64_t)(int16_t)src:(int64_t)(int32_t)src;
        int64_t product=a*b;
        result=(uint32_t)product&b86_pm16_mask(n);
        int64_t truncated=(n==2u)?(int64_t)(int16_t)result:(int64_t)(int32_t)result;
        b86_pm6_putreg(c,ea.reg,n,result);
        c->flags=(c->flags&~(uint32_t)(B86_CF|B86_OF))|
                  (product!=truncated?(B86_CF|B86_OF):0u);
    }else if(ext==0xbcu||ext==0xbdu){
        if(ismem){
            if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,n,
                       B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
            src=b86_pm12_load(c,lin,n);
        }else src=b86_pm6_reg(c,ea.rm,n);
        if(src==0u)c->flags|=B86_ZF;
        else{
            unsigned pos=0;
            if(ext==0xbcu){while(!(src&1u)){src>>=1u;++pos;}}
            else{while(src>>1u){src>>=1u;++pos;}}
            b86_pm6_putreg(c,ea.reg,n,pos);
            c->flags&=~(uint32_t)B86_ZF;
        }
    }else if(ext==0xa3u||ext==0xabu||ext==0xb3u||ext==0xbbu||ext==0xbau){
        if(ext==0xbau){
            if(ea.reg<4u||ea.reg>7u)return B86_UNSUPPORTED;
            if(!b86_pm4_byte(c,&ip,start,&im))return B86_UNSUPPORTED;
            action=ea.reg-4u;idx=im;
        }else{
            action=(ext==0xa3u)?0u:(ext==0xabu)?1u:(ext==0xb3u)?2u:3u;
            idx=b86_pm6_reg(c,ea.reg,n);
        }
        nextoff=ea.off;
        if(ismem && ext!=0xbau){
            int64_t v=n==2u?(int64_t)(int16_t)idx:(int64_t)(int32_t)idx;
            int64_t cells=v/(int64_t)bits;
            if(v<0 && v%(int64_t)bits) --cells;
            int64_t off=(int64_t)ea.off+cells*(int64_t)n;
            if(off<0||off>UINT32_MAX)return B86_UNSUPPORTED;
            nextoff=(uint32_t)off;
        }
        bit=idx&(bits-1u);
        if(ismem){
            if(b86_pm2_check_access(c,segpref?seg:ea.seg,nextoff,n,
                action?B86_PM1_WRITE:B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            dst=b86_pm12_load(c,lin,n);
        }else dst=b86_pm6_reg(c,ea.rm,n);
        result=dst;
        if(action==1u)result|=1u<<bit;
        if(action==2u)result&=~(1u<<bit);
        if(action==3u)result^=1u<<bit;
        if(action){
            if(ismem)change_mem=1;
            else b86_pm6_putreg(c,ea.rm,n,result);
        }
        c->flags=(c->flags&~(uint32_t)B86_CF)|((dst>>bit)&1u);
        if(change_mem)b86_pm16_store(c,lin,result,n);
    }else{
        /* SHLD / SHRD: source register from ModR/M.reg, destination r/m. */
        if(ext==0xa4u||ext==0xacu){
            if(!b86_pm4_byte(c,&ip,start,&im))return B86_UNSUPPORTED;
            count=im&31u;
        }else count=c->r[B86_CX]&31u;
        if(count>bits)return B86_UNSUPPORTED;/* 386 > operand width: undefined */
        if(ismem){
            if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,n,
                     count?B86_PM1_WRITE:B86_PM1_READ,&lin,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            dst=b86_pm12_load(c,lin,n);
        }else dst=b86_pm6_reg(c,ea.rm,n);
        src=b86_pm6_reg(c,ea.reg,n);
        if(count){
            uint32_t mask=b86_pm16_mask(n),sign=1u<<(bits-1u),cf;
            if(ext==0xa4u||ext==0xa5u){
                result=((uint32_t)((uint64_t)dst<<count)|
                       (src>>(bits-count)))&mask;
                cf=(dst>>(bits-count))&1u;
            }else{
                result=((dst>>count)|
                       (uint32_t)((uint64_t)src<<(bits-count)))&mask;
                cf=(dst>>(count-1u))&1u;
            }
            uint32_t f=c->flags&~(uint32_t)(B86_CF|B86_OF|B86_ZF|B86_SF|B86_PF);
            if(cf)f|=B86_CF;
            if(!result)f|=B86_ZF;
            if(result&sign)f|=B86_SF;
            if(b86_pm16_parity(result))f|=B86_PF;
            if(count==1u && ((dst^result)&sign))f|=B86_OF;
            else if(count!=1u)f|=c->flags&B86_OF;/* undefined: preserve */
            c->flags=f;
            if(ismem)b86_pm16_store(c,lin,result,n);
            else b86_pm6_putreg(c,ea.rm,n,result);
        }
    }
    c->ip=ip;++c->icount;return B86_OK;
}

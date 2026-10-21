/* B86_PM_UNIFIED_V5: i386_pm19.inc */
/* PM19: CPL0 32-bit protected string opcodes.
 * A4-A7 / AA-AF MOVS/CMPS/STOS/LODS/SCAS; F2/F3 repeat; FC/FD CLD/STD.
 * 66h operand and 67h address overrides; DS/FS/GS/etc source overrides;
 * fixed ES destination. Segment checks precede each iteration's stores.
 * A bounded 64-iteration chunk ensures large REP does not monopolize b86_step.
 * If a later iteration is invalid, prior iterations remain architecturally
 * visible and the faulting element's registers/memory remain unchanged.
 * Protected interrupts, task/privilege changes and 16-bit CS remain out of scope.
 */
#include "b86_pm19.h"
#ifndef B86_PM19_FLASH
#define B86_PM19_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
#define B86_PM19_DF 0x400u
static B86_PM19_FLASH uint32_t b86_pm19_szp(uint32_t v,unsigned bits)
{
 uint32_t pf=0;unsigned x=v&255u;while(x){pf^=x&1u;x>>=1u;}
 return (v?0u:B86_ZF)|((v>>(bits-1u)&1u)?B86_SF:0u)|(pf?0u:B86_PF);
}
static B86_PM19_FLASH uint32_t b86_pm19_subflags(uint32_t old,uint32_t a,uint32_t b,unsigned n)
{
 uint32_t mask=n==4u?UINT32_MAX:n==2u?0xffffu:0xffu;
 uint32_t sign=1u<<(n*8u-1u);
 uint32_t r=(a-b)&mask;
 uint32_t f=old&~(uint32_t)(B86_CF|B86_OF|B86_AF|B86_SF|B86_ZF|B86_PF);
 if(a<b)f|=B86_CF;
 if(((a^b)&(a^r)&sign)!=0)f|=B86_OF;
 if(((a^b^r)&16u)!=0)f|=B86_AF;
 return f|b86_pm19_szp(r,n*8u);
}
B86_PM19_FLASH int b86_pm19_step(B86Cpu *c)
{
 uint32_t start,ip,slinear=0,dlinear=0,sv=0,dv=0;
 uint8_t op;
 unsigned srcseg=B86_DS,width=0,rep=0,p66=0,p67=0,segpref=0,iterations=0;
 int op32,ad32,use_src,use_dst,dst_write,cmp,si_used,di_used;
 B86PM2Fault fault;
 if(c&&c->model>=B86_CPU_386&&c->shutdown386)return B86_SHUTDOWN;
 if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
    !c->pm386.seg[B86_CS].valid||
    c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
    !(c->pm386.seg[B86_CS].access&8u)||(c->seg[B86_CS]&3u)||
    c->ip>UINT32_MAX-15u || (!(c->pm386.seg[B86_CS].flags&4u) && c->ip>0xFFFFu) ||
    c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
    (c->flags&0x20000u))return (c->pm386.seg[B86_CS].flags&4u)?b86_pm18_step(c):b86_pm11_step(c);
 ip=start=c->ip;
 for(;;){
  if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
  if(op==0x66u){if(p66)return B86_UNSUPPORTED;p66=1;continue;}
  if(op==0x67u){if(p67)return B86_UNSUPPORTED;p67=1;continue;}
  if(op==0xf2u||op==0xf3u){if(rep)return B86_UNSUPPORTED;rep=op;continue;}
  if(op==0x26u||op==0x2eu||op==0x36u||op==0x3eu||op==0x64u||op==0x65u){
   if(segpref)return B86_UNSUPPORTED;
   segpref=1;
   switch(op){case 0x26u:srcseg=B86_ES;break;case 0x2eu:srcseg=B86_CS;break;
    case 0x36u:srcseg=B86_SS;break;case 0x3eu:srcseg=B86_DS;break;
    case 0x64u:srcseg=B86_FS;break;default:srcseg=B86_GS;break;}
   continue;
  }
  break;
 }
 if(op!=0xfcu&&op!=0xfdu&&!(op>=0xa4u&&op<=0xa7u)&&
    !(op>=0xaau&&op<=0xafu))return (c->pm386.seg[B86_CS].flags&4u)?b86_pm18_step(c):b86_pm11_step(c);
 if(op==0xfcu||op==0xfdu){
  if(rep||segpref||p66||p67)return B86_UNSUPPORTED;
  if(op==0xfcu)c->flags&=~(uint32_t)B86_PM19_DF;
  else c->flags|=B86_PM19_DF;
  c->ip=(c->pm386.seg[B86_CS].flags&4u)?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
 }
 op32=!!(c->pm386.seg[B86_CS].flags&4u)^(int)p66;
 ad32=!!(c->pm386.seg[B86_CS].flags&4u)^(int)p67;
 width=(op&1u)?(op32?4u:2u):1u;
 /* A4/A5 MOVS; A6/A7 CMPS; AA/AB STOS; AC/AD LODS; AE/AF SCAS. */
 use_src=(op>=0xa4u&&op<=0xa7u)||(op>=0xacu&&op<=0xadu);
 use_dst=(op==0xa4u||op==0xa5u||op==0xa6u||op==0xa7u||
          op==0xaau||op==0xabu||op==0xaeu||op==0xafu);
 dst_write=(op==0xa4u||op==0xa5u||op==0xaau||op==0xabu);
 cmp=(op==0xa6u||op==0xa7u||op==0xaeu||op==0xafu);
 si_used=use_src;di_used=use_dst;
 if(segpref&&!use_src)return B86_UNSUPPORTED;
 if(rep && ((ad32?c->r[B86_CX]:(c->r[B86_CX]&0xffffu))==0u)){
  c->ip=(c->pm386.seg[B86_CS].flags&4u)?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
 }
 do{
  uint32_t mask=ad32?UINT32_MAX:0xffffu;
  uint32_t soff=c->r[B86_SI]&mask,doff=c->r[B86_DI]&mask;
  uint32_t counter=ad32?c->r[B86_CX]:(c->r[B86_CX]&0xffffu);
  int32_t delta=(c->flags&B86_PM19_DF)?-(int32_t)width:(int32_t)width;
  if(rep && !counter)break;
  if(use_src){
   if(b86_pm2_check_access(c,srcseg,soff,width,B86_PM1_READ,&slinear,&fault)!=B86_PM2_OK)
    return B86_UNSUPPORTED;
  }
  if(use_dst){
   if(b86_pm2_check_access(c,B86_ES,doff,width,dst_write?B86_PM1_WRITE:B86_PM1_READ,
          &dlinear,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
  }
  if(use_src)sv=b86_pm12_load(c,slinear,width);
  if(use_dst && !dst_write)dv=b86_pm12_load(c,dlinear,width);
  /* Sources for STOS/SCAS are accumulator, not DS:SI. */
  if(op==0xaau||op==0xabu||op==0xaeu||op==0xafu)sv=b86_pm6_reg(c,B86_AX,width);
  if(dst_write)b86_pm16_store(c,dlinear,sv,width);
  if(op==0xacu||op==0xadu)b86_pm6_putreg(c,B86_AX,width,sv);
  if(cmp)c->flags=b86_pm19_subflags(c->flags,sv,dv,width);
  if(si_used)c->r[B86_SI]=(c->r[B86_SI]&~mask)|((soff+(uint32_t)delta)&mask);
  if(di_used)c->r[B86_DI]=(c->r[B86_DI]&~mask)|((doff+(uint32_t)delta)&mask);
  if(rep){
   counter=(counter-1u)&mask;
   c->r[B86_CX]=(c->r[B86_CX]&~mask)|counter;
   if(cmp&&((rep==0xf3u && !(c->flags&B86_ZF))||
             (rep==0xf2u && (c->flags&B86_ZF))))break;
   if(!counter)break;
  }
  ++iterations;
 }while(rep && iterations<64u);
 /* If more REP iterations are pending, resume same instruction on next step.
  * No interrupt is injected here; stepping is bounded for responsiveness. */
 if(rep && (ad32?c->r[B86_CX]:(c->r[B86_CX]&0xffffu))!=0u &&
    (!cmp || (rep==0xf3u?!!(c->flags&B86_ZF):!(c->flags&B86_ZF))) &&
    iterations>=64u) return B86_OK;
 c->ip=(c->pm386.seg[B86_CS].flags&4u)?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
}

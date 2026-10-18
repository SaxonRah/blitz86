/* PM18: CPL0 32-bit protected-code shifts/rotates and test/unary operations.
 * C0/C1, D0..D3: ROL/ROR/RCL/RCR/SHL/SHR/SAL/SAR (8/16/32-bit).
 * 84/85, A8/A9, F6/F7 /0 TEST; F6/F7 /2 NOT, /3 NEG.
 * Preflight and fail-closed for unimplemented/undefined counts, real-mode
 * delegation for other opcodes, no ABI or JIT modifications.
 */
#include "b86_pm18.h"
#ifndef B86_PM18_FLASH
#define B86_PM18_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM18_FLASH uint32_t b86_pm18_mask(unsigned n)
{ return n==4u?UINT32_MAX:(1u<<(n*8u))-1u; }
static B86_PM18_FLASH uint32_t b86_pm18_parity(uint32_t v)
{ unsigned k=0;v&=0xFFu;while(v){k^=v&1u;v>>=1u;}return k?0u:4u; }
static B86_PM18_FLASH uint32_t b86_pm18_szp(uint32_t v,unsigned bits)
{
 return (v?0u:B86_ZF)|((v>>(bits-1u))?B86_SF:0u)|b86_pm18_parity(v);
}
static B86_PM18_FLASH uint32_t b86_pm18_getimm(B86Cpu*c,uint32_t*ip,uint32_t start,unsigned n,int*ok)
{
 uint32_t v=0;uint8_t b;
 for(unsigned i=0;i<n;i++){
  if(!b86_pm4_byte(c,ip,start,&b)){*ok=0;return 0u;}
  v|=(uint32_t)b<<(i*8u);
 }
 *ok=1;return v;
}
B86_PM18_FLASH int b86_pm18_step(B86Cpu*c)
{
 uint32_t ip,start,lin=0,src=0,result=0,imm=0,flags,mask,sign,cf;
 uint8_t op;
 B86PM6EA ea;B86PM2Fault fault;
 unsigned seg=B86_DS,n=0,bits,group=0,count=0,effective=0,action=0;
 int p66=0,p67=0,segpref=0,op32,ad32,ok=0,ismem=0,do_write=0;
 if(c && c->model>=B86_CPU_386 && c->shutdown386)return B86_SHUTDOWN;
 if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
    !c->pm386.seg[B86_CS].valid||
    c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
    !(c->pm386.seg[B86_CS].access&8u)||(c->seg[B86_CS]&3u)||
    !(c->pm386.seg[B86_CS].flags&4u)||c->ip>UINT32_MAX-15u||
    c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
    (c->flags&(0x20000u|0x4000u)))return b86_pm17_step(c);
 start=ip=c->ip;
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
 if(!(op==0xC0u||op==0xC1u||(op>=0xD0u&&op<=0xD3u)||
      op==0xF6u||op==0xF7u||op==0x84u||op==0x85u||
      op==0xA8u||op==0xA9u))return b86_pm17_step(c);
 op32=1^p66;ad32=1^p67;
 if(op==0xA8u||op==0xA9u){
  if(segpref)return B86_UNSUPPORTED;
  n=op==0xA8u?1u:(op32?4u:2u);
  imm=b86_pm18_getimm(c,&ip,start,n,&ok);
  if(!ok)return B86_UNSUPPORTED;
  src=b86_pm6_reg(c,B86_AX,n);
  result=(src&imm)&b86_pm18_mask(n);
  flags=c->flags&~(uint32_t)(B86_CF|B86_OF|B86_SF|B86_ZF|B86_PF);
  c->flags=flags|b86_pm18_szp(result,n*8u);
  c->ip=ip;++c->icount;return B86_OK;
 }
 if(!b86_pm6_ea(c,&ip,start,ad32,&ea))return B86_UNSUPPORTED;
 ismem=!ea.is_reg;
 if(segpref&&!ismem)return B86_UNSUPPORTED;
 group=ea.reg;
 if(op==0xF6u||op==0xF7u){
  if(group>=4u)return b86_pm17_step(c);
  if(group==1u)return B86_UNSUPPORTED;
  n=op==0xF6u?1u:(op32?4u:2u);
  if(group==0u){imm=b86_pm18_getimm(c,&ip,start,n,&ok);if(!ok)return B86_UNSUPPORTED;}
  else do_write=1;
 }else if(op==0x84u||op==0x85u)n=op==0x84u?1u:(op32?4u:2u);
 else{
  n=(op==0xC0u||op==0xD0u||op==0xD2u)?1u:(op32?4u:2u);
  if(op==0xC0u||op==0xC1u){
   imm=b86_pm18_getimm(c,&ip,start,1u,&ok);if(!ok)return B86_UNSUPPORTED;
   count=imm&31u;
  }else if(op==0xD0u||op==0xD1u)count=1u;
  else count=c->r[B86_CX]&31u;
  do_write=1;
 }
 bits=n*8u;mask=b86_pm18_mask(n);sign=1u<<(bits-1u);
 if(op==0xC0u||op==0xC1u||(op>=0xD0u&&op<=0xD3u)){
  /* 386 masks count to 5 bits. For rotates, the actual rotation width
   * further reduces the count. Counts larger than operand width for shifts
   * are explicitly unsupported until their undefined-flag behavior is modeled. */
  if(group==0u||group==1u)effective=count%bits;
  else if(group==2u||group==3u)effective=count%(bits+1u);
  else if(group>=4u && group<=7u){
   if(count>bits)return B86_UNSUPPORTED;
   effective=count;
  }
  if(group>7u)return B86_UNSUPPORTED;
 }
 if(ismem){
  action=do_write?B86_PM1_WRITE:B86_PM1_READ;
  if(do_write && (op>=0xC0u&&op<=0xD3u) && !count)action=B86_PM1_READ;
  if(b86_pm2_check_access(c,segpref?seg:ea.seg,ea.off,n,
                          action,&lin,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
  src=b86_pm12_load(c,lin,n);
 }else src=b86_pm6_reg(c,ea.rm,n);
 src&=mask;
 if(op==0x84u||op==0x85u||((op==0xF6u||op==0xF7u)&&group==0u)){
  uint32_t operand=(op==0x84u||op==0x85u)?b86_pm6_reg(c,ea.reg,n):imm;
  result=src&operand;
  flags=c->flags&~(uint32_t)(B86_CF|B86_OF|B86_SF|B86_ZF|B86_PF);
  c->flags=flags|b86_pm18_szp(result,bits);
 }else if(op==0xF6u||op==0xF7u){
  if(group==2u)result=(~src)&mask;
  else{
   result=(-src)&mask;
   flags=c->flags&~(uint32_t)(B86_CF|B86_OF|B86_SF|B86_ZF|B86_PF|0x10u);
   if(src)flags|=B86_CF;
   if(src==sign)flags|=B86_OF;
   if(src&0xFu)flags|=0x10u;
   c->flags=flags|b86_pm18_szp(result,bits);
  }
 }else{
  result=src;
  if(count){
   cf=(c->flags&B86_CF)?1u:0u;
   if(group==0u){ /* ROL */
    if(effective)result=((src<<effective)|(src>>(bits-effective)))&mask;
    cf=result&1u;
   }else if(group==1u){ /* ROR */
    if(effective)result=((src>>effective)|(src<<(bits-effective)))&mask;
    cf=(result>>(bits-1u))&1u;
   }else if(group==2u){ /* RCL via carry-inclusive rotation */
    for(unsigned i=0;i<effective;i++){
     uint32_t out=(result>>(bits-1u))&1u;
     result=((result<<1u)|cf)&mask;cf=out;
    }
   }else if(group==3u){ /* RCR */
    for(unsigned i=0;i<effective;i++){
     uint32_t out=result&1u;
     result=(result>>1u)|(cf<<(bits-1u));cf=out;
    }
   }else if(group==4u||group==6u){ /* SHL / SAL */
    cf=(src>>(bits-count))&1u;
    result=count==bits?0u:(src<<count)&mask;
   }else if(group==5u){ /* SHR */
    cf=(src>>(count-1u))&1u;result=count==bits?0u:src>>count;
   }else{ /* SAR */
    cf=(src>>(count-1u))&1u;
    int64_t signed_src=n==1u?(int64_t)(int8_t)src:n==2u?(int64_t)(int16_t)src:(int64_t)(int32_t)src;
    result=(uint32_t)(signed_src>>count)&mask;
   }
   flags=c->flags&~(uint32_t)B86_CF;
   flags|=cf?B86_CF:0u;
   if(group>=4u){
    flags&=~(uint32_t)(B86_SF|B86_ZF|B86_PF);
    flags|=b86_pm18_szp(result,bits);
   }
   if(count==1u){
    flags&=~(uint32_t)B86_OF;
    if(group==0u||group==2u||group==4u||group==6u){
     if((((result>>(bits-1u))&1u)^cf)!=0u)flags|=B86_OF;
    }else if(group==1u||group==3u){
     if((((result>>(bits-1u))^(result>>(bits-2u)))&1u)!=0u)flags|=B86_OF;
    }else if(group==5u){if(src&sign)flags|=B86_OF;}
    /* SAR: OF=0 */
   }
   c->flags=flags;
  }
  /* Even effective=0 after masking to the operand width (ROL by 8 etc.)
   * can update CF; only an actual masked count=0 preserves flags. */
  if(!count)do_write=0;
 }
 if(do_write){
  if(ismem)b86_pm16_store(c,lin,result,n);
  else b86_pm6_putreg(c,ea.rm,n,result);
 }
 c->ip=ip;++c->icount;return B86_OK;
}

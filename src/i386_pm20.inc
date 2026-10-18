/* PM20: 80386 CPL0 32-bit CS frame/flag/utility instructions.
 * PUSHA/POPA, ENTER/LEAVE, PUSHF/POPF, LAHF/SAHF, conversion and carry.
 * All multi-slot stack accesses are checked before any register/memory change.
 * Unsupported task mode, nested task flags and unimplemented privilege modes
 * remain fail-closed. This module never changes real-mode/JIT execution.
 */
#include "b86_pm20.h"
#ifndef B86_PM20_FLASH
#define B86_PM20_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM20_FLASH int b86_pm20_slot(B86Cpu*c,uint32_t sp,unsigned n,unsigned access,uint32_t*linear)
{
 B86PM2Fault f;
 int ss32=!!(c->pm386.seg[B86_SS].flags&4u);
 return b86_pm2_check_access(c,B86_SS,ss32?sp:(sp&0xffffu),n,access,linear,&f)==B86_PM2_OK;
}
static B86_PM20_FLASH uint32_t b86_pm20_add(B86Cpu*c,uint32_t sp,int32_t delta)
{
 return b86_pm7_stack_add(sp,!!(c->pm386.seg[B86_SS].flags&4u),(uint32_t)delta);
}
static B86_PM20_FLASH uint32_t b86_pm20_sreg(B86Cpu*c,unsigned r,unsigned n)
{ return b86_pm6_reg(c,r,n); }
B86_PM20_FLASH int b86_pm20_step(B86Cpu*c)
{
 uint32_t ip,start,sp,newsp,at=0,value=0,oldbp=0,frame=0,flags;
 uint32_t addr[33],word[33],reads[31],readaddr[31];
 unsigned count=0,r=0,w=4,nesting=0,locals=0,p66=0,p67=0,segpref=0,segment=B86_DS;
 uint8_t op,b;
 B86PM2Fault fault;
 if(c && c->model>=B86_CPU_386&&c->shutdown386)return B86_SHUTDOWN;
 if(!c||!c->mem||c->model!=B86_CPU_386||!(c->msw&1u)||
    !c->pm386.seg[B86_CS].valid||
    c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER||
    !(c->pm386.seg[B86_CS].access&8u)||(c->seg[B86_CS]&3u)||
    !(c->pm386.seg[B86_CS].flags&4u)||c->ip>UINT32_MAX-15u||
    c->lz_kind!=LZ_NONE||c->lz_ikind!=LZ_NONE||
    (c->flags&(0x20000u|0x4000u)))return b86_pm19_step(c);
 ip=start=c->ip;
 for(;;){
  if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
  if(op==0x66u){if(p66)return B86_UNSUPPORTED;p66=1;continue;}
  if(op==0x67u){if(p67)return B86_UNSUPPORTED;p67=1;continue;}
  if(op==0x26u||op==0x2eu||op==0x36u||op==0x3eu||op==0x64u||op==0x65u){
   if(segpref)return B86_UNSUPPORTED;
   segpref=1;
   switch(op){case 0x26u:segment=B86_ES;break;case 0x2eu:segment=B86_CS;break;
    case 0x36u:segment=B86_SS;break;case 0x3eu:segment=B86_DS;break;
    case 0x64u:segment=B86_FS;break;default:segment=B86_GS;break;}
   continue;
  }
  break;
 }
 if(!(op==0x60u||op==0x61u||op==0x98u||op==0x99u||
      (op>=0x9cu&&op<=0x9fu)||op==0xc8u||op==0xc9u||
      op==0xf5u||op==0xf8u||op==0xf9u||op==0xd7u))return b86_pm19_step(c);
 if((p67 && op!=0xd7u) || (segpref && op!=0xd7u) ||
    (p66 && (op==0xd7u||op==0xf5u||op==0xf8u||op==0xf9u||op==0x9eu||op==0x9fu)))
   return B86_UNSUPPORTED;
 w=p66?2u:4u;sp=c->r[B86_SP];
 if(op==0xf5u||op==0xf8u||op==0xf9u){
  if(op==0xf5u)c->flags^=B86_CF;
  else if(op==0xf8u)c->flags&=~(uint32_t)B86_CF;
  else c->flags|=B86_CF;
 }else if(op==0x98u){
  if(w==2u) b86_pm6_putreg(c,B86_AX,2u,(uint16_t)(int16_t)(int8_t)c->r[B86_AX]);
  else c->r[B86_AX]=(uint32_t)(int32_t)(int16_t)c->r[B86_AX];
 }else if(op==0x99u){
  if(w==2u)b86_pm6_putreg(c,B86_DX,2u,((int16_t)c->r[B86_AX]<0)?0xffffu:0);
  else c->r[B86_DX]=((int32_t)c->r[B86_AX]<0)?UINT32_MAX:0;
 }else if(op==0x9eu){
  uint32_t ah=(c->r[B86_AX]>>8u)&255u;
  c->flags=(c->flags&~(uint32_t)0xd5u)|(ah&0xd5u)|2u;
 }else if(op==0x9fu){
  uint32_t ah=(c->flags&0xd5u)|2u;
  c->r[B86_AX]=(c->r[B86_AX]&~0xff00u)|(ah<<8u);
 }else if(op==0xd7u){
  uint32_t base=p67?(c->r[B86_BX]&0xffffu):c->r[B86_BX];
  uint32_t off=(base+(c->r[B86_AX]&255u))&(p67?0xffffu:UINT32_MAX);
  if(b86_pm2_check_access(c,segment,off,1u,B86_PM1_READ,&at,&fault)!=B86_PM2_OK)return B86_UNSUPPORTED;
  b86_pm6_putreg(c,B86_AX,1u,b86_pm12_load(c,at,1u));
 }else if(op==0x9cu){
  uint32_t val=(c->flags&~(uint32_t)(0x10000u|0x20000u))|2u;
  newsp=b86_pm20_add(c,sp,-(int32_t)w);
  if(!b86_pm20_slot(c,newsp,w,B86_PM1_WRITE,&at))return B86_UNSUPPORTED;
  b86_pm16_store(c,at,val,w);c->r[B86_SP]=newsp;
 }else if(op==0x9du){
  if(!b86_pm20_slot(c,sp,w,B86_PM1_READ,&at))return B86_UNSUPPORTED;
  value=b86_pm12_load(c,at,w);
  flags=w==2u?(c->flags&0xffff0000u)|(value&0xffffu):value;
  /* Task and virtual mode control need TSS/VM86 handling. */
  if(flags&(0x20000u|0x4000u))return B86_UNSUPPORTED;
  c->flags=(flags&~(uint32_t)0x10000u)|(c->flags&0x10000u)|2u;
  c->r[B86_SP]=b86_pm20_add(c,sp,(int32_t)w);
 }else if(op==0x60u){
  uint32_t original=sp;
  static const unsigned regs[8]={B86_AX,B86_CX,B86_DX,B86_BX,B86_SP,B86_BP,B86_SI,B86_DI};
  for(r=0;r<8u;r++){
   sp=b86_pm20_add(c,sp,-(int32_t)w);
   if(!b86_pm20_slot(c,sp,w,B86_PM1_WRITE,&addr[r]))return B86_UNSUPPORTED;
   word[r]=(r==4u?original:b86_pm20_sreg(c,regs[r],w));
  }
  for(r=0;r<8u;r++)b86_pm16_store(c,addr[r],word[r],w);
  c->r[B86_SP]=sp;
 }else if(op==0x61u){
  uint32_t val[8];
  static const unsigned regs[8]={B86_DI,B86_SI,B86_BP,B86_SP,B86_BX,B86_DX,B86_CX,B86_AX};
  for(r=0;r<8u;r++){
   if(!b86_pm20_slot(c,sp,w,B86_PM1_READ,&addr[r]))return B86_UNSUPPORTED;
   val[r]=b86_pm12_load(c,addr[r],w);
   sp=b86_pm20_add(c,sp,(int32_t)w);
  }
  for(r=0;r<8u;r++)if(r!=3u)b86_pm6_putreg(c,regs[r],w,val[r]);
  c->r[B86_SP]=sp;
 }else if(op==0xc9u){ /* LEAVE: move BP into SP then pop BP */
  uint32_t from=c->pm386.seg[B86_SS].flags&4u?c->r[B86_BP]:
    (c->r[B86_SP]&0xffff0000u)|(c->r[B86_BP]&0xffffu);
  if(!b86_pm20_slot(c,from,w,B86_PM1_READ,&at))return B86_UNSUPPORTED;
  value=b86_pm12_load(c,at,w);
  newsp=b86_pm20_add(c,from,(int32_t)w);
  b86_pm6_putreg(c,B86_BP,w,value);c->r[B86_SP]=newsp;
 }else if(op==0xc8u){
  uint32_t stack_at;
  if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
  locals=b;
  if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
  locals|=(uint32_t)b<<8u;
  if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
  nesting=b&31u;
  oldbp=c->r[B86_BP];
  /* Preflight all nested BP reads and all writes, including the final
   * reserved local storage boundary. No partial frame is ever committed. */
  sp=b86_pm20_add(c,sp,-(int32_t)w);
  if(!b86_pm20_slot(c,sp,w,B86_PM1_WRITE,&addr[count]))return B86_UNSUPPORTED;
  word[count++]=b86_pm20_sreg(c,B86_BP,w);frame=sp;
  for(r=1u;r<nesting;r++){
   oldbp=b86_pm20_add(c,oldbp,-(int32_t)w);
   if(!b86_pm20_slot(c,oldbp,w,B86_PM1_READ,&readaddr[r-1u]))return B86_UNSUPPORTED;
   reads[r-1u]=b86_pm12_load(c,readaddr[r-1u],w);
   sp=b86_pm20_add(c,sp,-(int32_t)w);
   if(!b86_pm20_slot(c,sp,w,B86_PM1_WRITE,&addr[count]))return B86_UNSUPPORTED;
   word[count++]=reads[r-1u];
  }
  if(nesting){
   sp=b86_pm20_add(c,sp,-(int32_t)w);
   if(!b86_pm20_slot(c,sp,w,B86_PM1_WRITE,&addr[count]))return B86_UNSUPPORTED;
   word[count++]=frame;
  }
  newsp=b86_pm20_add(c,sp,-(int32_t)locals);
  /* Verify the allocated local region; software can use all bytes in it. */
  if(locals){
   if(!b86_pm20_slot(c,newsp,1u,B86_PM1_WRITE,&stack_at))return B86_UNSUPPORTED;
   /* Crossing an SS wrap is unsupported rather than silently aliasing. */
   if((uint64_t)(c->pm386.seg[B86_SS].flags&4u?newsp:(newsp&0xffffu))+locals-1u>
      c->pm386.seg[B86_SS].limit)return B86_UNSUPPORTED;
  }
  for(r=0;r<count;r++)b86_pm16_store(c,addr[r],word[r],w);
  b86_pm6_putreg(c,B86_BP,w,frame);c->r[B86_SP]=newsp;
 }
 c->ip=ip;++c->icount;return B86_OK;
}

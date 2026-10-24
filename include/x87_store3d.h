#ifndef B86_X87_STORE3D_H
#define B86_X87_STORE3D_H
#include "x87_memory3b.h"
/* Standalone integer-store extension. Caller supplies prevalidated guest EA.
   Memory callbacks MUST atomically complete or fail without side effects.
   Unmasked invalid/precision exceptions are not yet wired to CPU #MF;
   this function conservatively declines those operations unchanged. */
enum { B86_X87_UNMASKED_EXCEPTION=-40 };
static inline void b86_x87_st_le(uint8_t *p,uint64_t n,unsigned bytes){unsigned k;for(k=0;k<bytes;k++)p[k]=(uint8_t)(n>>(8u*k));}
static inline int b86_x87_to_int_bits(const uint8_t v[10],unsigned width,unsigned rounding,
                                       uint64_t *bits,unsigned *invalid,unsigned *inexact){
    unsigned c=b86_x87_class(v),negative=b86_x87_sign(v),e=b86_x87_exp(v),r=0;
    uint64_t m=b86_x87_significand(v),mag=0,remain=0,half=0,lim;
    int shift=(int)(e?e:1u)-16383-63;
    *invalid=0;*inexact=0;
    if(c==B86_X87_ZERO){*bits=0;return 0;}
    if(c!=B86_X87_FINITE){*invalid=1;return 0;}
    if(shift>=0){
        if(shift>63 || (shift && (m>>(64-(unsigned)shift)))){*invalid=1;return 0;}
        mag=m<<(unsigned)shift;
    }else{
        r=(unsigned)(-shift);
        if(r>=65u){mag=0;remain=(m!=0u);}
        else if(r==64u){mag=0;remain=m;half=UINT64_C(0x8000000000000000);}
        else {mag=m>>r;remain=m&((UINT64_C(1)<<r)-1u);half=UINT64_C(1)<<(r-1u);}
        *inexact=(remain!=0u);
        if(remain){
            unsigned increment=0;
            if(rounding==0u){increment=half && (remain>half || (remain==half && (mag&1u)));}
            else if(rounding==1u){increment=negative;}
            else if(rounding==2u){increment=!negative;}
            if(increment)mag++;
        }
    }
    lim=UINT64_C(1)<<(width-1u);
    if((!negative && mag>=lim)||(negative && mag>lim)){*invalid=1;return 0;}
    *bits=negative?(~mag)+UINT64_C(1):mag;
    if(width<64u)*bits&=((UINT64_C(1)<<width)-UINT64_C(1));
    return 0;
}
/* Recognizes: DF /2 FIST m16, DB /2 FIST m32;
   DF /3 FISTP m16, DB /3 FISTP m32, DF /7 FISTP m64;
   DF /1 FISTTP m16, DB /1 FISTTP m32, DD /1 FISTTP m64. */
static inline int b86_x87_exec_mem3d(B86X87 *f,unsigned op,unsigned modrm,
                                      uint32_t ea,const B86X87Memory *mem){
    unsigned sub=(modrm>>3)&7u,width=0,pop=0,trunc=0,invalid=0,inexact=0,mode;
    uint8_t value[10],data[8];uint64_t bits=0;
    B86X87 next;
    int rc;
    if(!f || !mem || modrm>=0xC0u || op<0xD8u || op>0xDFu)return B86_X87_DEC_UNSUPPORTED;
    if(op==0xDFu && (sub==1u || sub==2u || sub==3u)){width=16;pop=sub!=2u;trunc=sub==1u;}
    else if(op==0xDBu && (sub==1u || sub==2u || sub==3u)){width=32;pop=sub!=2u;trunc=sub==1u;}
    else if(op==0xDDu && sub==1u){width=64;pop=1;trunc=1;}
    else if(op==0xDFu && sub==7u){width=64;pop=1;}
    else return b86_x87_exec_mem(f,op,modrm,ea,mem);
    if(!mem->write)return B86_X87_MEM_FAULT;
    next=*f;
    if(b86_x87_peek_raw(&next,0,value))return B86_X87_DEC_STACK;
    mode=trunc?3u:((next.control>>10)&3u);
    b86_x87_to_int_bits(value,width,mode,&bits,&invalid,&inexact);
    if(invalid){
        if(!(next.control&1u))return B86_X87_UNMASKED_EXCEPTION;
        bits=UINT64_C(1)<<(width-1u);next.status|=1u;inexact=0;
    }else if(inexact){
        if(!(next.control&(1u<<5)))return B86_X87_UNMASKED_EXCEPTION;
        next.status|=(1u<<5);
    }
    b86_x87_st_le(data,bits,width/8u);
    if(pop && b86_x87_pop(&next))return B86_X87_DEC_STACK;
    rc=mem->write(mem->opaque,ea,data,width/8u);
    if(rc)return B86_X87_MEM_FAULT;
    *f=next;
    return B86_X87_OK;
}
#endif

#ifndef B86_X87_MULDIV_H
#define B86_X87_MULDIV_H
#include "x87_numeric.h"
/* Finite normal extended-real multiply/divide, precision=64, nearest/even.
 * Integer-only portable C11, no __int128, compiler FP or target hardware FP.
 * Unsupported exceptional cases return -3 without modifying output.
 * This does not yet implement x87 exception/status/control semantics. */
static inline void b86_x87_mul64wide(uint64_t a,uint64_t b,uint64_t *hi,uint64_t *lo){
    uint64_t a0=(uint32_t)a,a1=a>>32,b0=(uint32_t)b,b1=b>>32;
    uint64_t p00=a0*b0,p01=a0*b1,p10=a1*b0,p11=a1*b1;
    uint64_t mid=(p00>>32)+(uint32_t)p01+(uint32_t)p10;
    *lo=(mid<<32)|(uint32_t)p00;
    *hi=p11+(p01>>32)+(p10>>32)+(mid>>32);
}
static inline int b86_x87_mul_rne(uint8_t out[10],const uint8_t a[10],const uint8_t b[10]){
    unsigned ca=b86_x87_class(a),cb=b86_x87_class(b),sa=b86_x87_sign(a)^b86_x87_sign(b);
    int exp;uint64_t hi,lo,sig,tail;unsigned increment;
    if(ca>=B86_X87_INFINITY||cb>=B86_X87_INFINITY)return -3;
    if(ca==B86_X87_ZERO||cb==B86_X87_ZERO){b86_x87_pack(out,sa,0,0);return 0;}
    if(!b86_x87_exp(a)||!b86_x87_exp(b))return -3;
    b86_x87_mul64wide(b86_x87_significand(a),b86_x87_significand(b),&hi,&lo);
    exp=(int)b86_x87_exp(a)+(int)b86_x87_exp(b)-16383;
    if(hi>>63){sig=hi;tail=lo;exp++;increment=(tail>UINT64_C(0x8000000000000000))||(tail==UINT64_C(0x8000000000000000)&&(sig&1u));}
    else{sig=(hi<<1)|(lo>>63);tail=lo<<1;increment=(tail>UINT64_C(0x8000000000000000))||(tail==UINT64_C(0x8000000000000000)&&(sig&1u));}
    if(exp<=0||exp>=32767)return -3;
    if(increment){sig++;if(!sig){sig=UINT64_C(0x8000000000000000);exp++;if(exp>=32767)return -3;}}
    b86_x87_pack(out,sa,(unsigned)exp,sig);return 0;
}
static inline int b86_x87_div_rne(uint8_t out[10],const uint8_t a[10],const uint8_t b[10]){
    unsigned ca=b86_x87_class(a),cb=b86_x87_class(b),sign=b86_x87_sign(a)^b86_x87_sign(b);
    uint64_t num,den,rem,q;unsigned i;int exp;
    if(ca>=B86_X87_INFINITY||cb>=B86_X87_INFINITY||cb==B86_X87_ZERO)return -3;
    if(ca==B86_X87_ZERO){b86_x87_pack(out,sign,0,0);return 0;}
    if(!b86_x87_exp(a)||!b86_x87_exp(b))return -3;
    num=b86_x87_significand(a);den=b86_x87_significand(b);
    exp=(int)b86_x87_exp(a)-(int)b86_x87_exp(b)+16383;
    if(num<den){/* scaled ratio: (2*num)/den; remainder=2*num-den as 65-bit */
        rem=(num<<1)-den; /* modulo-2^64 gives exact 2*num-den here */
        exp--;
    }else rem=num-den;
    if(exp<=0||exp>=32767)return -3;
    q=UINT64_C(0x8000000000000000);
    for(i=0;i<63u;i++){
        unsigned carry=(unsigned)(rem>>63);
        rem<<=1;/* remainder * 2, with 65-bit carry */
        if(carry||rem>=den){rem-=den;q|=UINT64_C(1)<<(62u-i);}
    }
    /* remaining fractional bits represent rem/den. Round to nearest/even. */
    if((rem>>63)||((rem<<1)>den)||((rem<<1)==den&&(q&1u))){
        /* comparison uses 65-bit 2*rem */
        q++;if(!q){q=UINT64_C(0x8000000000000000);exp++;if(exp>=32767)return -3;}
    }
    b86_x87_pack(out,sign,(unsigned)exp,q);return 0;
}
#endif

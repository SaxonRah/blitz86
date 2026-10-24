#ifndef B86_X87_ARITH_H
#define B86_X87_ARITH_H
#include "x87_numeric.h"
/* Stage 2B finite x87 add/sub. Exact 64-bit significand plus GRS bits,
 * round-to-nearest-even, 64-bit precision. Deliberately rejects NaN,
 * infinity, unmasked/other rounding/precision controls, exponent overflow,
 * and tiny/subnormal results until exception delivery is implemented.
 * No host floating point, 128-bit compiler extensions, or hardware FPU. */
enum {B86_X87_ARITH_OK=0,B86_X87_ARITH_UNSUPPORTED=-3};
typedef struct {uint64_t lo,hi;} B86X87Wide;
static inline B86X87Wide b86_wide_shl3(uint64_t n){B86X87Wide x; x.hi=n>>61;x.lo=n<<3;return x;}
static inline B86X87Wide b86_wide_shr1(B86X87Wide a){B86X87Wide x;x.lo=(a.lo>>1)|(a.hi<<63);x.hi=a.hi>>1;return x;}
static inline B86X87Wide b86_wide_shl1(B86X87Wide a){B86X87Wide x;x.hi=(a.hi<<1)|(a.lo>>63);x.lo=a.lo<<1;return x;}
static inline B86X87Wide b86_wide_shr_sticky(B86X87Wide a,unsigned count){
    unsigned i;for(i=0;i<count;i++){unsigned sticky=(unsigned)(a.lo&1u);a=b86_wide_shr1(a);a.lo|=sticky;}return a;
}
static inline int b86_wide_cmp(B86X87Wide a,B86X87Wide b){return a.hi>b.hi?1:a.hi<b.hi?-1:a.lo>b.lo?1:a.lo<b.lo?-1:0;}
static inline B86X87Wide b86_wide_add(B86X87Wide a,B86X87Wide b){B86X87Wide x;x.lo=a.lo+b.lo;x.hi=a.hi+b.hi+(x.lo<a.lo);return x;}
static inline B86X87Wide b86_wide_sub(B86X87Wide a,B86X87Wide b){B86X87Wide x;x.lo=a.lo-b.lo;x.hi=a.hi-b.hi-(a.lo<b.lo);return x;}
static inline int b86_x87_add_rne(uint8_t out[10],const uint8_t a[10],const uint8_t b[10],int subtract){
    uint8_t tmp[10];unsigned ca=b86_x87_class(a),cb=b86_x87_class(b),sa=b86_x87_sign(a),sb=b86_x87_sign(b)^(subtract!=0);
    unsigned ea=b86_x87_exp(a),eb=b86_x87_exp(b);B86X87Wide x,y,z;
    uint64_t ma=b86_x87_significand(a),mb=b86_x87_significand(b),sig;
    unsigned guard;int cp;
    if(ca>=B86_X87_INFINITY||cb>=B86_X87_INFINITY)return B86_X87_ARITH_UNSUPPORTED;
    if(ca==B86_X87_ZERO&&cb==B86_X87_ZERO){b86_x87_pack(out,sa&sb,0,0);return 0;}
    if(ca==B86_X87_ZERO){b86_x87_pack(out,sb,eb,mb);return 0;}
    if(cb==B86_X87_ZERO){memcpy(out,a,10);return 0;}
    /* This initial arithmetic stage handles normalized extended values.
     * Subnormal extended values are deferred until exception semantics exist. */
    if(ea==0||eb==0)return B86_X87_ARITH_UNSUPPORTED;
    x=b86_wide_shl3(ma);y=b86_wide_shl3(mb);
    if(ea>eb)y=b86_wide_shr_sticky(y,ea-eb>70u?70u:ea-eb);
    else if(eb>ea){x=b86_wide_shr_sticky(x,eb-ea>70u?70u:eb-ea);ea=eb;}
    if(sa==sb){z=b86_wide_add(x,y);if(z.hi&8u){z=b86_wide_shr_sticky(z,1u);ea++;} }
    else {cp=b86_wide_cmp(x,y);if(!cp){b86_x87_pack(out,0,0,0);return 0;}
       if(cp>0)z=b86_wide_sub(x,y);else{z=b86_wide_sub(y,x);sa=sb;}
       while(z.hi<4u){z=b86_wide_shl1(z);if(ea<=1u)return B86_X87_ARITH_UNSUPPORTED;ea--;}
    }
    if(ea>=32767u)return B86_X87_ARITH_UNSUPPORTED;
    sig=(z.hi<<61)|(z.lo>>3);
    guard=(unsigned)(z.lo&7u);
    if(guard>4u||(guard==4u&&(sig&1u))){sig++;if(!sig){sig=UINT64_C(0x8000000000000000);ea++;if(ea>=32767u)return B86_X87_ARITH_UNSUPPORTED;}}
    b86_x87_pack(tmp,sa,ea,sig);memcpy(out,tmp,10);return 0;
}
#endif

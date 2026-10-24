#ifndef B86_X87_NUMERIC_H
#define B86_X87_NUMERIC_H
/* Stage 2A: exact inputs, classification, ordered comparisons. No arithmetic yet.
 * Caller supplies IEEE-754 bits: no host-FP precision or endian assumptions.
 * x87 pseudo-denormals/unsupported encodings are classified separately. */
#include "x87_state.h"

enum { B86_X87_ZERO=0, B86_X87_FINITE=1, B86_X87_INFINITY=2,
       B86_X87_NAN=3, B86_X87_UNSUPPORTED=4 };
static inline uint64_t b86_x87_significand(const uint8_t v[10]) {
    uint64_t n=0; unsigned i;
    for(i=0;i<8u;i++)n|=((uint64_t)v[i])<<(8u*i);
    return n;
}
static inline unsigned b86_x87_exp(const uint8_t v[10]) {
    return ((unsigned)v[8] | (((unsigned)v[9]&0x7fu)<<8));
}
static inline unsigned b86_x87_sign(const uint8_t v[10]) {return (v[9]>>7)&1u;}
static inline void b86_x87_pack(uint8_t v[10],unsigned sign,unsigned exp,uint64_t sig){
    unsigned i;for(i=0;i<8u;i++)v[i]=(uint8_t)(sig>>(i*8u));
    v[8]=(uint8_t)exp;v[9]=(uint8_t)((exp>>8)|((sign&1u)<<7));
}
static inline unsigned b86_x87_class(const uint8_t v[10]) {
    unsigned e=b86_x87_exp(v);uint64_t m=b86_x87_significand(v);
    if(e==0u){if(!m)return B86_X87_ZERO;return (m>>63)?B86_X87_UNSUPPORTED:B86_X87_FINITE;}
    if(e==32767u){if(m==UINT64_C(0x8000000000000000))return B86_X87_INFINITY;
        return (m>>63)?B86_X87_NAN:B86_X87_UNSUPPORTED;}
    return (m>>63)?B86_X87_FINITE:B86_X87_UNSUPPORTED;
}
/* Exact encoding of finite IEEE-754 32-bit bit patterns, infinities and NaNs.
 * Preserve sign, quiet/signaling payload and category where possible. */
static inline void b86_x87_from_f32_bits(uint8_t out[10],uint32_t bits){
    unsigned s=bits>>31,e=(bits>>23)&255u;uint32_t m=bits&0x7fffffu;
    if(e==255u){b86_x87_pack(out,s,32767u,m? (UINT64_C(0x8000000000000000)|((uint64_t)m<<40)):
        UINT64_C(0x8000000000000000));return;}
    if(e==0u){
        unsigned lead=0u;
        if(!m){b86_x87_pack(out,s,0u,0u);return;}
        while((m & (1u<<22))==0u){m<<=1;lead++;}
        b86_x87_pack(out,s, (unsigned)(16383-126-1-lead),((uint64_t)m)<<41);
        return;
    }
    b86_x87_pack(out,s,(unsigned)(e+16383-127),((UINT64_C(1)<<23)|m)<<40);
}
static inline void b86_x87_from_f64_bits(uint8_t out[10],uint64_t bits){
    unsigned s=(unsigned)(bits>>63),e=(unsigned)((bits>>52)&2047u);uint64_t m=bits&UINT64_C(0x000fffffffffffff);
    if(e==2047u){b86_x87_pack(out,s,32767u,m?(UINT64_C(0x8000000000000000)|(m<<11)):
        UINT64_C(0x8000000000000000));return;}
    if(e==0u){
        unsigned lead=0u;
        if(!m){b86_x87_pack(out,s,0u,0u);return;}
        while((m & (UINT64_C(1)<<51))==0u){m<<=1;lead++;}
        b86_x87_pack(out,s,(unsigned)(16383-1022-1-lead),m<<12);
        return;
    }
    b86_x87_pack(out,s,(unsigned)(e+16383-1023),( (UINT64_C(1)<<52)|m )<<11);
}
/* Exact signed-64 integer -> 80-bit value, including INT64_MIN. */
static inline void b86_x87_from_i64(uint8_t out[10],int64_t value){
    uint64_t n;unsigned sign=value<0,lead=0;
    if(!value){b86_x87_pack(out,0,0,0);return;}
    n=sign? (~(uint64_t)value)+1u:(uint64_t)value;
    {uint64_t t=n;while(t>>1){t>>=1;lead++;}}
    b86_x87_pack(out,sign,16383u+lead,n<<(63u-lead));
}
static inline void b86_x87_fchs(uint8_t v[10]){v[9]^=0x80u;}
static inline void b86_x87_fabs(uint8_t v[10]){v[9]&=0x7fu;}
/* -2 unordered, -1 a<b, 0 equal, 1 a>b. NaNs and unsupported: unordered.
 * Signed zeros compare equal; subnormal and normal boundaries order correctly. */
static inline int b86_x87_compare(const uint8_t a[10],const uint8_t b[10]){
    unsigned ca=b86_x87_class(a),cb=b86_x87_class(b),sa,sb,ea,eb;
    uint64_t ma,mb;int cmp;
    if(ca>=B86_X87_NAN||cb>=B86_X87_NAN)return -2;
    if(ca==B86_X87_ZERO&&cb==B86_X87_ZERO)return 0;
    sa=b86_x87_sign(a);sb=b86_x87_sign(b);
    if(sa!=sb)return sa?-1:1;
    ea=b86_x87_exp(a);eb=b86_x87_exp(b);
    ma=b86_x87_significand(a);mb=b86_x87_significand(b);
    cmp=ea<eb?-1:ea>eb?1:ma<mb?-1:ma>mb?1:0;
    return sa?-cmp:cmp;
}
/* x87 full two-bit tag: valid=0, zero=1, special=2; empty=3 is stack state. */
static inline unsigned b86_x87_full_tag(const uint8_t v[10]){
    unsigned c=b86_x87_class(v);
    return c==B86_X87_ZERO?1u:c==B86_X87_FINITE?0u:2u;
}
static inline int b86_x87_push_f64_bits(B86X87 *f,uint64_t bits){
    uint8_t v[10];b86_x87_from_f64_bits(v,bits);
    return b86_x87_push_raw(f,v,b86_x87_full_tag(v));
}
static inline int b86_x87_push_i64(B86X87 *f,int64_t value){
    uint8_t v[10];b86_x87_from_i64(v,value);
    return b86_x87_push_raw(f,v,b86_x87_full_tag(v));
}
#endif

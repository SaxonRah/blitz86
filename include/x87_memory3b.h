#ifndef B86_X87_MEMORY3B_H
#define B86_X87_MEMORY3B_H
#include "x87_decode3a.h"
/* Stage 3B: gated memory operands. EA is precomputed by the caller using
 * x86 segment, 16/32-bit address-size, page, permissions and limit checks.
 * The callbacks must implement atomic all-or-fail transfers for a full width,
 * including page crossing. Failed reads/writes MUST have no side effects.
 * These callbacks are NOT direct pointers into guest memory. */
typedef int (*B86X87MemRead)(void *, uint32_t ea, uint8_t *dst, unsigned size);
typedef int (*B86X87MemWrite)(void *, uint32_t ea, const uint8_t *src, unsigned size);
typedef struct B86X87Memory {void *opaque; B86X87MemRead read; B86X87MemWrite write;} B86X87Memory;
enum { B86_X87_MEM_FAULT=-30 };
static inline uint32_t b86_x87_le32(const uint8_t *a){return (uint32_t)a[0]|((uint32_t)a[1]<<8)|((uint32_t)a[2]<<16)|((uint32_t)a[3]<<24);}
static inline uint64_t b86_x87_le64(const uint8_t *a){return (uint64_t)b86_x87_le32(a)|((uint64_t)b86_x87_le32(a+4)<<32);}
static inline void b86_x87_st16(uint8_t *a,unsigned n){a[0]=(uint8_t)n;a[1]=(uint8_t)(n>>8);}
/* Supported: D9 /0 FLD m32real; DD /0 FLD m64real;
 * DB /5 FLD m80real; DB /3 FSTP m80real;
 * DB /0 FILD m32int; DF /0 FILD m16int; DF /5 FILD m64int;
 * D9 /5 FLDCW m2byte;
 * D9 /7 FNSTCW m2byte; DD /7 FNSTSW m2byte.
 * Unsupported instructions return immediately without calling callbacks. */
/* Stage4R: complete memory arithmetic/compare families, with operand width
 * determined by opcode: D8 m32real, DC m64real, DA m32int, DE m16int.
 * Arithmetic is limited to the already-tested numeric core (normal finite,
 * masked finite/zero division); unsupported cases refuse atomically.
 * Memory reads use the existing full-range PM/paging preflight. */
/* Stage4T: binary80 -> IEEE754 binary32/binary64 scalar store.
 * Pure integer conversion with x87 RC, masked IE/OE/UE/PE and C1.
 * Refuses unmasked exceptions transactionally, before any memory callback.
 * This does not implement 387 precision-control rounding for arithmetic. */
static inline uint64_t b86_x87_round_shr64(uint64_t sig,unsigned sh,unsigned sign,
                            unsigned rc,unsigned *inexact,unsigned *up){
    uint64_t q,rem=0,half=0;
    unsigned inc=0;
    if(!sh){*inexact=0;*up=0;return sig;}
    if(sh<64u){q=sig>>sh;rem=sig&((UINT64_C(1)<<sh)-1u);half=UINT64_C(1)<<(sh-1u);}
    else {q=0u;rem=sig;half=(sh==64u)?UINT64_C(0x8000000000000000):0u;}
    *inexact=(rem!=0u);
    if(rem){
        if(rc==0u)inc=(half!=0u && (rem>half || (rem==half && (q&1u))));
        else if(rc==1u)inc=sign;
        else if(rc==2u)inc=!sign;
    }
    *up=inc;return q+inc;
}
static inline int b86_x87_to_ieee_bits(B86X87 *f,const uint8_t v[10],
                                       unsigned width,uint64_t *outbits){
    unsigned c=b86_x87_class(v),sign=b86_x87_sign(v),exp=b86_x87_exp(v);
    unsigned p=(width==32u)?24u:53u,eb=(width==32u)?8u:11u;
    unsigned bias=(width==32u)?127u:1023u,rc=(f->control>>10)&3u;
    unsigned inexact=0u,up=0u,underflow=0u,overflow=0u,invalid=0u,denorm=0u;
    uint64_t sig=b86_x87_significand(v),fracmask=(UINT64_C(1)<<(p-1u))-1u;
    uint64_t q,bits=0;
    int e,emin=1-(int)bias,emax=(int)bias;
    if(width!=32u && width!=64u)return B86_X87_DEC_UNSUPPORTED;
    if(c==B86_X87_UNSUPPORTED)return B86_X87_DEC_UNSUPPORTED;
    if(c==B86_X87_ZERO)bits=0u;
    else if(c==B86_X87_INFINITY)bits=(((UINT64_C(1)<<eb)-1u)<<(p-1u));
    else if(c==B86_X87_NAN){
        /* Signal invalid and quiet an SNaN; keep high payload. */
        invalid=(sig & UINT64_C(0x4000000000000000))==0u;
        bits=(((UINT64_C(1)<<eb)-1u)<<(p-1u)) |
             (((sig>>(64u-p)) & fracmask) | (UINT64_C(1)<<(p-2u)));
    } else {
        e=(exp?(int)exp-16383:-16382);
        if(exp==0u){
            unsigned shifts=0u;
            denorm=1u;
            while((sig>>63u)==0u){sig<<=1u;shifts++;}
            e-=(int)shifts;
        }
        if(e>emax){overflow=1u;inexact=1u;}
        else {
            unsigned shift=(64u-p)+(e<emin?(unsigned)(emin-e):0u);
            q=b86_x87_round_shr64(sig,shift,sign,rc,&inexact,&up);
            if(e>=emin && q==(UINT64_C(1)<<p)){q>>=1u;e++;}
            if(e>emax){overflow=1u;inexact=1u;}
            else if(e>=emin){bits=(((uint64_t)(e+(int)bias))<<(p-1u))|(q&fracmask);}
            else if(q>=(UINT64_C(1)<<(p-1u)))bits=UINT64_C(1)<<(p-1u); /* smallest normal */
            else {bits=q;underflow=inexact;}
        }
        if(overflow){
            unsigned infinity=(rc==0u)||(rc==1u && sign)||(rc==2u && !sign);
            bits=infinity?(((UINT64_C(1)<<eb)-1u)<<(p-1u)):
                         ((((UINT64_C(1)<<eb)-2u)<<(p-1u))|fracmask);
            up=infinity;
        }
    }
    if((invalid && !(f->control & 1u)) || (denorm && !(f->control & 2u)) ||
       (overflow && !(f->control & 8u)) || (underflow && !(f->control & 16u)) ||
       (inexact && !(f->control & 32u)))return B86_X87_DEC_UNSUPPORTED;
    if(invalid)f->status|=1u;
    if(denorm)f->status|=2u;
    if(overflow)f->status|=8u;
    if(underflow)f->status|=16u;
    if(inexact)f->status|=32u;
    f->status=(uint16_t)((f->status & (uint16_t)~0x0200u) | (up?0x0200u:0u));
    *outbits=bits|((uint64_t)sign<<(width-1u));
    return B86_X87_OK;
}
static inline int b86_x87_exec_mem_arith(B86X87 *f,unsigned op,unsigned modrm,
                                          uint32_t ea,const B86X87Memory *mem)
{
    B86X87 next;
    uint8_t raw[8]={0},lhs[10],rhs[10],out[10];
    unsigned sub=(modrm>>3)&7u,size;
    int rc;
    if(!f || !mem || !mem->read || modrm>=0xc0u)
        return B86_X87_DEC_UNSUPPORTED;
    size=(op==0xD8u||op==0xDAu)?4u:2u;
    if(op==0xDCu)size=8u;
    else if(op!=0xD8u&&op!=0xDAu&&op!=0xDEu)
        return B86_X87_DEC_UNSUPPORTED;
    next=*f;
    if(b86_x87_peek_raw(&next,0u,lhs)!=B86_X87_OK)
        return B86_X87_DEC_STACK;
    if(mem->read(mem->opaque,ea,raw,size))return B86_X87_MEM_FAULT;
    if(op==0xD8u)b86_x87_from_f32_bits(rhs,b86_x87_le32(raw));
    else if(op==0xDCu)b86_x87_from_f64_bits(rhs,b86_x87_le64(raw));
    else if(op==0xDAu)b86_x87_from_i64(rhs,(int64_t)(int32_t)b86_x87_le32(raw));
    else {
        uint16_t u=(uint16_t)((unsigned)raw[0]|((unsigned)raw[1]<<8));
        int32_t signed_value=(u&0x8000u)?(int32_t)u-65536:(int32_t)u;
        b86_x87_from_i64(rhs,(int64_t)signed_value);
    }
    if(sub==2u||sub==3u){
        rc=b86_x87_compare_value(&next,rhs,sub==3u,0);
        if(rc)return rc;
    } else {
        if(sub==0u)rc=b86_x87_add_rne(out,lhs,rhs,0);
        else if(sub==1u)rc=b86_x87_mul_rne(out,lhs,rhs);
        else if(sub==4u)rc=b86_x87_add_rne(out,lhs,rhs,1);
        else if(sub==5u)rc=b86_x87_add_rne(out,rhs,lhs,1);
        else if(sub==6u)rc=b86_x87_div_masked_or_rne(&next,out,lhs,rhs);
        else rc=b86_x87_div_masked_or_rne(&next,out,rhs,lhs);
        if(rc)return B86_X87_DEC_UNSUPPORTED;
        memcpy(next.reg[b86_x87_phys(&next,0u)],out,10u);
        b86_x87_set_tag(&next,b86_x87_phys(&next,0u),b86_x87_full_tag(out));
    }
    *f=next;
    return B86_X87_OK;
}
static inline int b86_x87_exec_mem(B86X87 *f,unsigned op,unsigned modrm,
                                   uint32_t ea,const B86X87Memory *mem){
    B86X87 next;
    uint8_t data[10]={0}, value[10]={0};
    unsigned sub=(modrm>>3)&7u, size=0u, do_read=1u;
    int rc;
    if(!f||!mem||modrm>=0xC0u||op<0xD8u||op>0xDFu)
        return B86_X87_DEC_UNSUPPORTED;
    if(op==0xD8u||op==0xDAu||op==0xDCu||op==0xDEu)
        return b86_x87_exec_mem_arith(f,op,modrm,ea,mem);
    if((op==0xD9u && sub==0u)||(op==0xDBu && (sub==0u||sub==5u||sub==7u)))
        size=op==0xD9u?4u:sub==0u?4u:10u;
    else if(op==0xDDu && (sub==0u||sub==2u||sub==3u))size=8u;
    else if(op==0xDFu&&(sub==0u||sub==5u))size=sub==0u?2u:8u;
    else if((op==0xD9u&&(sub==5u||sub==7u))||(op==0xDDu&&sub==7u))size=2u;
    else if(op==0xD9u&&(sub==2u||sub==3u))size=4u;
    else return B86_X87_DEC_UNSUPPORTED;
    do_read=!((op==0xDBu&&sub==7u)||(op==0xD9u&&(sub==2u||sub==3u||sub==7u))||(op==0xDDu&&(sub==2u||sub==3u||sub==7u)));
    if(do_read&&!mem->read)return B86_X87_MEM_FAULT;
    if(!do_read&&!mem->write)return B86_X87_MEM_FAULT;
    next=*f;
    /* For stores, check stack preconditions before invoking host callback. */
    if(op==0xDBu&&sub==7u){
        if(b86_x87_peek_raw(&next,0,data))return B86_X87_DEC_STACK;
        if(b86_x87_pop(&next))return B86_X87_DEC_STACK;
    }else if((op==0xD9u||op==0xDDu)&&(sub==2u||sub==3u)){
        uint64_t bits;
        if(b86_x87_peek_raw(&next,0,value))return B86_X87_DEC_STACK;
        rc=b86_x87_to_ieee_bits(&next,value,op==0xD9u?32u:64u,&bits);
        if(rc)return rc;
        {unsigned k;for(k=0;k<size;k++)data[k]=(uint8_t)(bits>>(8u*k));}
        if(sub==3u && b86_x87_pop(&next))return B86_X87_DEC_STACK;
    }else if(op==0xD9u&&sub==7u)b86_x87_st16(data,next.control);
    else if(op==0xDDu&&sub==7u)b86_x87_st16(data,next.status);
    if(do_read){
        rc=mem->read(mem->opaque,ea,data,size);
        if(rc)return B86_X87_MEM_FAULT;
        if(op==0xD9u&&sub==5u)next.control=(uint16_t)((unsigned)data[0]|((unsigned)data[1]<<8));
        else {
            if(op==0xD9u&&sub==0u)b86_x87_from_f32_bits(value,b86_x87_le32(data));
            else if(op==0xDDu&&sub==0u)b86_x87_from_f64_bits(value,b86_x87_le64(data));
            else if(op==0xDBu&&sub==0u)b86_x87_from_i64(value,(int64_t)(int32_t)b86_x87_le32(data));
            else if(op==0xDFu&&sub==0u){
                uint16_t u=(uint16_t)((unsigned)data[0]|((unsigned)data[1]<<8));
                int32_t signed_value=(u&0x8000u)?(int32_t)u-65536:(int32_t)u;
                b86_x87_from_i64(value,(int64_t)signed_value);
            }else if(op==0xDFu&&sub==5u){
                uint64_t raw=b86_x87_le64(data);
                /* Converting unsigned bits to signed64 can be implementation-defined.
                   Compute negatives via -(magnitude-1)-1, guarding INT64_MIN. */
                int64_t signed_value;
                if(raw & UINT64_C(0x8000000000000000)){
                    uint64_t mag=(~raw)+UINT64_C(1);
                    signed_value=-(int64_t)(mag-UINT64_C(1))-INT64_C(1);
                } else signed_value=(int64_t)raw;
                b86_x87_from_i64(value,signed_value);
            }
            else memcpy(value,data,10);
            if(b86_x87_push_raw(&next,value,b86_x87_full_tag(value)))return B86_X87_DEC_STACK;
        }
    } else {
        rc=mem->write(mem->opaque,ea,data,size);
        if(rc)return B86_X87_MEM_FAULT;
    }
    *f=next;
    return B86_X87_OK;
}
#endif

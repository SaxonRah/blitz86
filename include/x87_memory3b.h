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
static inline int b86_x87_exec_mem(B86X87 *f,unsigned op,unsigned modrm,
                                   uint32_t ea,const B86X87Memory *mem){
    B86X87 next;
    uint8_t data[10]={0}, value[10]={0};
    unsigned sub=(modrm>>3)&7u, size=0u, do_read=1u;
    int rc;
    if(!f||!mem||modrm>=0xC0u||op<0xD8u||op>0xDFu)
        return B86_X87_DEC_UNSUPPORTED;
    if((op==0xD9u && sub==0u)||(op==0xDBu && (sub==0u||sub==3u||sub==5u)))
        size=op==0xD9u?4u:sub==0u?4u:10u;
    else if(op==0xDDu && sub==0u)size=8u;
    else if(op==0xDFu&&(sub==0u||sub==5u))size=sub==0u?2u:8u;
    else if((op==0xD9u&&(sub==5u||sub==7u))||(op==0xDDu&&sub==7u))size=2u;
    else return B86_X87_DEC_UNSUPPORTED;
    do_read=!((op==0xDBu&&sub==3u)||(op==0xD9u&&sub==7u)||(op==0xDDu&&sub==7u));
    if(do_read&&!mem->read)return B86_X87_MEM_FAULT;
    if(!do_read&&!mem->write)return B86_X87_MEM_FAULT;
    next=*f;
    /* For stores, check stack preconditions before invoking host callback. */
    if(op==0xDBu&&sub==3u){
        if(b86_x87_peek_raw(&next,0,data))return B86_X87_DEC_STACK;
        if(b86_x87_pop(&next))return B86_X87_DEC_STACK;
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

/* BB_386_STAGE5C5G_V1: bounded arithmetic/branch execution after CS load.
 * Cold interpreter only; no protected JIT, paging or interrupt support.
 * All code fetches/branch targets are validated against the hidden CS cache.
 */
#include "b86_pm5.h"
#ifndef B86_PM5_FLASH
#define B86_PM5_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

/* Return 1 when condition-code nibble is true for already-materialized flags. */
static B86_PM5_FLASH int b86_pm5_condition(uint32_t f, unsigned cc)
{
    int v;
    switch (cc >> 1) {
    case 0: v = !!(f & B86_OF); break;
    case 1: v = !!(f & B86_CF); break;
    case 2: v = !!(f & B86_ZF); break;
    case 3: v = !!(f & (B86_CF | B86_ZF)); break;
    case 4: v = !!(f & B86_SF); break;
    case 5: v = !!(f & B86_PF); break;
    case 6: v = !!(!!(f & B86_SF) != !!(f & B86_OF)); break;
    default: v = !!((f & B86_ZF) || (!!(f & B86_SF) != !!(f & B86_OF))); break;
    }
    return (cc & 1u) ? !v : v;
}

/* Form arithmetic flags exactly for 8/16/32-bit ADD, SUB and CMP.
 * Caller commits flags and destination only after full fetch preflight.
 */
static B86_PM5_FLASH uint32_t b86_pm5_arith_flags(uint32_t flags,
                              uint32_t a, uint32_t b, unsigned width,
                              int subtract, uint32_t *result)
{
    const uint32_t mask= width==4u ? UINT32_MAX : ((1u << (width*8u))-1u);
    const uint32_t sign= (uint32_t)1u << (width*8u-1u);
    uint32_t r, out=0;
    a &= mask; b &= mask;
    if (subtract) {
        r=(a-b)&mask;
        if (a<b) out|=B86_CF;
        if (((a^b)&(a^r)&sign)!=0) out|=B86_OF;
    } else {
        uint64_t sum=(uint64_t)a+(uint64_t)b;
        r=(uint32_t)sum & mask;
        if (sum>mask) out|=B86_CF;
        if (((~(a^b))&(a^r)&sign)!=0) out|=B86_OF;
    }
    if ((a^b^r)&16u) out|=B86_AF;
    if (r==0u) out|=B86_ZF;
    if (r&sign) out|=B86_SF;
    if (b86_parity[r&0xFFu]) out|=B86_PF;
    *result=r;
    return (flags & ~(uint32_t)B86_ARITH)|out;
}

B86_PM5_FLASH int b86_pm5_step(B86Cpu *c)
{
    uint32_t start,next,offset=0u,old,imm=0u,result=0u,flags=0u,linear;
    uint8_t op,b,op2=0u; /* DESCENT_PM16_JCC_V2 */
    B86PM2Fault fault;
    unsigned i,width=0u,reg=0u,bytes=0u,kind=0u;
    int code32,pref66=0,chosen=0;
    if (!c || c->model!=B86_CPU_386 || !c->mem || !(c->msw&1u) ||
        c->shutdown386) return b86_pm4_step(c);
    if (c->pm386.seg[B86_CS].reserved == B86_PM3_TRANSITION_MARKER)
        return b86_pm4_step(c);
    if (!c->pm386.seg[B86_CS].valid ||
        !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS]&3u)!=0u || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags & 4u);
    if (!code32 && c->ip>0xFFFFu) return B86_UNSUPPORTED;
    start=next=c->ip;
    if (!b86_pm4_byte(c,&next,start,&op)) return B86_UNSUPPORTED;
    if (op==0x66u) {
        pref66=1;
        if (!b86_pm4_byte(c,&next,start,&op)) return B86_UNSUPPORTED;
        if (op==0x66u) return B86_UNSUPPORTED;
    }
    /* DESCENT_PM16_JCC_V2: decode the second opcode only for 0F 80..8F.
     * Never commit the architectural IP while probing; PM4 sees the same
     * unchanged state on fallback. Every opcode/operand byte is checked
     * against the cached executable CS descriptor via b86_pm4_byte.
     */
    if (op==0x0Fu) {
        if (!b86_pm4_byte(c,&next,start,&op2)) return B86_UNSUPPORTED;
        if (op2<0x80u || op2>0x8Fu) return b86_pm4_step(c);
    }
    /* The new subset has no segment override, address override, LOCK/REP
     * or ModR/M memory forms. PM4 handles its own prior instruction set. */
    if ((op==0x04u || op==0x05u || op==0x2Cu || op==0x2Du ||
         op==0x3Cu || op==0x3Du) ||
        (op>=0x70u && op<=0x7Fu) || (op==0x0Fu && op2>=0x80u && op2<=0x8Fu) ||
        op==0xEBu || op==0xE9u ||
        (op>=0x40u && op<=0x4Fu)) chosen=1;
    if (!chosen) return b86_pm4_step(c);
    /* In the 386 cold interpreter no arithmetic lazy producer can survive
     * entry: do not silently branch on stale non-materialized flags. */
    if (c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE) return B86_UNSUPPORTED;
    if (op>=0x70u && op<=0x7Fu) {
        if (pref66) return B86_UNSUPPORTED;
        bytes=1u; kind=3u;
    } else if (op==0x0Fu) { /* 0F 80..8F, near conditional branch */
        /* Conservative PM16 scope: 66h rel32 needs wider EIP handling in
         * other 16-bit-CS paths. Refuse rather than silently truncating. */
        if (!code32 && pref66) return B86_UNSUPPORTED;
        bytes=(code32^pref66)?4u:2u; kind=3u;
    } else if (op==0xEBu) {
        if (pref66) return B86_UNSUPPORTED;
        bytes=1u; kind=4u;
    } else if (op==0xE9u) {
        /* rel32 in a 16-bit CS may set high EIP: PM4 cannot execute it. */
        if (!code32 && pref66) return B86_UNSUPPORTED;
        bytes=(code32^pref66)?4u:2u; kind=4u;
    } else if (op>=0x40u && op<=0x4Fu) {
        bytes=0u; width=(code32^pref66)?4u:2u;
        kind=op<0x48u?5u:6u; reg=op&7u;
    } else {
        if ((op&1u)==0u && pref66) return B86_UNSUPPORTED;
        width=(op&1u)?((code32^pref66)?4u:2u):1u;
        bytes=width;
        if (op==0x04u || op==0x05u) kind=1u;
        else if (op==0x2Cu || op==0x2Du) kind=2u;
        else kind=7u;
    }
    for (i=0;i<bytes;i++) {
        if (!b86_pm4_byte(c,&next,start,&b)) return B86_UNSUPPORTED;
        imm|=(uint32_t)b<<(8u*i);
    }
    if (!code32) next&=0xFFFFu;
    if (kind==3u || kind==4u) {
        int32_t disp = bytes==1u ? (int8_t)imm :
                       bytes==2u ? (int16_t)imm : (int32_t)imm;
        if (kind==3u && !b86_pm5_condition(c->flags,
                                               op==0x0Fu?(op2&15u):(op&15u))) {
            c->ip=next; ++c->icount; return B86_OK;
        }
        /* rel16 branch operand-size truncates target; rel8 inherits CS D. */
        offset=next+(uint32_t)disp;
        if (bytes==2u && (op==0xE9u || op==0x0Fu)) offset&=0xFFFFu;
        if (!code32) offset&=0xFFFFu;
        if (b86_pm2_check_access(c,B86_CS,offset,1u,B86_PM1_EXEC,
                                 &linear,&fault)!=B86_PM2_OK) return B86_UNSUPPORTED;
        c->ip=offset; ++c->icount; return B86_OK;
    }
    old=c->r[reg];
    if (width==1u) old &=0xFFu;
    else if (width==2u) old &=0xFFFFu;
    if (kind==5u || kind==6u) imm=1u;
    flags=b86_pm5_arith_flags(c->flags,old,imm,width,
                              kind==2u || kind==6u || kind==7u,&result);
    /* INC/DEC retain CF rather than using the discarded carry/borrow. */
    if (kind==5u || kind==6u)
        flags=(flags&~(uint32_t)B86_CF)|(c->flags&B86_CF);
    if (kind!=7u) {
        const uint32_t mask=width==4u?UINT32_MAX:(width==2u?0xFFFFu:0xFFu);
        c->r[reg]=(c->r[reg]&~mask)|result;
    }
    c->flags=flags;
    c->ip=next;
    ++c->icount;
    return B86_OK;
}

#ifndef B86_X87_DECODE3A_H
#define B86_X87_DECODE3A_H
/* Standalone strictly gated x87 register-only instruction executor.
   No CPU hooks, guest memory or exceptions. Unhandled instructions NEVER
   advance EIP or modify FPU state. This is not a full x87 implementation. */
#include "x87_arith.h"
#include "x87_muldiv.h"
enum { B86_X87_DEC_UNSUPPORTED=-20, B86_X87_DEC_STACK=-21 };
/* Stage 4P: narrowly implement masked x87 division with zero denominator.
 * Existing finite/finite arithmetic is unchanged. Only a *normal finite*
 * nonzero numerator divided by zero, or zero divided by zero, is handled
 * here. Other special values/denormals still reject with no state change.
 * An unmasked exception must eventually be delivered as #MF; until the
 * exception-delivery bridge exists, refuse the instruction atomically.
 * This routine is called on the transaction's TEMPORARY state, not live FPU.
 */
static inline int b86_x87_div_masked_or_rne(B86X87 *f,uint8_t out[10],
                                              const uint8_t n[10],const uint8_t d[10]) {
    unsigned cn=b86_x87_class(n),cd=b86_x87_class(d);
    if(cd==B86_X87_ZERO) {
        if(cn==B86_X87_ZERO) {
            /* 0 / 0: invalid operation, masked result = x87 indefinite QNaN. */
            if(!(f->control&0x0001u))return B86_X87_ARITH_UNSUPPORTED;
            b86_x87_pack(out,1u,32767u,UINT64_C(0xC000000000000000));
            f->status|=0x0001u; /* IE; ES remains clear while masked */
            return 0;
        }
        if(cn==B86_X87_FINITE && b86_x87_exp(n)!=0u) {
            /* Nonzero normal finite / signed zero: masked #Z result infinity. */
            if(!(f->control&0x0004u))return B86_X87_ARITH_UNSUPPORTED;
            b86_x87_pack(out,b86_x87_sign(n)^b86_x87_sign(d),32767u,
                         UINT64_C(0x8000000000000000));
            f->status|=0x0004u; /* ZE; no exception summary when masked */
            return 0;
        }
        return B86_X87_ARITH_UNSUPPORTED;
    }
    return b86_x87_div_rne(out,n,d);
}

/* Stage 4Q: x87 compare/status subset, all changes transactional.
 * C0/C2/C3 indicate less/unordered/equal; C1 clears on a completed compare.
 * FCOM is ordered (all NaNs signal IE); FUCOM is quiet for QNaNs, not SNaNs.
 * Masked exceptions set sticky status and return unordered; the core's #MF
 * delivery isn't complete, so unmasked exceptions must decline without
 * modifying any guest FPU state (the caller uses an isolated temporary copy).
 * Empty-register architectural stack faults are still unsupported/atomic.
 */
/* Compare a decoded memory value against ST(0), shared with register
 * comparisons. The caller runs this on a temporary x87 state. */
static inline int b86_x87_compare_value(B86X87 *f,const uint8_t rhs[10],
                                        unsigned pops,int quiet)
{
    uint8_t lhs[10];
    unsigned ca,cb,nana,nanb,invalid=0u,denorm=0u,cc=0u;
    int cmp;
    if(!f || !rhs || pops>2u) return B86_X87_DEC_UNSUPPORTED;
    if(b86_x87_peek_raw(f,0u,lhs)!=B86_X87_OK) return B86_X87_DEC_STACK;
    ca=b86_x87_class(lhs);cb=b86_x87_class(rhs);
    /* Do not mistake unnormal/pseudo-denormal encodings for ordered values. */
    if(ca==B86_X87_UNSUPPORTED || cb==B86_X87_UNSUPPORTED)
        return B86_X87_DEC_UNSUPPORTED;
    nana=(ca==B86_X87_NAN);nanb=(cb==B86_X87_NAN);
    if(nana || nanb){
        unsigned signal_a=nana && ((b86_x87_significand(lhs)&UINT64_C(0x4000000000000000))==0u);
        unsigned signal_b=nanb && ((b86_x87_significand(rhs)&UINT64_C(0x4000000000000000))==0u);
        invalid=(!quiet || signal_a || signal_b);
        cc=0x4500u; /* unordered: C3|C2|C0 */
    } else {
        cmp=b86_x87_compare(lhs,rhs);
        if(cmp==-2) return B86_X87_DEC_UNSUPPORTED;
        cc=cmp<0?0x0100u:cmp==0?0x4000u:0u;
    }
    denorm=((ca==B86_X87_FINITE && b86_x87_exp(lhs)==0u) ||
            (cb==B86_X87_FINITE && b86_x87_exp(rhs)==0u));
    if((invalid && !(f->control&0x0001u)) ||
       (denorm && !(f->control&0x0002u)))
        return B86_X87_DEC_UNSUPPORTED;
    f->status=(uint16_t)((f->status & (uint16_t)~0x4700u) | cc);
    if(invalid)f->status|=0x0001u; /* IE, sticky */
    if(denorm)f->status|=0x0002u; /* DE, sticky */
    while(pops--){
        if(b86_x87_pop(f)!=B86_X87_OK)return B86_X87_DEC_STACK;
    }
    return B86_X87_OK;
}
static inline int b86_x87_compare_reg(B86X87 *f,unsigned other,unsigned pops,
                                      int quiet,int against_zero)
{
    uint8_t rhs[10];
    if(!f || pops>2u || other>=8u)return B86_X87_DEC_UNSUPPORTED;
    if(against_zero)b86_x87_pack(rhs,0u,0u,UINT64_C(0));
    else if(b86_x87_peek_raw(f,other,rhs)!=B86_X87_OK)return B86_X87_DEC_STACK;
    return b86_x87_compare_value(f,rhs,pops,quiet);
}
/* Register form DF E0 is special: the result goes in CPU AX (not ST(i)).
 * Only the protected-mode CPU integration layer can write EAX; this pure
 * helper exports SW so the main decoder and CPU bridge share one definition. */
static inline int b86_x87_fnstsw_ax(const B86X87 *f,unsigned op,
                                    unsigned modrm,uint16_t *ax)
{
    if(!f || !ax || op!=0xDFu || modrm!=0xE0u)
        return B86_X87_DEC_UNSUPPORTED;
    *ax=f->status;
    return B86_X87_OK;
}

/* Stage 4S: FRNDINT D9 FC. Convert ST(0) to an integral extended-real
 * value, honoring the 80387 RC bits independently of precision control.
 * Pure 64-bit integer bit arithmetic; no host floating point or libm.
 * Pending unmasked #P/#D/#I is declined transactionally until #MF exists.
 */
static inline int b86_x87_frndint(B86X87 *f) {
    uint8_t in[10],result[10];
    unsigned cls, exp, sign, rc, rounded_up=0u, inexact=0u, denorm=0u;
    uint64_t sig, whole=0u, rem=0u;
    if(!f || b86_x87_peek_raw(f,0u,in)!=B86_X87_OK)
        return B86_X87_DEC_STACK;
    cls=b86_x87_class(in);
    exp=b86_x87_exp(in); sign=b86_x87_sign(in);
    sig=b86_x87_significand(in);
    rc=(f->control>>10)&3u;
    if(cls==B86_X87_UNSUPPORTED)
        return B86_X87_DEC_UNSUPPORTED;
    memcpy(result,in,sizeof(result));
    if(cls==B86_X87_NAN) {
        /* Quiet signaling NaN if the invalid exception is masked. */
        if((sig & UINT64_C(0x4000000000000000))==0u) {
            if((f->control & 1u)==0u) return B86_X87_DEC_UNSUPPORTED;
            sig |= UINT64_C(0x4000000000000000);
            b86_x87_pack(result,sign,32767u,sig);
            f->status |= 0x0001u;
        }
    } else if(cls==B86_X87_FINITE) {
        denorm=(exp==0u);
        if(exp>=16446u) { /* All bits of the 64-bit significand are integral. */
        } else {
            inexact=1u;
            if(exp>=16383u) {
                unsigned shift=16446u-exp; /* 1 .. 63 */
                uint64_t mask=(UINT64_C(1)<<shift)-1u;
                whole=sig>>shift;
                rem=sig&mask;
                inexact=(rem!=0u);
                if(inexact) {
                    if(rc==0u) { /* nearest, midpoint ties to even */
                        uint64_t half=UINT64_C(1)<<(shift-1u);
                        rounded_up=(rem>half)||(rem==half && (whole&1u)!=0u);
                    } else if(rc==1u) rounded_up=sign;
                    else if(rc==2u) rounded_up=!sign;
                }
            } else { /* Nonzero |x|<1.0, including 80-bit subnormals. */
                whole=0u;
                if(rc==0u) {
                    rounded_up=(exp==16382u &&
                        sig>UINT64_C(0x8000000000000000));
                } else if(rc==1u) rounded_up=sign;
                else if(rc==2u) rounded_up=!sign;
            }
            if(inexact) {
                unsigned leading=0u;
                if(whole+rounded_up==0u) {
                    b86_x87_pack(result,sign,0u,0u);
                } else {
                    uint64_t n=whole+rounded_up;
                    uint64_t t=n;
                    while(t>>1u) {t>>=1u;leading++;}
                    b86_x87_pack(result,sign,16383u+leading,n<<(63u-leading));
                }
            }
        }
        /* #D applies to a true x87 denormal operand; #P to rounding. */
        if(denorm && (f->control&0x0002u)==0u)
            return B86_X87_DEC_UNSUPPORTED;
        if(inexact && (f->control&0x0020u)==0u)
            return B86_X87_DEC_UNSUPPORTED;
    }
    /* C1 is set when the magnitude was increased, even for negative values.
     * FRNDINT does not modify C0, C2, or C3. */
    f->status=(uint16_t)((f->status & (uint16_t)~0x0200u)|
               (rounded_up?0x0200u:0u)|
               (denorm?0x0002u:0u)|(inexact?0x0020u:0u));
    memcpy(f->reg[b86_x87_phys(f,0u)],result,10);
    b86_x87_set_tag(f,b86_x87_phys(f,0u),b86_x87_full_tag(result));
    return B86_X87_OK;
}

static inline int b86_x87_exec_reg(B86X87 *f, unsigned op, unsigned modrm) {
    B86X87 temp;
    uint8_t a[10],b[10],out[10];
    unsigned i=modrm&7u, sub=(modrm>>3)&7u, dest;
    int rc,kind=-1;
    if(!f || op<0xD8u || op>0xDFu || modrm<0xC0u || modrm>255u)
        return B86_X87_DEC_UNSUPPORTED;
    temp=*f;
    /* All changes happen in temp. Only commit when operation succeeds. */
    if(op==0xD9u && sub==0u) { /* FLD ST(i) */
        if(b86_x87_peek_raw(&temp,i,a))return B86_X87_DEC_STACK;
        rc=b86_x87_push_raw(&temp,a,b86_x87_full_tag(a));
    } else if(op==0xD9u && sub==1u) { /* FXCH ST(i) */
        rc=b86_x87_fxch(&temp,i);
    } else if(op==0xD9u && modrm>=0xE8u && modrm<=0xEEu) {
        /* FLD1 (D9 E8), FLDZ (D9 EE): exact 80-bit constants.
         * Push through the regular stack/tag checks; a full stack cannot
         * modify architectural state because this function commits temp
         * only after success. Neither constant changes exception flags. */
        static const uint64_t k[5]={
            UINT64_C(0xD49A784BCD1B8AFE), /* FLDL2T */
            UINT64_C(0xB8AA3B295C17F0BC), /* FLDL2E */
            UINT64_C(0xC90FDAA22168C235), /* FLDPI  */
            UINT64_C(0x9A209A84FBCFF799), /* FLDLG2 */
            UINT64_C(0xB17217F7D1CF79AC)  /* FLDLN2 */
        };
        static const unsigned e[5]={16384u,16383u,16384u,16381u,16382u};
        if(modrm==0xE8u)b86_x87_from_i64(a,1);
        else if(modrm==0xEEu)b86_x87_pack(a,0u,0u,UINT64_C(0));
        else b86_x87_pack(a,0u,e[modrm-0xE9u],k[modrm-0xE9u]);
        rc=b86_x87_push_raw(&temp,a,modrm==0xEEu?1u:0u);
    } else if(op==0xD9u && modrm==0xD0u) { /* FNOP */
        rc=0;
    } else if(op==0xD9u && (modrm==0xF6u||modrm==0xF7u)) { /* FDECSTP/FINCSTP */
        b86_x87_set_top(&temp,(b86_x87_top(&temp)+(modrm==0xF6u?7u:1u))&7u);
        temp.status &= (uint16_t)~0x0200u;rc=0;
    } else if(op==0xDFu && sub==0u) { /* FFREEP ST(i) */
        unsigned freed=b86_x87_phys(&temp,i);
        if(b86_x87_tag(&temp,b86_x87_top(&temp))==3u)return B86_X87_DEC_STACK;
        rc=b86_x87_pop(&temp);
        if(!rc)b86_x87_set_tag(&temp,freed,3u);
    } else if(op==0xDDu && sub==2u) { /* FST ST(i) */
        if(b86_x87_peek_raw(&temp,0,a))return B86_X87_DEC_STACK;
        dest=b86_x87_phys(&temp,i);
        memcpy(temp.reg[dest],a,10);b86_x87_set_tag(&temp,dest,b86_x87_full_tag(a));
        rc=0;
    } else if(op==0xDDu && sub==3u) { /* FSTP ST(i) */
        if(b86_x87_peek_raw(&temp,0,a))return B86_X87_DEC_STACK;
        dest=b86_x87_phys(&temp,i);
        memcpy(temp.reg[dest],a,10);b86_x87_set_tag(&temp,dest,b86_x87_full_tag(a));
        rc=b86_x87_pop(&temp);
    } else if(op==0xDDu && sub==0u) { /* FFREE ST(i) */
        b86_x87_ffree(&temp,i);rc=0;
    } else if(op==0xD9u && modrm==0xE0u) { /* FCHS */
        if(b86_x87_peek_raw(&temp,0,a))return B86_X87_DEC_STACK;
        b86_x87_fchs(temp.reg[b86_x87_phys(&temp,0)]);rc=0;
    } else if(op==0xD9u && modrm==0xE1u) { /* FABS */
        if(b86_x87_peek_raw(&temp,0,a))return B86_X87_DEC_STACK;
        b86_x87_fabs(temp.reg[b86_x87_phys(&temp,0)]);rc=0;
    } else if(op==0xD8u && (sub==2u || sub==3u)) { /* FCOM/FCOMP ST(i) */
        rc=b86_x87_compare_reg(&temp,i,sub==3u?1u:0u,0,0);
    } else if(op==0xDDu && (sub==4u || sub==5u)) { /* FUCOM/FUCOMP ST(i) */
        rc=b86_x87_compare_reg(&temp,i,sub==5u?1u:0u,1,0);
    } else if(op==0xDEu && modrm==0xD9u) { /* FCOMPP: two pops */
        rc=b86_x87_compare_reg(&temp,1u,2u,0,0);
    } else if(op==0xDAu && modrm==0xE9u) { /* FUCOMPP: two pops */
        rc=b86_x87_compare_reg(&temp,1u,2u,1,0);
    } else if(op==0xD9u && modrm==0xFCu) { /* FRNDINT per control RC */
        rc=b86_x87_frndint(&temp);
    } else if(op==0xD9u && modrm==0xE4u) { /* FTST: compare ST(0) with +0 */
        rc=b86_x87_compare_reg(&temp,0u,0u,0,1);
    } else if(op==0xDBu && modrm==0xE2u) { /* FNCLEX */
        b86_x87_fclex(&temp);rc=0;
    } else if(op==0xDBu && modrm==0xE3u) { /* FNINIT */
        b86_x87_init(&temp);rc=0;
    } else if(op==0xD8u && (sub==0u||sub==1u||sub==4u||sub==5u||sub==6u||sub==7u)) {
        /* ST(0) = ST(0) (+, *, -, reversed -, /, reversed /) ST(i) */
        if(b86_x87_peek_raw(&temp,0,a)||b86_x87_peek_raw(&temp,i,b))return B86_X87_DEC_STACK;
        kind=(int)sub;
        if(kind==0)rc=b86_x87_add_rne(out,a,b,0);
        else if(kind==1)rc=b86_x87_mul_rne(out,a,b);
        else if(kind==4)rc=b86_x87_add_rne(out,a,b,1);
        else if(kind==5)rc=b86_x87_add_rne(out,b,a,1);
        else if(kind==6)rc=b86_x87_div_masked_or_rne(&temp,out,a,b);
        else rc=b86_x87_div_masked_or_rne(&temp,out,b,a);
        if(rc)return B86_X87_DEC_UNSUPPORTED;
        memcpy(temp.reg[b86_x87_phys(&temp,0)],out,10);
        b86_x87_set_tag(&temp,b86_x87_phys(&temp,0),b86_x87_full_tag(out));
        rc=0;
    } else if(op==0xDEu && (sub==0u||sub==1u||sub==4u||sub==5u||sub==6u||sub==7u)) {
        /* ST(i) = ST(i) op ST(0), pop. */
        if(b86_x87_peek_raw(&temp,0,a)||b86_x87_peek_raw(&temp,i,b))return B86_X87_DEC_STACK;
        if(sub==0)rc=b86_x87_add_rne(out,b,a,0);
        else if(sub==1)rc=b86_x87_mul_rne(out,b,a);
        else if(sub==4)rc=b86_x87_add_rne(out,a,b,1); /* FSUBRP */
        else if(sub==5)rc=b86_x87_add_rne(out,b,a,1); /* FSUBP */
        else if(sub==6)rc=b86_x87_div_masked_or_rne(&temp,out,a,b);   /* FDIVRP */
        else rc=b86_x87_div_masked_or_rne(&temp,out,b,a); /* FDIVP */
        if(rc)return B86_X87_DEC_UNSUPPORTED;
        dest=b86_x87_phys(&temp,i);
        memcpy(temp.reg[dest],out,10);b86_x87_set_tag(&temp,dest,b86_x87_full_tag(out));
        rc=b86_x87_pop(&temp);
    } else return B86_X87_DEC_UNSUPPORTED;
    if(rc) return rc==B86_X87_DEC_UNSUPPORTED?B86_X87_DEC_UNSUPPORTED:B86_X87_DEC_STACK;
    *f=temp;
    return B86_X87_OK;
}
#endif

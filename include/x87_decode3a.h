#ifndef B86_X87_DECODE3A_H
#define B86_X87_DECODE3A_H
/* Standalone strictly gated x87 register-only instruction executor.
   No CPU hooks, guest memory or exceptions. Unhandled instructions NEVER
   advance EIP or modify FPU state. This is not a full x87 implementation. */
#include "x87_arith.h"
#include "x87_muldiv.h"
enum { B86_X87_DEC_UNSUPPORTED=-20, B86_X87_DEC_STACK=-21 };
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
        else if(kind==6)rc=b86_x87_div_rne(out,a,b);
        else rc=b86_x87_div_rne(out,b,a);
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
        else if(sub==6)rc=b86_x87_div_rne(out,a,b);   /* FDIVRP */
        else rc=b86_x87_div_rne(out,b,a);             /* FDIVP */
        if(rc)return B86_X87_DEC_UNSUPPORTED;
        dest=b86_x87_phys(&temp,i);
        memcpy(temp.reg[dest],out,10);b86_x87_set_tag(&temp,dest,b86_x87_full_tag(out));
        rc=b86_x87_pop(&temp);
    } else return B86_X87_DEC_UNSUPPORTED;
    if(rc)return B86_X87_DEC_STACK;
    *f=temp;
    return B86_X87_OK;
}
#endif

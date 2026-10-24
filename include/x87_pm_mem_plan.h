#ifndef B86_X87_PM_MEM_PLAN_H
#define B86_X87_PM_MEM_PLAN_H
#include <stdint.h>
/* A strict, pure classification pass before the PM2 access check. */
typedef struct B86X87MemPlan { unsigned size, write; } B86X87MemPlan;
static inline int b86_x87_mem_plan(unsigned op,unsigned modrm,B86X87MemPlan *p){
 unsigned s=(modrm>>3)&7u;
 if(!p||modrm>=0xc0u||op<0xd8u||op>0xdfu)return 0;
 p->size=0;p->write=0;
 switch(op){
 case 0xd8u: p->size=4u;break;
 case 0xdau: p->size=4u;break;
 case 0xdcu: p->size=8u;break;
 case 0xdeu: p->size=2u;break;
 case 0xd9u:
    if(s==0u) p->size=4u; /* FLD m32real */
    if(s==2u||s==3u){p->size=4u;p->write=1u;} /* FST/FSTP m32real */
    if(s==5u) p->size=2u; /* FLDCW */
    if(s==7u){p->size=2u;p->write=1u;} /* FNSTCW */
    break;
 case 0xdbu:
    if(s==0u) p->size=4u; /* FILD m32int */
    if(s==1u||s==2u||s==3u){p->size=4u;p->write=1u;} /* FISTTP/FIST/FISTP */
    if(s==5u) p->size=10u; /* FLD m80real */
    if(s==7u){p->size=10u;p->write=1u;} /* FSTP m80real */
    break;
 case 0xddu:
    if(s==0u) p->size=8u; /* FLD m64real */
    if(s==1u||s==2u||s==3u){p->size=8u;p->write=1u;} /* FISTTP/FST/FSTP */
    if(s==7u){p->size=2u;p->write=1u;} /* FNSTSW */
    break;
 case 0xdfu:
    if(s==0u) p->size=2u; /* FILD m16int */
    if(s==1u||s==2u||s==3u){p->size=2u;p->write=1u;} /* FISTTP/FIST/FISTP */
    if(s==5u) p->size=8u; /* FILD m64int */
    if(s==7u){p->size=8u;p->write=1u;} /* FISTP m64int */
    break;
 default:break;
 }
 return p->size!=0;
}
#endif

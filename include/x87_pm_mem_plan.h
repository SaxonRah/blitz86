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
 case 0xd9:if(s==0u)p->size=4;
           else if(s==5u)p->size=2;
           else if(s==7u){p->size=2;p->write=1;}break;
 case 0xdd:if(s==0u)p->size=8;
           else if(s==7u){p->size=2;p->write=1;}
           else if(s==1u||s==7u){}break;
 case 0xdb:if(s==0u)p->size=4;
           else if(s==5u)p->size=10;
           else if(s==3u){p->size=10;p->write=1;}
           else if(s==1u||s==2u){p->size=4;p->write=1;}break;
 case 0xdf:if(s==0u)p->size=2;
           else if(s==5u)p->size=8;
           else if(s==1u||s==2u||s==3u){p->size=2;p->write=1;}
           else if(s==7u){p->size=8;p->write=1;}break;
 default:break;
 }
 if(op==0xdd && s==1u){p->size=8;p->write=1;}
 return p->size!=0;
}
#endif

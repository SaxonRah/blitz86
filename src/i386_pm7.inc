/* BB_386_STAGE5C5I_V1: CPL0 protected near stack/flow, cold interpreter.
 * No extension of the hot CPU/JIT ABI and no real-mode behavior change.
 */
#include "b86_pm7.h"
#ifndef B86_PM7_FLASH
#define B86_PM7_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

static B86_PM7_FLASH uint32_t b86_pm7_stack_add(uint32_t esp, int ss32,
                                                 uint32_t amount)
{
    return ss32 ? esp+amount : (esp & 0xFFFF0000u) | ((esp+amount)&0xFFFFu);
}

/* Execute only after PM3's initial far transfer installed the protected CS.
 * Fall back to PM6 for all opcodes not handled here. Both the stack and
 * near branches use cached descriptor bases, never selector*16.
 */
B86_PM7_FLASH int b86_pm7_step(B86Cpu *c)
{
    uint32_t start, ip, imm=0, old_sp, new_sp=0, target=0, linear=0;
    uint32_t old_ip=0, pushval=0, popped=0;
    B86PM2Fault fault;
    unsigned k, width, nbytes=0, reg=0, kind=0;
    uint8_t op, b;
    int code32, op32, ss32, pref66=0;
    if (!c || c->model!=B86_CPU_386 || !c->mem || !(c->msw&1u) ||
        c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER)
        return b86_pm6_step(c);
    if (!c->pm386.seg[B86_CS].valid ||
        !(c->pm386.seg[B86_CS].access&8u) ||
        (c->seg[B86_CS]&3u)!=0u || c->ip>UINT32_MAX-15u)
        return B86_UNSUPPORTED;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if (!code32 && c->ip>0xFFFFu) return B86_UNSUPPORTED;
    start=ip=c->ip;
    if (!b86_pm4_byte(c,&ip,start,&op)) return B86_UNSUPPORTED;
    if (op==0x66u) {
        pref66=1;
        if (!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if (op==0x66u)return B86_UNSUPPORTED;
    }
    if (op>=0x50u && op<=0x57u)kind=1;          /* PUSH r */
    else if (op>=0x58u && op<=0x5Fu)kind=2;     /* POP r */
    else if (op==0x68u || op==0x6Au)kind=3;     /* PUSH imm */
    else if (op==0xE8u)kind=4;                  /* CALL relative */
    else if (op==0xC3u || op==0xC2u)kind=5;    /* near RET */
    if (!kind) return b86_pm6_step(c);
    op32=code32^pref66;
    width=op32?4u:2u;
    /* The 16-bit code subset cannot execute 32-bit EIP. PM4/PM5 enforce
     * the same conservative restriction on oversized near transfers. */
    if (!code32 && op32 && (kind==4u || kind==5u))return B86_UNSUPPORTED;
    if (kind==4u)nbytes=width;
    else if (kind==3u)nbytes=(op==0x6Au)?1u:width;
    else if (kind==5u && op==0xC2u)nbytes=2u;
    for(k=0;k<nbytes;k++) {
        if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        imm|=(uint32_t)b << (8u*k);
    }
    old_ip=code32?ip:(ip&0xFFFFu);
    old_sp=c->r[B86_SP];
    ss32=!!(c->pm386.seg[B86_SS].flags&4u);
    if (kind==1u || kind==3u || kind==4u) {
        if (kind==1u) {reg=op&7u;pushval=b86_pm6_reg(c,reg,width);}
        else if (kind==3u) {
            pushval=(op==0x6Au)?(uint32_t)(int32_t)(int8_t)imm:imm;
        } else {
            int32_t disp=width==2u?(int16_t)imm:(int32_t)imm;
            target=old_ip+(uint32_t)disp;
            if(width==2u)target&=0xFFFFu;
            if(!code32)target&=0xFFFFu;
            if(b86_pm2_check_access(c,B86_CS,target,1u,B86_PM1_EXEC,
                                    &linear,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            pushval=old_ip;
        }
        new_sp=b86_pm7_stack_add(old_sp,ss32,(uint32_t)(0u-width));
        if(b86_pm2_check_access(c,B86_SS,ss32?new_sp:(new_sp&0xFFFFu),
                                width,B86_PM1_WRITE,&linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        for(k=0;k<width;k++)
            *b86_host(c,linear+k)=(uint8_t)(pushval>>(8u*k));
        smc(c,linear,width);
        c->r[B86_SP]=new_sp;
        c->ip=kind==4u?target:old_ip;
    } else {
        if(b86_pm2_check_access(c,B86_SS,ss32?old_sp:(old_sp&0xFFFFu),
                                width,B86_PM1_READ,&linear,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        for(k=0;k<width;k++)
            popped|=(uint32_t)(*b86_host(c,linear+k))<<(8u*k);
        new_sp=b86_pm7_stack_add(old_sp,ss32,width);
        if(kind==5u) {
            target=width==2u?(popped&0xFFFFu):popped;
            if(!code32)target&=0xFFFFu;
            if(b86_pm2_check_access(c,B86_CS,target,1u,B86_PM1_EXEC,
                                    &linear,&fault)!=B86_PM2_OK)
                return B86_UNSUPPORTED;
            new_sp=b86_pm7_stack_add(new_sp,ss32,imm);
            c->r[B86_SP]=new_sp;
            c->ip=target;
        } else {
            reg=op&7u;
            c->r[B86_SP]=new_sp;
            /* POP SP/ESP writes the popped value after the increment. */
            b86_pm6_putreg(c,reg,width,popped);
            c->ip=old_ip;
        }
    }
    ++c->icount;
    return B86_OK;
}

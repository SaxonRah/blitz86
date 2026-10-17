/* BB_386_STAGE5C5F_V1: bounded post-far-jump protected-mode execution.
 * Fully cold 386-only path. Real-mode decode/JIT remain untouched.
 * Safe failures are UNSUPPORTED until protected exceptions are delivered.
 */
#include "b86_pm4.h"
#ifndef B86_PM4_FLASH
#define B86_PM4_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

/* Use descriptor-backed preflight for every fetched byte, including
 * prefixes/immediates. No offset wrap within an instruction. */
static B86_PM4_FLASH int b86_pm4_fetch(const B86Cpu *c, uint32_t off, uint8_t *v)
{
    uint32_t linear;
    B86PM2Fault fault;
    if (b86_pm2_check_access(c, B86_CS, off, 1u, B86_PM1_EXEC,
                             &linear, &fault) != B86_PM2_OK) return 0;
    *v = *b86_host(c, linear);
    return 1;
}

static B86_PM4_FLASH int b86_pm4_byte(const B86Cpu *c, uint32_t *ip,
                                      uint32_t start, uint8_t *v)
{
    if (*ip < start || *ip-start >= 15u || !b86_pm4_fetch(c, *ip, v))
        return 0;
    ++*ip;
    return 1;
}

B86_PM4_FLASH int b86_pm4_step(B86Cpu *c)
{
    B86PM2Fault fault;
    uint32_t linear=0u, start, next, addr=0u, value=0u;
    unsigned k, n, seg=B86_DS, reg, width=0u;
    int op32, ad32, code32, pref66=0, pref67=0, seg_override=0;
    uint8_t op, b;
    if (!c || c->model != B86_CPU_386 || !c->mem || !(c->msw & 1u) ||
        c->shutdown386) return B86_UNSUPPORTED;
    if (c->pm386.seg[B86_CS].reserved == B86_PM3_TRANSITION_MARKER)
        return b86_pm3_transition_step(c);
    if (!c->pm386.seg[B86_CS].valid ||
        !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS] & 3u) != 0u ||  /* CPL0 only in Stage5C5F */
        c->ip > UINT32_MAX-15u) return B86_UNSUPPORTED;
    code32 = (c->pm386.seg[B86_CS].flags & 4u) != 0u;
    if (!code32 && c->ip > 0xFFFFu) return B86_UNSUPPORTED;
    start=next=c->ip;
    for (;;) {
        if (!b86_pm4_byte(c,&next,start,&op)) return B86_UNSUPPORTED;
        if (op == 0x66u) { if (pref66) return B86_UNSUPPORTED;pref66=1;continue; }
        if (op == 0x67u) { if (pref67) return B86_UNSUPPORTED;pref67=1;continue; }
        if (op==0x26u || op==0x2Eu || op==0x36u || op==0x3Eu ||
            op==0x64u || op==0x65u) {
            if (seg_override) return B86_UNSUPPORTED;
            seg_override=1;
            switch(op){
            case 0x26u:seg=B86_ES;break;
            case 0x2Eu:seg=B86_CS;break;
            case 0x36u:seg=B86_SS;break;
            case 0x3Eu:seg=B86_DS;break;
            case 0x64u:seg=B86_FS;break;
            default:seg=B86_GS;break;
            }
            continue;
        }
        break;
    }
    op32 = code32 ^ pref66;
    ad32 = code32 ^ pref67;
    /* Instructions without a data operand cannot consume size/segment
     * overrides in this intentionally narrow interpreter. */
    if (op==0x90u || op==0xF4u) {
        if (pref66 || pref67 || seg_override) return B86_UNSUPPORTED;
        c->ip=code32?next:(next & 0xFFFFu);
        ++c->icount;
        return op==0xF4u ? B86_HALT : B86_OK;
    }
    if (op>=0xB0u && op<=0xBFu) {
        if (pref67 || seg_override) return B86_UNSUPPORTED;
        width = op<0xB8u ? 1u : (op32?4u:2u);
        if (width==1u && pref66) return B86_UNSUPPORTED;
        for (k=0;k<width;++k) {
            if (!b86_pm4_byte(c,&next,start,&b)) return B86_UNSUPPORTED;
            value |= (uint32_t)b << (8u*k);
        }
        reg=op & 7u;
        if (width==1u) {
            if (reg<4u) c->r[reg] = (c->r[reg]&~0xFFu) | value;
            else c->r[reg-4u] = (c->r[reg-4u]&~0xFF00u) | (value<<8);
        } else if (width==2u)
            c->r[reg]=(c->r[reg]&0xFFFF0000u) | value;
        else c->r[reg]=value;
        c->ip=code32?next:(next&0xFFFFu);
        ++c->icount;
        return B86_OK;
    }
    if (op>=0xA0u && op<=0xA3u) {
        n=ad32 ? 4u : 2u;
        for(k=0;k<n;++k) {
            if (!b86_pm4_byte(c,&next,start,&b)) return B86_UNSUPPORTED;
            addr |= (uint32_t)b << (8u*k);
        }
        width=(op&1u)?(op32?4u:2u):1u;
        if (b86_pm2_check_access(c,seg,addr,width,
                (op&2u)?B86_PM1_WRITE:B86_PM1_READ,
                &linear,&fault)!=B86_PM2_OK) return B86_UNSUPPORTED;
        if ((op&2u)==0u) {  /* MOV AL/AX/EAX, moffs */
            for(k=0;k<width;++k)
                value |= (uint32_t)(*b86_host(c,linear+k)) << (8u*k);
            if(width==1u)c->r[B86_AX]=(c->r[B86_AX]&~0xFFu)|value;
            else if(width==2u)c->r[B86_AX]=(c->r[B86_AX]&0xFFFF0000u)|value;
            else c->r[B86_AX]=value;
        } else {  /* MOV moffs, AL/AX/EAX: entire operand preflighted */
            value=c->r[B86_AX];
            for(k=0;k<width;++k)
                *b86_host(c,linear+k)=(uint8_t)(value>>(8u*k));
            smc(c,linear,width); /* same SMC bookkeeping as real-mode writes */
        }
        c->ip=code32?next:(next&0xFFFFu);
        ++c->icount;
        return B86_OK;
    }
    return B86_UNSUPPORTED; /* No fault delivery or fallback to RM interpreter */
}

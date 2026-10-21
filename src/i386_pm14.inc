/* BB_386_STAGE5C5P_V1: bounded CPL0 contributory fault escalation.
 * If #GP/#NP/#SS is raised by register-form MOV Sreg,r16 and its IDT
 * delivery fails, recognize the second contributory delivery fault and
 * invoke vector 8 (#DF) with a ZERO error-code DWORD. If #DF delivery
 * itself fails, enter persistent 386 shutdown (triple fault).
 * Unrecognized/nested fault sources remain fail-closed. No task, VM86,
 * paging, CPL change, hardware IRQ, or general fault conversion.
 */
#include "b86_pm14.h"
#ifndef B86_PM14_FLASH
#define B86_PM14_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM14_FLASH int b86_pm14_step(B86Cpu *c)
{
    uint32_t ip,start;
    uint8_t op,modrm;
    unsigned sr,sel;
    B86PM2Plan plan;
    B86PM2Fault fault;
    int rc;
    /* Keep initial PE transition and non-386 modes on their old paths.
     * Flags and segment constraints exactly match PM13's admission rules. */
    if(c && c->shutdown386 && c->model>=B86_CPU_386)return B86_SHUTDOWN;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER ||
       !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || !(c->pm386.seg[B86_CS].flags&4u) ||
       c->ip>UINT32_MAX-15u || c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE ||
       (c->flags&0x20000u))
        return b86_pm13_step(c);
    start=ip=c->ip;
    if(!b86_pm4_byte(c,&ip,start,&op) || op!=0x8Eu ||
       !b86_pm4_byte(c,&ip,start,&modrm) || (modrm&0xC0u)!=0xC0u)
        return b86_pm13_step(c);
    sr=(modrm>>3u)&7u;
    if(sr!=B86_ES && sr!=B86_SS && sr!=B86_DS && sr!=B86_FS && sr!=B86_GS)
        return b86_pm13_step(c);
    sel=c->r[modrm&7u]&0xFFFFu;
    if(b86_pm2_plan_segment(c,sr,(uint16_t)sel,0u,0u,&plan,&fault)!=B86_PM2_FAULT ||
       (fault.vector!=B86_PM2_GP && fault.vector!=B86_PM2_NP &&
        fault.vector!=B86_PM2_SS))
        return b86_pm13_step(c);
    /* The first exception either commits completely or not at all.
     * Its delivery failure is classified as a contributory second fault
     * only for the narrow descriptor-fault source established above. */
    rc=b86_pm13_step(c);
    if(rc!=B86_UNSUPPORTED)return rc;
    rc=b86_pm13_deliver(c,8u,1u,0u);
    if(rc==B86_OK)return B86_OK;
    /* Failure while invoking #DF is a triple fault. Sticky shutdown has
     * priority over future fetch, host IRQ and translated dispatch. */
    c->shutdown386=1u;
    return B86_SHUTDOWN;
}

#ifndef B86_X87_PG_RIGHTS_H
#define B86_X87_PG_RIGHTS_H
#include <stdint.h>
/* Read-only 80386 4KiB PDE/PTE permission preflight.
 * Explicitly does not update A/D bits or mutate the page-table/TLB.
 * Reports PF error code on failure: P | W/R | U/S.
 * A user access requires U/S in BOTH PDE/PTE.
 * A write requires R/W in BOTH, except supervisor write with CR0.WP=0.
 * Caller supplies physical RAM bounds and reads page tables without faults.
 * No 4MiB PSE pages supported: not an original 80386 paging feature.
 */
typedef int (*B86X87PgRead32)(void*,uint32_t,uint32_t*);
typedef struct B86X87PgRights {
    uint32_t cr3, ram_bytes;
    unsigned cpl, wp;
    B86X87PgRead32 read32;
    void *opaque;
    uint32_t fault_linear;
    uint8_t fault_error;
} B86X87PgRights;
static inline int b86_x87_pg_rights(B86X87PgRights *s,uint32_t linear,unsigned write,uint32_t *phys){
    uint32_t pdea,pte_addr,pde,pte,frame;
    unsigned user,denied=0,present=0;
    if(!s||!phys||!s->read32)return 0;
    user=s->cpl==3u;
    s->fault_linear=linear;
    s->fault_error=(uint8_t)((write?2u:0u)|(user?4u:0u));
    pdea=(s->cr3&0xfffff000u)+((linear>>22)*4u);
    if(pdea<s->cr3 && (s->cr3&0xfffff000u)>pdea)goto fault;
    if(pdea>s->ram_bytes || s->ram_bytes-pdea<4u || !s->read32(s->opaque,pdea,&pde))goto fault;
    if(!(pde&1u))goto fault;
    pte_addr=(pde&0xfffff000u)+(((linear>>12)&1023u)*4u);
    if(pte_addr>s->ram_bytes || s->ram_bytes-pte_addr<4u || !s->read32(s->opaque,pte_addr,&pte))goto fault;
    if(!(pte&1u))goto fault;
    present=1;
    if(user && (!(pde&4u)||!(pte&4u)))denied=1;
    if(write && (user || s->wp) && (!(pde&2u)||!(pte&2u)))denied=1;
    if(denied)goto fault;
    frame=pte&0xfffff000u;
    if(frame>s->ram_bytes || linear%4096u>=s->ram_bytes-frame)goto fault;
    *phys=frame+(linear&4095u);
    return 1;
fault:
    if(present)s->fault_error|=1u;
    return 0;
}
#endif

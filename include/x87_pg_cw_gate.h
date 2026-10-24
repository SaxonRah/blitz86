#ifndef B86_X87_PG_CW_GATE_H
#define B86_X87_PG_CW_GATE_H
/* Stage4M: only the D9 /7 FNSTCW m16 form can use paged writes in
   experimental x87 mode. All other paged writes remain refused unless
   the separate full Stage4I trial mode is explicitly enabled. */
#ifndef B86_X87_PG_CW_STORE
#define B86_X87_PG_CW_STORE 1
#endif
static inline int b86_x87_pg_cw_store_ok(unsigned op,unsigned modrm,unsigned n){
    return B86_X87_PG_CW_STORE && op==0xd9u && modrm<0xc0u &&
           ((modrm>>3)&7u)==7u && n==2u;
}
#endif

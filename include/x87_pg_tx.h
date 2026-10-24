#ifndef B86_X87_PG_TX_H
#define B86_X87_PG_TX_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Caller guarantees no concurrent page-table changes between prepare and commit.
   resolve returns physical address and validates guest page rights & backing.
   map returns writable physical bytes or NULL.  invalidate must be infallible.
   One transaction supports up to 10 bytes (largest x87 scalar operand). */
typedef struct B86X87TxSpan {uint32_t lin,phys;unsigned len;} B86X87TxSpan;
typedef struct B86X87Tx { B86X87TxSpan s[2];unsigned n;unsigned len; } B86X87Tx;
typedef int (*B86X87TxResolve)(void*,uint32_t,unsigned,uint32_t*);
typedef uint8_t *(*B86X87TxMap)(void*,uint32_t,unsigned);
typedef void (*B86X87TxInvalidate)(void*,uint32_t,uint32_t,unsigned);
static inline int b86_x87_tx_prepare(B86X87Tx *t,void *ctx,uint32_t lin,unsigned n,
                                      B86X87TxResolve resolve,B86X87TxMap map){
    uint64_t last;
    unsigned remain=n;
    uint32_t at=lin;
    if(!t||!resolve||!map||n==0u||n>10u)return -1;
    memset(t,0,sizeof *t);
    last=(uint64_t)lin+n-1u;
    if(last>UINT32_MAX)return -1;
    while(remain){
        unsigned part=4096u-(at&4095u);
        uint32_t phys=0;
        if(part>remain)part=remain;
        if(t->n>=2u||!resolve(ctx,at,1u,&phys)||!map(ctx,phys,part)){
            memset(t,0,sizeof *t);return -1;
        }
        t->s[t->n].lin=at;t->s[t->n].phys=phys;t->s[t->n].len=part;
        ++t->n;at+=part;remain-=part;
    }
    t->len=n;return 0;
}
static inline int b86_x87_tx_write(const B86X87Tx *t,void *ctx,const uint8_t *src,
                                    B86X87TxMap map,B86X87TxInvalidate invalidate){
    unsigned i,pos=0;
    uint8_t *ptrs[2]={0,0};
    if(!t||!src||!map||!invalidate||t->n==0||t->n>2||!t->len)return -1;
    for(i=0;i<t->n;i++){
        /* Revalidate mapping before first store. In a single-threaded guest,
           no page-map changes can interleave the following writes. */
        ptrs[i]=map(ctx,t->s[i].phys,t->s[i].len);
        if(!ptrs[i])return -1;
    }
    for(i=0;i<t->n;i++){
        memcpy(ptrs[i],src+pos,t->s[i].len);
        pos+=t->s[i].len;
    }
    for(i=0;i<t->n;i++)invalidate(ctx,t->s[i].lin,t->s[i].phys,t->s[i].len);
    return 0;
}
#endif

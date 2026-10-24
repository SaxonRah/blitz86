#ifndef B86_X87_STATE_H
#define B86_X87_STATE_H
/* 80387 architectural register-state foundation. No floating-point arithmetic. */
#include <stdint.h>
#include <string.h>

typedef struct B86X87 {
    uint8_t reg[8][10]; /* exact 80-bit extended-real storage, little endian */
    uint16_t control, status, tag;
    uint16_t opcode;
    uint32_t instruction_pointer, data_pointer;
    uint16_t instruction_selector, data_selector;
} B86X87;

enum { B86_X87_OK=0, B86_X87_STACK_FAULT=-1, B86_X87_BAD_ARG=-2 };
static inline unsigned b86_x87_top(const B86X87 *f) { return (f->status >> 11) & 7u; }
static inline unsigned b86_x87_phys(const B86X87 *f, unsigned logical) { return (b86_x87_top(f) + logical) & 7u; }
static inline unsigned b86_x87_tag(const B86X87 *f, unsigned physical) { return (f->tag >> (physical*2u)) & 3u; }
static inline void b86_x87_set_tag(B86X87 *f, unsigned physical, unsigned tag) {
    unsigned sh=2u*physical;
    f->tag=(uint16_t)((f->tag & ~(3u<<sh)) | ((tag&3u)<<sh));
}
static inline void b86_x87_set_top(B86X87 *f, unsigned top) {
    f->status=(uint16_t)((f->status & ~(7u<<11)) | ((top&7u)<<11));
}
/* 387 reset: all empty; extended precision, masked exceptions, round nearest. */
static inline void b86_x87_init(B86X87 *f) {
    memset(f,0,sizeof(*f)); f->control=0x037Fu; f->tag=0xFFFFu;
}
static inline int b86_x87_push_raw(B86X87 *f,const uint8_t value[10],unsigned classification) {
    unsigned target;
    if (!f||!value||classification>2u) return B86_X87_BAD_ARG;
    target=(b86_x87_top(f)+7u)&7u;
    if(b86_x87_tag(f,target)!=3u) return B86_X87_STACK_FAULT;
    memcpy(f->reg[target],value,10);
    b86_x87_set_tag(f,target,classification);
    b86_x87_set_top(f,target);
    return B86_X87_OK;
}
static inline int b86_x87_peek_raw(const B86X87 *f,unsigned logical,uint8_t out[10]) {
    unsigned idx;
    if(!f||!out||logical>=8u) return B86_X87_BAD_ARG;
    idx=b86_x87_phys(f,logical);
    if(b86_x87_tag(f,idx)==3u) return B86_X87_STACK_FAULT;
    memcpy(out,f->reg[idx],10);
    return B86_X87_OK;
}
static inline int b86_x87_pop(B86X87 *f) {
    unsigned idx;
    if(!f)return B86_X87_BAD_ARG;
    idx=b86_x87_top(f);
    if(b86_x87_tag(f,idx)==3u)return B86_X87_STACK_FAULT;
    b86_x87_set_tag(f,idx,3u);
    b86_x87_set_top(f,(idx+1u)&7u);
    return B86_X87_OK;
}
static inline int b86_x87_fxch(B86X87 *f,unsigned logical) {
    unsigned a,b,ta,tb; uint8_t tmp[10];
    if(!f||logical>=8u) return B86_X87_BAD_ARG;
    a=b86_x87_top(f); b=b86_x87_phys(f,logical);
    ta=b86_x87_tag(f,a); tb=b86_x87_tag(f,b);
    if(ta==3u||tb==3u)return B86_X87_STACK_FAULT;
    memcpy(tmp,f->reg[a],10); memcpy(f->reg[a],f->reg[b],10); memcpy(f->reg[b],tmp,10);
    b86_x87_set_tag(f,a,tb); b86_x87_set_tag(f,b,ta);
    return B86_X87_OK;
}
static inline void b86_x87_fclex(B86X87 *f) {
    if(f)f->status &= (uint16_t)~0x80FFu; /* clear exception flags, SF, ES, B */
}
static inline void b86_x87_ffree(B86X87 *f,unsigned logical) {
    if(f&&logical<8u)b86_x87_set_tag(f,b86_x87_phys(f,logical),3u);
}
#endif

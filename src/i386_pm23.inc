/* B86_PM_UNIFIED_V5: i386_pm23.inc */
/* PM23 protected port IO + host-side master PIC/PIT0 register model.
 * Intentional limits: 32-bit CPL0 code only; no I/O bitmap, slave PIC,
 * read-back/latch PIT, channel 1/2, wall-clock scheduling or VM86.
 */
#include "b86_pm23.h"
#ifndef B86_PM23_FLASH
#define B86_PM23_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
B86_PM23_FLASH int b86_pm23_hw_init(B86Pm23Hw *h,unsigned initial_base)
{
    if(!h || b86_pm22_pic_init(&h->pic,initial_base)!=B86_OK)return B86_UNSUPPORTED;
    h->pit_reload=0u;h->pit_phase=0u;h->pit_mode=2u;h->pit_access=3u;
    h->pit_write_phase=0u;h->pit_low=0u;h->pic_init_step=0u;
    h->pic_require_icw3=0u;h->pic_require_icw4=0u;h->pic_isr_read=0u;
    h->pic_icw3=0u;h->pic_icw4=0u;
    return B86_OK;
}
B86_PM23_FLASH uint8_t b86_pm23_port_in8(B86Pm23Hw *h,unsigned port)
{
    if(!h)return 0xffu;
    if(port==0x20u)return h->pic_isr_read?h->pic.in_service:h->pic.pending;
    if(port==0x21u)return h->pic.mask;
    /* Not all PIT channel-read/latch modes are implemented. */
    return 0xffu;
}
B86_PM23_FLASH int b86_pm23_port_out8(B86Pm23Hw *h,unsigned port,unsigned value)
{
    uint32_t reload;
    if(!h || value>255u)return B86_UNSUPPORTED;
    if(port==0x20u){
        if(value&0x10u){ /* ICW1: initialization sequence; no polling/AEOI */
            if((value&0xe0u)!=0u || !(value&1u))return B86_UNSUPPORTED;
            h->pic.pending=0u;h->pic.in_service=0u;h->pic.mask=0u;
            h->pic_require_icw3=(uint8_t)((value&2u)?0u:1u);
            h->pic_require_icw4=(uint8_t)((value&1u)!=0u);
            h->pic_init_step=1u;h->pic_isr_read=0u;
            return B86_OK;
        }
        if(h->pic_init_step)return B86_UNSUPPORTED;
        if((value&0xf8u)==0x20u)return b86_pm22_pic_eoi_highest(&h->pic);
        if((value&0xf8u)==0x60u)return b86_pm22_pic_eoi(&h->pic,value&7u);
        if(value==0x0au){h->pic_isr_read=0u;return B86_OK;}
        if(value==0x0bu){h->pic_isr_read=1u;return B86_OK;}
        return B86_UNSUPPORTED;
    }
    if(port==0x21u){
        if(h->pic_init_step==1u){
            if(value&7u)return B86_UNSUPPORTED;
            h->pic.vector_base=(uint8_t)value;
            h->pic_init_step=h->pic_require_icw3?2u:(h->pic_require_icw4?3u:0u);
            return B86_OK;
        }
        if(h->pic_init_step==2u){h->pic_icw3=(uint8_t)value;
            h->pic_init_step=h->pic_require_icw4?3u:0u;return B86_OK;}
        if(h->pic_init_step==3u){
            if(!(value&1u) || (value&0x0eu))return B86_UNSUPPORTED;
            h->pic_icw4=(uint8_t)value;h->pic_init_step=0u;return B86_OK;
        }
        return b86_pm22_pic_mask(&h->pic,value);
    }
    if(port==0x43u){
        unsigned ch=(value>>6u)&3u,access=(value>>4u)&3u;
        unsigned mode=(value>>1u)&7u;
        if(ch!=0u || access==0u || (value&1u))return B86_UNSUPPORTED;
        if(mode!=2u && mode!=3u)return B86_UNSUPPORTED;
        h->pit_mode=(uint8_t)mode;h->pit_access=(uint8_t)access;
        h->pit_write_phase=0u;h->pit_reload=0u;h->pit_phase=0u;
        return B86_OK;
    }
    if(port==0x40u){
        if(h->pit_access==3u && !h->pit_write_phase){
            h->pit_low=(uint8_t)value;h->pit_write_phase=1u;return B86_OK;
        }
        if(h->pit_access==3u)reload=(uint32_t)h->pit_low|((uint32_t)value<<8u);
        else if(h->pit_access==1u)reload=value;
        else if(h->pit_access==2u)reload=(uint32_t)value<<8u;
        else return B86_UNSUPPORTED;
        h->pit_reload=reload?reload:65536u;
        h->pit_phase=0u;h->pit_write_phase=0u;return B86_OK;
    }
    return B86_UNSUPPORTED;
}
B86_PM23_FLASH int b86_pm23_advance(B86Pm23Hw *h,uint32_t clocks,uint32_t *edges)
{
    uint64_t total;
    uint32_t n;
    if(!h)return B86_UNSUPPORTED;
    if(edges)*edges=0u;
    if(!h->pit_reload || !clocks)return B86_OK;
    total=(uint64_t)h->pit_phase+clocks;
    n=(uint32_t)(total/h->pit_reload);
    h->pit_phase=(uint32_t)(total%h->pit_reload);
    if(edges)*edges=n;
    if(n)return b86_pm22_pic_raise(&h->pic,0u);
    return B86_OK;
}
B86_PM23_FLASH int b86_pm23_poll(B86Cpu *cpu,B86Pm23Hw *h,unsigned *vector)
{
    if(!h || h->pic_init_step)return B86_UNSUPPORTED;
    return b86_pm22_pic_poll(cpu,&h->pic,vector);
}
/* r61: x87 ESC (D8-DF) and WAIT in protected mode without a coprocessor.
 * EM=1 (or WAIT with MP+TS) needs #NM through the IDT: left UNSUPPORTED.
 * Otherwise decode prefixes + ModRM (+SIB/displacement, 16/32-bit
 * addressing from CS.D xor 67h) and advance EIP: absent-FPU probes see no
 * coprocessor. Returns -1 when the instruction is not ESC/WAIT. */
/* B86_PM31_V1: #NM delivery for CR0.EM=1 (no coprocessor, emulate) and
 * WAIT with MP+TS. DOS/4GW sets EM when no 387 is present; the program's
 * x87 emulator owns vector 7 through the IDT. Fault frame: EIP of the first
 * prefix byte, no error code. Defined in PM26. */
static int b86_pm26_interrupt(B86Cpu *c, unsigned vec, uint32_t return_ip);
B86_PM23_FLASH int b86_pm23_esc(B86Cpu *c)
{
    uint32_t ip, start;
    uint8_t op, m, sib, b;
    unsigned ad32, mod, rm, disp = 0u, i;
    if (!c->pm386.seg[B86_CS].valid || c->ip > UINT32_MAX - 15u) return -1;
    ad32 = (c->pm386.seg[B86_CS].flags & 4u) != 0u;
    ip = start = c->ip;
    for (;;) {
        if (!b86_pm4_byte(c, &ip, start, &op)) return -1;
        if (op == 0x67u) { ad32 ^= 1u; continue; }
        if (op == 0x66u || op == 0x26u || op == 0x2Eu || op == 0x36u ||
            op == 0x3Eu || op == 0x64u || op == 0x65u || op == 0xF0u ||
            op == 0xF2u || op == 0xF3u) continue;
        break;
    }
    if (op == 0x9Bu) {
        if ((c->msw & 0xAu) == 0xAu) return b86_pm26_interrupt(c, 7u, start);
        c->ip = ip; ++c->icount; return B86_OK;
    }
    if (op < 0xD8u || op > 0xDFu) return -1;
    if (c->msw & 4u) return b86_pm26_interrupt(c, 7u, start);
    if (!b86_pm4_byte(c, &ip, start, &m)) return B86_UNSUPPORTED;
    mod = m >> 6; rm = m & 7u;
    if (mod != 3u) {
        if (ad32) {
            if (rm == 4u) {
                if (!b86_pm4_byte(c, &ip, start, &sib)) return B86_UNSUPPORTED;
                if (mod == 0u && (sib & 7u) == 5u) disp = 4u;
            } else if (mod == 0u && rm == 5u) disp = 4u;
            if (mod == 1u) disp = 1u; else if (mod == 2u) disp = 4u;
        } else {
            if (mod == 0u && rm == 6u) disp = 2u;
            else if (mod == 1u) disp = 1u; else if (mod == 2u) disp = 2u;
        }
        for (i = 0; i < disp; ++i)
            if (!b86_pm4_byte(c, &ip, start, &b)) return B86_UNSUPPORTED;
    }
    c->ip = ip; ++c->icount;
    return B86_OK;
}

/* CPU protected IN/OUT: byte/word/dword port-width transfer. We use the
 * existing byte-wide hooks, little-endian consecutive ports for wide I/O.
 * Instruction bytes are preflighted before any externally visible callback.
 */
B86_PM23_FLASH int b86_pm23_step(B86Cpu *c)
{
    uint32_t ip,start,oldax,v=0u,mask;
    uint8_t op,b;
    unsigned op32,n,i,port;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || !c->pm386.seg[B86_CS].valid ||
       !(c->pm386.seg[B86_CS].access&8u) ||
       (!(c->pm386.seg[B86_CS].flags&4u) && c->ip>0xFFFFu) ||
       c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER ||
       (c->seg[B86_CS]&3u) || c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE ||
       (c->flags&0x20000u) || c->ip>UINT32_MAX-15u)
        return b86_pm21_step(c);
    ip=start=c->ip;op32=!!(c->pm386.seg[B86_CS].flags&4u);
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){op32=!((c->pm386.seg[B86_CS].flags&4u)!=0u);continue;}
        if(op==0x67u || op==0x26u || op==0x2eu || op==0x36u ||
           op==0x3eu || op==0x64u || op==0x65u)continue;
        break;
    }
    if(!((op>=0xe4u && op<=0xe7u)||(op>=0xecu && op<=0xefu)))
        return b86_pm21_step(c);
    if(op<=0xe7u){if(!b86_pm4_byte(c,&ip,start,&b))return B86_UNSUPPORTED;
        port=b;}else port=c->r[B86_DX]&0xffffu;
    n=((op&1u)==0u)?1u:(op32?4u:2u);
    mask=n==1u?0xffu:(n==2u?0xffffu:UINT32_MAX);
    oldax=c->r[B86_AX];
    if(op==0xe4u||op==0xe5u||op==0xecu||op==0xedu){
        for(i=0;i<n;i++)v|=(uint32_t)(c->in8?c->in8(c,(uint16_t)(port+i)):0xffu)<<(8u*i);
        c->r[B86_AX]=(oldax&~mask)|(v&mask);
    }else if(c->out8){
        for(i=0;i<n;i++)c->out8(c,(uint16_t)(port+i),(uint8_t)(oldax>>(8u*i)));
    }
    c->ip=(c->pm386.seg[B86_CS].flags&4u)?ip:(ip&0xFFFFu);++c->icount;
    return B86_OK;
}

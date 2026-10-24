/* B86_PM_UNIFIED_V5: i386_pm23.inc */
/* PM23 protected port IO + host-side master PIC/PIT0 register model.
 * Intentional limits: 32-bit CPL0 code only; no I/O bitmap, slave PIC,
 * read-back/latch PIT, channel 1/2, wall-clock scheduling or VM86.
 */
#include "b86_pm23.h"
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
#include "x87_decode3a.h"
#include "x87_store3d.h"
#include "x87_pm_mem_plan.h"
#if defined(B86_X87_TRACE_FIRST_REJECT) && B86_X87_TRACE_FIRST_REJECT
#include <stdio.h>
#endif
#endif
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
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
/* Stage4L: opt-in first-occurrence execution trace.  No data or CPU
 * mutation outside a 32-bit host-side "already printed" mask.  One line per
 * reason class, never a log per instruction.  Disabled in ordinary builds. */
#if defined(B86_X87_TRACE_FIRST_REJECT) && B86_X87_TRACE_FIRST_REJECT
static uint32_t b86_x87_diag_seen;
static void b86_x87_diag_event(const B86Cpu *c,uint32_t start,
                               unsigned op,unsigned m,unsigned kind,
                               const char *reason)
{
    uint32_t bit;
    if(!c || kind>=32u) return;
    bit=1u<<kind;
    if(b86_x87_diag_seen&bit) return;
    b86_x87_diag_seen|=bit;
    printf("[bb386-x87-first] kind=%u reason=%s cs=%04lX ip=%08lX csbase=%08lX "
           "op=%02X modrm=%02X pg=%u cr3=%08lX pf=%u pf_lin=%08lX pf_err=%02X\n",
           kind,reason,(unsigned long)c->seg[B86_CS],(unsigned long)start,
           (unsigned long)c->pm386.seg[B86_CS].base,op,m,(unsigned)c->pg,
           (unsigned long)c->cr3,(unsigned)c->pf_pending,
           (unsigned long)c->pf_lin,(unsigned)c->pf_err);
}
#define B86_X87_DIAG(c,ip,op,m,k,reason) b86_x87_diag_event(c,ip,op,m,k,reason)
#else
#define B86_X87_DIAG(c,ip,op,m,k,reason) ((void)0)
#endif
/* Reasons are printed once per classification, not once per x87 opcode.
 * 0 attempt; 1 reg-ok; 2 reg-unsupported; 3 mem-ok;
 * 4 plan; 5 paged-store-refused; 6 opcode-rescan; 7 EA-decode;
 * 8 segment; 9 wrap; 10 permissions; 11 page-backing;
 * 12 execution; 13 ModRM-fetch; 14 SIB-fetch; 15 displacement-fetch. */
/* Strict Stage4B: paging and unhandled memory forms stay unsupported. */
/* Stage4H opt-in paging rights check: full operand preflight before x87
 * register state or guest data changes.  Does not mutate the global TLB. */
#include "x87_pg_rights.h"
#include "x87_pg_cw_gate.h"
#ifndef B86_X87_PG_WRITES_TRIAL
#define B86_X87_PG_WRITES_TRIAL 0
#endif
#if B86_X87_PG_WRITES_TRIAL || B86_X87_PG_CW_STORE
#include "x87_pg_tx.h"
#if defined(B86_PJ) && B86_PJ
static void b86_x87_pg_note_pj_store(B86Cpu *c,uint32_t physical,unsigned size);
#endif
#endif
static int b86_x87_pm_read_pte(void *opaque,uint32_t physical,uint32_t *value)
{
    B86Cpu *c=(B86Cpu *)opaque;
    if (!c || !value || physical > B86_RAM_BYTES ||
        B86_RAM_BYTES - physical < 4u) return 0;
    *value=b86_pg_rd32(c,physical);
    return 1;
}
static int b86_x87_pm_rights(B86Cpu *c,uint32_t linear,
                             unsigned size,unsigned write)
{
    B86X87PgRights r;
    uint32_t last,at,physical;
    if (!c || !size || linear > UINT32_MAX-(size-1u)) return 0;
    if (!c->pg) return 1;
    memset(&r,0,sizeof r);
    r.cr3=c->cr3;r.ram_bytes=B86_RAM_BYTES;
    r.cpl=(unsigned)(c->seg[B86_CS]&3u);
    /* The existing PM/paging backend emulates an 80386 (B86_CPU_386).
     * Supervisor stores ignore the PTE/PDE R/W bit on that CPU, including
     * writes into present read-only pages. The 386 has no CR0.WP bit.
     * This matches i386_pg.inc; CPL3 writes remain permission-checked.
     * Any future CPU model must retain conservative WP=1 here until the
     * complete CR0.WP state is implemented rather than inferred from msw. */
    r.wp=(c->model==B86_CPU_386)?0u:1u;
    r.read32=b86_x87_pm_read_pte;r.opaque=c;
    last=linear+size-1u;
    at=linear;
    for (;;) {
        uint32_t end=(at&0xfffff000u)|0xfffu;
        if(!b86_x87_pg_rights(&r,at,write,&physical)) {
            if(!c->pf_pending) {
                c->pf_pending=1u;
                c->pf_lin=r.fault_linear;
                c->pf_err=r.fault_error;
            }
            return 0;
        }
        if(last<=end) break;
        at=end+1u;
    }
    return 1;
}
/* x87 Stage4D: guest-paging range preflight.  Does NOT repair the core
 * paging engine's missing permission enforcement or A/D-bit semantics. */
static int b86_x87_pm_page_preflight(B86Cpu *c, uint32_t linear,
                                     unsigned size, unsigned write)
{
    uint32_t last, at, phys;
    if (!c->pg) return 1;
    if (!size || linear > UINT32_MAX - (size - 1u)) return 0;
    last = linear + size - 1u;
    /* b86_pg_probe records the FIRST #PF and b86_step delivers it. */
    if (!b86_pg_probe(c, linear, size, write)) return 0;
    at = linear;
    for (;;) {
        uint32_t end = (at & 0xfffff000u) + 0xfffu;
        uint32_t chunk = (last < end ? last : end) - at;
        if (!b86_pg_walk(c, at, &phys)) {
            b86_pg_fault(c, at, write);
            return 0;
        }
        if (phys >= B86_RAM_BYTES || chunk >= B86_RAM_BYTES - phys)
            return 0; /* Out-of-RAM frames: refuse, never write sink. */
        if (last <= end) break;
        at = end + 1u;
    }
    return 1;
}
static uint8_t *b86_x87_pm_ptr(B86Cpu *c,uint32_t linear)
{
    uint32_t phys;
    if (!c->pg) return b86_host(c, linear);
    if (!b86_pg_walk(c, linear, &phys) || phys >= B86_RAM_BYTES)
        return (uint8_t *)0;
    return b86_host_phys(c, phys);
}

typedef struct B86X87PmMem { B86Cpu *cpu; uint32_t address; unsigned width; } B86X87PmMem;
#if B86_X87_PG_WRITES_TRIAL || B86_X87_PG_CW_STORE
/* Trial-only: revalidate the complete store against live PDE/PTE before
 * touching backing RAM. One instruction is serialized within b86_step().
 * WP=1 is deliberately conservative because CR0.WP is not represented
 * in the 16-bit MSW here: supervisor writes to read-only pages decline.
 */
static int b86_x87_pm_tx_resolve(void *opaque,uint32_t linear,
                                 unsigned write,uint32_t *physical)
{
    B86X87PmMem *ctx=(B86X87PmMem *)opaque;
    B86X87PgRights r;
    B86Cpu *c;
    if (!ctx || !physical || !(c=ctx->cpu) || !c->pg) return 0;
    memset(&r,0,sizeof r);
    r.cr3=c->cr3; r.ram_bytes=B86_RAM_BYTES;
    r.cpl=(unsigned)(c->seg[B86_CS]&3u);
    /* Same 80386 supervisor policy as the operand-range preflight above.
     * This is a SECOND, independent permission check immediately before
     * the all-or-nothing write transaction, not a permissions bypass. */
    r.wp=(c->model==B86_CPU_386)?0u:1u;
    r.read32=b86_x87_pm_read_pte; r.opaque=c;
    if (!b86_x87_pg_rights(&r,linear,write,physical)) {
        if (!c->pf_pending) {
            c->pf_pending=1u;
            c->pf_lin=r.fault_linear;
            c->pf_err=r.fault_error;
        }
        return 0;
    }
    return 1;
}
static uint8_t *b86_x87_pm_tx_map(void *opaque,uint32_t physical,unsigned size)
{
    B86X87PmMem *ctx=(B86X87PmMem *)opaque;
    if(!ctx || !ctx->cpu || !size || physical >= B86_RAM_BYTES ||
       size > B86_RAM_BYTES-physical)return (uint8_t *)0;
    return b86_host_phys(ctx->cpu,physical);
}
static void b86_x87_pm_tx_invalidate(void *opaque,uint32_t linear,
                                      uint32_t physical,unsigned size)
{
    B86X87PmMem *ctx=(B86X87PmMem *)opaque;
    B86Cpu *c=ctx->cpu;
    /* Keep existing 8086 codemap invalidation and linear PJ SMC checks.
     * Stage4U additionally checks the exact physical translated-code set,
     * so a non-code x87 data store does not throw out the native cache. */
    if(linear < B86_MEM_BYTES && size <= B86_MEM_BYTES-linear)
        smc(c,linear,size);
    if(physical != linear && physical < B86_MEM_BYTES &&
       size <= B86_MEM_BYTES-physical)smc(c,physical,size);
#if defined(B86_PJ) && B86_PJ
    b86_x87_pg_note_pj_store(c,physical,size);
#endif
}
#endif
static int b86_x87_pm_rd(void *opaque,uint32_t addr,uint8_t *dst,unsigned size)
{
    B86X87PmMem *ctx=(B86X87PmMem *)opaque;
    unsigned i;
    if(!ctx || addr!=ctx->address || size!=ctx->width)return -1;
    for(i=0;i<size;i++){uint8_t *p=b86_x87_pm_ptr(ctx->cpu,addr+i);if(!p)return -1;dst[i]=*p;}
    return 0;
}
static int b86_x87_pm_wr(void *opaque,uint32_t addr,const uint8_t *src,unsigned size)
{
    B86X87PmMem *ctx=(B86X87PmMem *)opaque;
    unsigned i;
    if(!ctx || !ctx->cpu || !src || addr!=ctx->address || size!=ctx->width)return -1;
#if B86_X87_PG_WRITES_TRIAL || B86_X87_PG_CW_STORE
    if(ctx->cpu->pg){
        B86X87Tx tx;
        if(b86_x87_tx_prepare(&tx,ctx,addr,size,
                             b86_x87_pm_tx_resolve,b86_x87_pm_tx_map))return -1;
        return b86_x87_tx_write(&tx,ctx,src,b86_x87_pm_tx_map,
                                b86_x87_pm_tx_invalidate);
    }
#else
    if(ctx->cpu->pg)return -1;
#endif
    for(i=0;i<size;i++){
        uint8_t *p=b86_x87_pm_ptr(ctx->cpu,addr+i);
        if(!p)return -1; /* caller preflights complete range */
        *p=src[i];
    }
    smc(ctx->cpu,addr,size);
    return 0;
}
/* Stage4T: approved scalar stores use the prevalidated 2-span atomic
 * paged transaction (not unrestricted trial writes).  Only operand forms
 * implemented by x87_memory3b/x87_store3d may be admitted here. */
static int b86_x87_pg_scalar_store_ok(unsigned op,unsigned m,unsigned n){
    unsigned sub=(m>>3)&7u;
    if(m>=0xC0u)return 0;
    if(op==0xD9u)return ((sub==2u||sub==3u)&&n==4u)||(sub==7u&&n==2u);
    if(op==0xDBu)return ((sub==1u||sub==2u||sub==3u)&&n==4u)||(sub==7u&&n==10u);
    if(op==0xDDu)return ((sub==1u||sub==2u||sub==3u)&&n==8u)||(sub==7u&&n==2u);
    if(op==0xDFu)return ((sub==1u||sub==2u||sub==3u)&&n==2u)||(sub==7u&&n==8u);
    return 0;
}
/* ip points at ModR/M. Caller has already fetched the x87 opcode. */
static int b86_x87_pm_mem(B86Cpu *c,uint32_t start,unsigned op,unsigned m,
                           unsigned ad32,uint32_t *ip,unsigned *why)
{
    B86PM6EA ea;
    B86X87MemPlan plan;
    B86PM2Fault fault;
    B86X87PmMem ctx;
    B86X87Memory memory;
    uint32_t operand=start, linear, scan=start;
    uint8_t b;
    unsigned seg=B86_DS;
    int override=0;
    if(!b86_x87_mem_plan(op,m,&plan)){*why=4u;return B86_X87_DEC_UNSUPPORTED;}
#if !B86_X87_PG_WRITES_TRIAL
    /* Narrow compatibility step: FNSTCW m16 is the first Descent store.
     * It uses the Stage4I all-pages-before-write transaction below. */
    if(c->pg && plan.write && !b86_x87_pg_scalar_store_ok(op,m,plan.size)){
        *why=5u;return B86_X87_DEC_UNSUPPORTED;}
#endif
    /* Address decoding already does instruction-fetch preflight for ModR/M,
       SIB and displacement. Input ip is after ModR/M, rewind one byte. */
    for(;;){
        if(!b86_pm4_byte(c,&scan,start,&b)){*why=6u;return B86_X87_DEC_UNSUPPORTED;}
        switch(b){
        case 0x26u:seg=B86_ES;override=1;break;
        case 0x2eu:seg=B86_CS;override=1;break;
        case 0x36u:seg=B86_SS;override=1;break;
        case 0x3eu:seg=B86_DS;override=1;break;
        case 0x64u:seg=B86_FS;override=1;break;
        case 0x65u:seg=B86_GS;override=1;break;
        case 0x66u:case 0x67u:break;
        default:if(b==op)goto x87_opcode_found;*why=6u;return B86_X87_DEC_UNSUPPORTED;
        }
    }
x87_opcode_found:
    /* scan is immediately after the opcode; ModR/M starts here. */
    operand = scan;
    if(!b86_pm6_ea(c,&operand,start,(int)ad32,&ea) || ea.is_reg){
        *why=7u;return B86_X87_DEC_UNSUPPORTED;}
    if(override)ea.seg=seg;
    if(b86_pm2_check_access(c,ea.seg,ea.off,plan.size,
         plan.write?B86_PM1_WRITE:B86_PM1_READ,&linear,&fault)!=B86_PM2_OK){
        *why=8u;return B86_X87_DEC_UNSUPPORTED;}
    if(linear>UINT32_MAX-(plan.size-1u)){*why=9u;return B86_X87_DEC_UNSUPPORTED;}
    if(!b86_x87_pm_rights(c,linear,plan.size,plan.write)){
        *why=10u;return B86_X87_DEC_UNSUPPORTED;}
    if(!b86_x87_pm_page_preflight(c,linear,plan.size,plan.write)){
        *why=11u;return B86_X87_DEC_UNSUPPORTED;}
    ctx.cpu=c;ctx.address=linear;ctx.width=plan.size;
    memory.opaque=&ctx;memory.read=b86_x87_pm_rd;memory.write=b86_x87_pm_wr;
    if(b86_x87_exec_mem3d(&c->x87,op,m,linear,&memory)!=B86_X87_OK){
        *why=12u;return B86_X87_DEC_UNSUPPORTED;}
    *ip=operand;
    return B86_X87_OK;
}
#endif
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
    if (!b86_pm4_byte(c, &ip, start, &m)) {
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
        B86_X87_DIAG(c,start,op,0xffu,13u,"modrm-fetch");
#endif
        return B86_UNSUPPORTED;
    }
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
    B86_X87_DIAG(c,start,op,m,0u,"first-x87-attempt");
#endif
    mod = m >> 6; rm = m & 7u;
    if (mod != 3u) {
        if (ad32) {
            if (rm == 4u) {
                if (!b86_pm4_byte(c, &ip, start, &sib)) {
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
                    B86_X87_DIAG(c,start,op,m,14u,"sib-fetch");
#endif
                    return B86_UNSUPPORTED;
                }
                if (mod == 0u && (sib & 7u) == 5u) disp = 4u;
            } else if (mod == 0u && rm == 5u) disp = 4u;
            if (mod == 1u) disp = 1u; else if (mod == 2u) disp = 4u;
        } else {
            if (mod == 0u && rm == 6u) disp = 2u;
            else if (mod == 1u) disp = 1u; else if (mod == 2u) disp = 2u;
        }
        for (i = 0; i < disp; ++i)
            if (!b86_pm4_byte(c, &ip, start, &b)) {
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
                B86_X87_DIAG(c,start,op,m,15u,"displacement-fetch");
#endif
                return B86_UNSUPPORTED;
            }
    }
#if defined(B86_X87_EXPERIMENTAL) && B86_X87_EXPERIMENTAL
    /* Stage4B: strict opt-in. Decoder must commit FPU state before EIP moves.
       Unsupported memory ops/special arithmetic never silently skip. */
    if (mod == 3u) {
        /* FNSTSW AX (DF E0): the x87 status word is copied to guest AX.
         * Preserve EAX[31:16] in 32-bit PM and leave FPU state untouched.
         * This non-waiting opcode does not access guest memory. */
        if(op==0xDFu && m==0xE0u) {
            uint16_t sw;
            if(b86_x87_fnstsw_ax(&c->x87,op,m,&sw)!=B86_X87_OK)
                return B86_UNSUPPORTED;
            c->r[B86_AX]=(c->r[B86_AX]&0xFFFF0000u)|(uint32_t)sw;
        } else if (b86_x87_exec_reg(&c->x87, op, m) != B86_X87_OK) {
            B86_X87_DIAG(c,start,op,m,2u,"register-unsupported");
            return B86_UNSUPPORTED;
        }
        B86_X87_DIAG(c,start,op,m,1u,"register-ok");
    } else {
        unsigned why=12u;
        if (b86_x87_pm_mem(c, start, op, m, ad32, &ip, &why) != B86_X87_OK) {
#if defined(B86_X87_TRACE_FIRST_REJECT) && B86_X87_TRACE_FIRST_REJECT
            static const char *const reasons[]={
                "none","reg-ok","reg-unsupported","mem-ok","mem-plan",
                "paged-store-disabled","opcode-rescan","effective-address",
                "segment-access","linear-wrap","paging-rights",
                "page-backing","mem-exec"
            };
            B86_X87_DIAG(c,start,op,m,why,why<13u?reasons[why]:"unknown");
#else
            (void)why;
#endif
            return B86_UNSUPPORTED;
        }
        B86_X87_DIAG(c,start,op,m,3u,"memory-ok");
    }
#endif
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

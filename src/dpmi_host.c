/* dpmi_host.c: DPMI 0.9 host for blitz86, stage A. See dpmi_host.h and the design
 * document "blitzBUS DPMI Host". Portable C; no execution of guest code here. */
#include "dpmi_host.h"
#include "b86_pm0.h"
#include "b86_pm1.h"
#include <stdio.h>
#include <string.h>
/* uint32_t is unsigned long on newlib/ARM, unsigned int elsewhere; both 32 bits, so %X prints
 * the same value. Keep the log formats portable instead of casting every argument. */
#pragma GCC diagnostic ignored "-Wformat"
#ifndef DPMI_LOG
#define DPMI_LOG(...) fprintf(stderr, __VA_ARGS__)   /* blitzBUS defines it as printf */
#endif

/* ---- host block layout (paragraph-aligned, allocated by the client) ---------- */
#define HB_GDT     0x0000u          /* 16 entries (0040h = BIOS data area, a DPMI convention) */
#define HB_IDT     0x0080u          /* 256 gates */
#define HB_LDT     0x0880u          /* 512 entries */
#define HB_RMSTACK 0x1880u          /* 1 KB real-mode stack, grows down from +0x400 */
#define HB_PMSTACK 0x1C80u          /* 4 KB protected-mode stack (host use) */
#define HB_CODE    0x2C80u          /* host code segment, 2 KB of HLT: offset = vector*4; 400h save/restore, 404h raw switch */
#define HB_TSS     0x3480u          /* 104 bytes, never switched to */
#define HB_SIZE    0x3580u
#define HC_SAVE    0x400u
#define HC_RAWRM   0x404u
#define HC_CBRET   0x408u            /* IRET target of a real-mode callback procedure */
#define HB_CBSTUB  0x3540u           /* 16 real-mode callback stubs, 4 bytes each: CD E3 <slot> 90 (after the TSS at 3480h+68h) */
#define CB_N       16u
#define HB_PARAS   ((HB_SIZE + 15u) / 16u)

#define SEL_HOST_CS 0x08u
#define SEL_HOST_DS 0x10u
#define SEL_FLAT    0x18u
#define SEL_LDT     0x20u
#define SEL_TSS     0x28u
#define LDT_N       512u
#define LDT_SEL(i)  ((uint16_t)(((i) << 3) | 7u))   /* ring-3 selectors, as real hosts issue */

#define E_UNSUPPORTED 0x8001u
#define E_DESC_UNAVAIL 0x8011u
#define E_LINEAR_UNAVAIL 0x8012u
#define E_INVALID_VALUE 0x8021u
#define E_INVALID_SELECTOR 0x8022u
#define E_INVALID_HANDLE 0x8023u

typedef struct Saved {                 /* protected-mode state saved around a real-mode call */
    uint32_t r[8], ip, flags, rhi[8];
    uint16_t seg[6];
    B86PM1State pm;
    uint32_t idt_base; uint16_t idt_limit;
    uint8_t kind;                      /* 1 reflect INT, 2 0300h, 3 0301h/0302h, 4 DOS alloc, 5 DOS free, 6 DOS resize */
    uint8_t vector;
    uint32_t frame_lin;                /* the client's IRET frame (INT 31h or the reflected INT) */
    uint32_t arg;                      /* 0300h: ES:EDI structure linear; DOS: size/selector */
} Saved;

typedef struct DpmiS {
    DpmiEnv env;
    int active, entered, client32;
    uint32_t hb; uint16_t hbseg;
    uint8_t ldt_used[LDT_N];
    uint16_t s2d_seg[32], s2d_sel[32]; unsigned s2d_n;
    struct { uint32_t base, size; uint8_t used; } blk[64]; unsigned nblk;
    uint32_t pm_vec_sel[256], pm_vec_off[256];
    uint32_t exc_sel[32], exc_off[32];
    Saved nest[8]; unsigned depth;
    struct { uint8_t used; uint16_t cs, es; uint32_t eip, edi; } cb[CB_N];
    uint16_t rmss_sel;
    uint32_t pend_r[8], pend_flags; uint16_t pend_seg[4], pend_ip;   /* entry waiting for the PSP */
    unsigned long calls[256], unsupported[256], reflect, rmcalls;
    /* virtual memory */
    uint32_t pd, vbase, vend, nslots, nframes, frame0, hand;
    uint32_t frame_vpn[1024];            /* per frame: virtual page number + 1 (0 = free) */
    uint8_t slot_used[4096 / 8];         /* swap slots (16 MB / 4 KB) */
    uint8_t page[4096];                  /* bounce buffer for swap I/O */
    unsigned long faults, zero_fills, swap_ins, swap_outs, evict_fail;
    B86Cpu *cpu;                         /* the CPU of the current host call (for TLB flushes) */
    unsigned long declined;              /* page faults passed on to the client */
} DpmiS;
static DpmiS *S;
#define E (S->env)
#define g_active S->active
#define g_entered S->entered
#define g_client32 S->client32
#define g_hb S->hb
#define g_hbseg S->hbseg
#define g_ldt_used S->ldt_used
#define g_seg2desc_seg S->s2d_seg
#define g_seg2desc_sel S->s2d_sel
#define g_seg2desc_n S->s2d_n
#define g_blk S->blk
#define g_nblk S->nblk
#define g_pm_vec_sel S->pm_vec_sel
#define g_pm_vec_off S->pm_vec_off
#define g_exc_sel S->exc_sel
#define g_exc_off S->exc_off
#define g_nest S->nest
#define g_depth S->depth
#define g_cb S->cb
#define g_rmss_sel S->rmss_sel
#define g_calls S->calls
#define g_unsupported S->unsupported
#define g_reflect S->reflect
#define g_rmcalls S->rmcalls
#define FNI(fn) ((((fn) >> 4) & 0xF0u) | ((fn) & 0x0Fu))   /* 0x0X0Y -> XY: unique for DPMI 0.9 functions */

/* ---- guest memory helpers ---------------------------------------------------- */
/* Host accesses use linear addresses. Below the virtual region linear =
 * physical; inside it, the page tables translate (faulting the page in if
 * needed). Page-table and frame accesses use the p* (physical) variants. */
static uint32_t xlat(uint32_t a);
static void tlb_drop(B86Cpu *c);
static uint8_t  rd8(uint32_t a) { return E.mem[xlat(a)]; }
static uint16_t rd16(uint32_t a) { return (uint16_t)(rd8(a) | rd8(a + 1) << 8); }
static uint32_t rd32(uint32_t a) { return (uint32_t)rd16(a) | (uint32_t)rd16(a + 2) << 16; }
static void wr8(uint32_t a, uint8_t v) { E.mem[xlat(a)] = v; }
static void wr16(uint32_t a, uint16_t v) { wr8(a, (uint8_t)v); wr8(a + 1, (uint8_t)(v >> 8)); }
static void wr32(uint32_t a, uint32_t v) { wr16(a, (uint16_t)v); wr16(a + 2, (uint16_t)(v >> 16)); }
static uint32_t prd32(uint32_t a) { return (uint32_t)E.mem[a] | (uint32_t)E.mem[a + 1] << 8 | (uint32_t)E.mem[a + 2] << 16 | (uint32_t)E.mem[a + 3] << 24; }
static void pwr32(uint32_t a, uint32_t v) { E.mem[a] = (uint8_t)v; E.mem[a + 1] = (uint8_t)(v >> 8); E.mem[a + 2] = (uint8_t)(v >> 16); E.mem[a + 3] = (uint8_t)(v >> 24); }

static void desc_write(uint32_t a, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    /* flags: bit 6 D/B, bit 7 G (descriptor byte 6 high nibble) */
    uint8_t g = 0;
    if (limit > 0xFFFFFu) { g = 0x80u; limit >>= 12; }
    wr16(a, (uint16_t)limit); wr16(a + 2, (uint16_t)base); wr8(a + 4, (uint8_t)(base >> 16));
    wr8(a + 5, (uint8_t)(((access & 0x10u) && a >= g_hb + HB_LDT && a < g_hb + HB_LDT + 8u * LDT_N) ? (access | 0x60u) : access));   /* LDT entries: DPL 3 */ wr8(a + 6, (uint8_t)((flags & 0x70u) | g | ((limit >> 16) & 0xFu))); wr8(a + 7, (uint8_t)(base >> 24));
}
static uint32_t desc_base(uint32_t a) { return (uint32_t)rd16(a + 2) | (uint32_t)rd8(a + 4) << 16 | (uint32_t)rd8(a + 7) << 24; }
static uint32_t desc_limit(uint32_t a) { uint32_t l = (uint32_t)rd16(a) | ((uint32_t)(rd8(a + 6) & 0xFu) << 16); return (rd8(a + 6) & 0x80u) ? (l << 12) | 0xFFFu : l; }
static uint32_t ldt_addr(uint16_t sel) { return g_hb + HB_LDT + (uint32_t)(sel & 0xFFF8u); }
static int sel_ok(uint16_t sel) { unsigned i = sel >> 3; return (sel & 4u) && i < LDT_N && g_ldt_used[i]; }

static void gate_write(unsigned v, uint16_t sel, uint32_t off)
{
    uint32_t a = g_hb + HB_IDT + 8u * v;
    wr16(a, (uint16_t)off); wr16(a + 2, sel); wr8(a + 4, 0); wr8(a + 5, 0xEEu); wr16(a + 6, (uint16_t)(off >> 16));   /* 32-bit interrupt gate, DPL 3 */
}

/* ---- visible selectors: ES CS SS DS in seg[4], FS GS in seg386[2] -------------- */
static uint16_t get_sel(const B86Cpu *c, unsigned s) { return s < 4u ? (uint16_t)c->seg[s] : c->seg386[s - 4u]; }
static void set_sel(B86Cpu *c, unsigned s, uint16_t v)
{
    if (s < 4u) { c->seg[s] = v; c->segp[s] = c->mem + ((uint32_t)v << 4); } else c->seg386[s - 4u] = v;
    c->pm386.seg[s].selector = v;
}
/* ---- segment cache loading ----------------------------------------------------- */
static void cache_from_desc(B86PM1Cache *k, uint16_t sel, uint32_t a)
{
    memset(k, 0, sizeof *k);
    k->base = desc_base(a); k->limit = desc_limit(a); k->selector = sel;
    k->access = rd8(a + 5); k->flags = (uint8_t)(rd8(a + 6) >> 4); k->valid = 1;
}
static uint32_t desc_of(B86Cpu *c, uint16_t sel)
{
    if (sel & 4u) return ldt_addr(sel);
    return (uint32_t)c->gdt_base + (sel & 0xFFF8u);
}
static void load_cs(B86Cpu *c, uint16_t sel) { cache_from_desc(&c->pm386.seg[B86_CS], sel, desc_of(c, sel)); set_sel(c, B86_CS, sel); }
static void load_seg(B86Cpu *c, unsigned s, uint16_t sel)
{
    if (s == B86_CS) { load_cs(c, sel); return; }
    if (b86_pm1_prepare_segment(c, s, sel, 0) != B86_PM1_OK) { cache_from_desc(&c->pm386.seg[s], sel, desc_of(c, sel)); }
    set_sel(c, s, sel);
}
static void reload_if_loaded(B86Cpu *c, uint16_t sel)
{
    for (unsigned s = 0; s < 6u; ++s) if ((get_sel(c, s) & 0xFFFCu) == (sel & 0xFFFCu) && get_sel(c, s)) load_seg(c, s, get_sel(c, s));
}
static int ss32(const B86Cpu *c) { return (c->pm386.seg[B86_SS].flags & 4u) != 0; }
static uint32_t stack_lin(const B86Cpu *c) { return c->pm386.seg[B86_SS].base + (ss32(c) ? c->r[B86_SP] : (c->r[B86_SP] & 0xFFFFu)); }
static void sp_add(B86Cpu *c, int32_t d)
{
    if (ss32(c)) c->r[B86_SP] += (uint32_t)d; else c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | ((c->r[B86_SP] + (uint32_t)d) & 0xFFFFu);
}

/* ---- LDT allocation ------------------------------------------------------------- */
static int ldt_alloc(unsigned n, uint16_t *first)
{
    for (unsigned i = 1; i + n <= LDT_N; ++i) {
        unsigned k; for (k = 0; k < n && !g_ldt_used[i + k]; ++k) ;
        if (k == n) { for (k = 0; k < n; ++k) { g_ldt_used[i + k] = 1; desc_write(g_hb + HB_LDT + 8u * (i + k), 0, 0, 0x92u, 0); }
                      *first = LDT_SEL(i); return 1; }
    }
    return 0;
}
static uint16_t ldt_data(uint32_t base, uint32_t limit, uint8_t flags)
{
    uint16_t s; if (!ldt_alloc(1, &s)) return 0;
    desc_write(ldt_addr(s), base, limit, 0x92u, flags); return s;
}

/* ---- linear pool ------------------------------------------------------------------- */
static uint32_t pool_largest(void)
{
    uint32_t best = 0; for (unsigned i = 0; i < g_nblk; ++i) if (!g_blk[i].used && g_blk[i].size > best) best = g_blk[i].size;
    return best;
}
static uint32_t pool_alloc(uint32_t size)
{
    size = (size + 0xFFFu) & ~0xFFFu;
    for (unsigned i = 0; i < g_nblk; ++i) if (!g_blk[i].used && g_blk[i].size >= size) {
        if (g_blk[i].size > size && g_nblk < 64u) {
            memmove(&g_blk[i + 2], &g_blk[i + 1], (g_nblk - i - 1u) * sizeof g_blk[0]); ++g_nblk;
            g_blk[i + 1].base = g_blk[i].base + size; g_blk[i + 1].size = g_blk[i].size - size; g_blk[i + 1].used = 0; g_blk[i].size = size;
        }
        g_blk[i].used = 1; return g_blk[i].base;
    }
    return 0;
}
static void pool_merge(void)
{
    for (unsigned i = 0; i + 1u < g_nblk; ) {
        if (!g_blk[i].used && !g_blk[i + 1].used) { g_blk[i].size += g_blk[i + 1].size; memmove(&g_blk[i + 1], &g_blk[i + 2], (g_nblk - i - 2u) * sizeof g_blk[0]); --g_nblk; }
        else ++i;
    }
}
static int pool_free(uint32_t base)
{
    for (unsigned i = 0; i < g_nblk; ++i) if (g_blk[i].used && g_blk[i].base == base) { g_blk[i].used = 0; pool_merge(); return 1; }
    return 0;
}
static unsigned pool_find(uint32_t base) { for (unsigned i = 0; i < g_nblk; ++i) if (g_blk[i].used && g_blk[i].base == base) return i; return 64u; }

/* ---- mode switching ----------------------------------------------------------------- */
static void enter_pm_tables(B86Cpu *c)
{
    c->gdt_base = g_hb + HB_GDT; c->gdt_limit = 0x7Fu;
    c->idt_base = g_hb + HB_IDT; c->idt_limit = 0x7FFu;
    c->msw |= 1u; c->trap_cs = SEL_HOST_CS;
    if (S->pd && !c->pg) { c->pg = 1; tlb_drop(c); }
    b86_pm1_prepare_ldtr(c, SEL_LDT);
}
static void to_real(B86Cpu *c, Saved *s)
{
    for (unsigned i = 0; i < 8u; ++i) { s->r[i] = c->r[i]; s->rhi[i] = c->rhi[i]; }
    s->ip = c->ip; s->flags = c->flags; for (unsigned i = 0; i < 6u; ++i) s->seg[i] = get_sel(c, i); s->pm = c->pm386;
    s->idt_base = c->idt_base; s->idt_limit = c->idt_limit;
    c->msw &= (uint16_t)~1u; c->idt_base = 0; c->idt_limit = 0x3FFu; c->trap_cs = E.rm_trap_cs;
    if (S->pd && E.pg_off_in_rm && c->pg) { c->pg = 0; tlb_drop(c); }
    for (unsigned i = 0; i < 6u; ++i) {
        uint32_t b = c->pm386.seg[i].valid ? c->pm386.seg[i].base : 0u;
        set_sel(c, i, b < 0x100000u ? (uint16_t)(b >> 4) : 0u);
    }
}
static void to_protected(B86Cpu *c, const Saved *s)
{
    c->msw |= 1u; c->idt_base = s->idt_base; c->idt_limit = s->idt_limit; c->trap_cs = SEL_HOST_CS;
    if (S->pd && !c->pg) { c->pg = 1; tlb_drop(c); }
    c->pm386 = s->pm; for (unsigned i = 0; i < 6u; ++i) set_sel(c, i, s->seg[i]);
    for (unsigned i = 0; i < 8u; ++i) { c->r[i] = s->r[i]; c->rhi[i] = s->rhi[i]; }
    c->ip = s->ip; c->flags = s->flags;
}

/* run a real-mode interrupt or far call; the CPU continues in real mode and
 * returns to the host through the DPMI_VEC_RMRET stub */
static void rm_call(B86Cpu *c, Saved *s, uint16_t cs, uint16_t ip, int is_int, const uint32_t *regs, const uint16_t *segs, uint32_t flags, const uint16_t *args, unsigned nargs)
{
    to_real(c, s);
    if (regs) for (unsigned i = 0; i < 8u; ++i) { c->r[i] = regs[i]; c->rhi[i] = 0; }
    if (segs) { set_sel(c, B86_DS, segs[0]); set_sel(c, B86_ES, segs[1]); set_sel(c, B86_FS, segs[2]); set_sel(c, B86_GS, segs[3]); }
    /* host real-mode stack unless the caller gave one */
    if (!segs || !(segs[4] | segs[5])) { b86_set_seg(c, B86_SS, g_hbseg); c->r[B86_SP] = HB_RMSTACK + 0x400u; }
    else { b86_set_seg(c, B86_SS, segs[4]); c->r[B86_SP] = segs[5]; }
    c->flags = (flags & 0xFFFFu) | 0x2u;
    uint32_t sp = ((uint32_t)c->seg[B86_SS] << 4) + (c->r[B86_SP] & 0xFFFFu), pushed = 0;
    for (unsigned i = nargs; i > 0; --i) { sp -= 2; wr16(sp, args[i - 1u]); pushed += 2; }   /* 0301h: CX words of arguments */
    if (is_int) { sp -= 2; wr16(sp, (uint16_t)c->flags); c->flags &= ~(uint32_t)(0x200u | 0x100u); pushed += 2; }
    sp -= 2; wr16(sp, E.stub_seg); sp -= 2; wr16(sp, (uint16_t)(E.stub_off + 3u)); pushed += 4;   /* return: INT E1 stub */
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | ((c->r[B86_SP] - pushed) & 0xFFFFu);
    c->ip = ip; b86_set_seg(c, B86_CS, cs);
    ++g_rmcalls;
}

/* ---- the IRET the host performs on the client's behalf ----------------------------- */
static void host_iret(B86Cpu *c, uint32_t frame, uint32_t flags_from, uint32_t flags_mask)
{
    uint32_t ip = rd32(frame), cs = rd32(frame + 4), fl = rd32(frame + 8);
    fl = (fl & ~flags_mask) | (flags_from & flags_mask);
    sp_add(c, 12);
    c->flags = (fl & 0x257FD5u) | 0x2u;          /* no VM/RF changes from here */
    c->ip = ip; load_cs(c, (uint16_t)cs);
}

/* ---- virtual memory ------------------------------------------------------------------- */
#define VM_PRESENT 0x007u                 /* present, writable, user */
#define VM_SWAPPED 0x200u                 /* not present; bits 12-31 = swap slot + 1 */
static uint32_t pte_addr(uint32_t lin) { return prd32(S->pd + ((lin >> 22) << 2)) & 0xFFFFF000u ? (prd32(S->pd + ((lin >> 22) << 2)) & 0xFFFFF000u) + (((lin >> 12) & 0x3FFu) << 2) : 0u; }
static void tlb_drop(B86Cpu *c) { if (c) b86_pg_flush_public(c); }
static int slot_alloc(void)
{
    for (uint32_t i = 0; i < S->nslots; ++i) if (!(S->slot_used[i >> 3] & (1u << (i & 7u)))) { S->slot_used[i >> 3] |= (uint8_t)(1u << (i & 7u)); return (int)i; }
    return -1;
}
static void slot_free(uint32_t i) { if (i < S->nslots) S->slot_used[i >> 3] &= (uint8_t)~(1u << (i & 7u)); }
/* a frame for a new page: free one, else evict by clock (second chance on the A bit) */
static int frame_get(B86Cpu *c, uint32_t *phys)
{
    for (uint32_t k = 0; k < S->nframes; ++k) if (!S->frame_vpn[k]) { *phys = S->frame0 + (k << 12); return (int)k; }
    if (!E.swap_write) { ++S->evict_fail; return -1; }
    for (uint32_t pass = 0; pass < 2u * S->nframes; ++pass) {
        uint32_t k = S->hand; S->hand = (S->hand + 1u) % S->nframes;
        if (!S->frame_vpn[k]) { *phys = S->frame0 + (k << 12); return (int)k; }
        uint32_t lin = (S->frame_vpn[k] - 1u) << 12, pa = pte_addr(lin), pte = prd32(pa);
        if (pte & 0x20u) { pwr32(pa, pte & ~0x20u); continue; }          /* accessed: second chance */
        int slot = slot_alloc(); if (slot < 0) { ++S->evict_fail; return -1; }
        uint32_t fr = S->frame0 + (k << 12);
        memcpy(S->page, E.mem + fr, 4096u);
        if (!E.swap_write((uint32_t)slot, S->page)) { slot_free((uint32_t)slot); ++S->evict_fail; return -1; }
        pwr32(pa, ((uint32_t)(slot + 1) << 12) | VM_SWAPPED);
        S->frame_vpn[k] = 0; ++S->swap_outs;
        tlb_drop(c);
        *phys = fr; return (int)k;
    }
    ++S->evict_fail; return -1;
}
static int vm_allocated(uint32_t lin)
{
    for (unsigned i = 0; i < g_nblk; ++i) if (g_blk[i].used && lin >= g_blk[i].base && lin < g_blk[i].base + g_blk[i].size) return 1;
    return 0;
}
static int vm_fault(B86Cpu *c, uint32_t lin, uint32_t err)
{
    (void)err;
    if (!S || !S->pd || lin < S->vbase || lin >= S->vend || !vm_allocated(lin)) return 0;
    uint32_t pa = pte_addr(lin); if (!pa) return 0;
    uint32_t pte = prd32(pa);
    if (pte & 1u) { tlb_drop(c); return 1; }                       /* stale TLB entry: retry */
    uint32_t phys; int k = frame_get(c, &phys); if (k < 0) return 0;
    pte = prd32(pa);                                                  /* eviction may not touch this PTE, but re-read */
    if (pte & VM_SWAPPED) {
        uint32_t slot = (pte >> 12) - 1u;
        if (!E.swap_read || !E.swap_read(slot, S->page)) return 0;
        memcpy(E.mem + phys, S->page, 4096u); slot_free(slot); ++S->swap_ins;
    } else { memset(E.mem + phys, 0, 4096u); ++S->zero_fills; }
    pwr32(pa, phys | VM_PRESENT);
    S->frame_vpn[k] = (lin >> 12) + 1u; ++S->faults;
    return 1;
}
static int vm_pf_hook(B86Cpu *c, uint32_t lin, uint32_t err)
{
    int ok = vm_fault(c, lin, err);
    if (!ok && S && ++S->declined <= 16u)                  /* the client gets the #PF: say why */
        DPMI_LOG("[dpmi] page fault at %08lX err=%lX NOT resolved (%s) -> client #PF\n", (unsigned long)lin, (unsigned long)err,
                 !S->pd ? "no paging" : (lin < S->vbase || lin >= S->vend) ? "outside the virtual region" :
                 !vm_allocated(lin) ? "not in an allocated block" : S->evict_fail ? "eviction/swap failed" : "swap read failed");
    return ok;
}
#define g_cpu (S->cpu)
static uint32_t xlat(uint32_t a)
{
    if (!S || !S->pd || a < S->vbase || a >= S->vend) return a & (B86_RAM_BYTES - 1u);
    for (int tries = 0; tries < 2; ++tries) {
        uint32_t pa = pte_addr(a); if (!pa) break;
        uint32_t pte = prd32(pa);
        if (pte & 1u) return (pte & 0xFFFFF000u) | (a & 0xFFFu);
        if (!vm_fault(g_cpu, a, 0)) break;
    }
    if (E.trace) DPMI_LOG("[dpmi] host access to unmapped %08lX\n", (unsigned long)a);
    return 0;                             /* unmapped: read the null page (logged) */
}
/* release the pages of [base, base+size): frames and swap slots, PTEs cleared */
static void vm_release(B86Cpu *c, uint32_t base, uint32_t size)
{
    for (uint32_t lin = base; lin < base + size; lin += 4096u) {
        uint32_t pa = pte_addr(lin); if (!pa) continue;
        uint32_t pte = prd32(pa);
        if (pte & 1u) { uint32_t k = ((pte & 0xFFFFF000u) - S->frame0) >> 12; if (k < S->nframes) S->frame_vpn[k] = 0; }
        else if (pte & VM_SWAPPED) slot_free((pte >> 12) - 1u);
        pwr32(pa, 0);
    }
    tlb_drop(c);
}
/* move the pages of a block to a new linear base (PTEs only, no data copy) */
static void vm_move(B86Cpu *c, uint32_t from, uint32_t to, uint32_t size)
{
    for (uint32_t off = 0; off < size; off += 4096u) {
        uint32_t pf = pte_addr(from + off), pt = pte_addr(to + off); if (!pf || !pt) continue;
        uint32_t pte = prd32(pf); pwr32(pt, pte); pwr32(pf, 0);
        if (pte & 1u) { uint32_t k = ((pte & 0xFFFFF000u) - S->frame0) >> 12; if (k < S->nframes) S->frame_vpn[k] = ((to + off) >> 12) + 1u; }
    }
    tlb_drop(c);
}
/* build page tables in the first frames of the pool and switch paging on */
static void vm_start(B86Cpu *c)
{
    uint32_t vsz = E.vm_size ? E.vm_size : 16u << 20;
    if (vsz > 16u << 20) vsz = 16u << 20;
    uint32_t nvpt = (vsz + 0x3FFFFFu) >> 22;
    uint32_t base = (E.ext_base + 0xFFFu) & ~0xFFFu;
    S->pd = base; uint32_t ipt = base + 4096u, vpt = base + 8192u;
    S->frame0 = vpt + nvpt * 4096u;
    S->nframes = (E.ext_end - S->frame0) >> 12; if (S->nframes > 1024u) S->nframes = 1024u;
    S->vbase = 0x00400000u; S->vend = S->vbase + vsz;
    S->nslots = E.swap_write ? (vsz >> 12) : 0u; if (S->nslots > 4096u) S->nslots = 4096u;
    memset(E.mem + S->pd, 0, (2u + nvpt) * 4096u);
    for (uint32_t pg = 0; pg < (E.ext_base >> 12); ++pg) pwr32(ipt + 4u * pg, (pg << 12) | VM_PRESENT);   /* identity: real-mode memory, HMA, host block */
    pwr32(S->pd, ipt | VM_PRESENT);
    for (uint32_t i = 0; i < nvpt; ++i) pwr32(S->pd + ((S->vbase >> 22) + i) * 4u, (vpt + 4096u * i) | VM_PRESENT);
    g_nblk = 1; g_blk[0].base = S->vbase; g_blk[0].size = vsz; g_blk[0].used = 0;
    c->cr3 = S->pd; c->pg = 1u; c->pf_hook = vm_pf_hook; tlb_drop(c);
    if (E.trace) DPMI_LOG("[dpmi] VM: %lu frames at %08lX, virtual %08lX-%08lX, %lu swap slots\n", (unsigned long)S->nframes, (unsigned long)S->frame0, (unsigned long)S->vbase, (unsigned long)S->vend, (unsigned long)S->nslots);
}

/* ---- INT 31h ------------------------------------------------------------------------- */
#define AX  (c->r[0] & 0xFFFFu)
#define SETAX(v) (c->r[0] = (c->r[0] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SETCX(v) (c->r[1] = (c->r[1] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SETDX(v) (c->r[2] = (c->r[2] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SETBX(v) (c->r[3] = (c->r[3] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SETSI(v) (c->r[6] = (c->r[6] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SETDI(v) (c->r[7] = (c->r[7] & 0xFFFF0000u) | ((v) & 0xFFFFu))

static int fail(B86Cpu *c, uint16_t code) { SETAX(code); return 1; }   /* returns CF value */

static uint32_t es_edi_lin(const B86Cpu *c)
{
    const B86PM1Cache *k = &c->pm386.seg[B86_ES];
    return k->base + ((k->flags & 4u) ? c->r[B86_DI] : (c->r[B86_DI] & 0xFFFFu));
}

/* 0300h-0302h: start a real-mode call from the register structure at ES:EDI;
 * completes in rmret() */
static void start_rm_struct(B86Cpu *c, unsigned fn, uint32_t frame)
{
    Saved *s = &g_nest[g_depth++];
    uint32_t st = es_edi_lin(c);
    uint32_t regs[8]; uint16_t segs[6];
    regs[7] = rd32(st); regs[6] = rd32(st + 4); regs[5] = rd32(st + 8); regs[3] = rd32(st + 16);
    regs[2] = rd32(st + 20); regs[1] = rd32(st + 24); regs[0] = rd32(st + 28);
    regs[4] = rd16(st + 46);                                                        /* SP */
    uint32_t fl = rd16(st + 32);
    segs[1] = rd16(st + 34); segs[0] = rd16(st + 36); segs[2] = rd16(st + 38); segs[3] = rd16(st + 40);
    segs[4] = rd16(st + 48); segs[5] = rd16(st + 46);
    s->kind = fn == 0x300u ? 2 : 3; s->vector = (uint8_t)c->r[3]; s->frame_lin = frame; s->arg = st;
    uint16_t cs, ip;
    if (fn == 0x300u) { uint8_t v = (uint8_t)c->r[3]; cs = rd16(4u * v + 2u); ip = rd16(4u * v); }
    else { cs = rd16(st + 44); ip = rd16(st + 42); }
    /* CX words of arguments copied from the protected-mode stack above the INT 31h frame */
    unsigned ncopy = c->r[1] & 0xFFFFu; if (ncopy > 64u) ncopy = 64u;
    uint16_t args[64]; for (unsigned i = 0; i < ncopy; ++i) args[i] = rd16(frame + 12u + 2u * i);
    rm_call(c, s, cs, ip, fn == 0x300u || fn == 0x302u, regs, segs, fl, args, ncopy);
}

static int int31(B86Cpu *c, uint32_t frame, int *async)
{
    unsigned fn = AX;
    ++g_calls[FNI(fn)];
    if (E.trace) DPMI_LOG("[dpmi] 31h %04X bx=%08X cx=%08X dx=%08X si=%08X di=%08X\n", fn, c->r[3], c->r[1], c->r[2], c->r[6], c->r[7]);
    switch (fn) {
    case 0x0000: { uint16_t s; unsigned n = c->r[1] & 0xFFFFu; if (!n || !ldt_alloc(n, &s)) return fail(c, E_DESC_UNAVAIL); SETAX(s); return 0; }
    case 0x0001: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); g_ldt_used[s >> 3] = 0; return 0; }
    case 0x0002: { uint16_t sg = (uint16_t)c->r[3];
        for (unsigned i = 0; i < g_seg2desc_n; ++i) if (g_seg2desc_seg[i] == sg) { SETAX(g_seg2desc_sel[i]); return 0; }
        uint16_t s = ldt_data((uint32_t)sg << 4, 0xFFFFu, 0); if (!s) return fail(c, E_DESC_UNAVAIL);
        if (g_seg2desc_n < 32u) { g_seg2desc_seg[g_seg2desc_n] = sg; g_seg2desc_sel[g_seg2desc_n++] = s; }
        SETAX(s); return 0; }
    case 0x0003: SETAX(8); return 0;
    case 0x0006: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t b = desc_base(ldt_addr(s)); SETCX(b >> 16); SETDX(b); return 0; }
    case 0x0007: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t a = ldt_addr(s), b = ((c->r[1] & 0xFFFFu) << 16) | (c->r[2] & 0xFFFFu);
        wr16(a + 2, (uint16_t)b); wr8(a + 4, (uint8_t)(b >> 16)); wr8(a + 7, (uint8_t)(b >> 24)); reload_if_loaded(c, s); return 0; }
    case 0x0008: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t a = ldt_addr(s), l = ((c->r[1] & 0xFFFFu) << 16) | (c->r[2] & 0xFFFFu);
        if (l > 0xFFFFFu && (l & 0xFFFu) != 0xFFFu) return fail(c, E_INVALID_VALUE);
        uint8_t g = 0; if (l > 0xFFFFFu) { g = 0x80u; l >>= 12; }
        wr16(a, (uint16_t)l); wr8(a + 6, (uint8_t)((rd8(a + 6) & 0x70u) | g | ((l >> 16) & 0xFu))); reload_if_loaded(c, s); return 0; }
    case 0x0009: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t a = ldt_addr(s);
        uint8_t acc = (uint8_t)(c->r[1] & 0xFFu), ext = (uint8_t)((c->r[1] >> 8) & 0xFFu);
        if (!(acc & 0x10u)) return fail(c, E_INVALID_VALUE);
        wr8(a + 5, (uint8_t)(acc | 0x60u)); wr8(a + 6, (uint8_t)((ext & 0xD0u) | (rd8(a + 6) & 0x0Fu))); reload_if_loaded(c, s); return 0; }
    case 0x000A: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t a = ldt_addr(s);
        uint16_t n = ldt_data(desc_base(a), desc_limit(a), (uint8_t)(rd8(a + 6) & 0x40u)); if (!n) return fail(c, E_DESC_UNAVAIL); SETAX(n); return 0; }
    case 0x000B: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t d = es_edi_lin(c), a = ldt_addr(s); for (unsigned i = 0; i < 8u; ++i) wr8(d + i, rd8(a + i)); return 0; }
    case 0x000C: { uint16_t s = (uint16_t)c->r[3]; if (!sel_ok(s)) return fail(c, E_INVALID_SELECTOR); uint32_t d = es_edi_lin(c), a = ldt_addr(s); for (unsigned i = 0; i < 8u; ++i) wr8(a + i, rd8(d + i)); wr8(a + 5, (uint8_t)(rd8(a + 5) | 0x60u)); reload_if_loaded(c, s); return 0; }
    case 0x0100: case 0x0101: case 0x0102: {            /* DOS memory through real-mode INT 21h */
        Saved *s = &g_nest[g_depth++]; uint32_t regs[8] = {0}; uint16_t segs[6] = {0};
        s->kind = (uint8_t)(4u + (fn - 0x100u)); s->frame_lin = frame; s->arg = c->r[3] & 0xFFFFu;
        if (fn == 0x100u) { regs[0] = 0x4800u; regs[3] = c->r[3] & 0xFFFFu; }
        else { uint16_t sel = (uint16_t)c->r[2]; if (!sel_ok(sel)) { --g_depth; return fail(c, E_INVALID_SELECTOR); }
               uint32_t b = desc_base(ldt_addr(sel)); segs[1] = (uint16_t)(b >> 4); s->arg = sel;
               regs[0] = fn == 0x101u ? 0x4900u : 0x4A00u; regs[3] = c->r[3] & 0xFFFFu; }
        rm_call(c, s, rd16(4u * 0x21u + 2u), rd16(4u * 0x21u), 1, regs, segs, c->flags, 0, 0); *async = 1; return 0; }
    case 0x0200: { unsigned v = c->r[3] & 0xFFu; SETCX(rd16(4u * v + 2u)); SETDX(rd16(4u * v)); return 0; }
    case 0x0201: { unsigned v = c->r[3] & 0xFFu; wr16(4u * v, (uint16_t)c->r[2]); wr16(4u * v + 2u, (uint16_t)c->r[1]); return 0; }
    case 0x0202: { unsigned v = c->r[3] & 0xFFu; if (v >= 32u) return fail(c, E_INVALID_VALUE); SETCX(g_exc_sel[v]); c->r[2] = g_exc_off[v]; return 0; }
    case 0x0203: { unsigned v = c->r[3] & 0xFFu; if (v >= 32u) return fail(c, E_INVALID_VALUE); g_exc_sel[v] = c->r[1] & 0xFFFFu; g_exc_off[v] = c->r[2]; return 0; }
    case 0x0204: { unsigned v = c->r[3] & 0xFFu; SETCX(g_pm_vec_sel[v]); c->r[2] = g_pm_vec_off[v]; return 0; }
    case 0x0205: { unsigned v = c->r[3] & 0xFFu; g_pm_vec_sel[v] = c->r[1] & 0xFFFFu; g_pm_vec_off[v] = c->r[2];
        if (g_pm_vec_sel[v] == SEL_HOST_CS) gate_write(v, SEL_HOST_CS, 4u * v); else gate_write(v, (uint16_t)g_pm_vec_sel[v], g_pm_vec_off[v]); return 0; }
    case 0x0300: case 0x0301: case 0x0302: start_rm_struct(c, fn, frame); *async = 1; return 0;
    case 0x0303: { /* allocate real-mode callback: DS:ESI = PM procedure, ES:EDI = call structure */
        unsigned i; for (i = 0; i < CB_N && g_cb[i].used; ++i) ;
        if (i == CB_N) return fail(c, 0x8015u);
        g_cb[i].used = 1; g_cb[i].cs = get_sel(c, B86_DS); g_cb[i].eip = c->r[6]; g_cb[i].es = get_sel(c, B86_ES); g_cb[i].edi = c->r[7];
        SETCX(g_hbseg); SETDX(HB_CBSTUB + 4u * i); return 0; }
    case 0x0304: { unsigned i, dx = c->r[2] & 0xFFFFu; if ((c->r[1] & 0xFFFFu) != g_hbseg || dx < HB_CBSTUB || dx >= HB_CBSTUB + 4u * CB_N) return fail(c, 0x8024u);
        i = (dx - HB_CBSTUB) / 4u; g_cb[i].used = 0; return 0; }
    case 0x0305: /* state save/restore: nothing to save; cannot fail */
        SETAX(0); SETBX(E.stub_seg); SETCX(E.stub_off + 8u); SETSI(SEL_HOST_CS); c->r[7] = HC_SAVE; return 0;
    case 0x0306: /* raw mode switch addresses; cannot fail */
        SETBX(E.stub_seg); SETCX(E.stub_off + 6u); SETSI(SEL_HOST_CS); c->r[7] = HC_RAWRM; return 0;
    case 0x0400: SETAX(0x005Au); SETBX(0x0001u); SETCX(3); SETDX(0x0870u); return 0;
    case 0x0500: { uint32_t d = es_edi_lin(c), largest = pool_largest(), total = E.ext_end - E.ext_base, freeb = 0;
        for (unsigned i = 0; i < g_nblk; ++i) if (!g_blk[i].used) freeb += g_blk[i].size;
        for (unsigned i = 0; i < 12u; ++i) wr32(d + 4u * i, 0xFFFFFFFFu);
        if (S->pd) total = S->vend - S->vbase;
        wr32(d, largest); wr32(d + 4, largest >> 12); wr32(d + 8, largest >> 12); wr32(d + 12, total >> 12);
        wr32(d + 16, freeb >> 12); wr32(d + 20, freeb >> 12); wr32(d + 24, total >> 12); wr32(d + 28, freeb >> 12); wr32(d + 32, 0); return 0; }
    case 0x0501: { uint32_t size = ((c->r[3] & 0xFFFFu) << 16) | (c->r[1] & 0xFFFFu), b; if (!size || !(b = pool_alloc(size))) return fail(c, E_LINEAR_UNAVAIL);
        SETBX(b >> 16); SETCX(b); SETSI(b >> 16); SETDI(b); return 0; }
    case 0x0502: { uint32_t h = ((c->r[6] & 0xFFFFu) << 16) | (c->r[7] & 0xFFFFu); unsigned i = pool_find(h);
        if (i == 64u) return fail(c, E_INVALID_HANDLE);
        if (S->pd) vm_release(c, h, g_blk[i].size);
        pool_free(h); return 0; }
    case 0x0503: { uint32_t h = ((c->r[6] & 0xFFFFu) << 16) | (c->r[7] & 0xFFFFu), size = ((c->r[3] & 0xFFFFu) << 16) | (c->r[1] & 0xFFFFu);
        unsigned i = pool_find(h); if (i == 64u) return fail(c, E_INVALID_HANDLE);
        size = (size + 0xFFFu) & ~0xFFFu; uint32_t old = g_blk[i].size, nb;
        if (size <= old) {
            if (size < old && S->pd) vm_release(c, h + size, old - size);
            if (size < old && g_nblk < 64u) { memmove(&g_blk[i + 2], &g_blk[i + 1], (g_nblk - i - 1u) * sizeof g_blk[0]); ++g_nblk; g_blk[i + 1].base = h + size; g_blk[i + 1].size = old - size; g_blk[i + 1].used = 0; g_blk[i].size = size; pool_merge(); } nb = h; }
        else if (i + 1u < g_nblk && !g_blk[i + 1].used && g_blk[i + 1].size >= size - old) { uint32_t take = size - old; g_blk[i + 1].base += take; g_blk[i + 1].size -= take; g_blk[i].size = size; if (!g_blk[i + 1].size) { memmove(&g_blk[i + 1], &g_blk[i + 2], (g_nblk - i - 2u) * sizeof g_blk[0]); --g_nblk; } nb = h; }
        else { nb = pool_alloc(size); if (!nb) return fail(c, E_LINEAR_UNAVAIL);
               if (S->pd) vm_move(c, h, nb, old); else memmove(E.mem + nb, E.mem + h, old); pool_free(h); }
        SETBX(nb >> 16); SETCX(nb); SETSI(nb >> 16); SETDI(nb); return 0; }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: return 0;
    case 0x0700: case 0x0701: case 0x0702: case 0x0703: return 0;   /* paging hints: no paging here */
    case 0x0604: SETBX(0); SETCX(4096); return 0;
    case 0x0900: { unsigned was = (c->flags >> 9) & 1u; c->flags &= ~0x200u; wr32(frame + 8, rd32(frame + 8) & ~0x200u); SETAX((AX & 0xFF00u) | was); return 0; }
    case 0x0901: { unsigned was = (c->flags >> 9) & 1u; wr32(frame + 8, rd32(frame + 8) | 0x200u); SETAX((AX & 0xFF00u) | was); return 0; }
    case 0x0902: SETAX((AX & 0xFF00u) | ((rd32(frame + 8) >> 9) & 1u)); return 0;
    case 0x0E00: SETAX(0x000Du); return 0;         /* coprocessor: MP, EM off, present */
    case 0x0E01: return 0;
    default: ++g_unsupported[FNI(fn)]; if (E.trace) DPMI_LOG("[dpmi] 31h %04X UNSUPPORTED\n", fn); return fail(c, E_UNSUPPORTED);
    }
}

/* ---- entry from real mode -------------------------------------------------------------- */
static void entry(B86Cpu *c, uint16_t psp)
{
    uint32_t sp = ((uint32_t)c->seg[B86_SS] << 4) + (c->r[B86_SP] & 0xFFFFu);
    uint16_t ret_ip = rd16(sp), ret_cs = rd16(sp + 2);
    int client32 = (c->r[0] & 1u) != 0; g_client32 = client32;
    g_hbseg = c->seg[B86_ES]; g_hb = (uint32_t)g_hbseg << 4;
    memset(E.mem + g_hb, 0, HB_SIZE);
    memset(E.mem + g_hb + HB_CODE, 0xF4, 0x800);                                   /* HLT: never executed */
    for (unsigned i = 0; i < CB_N; ++i) { uint32_t a = g_hb + HB_CBSTUB + 4u * i; wr8(a, 0xCDu); wr8(a + 1, DPMI_VEC_CB); wr8(a + 2, (uint8_t)i); wr8(a + 3, 0x90u); }
    memset(g_cb, 0, sizeof g_cb); g_rmss_sel = 0;
    desc_write(g_hb + HB_GDT + 8u * 1u, g_hb + HB_CODE, 0x7FFu, 0x9Au, 0);
    desc_write(g_hb + HB_GDT + 8u * 2u, g_hb, 0xFFFFu, 0x92u, 0);
    desc_write(g_hb + HB_GDT + 8u * 3u, 0, 0xFFFFFFFFu, 0x92u, 0x40u);
    desc_write(g_hb + HB_GDT + 8u * 4u, g_hb + HB_LDT, 0xFFFu, 0x82u, 0);
    desc_write(g_hb + HB_GDT + 8u * 5u, g_hb + HB_TSS, 0x67u, 0x89u, 0);
    desc_write(g_hb + HB_GDT + 8u * 8u, 0x400u, 0xFFFFu, 0xF2u, 0);              /* selector 0040h: BIOS data area, DPL 3 */
    memset(g_ldt_used, 0, sizeof g_ldt_used); g_seg2desc_n = 0;
    for (unsigned v = 0; v < 256u; ++v) { gate_write(v, SEL_HOST_CS, 4u * v); g_pm_vec_sel[v] = SEL_HOST_CS; g_pm_vec_off[v] = 4u * v; }
    g_nblk = 0;                                                                   /* the pool is set up by vm_start */
    /* client selectors: CS, DS, SS from the real-mode segments; ES = PSP */
    /* CS is the 16-bit code that called us; a 32-bit client only gets a 32-bit SS (ESP zero-extended) */
    uint16_t cs = ldt_data((uint32_t)ret_cs << 4, 0xFFFFu, 0); wr8(ldt_addr(cs) + 5, 0xFAu);
    uint16_t ds = ldt_data((uint32_t)c->seg[B86_DS] << 4, 0xFFFFu, 0);
    uint16_t ss = ldt_data((uint32_t)c->seg[B86_SS] << 4, 0xFFFFu, client32 ? 0x40u : 0u);
    E.psp = psp;
    uint16_t es = ldt_data((uint32_t)E.psp << 4, 0xFFu, 0);
    {   /* the PSP's environment segment becomes a selector (DPMI 0.9, entry requirements) */
        uint16_t envseg = rd16(((uint32_t)E.psp << 4) + 0x2Cu);
        if (envseg) { uint16_t envsel = ldt_data((uint32_t)envseg << 4, 0xFFFFu, 0); wr16(((uint32_t)E.psp << 4) + 0x2Cu, envsel); }
    }
    c->r[B86_SP] = (c->r[B86_SP] + 4u) & 0xFFFFu;                                  /* pop the far return; ESP zero-extended */
    c->priv_relaxed = 1;                                                           /* ring-3 selectors at the emulator's CPL 0 */
    enter_pm_tables(c);
    vm_start(c);                                                                   /* paging on, for good: identity below ext_base */
    load_cs(c, cs); load_seg(c, B86_DS, ds); load_seg(c, B86_SS, ss); load_seg(c, B86_ES, es);
    load_seg(c, B86_FS, 0); load_seg(c, B86_GS, 0);
    c->ip = ret_ip; c->flags = (c->flags & ~1u) | 0x3000u;                          /* CF=0: success; IOPL 3 for the ring-3 client */
    g_active = 1; g_entered = 1;
    if (E.trace) DPMI_LOG("[dpmi] entered protected mode: client %s-bit, CS=%04X DS=%04X SS=%04X ES=%04X at %04X:%04X\n", client32 ? "32" : "16", cs, ds, ss, es, cs, ret_ip);
}

/* ---- return from real mode (the INT E1 stub) -------------------------------------------- */
static void rmret(B86Cpu *c)
{
    if (!g_depth) return;
    Saved *s = &g_nest[--g_depth];
    uint32_t rr[8]; uint16_t rs[6]; uint32_t rfl = c->flags;
    for (unsigned i = 0; i < 8u; ++i) rr[i] = c->r[i];
    for (unsigned i = 0; i < 6u; ++i) rs[i] = get_sel(c, i);
    to_protected(c, s);
    if (E.trace) DPMI_LOG("[dpmi] rmret kind=%u frame=%08X esp=%08X ss=%04X ssflags=%02X rm-flags=%04X\n", s->kind, s->frame_lin, c->r[B86_SP], get_sel(c, B86_SS), c->pm386.seg[B86_SS].flags, (unsigned)rfl);
    switch (s->kind) {
    case 1:  /* reflected interrupt: registers and arithmetic flags come back from real mode */
        for (unsigned i = 0; i < 8u; ++i) if (i != B86_SP) c->r[i] = rr[i];
        host_iret(c, s->frame_lin, rfl, 0x8D5u);
        break;
    case 2: case 3: {  /* 0300h-0302h: results into the structure, CF=0 to the INT 31h caller */
        uint32_t st = s->arg;
        wr32(st, rr[7]); wr32(st + 4, rr[6]); wr32(st + 8, rr[5]); wr32(st + 16, rr[3]); wr32(st + 20, rr[2]); wr32(st + 24, rr[1]); wr32(st + 28, rr[0]);
        wr16(st + 32, (uint16_t)rfl); wr16(st + 34, rs[B86_ES]); wr16(st + 36, rs[B86_DS]); wr16(st + 38, rs[B86_FS]); wr16(st + 40, rs[B86_GS]);
        host_iret(c, s->frame_lin, 0, 1u);
        break; }
    case 4: {  /* DOS allocate */
        if (rfl & 1u) { SETAX(rr[0] & 0xFFu ? 0x8000u | (rr[0] & 0xFFu) : 0x8011u); SETBX(rr[3]); host_iret(c, s->frame_lin, 1u, 1u); break; }
        uint32_t paras = s->arg, bytes = paras << 4; unsigned n = (unsigned)((bytes + 0xFFFFu) >> 16); if (!n) n = 1;
        uint16_t first; if (!ldt_alloc(n, &first)) { SETAX(E_DESC_UNAVAIL); host_iret(c, s->frame_lin, 1u, 1u); break; }
        for (unsigned i = 0; i < n; ++i) { uint32_t left = bytes - (i << 16); desc_write(ldt_addr((uint16_t)(first + 8u * i)), ((uint32_t)rr[0] << 4) + (i << 16), left > 0x10000u ? 0xFFFFu : left - 1u, 0x92u, 0); }
        SETAX(rr[0]); SETDX(first); host_iret(c, s->frame_lin, 0, 1u); break; }
    case 5: {  /* DOS free */
        if (rfl & 1u) { SETAX(0x8000u | (rr[0] & 0xFFu)); host_iret(c, s->frame_lin, 1u, 1u); break; }
        uint16_t sel = (uint16_t)s->arg; uint32_t b = desc_base(ldt_addr(sel)), l = desc_limit(ldt_addr(sel)); (void)b; (void)l;
        g_ldt_used[sel >> 3] = 0; host_iret(c, s->frame_lin, 0, 1u); break; }
    case 6: {  /* DOS resize */
        if (rfl & 1u) { SETAX(0x8000u | (rr[0] & 0xFFu)); SETBX(rr[3]); host_iret(c, s->frame_lin, 1u, 1u); break; }
        host_iret(c, s->frame_lin, 0, 1u); break; }
    default: host_iret(c, s->frame_lin, 0, 0); break;
    }
    if (E.trace) DPMI_LOG("[dpmi] rmret done: cs=%04X eip=%08X esp=%08X flags=%08X\n", get_sel(c, B86_CS), c->ip, c->r[B86_SP], c->flags);
}

/* ---- public ------------------------------------------------------------------------------ */
DPMI_COLD void dpmi_init(DpmiEnv *env)
{
    if (!S) S = (DpmiS *)(env->alloc ? env->alloc(1, sizeof(DpmiS)) : 0);
    if (!S) return;
    memset(S, 0, sizeof *S);
    E = *env; g_active = 0; g_depth = 0;
    uint32_t st = ((uint32_t)E.stub_seg << 4) + E.stub_off;
    /* +0 INT E0; RETF (entry)  +3 INT E1 (return from real mode)  +6 INT E2 (raw switch to PM)  +8 RETF (save/restore) */
    /* +9 INT E4 (entry continues with the PSP in BX) */
    const uint8_t stub[] = { 0xCDu, DPMI_VEC_ENTRY, 0xCBu, 0xCDu, DPMI_VEC_RMRET, 0xF4u, 0xCDu, DPMI_VEC_RAWPM, 0xCBu, 0xCDu, DPMI_VEC_PSP, 0xF4u };
    memcpy(E.mem + st, stub, sizeof stub);
}
DPMI_COLD uint16_t dpmi_host_cs(void) { return SEL_HOST_CS; }
DPMI_COLD int dpmi_active(void) { return S && g_active; }
DPMI_COLD int dpmi_ready(void) { return S != 0; }

DPMI_COLD int dpmi_rm_int(B86Cpu *c, uint8_t vector)
{
    if (!S) return 0;
    g_cpu = c;
    if (vector == 0x2Fu) {
        unsigned ax = c->r[0] & 0xFFFFu;
        if (ax == 0x1687u) {
            SETAX(0); SETBX(1); SETCX(3); SETDX(0x005Au); SETSI(HB_PARAS);
            b86_set_seg(c, B86_ES, E.stub_seg); SETDI(E.stub_off);
            if (E.trace) DPMI_LOG("[dpmi] INT 2Fh 1687h: host present, %u paragraphs\n", HB_PARAS);
            return 1;
        }
        if (ax == 0x1686u) { SETAX(1); return 1; }   /* not in protected mode (real-mode caller) */
        return 0;
    }
    if (vector == DPMI_VEC_ENTRY) {               /* first ask DOS for the current PSP (INT 21h AH=62h) */
        for (unsigned i = 0; i < 8u; ++i) S->pend_r[i] = c->r[i];
        S->pend_flags = c->flags; for (unsigned i = 0; i < 4u; ++i) S->pend_seg[i] = (uint16_t)c->seg[i];
        S->pend_ip = (uint16_t)c->ip;
        uint32_t sp = ((uint32_t)c->seg[B86_SS] << 4) + (c->r[B86_SP] & 0xFFFFu);
        sp -= 2; wr16(sp, (uint16_t)c->flags); sp -= 2; wr16(sp, E.stub_seg); sp -= 2; wr16(sp, (uint16_t)(E.stub_off + 9u));
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | ((c->r[B86_SP] - 6u) & 0xFFFFu);
        c->r[0] = (c->r[0] & 0xFFFF0000u) | 0x6200u; c->flags &= ~0x300u;
        c->ip = rd16(4u * 0x21u); set_sel(c, B86_CS, rd16(4u * 0x21u + 2u));
        return 1;
    }
    if (vector == DPMI_VEC_PSP) {                 /* INT 21h returned: BX = PSP; restore the caller and enter */
        uint16_t psp = (uint16_t)c->r[3];
        for (unsigned i = 0; i < 8u; ++i) c->r[i] = S->pend_r[i];
        c->flags = S->pend_flags; for (unsigned i = 0; i < 4u; ++i) set_sel(c, i, S->pend_seg[i]);
        c->ip = S->pend_ip;
        entry(c, psp); return 1;
    }
    if (vector == DPMI_VEC_RMRET) { rmret(c); return 1; }
    if (vector == DPMI_VEC_CB && c->seg[B86_CS] == g_hbseg) {   /* real-mode callback: IP is past "INT E3"; the slot byte follows */
        unsigned i = rd8(((uint32_t)c->seg[B86_CS] << 4) + c->ip);
        if (i >= CB_N || !g_cb[i].used || g_depth >= 8u) return 1;
        Saved *s = &g_nest[g_depth++]; s->kind = 7; s->vector = (uint8_t)i;
        /* real-mode registers into the client's structure */
        uint32_t st = 0; { B86PM1Cache k; cache_from_desc(&k, g_cb[i].es, ldt_addr(g_cb[i].es)); st = k.base + ((k.flags & 4u) ? g_cb[i].edi : (g_cb[i].edi & 0xFFFFu)); }
        wr32(st, c->r[7]); wr32(st + 4, c->r[6]); wr32(st + 8, c->r[5]); wr32(st + 16, c->r[3]); wr32(st + 20, c->r[2]); wr32(st + 24, c->r[1]); wr32(st + 28, c->r[0]);
        wr16(st + 32, (uint16_t)c->flags); wr16(st + 34, c->seg[B86_ES]); wr16(st + 36, c->seg[B86_DS]); wr16(st + 38, c->seg386[0]); wr16(st + 40, c->seg386[1]);
        wr16(st + 42, (uint16_t)c->ip); wr16(st + 44, c->seg[B86_CS]); wr16(st + 46, (uint16_t)c->r[B86_SP]); wr16(st + 48, c->seg[B86_SS]);
        s->arg = st;
        for (unsigned k = 0; k < 8u; ++k) { s->r[k] = c->r[k]; }
        s->ip = c->ip; s->flags = c->flags;
        for (unsigned k = 0; k < 6u; ++k) { s->seg[k] = get_sel(c, k); }
        s->idt_base = c->idt_base; s->idt_limit = c->idt_limit;
        if (!g_rmss_sel) g_rmss_sel = ldt_data((uint32_t)c->seg[B86_SS] << 4, 0xFFFFu, 0);
        else { uint32_t a = ldt_addr(g_rmss_sel), b = (uint32_t)c->seg[B86_SS] << 4; wr16(a + 2, (uint16_t)b); wr8(a + 4, (uint8_t)(b >> 16)); wr8(a + 7, 0); }
        uint32_t rmsp = c->r[B86_SP] & 0xFFFFu;
        enter_pm_tables(c); c->priv_relaxed = 1;
        load_cs(c, g_cb[i].cs); load_seg(c, B86_DS, g_rmss_sel); load_seg(c, B86_ES, g_cb[i].es); load_seg(c, B86_SS, SEL_HOST_DS); load_seg(c, B86_FS, 0); load_seg(c, B86_GS, 0);
        c->r[6] = rmsp; c->r[7] = g_cb[i].edi; c->r[B86_SP] = HB_PMSTACK + 0x1000u;
        /* IRET frame for the procedure's return: EFLAGS, host CS, HC_CBRET */
        uint32_t sp = g_hb + c->r[B86_SP];
        if (g_client32) { sp -= 4; wr32(sp, c->flags & ~0x200u); sp -= 4; wr32(sp, SEL_HOST_CS); sp -= 4; wr32(sp, HC_CBRET); c->r[B86_SP] -= 12u; }
        else { sp -= 2; wr16(sp, (uint16_t)(c->flags & ~0x200u)); sp -= 2; wr16(sp, SEL_HOST_CS); sp -= 2; wr16(sp, HC_CBRET); c->r[B86_SP] -= 6u; }
        c->ip = g_cb[i].eip; c->flags &= ~0x200u;
        if (E.trace) DPMI_LOG("[dpmi] real-mode callback %u -> %04X:%08X\n", i, g_cb[i].cs, g_cb[i].eip);
        return 1;
    }
    if (vector == DPMI_VEC_RAWPM) {                   /* raw switch: DS=AX ES=CX SS=DX ESP=EBX CS=SI EIP=EDI */
        uint16_t ds = (uint16_t)c->r[0], es = (uint16_t)c->r[1], ss = (uint16_t)c->r[2], cs = (uint16_t)c->r[6];
        uint32_t sp = c->r[3], ip = c->r[7];
        enter_pm_tables(c);
        load_cs(c, cs); load_seg(c, B86_DS, ds); load_seg(c, B86_ES, es); load_seg(c, B86_SS, ss); load_seg(c, B86_FS, 0); load_seg(c, B86_GS, 0);
        c->r[B86_SP] = sp; c->ip = ip; c->flags &= ~0x200u;
        if (E.trace) DPMI_LOG("[dpmi] raw switch -> PM %04X:%08X ss=%04X esp=%08X\n", cs, ip, ss, sp);
        return 1;
    }
    return 0;
}

DPMI_COLD void dpmi_pm_trap(B86Cpu *c)
{
    if (!S) return;
    g_cpu = c;
    if (c->ip == HC_SAVE) {                           /* state save/restore procedure: far return (16- or 32-bit call) */
        uint32_t sp = stack_lin(c);
        if (sel_ok(rd16(sp + 2u)) || rd16(sp + 2u) == SEL_HOST_CS) { c->ip = rd16(sp); load_cs(c, rd16(sp + 2u)); sp_add(c, 4); }
        else { c->ip = rd32(sp); load_cs(c, (uint16_t)rd32(sp + 4u)); sp_add(c, 8); }
        return;
    }
    if (c->ip == HC_CBRET && g_depth && g_nest[g_depth - 1u].kind == 7) {   /* callback procedure returned: back to real mode from the structure */
        Saved *s = &g_nest[--g_depth]; uint32_t st = s->arg;
        c->msw &= (uint16_t)~1u; c->idt_base = 0; c->idt_limit = 0x3FFu; c->trap_cs = E.rm_trap_cs;
        if (S->pd && E.pg_off_in_rm && c->pg) { c->pg = 0; tlb_drop(c); }
        c->r[7] = rd32(st); c->r[6] = rd32(st + 4); c->r[5] = rd32(st + 8); c->r[3] = rd32(st + 16); c->r[2] = rd32(st + 20); c->r[1] = rd32(st + 24); c->r[0] = rd32(st + 28);
        for (unsigned k = 0; k < 8u; ++k) c->rhi[k] = 0;
        c->flags = (rd16(st + 32) & 0xFFFFu) | 0x2u;
        set_sel(c, B86_ES, rd16(st + 34)); set_sel(c, B86_DS, rd16(st + 36)); set_sel(c, B86_FS, rd16(st + 38)); set_sel(c, B86_GS, rd16(st + 40));
        set_sel(c, B86_SS, rd16(st + 48)); c->r[B86_SP] = rd16(st + 46); c->ip = rd16(st + 42); set_sel(c, B86_CS, rd16(st + 44));
        if (E.trace) DPMI_LOG("[dpmi] callback returned -> real %04X:%04X\n", c->seg[B86_CS], (unsigned)c->ip);
        return;
    }
    if (c->ip == HC_RAWRM) {                          /* raw switch: protected -> real, DS=AX ES=CX SS=DX SP=BX CS=SI IP=DI */
        uint16_t ds = (uint16_t)c->r[0], es = (uint16_t)c->r[1], ss = (uint16_t)c->r[2], cs = (uint16_t)c->r[6];
        uint32_t sp = c->r[3] & 0xFFFFu, ip = c->r[7] & 0xFFFFu;
        c->msw &= (uint16_t)~1u; c->idt_base = 0; c->idt_limit = 0x3FFu; c->trap_cs = E.rm_trap_cs;
        if (S->pd && E.pg_off_in_rm && c->pg) { c->pg = 0; tlb_drop(c); }
        set_sel(c, B86_DS, ds); set_sel(c, B86_ES, es); set_sel(c, B86_SS, ss); set_sel(c, B86_FS, 0); set_sel(c, B86_GS, 0);
        c->r[B86_SP] = sp; c->ip = ip; set_sel(c, B86_CS, cs); c->flags &= ~0x200u;
        if (E.trace) DPMI_LOG("[dpmi] raw switch -> real %04X:%04X\n", cs, (unsigned)ip);
        return;
    }
    unsigned v = (c->ip / 4u) & 0xFFu;
    uint32_t frame = stack_lin(c);
    if (v == 0x31u) {
        int async = 0;
        int cf = int31(c, frame, &async);
        if (!async) host_iret(c, frame, (uint32_t)cf, 1u);
        return;
    }
    if (v == 0x2Fu && (c->r[0] & 0xFFFFu) == 0x1686u) { SETAX(0); host_iret(c, frame, 0, 0); return; }
    /* unhooked vector: reflect to the real-mode handler with the client's registers */
    ++g_reflect;
    if (E.trace) DPMI_LOG("[dpmi] reflect INT %02X to real mode (ax=%04X) frame=%08X esp=%08X ss=%04X\n", v, (unsigned)(c->r[0] & 0xFFFFu), frame, c->r[B86_SP], get_sel(c, B86_SS));
    Saved *s = &g_nest[g_depth++]; s->kind = 1; s->vector = (uint8_t)v; s->frame_lin = frame;
    uint32_t regs[8]; for (unsigned i = 0; i < 8u; ++i) regs[i] = c->r[i];
    uint16_t segs[6] = {0};
    for (unsigned i = 0; i < 4u; ++i) { static const unsigned m[4] = { B86_DS, B86_ES, B86_FS, B86_GS }; uint32_t b = c->pm386.seg[m[i]].valid ? c->pm386.seg[m[i]].base : 0u; segs[i] = b < 0x100000u ? (uint16_t)(b >> 4) : 0u; }
    uint32_t fl = rd32(frame + 8);
    rm_call(c, s, rd16(4u * v + 2u), rd16(4u * v), 1, regs, segs, fl, 0, 0);
}

DPMI_COLD void dpmi_report(void)
{
    if (!S) return;
    DPMI_LOG("[dpmi] active=%d real-mode calls=%lu reflected=%lu\n", g_active, g_rmcalls, g_reflect);
    DPMI_LOG("[dpmi] vm: faults=%lu zero-fills=%lu swap-ins=%lu swap-outs=%lu evict-fail=%lu frames=%lu\n", S->faults, S->zero_fills, S->swap_ins, S->swap_outs, S->evict_fail, (unsigned long)S->nframes);
    for (unsigned i = 0; i < 256u; ++i) if (g_calls[i]) DPMI_LOG("[dpmi]   fn %04X: %lu calls%s\n", ((i >> 4) << 8) | (i & 15u), g_calls[i], g_unsupported[i] ? " (UNSUPPORTED)" : "");
}

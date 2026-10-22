/* dos4gw_host: run a real DOS/4GW program (Descent) on the host under the
 * blitz86 386 interpreter with a small high-level DOS/BIOS emulation, so
 * real-mode and protected-mode gaps can be found and fixed without a Pico.
 *
 *   gcc -O2 -DB86_RAM_BYTES=0x400000u -Iinclude src/interp.c tests/dos4gw_host.c -o build/dos4gw_host
 *   build/dos4gw_host DIR PROGRAM.EXE [command tail] [--steps N] [--trace-int] [--trace-pe]
 *
 * DIR is the host directory that plays C:\ (and the current directory).
 * BIOS/DOS services are reached through IVT stubs at F000:v*8 ("INT v;
 * RETF 2"): blitz86's int_hook services INT v only when it is executed by
 * that stub, so programs that hook vectors (DOS/4GW does) chain normally.
 * No timer IRQ, no XMS/EMS/VCPI/DPMI: DOS/4GW takes its raw INT 15h path,
 * exactly what blitzBUS offers on the Pico. */
#include "b86.h"
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define STUB_SEG 0xF000u
#define MCB_FIRST 0x0200u
#define MEM_TOP 0xA000u
#define EXT_KB 3072u

static uint8_t *M;
static B86Cpu C;
static const char *g_dir;
static int g_trace_int, g_trace_pe, g_quiet, g_pico_ivt;
static uint16_t g_psp, g_dta_seg, g_dta_off;
static int g_exit = -1;
static uint16_t g_last_err;
static const char *g_env[8]; static int g_nenv;
#ifdef DOS4GW_JIT
static struct B86Jit *J;
#else
static void *J;
#define b86_jit_invalidate(j, a, n) ((void)(j), (void)(a), (void)(n))
#endif            /* --jit: the Thumb-2/A64 translator, as on the Pico */
/* 18.2 Hz BIOS tick stand-in: one tick per 16384 retired instructions in both
   modes (no IRQ0; DOS/4GW and Descent poll 0040:006C). */
static unsigned long long g_tick_at;
/* r67: PIT channel 0 + master PIC + IRQ0 delivery on a virtual clock
   (--irq). Guest time advances with retired instructions at g_vmips. */
static void logf_(const char *fmt, ...);
static uint8_t g_pic_mask[2] = { 0xB8, 0xFF };
static int g_irq_on, g_pj;
static double g_vmips = 25e6;
static uint32_t g_pit_reload = 65536u;
static int g_pit_lo_pending, g_pit_lo;
static uint8_t g_pic_irr, g_pic_isr, g_pic_base = 8, g_pic_readisr, g_pic_init;
static unsigned long long g_irq_next, g_irq_delivered, g_irq_edges;
static unsigned long long irq_period(void) { return (unsigned long long)((double)g_pit_reload * g_vmips / 1193182.0) + 1ull; }
/* r68: scripted keyboard (--key INSN:SCANCODE, break code follows 300k
   instructions later) through port 60h/64h and IRQ1; VGA DAC palette for
   the mode 13h screenshot written at exit (screen.ppm). */
#define MAXKEYS 64
static unsigned long long g_key_at[MAXKEYS]; static uint8_t g_key_sc[MAXKEYS]; static int g_nkeys, g_key_next;
static uint8_t g_kb_q[32], g_kb_qn, g_kb_data, g_kb_obf;
static uint8_t g_pal[256][3], g_pal_w, g_pal_r, g_pal_c, g_vmode = 3;
static unsigned long long g_kb_delivered;
static void kb_feed(unsigned long long retired)
{
    while (g_key_next < g_nkeys && retired >= g_key_at[g_key_next]) {
        if (g_kb_qn < sizeof g_kb_q) g_kb_q[g_kb_qn++] = g_key_sc[g_key_next];
        logf_("KEY scancode %02X queued at insn %llu", g_key_sc[g_key_next], retired);
        ++g_key_next;
    }
    if (!g_kb_obf && g_kb_qn) {
        g_kb_data = g_kb_q[0]; memmove(g_kb_q, g_kb_q + 1, --g_kb_qn); g_kb_obf = 1; g_pic_irr |= 2u;
    }
}
static void irq_service(unsigned long long retired)
{
    if (!g_irq_on) return;
    kb_feed(retired);
    if (!g_irq_next) g_irq_next = retired + irq_period();
    if (retired >= g_irq_next) {
        g_pic_irr |= 1u; ++g_irq_edges;
        g_irq_next += irq_period();
        if (g_irq_next <= retired) g_irq_next = retired + irq_period();   /* coalesce */
    }
    if ((g_pic_irr & 2u) && !(g_pic_mask[0] & 2u) && !(g_pic_isr & 3u) && b86_irq(&C, (uint8_t)(g_pic_base + 1u))) {
        g_pic_irr &= (uint8_t)~2u; g_pic_isr |= 2u; ++g_kb_delivered;
        logf_("IRQ1 -> INT %02X data %02X at insn %llu, now %s %04X:%08X", g_pic_base + 1u, g_kb_data, retired,
              (C.msw & 1u) ? "PM" : "RM", (unsigned)(uint16_t)C.seg[B86_CS], C.ip);
    }
    if ((g_pic_irr & 1u) && !(g_pic_mask[0] & 1u) && !(g_pic_isr & 1u) && b86_irq(&C, g_pic_base)) {
        g_pic_irr &= (uint8_t)~1u; g_pic_isr |= 1u; ++g_irq_delivered;
        if (g_irq_delivered <= 12 || getenv("TRACE_IRQ"))
            logf_("IRQ0 #%llu -> INT %02X at insn %llu, now %s %04X:%08X", g_irq_delivered, g_pic_base, retired,
                  (C.msw & 1u) ? "PM" : "RM", (unsigned)(uint16_t)C.seg[B86_CS], C.ip);
    }
}
static void tick(unsigned long long retired)
{
    irq_service(retired);
    if (g_irq_on) return;                   /* BDA ticks come from INT 08h */
    while (retired >= g_tick_at + 16384u) {
        uint32_t t = (uint32_t)(M[0x46C] | M[0x46D] << 8 | M[0x46E] << 16 | (uint32_t)M[0x46F] << 24) + 1u;
        M[0x46C] = (uint8_t)t; M[0x46D] = (uint8_t)(t >> 8); M[0x46E] = (uint8_t)(t >> 16); M[0x46F] = (uint8_t)(t >> 24);
        g_tick_at += 16384u;
    }
}
static void dirty(uint32_t a, uint32_t n) { if (J && n) b86_jit_invalidate(J, a, n); }
static unsigned long long g_ints, g_tfrom = ~0ull, g_tcount;

/* ------------------------------------------------------------------ util */
static uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
static uint8_t rb(uint32_t a) { return M[a]; }
static uint16_t rw(uint32_t a) { return (uint16_t)(M[a] | (M[a + 1] << 8)); }
/* r66: read a guest *linear* byte for diagnostics (honours paging, never
   leaves a page fault behind, never reads outside guest RAM). */
static uint8_t rlin(uint32_t a)
{
    if (C.pg) {
        uint8_t pend = C.pf_pending, err = C.pf_err; uint32_t pl = C.pf_lin;
        uint8_t v = *b86_host(&C, a);
        C.pf_pending = pend; C.pf_err = err; C.pf_lin = pl;
        return v;
    }
    return a < B86_RAM_BYTES ? M[a] : 0;
}
static void wb(uint32_t a, uint8_t v) { M[a] = v; }
static void ww(uint32_t a, uint16_t v) { M[a] = (uint8_t)v; M[a + 1] = (uint8_t)(v >> 8); }
static uint16_t R(int r) { return (uint16_t)C.r[r]; }
static void W(int r, uint16_t v) { C.r[r] = (C.r[r] & 0xFFFF0000u) | v; }
static uint8_t RH(int r) { return (uint8_t)(C.r[r] >> 8); }
static uint8_t RL(int r) { return (uint8_t)C.r[r]; }
static void WL(int r, uint8_t v) { C.r[r] = (C.r[r] & 0xFFFFFF00u) | v; }
static void WH(int r, uint8_t v) { C.r[r] = (C.r[r] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static uint16_t S(int s) { return (uint16_t)C.seg[s]; }
static void flag(uint16_t bit, int on)
{
    uint16_t f = b86_get_flags(&C);
    b86_set_flags(&C, on ? (uint16_t)(f | bit) : (uint16_t)(f & ~bit));
}
static void cf(int on) { flag(B86_CF, on); }
static void fail(uint16_t err) { W(B86_AX, err); g_last_err = err; cf(1); }
static void logf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("[host] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void rstr(uint16_t s, uint16_t o, char *out, size_t n)
{
    size_t i = 0;
    while (i + 1 < n) { char ch = (char)rb(lin(s, (uint16_t)(o + i))); if (!ch) break; out[i++] = ch; }
    out[i] = 0;
}

/* -------------------------------------------------------------- MCBs */
static uint8_t mcb_t(uint16_t m) { return rb(lin(m, 0)); }
static uint16_t mcb_owner(uint16_t m) { return rw(lin(m, 1)); }
static uint16_t mcb_size(uint16_t m) { return rw(lin(m, 3)); }
static void mcb_set(uint16_t m, uint8_t t, uint16_t owner, uint16_t size)
{
    wb(lin(m, 0), t); ww(lin(m, 1), owner); ww(lin(m, 3), size);
}
static void mcb_merge(void)
{
    for (uint16_t m = MCB_FIRST; mcb_t(m) == 'M';) {
        uint16_t n = (uint16_t)(m + mcb_size(m) + 1u);
        if (mcb_owner(m) == 0 && mcb_owner(n) == 0) {
            mcb_set(m, mcb_t(n), 0, (uint16_t)(mcb_size(m) + mcb_size(n) + 1u));
            continue;
        }
        m = n;
    }
}
static int mcb_alloc(uint16_t paras, uint16_t owner, uint16_t *seg, uint16_t *largest)
{
    uint16_t best = 0;
    mcb_merge();
    for (uint16_t m = MCB_FIRST;; m = (uint16_t)(m + mcb_size(m) + 1u)) {
        if (mcb_owner(m) == 0) {
            uint16_t sz = mcb_size(m);
            if (sz >= paras) {
                if (sz > paras) {
                    uint16_t n = (uint16_t)(m + paras + 1u);
                    mcb_set(n, mcb_t(m), 0, (uint16_t)(sz - paras - 1u));
                    mcb_set(m, 'M', owner, paras);
                } else ww(lin(m, 1), owner);
                *seg = (uint16_t)(m + 1u);
                return 1;
            }
            if (sz > best) best = sz;
        }
        if (mcb_t(m) == 'Z') break;
    }
    *largest = best;
    return 0;
}
static int mcb_resize(uint16_t seg, uint16_t paras, uint16_t *largest)
{
    uint16_t m = (uint16_t)(seg - 1u), sz;
    mcb_merge();
    sz = mcb_size(m);
    if (paras <= sz) {
        if (paras < sz) {
            uint16_t n = (uint16_t)(m + paras + 1u);
            mcb_set(n, mcb_t(m), 0, (uint16_t)(sz - paras - 1u));
            mcb_set(m, 'M', mcb_owner(m), paras);
            mcb_merge();
        }
        return 1;
    }
    if (mcb_t(m) == 'M') {
        uint16_t n = (uint16_t)(m + sz + 1u);
        if (mcb_owner(n) == 0 && sz + mcb_size(n) + 1u >= paras) {
            uint16_t total = (uint16_t)(sz + mcb_size(n) + 1u);
            uint8_t t = mcb_t(n);
            if (total > paras) {
                uint16_t k = (uint16_t)(m + paras + 1u);
                mcb_set(k, t, 0, (uint16_t)(total - paras - 1u));
                mcb_set(m, 'M', mcb_owner(m), paras);
            } else mcb_set(m, t, mcb_owner(m), total);
            return 1;
        }
        *largest = (uint16_t)(mcb_owner(n) == 0 ? sz + mcb_size(n) + 1u : sz);
    } else *largest = sz;
    return 0;
}

/* -------------------------------------------------------------- files */
#define NH 32
static FILE *g_fh[NH];
static int host_path(const char *dos, char *out, size_t n)
{
    const char *p = dos;
    char rel[256]; size_t k = 0;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') ++p;
    for (; *p && k + 1 < sizeof rel; ++p) rel[k++] = *p == '\\' ? '/' : *p;
    rel[k] = 0;
    /* case-insensitive single-level lookup (Descent lives in one directory) */
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, rel)) { snprintf(out, n, "%s/%s", g_dir, e->d_name); closedir(d); return 1; }
        closedir(d);
    }
    snprintf(out, n, "%s/%s", g_dir, rel);
    return 0;
}
/* r67: DOS handle model. A handle indexes the current PSP's job file table
   (PSP:32h count, PSP:34h far pointer); the JFT byte is an SFT number, which
   indexes g_fh. DOS/4GW copies JFT entries between PSPs, so this matters. */
static uint8_t g_ref[NH];
static uint8_t *jft_slot(uint16_t h)
{
    uint32_t p = ((uint32_t)g_psp << 4);
    uint16_t n = (uint16_t)(M[p + 0x32] | M[p + 0x33] << 8);
    if (h >= n) return NULL;
    uint32_t t = ((uint32_t)(M[p + 0x36] | M[p + 0x37] << 8) << 4) + (uint16_t)(M[p + 0x34] | M[p + 0x35] << 8);
    return M + t + h;
}
static int sft_of(uint16_t h)
{
    uint8_t *e = jft_slot(h);
    if (!e || *e == 0xFF || *e >= NH) return -1;
    return *e;
}
static int new_sft(FILE *f)
{
    for (int s = 5; s < NH; ++s) if (!g_fh[s]) { g_fh[s] = f; g_ref[s] = 1; return s; }
    return -1;
}
static int new_jft(int sft)
{
    for (uint16_t h = 0; ; ++h) { uint8_t *e = jft_slot(h); if (!e) return -1; if (*e == 0xFF) { *e = (uint8_t)sft; return h; } }
}
static int new_handle(FILE *f)
{
    int s = new_sft(f), h;
    if (s < 0) return -1;
    if ((h = new_jft(s)) < 0) { g_fh[s] = 0; g_ref[s] = 0; return -1; }
    return h;
}
static int wild(const char *pat, const char *name)
{
    for (; *pat; ++pat, ++name) {
        if (*pat == '*') { for (const char *q = name;; ++q) { if (wild(pat + 1, q)) return 1; if (!*q) return 0; } }
        if (!*name) return 0;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*name)) return 0;
    }
    return !*name;
}
static char g_find_pat[64];
static long g_find_pos;
static int find_next(void)
{
    DIR *d = opendir(g_dir);
    struct dirent *e; long i = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || i++ < g_find_pos) continue;
        char pat[64]; snprintf(pat, sizeof pat, "%s", g_find_pat);
        if (!strcmp(pat, "*.*")) strcpy(pat, "*");
        if (!wild(pat, e->d_name)) continue;
        char hp[512]; struct stat st;
        snprintf(hp, sizeof hp, "%s/%s", g_dir, e->d_name);
        if (stat(hp, &st) || !S_ISREG(st.st_mode)) continue;
        g_find_pos = i;
        uint32_t dta = lin(g_dta_seg, g_dta_off);
        memset(M + dta, 0, 43);
        wb(dta + 21, 0x20);
        ww(dta + 22, 0); ww(dta + 24, (uint16_t)((15u << 9) | (2u << 5) | 14u));
        ww(dta + 26, (uint16_t)st.st_size); ww(dta + 28, (uint16_t)(st.st_size >> 16));
        for (int k = 0; k < 12 && e->d_name[k]; ++k) wb(dta + 30 + k, (uint8_t)toupper((unsigned char)e->d_name[k]));
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

/* -------------------------------------------------------------- INT 21 */
static void int21(void)
{
    uint8_t ah = RH(B86_AX), al = RL(B86_AX);
    char name[256], hp[512];
    cf(0);
    switch (ah) {
    case 0x02: putchar(RL(B86_DX)); break;
    case 0x06: if (RL(B86_DX) != 0xFF) putchar(RL(B86_DX)); else { WL(B86_AX, 0); flag(B86_ZF, 1); } break;
    case 0x09: for (uint16_t o = R(B86_DX); rb(lin(S(B86_DS), o)) != '$'; ++o) putchar(rb(lin(S(B86_DS), o))); WL(B86_AX, '$'); break;
    case 0x0B: WL(B86_AX, 0); break;
    case 0x0E: WL(B86_AX, 26); break;
    case 0x19: WL(B86_AX, 2); break;
    case 0x1A: g_dta_seg = S(B86_DS); g_dta_off = R(B86_DX); break;
    case 0x25: ww(al * 4u, R(B86_DX)); ww(al * 4u + 2u, S(B86_DS)); break;
    case 0x2A: { time_t t = time(0); struct tm *tm = localtime(&t);
        W(B86_CX, (uint16_t)(tm->tm_year + 1900)); WH(B86_DX, (uint8_t)(tm->tm_mon + 1)); WL(B86_DX, (uint8_t)tm->tm_mday); WL(B86_AX, (uint8_t)tm->tm_wday); break; }
    case 0x2C: { time_t t = time(0); struct tm *tm = localtime(&t);
        WH(B86_CX, (uint8_t)tm->tm_hour); WL(B86_CX, (uint8_t)tm->tm_min); WH(B86_DX, (uint8_t)tm->tm_sec); WL(B86_DX, 0); break; }
    case 0x2F: b86_set_seg(&C, B86_ES, g_dta_seg); W(B86_BX, g_dta_off); break;
    case 0x30: W(B86_AX, 0x1606); W(B86_BX, 0); W(B86_CX, 0); break;
    case 0x33: if (al == 0x05) WL(B86_DX, 3); else if (al == 0x06) { W(B86_BX, 0x1606); W(B86_DX, 0); } else WL(B86_DX, 0); break;
    case 0x34: b86_set_seg(&C, B86_ES, 0x0060); W(B86_BX, 0x0001); break;   /* InDOS flag */
    case 0x35: W(B86_BX, rw(al * 4u)); b86_set_seg(&C, B86_ES, rw(al * 4u + 2u)); break;
    case 0x36: W(B86_AX, 16); W(B86_BX, 8000); W(B86_CX, 512); W(B86_DX, 16000); break;   /* 64 MiB free */
    case 0x37: if (al == 0) WL(B86_DX, '/'); break;
    case 0x38: memset(M + lin(S(B86_DS), R(B86_DX)), 0, 34); wb(lin(S(B86_DS), R(B86_DX)) + 2, '$'); W(B86_BX, 1); break;
    case 0x3B: break;
    case 0x3C: case 0x3D: case 0x6C: {
        uint16_t o = ah == 0x6C ? R(B86_SI) : R(B86_DX);
        rstr(S(B86_DS), o, name, sizeof name);
        int exists = host_path(name, hp, sizeof hp);
        const char *mode = ah == 0x3C ? "w+b" : ((al & 3) ? "r+b" : "rb");
        if (ah == 0x6C) mode = exists ? ((RL(B86_BX) & 3) ? "r+b" : "rb") : ((R(B86_DX) & 0x10) ? "w+b" : NULL);
        FILE *f = mode ? fopen(hp, mode) : NULL;
        if (g_trace_int || !f) logf_("open %s -> %s", name, f ? "ok" : "FAIL");
        if (!f) { fail(2); break; }
        int h = new_handle(f);
        if (h < 0) { fclose(f); fail(4); break; }
        W(B86_AX, (uint16_t)h); if (ah == 0x6C) W(B86_CX, exists ? 1 : 2);
        break; }
    case 0x3E: { int sf = sft_of(R(B86_BX)); if (sf < 0) { fail(6); break; }
        *jft_slot(R(B86_BX)) = 0xFF;
        if (sf > 4 && g_fh[sf] && --g_ref[sf] == 0) { fclose(g_fh[sf]); g_fh[sf] = 0; }
        break; }
    case 0x3F: { int h = sft_of(R(B86_BX)); uint16_t n = R(B86_CX); uint32_t a = lin(S(B86_DS), R(B86_DX));
        if (h == 0) { W(B86_AX, 0); break; }
        if (h < 0 || h >= NH || !g_fh[h]) { fail(6); break; }
        fseek(g_fh[h], 0, SEEK_CUR);           /* r67: legal read<->write switch */
        size_t got = fread(M + a, 1, n, g_fh[h]); dirty(a, (uint32_t)got); W(B86_AX, (uint16_t)got); break; }
    case 0x40: { int h = sft_of(R(B86_BX)); uint16_t n = R(B86_CX); uint32_t a = lin(S(B86_DS), R(B86_DX));
        if (h == 1 || h == 2) { fwrite(M + a, 1, n, stdout); fflush(stdout); W(B86_AX, n); break; }
        if (h < 0 || h >= NH || !g_fh[h]) { fail(6); break; }
        fseek(g_fh[h], 0, SEEK_CUR);
        if (n == 0) {                          /* r67: DOS: CX=0 sets the file size to the position */
            fflush(g_fh[h]);
            if (ftruncate(fileno(g_fh[h]), ftell(g_fh[h]))) { fail(5); break; }
            W(B86_AX, 0); break;
        }
        W(B86_AX, (uint16_t)fwrite(M + a, 1, n, g_fh[h])); break; }
    case 0x41: rstr(S(B86_DS), R(B86_DX), name, sizeof name); host_path(name, hp, sizeof hp); if (remove(hp)) fail(2); break;
    case 0x42: { int h = sft_of(R(B86_BX)); long off = (long)(((uint32_t)R(B86_CX) << 16) | R(B86_DX));
        if (h < 0 || h >= NH || !g_fh[h]) { fail(6); break; }
        if (fseek(g_fh[h], al == 0 ? off : (long)(int32_t)off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END)) { fail(25); break; }
        long p = ftell(g_fh[h]); W(B86_AX, (uint16_t)p); W(B86_DX, (uint16_t)(p >> 16)); break; }
    case 0x43: rstr(S(B86_DS), R(B86_DX), name, sizeof name);
        if (!host_path(name, hp, sizeof hp)) { fail(2); break; } if (al == 0) W(B86_CX, 0x20); break;
    case 0x44: { int h = sft_of(R(B86_BX));
        if (al == 0) { if (h < 0) { fail(6); break; } W(B86_DX, h <= 2 ? (uint16_t)(0x80C0u | (h == 0 ? 1u : 2u)) : 0x0002u); }
        else if (al == 1 || al == 6 || al == 7) WL(B86_AX, 0xFF);
        else if (al == 8) W(B86_AX, 1);
        else if (al == 9) W(B86_DX, 0);
        else fail(1);
        break; }
    case 0x45: { int sf = sft_of(R(B86_BX)), n; if (sf < 0) { fail(6); break; }
        if ((n = new_jft(sf)) < 0) { fail(4); break; }
        if (sf < NH) ++g_ref[sf];
        W(B86_AX, (uint16_t)n); break; }
    case 0x46: { int sf = sft_of(R(B86_BX)), old; uint8_t *e = jft_slot(R(B86_CX)); if (sf < 0 || !e) { fail(6); break; }
        old = *e;
        if (old != 0xFF && old > 4 && old < NH && g_fh[old] && --g_ref[old] == 0) { fclose(g_fh[old]); g_fh[old] = 0; }
        *e = (uint8_t)sf; ++g_ref[sf]; break; }
    case 0x47: wb(lin(S(B86_DS), R(B86_SI)), 0); break;
    case 0x48: { uint16_t seg, big = 0; if (mcb_alloc(R(B86_BX), g_psp, &seg, &big)) W(B86_AX, seg); else { fail(8); W(B86_BX, big); } break; }
    case 0x49: ww(lin((uint16_t)(S(B86_ES) - 1u), 1), 0); mcb_merge(); break;
    case 0x4A: { uint16_t big = 0; if (!mcb_resize(S(B86_ES), R(B86_BX), &big)) { fail(8); W(B86_BX, big); }
        else if (S(B86_ES) == g_psp) ww(lin(g_psp, 2), (uint16_t)(g_psp + R(B86_BX))); break; }
    case 0x4C: g_exit = al; C.irq |= 1u; break;
    case 0x4E: rstr(S(B86_DS), R(B86_DX), name, sizeof name);
        { const char *b = strrchr(name, '\\'); snprintf(g_find_pat, sizeof g_find_pat, "%s", b ? b + 1 : (name[1] == ':' ? name + 2 : name)); }
        g_find_pos = 0; if (!find_next()) fail(18); else g_find_pos++; break;
    case 0x4F: if (!find_next()) fail(18); else g_find_pos++; break;
    case 0x50: g_psp = R(B86_BX); break;
    case 0x51: case 0x62: W(B86_BX, g_psp); break;
    case 0x52:   /* list of lists at 0060:0010; first MCB at ES:[BX-2] */
        memset(M + 0x600, 0, 0x80); ww(0x60E, MCB_FIRST); ww(0x610 + 0x10, 512);
        wb(0x610 + 0x20, 26); dirty(0x600, 0x80); b86_set_seg(&C, B86_ES, 0x0060); W(B86_BX, 0x0010); break;
    case 0x57: if (al == 0) { W(B86_CX, 0); W(B86_DX, (uint16_t)((15u << 9) | (2u << 5) | 14u)); } break;
    case 0x58: if (al == 0) W(B86_AX, 0); break;
    case 0x59: W(B86_AX, g_last_err); W(B86_BX, 0x0101); break;
    case 0x63: fail(1); break;
    case 0x65: fail(1); break;
    case 0x66: W(B86_BX, 437); W(B86_DX, 437); break;
    case 0x67: break;
    default:
        logf_("INT 21 AH=%02X AL=%02X unhandled (from %04X:%04X)", ah, al, rw(lin(S(B86_SS), R(B86_SP)) + 2), rw(lin(S(B86_SS), R(B86_SP))));
        fail(1);
    }
}

/* -------------------------------------------------------------- BIOS */
static int hook(B86Cpu *c, uint8_t v)
{
    if (c->msw & 1u) return 0;
    /* --pico-ivt: blitzBUS services BIOS vectors 10h-1Ah for every INT
       instruction and leaves their IVT entries 0000:0000. */
    if ((uint16_t)c->seg[B86_CS] != STUB_SEG && !(g_pico_ivt && v >= 0x10 && v <= 0x1A)) return 0;
    ++g_ints;
    if (g_trace_int && v != 0x16 && v != 0x28 && v != 0x2A)
        logf_("INT %02X AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X", v, R(B86_AX), R(B86_BX), R(B86_CX), R(B86_DX), S(B86_DS), S(B86_ES));
    switch (v) {
    case 0x21: int21(); break;
    case 0x20: g_exit = 0; break;
    case 0x10:
        switch (RH(B86_AX)) {
        case 0x0E: putchar(RL(B86_AX)); fflush(stdout); break;
        case 0x0F: W(B86_AX, 0x5003); WH(B86_BX, 0); break;
        case 0x03: W(B86_DX, 0); W(B86_CX, 0x0607); break;
        case 0x12: if (RL(B86_BX) == 0x10) W(B86_BX, 0x0003), W(B86_CX, 0x0009); break;
        case 0x1A: if (RL(B86_AX) == 0) { WL(B86_AX, 0x1A); W(B86_BX, 0x0008); } break;
        case 0x00: wb(0x449, RL(B86_AX) & 0x7F); g_vmode = RL(B86_AX) & 0x7F; logf_("INT 10 set mode %02X", RL(B86_AX)); break;
        default: break;
        }
        break;
    case 0x11: W(B86_AX, rw(0x410)); break;
    case 0x12: W(B86_AX, 640); break;
    case 0x13: WH(B86_AX, 1); cf(1); break;
    case 0x14: case 0x17: W(B86_AX, 0x8000); break;
    case 0x15:
        switch (RH(B86_AX)) {
        case 0x88: W(B86_AX, EXT_KB); cf(0); break;
        case 0x24: if (RL(B86_AX) == 2) { WL(B86_AX, 1); WH(B86_AX, 0); } else WH(B86_AX, 0); cf(0); break;
        default: WH(B86_AX, 0x86); cf(1); break;
        }
        break;
    case 0x16:
        if (RH(B86_AX) == 0x01 || RH(B86_AX) == 0x11) flag(B86_ZF, 1);
        else if (RH(B86_AX) == 0x02 || RH(B86_AX) == 0x12) W(B86_AX, 0);
        else if (RH(B86_AX) == 0x00 || RH(B86_AX) == 0x10) { W(B86_AX, 0x011B); }
        break;
    case 0x1A: if (RH(B86_AX) == 0) { W(B86_CX, rw(0x46E)); W(B86_DX, rw(0x46C)); WL(B86_AX, 0); } else cf(1); break;
    case 0x2F:
        if (R(B86_AX) == 0x1600 || R(B86_AX) == 0x4300 || R(B86_AX) == 0x1680) WL(B86_AX, 0);
        break;
    case 0x67: WH(B86_AX, 0x84); break;
    case 0x08:                                /* r67: BIOS timer: BDA tick + EOI */
        if (g_irq_on) {
            uint32_t t = (uint32_t)(M[0x46C] | M[0x46D] << 8 | M[0x46E] << 16 | (uint32_t)M[0x46F] << 24) + 1u;
            M[0x46C] = (uint8_t)t; M[0x46D] = (uint8_t)(t >> 8); M[0x46E] = (uint8_t)(t >> 16); M[0x46F] = (uint8_t)(t >> 24);
            g_pic_isr &= (uint8_t)~1u;
        }
        break;
    case 0x09: if (g_irq_on) { g_kb_obf = 0; g_pic_isr &= (uint8_t)~2u; } break;   /* r68: BIOS keyboard: drop + EOI */
    case 0x1C: case 0x28: case 0x2A: case 0x1B: case 0x23: case 0x24: break;
    default:
        logf_("INT %02X AX=%04X: no BIOS service", v, R(B86_AX));
        break;
    }
    return 1;
}

static uint8_t g_cmos_idx, g_kbc_out = 0x01;
static uint8_t in8(B86Cpu *c, uint16_t port)
{
    (void)c;
    switch (port) {
    case 0x20: return g_pic_readisr ? g_pic_isr : g_pic_irr;   /* r67 */
    case 0xA0: return 0x00;
    case 0x21: return g_pic_mask[0];
    case 0xA1: return g_pic_mask[1];
    case 0x64: return (uint8_t)(0x1C | g_kb_obf);   /* system flag; OBF from the key script */
    case 0x60: g_kb_obf = 0; return g_kb_data;
    case 0x3C9: { uint8_t v = g_pal[g_pal_r][g_pal_c]; if (++g_pal_c == 3) { g_pal_c = 0; ++g_pal_r; } return v; }
    case 0x61: return 0x20;
    case 0x92: return 0x02;                 /* A20 on */
    case 0x71:
        switch (g_cmos_idx & 0x7F) {
        case 0x17: case 0x30: return (uint8_t)EXT_KB;
        case 0x18: case 0x31: return (uint8_t)(EXT_KB >> 8);
        case 0x15: return 0x80; case 0x16: return 0x02;
        case 0x0B: return 0x02; case 0x0D: return 0x80;
        default: return 0;
        }
    case 0x40: case 0x41: case 0x42: { static uint8_t t; return t += 37; }   /* counter latch not modelled */
    case 0x3DA: { static uint8_t t; return (t ^= 9); }
    default: return 0xFF;
    }
}
static void out8(B86Cpu *c, uint16_t port, uint8_t v)
{
    (void)c;
    switch (port) {
    case 0x21:
        if (g_pic_init == 1) { g_pic_base = v & 0xF8u; g_pic_init = 2; }
        else if (g_pic_init == 2) g_pic_init = 3;
        else if (g_pic_init == 3) g_pic_init = 0;
        else g_pic_mask[0] = v;
        break;
    case 0x20:
        if (v & 0x10u) { g_pic_init = 1; g_pic_irr = g_pic_isr = 0; }
        else if ((v & 0x18u) == 0x08u) { if (v & 2u) g_pic_readisr = v & 1u; }
        else if (v & 0x20u) {
            if (v & 0x40u) g_pic_isr &= (uint8_t)~(1u << (v & 7u));
            else for (unsigned k = 0; k < 8; ++k) if (g_pic_isr & (1u << k)) { g_pic_isr &= (uint8_t)~(1u << k); break; }
        }
        break;
    case 0x43: if ((v >> 6) == 0u && ((v >> 4) & 3u) == 3u) g_pit_lo_pending = 1; break;
    case 0x40:
        if (g_pit_lo_pending) { g_pit_lo = v; g_pit_lo_pending = 0; }
        else { g_pit_reload = (uint32_t)(g_pit_lo | v << 8); if (!g_pit_reload) g_pit_reload = 65536u;
               g_irq_next = 0; logf_("PIT ch0 reload %u (%.1f Hz)", g_pit_reload, 1193182.0 / g_pit_reload); }
        break;
    case 0xA1: g_pic_mask[1] = v; break;
    case 0x70: g_cmos_idx = v; break;
    case 0x3C7: g_pal_r = v; g_pal_c = 0; break;
    case 0x3C8: g_pal_w = v; g_pal_c = 0; break;
    case 0x3C9: g_pal[g_pal_w][g_pal_c] = v & 63u; if (++g_pal_c == 3) { g_pal_c = 0; ++g_pal_w; } break;
    case 0xA0:
        if (getenv("TRACE_PORTS")) logf_("OUT %02X <- %02X at %04X:%08X insn %llu", port, v, (unsigned)(uint16_t)C.seg[B86_CS], C.ip, (unsigned long long)C.icount);
        break;
    case 0x64: if (v == 0xD1) g_kbc_out = 2; else if (v == 0xFE) logf_("KBC CPU reset requested"); break;
    case 0x60: if (g_kbc_out == 2) { g_kbc_out = 1; if (!(v & 2)) logf_("A20 disable request (ignored)"); } break;
    default: break;
    }
}

/* -------------------------------------------------------------- loader */
static int load_exe(const char *exe, const char *tail)
{
    char hp[512];
    host_path(exe, hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    if (!f) { logf_("cannot open %s", hp); return 0; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 28 || buf[0] != 'M' || buf[1] != 'Z') { logf_("%s: not MZ", hp); return 0; }
    uint16_t last = (uint16_t)(buf[2] | buf[3] << 8), pages = (uint16_t)(buf[4] | buf[5] << 8);
    uint16_t nrel = (uint16_t)(buf[6] | buf[7] << 8), hdr = (uint16_t)(buf[8] | buf[9] << 8);
    uint16_t minx = (uint16_t)(buf[10] | buf[11] << 8);
    uint16_t ss = (uint16_t)(buf[14] | buf[15] << 8), sp = (uint16_t)(buf[16] | buf[17] << 8);
    uint16_t ip = (uint16_t)(buf[20] | buf[21] << 8), cs = (uint16_t)(buf[22] | buf[23] << 8);
    uint16_t relo = (uint16_t)(buf[24] | buf[25] << 8);
    uint32_t img = (uint32_t)pages * 512u - (last ? 512u - last : 0u) - (uint32_t)hdr * 16u;
    if ((uint32_t)hdr * 16u + img > n) img = (uint32_t)(n - (uint32_t)hdr * 16u);

    /* free memory: one 'Z' block from MCB_FIRST to MEM_TOP */
    mcb_set(MCB_FIRST, 'Z', 0, (uint16_t)(MEM_TOP - MCB_FIRST - 1u));
    /* environment */
    uint16_t env, big = 0;
    mcb_alloc(0x20, 8, &env, &big);
    uint32_t e = lin(env, 0); size_t k = 0;
    const char *vars[] = { "COMSPEC=C:\\COMMAND.COM", "PATH=C:\\", "PROMPT=$P$G", NULL };
    for (int i = 0; vars[i]; ++i) { strcpy((char *)M + e + k, vars[i]); k += strlen(vars[i]) + 1; }
    for (int i = 0; i < g_nenv; ++i) { strcpy((char *)M + e + k, g_env[i]); k += strlen(g_env[i]) + 1; }
    M[e + k++] = 0; ww(e + k, 1); k += 2;
    { char full[128]; snprintf(full, sizeof full, "C:\\%s", exe); for (char *p = full; *p; ++p) *p = (char)toupper((unsigned char)*p);
      strcpy((char *)M + e + k, full); }
    /* program block: everything that is left */
    mcb_merge();
    uint16_t psp = 0;
    mcb_alloc(1, 0xFFFF, &psp, &big);
    big = 0;
    if (!mcb_resize(psp, 0xFFFFu, &big)) mcb_resize(psp, big, &big);   /* grow to all free memory */
    ww(lin((uint16_t)(psp - 1u), 1), psp);
    ww(lin((uint16_t)(env - 1u), 1), psp);
    { const char *nm = "DESCENT"; memcpy(M + lin((uint16_t)(psp - 1u), 8), nm, 7); }
    uint16_t top = (uint16_t)(psp + mcb_size((uint16_t)(psp - 1u)));
    if (img + 0x100u + (uint32_t)minx * 16u > (uint32_t)(top - psp) * 16u) { logf_("not enough memory"); return 0; }
    g_psp = psp;
    uint32_t p = lin(psp, 0);
    memset(M + p, 0, 256);
    wb(p, 0xCD); wb(p + 1, 0x20);
    ww(p + 2, top);
    wb(p + 5, 0x9A);
    ww(p + 0x16, psp);
    ww(p + 0x2C, env);
    for (int i = 0; i < 20; ++i) wb(p + 0x18 + i, i < 5 ? (uint8_t)i : 0xFF);
    ww(p + 0x32, 20); ww(p + 0x34, 0x18); ww(p + 0x36, psp);
    wb(p + 0x50, 0xCD); wb(p + 0x51, 0x21); wb(p + 0x52, 0xCB);
    size_t tl = strlen(tail);
    wb(p + 0x80, (uint8_t)tl); memcpy(M + p + 0x81, tail, tl); wb(p + 0x81 + tl, 0x0D);
    g_dta_seg = psp; g_dta_off = 0x80;
    uint16_t load = (uint16_t)(psp + 0x10u);
    memcpy(M + lin(load, 0), buf + (uint32_t)hdr * 16u, img);
    for (uint16_t i = 0; i < nrel; ++i) {
        uint16_t ro = (uint16_t)(buf[relo + i * 4] | buf[relo + i * 4 + 1] << 8);
        uint16_t rs = (uint16_t)(buf[relo + i * 4 + 2] | buf[relo + i * 4 + 3] << 8);
        uint32_t a = lin((uint16_t)(load + rs), ro);
        ww(a, (uint16_t)(rw(a) + load));
    }
    b86_set_seg(&C, B86_DS, psp); b86_set_seg(&C, B86_ES, psp);
    b86_set_seg(&C, B86_SS, (uint16_t)(load + ss)); C.r[B86_SP] = sp;
    b86_set_seg(&C, B86_CS, (uint16_t)(load + cs)); C.ip = ip;
    C.r[B86_AX] = 0;
    logf_("loaded %s: PSP=%04X CS:IP=%04X:%04X SS:SP=%04X:%04X image=%lu top=%04X tail='%s'",
          exe, psp, load + cs, ip, load + ss, sp, (unsigned long)img, top, tail);
    return 1;
}

/* -------------------------------------------------------------- run */
#define RING 256
typedef struct { uint16_t cs; uint8_t pe, d; uint32_t ip; uint8_t b[8]; } Tr;
static Tr g_ring[RING];
static unsigned g_rpos;
static uint32_t cs_base(void) { return (C.msw & 1u) ? C.pm386.seg[B86_CS].base : ((uint32_t)(uint16_t)C.seg[B86_CS] << 4); }
static void record(void)
{
    Tr *t = &g_ring[g_rpos++ % RING];
    uint32_t a = cs_base() + ((C.msw & 1u) ? C.ip : (C.ip & 0xFFFFu));
    t->cs = (uint16_t)C.seg[B86_CS]; t->ip = C.ip; t->pe = (uint8_t)(C.msw & 1u);
    t->d = (uint8_t)((C.msw & 1u) && (C.pm386.seg[B86_CS].flags & 4u));
    for (int i = 0; i < 8; ++i) t->b[i] = rlin(a + i);
    /* r67: --trace-dpmi: protected-mode INT 31h calls from 32-bit code and their results */
    static int dpmi_on = -1; static uint16_t rcs; static uint32_t rip, rax; static int pending;
    if (dpmi_on < 0) dpmi_on = getenv("TRACE_DPMI") != NULL;
    if (!dpmi_on || !t->pe) return;
    if (pending && (uint16_t)C.seg[B86_CS] == rcs && C.ip == rip) {
        logf_("DPMI %04X -> CF=%u EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X", rax & 0xFFFF,
              b86_get_flags(&C) & 1u, C.r[0], C.r[3], C.r[1], C.r[2], C.r[6], C.r[7]);
        if ((rax & 0xFFFF) == 0x0500) {
            uint32_t b = C.pm386.seg[B86_ES].base + C.r[7], v[8];
            for (int k = 0; k < 8; ++k) v[k] = *b86_host(&C, b + 4u*k) | *b86_host(&C, b + 4u*k + 1u) << 8 |
                                              *b86_host(&C, b + 4u*k + 2u) << 16 | (uint32_t)*b86_host(&C, b + 4u*k + 3u) << 24;
            logf_("DPMI 0500 info: largest=%08X maxunlock=%u maxlock=%u linear=%u unlocked=%u freephys=%u totalphys=%u freelinear=%u (pages)",
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
        pending = 0;
    }
    if (t->d && t->b[0] == 0xCD && t->b[1] == 0x31) {
        rcs = t->cs; rip = C.ip + 2u; rax = C.r[0]; pending = 1;
        logf_("DPMI %04X call EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X", C.r[0] & 0xFFFF, C.r[3], C.r[1], C.r[2], C.r[6], C.r[7]);
    }
}
static void dump(const char *why, int rc)
{
    fprintf(stderr, "\n[host] STOP %s rc=%d after %llu insns, %llu BIOS/DOS calls\n", why, rc,
            (unsigned long long)C.icount, g_ints);
    unsigned n = g_rpos < 40 ? g_rpos : 40;
    for (unsigned i = g_rpos - n; i < g_rpos; ++i) {
        Tr *t = &g_ring[i % RING];
        fprintf(stderr, "  %s%u %04X:%08X ", t->pe ? "PM" : "RM", t->pe ? 16u + 16u * t->d : 16u, t->cs, t->ip);
        for (int k = 0; k < 8; ++k) fprintf(stderr, "%02X ", t->b[k]);
        fputc('\n', stderr);
    }
    fprintf(stderr, "  CR0=%04X CS=%04X EIP=%08X base=%08X acc=%02X flg=%02X  EFLAGS=%08X\n",
            C.msw, (unsigned)(uint16_t)C.seg[B86_CS], C.ip, cs_base(),
            C.pm386.seg[B86_CS].access, C.pm386.seg[B86_CS].flags, (unsigned)((C.flags & 0xFFFF0000u) | b86_get_flags(&C)));
    fprintf(stderr, "  EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
            C.r[B86_AX], C.r[B86_BX], C.r[B86_CX], C.r[B86_DX], C.r[B86_SI], C.r[B86_DI], C.r[B86_BP], C.r[B86_SP]);
    const char *nm[] = { "ES", "CS", "SS", "DS", "FS", "GS" };
    for (int s = 0; s < 6; ++s)
        fprintf(stderr, "  %s=%04X base=%08X lim=%08X acc=%02X flg=%02X%s\n", nm[s],
                s < 4 ? (unsigned)(uint16_t)C.seg[s] : C.seg386[s - 4], C.pm386.seg[s].base, C.pm386.seg[s].limit,
                C.pm386.seg[s].access, C.pm386.seg[s].flags, C.pm386.seg[s].valid ? "" : " (invalid)");
    {
        uint32_t sb = (C.msw & 1u) ? C.pm386.seg[B86_SS].base : ((uint32_t)(uint16_t)C.seg[B86_SS] << 4);
        uint32_t sp = (C.msw & 1u) && (C.pm386.seg[B86_SS].flags & 4u) ? C.r[B86_SP] : (C.r[B86_SP] & 0xFFFFu);
        fprintf(stderr, "  stack@%08X:", sb + sp);
        for (int i = 0; i < 12; ++i) fprintf(stderr, " %04X", rlin(sb + sp + 2u * i) | (rlin(sb + sp + 2u * i + 1u) << 8));
        fputc('\n', stderr);
    }
    { extern unsigned long b86_pg_stat_faults, b86_pg_stat_tswitch, b86_pg_stat_walks, b86_pg_stat_flush;
      extern unsigned long b86_pm_memo_hits, b86_pm_memo_misses, b86_pm_memo_fallbacks;
      fprintf(stderr, "  pm-memo: hits=%lu misses=%lu fallbacks=%lu\n", b86_pm_memo_hits, b86_pm_memo_misses, b86_pm_memo_fallbacks);
      fprintf(stderr, "  paging: faults=%lu task-switches=%lu walks=%lu flushes=%lu\n", b86_pg_stat_faults, b86_pg_stat_tswitch, b86_pg_stat_walks, b86_pg_stat_flush); }
    fprintf(stderr, "  PG=%u CR2=%08X CR3=%08X pf_pending=%u pf_lin=%08X pf_err=%u TR=%04X\n", C.pg, C.cr2, C.cr3, C.pf_pending, C.pf_lin, C.pf_err, C.tr_sel);
    fprintf(stderr, "  GDT=%08X/%04X IDT=%08X/%04X LDTR=%04X\n", C.gdt_base, C.gdt_limit, C.idt_base, C.idt_limit,
            C.pm386.ldtr_selector);
}

int main(int argc, char **argv)
{
    unsigned long long max = 2000000000ull;
    int use_jit = 0;
    char tail[128] = "";
    const char *exe = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--steps") && i + 1 < argc) max = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--trace-int")) g_trace_int = 1;
        else if (!strcmp(argv[i], "--trace-pe")) g_trace_pe = 1;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else if (!strcmp(argv[i], "--jit")) use_jit = 1;
        else if (!strcmp(argv[i], "--pico-ivt")) g_pico_ivt = 1;
        else if (!strcmp(argv[i], "--irq")) g_irq_on = 1;
        else if (!strcmp(argv[i], "--pj")) g_pj = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc && g_nkeys + 2 <= MAXKEYS) {
            unsigned long long at; unsigned sc;
            if (sscanf(argv[++i], "%llu:%x", &at, &sc) == 2) {
                g_key_at[g_nkeys] = at; g_key_sc[g_nkeys++] = (uint8_t)sc;
                g_key_at[g_nkeys] = at + 300000ull; g_key_sc[g_nkeys++] = (uint8_t)(sc | 0x80u);
            }
        }
        else if (!strcmp(argv[i], "--vmips") && i + 1 < argc) g_vmips = atof(argv[++i]) * 1e6;
        else if (!strcmp(argv[i], "--env") && i + 1 < argc && g_nenv < 8) g_env[g_nenv++] = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 2 < argc) { g_tfrom = strtoull(argv[++i], 0, 0); g_tcount = strtoull(argv[++i], 0, 0); }
        else if (!g_dir) g_dir = argv[i];
        else if (!exe) exe = argv[i];
        else { strncat(tail, " ", sizeof tail - strlen(tail) - 1); strncat(tail, argv[i], sizeof tail - strlen(tail) - 1); }
    }
    if (!g_dir || !exe) { fprintf(stderr, "usage: dos4gw_host DIR PROGRAM.EXE [args] [--steps N] [--trace-int] [--trace-pe]\n"); return 2; }
    /* Guest RAM where the Pico has it (PSRAM 0x112A0000): the paged JIT's
       code map indexes host address >> 6 and needs a compatible layout. */
    M = mmap((void *)(uintptr_t)0x112A0000u, B86_RAM_BYTES + 4096u, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (M == MAP_FAILED) { logf_("guest mmap"); return 2; }
    b86_init(&C, M);
    C.model = B86_CPU_386;
    C.int_hook = hook; C.in8 = in8; C.out8 = out8;
    /* IVT -> F000:v*8 stubs: INT v ; RETF 2.  IRQ vectors: INT v ; IRET */
    /* Unserviced vectors share one IRET, as on a real BIOS (DOS/4GW looks for
       that duplicated "dummy" handler when choosing free vectors). */
    wb(lin(STUB_SEG, 0xFF53), 0xCF);
    for (unsigned v = 0; v < 256; ++v) {
        uint32_t s = lin(STUB_SEG, (uint16_t)(v * 8u));
        static const uint8_t svc[] = { 0x08,0x09,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x1A,0x1C,0x20,0x21,0x28,0x29,0x2A,0x2F,0x67 };
        int used = 0;
        for (unsigned k = 0; k < sizeof svc; ++k) used |= svc[k] == v;
        ww(v * 4u, used ? (uint16_t)(v * 8u) : 0xFF53u); ww(v * 4u + 2u, STUB_SEG);
        if (!used) continue;
        if (v == 8 || v == 9 || v == 0x1C) { wb(s, 0xCD); wb(s + 1, (uint8_t)v); wb(s + 2, 0xCF); }
        else {  /* r67: STI; INT v; RETF 2 -- DOS/BIOS services run with IF=1, as on a PC */
            wb(s, 0xFB); wb(s + 1, 0xCD); wb(s + 2, (uint8_t)v); wb(s + 3, 0xCA); wb(s + 4, 2); wb(s + 5, 0);
        }
    }
    ww(0x410, 0x0021); ww(0x413, 640); wb(0x449, 3); ww(0x44A, 80); wb(0x484, 24);
    ww(0x463, 0x3D4);
    wb(lin(STUB_SEG, 0xFFFE), 0xFC);
    memcpy(M + lin(STUB_SEG, 0xFFF5), "01/01/94", 8);
    if (g_pico_ivt) for (unsigned v = 0x10; v <= 0x1A; ++v) { ww(v * 4u, 0); ww(v * 4u + 2u, 0); }
    if (!load_exe(exe, tail)) return 2;
#if defined(B86_PJ) && B86_PJ
    if (g_pj) {                                  /* r70: native PM translator */
        size_t cb = 512u * 1024u;
        void *code = mmap(NULL, cb, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *st = calloc(1, b86_pj_state_bytes());
        if (code == MAP_FAILED || !st || !b86_pj_attach(&C, st, b86_pj_state_bytes(), code, cb)) { logf_("pj attach failed"); return 2; }
        logf_("PJ on: %u KiB code, %u B state", (unsigned)(cb / 1024u), (unsigned)b86_pj_state_bytes());
    }
#endif

    int rc = B86_OK, pe = 0;
    unsigned long pe_in = 0, pe_out = 0;
    unsigned long long jit_insns = 0, jit_runs = 0, nudges = 0;
#ifndef DOS4GW_JIT
    if (use_jit) { logf_("built without -DDOS4GW_JIT (needs src/jit.c + a backend)"); return 2; }
#else
    if (use_jit) {
        static uint8_t hot[64 * 1024] __attribute__((aligned(64)));
        size_t code_bytes = 224u * 1024u;
        void *code = mmap(NULL, code_bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED || b86_jit_hot_bytes() > sizeof hot) { logf_("jit buffers"); return 2; }
        J = b86_jit_create_ex(&C, code, code_bytes, hot, sizeof hot);
        if (!J) { logf_("b86_jit_create_ex failed"); return 2; }
        b86_jit_set_shadow(J, aligned_alloc(64, (B86_MEM_BYTES + 63u) & ~63u));
        b86_jit_set_max_block(J, 48);
        b86_jit_set_count_retired(J, 1);
        b86_jit_set_hot_threshold(J, 64);
        logf_("JIT on: %u KiB code cache, hot threshold 64 (blitzBUS settings)", (unsigned)(code_bytes / 1024u));
    }
#endif
    while (use_jit && jit_insns + C.icount < max && g_exit < 0) {
        uint32_t i0 = C.icnt; uint64_t s0 = C.icount;
        uint16_t cs0 = (uint16_t)C.seg[B86_CS]; uint32_t ip0 = C.ip;
        record();
        C.irq = 0;
#ifdef DOS4GW_JIT
        rc = b86_jit_run(&C, 4096);
#else
        rc = b86_step(&C);
#endif
        ++jit_runs;
        jit_insns += (uint32_t)(C.icnt - i0);
        tick(jit_insns);
        int progress = C.icnt != i0 || C.icount != s0 || (uint16_t)C.seg[B86_CS] != cs0 || C.ip != ip0;
        if ((rc == B86_BUDGET || rc == B86_EXIT) && !progress) {
            ++nudges;                       /* bb_live v12: one interpreter step */
            rc = b86_step(&C);
        }
        if ((C.msw & 1) != pe) {
            pe = C.msw & 1;
            if (pe) ++pe_in; else ++pe_out;
            if (g_trace_pe || pe_in + pe_out <= 8)
                logf_("%s protected mode #%lu (slice %llu) -> %04X:%08X", pe ? "ENTER" : "LEAVE", pe ? pe_in : pe_out,
                      jit_runs, (unsigned)(uint16_t)C.seg[B86_CS], C.ip);
        }
        if (rc != B86_OK && rc != B86_BUDGET && rc != B86_EXIT) break;
        rc = B86_OK;
    }
    if (use_jit) logf_("JIT: %llu native-retired, %llu interpreted, %llu runs, %llu no-progress nudges",
                       jit_insns, (unsigned long long)C.icount, jit_runs, nudges);
    while (!use_jit && C.icount < max && g_exit < 0) {
        record();
        if (C.icount >= g_tfrom && C.icount < g_tfrom + g_tcount) {
            Tr *t = &g_ring[(g_rpos - 1) % RING];
            fprintf(stderr, "%llu %s %04X:%04X %02X %02X %02X %02X %02X %02X  AX=%08X BX=%08X CX=%08X DX=%08X SI=%08X DI=%08X BP=%08X SP=%08X DS=%04X ES=%04X SS=%04X F=%04X\n",
                (unsigned long long)C.icount, t->pe ? (t->d ? "P32" : "P16") : "R16", t->cs, t->ip, t->b[0], t->b[1], t->b[2], t->b[3], t->b[4], t->b[5],
                C.r[0], C.r[3], C.r[1], C.r[2], C.r[6], C.r[7], C.r[5], C.r[4], (unsigned)(uint16_t)C.seg[B86_DS], (unsigned)(uint16_t)C.seg[B86_ES], (unsigned)(uint16_t)C.seg[B86_SS], b86_get_flags(&C));
        }
        rc = b86_step(&C);
        tick(C.icount);
        if ((C.msw & 1) != pe) {
            pe = C.msw & 1;
            if (pe) ++pe_in; else ++pe_out;
            if (g_trace_pe || pe_in + pe_out <= 8)
                logf_("%s protected mode #%lu at insn %llu -> %04X:%08X", pe ? "ENTER" : "LEAVE", pe ? pe_in : pe_out,
                      (unsigned long long)C.icount, (unsigned)(uint16_t)C.seg[B86_CS], C.ip);
        }
        if (rc != B86_OK) break;
    }
    if (g_exit >= 0) logf_("program exited with code %d after %llu insns (PE entries %lu, exits %lu)", g_exit,
                           (unsigned long long)C.icount, pe_in, pe_out);
    else dump(rc == B86_OK ? "step limit" : rc == B86_HALT ? "HLT" : rc == B86_UNSUPPORTED ? "UNSUPPORTED" :
              rc == B86_SHUTDOWN ? "SHUTDOWN" : "rc", rc);
    fprintf(stderr, "[host] PE entries %lu exits %lu\n", pe_in, pe_out);
#if defined(B86_PJ) && B86_PJ
    if (g_pj) { unsigned long st[8], so[256]; b86_pj_stats(&C, st); b86_pj_stop_ops(&C, so);
        fprintf(stderr, "[host] pj: blocks %lu flushes %lu runs %lu insns %lu (%.1f/run) mem-exits %lu declined %lu code %lu/%lu\n",
                st[0], st[1], st[2], st[3], st[2] ? (double)st[3] / st[2] : 0.0, st[4], st[5], st[6], st[7]);
        fprintf(stderr, "[host] pj blocks end at:");
        for (int k = 0; k < 16; ++k) { int best = 0; for (int q = 1; q < 256; ++q) if (so[q] > so[best]) best = q; if (!so[best]) break; fprintf(stderr, " %02X:%lu", best, so[best]); so[best] = 0; }
        fprintf(stderr, "\n"); }
#endif
#ifdef B86_PJ_VERIFY
    { extern unsigned long b86_pj_vfy_checked, b86_pj_vfy_bad;
      fprintf(stderr, "[host] pj-verify: blocks checked %lu mismatches %lu\n", b86_pj_vfy_checked, b86_pj_vfy_bad); }
#endif
    { extern unsigned long b86_pm40_stat_ok, b86_pm40_stat_decline;
      fprintf(stderr, "[host] pm40: executed %lu declined %lu\n", b86_pm40_stat_ok, b86_pm40_stat_decline); }
#ifdef B86_PM40_STATS
    { extern unsigned long b86_pm40_decl[512]; unsigned idx[512]; for (unsigned k = 0; k < 512; ++k) idx[k] = k;
      for (unsigned a = 0; a < 24; ++a) for (unsigned b = a + 1; b < 512; ++b) if (b86_pm40_decl[idx[b]] > b86_pm40_decl[idx[a]]) { unsigned t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
      fprintf(stderr, "[host] pm40 declined by opcode:");
      for (unsigned a = 0; a < 24 && b86_pm40_decl[idx[a]]; ++a) fprintf(stderr, " %s%02X:%lu", idx[a] >= 256 ? "0F" : "", idx[a] & 255, b86_pm40_decl[idx[a]]);
      fprintf(stderr, "\n"); }
#endif
#ifdef B86_PM40_VERIFY
    { extern unsigned long b86_pm40_vfy_checked, b86_pm40_vfy_bad;
      fprintf(stderr, "[host] pm40-verify: checked %lu mismatches %lu\n", b86_pm40_vfy_checked, b86_pm40_vfy_bad); }
#endif
    if (g_irq_on) {
        extern unsigned long b86_irq_stat_rm, b86_irq_stat_pm, b86_irq_stat_task, b86_irq_stat_refused;
        fprintf(stderr, "[host] IRQ0 edges %llu delivered %llu (real %lu, pm %lu, task %lu, refused %lu) pit reload %u isr %02X imr %02X\n",
                g_irq_edges, g_irq_delivered, b86_irq_stat_rm, b86_irq_stat_pm, b86_irq_stat_task, b86_irq_stat_refused,
                g_pit_reload, g_pic_isr, g_pic_mask[0]);
    }
    if (g_vmode == 0x13) {                       /* r68: mode 13h screenshot */
        FILE *pf = fopen("screen.ppm", "wb");
        if (pf) {
            fprintf(pf, "P6\n320 200\n255\n");
            for (unsigned k = 0; k < 64000u; ++k) { const uint8_t *e = g_pal[M[0xA0000u + k]];
                fputc(e[0] * 255 / 63, pf); fputc(e[1] * 255 / 63, pf); fputc(e[2] * 255 / 63, pf); }
            fclose(pf); logf_("wrote screen.ppm (mode 13h)");
        }
    }
    { FILE *mf = fopen("dos4gw_mem.bin", "wb"); if (mf) { fwrite(M, 1, B86_RAM_BYTES, mf); fclose(mf); } }
    return g_exit >= 0 ? 0 : 1;
}

/* B86_PG_V1 (r65): 80386 two-level paging for protected mode.
 * Included by interp.c before the PM stages. CR0.PG is mirrored in c->pg and
 * is only set while PE=1, so real mode and the JIT never translate.
 *  - 4 KiB pages, PDE/PTE present bits; supervisor/user and R/W are not
 *    enforced (a 386 at CPL0 ignores them and the guest runs at CPL0);
 *  - Accessed is set on the PDE, Accessed+Dirty on the PTE whenever a
 *    translation is filled (every access is treated as a possible write so a
 *    VMM never discards a modified page);
 *  - 64-entry direct-mapped TLB, flushed by CR3 writes and PG changes;
 *  - a miss records the first faulting linear address (CR2) and returns a
 *    scratch page; b86_step rolls the instruction back and delivers #PF.
 * Physical frames beyond guest RAM read/write the scratch page. */
#ifndef B86_PG_FLASH
#define B86_PG_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static uint8_t b86_pg_sink[256u + 16u];   /* scratch for non-present/unbacked pages */

/* V30M PHYSICAL PAGE JOURNAL OBSERVER, STRICTLY NON-AUTHORITATIVE.
 * The real-mode and PM native/interpret store hook is smc(); PTE bit writes
 * are observed separately below. Host loaders, DMA, or other direct writes
 * may bypass the hook. Never use these generations to skip exact checks.
 * Observe=0 retains the original paging source behavior and no table. */
#ifndef B86_PJ_V30M_OBSERVE
#if defined(B86_PJ) && B86_PJ
#define B86_PJ_V30M_OBSERVE 0
#else
#define B86_PJ_V30M_OBSERVE 0
#endif
#endif
#if B86_PJ_V30M_OBSERVE != 0 && B86_PJ_V30M_OBSERVE != 1
#error "B86_PJ_V30M_OBSERVE must be 0 or 1"
#endif
#if B86_PJ_V30M_OBSERVE
#define B86_V30M_NPAGES ((B86_RAM_BYTES + 4095u) / 4096u)
/* uint16 generations are telemetry ONLY. A wrap never asserts validity. */
static uint16_t b86_v30m_generation[B86_V30M_NPAGES];
static uint32_t b86_v30m_seen[(B86_V30M_NPAGES + 31u) / 32u];
static unsigned long b86_v30m_unique;
static unsigned long b86_v30m_rm_stores;
static unsigned long b86_v30m_pm_stores;
static unsigned long b86_v30m_pte_writes;
static unsigned long b86_v30m_unknown;
static unsigned long b86_v30m_generation_wraps;
static inline void b86_v30m_note_physical(B86Cpu *c, uint32_t phys,
                                           uint32_t n, unsigned kind)
{
    uint32_t last, pg;
    if (kind == 2u) ++b86_v30m_pte_writes;
    else if (c->pg) ++b86_v30m_pm_stores;
    else ++b86_v30m_rm_stores;
    if (!n || phys >= B86_RAM_BYTES || n > B86_RAM_BYTES - phys) {
        ++b86_v30m_unknown;
        return;
    }
    last = (phys + n - 1u) >> 12;
    for (pg = phys >> 12; ; ++pg) {
        const uint32_t bit = 1u << (pg & 31u);
        uint16_t next;
        if (!(b86_v30m_seen[pg >> 5] & bit)) {
            b86_v30m_seen[pg >> 5] |= bit;
            ++b86_v30m_unique;
        }
        next = (uint16_t)(b86_v30m_generation[pg] + 1u);
        if (!next) ++b86_v30m_generation_wraps;
        b86_v30m_generation[pg] = next;
        if (pg == last) break;
    }
}
static inline void b86_v30m_note_linear(B86Cpu *c, uint32_t lin,
                                        uint32_t n)
{
    uint32_t remain = n;
    if (!n || n > B86_RAM_BYTES || lin > UINT32_MAX - (n - 1u)) {
        ++b86_v30m_unknown;
        return;
    }
    while (remain) {
        uint32_t span = 4096u - (lin & 4095u), phys = lin;
        if (span > remain) span = remain;
        if (c->pg) {
            const unsigned slot = (lin >> 12) & 63u;
            if (c->tlb_tag[slot] != ((lin & 0xFFFFF000u) | 1u)) {
                /* Never walk a missed TLB entry merely for profiling:
                 * this could set A/D bits or alter paging semantics. */
                ++b86_v30m_unknown;
                lin += span;
                remain -= span;
                continue;
            }
            phys = c->tlb_phys[slot] | (lin & 4095u);
        }
        b86_v30m_note_physical(c, phys, span, 0u);
        lin += span;
        remain -= span;
    }
}
#endif /* B86_PJ_V30M_OBSERVE */

/* V33 conservative physical dirty-page foundation.
 * - Exactly one byte per 4 KiB physical guest RAM page, monotonic 0->1.
 * - Marks are made before stores; false positives are safe.
 * - Missing TLB/bounds => an unknown counter, NEVER an assumed clean page.
 * - Host memcpy/DMA/BIOS direct access is NOT yet instrumented.
 * - No caller may use this table to skip pj_v29_walk or PJ coherence.
 * - The v25 native fastwrite emitter marks by physical frame independently.
 */
#if B86_PJ_V33_PHYSDIRTY
#define B86_V33_NPAGES ((B86_RAM_BYTES + 4095u) / 4096u)
static uint8_t b86_v33_dirty[B86_V33_NPAGES];
static unsigned long b86_v33_unknown, b86_v33_pte, b86_v33_cpu;
/* BB_V37_EPOCH_LEDGER_V1: observational transition-scoped physical page marks.
 * Native ARM stores stamp directly in the same table as C stores.
 * Stamps never authorize skipping an exact byte or physical-map comparison.
 * On 8-bit stamp wrap clear all entries before reusing token 1. */
#define B86_PJ_V37_EPOCH_LEDGER 1
#if B86_PJ_V37_EPOCH_LEDGER
static uint8_t b86_v37_page_epoch[B86_V33_NPAGES];
static volatile uint8_t b86_v37_token = 1u;
/* V42: provenance of the just-closing execution window.  The RM/PM
 * labels are supplied by the two real transitions, not inferred from parity.
 * All values are diagnostics only and must never control PJ admission. */
#define B86_V42_CLOSE_RM 1u
#define B86_V42_CLOSE_PM 2u
static uint8_t b86_v42_closing_kind = 0u;
static unsigned long b86_v42_rm_windows, b86_v42_pm_windows, b86_v42_other_windows;

static unsigned long b86_v37_windows, b86_v37_pages_sum, b86_v37_pages_peak;
static unsigned long b86_v37_empty_windows, b86_v37_unsafe_windows;
static unsigned long b86_v37_unknown_seen, b86_v37_wraps;
static unsigned long b86_v37_last_pages;
/* BB_V38_SPOT_ORACLE_V1: observational cross-window byte sentinel.
 * 32 pages x 16 dispersed bytes, rotating through guest physical RAM.
 * A changed sampled byte with no dirty stamp is positive evidence of a
 * missed write or other coherence issue. A clean sample is NOT proof that
 * the page is clean: this is neither a full-page hash nor a complete audit.
 * Remapped host pointers are classified unknown, not as missing writes.
 * No reader may use these counters to skip exact source comparison. */
#ifndef B86_PJ_V38_SPOT_ORACLE
#define B86_PJ_V38_SPOT_ORACLE 1
#endif
#if B86_PJ_V38_SPOT_ORACLE
#define B86_V38_SAMPLE_PAGES 32u
#define B86_V38_SAMPLE_BYTES 16u
static uint32_t b86_v38_page[B86_V38_SAMPLE_PAGES];
static uintptr_t b86_v38_ptr[B86_V38_SAMPLE_PAGES];
static uint8_t b86_v38_bytes[B86_V38_SAMPLE_PAGES][B86_V38_SAMPLE_BYTES];
static uint8_t b86_v38_valid;
static unsigned long b86_v38_windows, b86_v38_compared_pages;
static unsigned long b86_v38_changed, b86_v38_covered, b86_v38_missed;
static unsigned long b86_v38_alias_skips, b86_v38_incomplete_pages;
static unsigned long b86_v38_missed_windows;
static void b86_v38_sample_window(B86Cpu *c, uint8_t closing_token, unsigned phase)
{
    unsigned k, j, missed_this_window = 0u;
    /* Exactly the same physical host mapping must be observed at both
     * boundaries. If the mapped pointer changed, the comparison would
     * misattribute an alias/remap as a physical memory write. */
    if (!c || !c->mem) return;
    if (!phase) {
    ++b86_v38_windows;
    if (b86_v38_valid) {
        for (k = 0u; k < B86_V38_SAMPLE_PAGES; ++k) {
            const uint32_t pg = b86_v38_page[k];
            const uint32_t phys = pg << 12;
            const uint8_t *p;
            unsigned changed = 0u;
            if (phys >= B86_RAM_BYTES || B86_RAM_BYTES - phys < 4096u) {
                ++b86_v38_incomplete_pages; continue;
            }
            p = b86_host_phys(c, phys);
            if ((uintptr_t)p != b86_v38_ptr[k]) {
                ++b86_v38_alias_skips; continue;
            }
            ++b86_v38_compared_pages;
            for (j = 0u; j < B86_V38_SAMPLE_BYTES; ++j) {
                if (p[j * 256u] != b86_v38_bytes[k][j]) {
                    changed = 1u; break;
                }
            }
            if (changed) {
                ++b86_v38_changed;
                if (b86_v37_page_epoch[pg] == closing_token)
                    ++b86_v38_covered;
                else {
                    ++b86_v38_missed;
                    missed_this_window = 1u;
                }
            }
        }
    }
    if (missed_this_window) ++b86_v38_missed_windows;
    return; /* closing snapshot only; token has not advanced yet */
    }
    /* New window: caller already advanced b86_v37_token. */
    /* Arm baseline after token rotation, never under the closing token. */
    for (k = 0u; k < B86_V38_SAMPLE_PAGES; ++k) {
        const uint32_t pg = (uint32_t)(((b86_v38_windows - 1u) * B86_V38_SAMPLE_PAGES + k) % B86_V33_NPAGES);
        const uint32_t phys = pg << 12;
        const uint8_t *p;
        b86_v38_page[k] = pg;
        if (phys >= B86_RAM_BYTES || B86_RAM_BYTES - phys < 4096u) {
            b86_v38_ptr[k] = 0u;
            ++b86_v38_incomplete_pages;
            for (j = 0u; j < B86_V38_SAMPLE_BYTES; ++j)
                b86_v38_bytes[k][j] = 0u;
            continue;
        }
        p = b86_host_phys(c, phys);
        b86_v38_ptr[k] = (uintptr_t)p;
        for (j = 0u; j < B86_V38_SAMPLE_BYTES; ++j)
            b86_v38_bytes[k][j] = p[j * 256u];
    }
    b86_v38_valid = 1u;
}
#endif
/* BB_V39_FULLPAGE_HASH_ORACLE_V1. Observational, not authoritative.
 * Two 4 KiB physical pages per window, entire page byte stream consumed
 * via two distinct 32-bit rolling checksums. Pointer stability is required;
 * a checksum collision or a write-then-restore is still undetectable.
 * Only v37's prior-window page stamp can classify the observed change;
 * this cannot authorize any exact-compare or native-unlink bypass.
 * O(8 KiB) extra scans per boundary and < 100 B of static metadata. */
#ifndef B86_PJ_V39_HASH_ORACLE
#define B86_PJ_V39_HASH_ORACLE 1
#endif
#if B86_PJ_V39_HASH_ORACLE
static unsigned long b86_v42_rm_checked, b86_v42_pm_checked, b86_v42_other_checked;
static unsigned long b86_v42_rm_changed, b86_v42_pm_changed, b86_v42_other_changed;
static unsigned long b86_v42_rm_missed, b86_v42_pm_missed, b86_v42_other_missed;
#define B86_V39_FULL_PAGES 2u
static uint32_t b86_v39_page[B86_V39_FULL_PAGES];
static uintptr_t b86_v39_ptr[B86_V39_FULL_PAGES];
static uint32_t b86_v39_hash_a[B86_V39_FULL_PAGES];
static uint32_t b86_v39_hash_b[B86_V39_FULL_PAGES];
/* v40: bounded, diagnostic-only locator. No coherence decisions use it. */
#ifndef B86_PJ_V40_MISSLOC
#define B86_PJ_V40_MISSLOC 1
#endif
#if B86_PJ_V40_MISSLOC
#define B86_V40_CHUNKS_PER_PAGE 16u
#define B86_V40_EVENT_CAP 32u
static uint32_t b86_v40_chunks[B86_V39_FULL_PAGES][B86_V40_CHUNKS_PER_PAGE];
typedef struct {
    uint32_t window, page;
    uint16_t chunk_mask;
    uint8_t closing_token, observed_stamp, closing_kind;
} B86V40MissEvent;
static B86V40MissEvent b86_v40_events[B86_V40_EVENT_CAP];
static unsigned long b86_v40_total, b86_v40_printed, b86_v40_lost;
static void b86_v40_note_miss(uint32_t win, uint32_t pg, uint8_t token,
                               uint8_t observed, uint16_t chunk_mask)
{
    /* Retained for the outer JIT statistics printer, not needed by host test. */
    (void)b86_v40_printed;
    (void)b86_v40_lost;
    B86V40MissEvent *e = &b86_v40_events[b86_v40_total % B86_V40_EVENT_CAP];
    e->window = win;
    e->page = pg;
    e->chunk_mask = chunk_mask;
    e->closing_token = token;
    e->observed_stamp = observed;
    e->closing_kind = b86_v42_closing_kind;
    ++b86_v40_total;
}
#endif
static uint8_t b86_v39_valid;
static unsigned long b86_v39_windows, b86_v39_compared, b86_v39_changed;
static unsigned long b86_v39_covered, b86_v39_missed, b86_v39_alias;
static unsigned long b86_v39_incomplete, b86_v39_missed_windows;
static uint32_t b86_v39_first_missed_page=UINT32_MAX;
static unsigned b86_v39_first_missed_token;
static void b86_v39_hash_page(const uint8_t *p, uint32_t *out_a, uint32_t *out_b
#if B86_PJ_V40_MISSLOC
    , uint32_t *chunks
#endif
    )
{
    uint32_t i, a = 2166136261u, b = 0x9e3779b9u;
#if B86_PJ_V40_MISSLOC
    uint32_t ch = 2166136261u;
#endif
    /* Assemble words from bytes, so alignment and host endianness cannot
     * influence the result. Each page byte contributes to BOTH checksums. */
    for (i = 0u; i < 4096u; i += 4u) {
        uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i+1u] << 8u)
            | ((uint32_t)p[i+2u] << 16u) | ((uint32_t)p[i+3u] << 24u);
        a = (a ^ v) * 16777619u;
        b += (v ^ (v >> 16u)) + 0x7f4a7c15u;
        b = (b << 5u) | (b >> 27u);
#if B86_PJ_V40_MISSLOC
        /* Same loaded word; no additional guest-memory reads. */
        ch = (ch ^ v) * 16777619u;
        if ((i & 255u) == 252u) {
            if (chunks) chunks[i >> 8u] = ch;
            ch = 2166136261u;
        }
#endif
    }
    *out_a = a;
    *out_b = b;
}
static void b86_v39_check_window(B86Cpu *c, uint8_t closing_token, unsigned phase)
{
    unsigned k, miss = 0u;
    unsigned long idx;
    if (!c || !c->mem) return;
    if (!phase) {
    ++b86_v39_windows;
    if (b86_v39_valid) {
        for (k = 0u; k < B86_V39_FULL_PAGES; ++k) {
            const uint32_t pg = b86_v39_page[k], phys = pg << 12u;
            uint32_t a, b;
#if B86_PJ_V40_MISSLOC
            uint32_t checked_chunks[B86_V40_CHUNKS_PER_PAGE];
#endif
            const uint8_t *p;
            if (phys >= B86_RAM_BYTES || B86_RAM_BYTES - phys < 4096u) {
                ++b86_v39_incomplete;
                continue;
            }
            p = b86_host_phys(c, phys);
            if ((uintptr_t)p != b86_v39_ptr[k]) {
                ++b86_v39_alias;
                continue;
            }
            b86_v39_hash_page(p, &a, &b
#if B86_PJ_V40_MISSLOC
                , checked_chunks
#endif
                );
            ++b86_v39_compared;
            if (b86_v42_closing_kind == B86_V42_CLOSE_RM) ++b86_v42_rm_checked;
            else if (b86_v42_closing_kind == B86_V42_CLOSE_PM) ++b86_v42_pm_checked;
            else ++b86_v42_other_checked;
            if (a != b86_v39_hash_a[k] || b != b86_v39_hash_b[k]) {
                ++b86_v39_changed;
                if (b86_v42_closing_kind == B86_V42_CLOSE_RM) ++b86_v42_rm_changed;
                else if (b86_v42_closing_kind == B86_V42_CLOSE_PM) ++b86_v42_pm_changed;
                else ++b86_v42_other_changed;
                if (b86_v37_page_epoch[pg] == closing_token)
                    ++b86_v39_covered;
                else {
                    ++b86_v39_missed;
                    if (b86_v42_closing_kind == B86_V42_CLOSE_RM) ++b86_v42_rm_missed;
                    else if (b86_v42_closing_kind == B86_V42_CLOSE_PM) ++b86_v42_pm_missed;
                    else ++b86_v42_other_missed;
                    miss = 1u;
#if B86_PJ_V40_MISSLOC
                    {
                        uint16_t changed_chunks = 0u;
                        unsigned chunk;
                        for (chunk = 0u; chunk < B86_V40_CHUNKS_PER_PAGE; ++chunk)
                            if (checked_chunks[chunk] != b86_v40_chunks[k][chunk])
                                changed_chunks = (uint16_t)(changed_chunks | (uint16_t)(1u << chunk));
                        b86_v40_note_miss((uint32_t)b86_v39_windows, pg,
                            closing_token, b86_v37_page_epoch[pg], changed_chunks);
                    }
#endif
                    if (b86_v39_first_missed_page == UINT32_MAX) {
                        b86_v39_first_missed_page = pg;
                        b86_v39_first_missed_token = closing_token;
                    }
                }
            }
        }
    }
    if (miss) ++b86_v39_missed_windows;
    return; /* complete comparison while closing token is active */
    }
    /* Only now capture a baseline for the freshly opened token. */
    idx = b86_v39_windows - 1u;
    /* 32 consecutive windows visit two offsets per 32-page group.
     * Each later 32-window cycle shifts the pair one position, ensuring
     * every physical page can be sampled across 16 x 32 windows when
     * B86_V33_NPAGES == 1024. Never interpret this as continuous coverage. */
    for (k = 0u; k < B86_V39_FULL_PAGES; ++k) {
        const uint32_t pg = (uint32_t)(((idx % 32u) * 32u
            + ((idx / 32u) % 16u) + k * 16u) % B86_V33_NPAGES);
        const uint32_t phys = pg << 12u;
        const uint8_t *p;
        b86_v39_page[k] = pg;
        if (phys >= B86_RAM_BYTES || B86_RAM_BYTES - phys < 4096u) {
            b86_v39_ptr[k] = 0u;
            b86_v39_hash_a[k] = b86_v39_hash_b[k] = 0u;
            ++b86_v39_incomplete;
            continue;
        }
        p = b86_host_phys(c, phys);
        b86_v39_ptr[k] = (uintptr_t)p;
        b86_v39_hash_page(p, &b86_v39_hash_a[k], &b86_v39_hash_b[k]
#if B86_PJ_V40_MISSLOC
            , b86_v40_chunks[k]
#endif
            );
    }
    b86_v39_valid = 1u;
}
#endif
/* Called at RM entry and at the next PM reconciliation. Never hot native. */
static void b86_v37_rotate(B86Cpu *c)
{
    uint32_t k, pages = 0u;
    const uint8_t completed = b86_v37_token;
    for (k = 0u; k < B86_V33_NPAGES; ++k)
        pages += b86_v37_page_epoch[k] == completed;
    ++b86_v37_windows;
    if (b86_v42_closing_kind == B86_V42_CLOSE_RM) ++b86_v42_rm_windows;
    else if (b86_v42_closing_kind == B86_V42_CLOSE_PM) ++b86_v42_pm_windows;
    else ++b86_v42_other_windows;
    b86_v37_last_pages = pages;
    b86_v37_pages_sum += pages;
    if (pages > b86_v37_pages_peak) b86_v37_pages_peak = pages;
    if (!pages) ++b86_v37_empty_windows;
    if (b86_v33_unknown != b86_v37_unknown_seen)
        ++b86_v37_unsafe_windows;
    b86_v37_unknown_seen = b86_v33_unknown;
/* V41: close both measurements under the completed token; rotate the
     * token BEFORE acquiring new baselines. This avoids attributing writes
     * during baseline acquisition to a different window. The callback and
     * native store journal remain unchanged and non-authoritative. */
#if B86_PJ_V38_SPOT_ORACLE
    b86_v38_sample_window(c, completed, 0u);
#else
    (void)c;
#endif
#if B86_PJ_V39_HASH_ORACLE
    b86_v39_check_window(c, completed, 0u);
#endif
    if (completed == 255u) {
        memset(b86_v37_page_epoch, 0, sizeof b86_v37_page_epoch);
        b86_v37_token = 1u;
        ++b86_v37_wraps;
    } else {
        b86_v37_token = (uint8_t)(completed + 1u);
    }
#if B86_PJ_V38_SPOT_ORACLE
    b86_v38_sample_window(c, completed, 1u);
#endif
#if B86_PJ_V39_HASH_ORACLE
    b86_v39_check_window(c, completed, 1u);
#endif
}
#endif
static inline void b86_v33_note_physical(uint32_t phys, uint32_t n)
{
    uint32_t first, last;
    if (!n || phys >= B86_RAM_BYTES || n > B86_RAM_BYTES - phys) {
        ++b86_v33_unknown;
        return;
    }
    first = phys >> 12;
    last = (phys + n - 1u) >> 12;
    do {
        b86_v33_dirty[first] = 1u;
#if B86_PJ_V37_EPOCH_LEDGER
        b86_v37_page_epoch[first] = b86_v37_token;
#endif
    } while (first++ < last);
}
static inline void b86_v33_note_linear(B86Cpu *c, uint32_t lin, uint32_t n)
{
    uint32_t remain = n;
    ++b86_v33_cpu;
    if (!n || lin > UINT32_MAX - (n - 1u)) {
        ++b86_v33_unknown;
        return;
    }
    while (remain) {
        uint32_t span = 4096u - (lin & 4095u), phys = lin;
        if (span > remain) span = remain;
        if (c->pg) {
            const unsigned slot = (lin >> 12) & 63u;
            if (c->tlb_tag[slot] != ((lin & 0xFFFFF000u) | 1u)) {
                /* A missing TLB is unknown, not a clean page. Do not walk
                 * for journaling (that would create paging side effects). */
                ++b86_v33_unknown;
                lin += span;
                remain -= span;
                continue;
            }
            phys = c->tlb_phys[slot] | (lin & 4095u);
        }
        b86_v33_note_physical(phys, span);
        lin += span;
        remain -= span;
    }
}
#endif /* B86_PJ_V33_PHYSDIRTY */

/* r65 statistics (diagnostics only) */
unsigned long b86_pg_stat_faults, b86_pg_stat_tswitch, b86_pg_stat_walks, b86_pg_stat_flush;

static inline uint32_t b86_pg_rd32(const B86Cpu *c, uint32_t a)
{
    const uint8_t *p = b86_host_phys(c, a);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void b86_pg_wr32(B86Cpu *c, uint32_t a, uint32_t v)
{
    uint8_t *p = b86_host_phys(c, a);
#if B86_PJ_V33_PHYSDIRTY
    /* Implicit PDE/PTE accessed/dirty mutations bypass smc(). */
    ++b86_v33_pte;
    b86_v33_note_physical(a, 4u);
#endif
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
#if B86_PJ_V30M_OBSERVE
    /* Also record implicit x86 PDE/PTE Accessed/Dirty-bit writes. */
    b86_v30m_note_physical(c, a, 4u, 2u);
#endif
}
static B86_PG_FLASH void b86_pg_flush(B86Cpu *c)
{
    ++b86_pg_stat_flush;
    memset(c->tlb_tag, 0, sizeof c->tlb_tag);
}
/* 1 = translated (*phys), 0 = not present. Never records a fault. */
static B86_PM_HOT int b86_pg_walk(B86Cpu *c, uint32_t lin, uint32_t *phys)
{
    const unsigned slot = (lin >> 12) & 63u;
    const uint32_t tag = (lin & 0xFFFFF000u) | 1u;
    uint32_t pdea, pde, ptea, pte;
    if (c->tlb_tag[slot] == tag) { *phys = c->tlb_phys[slot] | (lin & 0xFFFu); return 1; }
    ++b86_pg_stat_walks;
    pdea = (c->cr3 & 0xFFFFF000u) + ((lin >> 22) << 2);
    if (pdea > B86_RAM_BYTES - 4u) return 0;
    pde = b86_pg_rd32(c, pdea);
    if (!(pde & 1u)) return 0;
    ptea = (pde & 0xFFFFF000u) + (((lin >> 12) & 0x3FFu) << 2);
    if (ptea > B86_RAM_BYTES - 4u) return 0;
    pte = b86_pg_rd32(c, ptea);
    if (!(pte & 1u)) return 0;
    if (!(pde & 0x20u)) b86_pg_wr32(c, pdea, pde | 0x20u);
    if ((pte & 0x60u) != 0x60u) b86_pg_wr32(c, ptea, pte | 0x60u);
    c->tlb_tag[slot] = tag;
    c->tlb_phys[slot] = pte & 0xFFFFF000u;
    *phys = (pte & 0xFFFFF000u) | (lin & 0xFFFu);
    return 1;
}
static void b86_pg_fault(B86Cpu *c, uint32_t lin, unsigned write)
{
    if (c->pf_pending) return;                 /* keep the first fault */
    c->pf_pending = 1u;
    c->pf_lin = lin;
    c->pf_err = (uint8_t)((write ? 2u : 0u) | ((c->seg[B86_CS] & 3u) == 3u ? 4u : 0u));
}
B86_PM_HOT uint8_t *b86_pg_host(B86Cpu *c, uint32_t lin)
{
    uint32_t phys;
    const unsigned slot = (lin >> 12) & 63u;                 /* r66: TLB hit inline */
    if (c->tlb_tag[slot] == ((lin & 0xFFFFF000u) | 1u)) {
        phys = c->tlb_phys[slot] | (lin & 0xFFFu);
        if (phys <= B86_RAM_BYTES - 1u) return b86_host_phys(c, phys);
    }
    if (!b86_pg_walk(c, lin, &phys)) {
        b86_pg_fault(c, lin, 0u);
        return b86_pg_sink + (lin & 0xFFu);
    }
    if (phys > B86_RAM_BYTES - 1u) return b86_pg_sink + (phys & 0xFFu);
    return b86_host_phys(c, phys);
}
/* Preflight every page of [lin, lin+width): 0 = #PF recorded. */
static B86_PG_FLASH int b86_pg_probe(B86Cpu *c, uint32_t lin, uint32_t width, unsigned write)
{
    uint32_t phys, at = lin, last = lin + (width ? width - 1u : 0u);
    for (;;) {
        if (!b86_pg_walk(c, at, &phys)) { b86_pg_fault(c, at, write); return 0; }
        if ((at & 0xFFFFF000u) == (last & 0xFFFFF000u)) return 1;
        at = (at & 0xFFFFF000u) + 0x1000u;
    }
}

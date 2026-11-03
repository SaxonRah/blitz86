/* dpmi_host: a DPMI 0.9 host for blitz86 (stage A), embedder-neutral.
 *
 * The embedder (dos4gw_host on the PC, blitzBUS on the Pico) provides:
 *   - the CPU and guest RAM,
 *   - a real-mode stub area (16 bytes below 1 MB) the host fills with INT stubs,
 *   - the current PSP segment,
 *   - the extended-memory pool [ext_base, ext_end) for linear allocations,
 * and calls:
 *   - dpmi_rm_int(c, vector): from its real-mode INT hook, for INT 2Fh and the
 *     host's own stub vectors (DPMI_VEC_ENTRY, DPMI_VEC_RMRET); returns 1 if handled,
 *   - dpmi_pm_trap(c): whenever the CPU is in protected mode with CS equal to
 *     dpmi_host_cs() (checked before every dispatch),
 *   - dpmi_active(): 1 once a client has entered protected mode through the host.
 * Everything else (INT 31h, reflection of unhooked interrupts, 0300h real-mode
 * calls) happens inside those two entry points as a state machine: the host never
 * runs the CPU itself.
 */
#ifndef DPMI_HOST_H
#define DPMI_HOST_H
#include "b86.h"

#define DPMI_VEC_ENTRY 0xE0u    /* real-mode stub: the DPMI entry point (far call) */
#define DPMI_VEC_RMRET 0xE1u    /* real-mode stub: return from a reflected INT / 0300h call */
#define DPMI_VEC_RAWPM 0xE2u    /* real-mode stub: raw switch real -> protected (0306h) */
#define DPMI_VEC_CB    0xE3u    /* real-mode stub: real-mode callback (0303h); slot byte follows the INT */

#define DPMI_VEC_PSP   0xE4u    /* real-mode stub: entry continues after INT 21h AH=62h */
#ifndef DPMI_COLD
#define DPMI_COLD __attribute__((noinline, section(".text.bb_dpmi")))   /* never in SRAM */
#endif
typedef struct DpmiEnv {
    uint8_t *mem;               /* guest RAM (physical) */
    void *(*alloc)(size_t n, size_t size);   /* zeroed allocation for the host state (PSRAM on the Pico) */
    uint32_t rm_trap_cs;        /* trap_cs to restore in real mode (blitzBUS: the BIOS segment) */
    uint16_t stub_seg, stub_off;/* real-mode stub area, 16 bytes, below 1 MB */
    uint16_t psp;               /* current PSP segment at entry */
    uint32_t ext_base, ext_end; /* linear pool for 0501h allocations */
    int trace;                  /* 1: log every host call to stderr */
    /* Virtual memory: client allocations live in a virtual region backed by
     * frames from [ext_base, ext_end) and a swap store of 4 KB slots. With no
     * swap callbacks, the region is limited to the frames (no eviction). */
    uint32_t vm_size;           /* bytes of virtual region (0 = 16 MB) */
    int pg_off_in_rm;           /* 1: paging off during host real-mode calls (0: identity-mapped, stays on) */
    int (*swap_read)(uint32_t slot, uint8_t *page4k);
    int (*swap_write)(uint32_t slot, const uint8_t *page4k);
} DpmiEnv;

DPMI_COLD void dpmi_init(DpmiEnv *env);
DPMI_COLD int  dpmi_rm_int(B86Cpu *c, uint8_t vector);
DPMI_COLD void dpmi_pm_trap(B86Cpu *c);
DPMI_COLD int  dpmi_active(void);
DPMI_COLD int  dpmi_ready(void);   /* 1 once dpmi_init has its state block */
DPMI_COLD uint16_t dpmi_host_cs(void);
DPMI_COLD void dpmi_report(void);         /* counters: calls per function, unsupported, reflections */

#endif

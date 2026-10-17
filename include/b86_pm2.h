/* Stage 5C5D: 80386 protected-mode segment-load planning and atomic commit.
 * NOT protected-mode execution: CR0.PE writes, descriptor accessed writes,
 * interrupt gates, task switches, control transfer execution and paging are
 * intentionally absent. A real-mode guest cannot invoke these helpers.
 */
#ifndef B86_PM2_H
#define B86_PM2_H
#include "b86_pm1.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    B86_PM2_OK = 0,
    B86_PM2_FAULT = 1,       /* fault output describes architecturally classified rejection */
    B86_PM2_UNSUPPORTED = 2, /* execution/address space not representable */
    B86_PM2_ARGUMENT = 3    /* programming error: no CPU mutations */
};
enum { B86_PM2_GP = 13, B86_PM2_SS = 12, B86_PM2_NP = 11 };
#define B86_PM2_PLAN_MAGIC 0x5c5d386u

typedef struct B86PM2Fault {
    uint16_t error_code; /* selector index and TI, RPL cleared; zero for offset faults */
    uint8_t vector;      /* 11=#NP, 12=#SS, 13=#GP */
    uint8_t reserved;
} B86PM2Fault;

typedef struct B86PM2Plan {
    B86PM1Cache cache;
    uint32_t target_eip;  /* validated far code target, zero for data/stack loads */
    uint32_t magic;
    uint16_t selector;    /* visible value (CS RPL rewritten to CPL) */
    uint8_t seg;          /* ES,CS,SS,DS,FS,GS */
    uint8_t cpl;          /* caller-supplied CPL; does not change during this stage */
} B86PM2Plan;

/* Pure validation: neither registers, descriptor tables nor caches change.
 * The caller provides the existing CPL. CS plans represent only same-level
 * far jumps, not calls, returns, gates, interrupts or task transitions.
 * Fault output is set only on B86_PM2_FAULT. */
int b86_pm2_plan_segment(const B86Cpu *cpu, unsigned seg, uint16_t selector,
                         unsigned cpl, uint32_t target_eip,
                         B86PM2Plan *plan, B86PM2Fault *fault);

/* Commit a successfully prepared plan. For safety, requires model=386 and
 * an externally established PE bit; ordinary guest MOV CR0 / LMSW still
 * refuse to set PE. The interpreter also refuses all PE execution.
 * Commits selector, hidden cache and optional EIP; never relies on the
 * real-mode segp[] bases. This is a building block, not an execution path. */
int b86_pm2_commit_segment(B86Cpu *cpu, const B86PM2Plan *plan,
                           unsigned cpl);

/* Check an already-cached segment address, classifying segment faults.
 * Doesn't perform the actual memory access. Does not do paging. */
int b86_pm2_check_access(const B86Cpu *cpu, unsigned seg, uint32_t offset,
                         uint32_t width, unsigned access, uint32_t *linear,
                         B86PM2Fault *fault);
#ifdef __cplusplus
}
#endif
#endif

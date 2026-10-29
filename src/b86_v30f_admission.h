/*
 * blitz86 v30f -- optional adaptive admission gate (not a standalone translator).
 * This file intentionally does not alter code-cache correctness or page invalidation.
 * All key construction, cache lookup, and interpreter fallbacks remain caller-owned.
 * Copyright (c) 2026 -- supplied for use in the blitz86 project.
 */
#ifndef B86_V30F_ADMISSION_H
#define B86_V30F_ADMISSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B86_V30F_WAYS 4u
#define B86_V30F_MIN_SLOTS 64u
#define B86_V30F_MAX_SLOTS 4096u
#define B86_V30F_ARM_EVICTIONS 32u

/* Slot storage must be supplied by the caller (prefer PSRAM, NOT Pico SRAM). */
typedef struct {
    uint64_t key;         /* caller's complete, context-aware cache identity */
    uint32_t generation;  /* caller's mapping/SMC coherence generation */
    uint16_t last_touch;  /* bounded deterministic replacement recency */
    uint8_t hits;         /* saturates at 4 */
    uint8_t valid;
} b86_v30f_slot;

typedef enum {
    B86_V30F_OFF = 0,
    B86_V30F_FIXED_2HIT = 2,
    B86_V30F_ADAPTIVE = 3,
    B86_V30F_GUARDED = 4  /* 2-hit filter with saturation escape */
} b86_v30f_mode;

typedef struct {
    b86_v30f_slot *slots;
    uint32_t slot_count;   /* power-of-two multiple of 4 */
    uint32_t evictions;
    uint32_t prev_blocks;  /* total successful PJ translations at previous eviction */
    uint32_t touch_seq;
    uint32_t first;
    uint32_t revisits;
    uint32_t replaced;
    uint32_t stale;
    uint32_t denied;
    uint32_t permitted;
    uint32_t prearm_permits;
    uint32_t pressure_up;
    uint32_t pressure_down;
    uint32_t last_blocks_per_evict;
    uint8_t pressure;      /* 0: 2-hit, 1: 3-hit, 2: 4-hit */
    uint8_t bad_streak;
    uint8_t calm_streak;
    uint8_t sample_ready;
    uint8_t mode;
    /* Guarded mode only. A 512-entry table cannot track millions of cold
     * misses; after two replacement-dominated windows, bypass entirely
     * for 16 real cache evictions. Saturation never changes correctness. */
    uint8_t guard_suspended;
    uint8_t guard_bad_windows;
    uint8_t guard_padding;
    uint32_t guard_resume_eviction;
    uint32_t guard_window_total;
    uint32_t guard_window_first;
    uint32_t guard_window_replaced;
    uint32_t guard_window_permitted;
    uint32_t guard_window_revisits;
    uint32_t guard_entries;
    uint32_t guard_exits;
    uint32_t guard_bypassed;
} b86_v30f_state;

/* Returns 1 on success, 0 for invalid inputs. No allocation, clock, or I/O. */
int b86_v30f_init(b86_v30f_state *state, b86_v30f_slot *slots,
                   uint32_t slot_count, b86_v30f_mode mode);

/* Eviction notification, called ONCE per actual PJ code-cache eviction.
 * compiled_blocks_total must be the monotonically wrapping 32-bit counter
 * for successfully translated PJ blocks, not the number of guest instructions.
 */
void b86_v30f_note_eviction(b86_v30f_state *state,
                             uint32_t compiled_blocks_total);

/* Call only for a PJ cache miss that would otherwise be eligible to compile.
 * 1 => may compile; 0 => use the EXISTING safe interpreter fallback.
 *
 * IMPORTANT: key must include the same logical identity as the existing v30e
 * admission/cache key, including address-space mode and CR3/mapping identity
 * when necessary. generation must change on any relevant paging / SMC change.
 * Do not feed a bare linear EIP if the same EIP can mean different code.
 */
int b86_v30f_allow(b86_v30f_state *state, uint64_t key,
                    uint32_t generation);

/* 2 in fixed/guarded filtering; 1 during guard bypass; 3/4 for legacy adaptive pressure. */
uint8_t b86_v30f_required_hits(const b86_v30f_state *state);

#ifdef __cplusplus
}
#endif
#endif

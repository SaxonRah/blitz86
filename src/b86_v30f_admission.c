#include "b86_v30f_admission.h"
#include <string.h>

/* Non-cryptographic avalanche. The caller must construct a correct key. */
static uint32_t b86_v30f_hash(uint64_t x) {
    /* Deliberately 32-bit: Cortex-M33 has no single-cycle 64-bit multiply. */
    uint32_t high = (uint32_t)(x >> 32);
    uint32_t h = (uint32_t)x ^ ((high << 13) | (high >> 19));
    h ^= h >> 16;
    h *= UINT32_C(0x7feb352d);
    h ^= h >> 15;
    h *= UINT32_C(0x846ca68b);
    h ^= h >> 16;
    return h;
}

int b86_v30f_init(b86_v30f_state *s, b86_v30f_slot *slots,
                   uint32_t n, b86_v30f_mode mode) {
    if (!s || !slots || n < B86_V30F_MIN_SLOTS || n > B86_V30F_MAX_SLOTS ||
        (n & (n - 1u)) || (n % B86_V30F_WAYS) != 0u ||
        (mode != B86_V30F_OFF && mode != B86_V30F_FIXED_2HIT &&
         mode != B86_V30F_ADAPTIVE && mode != B86_V30F_GUARDED)) return 0;
    memset(s, 0, sizeof(*s));
    memset(slots, 0, sizeof(*slots) * (size_t)n);
    s->slots = slots;
    s->slot_count = n;
    s->mode = (uint8_t)mode;
    return 1;
}

uint8_t b86_v30f_required_hits(const b86_v30f_state *s) {
    if (!s || s->mode != B86_V30F_ADAPTIVE)
        return (s && s->mode == B86_V30F_GUARDED && s->guard_suspended) ? 1u : 2u;
    return (uint8_t)(2u + s->pressure);
}

/* Pressure is sampled only at real evictions; avoids a timer and divisions
 * in the high-frequency admission path. Wrap-safe if successive block counts
 * differ by < 2^31, true for every practical single-eviction interval.
 *
 * 2 consecutive short intervals (< 4096 blocks/eviction): tighten +1.
 * 8 consecutive long intervals (> 8192 blocks/eviction): relax -1.
 * Hysteresis prevents rapid switching on mixed workloads.
 */
void b86_v30f_note_eviction(b86_v30f_state *s, uint32_t blocks_total) {
    uint32_t delta;
    if (!s || !s->slots || !s->slot_count) return;
    s->evictions++;
    if (s->mode == B86_V30F_GUARDED && s->guard_suspended &&
        (int32_t)(s->evictions - s->guard_resume_eviction) >= 0) {
        s->guard_suspended = 0u;
        s->guard_bad_windows = 0u;
        s->guard_window_total = 0u;
        s->guard_window_first = 0u;
        s->guard_window_replaced = 0u;
        s->guard_window_permitted = 0u;
        s->guard_window_revisits = 0u;
        s->guard_exits++;
    }
    if (!s->sample_ready) {
        s->prev_blocks = blocks_total;
        s->sample_ready = 1u;
        return;
    }
    delta = blocks_total - s->prev_blocks;
    s->prev_blocks = blocks_total;
    s->last_blocks_per_evict = delta;
    if (s->mode != B86_V30F_ADAPTIVE ||
        s->evictions < B86_V30F_ARM_EVICTIONS) return;
    if (delta < 4096u) {
        if (s->bad_streak < 255u) s->bad_streak++;
        s->calm_streak = 0u;
        if (s->bad_streak >= 2u && s->pressure < 2u) {
            s->pressure++;
            s->pressure_up++;
            s->bad_streak = 0u;
        }
    } else if (delta > 8192u) {
        if (s->calm_streak < 255u) s->calm_streak++;
        s->bad_streak = 0u;
        if (s->calm_streak >= 8u && s->pressure > 0u) {
            s->pressure--;
            s->pressure_down++;
            s->calm_streak = 0u;
        }
    } else {
        s->bad_streak = 0u;
        s->calm_streak = 0u;
    }
}

/* Called only in guarded mode on a filter decision. 4096-lookup
 * windows keep stats bounded; this never runs in the interpreter-bypass
 * path. A two-window requirement reduces false positive disengagement.
 * Escape deliberately favors guest progress over translation selectivity. */
static void b86_v30f_guard_observe(b86_v30f_state *s,
                                   unsigned first, unsigned replaced,
                                   unsigned revisit, unsigned permitted) {
    if (s->mode != B86_V30F_GUARDED) return;
    ++s->guard_window_total;
    s->guard_window_first += first;
    s->guard_window_replaced += replaced;
    s->guard_window_revisits += revisit;
    s->guard_window_permitted += permitted;
    if (s->guard_window_total != 4096u) return;
    if (s->guard_window_first >= 3072u &&
        s->guard_window_replaced >= 2560u &&
        s->guard_window_revisits <= 1024u &&
        s->guard_window_permitted <= 256u) {
        if (s->guard_bad_windows < 255u) ++s->guard_bad_windows;
        if (s->guard_bad_windows >= 2u) {
            s->guard_suspended = 1u;
            s->guard_resume_eviction = s->evictions + 16u;
            ++s->guard_entries;
        }
    } else {
        s->guard_bad_windows = 0u;
    }
    s->guard_window_total = 0u;
    s->guard_window_first = 0u;
    s->guard_window_replaced = 0u;
    s->guard_window_permitted = 0u;
    s->guard_window_revisits = 0u;
}

int b86_v30f_allow(b86_v30f_state *s, uint64_t key,
                    uint32_t generation) {
    uint32_t base, i, victim = 0;
    uint8_t victim_hits = 255u;
    uint16_t oldest_age = 0u;
    b86_v30f_slot *slot;
    if (!s || !s->slots || !s->slot_count) return 0; /* fail closed */
    if (s->mode == B86_V30F_OFF ||
        s->evictions < B86_V30F_ARM_EVICTIONS) {
        s->prearm_permits++;
        return 1;
    }
    /* Under proven table saturation, prefer normal native translation over
     * repeated interpreter fallback. This path does not call the hash or
     * touch PSRAM candidate slots, so it is genuinely cheap to bypass. */
    if (s->mode == B86_V30F_GUARDED && s->guard_suspended) {
        ++s->guard_bypassed;
        ++s->permitted;
        return 1;
    }
    /* Four-way set associativity with deterministic cold/stale priority. */
    base = (b86_v30f_hash(key) & ((s->slot_count / 4u) - 1u)) * 4u;
    s->touch_seq++;
    for (i = 0u; i < 4u; i++) {
        slot = &s->slots[base + i];
        if (slot->valid && slot->key == key) {
            if (slot->generation == generation) {
                if (slot->hits < 4u) slot->hits++;
                slot->last_touch = (uint16_t)s->touch_seq;
                s->revisits++;
                if (slot->hits >= b86_v30f_required_hits(s)) {
                    s->permitted++;
                    b86_v30f_guard_observe(s, 0u, 0u, 1u, 1u);
                    return 1;
                }
                s->denied++;
                b86_v30f_guard_observe(s, 0u, 0u, 1u, 0u);
                return 0;
            }
            /* Same key, changed coherence generation: it is a first sighting. */
            slot->generation = generation;
            slot->hits = 1u;
            slot->last_touch = (uint16_t)s->touch_seq;
            s->stale++;
            s->denied++;
            b86_v30f_guard_observe(s, 0u, 0u, 0u, 0u);
            return 0;
        }
    }
    for (i = 0u; i < 4u; i++) {
        uint16_t age;
        slot = &s->slots[base + i];
        if (!slot->valid) {
            victim = i;
            victim_hits = 0u;
            break;
        }
        age = (uint16_t)(s->touch_seq - (uint32_t)slot->last_touch);
        if (slot->hits < victim_hits ||
            (slot->hits == victim_hits && age > oldest_age)) {
            victim = i;
            victim_hits = slot->hits;
            oldest_age = age;
        }
    }
    slot = &s->slots[base + victim];
    {
        const unsigned was_replaced = slot->valid != 0u;
        if (was_replaced) s->replaced++;
        slot->valid = 1u;
        slot->key = key;
        slot->generation = generation;
        slot->hits = 1u;
        slot->last_touch = (uint16_t)s->touch_seq;
        s->first++;
        s->denied++;
        b86_v30f_guard_observe(s, 1u, was_replaced, 0u, 0u);
        return 0;
    }
}

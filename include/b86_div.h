/* b86_div.h -- r73i: x86 DIV/IDIV without libgcc's 64-bit division.
 *
 * The PM translator and the PM interpreter used C `uint64_t / uint32_t`
 * for DIV/IDIV, which the Cortex-M33 has no instruction for: every guest
 * divide called __aeabi_uldivmod -> __udivmoddi4 (the single largest C
 * function in the Pico profile, ~11%; Descent's fixed-point code divides
 * EDX:EAX constantly). Here 8/16-bit and EDX=0 cases use the hardware
 * 32/32 UDIV/SDIV, and the general 64/32 case uses Hacker's Delight
 * `divlu` (two 32/16 digit steps on top of UDIV). x86 guarantees the
 * quotient fits in 32 bits or the instruction faults (#DE), so the full
 * 64/64 algorithm is never needed.
 *
 * b86_divx() has exactly the semantics of the old b86_pj_div():
 *   lo/hi = AX|EAX and DX|EDX register contents (w=1 uses AX only),
 *   d = divisor (masked to the width here), w = 1/2/4, sgn = IDIV.
 *   Returns 0 for #DE (divide by zero or quotient overflow), else stores
 *   the quotient and remainder as 32-bit two's-complement values. */
#ifndef B86_DIV_H
#define B86_DIV_H
#include <stdint.h>

/* (u1:u0) / v with u1 < v, v != 0; quotient returned, remainder in *r. */
static inline uint32_t b86_divlu(uint32_t u1, uint32_t u0, uint32_t v, uint32_t *r)
{
    const uint32_t b = 65536u;
    uint32_t un1, un0, vn1, vn0, q1, q0, un32, un21, un10, rhat;
    const int s = __builtin_clz(v);
    v <<= s;
    vn1 = v >> 16; vn0 = v & 0xFFFFu;
    un32 = (u1 << s) | (s ? (u0 >> (32 - s)) : 0u);
    un10 = u0 << s;
    un1 = un10 >> 16; un0 = un10 & 0xFFFFu;
    q1 = un32 / vn1; rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) { --q1; rhat += vn1; if (rhat >= b) break; }
    un21 = un32 * b + un1 - q1 * v;
    q0 = un21 / vn1; rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) { --q0; rhat += vn1; if (rhat >= b) break; }
    *r = (un21 * b + un0 - q0 * v) >> s;
    return q1 * b + q0;
}

static inline int b86_divx(uint32_t lo, uint32_t hi, uint32_t d, unsigned w, unsigned sgn,
                           uint32_t *qo, uint32_t *ro)
{
    if (w == 4u) {
        if (!d) return 0;
        if (!sgn) {
            if (hi >= d) return 0;                         /* quotient >= 2^32 */
            if (!hi) { *qo = lo / d; *ro = lo % d; return 1; }
            *qo = b86_divlu(hi, lo, d, ro);
            return 1;
        } else {
            const int neg = (int32_t)hi < 0, dneg = (int32_t)d < 0;
            uint32_t uh = hi, ul = lo, ud = dneg ? 0u - d : d, qu, ru;
            if (neg) { ul = 0u - lo; uh = ~hi + (lo == 0u); }  /* 64-bit negate */
            if (uh >= ud) return 0;                         /* |quotient| >= 2^32 */
            if (!uh) { qu = ul / ud; ru = ul % ud; }
            else qu = b86_divlu(uh, ul, ud, &ru);
            if (neg != dneg) { if (qu > 0x80000000u) return 0; *qo = 0u - qu; }
            else { if (qu > 0x7FFFFFFFu) return 0; *qo = qu; }
            *ro = neg ? 0u - ru : ru;
            return 1;
        }
    }
    if (w == 2u) {
        d &= 0xFFFFu;
        if (!d) return 0;
        if (!sgn) {
            const uint32_t num = ((hi & 0xFFFFu) << 16) | (lo & 0xFFFFu), q = num / d;
            if (q > 0xFFFFu) return 0;
            *qo = q; *ro = num % d;
        } else {
            const int32_t num = (int32_t)(((hi & 0xFFFFu) << 16) | (lo & 0xFFFFu)), den = (int16_t)d;
            int32_t q;
            if (den == -1 && num == INT32_MIN) return 0;
            q = num / den;
            if (q > 32767 || q < -32768) return 0;
            *qo = (uint32_t)q; *ro = (uint32_t)(num % den);
        }
        return 1;
    }
    d &= 0xFFu;
    if (!d) return 0;
    if (!sgn) {
        const uint32_t num = lo & 0xFFFFu, q = num / d;
        if (q > 0xFFu) return 0;
        *qo = q; *ro = num % d;
    } else {
        const int32_t num = (int16_t)lo, den = (int8_t)d, q = num / den;
        if (q > 127 || q < -128) return 0;
        *qo = (uint32_t)q; *ro = (uint32_t)(num % den);
    }
    return 1;
}
#endif

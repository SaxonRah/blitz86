/* B86_PM31_V1: protected-mode odds and ends found by running DOS/4GW
 * (tests/dos4gw_host.c). Shared by 16- and 32-bit code segments; CPL0.
 *   90 NOP, CLI/STI/CLC/STC/CMC/CLD/STD with redundant 66h/67h/segment prefixes (DOS/4GW emits 67 90
 *      after 32-bit-address string moves: the 386 B1 errata workaround).
 * Returns -1 for everything else.
 */
#ifndef B86_PM31_FLASH
#define B86_PM31_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM31_FLASH int b86_pm31_step(B86Cpu *c)
{
    uint32_t start, ip;
    unsigned code32, nprefix = 0u;
    uint8_t op;
    if (!c || !c->mem || c->model != B86_CPU_386 || !(c->msw & 1u) || c->shutdown386 ||
        c->pm386.seg[B86_CS].reserved == B86_PM3_TRANSITION_MARKER ||
        !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS] & 3u) || c->ip > UINT32_MAX - 15u)
        return -1;
    code32 = !!(c->pm386.seg[B86_CS].flags & 4u);
    if (!code32 && c->ip > 0xFFFFu) return -1;
    start = ip = c->ip;
    for (;;) {
        if (!b86_pm4_byte(c, &ip, start, &op)) return -1;
        if (op == 0x66u || op == 0x67u || op == 0x26u || op == 0x2Eu || op == 0x36u ||
            op == 0x3Eu || op == 0x64u || op == 0x65u) { if (++nprefix > 4u) return -1; continue; }
        break;
    }
    if (nprefix && (op == 0x90u || op == 0xFAu || op == 0xFBu || op == 0xF8u || op == 0xF9u ||
                    op == 0xF5u || op == 0xFCu || op == 0xFDu)) {
        /* flag-only one-byte instructions behind redundant prefixes
         * (DOS/4GW: 67 FB after 67 66 F3 AA). Lazy flags are materialized
         * by b86_step before the PM chain. */
        if (c->lz_kind != LZ_NONE || c->lz_ikind != LZ_NONE) b86_flags_materialize(c);
        switch (op) {
        case 0xFAu: c->flags &= ~(uint32_t)B86_IF; break;
        case 0xFBu: c->flags |= B86_IF; break;
        case 0xF8u: c->flags &= ~(uint32_t)B86_CF; break;
        case 0xF9u: c->flags |= B86_CF; break;
        case 0xF5u: c->flags ^= B86_CF; break;
        case 0xFCu: c->flags &= ~(uint32_t)B86_DF; break;
        case 0xFDu: c->flags |= B86_DF; break;
        default: break;
        }
        c->ip = code32 ? ip : (ip & 0xFFFFu);
        ++c->icount;
        return B86_OK;
    }
    return -1;
}

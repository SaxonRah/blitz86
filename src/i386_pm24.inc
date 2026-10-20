/* r62: protected-mode integer core gap fill (all CPLs' flat/segmented code
 * in the same CPL0 environment as the earlier stages).
 *   00-3D  all eight ALU ops (r/m,r / r,r/m / AL|eAX,imm)
 *   80/81/83 immediate groups, 84/85 TEST, A8/A9 TEST, F6/F7 /0 TEST imm
 *   86/87 XCHG r/m, 91-97 XCHG eAX,r, 0F 90-9F SETcc, 98/99 handled earlier
 *   27/2F/37/3F DAA/DAS/AAA/AAS, D4/D5 AAM/AAD, F5/F8/F9 handled earlier
 * Decodes 66/67/segment prefixes, validates every memory operand through
 * the descriptor cache before any state change. Returns -1 when the opcode
 * belongs to the older stage chain. Flags are computed eagerly. */
#ifndef B86_PM24_FLASH
#define B86_PM24_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
static B86_PM24_FLASH unsigned b86_pm24_parity(uint32_t v)
{
    v &= 255u; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1u) ? 0u : 4u;
}
static B86_PM24_FLASH uint32_t b86_pm24_szp(uint32_t r, unsigned width)
{
    uint32_t mask = width == 1u ? 0xFFu : width == 2u ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t sign = width == 1u ? 0x80u : width == 2u ? 0x8000u : 0x80000000u;
    uint32_t f = b86_pm24_parity(r);
    if (!(r & mask)) f |= 0x40u;
    if (r & sign) f |= 0x80u;
    return f;
}
/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static B86_PM24_FLASH uint32_t b86_pm24_alu(B86Cpu *c, unsigned op, uint32_t d,
                                             uint32_t s, unsigned width, uint32_t *flags)
{
    uint32_t mask = width == 1u ? 0xFFu : width == 2u ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t sign = width == 1u ? 0x80u : width == 2u ? 0x8000u : 0x80000000u;
    uint32_t cin = (op == 2u || op == 3u) ? (c->flags & 1u) : 0u;
    uint64_t wide; uint32_t r, f = 0;
    d &= mask; s &= mask;
    switch (op) {
    case 0: case 2:
        wide = (uint64_t)d + s + cin; r = (uint32_t)wide & mask;
        if (wide > mask) f |= 1u;
        if (~(d ^ s) & (d ^ r) & sign) f |= 0x800u;
        f |= (d ^ s ^ r) & 0x10u;
        break;
    case 3: case 5: case 7:
        wide = (uint64_t)d - s - cin; r = (uint32_t)wide & mask;
        if ((uint64_t)s + cin > d) f |= 1u;
        if ((d ^ s) & (d ^ r) & sign) f |= 0x800u;
        f |= (d ^ s ^ r) & 0x10u;
        break;
    case 1: r = d | s; break;
    case 4: r = d & s; break;
    default: r = d ^ s; break;
    }
    f |= b86_pm24_szp(r, width);
    *flags = (c->flags & ~0x8D5u) | f;
    return r;
}
static B86_PM24_FLASH int b86_pm24_cond(const B86Cpu *c, unsigned cc)
{
    uint32_t f = c->flags; int r;
    switch (cc >> 1) {
    case 0: r = !!(f & 0x800u); break;
    case 1: r = !!(f & 1u); break;
    case 2: r = !!(f & 0x40u); break;
    case 3: r = !!(f & 0x41u); break;
    case 4: r = !!(f & 0x80u); break;
    case 5: r = !!(f & 4u); break;
    case 6: r = !!(f & 0x80u) != !!(f & 0x800u); break;
    default: r = (f & 0x40u) || (!!(f & 0x80u) != !!(f & 0x800u)); break;
    }
    return (cc & 1u) ? !r : r;
}
static B86_PM24_FLASH int b86_pm24_rmget(B86Cpu *c, const B86PM6EA *ea, uint32_t linear,
                                          unsigned width, uint32_t *v)
{
    uint32_t x = 0;
    if (ea->is_reg) { *v = b86_pm6_reg(c, ea->rm, width); return 1; }
    for (unsigned i = 0; i < width; i++) x |= (uint32_t)*b86_host(c, linear + i) << (8u * i);
    *v = x; return 1;
}
static B86_PM24_FLASH void b86_pm24_rmput(B86Cpu *c, const B86PM6EA *ea, uint32_t linear,
                                           unsigned width, uint32_t v)
{
    if (ea->is_reg) { b86_pm6_putreg(c, ea->rm, width, v); return; }
    for (unsigned i = 0; i < width; i++) *b86_host(c, linear + i) = (uint8_t)(v >> (8u * i));
    smc(c, linear, width);
}
B86_PM24_FLASH int b86_pm24_step(B86Cpu *c)
{
    B86PM6EA ea; B86PM2Fault fault;
    uint32_t start, ip, imm = 0, linear = 0, a = 0, b = 0, r, nf;
    unsigned width = 0, seg = B86_DS, kind, aluop = 0, access = 0, imm_bytes = 0;
    int pref66 = 0, pref67 = 0, segpref = 0, op32, ad32, code32, has_ea = 1;
    uint8_t op, op2 = 0;
    if (!c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access & 8u) ||
        (c->seg[B86_CS] & 3u) != 0u || c->ip > UINT32_MAX - 15u ||
        c->lz_kind != LZ_NONE || c->lz_ikind != LZ_NONE) return -1;
    code32 = !!(c->pm386.seg[B86_CS].flags & 4u);
    if (!code32 && c->ip > 0xFFFFu) return -1;
    start = ip = c->ip;
    for (;;) {
        if (!b86_pm4_byte(c, &ip, start, &op)) return -1;
        if (op == 0x66u) { pref66 = 1; continue; }
        if (op == 0x67u) { pref67 = 1; continue; }
        if (op == 0x26u || op == 0x2Eu || op == 0x36u || op == 0x3Eu || op == 0x64u || op == 0x65u) {
            segpref = 1;
            seg = op == 0x26u ? B86_ES : op == 0x2Eu ? B86_CS : op == 0x36u ? B86_SS :
                  op == 0x3Eu ? B86_DS : op == 0x64u ? B86_FS : B86_GS;
            continue;
        }
        break;
    }
    op32 = code32 ^ pref66; ad32 = code32 ^ pref67;
    /* classify */
    if (op < 0x40u && (op & 7u) < 4u && op != 0x0Fu) {            /* ALU r/m forms */
        kind = 1; aluop = op >> 3; width = (op & 1u) ? (op32 ? 4u : 2u) : 1u;
    } else if (op < 0x40u && ((op & 7u) == 4u || (op & 7u) == 5u)) { /* ALU acc,imm */
        kind = 2; aluop = op >> 3; width = (op & 1u) ? (op32 ? 4u : 2u) : 1u; has_ea = 0;
    } else if (op >= 0x80u && op <= 0x83u && op != 0x82u) {
        kind = 3; width = op == 0x80u ? 1u : (op32 ? 4u : 2u);
        imm_bytes = (op == 0x81u) ? width : 1u;
    } else if (op == 0x84u || op == 0x85u) { kind = 4; width = op == 0x84u ? 1u : (op32 ? 4u : 2u); }
    else if (op == 0xA8u || op == 0xA9u) { kind = 5; width = op == 0xA8u ? 1u : (op32 ? 4u : 2u); has_ea = 0; }
    else if (op == 0x86u || op == 0x87u) { kind = 6; width = op == 0x86u ? 1u : (op32 ? 4u : 2u); }
    else if (op >= 0x91u && op <= 0x97u) { kind = 7; width = op32 ? 4u : 2u; has_ea = 0; }
    else if (op == 0x0Fu) {
        if (!b86_pm4_byte(c, &ip, start, &op2)) return -1;
        if (op2 < 0x90u || op2 > 0x9Fu) return -1;
        kind = 8; width = 1u;
    } else if (op == 0x27u || op == 0x2Fu || op == 0x37u || op == 0x3Fu) { kind = 9; has_ea = 0; }
    else if (op == 0xF6u || op == 0xF7u) {
        /* only TEST r/m,imm (/0 and /1) here; the rest is in older stages */
        uint32_t peek = ip; uint8_t m;
        if (!b86_pm4_byte(c, &peek, start, &m) || ((m >> 3) & 6u) != 0u) return -1;
        kind = 10; width = op == 0xF6u ? 1u : (op32 ? 4u : 2u); imm_bytes = width;
    } else if ((op == 0xFEu || op == 0xFFu)) {
        uint32_t peek = ip; uint8_t m;
        if (!b86_pm4_byte(c, &peek, start, &m) || ((m >> 3) & 7u) > 1u) return -1;
        kind = 11; width = op == 0xFEu ? 1u : (op32 ? 4u : 2u);
    } else if (op >= 0xE0u && op <= 0xE3u) { kind = 12; has_ea = 0; }
    else if (op == 0xD4u || op == 0xD5u) { kind = 13; has_ea = 0; }
    else if (op == 0xC0u || op == 0xC1u || (op >= 0xD0u && op <= 0xD3u)) {
        uint32_t peek = ip; uint8_t m;
        if (!b86_pm4_byte(c, &peek, start, &m) || ((m >> 3) & 7u) == 6u) return -1;
        kind = 14; width = (op & 1u) ? (op32 ? 4u : 2u) : 1u;
        imm_bytes = (op == 0xC0u || op == 0xC1u) ? 1u : 0u;
    } else return -1;
    if (has_ea) {
        if (!b86_pm6_ea(c, &ip, start, ad32, &ea)) return B86_UNSUPPORTED;
        if (imm_bytes && !b86_pm6_imm(c, &ip, start, imm_bytes, &imm)) return B86_UNSUPPORTED;
        if (kind == 3 && imm_bytes == 1u && op == 0x83u)
            imm = width == 2u ? (uint32_t)(uint16_t)(int16_t)(int8_t)imm : (uint32_t)(int32_t)(int8_t)imm;
        if (!ea.is_reg) {
            unsigned reads_only = (kind == 1 && aluop == 7u) || kind == 4 || kind == 10 ||
                                  (kind == 3 && ea.reg == 7u) || (kind == 1 && (op & 2u));
            if (segpref) ea.seg = seg;
            access = reads_only ? B86_PM1_READ : (kind == 8 ? B86_PM1_WRITE : (B86_PM1_READ | B86_PM1_WRITE));
            if (b86_pm2_check_access(c, ea.seg, ea.off, width, access, &linear, &fault) != B86_PM2_OK)
                return B86_UNSUPPORTED;
        }
    } else if (kind == 2 || kind == 5) {
        if (!b86_pm6_imm(c, &ip, start, width, &imm)) return B86_UNSUPPORTED;
    } else if (kind == 12 || kind == 13) {
        if (!b86_pm6_imm(c, &ip, start, 1u, &imm)) return B86_UNSUPPORTED;
    }
    if (!code32) ip &= 0xFFFFu;
    switch (kind) {
    case 1: {
        int to_reg = (op & 2u) != 0u;
        uint32_t regv = b86_pm6_reg(c, ea.reg, width);
        b86_pm24_rmget(c, &ea, linear, width, &a);
        r = to_reg ? b86_pm24_alu(c, aluop, regv, a, width, &nf) : b86_pm24_alu(c, aluop, a, regv, width, &nf);
        if (aluop != 7u) {
            if (to_reg) b86_pm6_putreg(c, ea.reg, width, r);
            else b86_pm24_rmput(c, &ea, linear, width, r);
        }
        c->flags = nf; break;
    }
    case 2:
        a = b86_pm6_reg(c, B86_AX, width);
        r = b86_pm24_alu(c, aluop, a, imm, width, &nf);
        if (aluop != 7u) b86_pm6_putreg(c, B86_AX, width, r);
        c->flags = nf; break;
    case 3:
        b86_pm24_rmget(c, &ea, linear, width, &a);
        r = b86_pm24_alu(c, ea.reg, a, imm, width, &nf);
        if (ea.reg != 7u) b86_pm24_rmput(c, &ea, linear, width, r);
        c->flags = nf; break;
    case 4: case 10:
        b86_pm24_rmget(c, &ea, linear, width, &a);
        b = kind == 4 ? b86_pm6_reg(c, ea.reg, width) : imm;
        (void)b86_pm24_alu(c, 4u, a, b, width, &nf);
        c->flags = nf & ~0x10u; break;                         /* AF cleared */
    case 5:
        a = b86_pm6_reg(c, B86_AX, width);
        (void)b86_pm24_alu(c, 4u, a, imm, width, &nf);
        c->flags = nf & ~0x10u; break;
    case 6:
        b86_pm24_rmget(c, &ea, linear, width, &a);
        b = b86_pm6_reg(c, ea.reg, width);
        b86_pm24_rmput(c, &ea, linear, width, b);
        b86_pm6_putreg(c, ea.reg, width, a); break;
    case 7:
        a = b86_pm6_reg(c, B86_AX, width); b = b86_pm6_reg(c, op & 7u, width);
        b86_pm6_putreg(c, B86_AX, width, b); b86_pm6_putreg(c, op & 7u, width, a); break;
    case 8:
        b86_pm24_rmput(c, &ea, linear, 1u, (uint32_t)b86_pm24_cond(c, op2 & 15u)); break;
    case 9: {
        uint32_t al = c->r[B86_AX] & 0xFFu, f = c->flags, cf = f & 1u, af = f & 0x10u;
        if (op == 0x27u || op == 0x2Fu) {                     /* DAA / DAS */
            uint32_t old = al, ncf = 0, naf = 0;
            if ((al & 15u) > 9u || af) { al = op == 0x27u ? al + 6u : al - 6u; naf = 0x10u;
                if (op == 0x2Fu && old < 6u) ncf = 1u; else ncf |= cf; }
            if (old > 0x99u || cf) { al = op == 0x27u ? al + 0x60u : al - 0x60u; ncf = 1u; }
            al &= 0xFFu;
            c->r[B86_AX] = (c->r[B86_AX] & ~0xFFu) | al;
            c->flags = (f & ~0x8D5u) | ncf | naf | b86_pm24_szp(al, 1u) | (f & 0x800u);
        } else {                                               /* AAA / AAS */
            uint32_t ax = c->r[B86_AX] & 0xFFFFu;
            if ((al & 15u) > 9u || af) {
                ax = op == 0x37u ? ax + 0x106u : ax - 0x106u;
                f |= 0x11u;
            } else f &= ~0x11u;
            ax = (ax & 0xFF00u) | (ax & 0x0Fu);
            c->r[B86_AX] = (c->r[B86_AX] & 0xFFFF0000u) | (ax & 0xFFFFu);
            c->flags = f;
        }
        break;
    }
    case 11: {                                                 /* INC/DEC r/m: CF kept */
        uint32_t keepcf = c->flags & 1u;
        b86_pm24_rmget(c, &ea, linear, width, &a);
        r = b86_pm24_alu(c, ea.reg ? 5u : 0u, a, 1u, width, &nf);
        b86_pm24_rmput(c, &ea, linear, width, r);
        c->flags = (nf & ~1u) | keepcf; break;
    }
    case 12: {                                                 /* LOOPcc / JECXZ */
        uint32_t cnt = pref67 ^ code32 ? c->r[B86_CX] : (c->r[B86_CX] & 0xFFFFu);
        int take;
        if (op != 0xE3u) {
            cnt = (pref67 ^ code32) ? cnt - 1u : ((cnt - 1u) & 0xFFFFu);
            if (pref67 ^ code32) c->r[B86_CX] = cnt;
            else c->r[B86_CX] = (c->r[B86_CX] & 0xFFFF0000u) | cnt;
            take = cnt != 0u;
            if (op == 0xE0u) take = take && !(c->flags & 0x40u);
            if (op == 0xE1u) take = take && (c->flags & 0x40u);
        } else take = cnt == 0u;
        if (take) {
            ip += (uint32_t)(int32_t)(int8_t)imm;
            if (!(pref66 ^ code32)) ip &= 0xFFFFu;
            if (ip > c->pm386.seg[B86_CS].limit) return B86_UNSUPPORTED;
        }
        break;
    }
    case 13: {                                                 /* AAM / AAD */
        uint32_t al = c->r[B86_AX] & 0xFFu, ah = (c->r[B86_AX] >> 8) & 0xFFu;
        if (op == 0xD4u) {
            if (!imm) return B86_UNSUPPORTED;                  /* #DE via IDT: later */
            ah = al / imm; al = al % imm;
        } else { al = (al + ah * imm) & 0xFFu; ah = 0; }
        c->r[B86_AX] = (c->r[B86_AX] & 0xFFFF0000u) | (ah << 8) | al;
        c->flags = (c->flags & ~0xC4u) | (b86_pm24_szp(al, 1u) & 0xC4u);
        break;
    }
    case 14: {                                                 /* shift / rotate group */
        unsigned bits = width * 8u, cnt, sub = ea.reg;
        uint32_t mask = width == 1u ? 0xFFu : width == 2u ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t sign = 1u << (bits - 1u), f = c->flags, cf, res;
        cnt = (op == 0xD0u || op == 0xD1u) ? 1u :
              (op == 0xD2u || op == 0xD3u) ? (c->r[B86_CX] & 31u) : (imm & 31u);
        b86_pm24_rmget(c, &ea, linear, width, &a);
        a &= mask;
        if (!cnt) break;                                       /* no change at all */
        if (sub <= 1u) {                                       /* ROL / ROR */
            unsigned k = cnt % bits;
            res = k ? (sub == 0u ? ((a << k) | (a >> (bits - k))) : ((a >> k) | (a << (bits - k)))) & mask : a;
            cf = sub == 0u ? (res & 1u) : !!(res & sign);
            f = (f & ~0x801u) | cf;
            if (sub == 0u ? (!!(res & sign) ^ cf) : (!!(res & sign) ^ !!(res & (sign >> 1)))) f |= 0x800u;
        } else if (sub <= 3u) {                                /* RCL / RCR */
            unsigned k = cnt % (bits + 1u);
            uint64_t v = ((uint64_t)(f & 1u) << bits) | a;     /* bits+1 wide */
            uint64_t m1 = ((uint64_t)1 << (bits + 1u)) - 1u;
            if (k) v = sub == 2u ? ((v << k) | (v >> (bits + 1u - k))) & m1
                                  : ((v >> k) | (v << (bits + 1u - k))) & m1;
            res = (uint32_t)v & mask; cf = (uint32_t)(v >> bits) & 1u;
            f = (f & ~0x801u) | cf;
            if (sub == 2u ? (!!(res & sign) ^ cf) : (!!(res & sign) ^ !!(res & (sign >> 1)))) f |= 0x800u;
        } else {                                               /* SHL/SAL, SHR, SAR */
            if (sub == 4u || sub == 6u) {
                res = cnt < 32u ? (a << cnt) & mask : 0u;
                cf = cnt <= bits ? (a >> (bits - cnt)) & 1u : 0u;
                f = (f & ~0x8C5u) | cf | b86_pm24_szp(res, width);
                if (!!(res & sign) ^ cf) f |= 0x800u;
            } else if (sub == 5u) {
                res = cnt < bits ? a >> cnt : 0u;
                cf = cnt <= bits ? (a >> (cnt - 1u)) & 1u : 0u;
                f = (f & ~0x8C5u) | cf | b86_pm24_szp(res, width);
                if (a & sign) f |= 0x800u;
            } else {
                int32_t sv = (int32_t)(a << (32u - bits)) >> (32u - bits);
                unsigned k = cnt < bits ? cnt : bits - 1u;
                res = (uint32_t)(sv >> k) & mask;
                cf = cnt < bits ? (uint32_t)(sv >> (cnt - 1u)) & 1u : !!(a & sign);
                f = (f & ~0x8C5u) | cf | b86_pm24_szp(res, width);
            }
        }
        b86_pm24_rmput(c, &ea, linear, width, res);
        c->flags = f; break;
    }
    default: return B86_UNSUPPORTED;
    }
    c->ip = ip; ++c->icount;
    return B86_OK;
}

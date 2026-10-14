/* BB_386_RM2_FLASH_SRAM_V1: force every 386-only handler to XIP flash. */
/* BB_386_RM2_V1: 80386 extended 0F map for real mode, interpreter-only.
 * Included by interp.c after i386_rm1.inc.  No additional CPU fields and
 * no change to the 8086/186/286 JIT or executable SRAM code-cache size.
 *
 * Implemented: Jcc rel16/rel32, SETcc, MOVZX/MOVSX, IMUL r,r/m,
 * BSF/BSR, BT/BTS/BTR/BTC (register or immediate index), SHLD/SHRD.
 * Unsupported 386 encodings return B86_UNSUPPORTED with the original IP.
 * The real-mode 64-KiB segment limit is guarded, with faults pending a
 * future architectural exception/descriptor-cache implementation.
 */

static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm2_operand(St *s, int address32, B86RM1EA *ea)
{
    if (address32) return b86_rm1_ea(s, ea);
    modrm(s);
    ea->mod = s->mod;
    ea->reg = s->reg;
    ea->rm = s->rm;
    ea->off = s->ea_off;
    ea->seg = s->ea_seg;
    return s->mod == 3 ? -1 : 0;
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) unsigned b86_rm2_bits(uint32_t x, unsigned width, int reverse)
{
    unsigned n = width == 2 ? 32u : 16u;
    if (reverse) {
        while (n > 1 && !(x & (1u << (n - 1)))) --n;
        return n - 1;
    }
    n = 0;
    while (n < 31u && !(x & (1u << n))) ++n;
    return n;
}

/* A register bit-index is signed when the bit-string operand is memory;
 * the quotient uses mathematical floor (C / rounds negative toward zero).
 * An imm8 bit offset is unsigned.  In registers both forms are masked. */
static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm2_bitpos(B86RM1EA *ea, unsigned width,
                                   uint32_t index, int signed_index)
{
    unsigned bits = width == 2 ? 32u : 16u;
    unsigned bytes = width == 2 ? 4u : 2u;
    if (ea->mod == 3) return 1;
    int64_t shift;
    if (signed_index) {
        int64_t ix = width == 2 ? (int32_t)index : (int16_t)index;
        shift = ix / (int64_t)bits;
        if (ix < 0 && ix % bits) --shift;
    } else shift = (uint8_t)index / bits;
    ea->off = (uint32_t)(ea->off + shift * bytes);
    return b86_rm1_memok(ea, bytes);
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm2_bit(B86Cpu *c, const B86RM1EA *arg,
                               unsigned width, unsigned operation,
                               uint32_t index, int signed_index)
{
    B86RM1EA ea = *arg;
    unsigned n = width == 2 ? 32u : 16u;
    if (!b86_rm2_bitpos(&ea, width, index, signed_index)) return 0;
    uint32_t mask = 1u << (index & (n - 1u));
    uint32_t old = b86_rm1_rmget(c, &ea, width);
    uint32_t changed = old;
    if (operation == 1) changed |= mask;            /* BTS */
    if (operation == 2) changed &= ~mask;           /* BTR */
    if (operation == 3) changed ^= mask;            /* BTC */
    if (operation) b86_rm1_rmput(c, &ea, width, changed);
    c->flags = (c->flags & ~(uint32_t)B86_CF) | ((old & mask) ? B86_CF : 0u);
    return 1;
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm2_double_shift(B86Cpu *c, const B86RM1EA *ea,
                                         unsigned width, uint32_t source,
                                         int right, unsigned count)
{
    unsigned bits = width == 2 ? 32u : 16u;
    uint32_t mask = width == 2 ? 0xFFFFFFFFu : 0xFFFFu;
    unsigned n = count & 31u;
    if (!n) return 1;                               /* no flags or write */
    if (n > bits) return 0;                          /* undefined result */
    uint32_t old = b86_rm1_rmget(c, ea, width) & mask;
    uint32_t src = source & mask;
    uint32_t res;
    uint32_t cf;
    if (!right) {                                    /* SHLD */
        res = n == bits ? src : (old << n) | (src >> (bits - n));
        cf = (old >> (bits - n)) & 1u;
    } else {                                         /* SHRD */
        res = n == bits ? src : (old >> n) | (src << (bits - n));
        cf = (old >> (n - 1u)) & 1u;
    }
    res &= mask;
    uint32_t f = c->flags & ~(uint32_t)(B86_CF | B86_SF | B86_ZF | B86_PF);
    f |= cf ? B86_CF : 0u;
    f |= res == 0 ? B86_ZF : 0u;
    f |= res & (1u << (bits - 1u)) ? B86_SF : 0u;
    f |= b86_parity[res & 255u];
    if (n == 1) {                                    /* OF defined only at 1 */
        unsigned of = !right ? (((res >> (bits - 1u)) ^ cf) & 1u)
                             : (((old ^ res) >> (bits - 1u)) & 1u);
        f = (f & ~(uint32_t)B86_OF) | (of ? B86_OF : 0u);
    }
    /* Commit the flags before an SMC-checked store can force an exit. */
    c->flags = f;
    b86_rm1_rmput(c, ea, width, res);
    return 1;
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm2_0f(St *s, int operand32, int address32)
{
    B86Cpu *c = s->c;
    uint8_t op = f8(s);
    unsigned width = operand32 ? 2u : 1u;
    B86RM1EA ea;
    uint32_t src, dst;
    int form;

    if (op >= 0x80 && op <= 0x8F) {                 /* Jcc near rel16/32 */
        int32_t delta = operand32 ? (int32_t)b86_rm1_f32(s) : (int16_t)f16(s);
        if (cond(c, op & 15u)) {
            uint32_t target = (uint32_t)s->ip + (uint32_t)delta;
            /* Real-mode CS has a 64-KiB default limit in this milestone. */
            if (operand32 && target > 0xFFFFu) goto unsupported;
            s->ip = (uint16_t)target;
        }
        goto done;
    }

    if (op >= 0x90 && op <= 0x9F) {                 /* SETcc r/m8 */
        form = b86_rm2_operand(s, address32, &ea);
        if (!form && !b86_rm1_memok(&ea, 1)) goto unsupported;
        b86_rm1_rmput(c, &ea, 0, cond(c, op & 15u) ? 1u : 0u);
        goto done;
    }

    if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) { /* MOVZX/SX */
        unsigned sw = (op & 1) ? 1u : 0u;
        /* MOVSX/Zx must widen. r16 <- r/m16 is not an encoding. */
        if (sw == 1u && width == 1u) goto unsupported;
        form = b86_rm2_operand(s, address32, &ea);
        if (!form && !b86_rm1_memok(&ea, 1u << sw)) goto unsupported;
        src = b86_rm1_rmget(c, &ea, sw);
        if (op == 0xBE) src = (uint32_t)(int32_t)(int8_t)src;
        if (op == 0xBF) src = (uint32_t)(int32_t)(int16_t)src;
        b86_rm1_regput(c, ea.reg, width, src);
        goto done;
    }

    if (op == 0xAF) {                              /* IMUL r16/32, r/m */
        form = b86_rm2_operand(s, address32, &ea);
        if (!form && !b86_rm1_memok(&ea, 1u << width)) goto unsupported;
        src = b86_rm1_rmget(c, &ea, width);
        dst = b86_rm1_regget(c, ea.reg, width);
        int64_t prod;
        uint32_t val;
        int overflow;
        if (width == 2) {
            prod = (int64_t)(int32_t)dst * (int64_t)(int32_t)src;
            val = (uint32_t)prod;
            overflow = prod != (int64_t)(int32_t)val;
        } else {
            prod = (int64_t)(int16_t)dst * (int64_t)(int16_t)src;
            val = (uint16_t)prod;
            overflow = prod != (int64_t)(int16_t)val;
        }
        b86_rm1_regput(c, ea.reg, width, val);
        c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF)) |
                   (overflow ? B86_CF | B86_OF : 0u);
        goto done;
    }

    if (op == 0xBC || op == 0xBD) {                 /* BSF / BSR */
        form = b86_rm2_operand(s, address32, &ea);
        if (!form && !b86_rm1_memok(&ea, 1u << width)) goto unsupported;
        src = b86_rm1_rmget(c, &ea, width);
        if (!src) { c->flags |= B86_ZF; goto done; }
        c->flags &= ~(uint32_t)B86_ZF;
        b86_rm1_regput(c, ea.reg, width, b86_rm2_bits(src, width, op == 0xBD));
        goto done;
    }

    if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) {
        /* BT, BTS, BTR, BTC; 0F BA /4,/5,/6,/7 imm8 */
        form = b86_rm2_operand(s, address32, &ea);
        uint32_t index;
        unsigned action;
        int signed_index;
        if (op == 0xBA) {
            if (ea.reg < 4 || ea.reg > 7) goto unsupported;
            action = ea.reg - 4u;
            index = f8(s);
            signed_index = 0;
        } else {
            action = op == 0xA3 ? 0u : op == 0xAB ? 1u : op == 0xB3 ? 2u : 3u;
            index = b86_rm1_regget(c, ea.reg, width);
            signed_index = 1;
        }
        if (!b86_rm2_bit(c, &ea, width, action, index, signed_index)) goto unsupported;
        goto done;
    }

    if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) { /* SHLD/SHRD */
        form = b86_rm2_operand(s, address32, &ea);
        unsigned count = (op & 1) ? (c->r[B86_CX] & 255u) : f8(s);
        count &= 31u;
        if (count && form == 0 && !b86_rm1_memok(&ea, 1u << width)) goto unsupported;
        src = b86_rm1_regget(c, ea.reg, width);
        if (!b86_rm2_double_shift(c, &ea, width, src, op >= 0xAC, count)) goto unsupported;
        goto done;
    }

unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
done:
    c->ip = s->ip;
    return B86_OK;
}

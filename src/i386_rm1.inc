/* BB_386_RM1_V1 - 80386 real-mode 67h addressing.
 * Interpreter-only, flash resident (B86_COLD). No new CPU/JIT state.
 * The legacy 8086/186/286 ModR/M, JIT, and memory implementations are unchanged.
 *
 * 386 real-mode segment descriptors have 64 KiB limits unless a protected-
 * mode descriptor cache was installed (unreal mode, not implemented here).
 * A memory operand exceeding that limit returns B86_UNSUPPORTED, never wraps
 * a 32-bit EA silently to a 16-bit offset. A later exception layer must
 * replace that status with the architecturally appropriate #GP/#SS.
 */

typedef struct B86RM1EA {
    uint32_t off;
    uint8_t mod, reg, rm;
    int seg;
} B86RM1EA;

B86_COLD static uint32_t b86_rm1_f32(St *s)
{
    uint32_t lo = f16(s);
    return lo | (uint32_t)f16(s) << 16;
}

/* Returns -1 for a register form, 0 for successfully decoded memory. */
B86_COLD static int b86_rm1_ea(St *s, B86RM1EA *m)
{
    B86Cpu *c = s->c;
    uint8_t b = f8(s);
    uint32_t base = 0, index = 0, disp = 0;
    int ss = 0, hasbase = 1;
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    m->off = 0;
    m->seg = s->seg >= 0 ? s->seg : B86_DS;
    if (m->mod == 3) return -1;

    if (m->rm == 4) {                           /* SIB */
        uint8_t sib = f8(s);
        uint8_t scale = sib >> 6;
        uint8_t idx = (sib >> 3) & 7;
        uint8_t bas = sib & 7;
        if (idx != 4) index = c->r[idx] << scale; /* index=ESP means none */
        if (m->mod == 0 && bas == 5) hasbase = 0;
        else { base = c->r[bas]; ss = (bas == B86_BP || bas == B86_SP); }
    } else if (m->mod == 0 && m->rm == 5) {
        hasbase = 0;                           /* disp32 without base */
    } else {
        base = c->r[m->rm];
        ss = (m->rm == B86_BP || m->rm == B86_SP);
    }
    if (m->mod == 0 && !hasbase) disp = b86_rm1_f32(s);
    else if (m->mod == 1) disp = (uint32_t)(int32_t)(int8_t)f8(s);
    else if (m->mod == 2) disp = b86_rm1_f32(s);
    m->off = base + index + disp;              /* 32-bit wrapping EA */
    if (s->seg < 0 && ss) m->seg = B86_SS;
    return 0;
}

/* Byte-granular accesses avoid host-unaligned loads and respect page mapping,
 * A20 and SMC. All bounds are checked before the first byte is modified. */
B86_COLD static int b86_rm1_memok(const B86RM1EA *m, unsigned bytes)
{
    return m->mod != 3 && m->off <= 0xFFFFu &&
           bytes >= 1 && bytes <= 4 && m->off <= 0x10000u - bytes;
}
B86_COLD static uint32_t b86_rm1_read(B86Cpu *c, const B86RM1EA *m, unsigned bytes)
{
    uint32_t r = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        uint32_t a = ((c->seg[m->seg] << 4) + m->off + i) & c->amask;
        r |= (uint32_t)*b86_host(c, a) << (8u * i);
    }
    return r;
}
B86_COLD static void b86_rm1_write(B86Cpu *c, const B86RM1EA *m, unsigned bytes, uint32_t v)
{
    for (unsigned i = 0; i < bytes; ++i) {
        uint32_t a = ((c->seg[m->seg] << 4) + m->off + i) & c->amask;
        *b86_host(c, a) = (uint8_t)(v >> (i * 8u));
        smc(c, a, 1);
    }
}
/* Width 0/1/2 means 8/16/32. Unlike the old 16-bit setters these always
 * retain upper halves on byte/word writes, even with a redundant 66h prefix. */
B86_COLD static uint32_t b86_rm1_regget(B86Cpu *c, unsigned reg, unsigned width)
{
    if (width == 2) return c->r[reg];
    if (width == 1) return c->r[reg] & 0xFFFFu;
    return reg < 4 ? c->r[reg] & 255u : (c->r[reg - 4] >> 8) & 255u;
}
B86_COLD static void b86_rm1_regput(B86Cpu *c, unsigned reg, unsigned width, uint32_t v)
{
    if (width == 2) c->r[reg] = v;
    else if (width == 1) c->r[reg] = (c->r[reg] & 0xFFFF0000u) | (v & 0xFFFFu);
    else if (reg < 4) c->r[reg] = (c->r[reg] & 0xFFFFFF00u) | (v & 255u);
    else c->r[reg - 4] = (c->r[reg - 4] & 0xFFFF00FFu) | ((v & 255u) << 8);
}
B86_COLD static uint32_t b86_rm1_rmget(B86Cpu *c, const B86RM1EA *m, unsigned width)
{
    return m->mod == 3 ? b86_rm1_regget(c, m->rm, width)
                       : b86_rm1_read(c, m, 1u << width);
}
B86_COLD static void b86_rm1_rmput(B86Cpu *c, const B86RM1EA *m, unsigned width, uint32_t v)
{
    if (m->mod == 3) b86_rm1_regput(c, m->rm, width, v);
    else b86_rm1_write(c, m, 1u << width, v);
}
/* Use the existing 8086 flag ALU for byte/word, and RM0 32-bit flag ALU
 * for DWORD. Neither allocates new state or changes the 286/JIT path. */
B86_COLD static uint32_t b86_rm1_alu(B86Cpu *c, unsigned width,
                                      unsigned kind, uint32_t a, uint32_t b)
{
    return width == 2 ? b86_rm0_alu(c, kind, a, b)
                      : alu(c, kind, width == 1, a, b);
}

/* 67h changes only effective address calculation and moffs width, not the
 * default 16-bit operand size of real mode. It has no JIT lowering yet. */
B86_COLD static int b86_rm1_67(St *s, uint8_t op, int operand32)
{
    B86Cpu *c = s->c;
    B86RM1EA m;
    unsigned width, kind;
    uint32_t a, b, r;
    int reg_form;

    /* Instructions that do not use an effective address can delegate to RM0
     * when 66 is active. That helper already handles 32-bit immediate MOV,
     * register INC/DEC, stack ops and CWDE/CDQ. */
    if (operand32 && ((op >= 0xB8 && op <= 0xBF) ||
                      (op >= 0x40 && op <= 0x5F) ||
                      op == 0x68 || op == 0x6A || op == 0x98 || op == 0x99 || op == 0x90))
        return b86_rm0_66(s, op);
    if (op >= 0xB8 && op <= 0xBF) {
        b86_rm1_regput(c, op & 7u, 1, f16(s));
        goto done;
    }
    if (op == 0x90) goto done;
    /* 386 MOFFS carries a 32-bit address with 67, even for AL/AX. */
    if (op >= 0xA0 && op <= 0xA3) {
        m.mod = 0; m.seg = s->seg >= 0 ? s->seg : B86_DS;
        m.off = b86_rm1_f32(s);
        width = (op & 1) ? (operand32 ? 2u : 1u) : 0u;
        if (!b86_rm1_memok(&m, 1u << width)) goto unsupported;
        if (op & 2) b86_rm1_write(c, &m, 1u << width, b86_rm1_regget(c, B86_AX, width));
        else b86_rm1_regput(c, B86_AX, width, b86_rm1_read(c, &m, 1u << width));
        goto done;
    }
    /* Arithmetic immediate accumulator form, address override is a NOP. */
    if (op < 0x40 && (op & 7u) >= 4 && (op & 7u) <= 5) {
        width = (op & 1) ? (operand32 ? 2u : 1u) : 0u;
        kind = op >> 3;
        a = b86_rm1_regget(c, B86_AX, width);
        b = width == 2 ? b86_rm1_f32(s) : width == 1 ? f16(s) : f8(s);
        r = b86_rm1_alu(c, width, kind, a, b);
        if (kind != 7) b86_rm1_regput(c, B86_AX, width, r);
        goto done;
    }
    if ((op < 0x40 && (op & 7u) <= 3u) ||
        op == 0x80 || op == 0x81 || op == 0x83 ||
        (op >= 0x84 && op <= 0x8B) || op == 0x8D ||
        op == 0xC6 || op == 0xC7) {
        reg_form = b86_rm1_ea(s, &m);
        if (op == 0x8D) {                        /* LEA: no memory access/check */
            if (reg_form) goto unsupported;
            b86_rm1_regput(c, m.reg, operand32 ? 2u : 1u, m.off);
            goto done;
        }
        width = (op & 1) ? (operand32 ? 2u : 1u) : 0u;
        if (!reg_form && !b86_rm1_memok(&m, 1u << width)) goto unsupported;
        if (op >= 0x88 && op <= 0x8B) {          /* MOV */
            if (op & 2) b86_rm1_regput(c, m.reg, width, b86_rm1_rmget(c, &m, width));
            else b86_rm1_rmput(c, &m, width, b86_rm1_regget(c, m.reg, width));
            goto done;
        }
        if (op == 0xC6 || op == 0xC7) {
            if (m.reg) goto unsupported;
            b = width == 2 ? b86_rm1_f32(s) : width == 1 ? f16(s) : f8(s);
            b86_rm1_rmput(c, &m, width, b);
            goto done;
        }
        if (op == 0x86 || op == 0x87) {           /* XCHG */
            a = b86_rm1_rmget(c, &m, width);
            b = b86_rm1_regget(c, m.reg, width);
            b86_rm1_rmput(c, &m, width, b);
            b86_rm1_regput(c, m.reg, width, a);
            goto done;
        }
        if (op == 0x84 || op == 0x85) {           /* TEST */
            a = b86_rm1_rmget(c, &m, width);
            b = b86_rm1_regget(c, m.reg, width);
            (void)b86_rm1_alu(c, width, 4, a, b);
            goto done;
        }
        if (op < 0x40) {
            kind = op >> 3;
            if (op & 2) {
                a = b86_rm1_regget(c, m.reg, width); b = b86_rm1_rmget(c, &m, width);
                r = b86_rm1_alu(c, width, kind, a, b);
                if (kind != 7) b86_rm1_regput(c, m.reg, width, r);
            } else {
                a = b86_rm1_rmget(c, &m, width); b = b86_rm1_regget(c, m.reg, width);
                r = b86_rm1_alu(c, width, kind, a, b);
                if (kind != 7) b86_rm1_rmput(c, &m, width, r);
            }
            goto done;
        }
        /* Grp 1: 80=imm8, 81=imm16/32, 83=sign-extended imm8. */
        kind = m.reg;
        a = b86_rm1_rmget(c, &m, width);
        if (op == 0x83) b = (uint32_t)(int32_t)(int8_t)f8(s);
        else b = width == 2 ? b86_rm1_f32(s) : width == 1 ? f16(s) : f8(s);
        r = b86_rm1_alu(c, width, kind, a, b);
        if (kind != 7) b86_rm1_rmput(c, &m, width, r);
        goto done;
    }
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
done:
    c->ip = s->ip;
    return B86_OK;
}

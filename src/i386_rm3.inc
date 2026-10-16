/* BB_386_RM3_V1 — 80386 real-mode segment and control-flow expansion.
 * This file is included AFTER i386_rm0.inc, i386_rm1.inc, i386_rm2.inc.
 * All functions are in XIP FLASH. Existing Thumb-2 / JIT paths unchanged.
 * NOTE: 386 real-mode CS/SS have 64KiB limits here. Protected mode, cached
 * descriptors, 32-bit stack-address attributes, and full EFLAGS come later.
 */
#ifndef BB386RM3_FLASH
#define BB386RM3_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif

/* Existing legacy segment registers keep their original layout/offsets. */
static BB386RM3_FLASH uint32_t b86_rm3_segvalue(B86Cpu *c, int which)
{
    if (which == B86_FS || which == B86_GS) return c->seg386[which - B86_FS];
    return (which >= B86_ES && which <= B86_DS) ? c->seg[which] : 0;
}

static BB386RM3_FLASH void b86_rm3_set_extra(B86Cpu *c, int which, uint16_t selector)
{
    c->seg386[which - B86_FS] = selector;
}

/* 16-bit stack address size (real mode) + 32-bit operand size: updates SP,
 * preserving high ESP. Return 0 on a cross-limit memory access. */
static BB386RM3_FLASH int b86_rm3_stack_read(B86Cpu *c, unsigned off, uint32_t *v)
{
    B86RM1EA m;
    memset(&m, 0, sizeof(m));
    m.seg = B86_SS;
    m.off = off;
    if (!b86_rm1_memok(&m, 4u)) return 0;
    *v = b86_rm1_read(c, &m, 4u);
    return 1;
}

static BB386RM3_FLASH int b86_rm3_stack_push(B86Cpu *c, uint32_t value)
{
    B86RM1EA m;
    memset(&m, 0, sizeof(m));
    m.seg = B86_SS;
    m.off = (uint16_t)(c->r[B86_SP] - 4u);
    if (!b86_rm1_memok(&m, 4u)) return 0;
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | m.off;
    b86_rm1_write(c, &m, 4u, value);
    return 1;
}

static BB386RM3_FLASH int b86_rm3_stack_pop(B86Cpu *c, uint32_t *value)
{
    uint16_t sp = (uint16_t)c->r[B86_SP];
    if (!b86_rm3_stack_read(c, sp, value)) return 0;
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 4u);
    return 1;
}

/* Near control flow with 32-bit operands. Explicitly reject a destination
 * past real-mode CS.limit before changing stack/IP. Return unsupported for
 * currently-unimplemented instruction forms, not a made-up #UD. */
static BB386RM3_FLASH int b86_rm3_near32(St *s, uint8_t op, int address32)
{
    B86Cpu *c = s->c;
    uint32_t target = 0, v = 0;
    B86RM1EA ea;
    if (op == 0xE8 || op == 0xE9) {                 /* CALL/JMP rel32 */
        int32_t disp = (int32_t)b86_rm1_f32(s);
        target = (uint32_t)s->ip + (uint32_t)disp;
        if (target > 0xFFFFu) goto unsupported;
        if (op == 0xE8 && !b86_rm3_stack_push(c, s->ip)) goto unsupported;
    } else if (op == 0xC3 || op == 0xC2) {         /* RET/RET imm16, pop EIP32 */
        uint16_t imm = op == 0xC2 ? f16(s) : 0;
        if (!b86_rm3_stack_read(c, (uint16_t)c->r[B86_SP], &target)) goto unsupported;
        if (target > 0xFFFFu) goto unsupported;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) |
                       (uint16_t)(c->r[B86_SP] + 4u + imm);
    } else if (op == 0xFF) {                      /* CALL/JMP/PUSH r/m32 */
        if (address32) (void)b86_rm1_ea(s, &ea);
        else {
            modrm(s);
            ea.mod = s->mod; ea.rm = s->rm; ea.reg = s->reg;
            ea.off = s->ea_off; ea.seg = s->ea_seg;
        }
        if (ea.reg != 2 && ea.reg != 4 && ea.reg != 6) goto unsupported;
        if (ea.mod != 3 && !b86_rm1_memok(&ea, 4u)) goto unsupported;
        v = b86_rm1_rmget(c, &ea, 2);
        if (ea.reg == 6) {
            if (!b86_rm3_stack_push(c, v)) goto unsupported;
            c->ip = s->ip;
            return B86_OK;
        }
        target = v;
        if (target > 0xFFFFu) goto unsupported;
        if (ea.reg == 2 && !b86_rm3_stack_push(c, s->ip)) goto unsupported;
    } else goto unsupported;
    c->ip = (uint16_t)target;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm3_stack32(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    uint32_t v;
    uint16_t sp = (uint16_t)c->r[B86_SP];
    if (op == 0x60) {                              /* PUSHAD */
        uint32_t save[8];
        for (unsigned i = 0; i < 8; ++i) save[i] = c->r[i];
        if (sp < 32u || sp > 0xFFFCu) goto unsupported;
        static const uint8_t order[8] = {B86_AX,B86_CX,B86_DX,B86_BX,B86_SP,B86_BP,B86_SI,B86_DI};
        for (unsigned i = 0; i < 8; ++i) {
            if (!b86_rm3_stack_push(c, save[order[i]])) goto unsupported;
        }
    } else if (op == 0x61) {                       /* POPAD */
        if (sp > 0x10000u - 32u) goto unsupported;
        static const uint8_t order[8] = {B86_DI,B86_SI,B86_BP,B86_SP,B86_BX,B86_DX,B86_CX,B86_AX};
        uint32_t save[8];
        for (unsigned i = 0; i < 8; ++i) {
            if (!b86_rm3_stack_read(c, sp + i * 4u, &save[i])) goto unsupported;
        }
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 32u);
        for (unsigned i = 0; i < 8; ++i) if (order[i] != B86_SP) c->r[order[i]] = save[i];
    } else if (op == 0xC9) {                       /* LEAVE with 32-bit operand */
        /* SS has a 16-bit stack-address attribute in 386 real mode. */
        if (!b86_rm3_stack_read(c, (uint16_t)c->r[B86_BP], &v)) goto unsupported;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(c->r[B86_BP] + 4u);
        c->r[B86_BP] = v;
    } else if (op == 0x9C) {                       /* PUSHFD (subset: 16 flag bits) */
        v = b86_get_flags(c);
        if (!b86_rm3_stack_push(c, v)) goto unsupported;
    } else if (op == 0x9D) {                       /* POPFD (subset: 16 flag bits) */
        if (!b86_rm3_stack_pop(c, &v)) goto unsupported;
        b86_set_flags(c, (uint16_t)v);
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* 386 segment MOV: 8C/8E encode ES CS SS DS FS GS as ModRM.reg 0..5.
 * We do not change the 286 segment array or native emitter's offsets. */
static BB386RM3_FLASH int b86_rm3_mov_seg(St *s, uint8_t op,
                                           int operand32, int address32)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    if (address32) (void)b86_rm1_ea(s, &ea);
    else {
        modrm(s);
        ea.mod = s->mod; ea.reg = s->reg; ea.rm = s->rm;
        ea.off = s->ea_off; ea.seg = s->ea_seg;
    }
    if (ea.reg > 5 || (op == 0x8E && ea.reg == B86_CS)) goto unsupported;
    if (ea.mod != 3 && !b86_rm1_memok(&ea, 2u)) goto unsupported;
    if (op == 0x8E) {                               /* MOV Sreg, r/m16 */
        uint16_t selector = (uint16_t)b86_rm1_rmget(c, &ea, 1u);
        if (ea.reg >= B86_FS) b86_rm3_set_extra(c, ea.reg, selector);
        else b86_set_seg(c, ea.reg, selector);
    } else {                                      /* MOV r/m16, Sreg */
        uint16_t selector = (uint16_t)b86_rm3_segvalue(c, ea.reg);
        /* Explicitly handle the r32 destination; memory stays a word. */
        if (ea.mod == 3 && operand32) b86_rm1_regput(c, ea.rm, 2u, selector);
        else b86_rm1_rmput(c, &ea, 1u, selector);
    }
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* LFS/LGS load a far pointer: m16:16 or m16:32 (operand-size=32).
 * Segment-cache descriptor semantics are not implemented in real mode. */
static BB386RM3_FLASH int b86_rm3_load_far(St *s, uint8_t opcode,
                                            int operand32, int address32)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    if (address32) (void)b86_rm1_ea(s, &ea);
    else {
        modrm(s);
        ea.mod = s->mod; ea.reg = s->reg; ea.rm = s->rm;
        ea.off = s->ea_off; ea.seg = s->ea_seg;
    }
    unsigned offset_bytes = operand32 ? 4u : 2u;
    unsigned total = offset_bytes + 2u;
    if (ea.mod == 3 || ea.off > 0x10000u - total) goto unsupported;
    uint32_t value = b86_rm1_read(c, &ea, offset_bytes);
    ea.off += offset_bytes;
    uint16_t selector = (uint16_t)b86_rm1_read(c, &ea, 2u);
    b86_rm1_regput(c, ea.reg, operand32 ? 2u : 1u, value);
    b86_rm3_set_extra(c, opcode == 0xB4 ? B86_FS : B86_GS, selector);
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm3_pushpop_seg(St *s, uint8_t subop, int operand32)
{
    B86Cpu *c = s->c;
    int index = subop == 0xA0 || subop == 0xA1 ? B86_FS : B86_GS;
    if ((subop & 1u) == 0u) {                       /* PUSH FS/GS */
        uint32_t v = b86_rm3_segvalue(c, index);
        if (operand32) {
            if (!b86_rm3_stack_push(c, v)) goto unsupported;
        } else push(c, (uint16_t)v);
    } else {                                       /* POP FS/GS */
        uint32_t v;
        if (operand32) {
            if (!b86_rm3_stack_pop(c, &v)) goto unsupported;
        } else v = pop(c);
        b86_rm3_set_extra(c, index, (uint16_t)v);
    }
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* FS/GS with 16-bit addressing. 67h forms are already handled by RM1/RM2;
 * this deliberately covers only validated real-mode 16-bit-EA data forms. */
static BB386RM3_FLASH int b86_rm3_fs16(St *s, uint8_t op, int operand32)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    unsigned width = (op & 1u) ? (operand32 ? 2u : 1u) : 0u;
    if (op >= 0xA0 && op <= 0xA3) {                /* MOV accumulator, moffs */
        ea.mod = 0;
        ea.seg = s->seg;
        ea.off = f16(s);
        if (!b86_rm1_memok(&ea, 1u << width)) goto unsupported;
        if (op & 2u) b86_rm1_write(c, &ea, 1u << width, b86_rm1_regget(c, B86_AX, width));
        else b86_rm1_regput(c, B86_AX, width, b86_rm1_read(c, &ea, 1u << width));
    } else if (op < 0x40 && (op & 7u) >= 4u && (op & 7u) <= 5u) {
        /* Segment prefix has no effect on accumulator-immediate ALU. */
        unsigned accwidth = (op & 1u) ? (operand32 ? 2u : 1u) : 0u;
        unsigned kind = op >> 3;
        uint32_t a = b86_rm1_regget(c, B86_AX, accwidth);
        uint32_t b = accwidth == 2u ? b86_rm1_f32(s) :
                     accwidth == 1u ? f16(s) : f8(s);
        uint32_t result = b86_rm1_alu(c, accwidth, kind, a, b);
        if (kind != 7u) b86_rm1_regput(c, B86_AX, accwidth, result);
    } else if ((op < 0x40 && (op & 7u) <= 3u) ||
               op == 0x80 || op == 0x81 || op == 0x83 ||
               (op >= 0x88 && op <= 0x8B) || op == 0xC6 || op == 0xC7 ||
               op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87 ||
               op == 0x8D) {
        modrm(s);
        ea.mod = s->mod; ea.reg = s->reg; ea.rm = s->rm;
        ea.off = s->ea_off; ea.seg = s->ea_seg;
        if (op == 0x8D) {  /* LEA: compute offset only; override ignored */
            if (ea.mod == 3) goto unsupported;
            b86_rm1_regput(c, ea.reg, operand32 ? 2u : 1u, ea.off);
            c->ip = s->ip;
            return B86_OK;
        }
        if (ea.mod != 3 && !b86_rm1_memok(&ea, 1u << width)) goto unsupported;
        if ((op < 0x40 && (op & 7u) <= 3u) ||
            op == 0x80 || op == 0x81 || op == 0x83) {
            /* 386 FS/GS with 16-bit effective addressing. The segment
               override matters only for memory operands. Reuse the existing
               8/16/32 ALU so ADC, SBB and all architecturally defined flags
               follow the same path as the validated non-overridden cases. */
            unsigned kind = op < 0x40 ? (op >> 3) : ea.reg;
            uint32_t a, b, result;
            if (op == 0x80 || op == 0x81 || op == 0x83) {
                a = b86_rm1_rmget(c, &ea, width);
                b = op == 0x83 ? (uint32_t)(int32_t)(int8_t)f8(s)
                    : op == 0x80 ? (uint32_t)f8(s)
                    : width == 2u ? b86_rm1_f32(s) : f16(s);
                /* 83 /r imm8 sign extends to the OPERAND width. When
                   the destination is 16-bit, trim 0xFFFFFFxx to 0xFFxx
                   before calling the legacy 16-bit ALU (CF depends on it). */
                if (op == 0x83 && width == 1u) b &= 0xFFFFu;
                result = b86_rm1_alu(c, width, kind, a, b);
                if (kind != 7u) b86_rm1_rmput(c, &ea, width, result);
            } else if (op & 2u) { /* ALU reg, r/m */
                a = b86_rm1_regget(c, ea.reg, width);
                b = b86_rm1_rmget(c, &ea, width);
                result = b86_rm1_alu(c, width, kind, a, b);
                if (kind != 7u) b86_rm1_regput(c, ea.reg, width, result);
            } else {             /* ALU r/m, reg */
                a = b86_rm1_rmget(c, &ea, width);
                b = b86_rm1_regget(c, ea.reg, width);
                result = b86_rm1_alu(c, width, kind, a, b);
                if (kind != 7u) b86_rm1_rmput(c, &ea, width, result);
            }
        } else if (op == 0x86 || op == 0x87) {  /* XCHG reg, r/m */
            uint32_t a = b86_rm1_rmget(c, &ea, width);
            uint32_t b = b86_rm1_regget(c, ea.reg, width);
            b86_rm1_rmput(c, &ea, width, b);
            b86_rm1_regput(c, ea.reg, width, a);
        } else if (op >= 0x88 && op <= 0x8B) {
            if (op & 2u) b86_rm1_regput(c, ea.reg, width, b86_rm1_rmget(c, &ea, width));
            else b86_rm1_rmput(c, &ea, width, b86_rm1_regget(c, ea.reg, width));
        } else if (op == 0xC6 || op == 0xC7) {
            if (ea.reg != 0) goto unsupported;
            uint32_t v = width == 2u ? b86_rm1_f32(s) : width == 1u ? f16(s) : f8(s);
            b86_rm1_rmput(c, &ea, width, v);
        } else {
            (void)b86_rm1_alu(c, width, 4u,
                              b86_rm1_rmget(c, &ea, width), b86_rm1_regget(c, ea.reg, width));
        }
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* Called ONLY in model 386 before the previous RM2/RM1/RM0 diversions.
 * -1 means no RM3 interception needed; >=0 means completed/status code. */
/* BB_386_SHIFT_V2: 386 real-mode group-2 shifter.  Only the 386 model
 * dispatches here; the existing 8086/186/286 execution and JIT are unchanged.
 * Width is 0/1/2 for 8/16/32.  Undefined shift flags are preserved rather
 * than assigned fabricated values; the SST runner remains responsible for
 * the hardware corpus' per-vector compare masks.
 */
static BB386RM3_FLASH uint32_t b86_rm3_shift_value(B86Cpu *c, unsigned kind,
                                                      unsigned width, uint32_t value,
                                                      unsigned n)
{
    unsigned bits = width == 2u ? 32u : width == 1u ? 16u : 8u;
    uint32_t mask = width == 2u ? 0xFFFFFFFFu : (1u << bits) - 1u;
    uint32_t msb = 1u << (bits - 1u);
    uint32_t v = value & mask;
    if (!n) return v;
    uint32_t cf = (c->flags & B86_CF) ? 1u : 0u;
    uint32_t of = 0u;
    uint32_t old_msb = (v & msb) ? 1u : 0u;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t lost;
        switch (kind) {
        case 0: /* ROL */
            lost = (v & msb) ? 1u : 0u;
            v = ((v << 1u) | lost) & mask; cf = lost;
            break;
        case 1: /* ROR */
            lost = v & 1u;
            v = (v >> 1u) | (lost ? msb : 0u); cf = lost;
            break;
        case 2: /* RCL */
            lost = (v & msb) ? 1u : 0u;
            v = ((v << 1u) | cf) & mask; cf = lost;
            break;
        case 3: /* RCR */
            lost = v & 1u;
            v = (v >> 1u) | (cf ? msb : 0u); cf = lost;
            break;
        case 4: case 6: /* SHL/SAL; /6 386 aliases SHL */
            cf = (v & msb) ? 1u : 0u;
            v = (v << 1u) & mask;
            break;
        case 5: /* SHR */
            cf = v & 1u;
            v >>= 1u;
            break;
        default: /* SAR */
            cf = v & 1u;
            v = (v >> 1u) | (v & msb);
            break;
        }
    }
    /* 386EX silicon records a deterministic OF for multi-bit counts too,
       although the x86 architecture leaves OF undefined except count 1.
       Keep this behavior 386-only and verify against the 386EX corpus. */
    {
        switch (kind) {
        case 0: case 2: case 4: case 6:
            of = ((v & msb) ? 1u : 0u) ^ cf; break;
        case 1: case 3:
            of = ((v & msb) ? 1u : 0u) ^ ((v & (msb >> 1u)) ? 1u : 0u);
            break;
        case 5: of = old_msb; break;
        default: of = 0u; break;
        }
    }
    uint32_t f = c->flags;
    f = (f & ~(uint32_t)B86_CF) | (cf ? B86_CF : 0u);
    if (kind >= 4u) {
        f &= ~(uint32_t)(B86_SF | B86_ZF | B86_PF);
        if (v & msb) f |= B86_SF;
        if (!v) f |= B86_ZF;
        f |= b86_parity[v & 255u];
    }
    /* The 386EX observed multi-count OF value is used here (other CPUs
       need not agree). AF remains undefined and is preserved. */
    f = (f & ~(uint32_t)B86_OF) | (of ? B86_OF : 0u);
    c->flags = f;
    return v;
}

static BB386RM3_FLASH int b86_rm3_shift386(St *s, uint8_t op,
                                             int operand32, int address32)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    if (address32) (void)b86_rm1_ea(s, &ea);
    else {
        modrm(s);
        ea.mod = s->mod; ea.reg = s->reg; ea.rm = s->rm;
        ea.off = s->ea_off; ea.seg = s->ea_seg;
    }
    unsigned width = (op & 1u) ? (operand32 ? 2u : 1u) : 0u;
    unsigned n = (op == 0xD0 || op == 0xD1) ? 1u :
                 (op == 0xD2 || op == 0xD3) ? (c->r[B86_CX] & 255u) : f8(s);
    n &= 31u; /* 80186+ architecturally masks the raw shift count */
    if (ea.mod != 3 && !b86_rm1_memok(&ea, 1u << width)) goto unsupported;
    if (n) {
        uint32_t a = b86_rm1_rmget(c, &ea, width);
        uint32_t r = b86_rm3_shift_value(c, ea.reg, width, a, n);
        b86_rm1_rmput(c, &ea, width, r);
    }
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm3_dispatch(St *s, uint8_t op,
                                             int operand32, int address32)
{
    B86Cpu *c = s->c;
    if ((op >= 0xD0 && op <= 0xD3) || op == 0xC0 || op == 0xC1)
        return b86_rm3_shift386(s, op, operand32, address32);
    if (op == 0x8C || op == 0x8E)
        return b86_rm3_mov_seg(s, op, operand32, address32);
    if (op == 0x0F) {
        uint8_t op2 = rd8(c, B86_CS, s->ip);
        /* RM2 does not own 0F 01; legacy step0f uses a 4-segment array. */
        if (s->seg >= B86_FS && op2 == 0x01) {
            c->ip = s->fip0; return B86_UNSUPPORTED;
        }
        if (op2 == 0xA0 || op2 == 0xA1 || op2 == 0xA8 || op2 == 0xA9) {
            (void)f8(s);
            return b86_rm3_pushpop_seg(s, op2, operand32);
        }
        if (op2 == 0xB4 || op2 == 0xB5) {
            (void)f8(s);
            return b86_rm3_load_far(s, op2, operand32, address32);
        }
    }
    if (operand32) {
        if (op == 0xE8 || op == 0xE9 || op == 0xC2 || op == 0xC3 || op == 0xFF)
            return b86_rm3_near32(s, op, address32);
        if (op == 0x60 || op == 0x61 || op == 0x9C || op == 0x9D || op == 0xC9)
            return b86_rm3_stack32(s, op);
    }
    if (s->seg >= B86_FS && !address32 && op != 0x0F)
        return b86_rm3_fs16(s, op, operand32);
    return -1;
}

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
/* BB_386_STAGE5C4A_V1: explicit persistent 386 real-mode shutdown.
 * Only called when the target exception/interrupt cannot enter atomically.
 * Real-address mode has no recoverable handler when vector 8 also fails.
 * A host must reset the CPU with b86_init() to leave shutdown. */
static BB386RM3_FLASH int b86_rm5c4a_shutdown(St *s)
{
    s->c->shutdown386 = 1u;
    s->c->ip = s->fip0;
    return B86_SHUTDOWN;
}

/* BB_386_STAGE5C3A_V1: IDTR-limit vector 8 and FLAGS stack #SS. */
/* BB_386_STAGE5C2_V1: bounded real-mode vector 12/13 fault entry.
 * Only preflighted 32-bit operand/address forms invoke this helper.
 * Real-address mode pushes WORD FLAGS, CS, faulting IP. No error code.
 * Do not invoke the software INT hook for processor-originated exceptions.
 * If the IDT entry or exception frame is itself inaccessible, the CPU's
 * current shutdown/double-fault state cannot be represented: fail closed.
 */
uint32_t b86_limit_faults;   /* r55: 386 real-mode #SS/#GP segment-limit faults raised (tests) */
static BB386RM3_FLASH int b86_rm5c2_fault(St *s, unsigned vector)
{
    B86Cpu *c = s->c;
    if (vector == 12u || vector == 13u) b86_limit_faults++;
    uint32_t slot = vector * 4u;
    uint64_t phys = (uint64_t)c->idt_base + slot;
    const uint16_t sp = (uint16_t)c->r[B86_SP];
    const uint16_t a = (uint16_t)(sp - 2u);
    const uint16_t b = (uint16_t)(sp - 4u);
    const uint16_t d = (uint16_t)(sp - 6u);
    uint16_t ip, cs, flags;
    /* Stage5C4D: vector 0 #DE and vector 5 #BR also use this bounded entry. */
    if (vector != 0u && vector != 5u && vector != 6u &&
        vector != 8u && vector != 12u && vector != 13u) goto unsupported;
    /* 80386 real-address mode: an IVT entry beyond IDTR.limit raises
     * interrupt 8 (NOT the protected-mode double-fault frame). */
    if (slot + 3u > c->idt_limit) {
        if (vector == 8u) goto unsupported;  /* cannot vector #8 itself */
        vector = 8u;
        slot = 8u * 4u;
        phys = (uint64_t)c->idt_base + slot;
    }
    if (slot + 3u > c->idt_limit || phys + 3u >= B86_MEM_BYTES ||
        a > 0xFFFEu || b > 0xFFFEu || d > 0xFFFEu)
        goto unsupported;
    ip = (uint16_t)(*b86_host(c, (uint32_t)phys & c->amask) |
          ((uint16_t)*b86_host(c, (uint32_t)(phys + 1u) & c->amask) << 8));
    cs = (uint16_t)(*b86_host(c, (uint32_t)(phys + 2u) & c->amask) |
          ((uint16_t)*b86_host(c, (uint32_t)(phys + 3u) & c->amask) << 8));
    flags = b86_get_flags(c);
    /* All three writes preflighted; SP wraps at each 16-bit push. */
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | a;
    wr16(c, B86_SS, a, flags);
    c->flags &= ~(uint32_t)(B86_IF | B86_TF);
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | b;
    wr16(c, B86_SS, b, (uint16_t)c->seg[B86_CS]);
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | d;
    wr16(c, B86_SS, d, s->fip0); /* restart at FIRST prefix */
    b86_set_seg(c, B86_CS, cs);
    c->ip = ip;
    return B86_OK;
unsupported:
    return b86_rm5c4a_shutdown(s);
}

static BB386RM3_FLASH int b86_rm3_near32(St *s, uint8_t op, int address32)
{
    B86Cpu *c = s->c;
    uint32_t target = 0, v = 0;
    B86RM1EA ea;
    if (op == 0xE8 || op == 0xE9) {                 /* CALL/JMP rel32 */
        int32_t disp = (int32_t)b86_rm1_f32(s);
        target = (uint32_t)s->ip + (uint32_t)disp;
        if (target > 0xFFFFu) goto gp;
        if (op == 0xE8 && !b86_rm3_stack_push(c, s->ip)) goto ss;
    } else if (op == 0xC3 || op == 0xC2) {         /* RET/RET imm16, pop EIP32 */
        uint16_t imm = op == 0xC2 ? f16(s) : 0;
        if (!b86_rm3_stack_read(c, (uint16_t)c->r[B86_SP], &target)) goto ss;
        if (target > 0xFFFFu) goto gp;
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
        if (ea.mod != 3 && !b86_rm1_memok(&ea, 4u))
            return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
        v = b86_rm1_rmget(c, &ea, 2);
        if (ea.reg == 6) {
            if (!b86_rm3_stack_push(c, v)) goto ss;
            c->ip = s->ip;
            return B86_OK;
        }
        target = v;
        if (target > 0xFFFFu) goto gp;
        if (ea.reg == 2 && !b86_rm3_stack_push(c, s->ip)) goto ss;
    } else goto unsupported;
    c->ip = (uint16_t)target;
    return B86_OK;
gp:
    return b86_rm5c2_fault(s, 13u);
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* BB_386_STAGE5C4B_V1: one dword must fit inside the 64-KiB SS limit.
 * The ADDRESS of each later PUSHAD/POPAD slot wraps at 16 bits; the four
 * bytes of any individual operand may not cross the segment limit. */
static BB386RM3_FLASH int b86_rm5c4b_dword_slot(uint16_t off)
{
    return off <= 0xFFFCu;
}

static BB386RM3_FLASH int b86_rm3_stack32(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    uint32_t v;
    uint16_t sp = (uint16_t)c->r[B86_SP];
    if (op == 0x60) {                              /* PUSHAD */
        uint32_t save[8];
        for (unsigned i = 0; i < 8; ++i) save[i] = c->r[i];
        /* Preflight ALL eight operand slots before the first write. SP
         * wraps between DWORD pushes, but an individual DWORD cannot. */
        for (unsigned i = 0; i < 8u; ++i)
            if (!b86_rm5c4b_dword_slot((uint16_t)(sp - (i + 1u) * 4u))) goto ss;
        static const uint8_t order[8] = {B86_AX,B86_CX,B86_DX,B86_BX,B86_SP,B86_BP,B86_SI,B86_DI};
        for (unsigned i = 0; i < 8; ++i) {
            if (!b86_rm3_stack_push(c, save[order[i]])) goto ss;
        }
    } else if (op == 0x61) {                       /* POPAD */
        /* POPAD may wrap SP between valid DWORD reads. */
        for (unsigned i = 0; i < 8u; ++i)
            if (!b86_rm5c4b_dword_slot((uint16_t)(sp + i * 4u))) goto ss;
        static const uint8_t order[8] = {B86_DI,B86_SI,B86_BP,B86_SP,B86_BX,B86_DX,B86_CX,B86_AX};
        uint32_t save[8];
        for (unsigned i = 0; i < 8; ++i) {
            if (!b86_rm3_stack_read(c, (uint16_t)(sp + i * 4u), &save[i])) goto ss;
        }
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 32u);
        for (unsigned i = 0; i < 8; ++i) if (order[i] != B86_SP) c->r[order[i]] = save[i];
    } else if (op == 0xC9) {                       /* LEAVE with 32-bit operand */
        /* SS has a 16-bit stack-address attribute in 386 real mode. */
        if (!b86_rm3_stack_read(c, (uint16_t)c->r[B86_BP], &v)) goto ss;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(c->r[B86_BP] + 4u);
        c->r[B86_BP] = v;
    } else if (op == 0x9C) {                       /* PUSHFD (subset: 16 flag bits) */
        v = b86_get_flags(c);
        if (!b86_rm3_stack_push(c, v)) goto ss;
    } else if (op == 0x9D) {                       /* POPFD (subset: 16 flag bits) */
        if (!b86_rm3_stack_pop(c, &v)) goto ss;
        b86_set_flags(c, (uint16_t)v);
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
ss:
    return b86_rm5c2_fault(s, 12u);
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
    if (ea.mod != 3 && !b86_rm1_memok(&ea, 2u))
        return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
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
    if (ea.mod == 3) goto unsupported;
    if (ea.off > 0x10000u - total)
        return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
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
            if (!b86_rm3_stack_push(c, v)) return b86_rm5c2_fault(s, 12u);
        } else {
            uint16_t dest = (uint16_t)(c->r[B86_SP] - 2u);
            if (dest > 0xFFFEu) return b86_rm5c2_fault(s, 12u);
            push(c, (uint16_t)v);
        }
    } else {                                       /* POP FS/GS */
        uint32_t v;
        if (operand32) {
            if (!b86_rm3_stack_pop(c, &v)) return b86_rm5c2_fault(s, 12u);
        } else {
            if ((uint16_t)c->r[B86_SP] > 0xFFFEu)
                return b86_rm5c2_fault(s, 12u);
            v = pop(c);
        }
        b86_rm3_set_extra(c, index, (uint16_t)v);
    }
    c->ip = s->ip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* BB_386_STAGE5C3B_V1: preflighted real-mode data memory faults. */
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
        if (!b86_rm1_memok(&ea, 1u << width))
            return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
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
        if ((op == 0xC6 || op == 0xC7) && ea.reg != 0) goto unsupported;
        if (ea.mod != 3 && !b86_rm1_memok(&ea, 1u << width))
            return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
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
    if (ea.mod != 3 && !b86_rm1_memok(&ea, 1u << width))
        return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
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

/* BB_386_STAGE5A_V1: 386 real-mode far32 / 32-bit stack extension.
 * The current real-mode CS and SS limit remains 0xffff. SP is the stack
 * ADDRESS register even with operand-size override 66h; high ESP is retained.
 * This milestone returns B86_UNSUPPORTED (not a fabricated CPU fault) when
 * validation fails. Preflight is done before state or stack modification.
 * No protected-mode selector checks, exception delivery or EFLAGS[31:16].
 */
static BB386RM3_FLASH int b86_rm5_slot(B86Cpu *c, unsigned off)
{
    B86RM1EA m;
    (void)c;
    memset(&m, 0, sizeof(m));
    m.seg = B86_SS;
    m.off = off;
    return b86_rm1_memok(&m, 4u);
}

/* Far CALL32 stores the old CS DWORD first, then the return EIP DWORD.
 * RETF32 pops EIP then CS. Check BOTH destinations before writing. */
static BB386RM3_FLASH int b86_rm5_far_enter(St *s, uint32_t ip, uint16_t cs, int call)
{
    B86Cpu *c = s->c;
    if (ip > 0xFFFFu) goto gp;
    if (call) {
        uint16_t sp = (uint16_t)c->r[B86_SP];
        unsigned first = (uint16_t)(sp - 4u);
        unsigned second = (uint16_t)(sp - 8u);
        if (!b86_rm5_slot(c, first) || !b86_rm5_slot(c, second)) goto ss;
        if (!b86_rm3_stack_push(c, c->seg[B86_CS])) goto ss;
        if (!b86_rm3_stack_push(c, s->ip)) goto ss;
    }
    b86_set_seg(c, B86_CS, cs);
    c->ip = (uint16_t)ip;
    return B86_OK;
gp:
    return b86_rm5c2_fault(s, 13u);
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm5_far32(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    if (op == 0x9A || op == 0xEA) { /* CALL/JMP ptr16:32 immediate */
        uint32_t ip = b86_rm1_f32(s);
        uint16_t cs = f16(s);
        return b86_rm5_far_enter(s, ip, cs, op == 0x9A);
    }
    if (op == 0xCA || op == 0xCB) { /* RETF32, RETF32 imm16 */
        uint16_t extra = op == 0xCA ? f16(s) : 0;
        uint16_t sp = (uint16_t)c->r[B86_SP];
        uint32_t ip, cs;
        if (sp > 0xFFF8u || !b86_rm5_slot(c, sp) ||
            !b86_rm5_slot(c, sp + 4u)) goto ss;
        if (!b86_rm3_stack_read(c, sp, &ip) ||
            !b86_rm3_stack_read(c, sp + 4u, &cs)) goto ss;
        if (ip > 0xFFFFu) goto gp;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) |
                       (uint16_t)(sp + 8u + extra);
        b86_set_seg(c, B86_CS, (uint16_t)cs);
        c->ip = (uint16_t)ip;
        return B86_OK;
    }
gp:
    return b86_rm5c2_fault(s, 13u);
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm5_far_indirect32(St *s, int address32, int call)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    if (address32) (void)b86_rm1_ea(s, &ea);
    else {
        modrm(s);
        ea.mod = s->mod; ea.rm = s->rm; ea.reg = s->reg;
        ea.off = s->ea_off; ea.seg = s->ea_seg;
    }
    /* FF /3 and /5 are MEMORY-ONLY m16:32; no torn selector read. */
    if (ea.mod == 3) goto unsupported;
    if (ea.off > 0xFFFAu || !b86_rm1_memok(&ea, 4u))
        return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
    uint32_t ip = b86_rm1_read(c, &ea, 4u);
    ea.off += 4u;
    uint16_t cs = (uint16_t)b86_rm1_read(c, &ea, 2u);
    return b86_rm5_far_enter(s, ip, cs, call);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm5_stack32(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    uint32_t value;
    if (op >= 0x50 && op <= 0x57) {  /* PUSH r32: PUSH ESP uses PRE-push ESP */
        value = c->r[op & 7u];
        if (!b86_rm3_stack_push(c, value)) goto ss;
    } else if (op >= 0x58 && op <= 0x5F) { /* POP r32: POP ESP overwrites ESP */
        if (!b86_rm3_stack_pop(c, &value)) goto ss;
        c->r[op & 7u] = value;
    } else if (op == 0x68) { /* PUSH imm32 */
        value = b86_rm1_f32(s);
        if (!b86_rm3_stack_push(c, value)) goto ss;
    } else if (op == 0x6A) { /* PUSH sign-extended imm8 */
        value = (uint32_t)(int32_t)(int8_t)f8(s);
        if (!b86_rm3_stack_push(c, value)) goto ss;
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* BB_386_STAGE5B_V1: bounded real-address interrupts and IRET(D).
 * The Intel 80386 real-address INT frame is ALWAYS three WORDs (FLAGS, CS,
 * IP), even for 66h INT. IRETD (66 CF) instead pops 3 DWORDs. These are
 * different operations; do not synthesize a 32-bit INT frame.
 *
 * The CPU's existing public flag model only implements bits 0x0FD5; this
 * stage refuses unsupported IRETD EFLAGS images (IOPL/NT/RF/VM etc.).
 * No protected mode, double fault, or architectural #GP/#SS delivery yet:
 * out-of-range stack, IDTR or EIP returns B86_UNSUPPORTED before mutation.
 * 16-bit stack address size preserves ESP[31:16] even for IRETD.
 */
static BB386RM3_FLASH int b86_rm5b_word_slot(unsigned off)
{
    return off <= 0xFFFEu;
}

static BB386RM3_FLASH uint16_t b86_rm5b_word(B86Cpu *c, unsigned off)
{
    return rd16(c, B86_SS, off);
}

static BB386RM3_FLASH void b86_rm5b_push_word(B86Cpu *c, uint16_t value)
{
    uint16_t sp = (uint16_t)(c->r[B86_SP] - 2u);
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | sp;
    wr16(c, B86_SS, sp, value);
}

static BB386RM3_FLASH int b86_rm5b_int(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    uint8_t vector = op == 0xCD ? f8(s) : op == 0xCC ? 3u : 4u;
    uint32_t slot = (uint32_t)vector * 4u;
    uint64_t phys = (uint64_t)c->idt_base + slot;
    uint16_t sp = (uint16_t)c->r[B86_SP];
    uint16_t retflags, destip, destcs;
    if (op == 0xCE && !(c->flags & B86_OF)) {
        c->ip = s->ip;
        return B86_OK;
    }
    /* Host software-INT hooks own their own state/stack, as in b86_interrupt.
     * Set architectural NEXT IP before calling the hook. */
    if (c->int_hook) {
        c->ip = s->ip;
        if (c->int_hook(c, vector)) return B86_OK;
    }
    if (slot + 3u > c->idt_limit) return b86_rm5c2_fault(s, 8u);
    if (phys + 3u >= B86_MEM_BYTES) return b86_rm5c4a_shutdown(s);
    /* The stack address-size remains 16, so each push wraps SP separately.
     * Check all three WORD destinations before touching memory. */
    if (!b86_rm5b_word_slot((uint16_t)(sp - 2u)) ||
        !b86_rm5b_word_slot((uint16_t)(sp - 4u)) ||
        !b86_rm5b_word_slot((uint16_t)(sp - 6u))) return b86_rm5c4a_shutdown(s);
    destip = (uint16_t)(*b86_host(c, (uint32_t)phys & c->amask) |
             ((uint16_t)*b86_host(c, (uint32_t)(phys + 1u) & c->amask) << 8));
    destcs = (uint16_t)(*b86_host(c, (uint32_t)(phys + 2u) & c->amask) |
             ((uint16_t)*b86_host(c, (uint32_t)(phys + 3u) & c->amask) << 8));
    retflags = b86_get_flags(c);
    b86_rm5b_push_word(c, retflags);
    c->flags &= ~(uint32_t)(B86_IF | B86_TF);
    b86_rm5b_push_word(c, (uint16_t)c->seg[B86_CS]);
    b86_rm5b_push_word(c, s->ip);
    b86_set_seg(c, B86_CS, destcs);
    c->ip = destip;
    return B86_OK;
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

static BB386RM3_FLASH int b86_rm5b_iret(St *s, int operand32)
{
    B86Cpu *c = s->c;
    uint16_t sp = (uint16_t)c->r[B86_SP];
    uint32_t eip, cs, flags;
    if (operand32) {  /* IRETD: three DWORDs; CS high 16 bits discarded */
        /* Independent DWORD reads wrap the 16-bit SP between operands.
         * Each individual operand must still fit within SS.limit. */
        if (!b86_rm5c4b_dword_slot(sp) ||
            !b86_rm5c4b_dword_slot((uint16_t)(sp + 4u)) ||
            !b86_rm5c4b_dword_slot((uint16_t)(sp + 8u))) goto ss;
        if (!b86_rm3_stack_read(c, sp, &eip) ||
            !b86_rm3_stack_read(c, (uint16_t)(sp + 4u), &cs) ||
            !b86_rm3_stack_read(c, (uint16_t)(sp + 8u), &flags)) goto ss;
        if (eip > 0xFFFFu) goto gp;
        /* Stage 5C1: preserve 386 IOPL/NT and RF, reject VM and new-mode bits. */
        if (flags & ~(uint32_t)(0x7FD5u | 0x0002u | 0x00010000u)) goto unsupported;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) |
                        (uint16_t)(sp + 12u);
    } else { /* IRET: three WORDs, including after 67h address override */
        /* Three separate WORD reads may wrap SP between operands. */
        if (!b86_rm5b_word_slot(sp) ||
            !b86_rm5b_word_slot((uint16_t)(sp + 2u)) ||
            !b86_rm5b_word_slot((uint16_t)(sp + 4u))) goto ss;
        eip = b86_rm5b_word(c, sp);
        cs = b86_rm5b_word(c, (uint16_t)(sp + 2u));
        flags = b86_rm5b_word(c, (uint16_t)(sp + 4u));
        /* Stage 5C1: 16-bit IRET restores IOPL/NT. */
        if (flags & ~(uint32_t)(0x7FD5u | 0x0002u)) goto unsupported;
        c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) |
                        (uint16_t)(sp + 6u);
    }
    b86_set_seg(c, B86_CS, (uint16_t)cs);
    b86_set_flags(c, (uint16_t)flags);
    if (operand32) c->flags = (c->flags & ~0x00010000u) | (flags & 0x00010000u);
    c->ip = (uint16_t)eip;
    return B86_OK;
gp:
    return b86_rm5c2_fault(s, 13u);
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* BB_386_STAGE5C1_V1: 386 FLAGS/IRET and bounded #UD fault delivery.
 * 386 real mode can represent IOPL and NT in the existing 32-bit flags
 * member; IRET32 may also load RF (bit 16). No VM86/PE transition is
 * supported. This is NOT yet generalized #GP/#SS or privilege machinery.
 * No CPU layout changes; non-386 paths remain byte-for-byte unchanged.
 */
static BB386RM3_FLASH int b86_rm5c_flags(St *s, uint8_t op, int operand32)
{
    B86Cpu *c = s->c;
    uint16_t sp = (uint16_t)c->r[B86_SP];
    uint32_t image;
    if (op == 0x9C) {
        /* PUSHFD clears RF/VM in its saved image. Here VM remains zero
         * because virtual-8086 mode is not implemented. */
        image = (uint32_t)b86_get_flags(c);
        if (operand32) {
            if (!b86_rm3_stack_push(c, image)) goto ss;
        } else {
            if (!b86_rm5b_word_slot((uint16_t)(sp - 2u))) goto ss;
            b86_rm5b_push_word(c, (uint16_t)image);
        }
    } else if (op == 0x9D) {
        if (operand32) {
            if (!b86_rm3_stack_read(c, sp, &image)) goto ss;
            c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 4u);
        } else {
            if (!b86_rm5b_word_slot(sp)) goto ss;
            image = b86_rm5b_word(c, sp);
            c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 2u);
        }
        /* Real-mode POPF(D) loads low flags (including IOPL and NT),
         * but does not enter VM86. RF is cleared upon instruction end. */
        b86_set_flags(c, (uint16_t)image);
        c->flags &= ~0x00010000u;
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
ss:
    return b86_rm5c2_fault(s, 12u);
unsupported:
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

/* 80386 reserved 0F 0B -> #UD. Deliver the architectural 16-bit real-mode
 * exception frame FLAGS, CS, faulting IP; #UD carries NO error code.
 * No software INT hook is called for a CPU-originated fault.
 * If the exception itself cannot be delivered with the current limited
 * machine state, return UNSUPPORTED without partial stack modification. */
static BB386RM3_FLASH int b86_rm5c_ud(St *s)
{
    /* One real-mode frame path, including IDTR-vector8 and shutdown. */
    return b86_rm5c2_fault(s, 6u);
}

/* BB_386_STAGE5C5A_V1: 386 real-mode, 32-bit operand-size 0F 01.
 * 66 changes the GDTR/IDTR base transfer from 24 bits to 32 bits.
 * Even with 67, the entire six-byte pseudo-descriptor must fit the
 * currently 64-KiB real-mode segment limit. No protected-mode entry.
 * The legacy 16-bit operand-size 0F 01 implementation is unchanged.
 */
static BB386RM3_FLASH int b86_rm5c5a_system_66(St *s, int address32)
{
    B86Cpu *c = s->c;
    B86RM1EA ea;
    uint32_t base;
    uint16_t limit;
    int form;
    (void)f8(s); /* consume second opcode byte 01h (0Fh already consumed) */
    form = b86_rm2_operand(s, address32, &ea);
    if (ea.reg <= 3u) {
        if (form != 0) return b86_rm5c2_fault(s, 6u); /* no register form */
        if (ea.off > 0xFFFAu)
            return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
        if (ea.reg < 2u) {   /* SGDT/SIDT: full base 32 and limit 16 */
            base = ea.reg == 0u ? c->gdt_base : c->idt_base;
            limit = ea.reg == 0u ? c->gdt_limit : c->idt_limit;
            b86_rm1_write(c, &ea, 2u, limit);
            ea.off += 2u;
            b86_rm1_write(c, &ea, 4u, base);
        } else {             /* LGDT/LIDT: full base 32 and limit 16 */
            limit = (uint16_t)b86_rm1_read(c, &ea, 2u);
            ea.off += 2u;
            base = b86_rm1_read(c, &ea, 4u);
            if (ea.reg == 2u) { c->gdt_limit = limit; c->gdt_base = base; }
            else { c->idt_limit = limit; c->idt_base = base; }
        }
    } else if (ea.reg == 4u || ea.reg == 6u) {
        /* SMSW/LMSW memory is still 16 bits. SMSW r32 zero-extends. */
        if (form == 0 && !b86_rm1_memok(&ea, 2u))
            return b86_rm5c2_fault(s, ea.seg == B86_SS ? 12u : 13u);
        if (ea.reg == 4u) {
            if (form != 0) b86_rm1_regput(c, ea.rm, 2u, c->msw);
            else b86_rm1_write(c, &ea, 2u, c->msw);
        } else {
            uint16_t v = (uint16_t)b86_rm1_rmget(c, &ea, 1u);
            /* PE=1 is legal but remains unimplemented, not a fake #UD. */
            if (v & 1u) { c->ip = s->fip0; return B86_UNSUPPORTED; }
            c->msw = (uint16_t)((c->msw & 1u) | (v & 0x000Eu));
        }
    } else return b86_rm5c2_fault(s, 6u); /* reserved group extensions */
    c->ip = s->ip;
    return B86_OK;
}

/* BB_386_STAGE5C5B_V1: 80386 real-mode MOV r32,CR0 / MOV CR0,r32.
 * Use the existing 286 MSW low bits, leaving B86Cpu layout and the native
 * Thumb-2 JIT ABI untouched. CR0.ET is a fixed 386 bit here (bit4).
 * PE, PG, NE and other unrepresented CR0 bits cannot be enabled yet;
 * return UNSUPPORTED atomically rather than fabricating protected mode.
 * CR2/CR3 encodings are architecturally valid and therefore unsupported,
 * NOT invalid-opcode exceptions. The other CR indexes are #UD on 386.
 * ModRM register form is mandatory and 66/67 do not alter the r32 width.
 */
static BB386RM3_FLASH int b86_rm5c5b_cr0(St *s, uint8_t op2)
{
    B86Cpu *c = s->c;
    uint8_t second = f8(s); /* 20h or 22h */
    uint8_t modrm = f8(s);
    unsigned cr = (modrm >> 3) & 7u;
    unsigned rm = modrm & 7u;
    uint32_t value;
    (void)second;
    if ((modrm & 0xC0u) != 0xC0u)
        return b86_rm5c2_fault(s, 6u); /* MOV CR cannot use memory */
    if (cr == 2u || cr == 3u) {
        c->ip = s->fip0; return B86_UNSUPPORTED; /* valid CR2/CR3 */
    }
    if (cr != 0u)
        return b86_rm5c2_fault(s, 6u); /* CR1,CR4..7 undefined on 386 */
    if (op2 == 0x20u) { /* MOV r32, CR0 */
        c->r[rm] = 0x10u | (uint32_t)(c->msw & 0xFu);
    } else { /* MOV CR0, r32 */
        value = c->r[rm];
        /* Only MP/EM/TS and the hardwired ET bit are representable.
         * No PE transition, paging, numeric-exception mode or reserved bits. */
        /* 5C5E: accept an isolated PE transition, never PG or unsupported
         * control bits; keep legacy MP/EM/TS handling byte-exact. */
        if ((c->msw & 1u) || (value & ~0x1Fu)) {
            c->ip = s->fip0; return B86_UNSUPPORTED;
        }
        if (value & 1u) {
            if (b86_pm3_enter(c, value) != B86_PM3_OK) {
                c->ip = s->fip0; return B86_UNSUPPORTED;
            }
        } else {
            c->msw = (uint16_t)((c->msw & ~(uint16_t)0xEu) | (value & 0xEu));
        }
    }
    c->ip = s->ip;
    return B86_OK;
}

static BB386RM3_FLASH int b86_rm3_dispatch(St *s, uint8_t op,
                                             int operand32, int address32)
{
    B86Cpu *c = s->c;
    /* Stage 5B intercepts 386 real-mode software INT and IRET first.
     * 66h does NOT widen a real-mode INT frame, but DOES widen IRET. */
    if (op == 0x9C || op == 0x9D)
        return b86_rm5c_flags(s, op, operand32);
    if (op == 0x0F && rd8(c, B86_CS, s->ip) == 0x0Bu)
        return b86_rm5c_ud(s);
    if (op == 0xCC || op == 0xCD || op == 0xCE)
        return b86_rm5b_int(s, op);
    if (op == 0xCF)
        return b86_rm5b_iret(s, operand32);
    if ((op >= 0xD0 && op <= 0xD3) || op == 0xC0 || op == 0xC1)
        return b86_rm3_shift386(s, op, operand32, address32);
    if (op == 0x8C || op == 0x8E)
        return b86_rm3_mov_seg(s, op, operand32, address32);
    if (op == 0x0F) {
        uint8_t op2 = rd8(c, B86_CS, s->ip);
        if (op2 == 0x20 || op2 == 0x22)
            return b86_rm5c5b_cr0(s, op2);
        if (op2 == 0x01 && operand32)
            return b86_rm5c5a_system_66(s, address32);
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
        /* Stage 5A: 32-bit operand forms in 16-bit real-mode segments. */
        if (op == 0x9A || op == 0xEA || op == 0xCA || op == 0xCB)
            return b86_rm5_far32(s, op);
        if ((op >= 0x50 && op <= 0x5F) || op == 0x68 || op == 0x6A)
            return b86_rm5_stack32(s, op);
        if (op == 0xFF) {
            uint8_t ext = (rd8(c, B86_CS, s->ip) >> 3) & 7u;
            if (ext == 3u || ext == 5u)
                return b86_rm5_far_indirect32(s, address32, ext == 3u);
        }
        if (op == 0xE8 || op == 0xE9 || op == 0xC2 || op == 0xC3 || op == 0xFF)
            return b86_rm3_near32(s, op, address32);
        if (op == 0x60 || op == 0x61 || op == 0x9C || op == 0x9D || op == 0xC9)
            return b86_rm3_stack32(s, op);
    }
    if (s->seg >= B86_FS && !address32 && op != 0x0F)
        return b86_rm3_fs16(s, op, operand32);
    return -1;
}

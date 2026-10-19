/* BB_386_RM2_FLASH_SRAM_V1: force every 386-only handler to XIP flash. */
/* blitz86 386 real-mode milestone 0.  Included once by interp.c after the
   existing 8086/186 helpers.  No JIT-generated 32-bit code is used yet.
   Address-size remains 16 bits; 67h is deliberately unsupported. */

static __attribute__((noinline, cold, section(".text.bb386_flash"))) uint32_t b86_rm0_f32(St *s)
{
    uint32_t lo = f16(s);
    return lo | ((uint32_t)f16(s) << 16);
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) uint32_t b86_rm0_rd32(B86Cpu *c, int seg, uint16_t off)
{
    uint32_t lo = rd16(c, seg, off);
    return lo | ((uint32_t)rd16(c, seg, (uint16_t)(off + 2u)) << 16);
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) void b86_rm0_wr32(B86Cpu *c, int seg, uint16_t off, uint32_t v)
{
    wr16(c, seg, off, (uint16_t)v);
    wr16(c, seg, (uint16_t)(off + 2u), (uint16_t)(v >> 16));
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) uint32_t b86_rm0_rmget(St *s)
{
    return s->mod == 3 ? s->c->r[s->rm] : b86_rm0_rd32(s->c, s->ea_seg, s->ea_off);
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) void b86_rm0_rmput(St *s, uint32_t v)
{
    if (s->mod == 3) s->c->r[s->rm] = v;
    else b86_rm0_wr32(s->c, s->ea_seg, s->ea_off, v);
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) void b86_rm0_push(B86Cpu *c, uint32_t v)
{
    uint16_t sp = (uint16_t)(c->r[B86_SP] - 4u);
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | sp;
    b86_rm0_wr32(c, B86_SS, sp, v);
}
static __attribute__((noinline, cold, section(".text.bb386_flash"))) uint32_t b86_rm0_pop(B86Cpu *c)
{
    uint16_t sp = (uint16_t)c->r[B86_SP];
    uint32_t value = b86_rm0_rd32(c, B86_SS, sp);
    c->r[B86_SP] = (c->r[B86_SP] & 0xFFFF0000u) | (uint16_t)(sp + 4u);
    return value;
}
/* 386 EFLAGS OSZAPC.  64-bit temporaries are confined to this cold flash
   routine.  The unchanged 8086/286 lazy record engine is never used for a
   32-bit operation (b86_step already materializes pending records). */
static __attribute__((noinline, cold, section(".text.bb386_flash"))) uint32_t b86_rm0_alu(B86Cpu *c, int kind, uint32_t a, uint32_t b)
{
    uint32_t r = 0, f = 0;
    uint32_t cin = (kind == 2 || kind == 3) && (c->flags & B86_CF) ? 1u : 0u;
    if (kind == 0 || kind == 2) {                       /* ADD / ADC */
        uint64_t full = (uint64_t)a + b + cin;
        r = (uint32_t)full;
        f = (full >> 32 ? B86_CF : 0u)
            | ((~(a ^ b) & (a ^ r) & 0x80000000u) ? B86_OF : 0u)
            | ((a ^ b ^ r) & B86_AF);
    } else if (kind == 5 || kind == 3 || kind == 7) {  /* SUB / SBB / CMP */
        uint64_t full = (uint64_t)a - b - cin;
        r = (uint32_t)full;
        f = ((uint64_t)a < (uint64_t)b + cin ? B86_CF : 0u)
            | (((a ^ b) & (a ^ r) & 0x80000000u) ? B86_OF : 0u)
            | ((a ^ b ^ r) & B86_AF);
    } else {
        switch (kind) {
        case 1: r = a | b; break;
        case 4: r = a & b; break;
        default: r = a ^ b; break;
        }
    }
    f |= (r & 0x80000000u ? B86_SF : 0u) | (!r ? B86_ZF : 0u)
         | b86_parity[r & 255u];
    c->flags = (c->flags & ~(uint32_t)B86_ARITH) | f;
    return r;
}

static __attribute__((noinline, cold, section(".text.bb386_flash"))) int b86_rm0_66(St *s, uint8_t op)
{
    B86Cpu *c = s->c;
    uint32_t a, b, result;
    int kind;
    if (op >= 0xB8 && op <= 0xBF) {
        c->r[op & 7] = b86_rm0_f32(s);
    } else if (op >= 0x50 && op <= 0x57) {
        b86_rm0_push(c, c->r[op & 7]);
    } else if (op >= 0x58 && op <= 0x5F) {
        c->r[op & 7] = b86_rm0_pop(c); /* POP ESP overwrites the adjusted ESP */
    } else if (op >= 0x40 && op <= 0x4F) {
        int r = op & 7;
        uint32_t cf = c->flags & B86_CF;
        c->r[r] = b86_rm0_alu(c, op >= 0x48 ? 5 : 0, c->r[r], 1);
        c->flags = (c->flags & ~(uint32_t)B86_CF) | cf;
    } else if (op == 0x68) {
        b86_rm0_push(c, b86_rm0_f32(s));
    } else if (op == 0x6A) {
        b86_rm0_push(c, (uint32_t)(int32_t)(int8_t)f8(s));
    } else if (op == 0x69 || op == 0x6B) {
        /* B86_386_IMUL_IMM_V1: 386 operand-size IMUL r32,r/m32,imm8/imm32 */
        /* Decode the ModR/M operand BEFORE the immediate, as on an 80386.
           A 6Bh immediate is signed byte; 69h is a signed DWORD. */
        modrm(s);
        int32_t multiplicand = (int32_t)b86_rm0_rmget(s);
        int32_t immediate = op == 0x69 ? (int32_t)b86_rm0_f32(s)
                                       : (int32_t)(int8_t)f8(s);
        /* Full 32x32 signed product fits int64_t. Truncation is defined
           after conversion to uint32_t; no signed 32-bit overflow UB. */
        int64_t product = (int64_t)multiplicand * (int64_t)immediate;
        uint32_t low = (uint32_t)product;
        c->r[s->reg] = low;
        /* x86 defines CF and OF for IMUL; other arithmetic flags undefined.
           Preserve those flags rather than manufacturing ALU flags. */
        uint32_t overflow = (product < (-2147483647LL - 1LL) || product > 2147483647LL);
        c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF))
                 | (overflow ? (B86_CF | B86_OF) : 0u);
    } else if (op == 0x98) {                           /* CWDE */
        c->r[B86_AX] = (uint32_t)(int32_t)(int16_t)c->r[B86_AX];
    } else if (op == 0x99) {                           /* CDQ */
        c->r[B86_DX] = (int32_t)c->r[B86_AX] < 0 ? 0xFFFFFFFFu : 0;
    } else if (op == 0x90) {
        /* 66 NOP */
    } else if (op == 0x89 || op == 0x8B) {
        modrm(s);
        if (op == 0x89) b86_rm0_rmput(s, c->r[s->reg]);
        else c->r[s->reg] = b86_rm0_rmget(s);
    } else if (op == 0xC7) {
        modrm(s);
        if (s->reg != 0) goto unsupported;
        b86_rm0_rmput(s, b86_rm0_f32(s));
    } else if (op == 0xA1 || op == 0xA3) {
        uint16_t offset = f16(s);
        int seg = s->seg >= 0 ? s->seg : B86_DS;
        if (op == 0xA1) c->r[B86_AX] = b86_rm0_rd32(c, seg, offset);
        else b86_rm0_wr32(c, seg, offset, c->r[B86_AX]);
    } else if (op == 0x81 || op == 0x83) {
        modrm(s);
        kind = s->reg;
        /* Read immediates before writing, including register destinations. */
        a = b86_rm0_rmget(s);
        b = op == 0x81 ? b86_rm0_f32(s) : (uint32_t)(int32_t)(int8_t)f8(s);
        result = b86_rm0_alu(c, kind, a, b);
        if (kind != 7) b86_rm0_rmput(s, result);
    } else if (op < 0x40 && (op & 7) <= 5 && (op & 1)) {
        kind = op >> 3;
        if ((op & 7) == 5) {
            a = c->r[B86_AX]; b = b86_rm0_f32(s);
            result = b86_rm0_alu(c, kind, a, b);
            if (kind != 7) c->r[B86_AX] = result;
        } else {
            modrm(s);
            if ((op & 2) == 0) {
                a = b86_rm0_rmget(s); b = c->r[s->reg];
                result = b86_rm0_alu(c, kind, a, b);
                if (kind != 7) b86_rm0_rmput(s, result);
            } else {
                a = c->r[s->reg]; b = b86_rm0_rmget(s);
                result = b86_rm0_alu(c, kind, a, b);
                if (kind != 7) c->r[s->reg] = result;
            }
        }
    } else if (op == 0x85 || op == 0xA9) {             /* r57: TEST r/m32,r32 / EAX,imm32 */
        if (op == 0xA9) { a = c->r[B86_AX]; b = b86_rm0_f32(s); }
        else { modrm(s); a = b86_rm0_rmget(s); b = c->r[s->reg]; }
        (void)b86_rm0_alu(c, 4, a, b);
    } else if (op >= 0x91 && op <= 0x97) {             /* r57: XCHG EAX, r32 */
        a = c->r[B86_AX]; c->r[B86_AX] = c->r[op & 7]; c->r[op & 7] = a;
    } else if (op == 0x87) {                           /* r57: XCHG r/m32, r32 */
        modrm(s);
        a = b86_rm0_rmget(s); b = c->r[s->reg];
        b86_rm0_rmput(s, b); c->r[s->reg] = a;
    } else if (op == 0x8D) {                           /* r57: LEA r32, m (16-bit address) */
        modrm(s);
        if (s->mod == 3) { c->ip = s->fip0; b86_interrupt(c, 6); return B86_OK; }
        c->r[s->reg] = (uint16_t)s->ea_off;
    } else if (op == 0xF7) {                           /* r57: group 3, 32-bit */
        modrm(s);
        uint32_t v = b86_rm0_rmget(s);
        switch (s->reg) {
        case 0: case 1: (void)b86_rm0_alu(c, 4, v, b86_rm0_f32(s)); break;      /* TEST */
        case 2: b86_rm0_rmput(s, ~v); break;                                   /* NOT */
        case 3: b86_rm0_rmput(s, b86_rm0_alu(c, 5, 0, v)); break;              /* NEG */
        case 4: case 5: {                                                       /* MUL / IMUL */
            uint64_t pr; int ov;
            if (s->reg == 4) { pr = (uint64_t)c->r[B86_AX] * v; ov = (pr >> 32) != 0; }
            else { int64_t sp = (int64_t)(int32_t)c->r[B86_AX] * (int32_t)v; pr = (uint64_t)sp; ov = sp != (int64_t)(int32_t)sp; }
            c->r[B86_AX] = (uint32_t)pr; c->r[B86_DX] = (uint32_t)(pr >> 32);
            c->flags = (c->flags & ~(uint32_t)(B86_CF | B86_OF)) | (ov ? (B86_CF | B86_OF) : 0u);
            break; }
        default: {                                                              /* DIV / IDIV */
            uint64_t n = (uint64_t)c->r[B86_DX] << 32 | c->r[B86_AX];
            if (!v) { c->ip = s->fip0; b86_interrupt(c, 0); return B86_OK; }
            if (s->reg == 6) {
                uint64_t q = n / v;
                if (q > 0xFFFFFFFFull) { c->ip = s->fip0; b86_interrupt(c, 0); return B86_OK; }
                c->r[B86_AX] = (uint32_t)q; c->r[B86_DX] = (uint32_t)(n % v);
            } else {
                int64_t sn = (int64_t)n, sd = (int32_t)v;
                if (sn == INT64_MIN && sd == -1) { c->ip = s->fip0; b86_interrupt(c, 0); return B86_OK; }
                int64_t q = sn / sd;
                if (q > 2147483647LL || q < -2147483648LL) { c->ip = s->fip0; b86_interrupt(c, 0); return B86_OK; }
                c->r[B86_AX] = (uint32_t)q; c->r[B86_DX] = (uint32_t)(sn % sd);
            }
            break; }
        }
    } else if (op == 0xFF) {                           /* r57: INC/DEC/PUSH r/m32 */
        modrm(s);
        if (s->reg == 0 || s->reg == 1) {
            uint32_t cf = c->flags & B86_CF;
            b86_rm0_rmput(s, b86_rm0_alu(c, s->reg ? 5 : 0, b86_rm0_rmget(s), 1));
            c->flags = (c->flags & ~(uint32_t)B86_CF) | cf;
        } else if (s->reg == 6) b86_rm0_push(c, b86_rm0_rmget(s));
        else goto unsupported;
    } else if (op == 0x8F) {                           /* r57: POP r/m32 */
        modrm(s);
        if (s->reg != 0) goto unsupported;
        a = b86_rm0_pop(c);
        b86_rm0_rmput(s, a);
    } else goto unsupported;
    c->ip = s->ip;
    return B86_OK;
unsupported:
    /* Do not turn a valid but not-yet-implemented 386 instruction into #UD. */
    c->ip = s->fip0;
    return B86_UNSUPPORTED;
}

# blitz86 — design

An 8086 / real-mode DOS CPU core for small ARM machines: a dynamic binary
translator with an AArch64 backend (Raspberry Pi Zero 2 W, Cortex-A53) and a
Thumb-2 backend (RP2350 / Pimoroni Pico 2 Plus, Cortex-M33), plus a
reference interpreter validated against physical-8086 test vectors.

## 1. Why this shape

microDOS taught one lesson repeatedly: its best emitter (Native v2) already
ran register loops at ~2.3 host cycles per guest instruction, yet whole-DOS
throughput stayed near 3.5 MIPS because that emitter only *admitted* about
8% of executed code. Every later engine either rejected shapes or paid tier
switching costs.

blitz86 therefore follows one rule: **translate everything, reject nothing.**
An instruction with no native lowering becomes an inline call to the
reference interpreter for that single instruction, inside the translated
block. There is no other execution tier, no admission, no region rejection.
Coverage of the native lowering only changes speed, never whether code runs
translated.

## 2. Components

| file | role |
|---|---|
| `src/interp.c` | reference interpreter: oracle for testing and the per-instruction fallback |
| `src/jit.c` | frontend (decode, classify, flag liveness, fusion/lazy decisions, lowering) and runtime (block cache, chaining, dispatcher, SMC) |
| `src/backend.h` | the contract both backends implement |
| `src/be_a64.c` | AArch64 encoder |
| `src/be_t2.c` | Thumb-2 encoder |

The frontend owns all x86 semantics. A backend encodes ~40 operations
(move, ALU, bitfield, shifted-domain compares, EA, load/store with SMC
check, exits, helper calls).

## 3. State and registers

Guest registers live in host registers for the whole time translated code
runs, across chained blocks. Bits 16–31 of a guest register are allowed to
hold garbage; consumers that care (addresses, compares, right shifts) mask
or shift. This removes a UXTH after almost every ALU operation.

| | AArch64 | Thumb-2 |
|---|---|---|
| AX..DI | w19–w26 (callee-saved: survive C helpers) | r0–r7 |
| `B86Cpu*` | x27 | r8 |
| SMC line map (biased by host addr >> 6) | x28 | r9 |
| DS base pointer | x29 | r10 |
| SS base pointer | x14 | r11 |
| ES base pointer | x15 | loaded on use |
| scratch | w9–w11, w16–w17 | r12, lr |

Segment bases are kept as host pointers `mem + seg*16`. With A20 enabled
(the model the JIT uses) no linear wrap is needed: every segment is a flat
64 KiB window, and an effective address is
`base + UXTH(b1 + b2 + disp)` — 2 instructions for `[bx]`, 4 for
`[bx+si+disp]`. Guest memory is 1 MiB + HMA + slack.

## 4. Flags

Per block, a backward liveness pass computes which of OSZAPC are live after
every instruction. Liveness at block exits comes from a *lookahead* that
decodes up to 12 instructions at each successor until every flag is
redefined; the scanned bytes join the block's SMC-guarded range so that
modifying them invalidates the block.

Each flag producer then takes one of three forms:

* **fused** — the producer computes host NZCV on operands shifted to the top
  of the register (`sh` = 16 for words, 24 for bytes), so N/Z/V equal
  SF/ZF/OF exactly and C equals CF (add) or !CF (sub). An in-block Jcc
  becomes one conditional branch. `cmp ax,100 / jl` = 2 instructions + branch.
* **lazy** — two stores: operand `a` and the result. The second operand is
  implied (`b = res - a` or `a - res`) because lazily recorded producers
  never have a carry-in. INC/DEC write an *overlay* record so CF keeps
  coming from the earlier producer with no extra work.
* **dead** — nothing at all; dead CMP/TEST emit no code.

Decisions are made in two passes: (A) each Jcc finds its producer walking
backwards and fuses if nothing in between can clobber NZCV (conservatively:
helpers, non-fused consumers, SMC-checked stores on Thumb-2, other live
producers); (B) a producer gets a lazy record unless every reader of its
flags is one of its fused consumers. Producers whose NZCV-held flags are
live across an SMC-checked store also get the record, because a store that
invalidates its own block exits right there.

If a lowering cannot be done at emit time (Thumb-2 register pressure), the
instruction is demoted to the helper and the whole block is re-analysed,
so a consumer is never fused to a producer that did not set NZCV.

## 5. Blocks, chaining, indirect jumps

* Superblocks: up to 48 instructions and 480 guarded bytes; conditional
  branches become side exits and translation continues on the fall-through.
* Every exit to a known target is a patchable `B`. The first time it is
  taken it returns to the dispatcher, which translates the target and
  patches the branch to jump straight into it.
* RET and indirect jumps go through one shared stub that probes a
  4096-entry direct-mapped `(CS:IP) → host` table (~13 host instructions on
  a hit), falling back to the dispatcher on a miss.
* Backward branches and the indirect stub poll `cpu->irq`, so the host can
  always regain control (timers, USB, video) within one loop iteration.

## 6. Self-modifying code

Stores test one byte per 64-byte line (`codemap`): 3 instructions on
AArch64 (no NZCV impact; the slow path saves it), 4 on Thumb-2. Only lines
holding live translated code take the slow path, which invalidates exactly
the blocks whose guarded range contains the written bytes; data next to
code costs a slow-path call, not a retranslation. Invalidated blocks get
their entry patched to an exit stub, so blocks that chained into them stay
safe. A store that invalidates its own running block exits right after the
store with exact state.

Implicit stack writes (PUSH, CALL, interrupt frames) are not SMC-checked;
code built on the stack is not supported. Explicit `[bp+..]` stores are
checked.

## 7. Correctness strategy

1. `tests/sst_interp.c` — interpreter vs 614,000 SingleStepTests vectors from
   a physical Intel P80C86A-2 (all normal/alias opcodes): **all pass**.
2. `tests/sst_jit.c` — the same vectors through the translator, one
   instruction per block, both backends (585,933 run; 28,067 depend on
   1 MiB / in-segment wrap, which the A20-on model deliberately omits).
3. `tests/fuzz.c` — random *structured* programs (all Jcc forms, loops,
   near/indirect/far calls, INT/IRET, divide faults, REP strings, segment
   loads, PUSHF/POPF, BCD, shifts, deliberate SMC into upcoming
   instructions) on translator vs interpreter, comparing registers, every
   flag bit including undefined ones, and all of memory.
4. `FUZZ_TRACE=1` lockstep mode: one translated block, then exactly the
   number of instructions it retired on the interpreter, compare. Exits
   record their retired count when `b86_jit_set_count_exits` is on.

## 8. Measured cost (steady state, from qemu exec logs)

Host instructions executed per guest instruction. "native" kernels run
entirely in translated code.

| kernel | AArch64 | Thumb-2 |
|---|---|---|
| regmix: `add/xor/sub/inc/loop` | 3.80 | 4.20 |
| cmpbr: `cmp/jl/sub/add/dec/jnz` | 4.54 | 4.54 |
| strlen: `mov al,[si]/inc/or/jnz` | 3.74 | 4.73 |
| memrmw: `add [bx+si+4],ax/add/dec/jnz` | 6.75 | 7.50 |
| call: `call/add/ret/dec/jnz` | 8.20 | 9.60 |
| stack: `push/push/pop/pop/loop` | 3.60 | 4.60 |
| lodsto: `lodsb/stosb/loop` (helper) | 134 | – |
| shift: `shl/adc/shr/loop` (helper) | 195 | – |
| mul: `mov/mul/add/loop` (helper) | 70 | – |

On a Cortex-M33 most of these instructions are single-cycle (loads 2,
taken branches 2–4), so the native kernels land roughly at 5–12 cycles per
guest instruction, i.e. **25–60 MIPS at 300 MHz** — before any of the
optimizations below. On the A53 at 1 GHz, dual issue, the same kernels
are in the 150–300 MIPS range. These are estimates from instruction
counts; hardware timing is the next measurement.

## 9. Known gaps and the optimization queue

Ranked by expected payoff:

1. **Native string ops**: LODS/STOS/MOVS/SCAS/CMPS without REP, and a
   bulk C helper for REP MOVS/STOS (memcpy/memset with one SMC range check).
   Today they single-step through the interpreter (~130 host insns each).
2. **Native shifts/rotates with live flags, MUL/IMUL, ADC/SBB, CLC/STC/CMC,
   XLAT, LES/LDS, PUSHF/POPF, LAHF/SAHF.**
3. **Exit-time flag materialization**: loop bodies currently write lazy
   records every iteration when flags are live only on the loop-exit path.
   Most producers' records can be rebuilt in the out-of-line exit stub
   from registers instead.
4. **Return-address prediction** for CALL/RET (shadow stack of host
   addresses) instead of the hashed lookup.
5. **Thumb-2 code size**: use 16-bit encodings where flags allow;
   RP2350 SRAM is the code cache.
6. **Pico SMC via the MPU**: ARMv8-M MPU regions with 32-byte granularity
   could remove inline store checks for most stores.
7. **SMC-tolerant translation** for code that rewrites itself every frame
   (load modified immediates from guest memory instead of baking them).

Not modelled: A20 wrap (EXEPACK-era binaries), EGA/VGA planar memory (needs
trapped segments), the 8086 prefetch queue, TF single-step traps, and
hardware interrupt delivery (the `irq` poll is the hook for it).

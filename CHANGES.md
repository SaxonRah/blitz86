# Changes

## 2026-10-08 (d) — flag records across RET

* RET cold paths write the callee's deferred flag record; the inline-cache
  hit enters a continuation specialized with the pending producers as a
  virtual prefix (records rebuilt only at exits that need them).
* Lookahead follows direct JMP/CALL; blocks guard up to three ranges.
* Call kernel host insns/guest insn: Thumb-2 8.2 -> 5.8, AArch64 7.0 -> 4.8.
* Fuzz caught a bug during development: the "record then normal block"
  fallback jumped to the path's final branch, skipping the record (fixed;
  covered by `FUZZ_NOSPEC=1`).
* Test knob: `b86_jit_set_no_spec`.

Verified (qemu): silicon 585,933/585,933 both backends; fuzz 0 mismatches
both backends (incl. NOSPEC and Pico table sizes); hwbench 9/9 both.

## 2026-10-08 (c) — unrolling + RET inline cache

Hardware result of (b), RP2350 @300 MHz warm median: regmix 121.7 MIPS
(Native-3 132, ratio 1.08), loop 64.4 (N3 186), callmix 20.7 (N3 86);
Pi Zero 2 W regmix 712 MIPS, loop 333.

* **Self-loop unrolling** (2-4x): one poll + back-edge per several
  iterations. Thumb-2 regmix 2.20 -> 1.53 host insns/guest insn.
* **RET inline cache** per site, filled by the dispatcher; the hit path is
  7 instructions instead of ~24 (pop + hashed lookup stub). Call kernel
  Thumb-2 10.6 -> 8.2, AArch64 8.8 -> 7.0.

Verified (qemu): silicon 585,933/585,933 both backends; fuzz 0 mismatches
both backends (incl. Pico table sizes); hwbench 9/9 both.

## 2026-10-08 (b) — loop-overhead release

First hardware numbers (previous release, warm median): RP2350 @300 MHz
20.7–54.9 MIPS, Pi Zero 2 W 104–356 MIPS, all states identical to Native-3.
Native-3 on the Pico was 2.4–5.4x faster on loop/regmix/callmix; the cause
was per-iteration loop overhead, addressed here:

* **Exit-time flag materialization.** A producer whose flags are needed only
  at exits (typically the loop's fall-through, e.g. HLT after `dec cx; jnz`)
  no longer writes its lazy record every iteration: the exit path rebuilds it
  from registers (`a = res -/+ b`, INC/DEC overlay, CMP/TEST recomputed).
  Not deferred across C helpers that materialize flags (found by fuzz seeds
  70419/70817: LOOPZ's helper folded an INC overlay, then a rebuilt CMP
  record overrode it).
* **Z/S-only fusion**: when the fused branch reads only ZF/SF, the producer is
  a plain op + one flag test (`LSLS` / `CMN`) instead of the 3-instruction
  shifted-domain sequence. `dec cx; jnz` = SUBW + LSLS + branch.
* **Inline backward exits**: `B!cc skip ; [records] ; irq poll ; B target`,
  one taken branch per iteration instead of two (Jcc, LOOP, JCXZ).
* RET/HLT no longer claim to read all flags (liveness unchanged).

Thumb-2 host instructions per guest instruction: regmix 4.20 -> 2.20,
cmpbr 4.54 -> 2.37, strlen 4.73 -> 3.24, memrmw 7.50 -> 4.75.

## 2026-10-08 — hardware-readiness release

* **Seed 51187 root-caused and fixed.** Not chaining or the fast table (the
  hang reproduces with both disabled): PUSH skipped the SMC check, and the
  runaway program's stack grew into the bytes it was executing. All pushes
  (and CALL's return-address push) are now SMC-checked. `src/jit.c` in the
  repo was the `no-chain` diagnostic variant; this release restores chaining.
* **`b86_jit_create_ex()`** places the fast lookup table and SMC line map in a
  caller-supplied buffer (SRAM on RP2350). `b86_jit_hot_bytes()` sizes it.
* **Configurable tables**: `B86_FAST_BITS`, `B86_MAXB`, `B86_MAP_BITS`. They
  must match across `jit.c` and the backend file.
* **New native lowerings**: LODS/STOS/MOVS; bulk REP MOVS/STOS; SHL/SHR/SAR
  by 1 with lazy kinds and SHL feeding NZCV; shifts by CL and MUL/IMUL when
  their flags are dead; ADC/SBB with carry fused from the previous ADD/SUB;
  CLC/STC/CMC inline.
* **Lazy flags**: INC/DEC overlay record (no per-iteration CF materialize);
  ADC/SBB record with explicit second operand.
* **Test knobs**: `b86_jit_set_no_chain`, `FUZZ_NOCHAIN`, `FUZZ_NOFAST`,
  `FUZZ_ALARM`; lockstep tracer reports how it ended.
* **tests/hwbench.c**: mirrors the microDOS hardware drivers (nine workloads,
  oracle compare, warm reruns on a live JIT) under qemu: `make hwbench`.

Verified (qemu): interpreter 614,000/614,000 silicon vectors; translator
585,933/585,933 on both backends; fuzz 0 mismatches on both backends
(seeds 1–2500, 50000–52599); hwbench 9/9 on both with helpers=0.

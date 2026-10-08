# Changes

## 2026-10-08 (h) — measurement release

RP2350 result of (g): active 4.73 s (3.98 MIPS; interpreter 7.93 s, 2.37),
native 4.48 s (translate 272 ms), sync 38 ms, XIP misses 10.2M.

* `B86JitStats`: runtime C round trips by kind (`rt_step`, `rt_cond`,
  `rt_flags`, `rt_light`, `rt_smc`, `rt_rep`) and `translate_misses`
  (`-DB86_MISSES=fn`). On the DOS session: flags 934K, light 525K, step 50K,
  cond 26K, smc 4K, rep 446.

## 2026-10-08 (g) — helper executions on real DOS code: 2.54M -> 50K

RP2350 result of (f): active 6.90 s (interpreter 7.93 s), native 6.46 s of
which translate 285 ms, sync 259 ms. A histogram of interpreter-helper
executions on the DOS2TEST+MDSTRESS session showed 2,544,569 round trips
(ROL/ROR r16,1 1.18M; PUSHF/POPF/SAHF/LAHF ~263K each; MUL r16 with live
flags 131K; DIV r16 132K), each also reading JIT state from PSRAM.

* Native ROL/ROR r,1 (CF/OF through a light C call only when live).
* Native PUSHF/POPF/SAHF/LAHF.
* Native MUL/IMUL with live flags (light call for CF/OF/SF/ZF/PF).
* Native DIV r/m16 via UDIV + MLS; divide-by-zero/overflow go to the
  interpreter, which raises INT 0.
* `be_udiv`, `be_mls`, `be_call_light` backend primitives.
* JIT state `J` and a per-block dead map now in normal (SRAM) heap; only the
  big tables use B86_CALLOC.
* `-DB86_HELPER_HISTO`: per-opcode helper execution histogram.

## 2026-10-08 (f) — first hardware DOS run follow-up

RP2350 result of (e): DOS2TEST 25/25, MDSTRESS 0xA298, blitz86 retired
18,845,294 of 18,873,065 instructions, but active time 9.56 s vs 7.93 s for
the interpreter, with XIP/QMI misses up from 3.98M to 18.87M (1.0 per guest
instruction). Causes and fixes:

* Code-hook (`cpu->code_hook`) reports every translated byte range, so an
  embedder can filter its own writes byte-exactly (blitzBUS feeds
  microDOS's TRBYTES bitmaps). DOS-session page syncs 6,513 -> 399.
* Dispatcher resolves re-entries from the SRAM fast table before touching
  the (PSRAM) block map/table: about half of all dispatches.
* `B86_HOT` / `-DB86_RAM_FUNCS=1`: all translator, dispatcher, helper and
  interpreter code goes to `.time_critical.blitz86` (RAM on the Pico SDK)
  instead of executing from flash through the XIP cache shared with PSRAM.
* `B86JitStats.translate_us` / `fast_dispatches` (`-DB86_NOW=fn`).

## 2026-10-08 (e) — DOS owner mode support + compact Thumb-2

blitz86 now runs MS-DOS 2.0 inside microDOS (blitzBUS live backend):
DOS2TEST 25/25 and MDSTRESS checksum 0xA298 on both backends under qemu,
99.9% of instructions retired by blitz86 (the rest is microDOS's BIOS
trampoline segment, by design).

* `cpu->trap_cs`: b86_jit_run returns B86_TRAP before running code in that CS.
* Exact retired-instruction counting (`b86_jit_set_count_retired`, cpu->icnt):
  one add per block exit; fuzz checks it equals the interpreter on every
  program.
* `b86_jit_invalidate`, and `b86_jit_set_shadow` + `b86_jit_sync_external`:
  byte-exact invalidation for memory written by someone else (microDOS
  hooks / BIOS interpretation). Cut DOS-session retranslations 71,868 ->
  15,442 before the code-size work below.
* `B86_CALLOC`/`B86_FREE`: put dispatcher-only metadata in slow memory.
* Guest memory must be 64-byte aligned; jit_create refuses otherwise.
* INT hooks that change CS:IP or set cpu->irq end the block immediately.
* Shared cold-path stubs (chain request, SMC slow path, interpreter call):
  call site = `BL stub` + data words. DOS session Thumb-2 code 54.7 -> 39.0
  bytes per guest insn; at 192 KiB / 2048 blocks flushes 14 -> 8.

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

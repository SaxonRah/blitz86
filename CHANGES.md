# Changes

## 2026-10-09 (p) — cold runs follow taken branches; cheaper interpreter flags

Pico (o): 30.3 fps. The slow phases still interpret 4-5% of instructions.

* Cold runs no longer return to the dispatcher at every taken branch: they
  follow it, bump the target's heat exactly as a dispatch would, and stop
  when the target turns hot, reaches translated code, or after 256 steps.
* b86_step reports its opcode and the opcode's IP (step_op/step_oip), so the
  cold loop classifies control flow without refetching bytes.
* REP MOVS/STOS are never interpreted: a cold run stops before one, and a
  run starting at one reports it hot so it is translated (bulk REP helper).
* ADD/SUB/logic flags are computed branch-light (one expression for all six).
* Tried and dropped: lazy flags inside the interpreter (cold runs are short
  and nearly every producer is consumed by the next Jcc, so evaluating the
  record cost more than computing flags eagerly).
* Host instructions per interpreted instruction (all-cold bench, vs (o)):
  regmix 235 -> 215, cmpbr 222 -> 189, memrmw 263 -> 238, stack 177 -> 166.

## 2026-10-09 (o) — cheaper cold (interpreted) instructions

Pico v46: 25.8 fps; slow phases spend ~4.5-6.5% of instructions in the
interpreter, likely 25-35% of their time.

* cold_run no longer runs a full decode()+classify() per instruction just to
  decide where a run stops: a 256-entry opcode table (prefixes, Jcc/LOOP,
  CALL/JMP/RET/INT/IRET/far, POP CS, plus ModRM.reg for FF and 8E) does it.
* b86_step only calls b86_flags_materialize when a lazy record is pending.
* Interpreter 16-bit reads/writes translate the page once unless they cross
  a 4 KiB page.
* Host instructions per interpreted guest instruction (qemu exec count,
  all-cold bench): regmix 370 -> 235, memrmw 400 -> 263, stack 303 -> 177
  (about -35%). An instruction-fetch window was tried and was slower.

## 2026-10-09 (n) — translator out of SRAM

* B86_COLD: the translator (analysis, lowering, emit, translate) and the
  whole Thumb-2 emitter now stay in flash; only the dispatcher, helpers,
  cold-run decode/interpreter and the code-patching entry points (B86_RT)
  remain in .time_critical. RAM code: jit.c ~32 KiB -> ~8 KiB, be_t2.c
  18 KiB -> 0.4 KiB. B86_COLD_RAM=1 restores the old placement.
* Translator scratch (two Insn[128], 14 KiB of .bss) now comes from
  B86_CALLOC (blitzBUS: PSRAM arena).
* b86_map_range failure codes: -2 frame index collides with guest lines,
  -3 translated code in the range (-1 otherwise).

## 2026-10-09 (m1) — frames may overlap the page-table index range

blitzBUS v44 moved its A000 frame below 0x20020000 (link order changed),
whose SMC-table index fell in the page-table range [0, 2 KiB) and
b86_map_range refused it ("A000 window NOT mapped"). That overlap only
costs a guarded slow-path call on stores to the few lines whose page-table
byte is nonzero, so it is allowed now; only overlap with guest lines (hot
code) is refused.

## 2026-10-09 (m) — cheaper paged accesses

v40-v42 on the Pico: paged code was ~25% bigger (33.6 host bytes per guest
insn vs ~27), the warm part of 3DBENCH no longer fit the code buffer and
10-15% of instructions ran interpreted in the slow phases (15.1 fps vs 18.8).

* Stack (SS) accesses are not page-translated; SS is cached in r11 again.
  Embedder rule: never map pages inside the current SS window.
* Page table now lives in the first 2 KiB of the 32 Ki SMC line table and is
  reached through r9 (`cpu->pt` points there once the JIT exists;
  `pt_store` before). Requires the guest buffer's line index >= 0x800
  (blitzBUS: 0x2800). b86_map_range refuses frames whose SMC-table index
  range would hit the page table or the guest lines.
* Translation add is a 16-bit ADD.
* Host 3DBENCH: 29.9 bytes per guest insn (was 33.6).
* Tests: paged harnesses place guest memory like the Pico (2 MiB + 640 KiB);
  FUZZ_MAP maps 0x30000-0x4FFFF (DS/ES, not the stack). Silicon quick set
  passes; fuzz mismatches only at mapped-range edges (known limit).

## 2026-10-09 (l1) — flushes keep hot entries hot

v40 on the Pico: frame-buffer misses fell ~8x but the score fell (18.8 ->
12.8): the paged code is larger, the code buffer smaller (160 KiB), so
flushes rose to ~4 per 8 s, and each flush zeroed the tiering heat, so the
whole hot set was re-interpreted 128 times per entry (cold-insns 2.6M per
interval, ~half the time). Now a flush keeps entries that had reached the
threshold at the threshold (translated on their first dispatch) and only
resets the rest.

## 2026-10-09 (l) — paged guest memory (-DB86_PAGED, Thumb-2)

* Every guest access goes through `cpu->pt[512]`: host = nominal + delta of
  the nominal 4 KiB page. Generated code: UBFX/LDR/ADD after each EA (r11 =
  table; SS is no longer cached in a register). `b86_map_range` /
  `b86_unmap_range` move page ranges to another buffer (SRAM) and back;
  `b86_host()` translates for C users. Interpreter, decoder and REP helper
  (page-sized chunks) honour it. `cpu->rep_page_bytes[]` counts REP store
  bytes per page for the embedder's choice of pages.
* SMC line map indexed by UBFX(addr, 6, 15) in a 32 Ki table (hot bytes =
  fast table + 32 KiB); guest buffer must be 4 KiB aligned with
  ((mem >> 6) & 0x7FFF) + lines <= 0x8000. Stores into mapped frames hit
  zero entries when the buffer sits away from SRAM's index range;
  b86h_smc ignores addresses outside the guest buffer.
* Code in mapped pages is always interpreted; mapping refuses pages that
  hold translated code.
* Known limit (kept): a 16-bit access straddling a mapped range's first or
  last byte is not exact (frames need one guard byte).
* Tests: `make paged` builds sst_jit_t2p / fuzz_t2p; `FUZZ_MAP=1` runs each
  program with 256 KiB of its data segments mapped (interpreter reference
  mapped too). Silicon quick set passes; fuzz mismatches seen only at
  range edges (the known limit).

## 2026-10-09 (k1) — wide STOSW now opt-in

(k) froze 3DBENCH on the Pico after the splash screen (host runs were
fine). Until that is bisected, the 32-bit STOSW fill is built only with
`-DB86_REP_WIDE`; the default is the (j) byte-pair loop again.

## 2026-10-09 (k) — wide REP STOSW, memory-traffic instrumentation

* REP STOSW fast path fills with 32-bit stores (was two byte stores per
  word): a quarter of the bus/cache accesses for 3DBENCH's back-buffer
  clears and span fills.
* `-DB86_MEMTRACE` calls `b86_memtrace(linear, write)` for every interpreter
  data access (not instruction fetches); blitzBUS uses it with an XIP cache
  model to attribute misses to guest memory regions.
* `-DB86_COND_HISTO` also counts REP bytes by kind and ES segment.

Finding (3DBENCH, 50M benchmark instructions through a 16 KiB / 2-way /
8-byte-line cache model): 91% of guest-data misses are frame buffers -
62% the back buffer at 222D:0000 (clears, spans, copy source) and 29% the
copy into A000 - against 3% stack, 5% data segment. The next step is
SRAM-backed guest pages for those buffers (see blitzBUS README).

## 2026-10-09 (j) — fewer C round trips and dispatcher detours

3DBENCH host profile (Thumb-2/qemu, blitzBUS v37 IRQs): per 10M benchmark
instructions, C-level dispatches 300-340K -> 10-60K, flag materialization
calls ~350K -> 130-230K, condition calls 40-80K -> ~10K; qemu native time for
a 300M-instruction run 17.4 s -> 10.4 s.

* **Block splitting removed** (left as an off-by-default experiment,
  `split_mid`). Killing the block that covered a new mid entry freed no code
  space in the generation, cost a retranslation, and left every branch
  already chained to the killed block detouring through its dead stub into
  the C dispatcher until the next flush: that was most of 3DBENCH's 300K
  dispatches per 10M instructions.
* **Cold runs continue through not-taken branches.** Stopping the
  interpreter at every Jcc made each fall-through a dispatch target that
  heated up and became its own entry, cutting hot loops into chained pieces.
* **Flags without C calls:**
  - `flags_if_lazy`: CF consumers (ADC/SBB, RCL/RCR, CMC/CLC/STC) only call
    the materializer when a record is pending (`lz_kind`, plus `lz_ikind`
    where OF matters).
  - CMC/CLC/STC are NZCV producers (C = CF) and CMC a fusable carry consumer;
    CMC reads only CF (was ALLF).
  - Taken JB/JAE exits whose target needs only CF store the known CF directly
    (`synth`); block-end exits that need only CF take it from NZCV
    (`endsynth`). Both leave `lz_kind = LZ_NONE` for the next block's fast path.
  - SHR/SAR by 1 produce C for fused consumers.
  - JE/JNE/JS/JNS that cannot fuse (a Thumb-2 store check clobbered NZCV)
    test the in-block producer's `lz_res` inline (`zsrc`).
  - Lookahead follows the taken edge of one conditional branch instead of
    assuming everything is live there.
  - `materializes()` only forces an earlier producer's record inline when
    that producer's flags outlive the materializing instruction.
* **In-block forward branches** may skip plain stores (MOV r/m) too: the
  store's NZCV clobber cannot matter after the join, because pass A never
  fuses across it on the fall-through path.

Debug: `-DB86_COND_HISTO` also records C-dispatch targets (`b86_disp_ip`)
and block exit reasons (`b86_xr`).

Verified (qemu): silicon 585,933/585,933 on both backends; fuzz 0 mismatches
(Thumb-2 and AArch64, incl. tiering and NOSPEC); blitzBUS v37/v38 DOS2TEST
25/25 + MDSTRESS 0xA298 on both ISAs, tiering on and off; 3DBENCH reaches
its score screen.

## 2026-10-09 (i) — 3DBENCH: code-cache thrash and interpreter fallbacks

Host reproduction of 3DBENCH 1.0 (blitzBUS bb_live + bb_vga, Thumb-2 under
qemu, Pico table sizes, 192 KiB code buffer): 20-100 full flushes per 10M
guest instructions. The benchmark touches only ~5,500 distinct instructions,
but each cache generation held ~6,900 translated instructions of which only
~4,000 were distinct, at ~27 bytes of Thumb-2 each; with an unlimited buffer
the whole program is 2,233 blocks and never flushes. About 8% of benchmark
instructions also went through the interpreter helper (RCL r16,1 2.8M,
CMP [mem],r8hi 1.0M, RCR r8,1 1.0M, IDIV, XCHG r8).

Code cache:
* **Superblock joins**: decoding stops (and chains) at another block's entry
  or where it re-enters already-translated code (bitmaps `cov`/`ent`, one bit
  per guest byte, B86_CALLOC; cleared per block at flush).
* **In-block forward branches**: a forward Jcc whose target is later in the
  same superblock branches there directly when every skipped instruction is
  plain native code with no flag definition, NZCV clobber or C call; the
  fall-through path settles the retired count at the join.
* **Splits**: a new entry inside a live block kills that block so its
  retranslation joins there instead of keeping a second copy of the tail.
* **Tiering** (`b86_jit_set_hot_threshold`, default 0 = off): an entry point
  is interpreted until it has been dispatched N times in the current cache
  generation (decayed 4096-slot heat table, reset at flush). blitzBUS uses
  128. 3DBENCH, 250M insns: flushes 959 -> 6, translate 17.5 s -> 0.22 s,
  total qemu time 114 s -> 14 s; ~4% of instructions run interpreted
  (mostly DOS and one-time setup).

Native lowerings:
* RCL/RCR r,1 (AX..DI, AL..BL): carry-in from NZCV when fused (SHL/RCL
  chains), NZCV out for fused consumers (RCL: CF/OF, RCR: CF), CF/OF into
  ctx flags only when needed past them. `be_carry_from_bit` backend
  primitive; fused carry consumers keep their condition in `cmode`
  (fuzz seed 103 on AArch64 caught `mode` being overwritten when the
  consumer is itself a producer).
* CMP/TEST [mem],AH..BH on Thumb-2 (two scratches): the address register is
  released at the load; CMP feeding NZCV and a record works in place.
* IDIV r/m16 via SDIV + MLS (zero divisor / overflow -> interpreter INT 0).
* XCHG r8,r8. `be_sdiv` backend primitive.

Stats: `joins`, `inner_branches`, `splits`, `cold_runs`, `cold_insns`.
Test knobs: `b86_jit_set_no_join`, fuzz `FUZZ_NOJOIN`, `FUZZ_HOT=n`.
Debug: `-DB86_COND_HISTO` records the guest site of every cond/flags C call
(`b86_cond_ip`, `b86_flags_ip`); `-DB86_HELPER_HISTO` adds `b86_helper_ip`.

Verified (qemu): silicon 585,933/585,933 on both backends; fuzz 0 mismatches
on both backends incl. tiering and NOSPEC; blitzBUS DOS2TEST 25/25 + MDSTRESS
0xA298 on both ISAs with tiering on and off.

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

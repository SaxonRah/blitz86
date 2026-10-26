r72: SRAM tier for the hottest PJ blocks (on top of r71; base blitz86 8c166b6)
==============================================================================
Only src/i386_pj.inc changes (BB_PJ_HOT_TIER_V1); without b86_pj_attach_hot()
PJ behaves exactly like r71.

Why: the Pico ran the Descent demo at ~1.5 MIPS with ~1.2 XIP-cache misses per
protected-mode instruction. A 16 KiB 2-way/8-byte-line XIP cache model over the
demo window (harness, B86_XIPSIM) put 0.63 of 0.68 modelled misses per
instruction on PJ code fetched from PSRAM and only 0.05 on guest data. The
demo runs 5,206 distinct blocks (825 KiB of native code); the hottest 96 KiB
carry ~76% of the code misses (first order).

How:
  * a flat 32-bit block dispatched PJ_HOT_AFTER (32) times is translated again
    into the SRAM buffer; its PSRAM copy is overwritten by LDR PC,[PC] -> SRAM
    copy (chained predecessors follow it), marked dead for SMC/re-find
  * the SRAM buffer carries its own shared routines (tails, exits, chk,
    memtramp, flag writers, native lookup): BL/B reach is +-16 MiB
  * links only within a tier; crossing tiers returns to the dispatcher
  * full tier: emptied at once; trampolines become exits to C, table entries
    and link-log entries into the tier are dropped, page lists rebuilt
  * PSRAM ring eviction drops trampolines living in the evicted half
  * header grows to 32 bytes ([7] = dispatch count)

API:  int  b86_pj_attach_hot(B86Cpu *c, void *buf, size_t bytes)  (>= 32 KiB,
      after b86_pj_attach; last 4 KiB hold the trampoline list)
      void b86_pj_hot_stats(c, out[6]): promotions, resets, refused, used, cap,
      trampolines
Harness: --pj-hot-kb N ; -DB86_XIPSIM (cache model, chaining off) in the
r72 work tree only.

Results (harness):
  cache model, demo window, 96 KiB tier: 0.682 -> 0.189 misses per instruction
    (code 0.630 -> 0.159, data 0.052 -> 0.030)
  normal build, 768 KiB PSRAM + 96 KiB tier: 8,821 promotions and 19 tier
    resets over the whole 1.5G-instruction run; ~119 instructions per dispatch
Verification:
  pj_fuzz 0 mismatches; startup with 48 KiB tier + forced evictions: 160,181
  runs, 0 mismatches; stress (promote after 4 dispatches, 32 KiB tier, with a
  reset): 160,009 runs, 0 mismatches, both with the identical 10,760,042 result.
  startup -> menus -> View Demo -> DESCENT.DEM, 1.0G instructions with a 96 KiB
  tier (8,894 promotions, 19 tier resets): 17,442,819 runs checked, 0 mismatches
PJ state: 7,520 bytes with HASH=512 (fits the 8 KiB arena).

r72b (fix for the Pico crash right after the first promotion)
-------------------------------------------------------------
b86_pj_link() patched every linked stub to the PSRAM copy of chk. For a stub
inside the SRAM tier that BL is ~240 MiB away (BL reaches +-16 MiB), so the
first chained exit out of a promoted block jumped into the weeds and the Pico
hard-faulted. The harness never saw it because its two mmaps were close
together. Now:
  * the stub links to the chk copy of its own region
  * the harness maps the tier ~224 MiB away from the main buffer, like the Pico
  * -DB86_PJ_RANGE_CHECK traps any branch emitted out of range (it found this)
Re-verified with the far layout: startup stress (32 KiB tier, promote after 4,
with a reset) 160,009 runs 0 mismatches; menus -> demo with 96 KiB tier,
8,894 promotions, 19 resets: 17,442,819 runs, 0 mismatches.

r72c (after the first Pico run of r72b)
---------------------------------------
Pico, demo window: 1.72-1.91 protected-mode MIPS (r71: 1.36-1.51), roughly
0.8 XIP misses per instruction (r71: ~1.2). But the tier was emptied 7 times
per dump interval and every emptying killed the PSRAM copies, so 14-15K
blocks per interval were translated again (r71: 3K, then 7).
  * a tier reset now restores each PSRAM copy (its first 8 bytes are saved
    when the trampoline is written) unless its SRAM copy was killed by a code
    write; trampoline entries are 16 bytes, 512 of them (8 KiB)
  * the promotion threshold starts at 32 dispatches and doubles at every
    tier reset (max 512), so a full tier raises the bar instead of churning
Verification (far layout): stress (32 KiB tier, promote on first dispatch,
resets) 160,106 runs 0 mismatches; menus -> demo, 96 KiB tier: 2,047
promotions (was 8,894), 3 resets (was 19), 16,834,937 runs, 0 mismatches.

r72d (after the Pico run of r72c)
---------------------------------
Pico, r72c: recompilation fixed (new PSRAM blocks per demo interval 7,520 ->
1,534 -> 422; 4 tier resets in total), demo 1.72-1.86 MIPS, ~0.86 XIP misses
per instruction. Phases whose hot code fits the tier ran at 3.6-5.6 MIPS with
almost no misses. But the resets happened while loading and doubled the
promotion threshold to 512 for good, so the demo's hot code was barely
promoted (tier 17-44 KiB used of 89 KiB).
  * the threshold doubles only when the tier refilled quickly (< 512
    promotions since the last reset), otherwise it halves
  * every 64K dispatches it halves again while the tier is less than half full
  * promotion fires at count >= threshold (so a lowered threshold applies to
    blocks already past it); the count restarts after each attempt
Verification (far layout): stress 160,106 runs 0 mismatches; menus -> demo,
96 KiB tier: 4,767 promotions, 9 resets, 17,155,194 runs, 0 mismatches.

r72e (after the Pico run of r72d): measurement only
---------------------------------------------------
r72d did what it was meant to (tier 21-81 KiB used, 4,307 promotions) but the
demo stayed at 1.75-1.91 MIPS and ~0.89 XIP misses per instruction. So the
SRAM copy is no longer what limits speed, and the remaining misses need to be
located on the device rather than in the model.
  * b86_pj_set_xip_counters(c, &ctr_acc, &ctr_hit, &us_timer): charge every
    native run (by the tier it starts in) and every gap between runs
  * b86_pj_xip_split(c, out[12]): sram runs/insns/misses/us, psram the same,
    gaps count/misses/us, dropped samples (counter cleared or saturated)
No change to emulation; with no counters attached the cost is one test per run.

r72f (after the Pico run of r72e): promote loops by time
--------------------------------------------------------
r72e on the Pico, Descent demo intervals:
  SRAM runs  ~61% of PM instructions, 2.9 MIPS, 0.31-0.38 XIP misses/insn
  PSRAM runs ~39% of PM instructions, 2.0 MIPS, 1.05-1.10 misses/insn,
             ~250 instructions per run (loops chained inside PSRAM)
  gaps       16-18% of wall time
Dispatch counts only see blocks entered from C. A loop chained in PSRAM is
entered once and never reaches the bar, however much time it takes.
  * b86_pj_pc_sample(pc): interrupt-safe; the host calls it from a timer
    interrupt with the interrupted PC. Samples are classified (PSRAM blocks,
    PSRAM shared routines, SRAM tier, other SRAM, flash, other) and PCs in
    PSRAM blocks are queued (64-entry ring)
  * b86_pj_pc_stats(out[8]): the class counts, [7] = promotions from samples
  * granule index in the PSRAM tail (2048 words): first block per granule,
    maintained on translate / half eviction / flush; maps a PC to its block
  * drained at the start of b86_pj_try (flat, non-PM16 only): a block with
    PJ_PCS_HOT (3) samples within one 2048-sample epoch, scaled by the
    adaptive dispatch bar (hot_after/32, max 255), moves to the SRAM tier
  * hd[7]: bits 0-19 dispatch count, 20-27 sample count, 28-31 epoch
Harness (qemu, SIGALRM sampler every 200 us, 900M insns menus -> demo):
                       PSRAM-block samples  SRAM-tier samples  total samples
  no sample promotion        2,800,018           312,331         4.86M
  r72f                         277,430           399,659         2.05M
  (19,413 promotions, 40 resets; total samples ~ wall time)
Verification (far layout, sampling on so promotions happen at arbitrary points):
  stress (32 KiB tier, promote-after-1, 100 us sampling): 159,986 runs, 0 mismatches
  menus -> demo 1.0G insns, 96 KiB tier: 16,827,609 runs, 0 mismatches,
  22,994 promotions (21,278 from samples), 59 resets

r73b (after the Pico run of r73)
--------------------------------
Pico r73: Descent halted at ~111M instructions (DOS/4GW dispatched to
0268:00000005). r72f stopped at a similar point (back at the DOS prompt).
Both builds had the 20 kHz core-0 PC sampler; r72e (no sampler) ran on.
  * the sample drain no longer touches block headers unless sample promotion
    is on, and then only for a block the page lists confirm (a granule walk
    over stale headers could otherwise write into native code; not seen in
    the harness: 87,383 sampled lookups, 0 bogus)
  * blitzBUS bb386_pj_pcs_v3.py: sampler OFF by default ($env:BB_PCS='1' on)
Loading with r73 (real-mode cache kept), Pico, first PM entry -> PM entry
#256: 41.6 s (r72f) -> 31.7 s; real-mode flushes 1,949 -> 58.

r73c (after the Pico run of r73b)
---------------------------------
r73b with the sampler OFF still died at ~114M instructions (USB gone: host
crash). So the sampler is not the cause. What r73/r73b share and r72e lacks:
the real-mode cache kept across protected mode. Kept, the 128 KiB cache
filled and region eviction (r52) ran under DOS/4GW for the first time
(Pico r73b: 28 evictions, 367 blocks, 198 unchains in 7 s); before r73 the
cache was flushed on every return and never filled.
  * once a program enters protected mode, a full real-mode cache is flushed
    whole (region eviction stays for pure real-mode programs)
  * the r72f sample index and drain are compiled out (B86_PJ_PCS=0, default):
    the protected-mode translator is r72e's again (stress verify 160,106 runs,
    the same count as r72d/e, 0 mismatches)
  * b86_pj_pc_sample_lr / b86_pj_pc_top_lr: caller (LR) profile
Harness (--jit --pj, 128 KiB real-mode cache): 55 flushes, 1,712 returns
kept the cache, identical final screen to the flushing build.

r73d (A/B build after r73c)
---------------------------
r73c (translator = r72e, sampler off, real-mode cache kept, region eviction
off after the first PM return) still died at ~117M instructions. The r73b
crash left a hard-fault record at the next boot: stacked PC 11724BDC, inside
the PSRAM protected-mode code buffer (116FA000 + 0x2ABDC).
r73d = r73c with B86_RM_KEEP_ON_PM 0 (now in include/b86.h, so interp.c and
jit.c agree): the real-mode cache is flushed on every PM return and smc()
checks linear addresses, exactly as in r72e. Harness: 1,767 flushes on PM
return, 0 kept.

r73e (baseline after r73d)
--------------------------
r73d (real-mode keep off) did not hard-fault, but at ~113M instructions the
SD layer started failing every access to DRIVE_C.IMG (FatFs FR_INVALID_OBJECT
on FAT sectors 610/90/342, then writes), DOS saw disk errors and Descent fell
back to the prompt behind the mode-13h screen. Every build since r72f has
died at 99-117M instructions, each time differently (DOS prompt, guest halt,
hard fault in PSRAM code twice at 11724BDx, SD failure): consistent with a
host memory overwrite triggered at one point of Descent's loading, or with a
damaged disk image.
r73e: sampler tables compiled out (B86_PJ_PCS=0 now also shrinks the ring and
heavy-hitter tables to 4 entries: SRAM back to the r72e size). Stress verify:
160,106 runs, 0 mismatches.

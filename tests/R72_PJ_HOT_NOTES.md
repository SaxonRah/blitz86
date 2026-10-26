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

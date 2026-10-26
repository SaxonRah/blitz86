r71: PJ translation-churn fixes and smaller code (base: blitz86 8c166b6)
=======================================================================
Only src/i386_pj.inc changes; tests/dos4gw_host.c gains harness options.

Why: on the Descent demo PJ was retranslating constantly (on device ~108K
translations per 37M instructions). Causes found in the harness:
  * every switch flat <-> 16-bit DOS/4GW code flushed the whole cache
    (3,450 flushes in the 10.7M-instruction startup);
  * the 256-entry code-page set (linear + physical keys) overflowed with
    Descent's ~300 code pages and flushed everything;
  * direct-mapped EIP tables: colliding hot blocks were recompiled on every
    alternation instead of being found again;
  * every store into a code page (the renderer patches its own span loops)
    flushed everything;
  * a full buffer flushed everything, hot code included.

Changes (markers r71 / BB_PJ_*_V1 in i386_pj.inc):
  BB_PJ_PRECISE_SMC_V1     block headers + per-page block lists; a store kills
                           only overlapping blocks (entry patched to exit to C)
  BB_PJ_PM16_CTX_CACHE_V1  PM16 blocks in 4 per-segment tables (512 entries);
                           no flush on flat <-> PM16 transitions
  BB_PJ_BLOCK_REFIND_V1    hash miss -> look in the page list before translating
  BB_PJ_RING_EVICT_V1      two-half ring: when full, evict the other half,
                           undo links into it (link log), rebuild page lists
  code-page set            2048 keys, moved after the code buffer (B86_PJ_CPS_KEYS)
  F2 MOVS/STOS             treated as REP (Watcom memcpy at 100DFC98)
  STI                      native (IF=1, IOPL checked; IRQs at next chain point)
  code size (BB_PJ_JCC_FUSE_V1, BB_PJ_MEMCALL_SHORT_V1)
                           16-bit MOV; STRD for lazy-flag pairs; a Jcc/SETcc right
                           after CMP/SUB (8/16/32-bit), ADD32, logic/TEST
                           (8/16/32) or INC/DEC32 uses the ARM flags directly;
                           memory call 16 -> 10 bytes. 37.4 -> 32.7 B per x86
                           instruction; JE 30 -> 9 bytes.
  BB_PJ_FETCH_PAGE_V1      entry from C walks the block's code page like a
                           real fetch: paged-out page -> interpreter raises the
                           #PF (DOS/4GW pages it in); present page gets its
                           Accessed bit, so hot code is not seen as cold
  BB_PJ_SHORT_STUB_V1      linkable static exits 20 -> 14 bytes
                           (BL exit_s/chk; .word eip; .hword n; B.W target)
  BB_PJ_LAZY_CALL_V1       lazy-flag record stores moved to 22 shared writers
                           (r2 = a, r3 = res): one 4-byte BL per ADD/SUB/logic/
                           INC/DEC producer instead of 16-24 bytes inline
                           Generated code: 37.4 -> 25.4 bytes per x86 instruction
  verifier                 snapshots segments, replays by retired count (REP
                           chunks), refuses device I/O, prints block bytes
  counters                 b86_pj_flush_causes(c, out[10]): flushes, smc, full,
                           pm16 context allocations, code-page overflow, smc
                           stores, blocks killed, refinds, evictions, unlinks

SRAM buffers (BB_PJ_PSRAM=0, e.g. Stage6D SRAM128): smaller side tables
(512 code-page keys, 128-entry PM16 tables, 1024 links, ~18 KiB) leave
~110 KiB of a 128 KiB buffer for code. Verified at 128 KiB: startup with 6
evictions, 159,302 runs, 0 mismatches.
Code buffer tail (PSRAM on Pico, outside the emitter): negative cache 2 KiB,
code-page set + list heads 16 KiB, PM16 tables 16 KiB, link log 64 KiB.
PJ state (SRAM) is 7,224 bytes with HASH=512 (was 8,160).

Verification (qemu-arm):
  * pj_fuzz: 0 mismatches over several 5-6K-sequence seeds after each change
  * Descent startup with forced evictions (200 KiB buffer): 159,343 runs,
    0 mismatches, identical 10,760,042-instruction result
  * startup -> menus -> View Demo -> DESCENT.DEM, 1.0G instructions, 246
    evictions, 1,067 kills: 15.5M runs checked, 17 reported mismatches; the
    ones inspected are replay page faults on code pages DOS/4GW had paged out
    (native code does not fetch, so it does not fault). Not all 17 inspected.

Harness options:
  --shot N            write shot_N.ppm (mode 13h) at N instructions
  --stats-from N      reset counters at N (measure only the demo)
  --pj-kb N           PJ code buffer size in KiB
  -DB86_PJ_CPROF      code bytes per x86 opcode + C-executed opcode histogram
Demo key script: --key 390000000:19 --key 393000000:17 --key 396000000:2E
  --key 399000000:18 --key 402000000:1C --key 700000000:1C, Down (50) five
  times from 760000000 every 4000000, --key 790000000:1C --key 830000000:1C

Demo window (900M-1.5G instructions, harness):
  before r71, 490 KB buffer:  ~1.14M translations
  r71, 1008 KiB buffer:       81.6K translations, 39 half-evictions,
                              24K self-modifying-code kills, 289K re-finds
On the Pico: shrink the unused real-mode JIT metadata and give PJ the space
(blitzBUS scripts\bb386_meta_kb_v1.py):
    $env:BB_META_KB = '656'; $env:BB_PJ_CODE_KB = '1008'
Final build verification (qemu-arm):
  startup, forced evictions: 159,345 runs, 0 mismatches, same 10,760,042 result
  startup -> menus -> View Demo -> DESCENT.DEM, 1.0G instructions, 209
  evictions, 1,212 kills: 15,756,811 runs checked, 0 mismatches (replay page
  faults on paged-out code pages are reported separately, not as mismatches)

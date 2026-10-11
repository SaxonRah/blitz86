# blitz86 unified hardware v5 — HID preflight PowerShell quoting fix

The v4 Pi preflight used `python -c` with nested quotes, which PowerShell passed incorrectly, producing a Python SyntaxError. This package replaces it with an actual Python script and preserves the existing microDOS v29 HID / rpiboot runner.

Extract **both** `b86_hardware.ps1` and `tools/b86_hid_preflight.py` into `C:\blitz86_v2`, maintaining the `tools` folder.

```powershell
cd C:\blitz86_v2
Unblock-File .\b86_hardware.ps1
.\b86_hardware.ps1 pi
```

To independently diagnose bridge discovery:

```powershell
python .\tools\b86_hid_preflight.py
```

Result exit codes: 0 found, 12 not found, 13 missing hidapi, 14 USB enumeration error. The Pi runner will then stream rpiboot/HID output and check its blitz86 PASS marker. No firmware or CMake changes. Windows execution is not verified here.

## r52: region eviction (code cache as a ring)

When the code buffer fills, blitz86 no longer flushes every block. It reclaims
the oldest 16 KiB chunk: its blocks are killed and their slots recycled, and
every patch aimed into the chunk (chain exits, RET caches, record branches)
is undone from a patch log, so surviving code falls back to the dispatcher.
`-DB86_EVICT=0` restores whole-cache flushes; the AArch64 backend always
flushes. Host 3DBENCH (232 KiB cache, threshold 128): cold-interpreted
instructions 8.1M -> 3.8M, flushes 3 -> 0, qemu time 12.1 s -> 10.2 s.

r52c: in eviction mode a translation that hits an internal limit (64 side
exits / SMC slow paths) or runs out of free space is discarded and retried
(smaller, or after reclaiming one more chunk) instead of flushing the whole
cache. A RET record left by a discarded attempt is dropped too.

r52d: with B86_OPT_PAGE_ZERO, b86_map_range flushed the whole cache on every
mapping. Now it flushes only if live code skipped page lookups (compiled while
no page was mapped); CS_CONST blocks (zero delta baked for CS-absolute
accesses) are killed individually when their code segment overlaps the new
mapping. 3DBENCH: the back-buffer mapping no longer flushes (host: flushes
2 -> 1, the remaining one is the mode-13h A000 mapping at startup).

r52e: fewer C helper round trips on Thumb-2 (two scratch registers):
POP r/m16 and CALL reg are native; ADC/SBB with a memory destination use
the host carry flag (ADC/SBC) instead of a third temp; AND/OR/XOR [mem], AH..BH
use a shifted register operand. 3DBENCH helper steps (excluding the INT 16h
keyboard poll): ~1.3M -> ~75K per 420M instructions. SST (8F, FF.2, ALU
families, 45k vectors) and fuzzers pass.

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

r52j: POP r16 (not SP) is a flag-free 3-instruction sequence (load indexed
from the SS base by uxth(SP + 2k)), and runs of consecutive POPs share one SP
update. Exact at the 64 KiB wrap and safe when a CMP/Jcc pair keeps its result
in NZCV across the pops (a first version with a flag-setting wrap guard broke
exactly that; the fuzzer caught it). Push runs were tried and dropped: with the
SMC range check and a fallback path the code grew.

r52k: fix the r52i/r52j regression on the Pico (50.0 -> 37.0). The inline REP
path stopped feeding rep_page_bytes, which blitzBUS uses to pick the 64 KiB
back buffer to move to SRAM: the buffer stayed in PSRAM (or a misaligned
window was picked) and PSRAM misses doubled. The inline path counts again
(7 instructions per REP, fast path only).

r52l: finer code-cache eviction. The ring used to reclaim 16 KiB at a time
and keep 16 KiB free before every translation, so up to ~32 KiB of the
232 KiB cache sat empty - and 3DBENCH's working set is right at the cache
size. Now 4 KiB chunks and a 2 KiB reserve (a block that does not fit
overflows, evicts another chunk and retries, as before). Host 3DBENCH at 420M
instructions: blocks evicted 4176 -> 848, guest code retranslated 45K -> 16K
instructions, translation time 345 -> 137 ms. Both are overridable
(-DB86_EVICT_CHUNK, -DB86_EVICT_ROOM).

Tried and dropped: a two-ring cache (nursery + tenured for blocks evicted
while still needed). Either ring alone is smaller than the working set, so
it thrashed: 3-7x more evictions than one ring.

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

r52m: fewer C calls for flags.
* A conditional jump whose producer is not in the block (typical: `call f;
  jae` where f returns CF from STC/CLC) tests the flag bit in ctx when no
  lazy record is pending, instead of calling b86h_cond.
* ADC/SBB right after ADC/SBB (32/48-bit arithmetic chains) gets its carry
  from the previous record inline (same formula as lazy_eval) instead of
  materializing all flags in C.
Host 3DBENCH (600M instructions): b86h_cond 6.70M -> 0.33M, b86h_flags
0.80M -> 0.35M. Remaining big one: IMUL's CF/OF (b86h_mulflags) before an
IDIV - left for later (the other flags IMUL leaves undefined would have to
match the interpreter exactly).

r52n: DS-absolute operands (globals, `mov ax,[1B8C]`) skip the page lookup.
Once any guest page is mapped to SRAM, every DS/ES access pays ubfx + table
load + add. A block with two or more absolute DS operands (before any DS
write or helper instruction) now starts with a guard: DS == the value at
translation. Under it, an absolute operand whose page is unmapped at
translation (and not the last byte of a page) is addressed directly. Guard
miss: the block is dropped and its entry is never baked again (b86h_dsmiss).
b86_map_range kills blocks that baked an access in a page being mapped.
Stats: ds_baked, ds_miss. -DB86_DS_BAKE=0 disables, -DB86_DS_BAKE_MIN sets the
operand threshold. Fuzz with FUZZ_MAP + B86_OPT_PAGE_ZERO and threshold 1:
~490 baked blocks, ~30 guard misses per 400 programs, no new mismatches (the
remaining FUZZ_MAP mismatches - seeds 189, 2108, 2575, 4056 - occur with the
pre-session JIT too).

r53: CPU model setting, 80186 instruction set (B86Cpu.model = B86_CPU_186).
Default stays B86_CPU_8086 (exact 8086: 60-6F are Jcc aliases, C0/C1 RET
aliases, 0F POP CS, shift counts unmasked). With model 186:
PUSHA/POPA, BOUND (INT 5; reg form INT 6), PUSH imm16/imm8, IMUL r,r/m,imm,
INS/OUTS (REP), shift/rotate r/m,imm8, ENTER/LEAVE, shift counts masked to 5
bits. Interpreter only for now: the JIT ends a block at these opcodes (and at
shift-by-CL) and the interpreter steps them; hot ones go native next.
Tests: tools/sst286_convert.py turns the SingleStepTests 80286 real-mode
vectors (moo2json output) into sst.bin; 286-only faults (segment limit) are
skipped. SST_MODEL=1 SST_A20=1 build/sst_interp: 28325/28326 (the one ENTER
miss reads memory the vector does not list); JIT runners: all pass. 8086
vectors, fuzzers (8086) unchanged; FUZZ_186=1 adds 186 instructions.

r53b: native 80186 paths (Thumb-2): PUSH imm16/imm8, LEAVE, shift by
immediate (C0/C1 SHL/SHR/SAR, flags dead) and shift by CL with the count
masked to 5 bits (register form). These decode to the unused op value 0xF1
(PUSH imm, LEAVE) or to D2/D3 with an immediate count, so no 8086 meaning of
60-6F / C0-C1 can leak into the analysis. Memory-form shift by CL, PUSHA/POPA,
IMUL imm, ENTER, BOUND, INS/OUTS still run in the interpreter. AArch64 keeps
them all in the interpreter. 186 vectors: 3865 of 24986 now native, all pass.

r54: 80286 real mode (B86Cpu.model = B86_CPU_286, includes the 186 set).
* 0F 01: SGDT/SIDT (6th byte FFh), LGDT/LIDT (24-bit base; LIDT moves the
  real-mode IVT), SMSW (FFF0h | MSW), LMSW (MP/EM/TS; setting PE -> #UD,
  protected mode is not implemented yet); 0F 06 CLTS; other 0F xx -> #UD.
* FLAGS bits 12-15 read 0 (PUSHF, interrupts); PUSH SP pushes the old SP.
* #UD (INT 6) for the 8086 alias forms: MOV sreg reg>3 / MOV CS,x; LEA, LES,
  LDS, CALL/JMP FAR with a register operand; 8F/C6/C7 reg != 0; FE reg > 1;
  FF /7.
* Divide error pushes the faulting IP; IDIV accepts -128 / -32768 quotients;
  AAA/AAS adjust AX as a whole; DAA/DAS carry/borrow from the +-6 step and
  test the old AL > 99h.
* Not modelled: 286 segment-limit faults in real mode (word access at offset
  FFFFh -> #GP/#SS); the converter skips those vectors.
SingleStepTests 80286 v1_real_mode (300 per opcode): 94427/94427 in the
interpreter, 83809/83809 through the JIT (the rest wrap-dependent). 8086
suite 61600/61600 unchanged; 186 28325/28326. FUZZ_286=1 for fuzzing.

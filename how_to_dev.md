I'm reading through the five repos to get the chain's roles and build flow right, and I still need the blitz86 design doc plus the microrender and microwave build docs. Then I'll write the guide.

Here's the guide, built from the five repos as they stand after the Oct 28 "unify" commits and from our earlier chats. I read the READMEs and design docs but didn't build anything, so check the commands against your tree.

## What each layer is

| Layer | Role | Runs on |
|---|---|---|
| **blitz86** | CPU core. Translates 8086 through 286 and 386 real mode to Thumb-2 (RP2350) or AArch64 (Pi Zero 2 W). The interpreter is the oracle and the fallback. | Host, QEMU, Pico, Pi |
| **blitzBUS** | Pico Plus 2 integration: PSRAM, SD and FAT16 disks, SPI LCD, VGA/PIT/PIC and BIOS adapters, firmware, and the live-run scripts. | RP2350 |
| **microDOS** | DOS runtime and system loop (MS-DOS 2.0 and the DOS 6.22 path). It also has its own interpreter, AOT and JIT engines. | Host, Pico, Pi |
| **microrender** | 16-bit DOS software renderer (Open Watcom, Mode X). Also builds for Pico and Raylib. | Guest workload |
| **microwave** | Audio mixer, the same shape as microrender. | Guest workload |

Your arrow order reads as a dependency chain. The code splits into two halves:

- **The bottom three are the platform.** blitz86 executes x86, microDOS supplies DOS semantics, and blitzBUS binds them to the hardware.
- **The top two are guest software.** microrender and microwave don't reference blitz at all. They're standalone C99 libraries that build their own DOS executables, which then run on the platform. They also act as the performance and correctness workloads for it.

## Working-tree layout

Keep three separate trees, since the scripts expect them:

```
C:\blitzBUS
C:\microDOS
C:\blitz86_v2
```

microrender and microwave live anywhere; they don't depend on the others.

## Per-repo dev loop

**blitz86** (host first, hardware last)
- `make all` builds the interpreter, the JIT runners for both backends and the fuzzers. `make test` / `make quick` runs them.
- Run the AArch64 and Thumb-2 test binaries under `qemu-aarch64` and `qemu-arm`.
- Correctness gates are the SingleStepTests vectors and the fuzzers, with the interpreter as reference. `make paged` covers the SRAM-mapped memory variants, and `make hwbench` covers on-device benchmarks.
- Rule from DESIGN.md: translate everything and reject nothing. A missing native lowering falls back to a single-instruction interpreter call, so coverage changes speed, never behavior.
- Treat any fuzz mismatch as a bug until you've shown the interpreter is the wrong one.

**microDOS**
- Host first: `.\md.bat deps msdos`, `.\md.bat build host`, `.\md.bat test`, `.\md.bat run dos2`. `DOS2TEST.COM` is the full-system regression.
- Pico firmware: `.\md.bat build pico`, which needs Pico SDK 2.3.0.
- Pi Zero 2 W: use the `md_pi0w_*.ps1` scripts.
- Rule: any fast path must return to the canonical runtime at an exact guest CS:IP when its assumptions break.

**blitzBUS**
- `bb.bat install | upgrade | restore | build | run`, or the `scripts\bb_live_run*.ps1` runners.
- Your standard benchmark loop:
  ```powershell
  cd C:\blitzBUS
  $env:BB_LCD_PERI_HZ = "150000000"
  $env:BB_LCD_SPI_HZ  = "75000000"
  $env:BB_LCD_LACE    = "1"
  .\scripts\bb_live_run.ps1 -MicroDOS C:\microDOS -Blitz86 C:\blitz86_v2 -Port COM5 -Seconds 180
  .\scripts\bb_pc_report.ps1
  ```
- DOS 6.22 and Descent runs go through the `bb_live_run_dos622_*.ps1` scripts. The IRQ0 trial runner is experimental.

**microrender**
- All builds go through `.\mr.bat`.
- Start with `.\mr.bat build tests`, then `.\mr.bat test`, which runs six CTest entries (unit, game, four fuzz seeds).
- DOS build: `$env:WATCOM='C:\WATCOM'; .\mr.bat build dos mode=both tile=16 vsync=0`. This produces `mrender.exe`, `mraw.exe`, `mstress.exe`, `msraw.exe`.
- Pico: `.\mr.bat build pico game` or `pico stress-lace ...`. Desktop: `.\mr.bat build raylib`.
- Check DECISIONS.md before trying an optimization. It records the ones that failed on real hardware.

**microwave**
- The entry point is `.\mw.bat`: `build`, `run`, `test`, `bench`.
- `.\mw.bat test all` runs unit and fuzz tests under ASan and UBSan across every sample format.
- The key invariant is block-size invariance: output must be identical sample for sample at any buffer size.
- DOS build needs `WATCOM`, and `run dos` uses DOSBox.

## Cross-chain workflow

1. **CPU change in blitz86.** Get it green on the host (SST, fuzz, interpreter vs JIT) and under QEMU for both ISAs. Then run microDOS `md.bat test` as the end-to-end DOS check. Only after that, flash through blitzBUS.
2. **Integration change in blitzBUS or microDOS.** Test on host where you can. Then do a live run and compare the score and `bb_pc_report.ps1` output against your last known-good numbers.
3. **Guest-side change in microrender or microwave.** Validate on host or Raylib, build the DOS exe with Watcom, and run it as a guest through the stack. A guest that behaves on the host but misbehaves on the Pico points at the platform, not the guest.
4. **Performance work.** Always measure on the Pico. Host MIPS and QEMU timings have misled you before: the interpreter beat the JIT on hardware until the XIP cache-miss fixes landed.

## Pitfalls

- **Flash status.** A successful compile doesn't mean the Pico is running your build, so confirm the image was actually flashed.
- **COM5.** Only one program can own it at a time. Run the log viewer in a separate window only when the runner isn't using the port.
- **IRQ failures.** Reboot the Pico after any IRQ-related guest-state failure. Don't enable continuous IRQ0 delivery.
- **Hidden state in blitzBUS.** The `.bb_*_state/` and `.bb622_*_state/` directories are required installer history. Don't delete them in a cleanup.
- **Disk images.** Back up `DOS622_HDD.IMG` before any run that writes to C:.
- **Version pairing.** blitzBUS and blitz86 releases were paired (for example blitzBUS v40 with blitz86 (l)). After pulling one, check the other still matches.
- **Memory.** SRAM is nearly full (98.54% earlier). If the link overflows RAM or you see `fail=jit-create`, try `BB_CODE_KB=144`.
- **Repo clutter.** microDOS and blitz86 carry many `*_backup` directories and loose `r73*.patch` files. Fold them into history or clean them up before they confuse a build.

# blitz86

blitz86 is an x86 CPU core for ARM microcontrollers and SBCs. It covers the 8086,
80186 and 80286 in real mode, and the 386 in both real and protected mode. It has:

- a reference interpreter (`src/interp.c`, with the 386 parts in `src/i386_*.inc`);
- a dynamic translator with a **Thumb-2** backend for the RP2350 (Cortex-M33,
  Pimoroni Pico Plus 2) and an **AArch64** backend for the Raspberry Pi Zero 2 W;
- a separate native tier for 386 protected-mode code (`src/i386_pj.inc`, "PJ"),
  including 16-bit protected-mode code such as the DOS/4GW kernel.

The translator translates everything. An instruction without a native lowering
becomes a call to the interpreter for that one instruction, never a rejected
region. `DESIGN.md` explains the design and `CHANGES.md` records the history.

blitz86 runs DOS 6.22 and DOS/4GW programs such as Descent on the Pico through
**blitzBUS**. blitzBUS compiles `src/interp.c`, `src/jit.c`, `src/be_t2.c` and
`include/` straight from this checkout (default `C:\blitz86_v2`).

## Commands (Windows)

Everything starts from `b86.bat`:

```powershell
cd C:\blitz86_v2
.\b86.bat help
```

**Host development** uses MSVC for the native build and WSL for the cross and
qemu tests.

| Command | What it does |
|---|---|
| `doctor` | checks the toolchains and paths |
| `deps` | installs the WSL packages (asks for the sudo password) |
| `build` | compiles only |
| `quick` | a short subset of the tests |
| `test [-FuzzCount N]` | the full suite |
| `bench` | the instruction-count benchmark |
| `all` | everything; downloads the SingleStepTests vectors on first use |
| `clean` | removes the build folders (`-NoClean` keeps them where supported) |
| `arm-build` | cross-compiles the backends |
| `capture -Port COM5 -Baud 115200 -Seconds 120` | serial capture; needs `py -m pip install pyserial` |
| `flash -Uf2 FILE [-BootDrive E:]` | copies a UF2 to a Pico in BOOTSEL mode |
| `microdos` | microDOS `md.bat test all` |
| `compare` | microDOS and blitz86 tests side by side (separate workloads, not a speed ratio) |

**Standalone blitz86 probe on hardware.** This is a microDOS sidecar target,
independent of blitzBUS.

| Command | What it does |
|---|---|
| `sidecar` | installs the probe target into microDOS `pico\` |
| `hw doctor\|build\|flash\|capture\|run [-Port COMx] [-NoFlash]` | interpreter probe on the Pico |
| `jit-pico doctor\|build\|flash\|capture\|run` | JIT probe on the Pico |
| `hardware doctor\|pico\|pi\|all\|pico-capture\|pi-capture` | Pico and Pi Zero 2 W runners |
| `jit-hardware pico\|pi\|all` | JIT probe on the Pico and Pi |

The Pi path needs microDOS `md_pi0w_run.ps1` and the USB HID bridge. Check the
bridge with `python tools\b86_hid_preflight.py`: it exits 0 when the bridge is
found, 12 when it isn't, 13 when hidapi is missing, and 14 on a USB error.

`b86 config` shows the settings. They live in `b86.config.psd1`:

| Setting | Default | Meaning |
|---|---|---|
| `B86_MICRODOS` | `C:\microDOS` | microDOS checkout |
| `B86_PORT` | empty | Empty means each tool's own default: COM5 for the dev tools, auto-detect for `hw`/`jit-pico`. |

A variable already set in the window wins:
`$env:B86_PORT = 'COM7'; .\b86.bat hw run`. Logs go to `logs\`.

## Test suite (Linux or WSL)

```sh
sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf qemu-user
make sst       # once: download and convert the 8086 SingleStepTests vectors (~170 MB)
make quick     # a minute: a subset of everything
make test      # silicon vectors + fuzzer through both backends (FUZZ_N=...)
make bench     # host-instructions-per-guest-instruction table
make hwbench   # the microDOS Pico/Pi driver workloads under qemu
make paged     # Thumb-2 with paged guest memory (the RP2350 build's addressing)
```

| Test | What it covers |
|---|---|
| `tests/sst_interp.c`, `tests/sst_jit.c` | interpreter and translator against physical-8086 vectors |
| `tests/sst386.c` | 386 real-mode vectors (`tools/sst386_convert.py`); `tools/sst286_convert.py` handles the 286 set |
| `tests/fuzz.c` | random structured programs, translator vs interpreter; `FUZZ_TRACE=1` prints the first diverging block |
| `tests/t386u.c` | single 386 instructions, interpreter vs JIT: `t386u "66 01 E8" [iters]` |
| `tests/flags/*.c` | flag edge cases (link with `src/jit.c` and a backend) |
| `tests/bb_386_*` | 386 real-mode, protected-mode and extended-memory smoke tests (link with `src/interp.c`) |
| `tests/pj_fuzz.c` | protected-mode native tier vs interpreter under `qemu-arm`. Modes 0–2 are flat 32-bit; modes 10–12 are 16-bit protected mode. Build command is in `tests/DOS4GW_HOST.md`. |
| `tests/pmfz/` | protected-mode fuzz templates (`tests/pmfz/README.md`) |
| `tests/dos4gw_host.c` | runs real DOS/4GW programs (Descent) on the host (`tests/DOS4GW_HOST.md`) |

`tests/R71_PJ_NOTES.md` and `tests/R72_PJ_HOT_NOTES.md` describe the
protected-mode tier and its SRAM hot tier.

Building with `-DB86_DEBUG_DUMP` writes every translated block to
`/tmp/blk_CS_IP.bin`. Disassemble them with
`objdump -D -b binary -marm -M force-thumb` or with `-maarch64`.

## Embedding

```c
static uint8_t guest[B86_MEM_BYTES] __attribute__((aligned(64)));  /* PSRAM ok */
static uint8_t code[192 * 1024] __attribute__((aligned(8)));       /* SRAM, executable */

B86Cpu cpu;
b86_init(&cpu, guest);
cpu.int_hook = my_int_hook;   /* return 1 to handle INT n in C (DOS/BIOS HLE) */
cpu.in8 = my_in;  cpu.out8 = my_out;
b86_jit_create(&cpu, code, sizeof code);
b86_set_seg(&cpu, B86_CS, 0x1000); cpu.ip = 0x100;
for (;;) {
    int r = b86_jit_run(&cpu, ~0ull);   /* returns on HLT or when cpu.irq != 0 */
    if (r == B86_EXIT) { cpu.irq = 0; service_timers_usb_video(); }
    if (r == B86_HALT) wait_for_interrupt();
}
```

- **Interrupt polling.** Set `cpu.irq` from a timer ISR. Translated code polls it
  on every backward branch and indirect jump.
- **Code writes from C.** Writes from C into guest memory that may hold
  translated code must go through `b86_jit_flush` or the SMC hook.
- **The full 386 path.** blitzBUS `src/bb_live.c` shows the real integration:
  paging, protected mode, the PJ tier and the PSRAM/SRAM code caches.

## Layout

| Path | Contents |
|---|---|
| `include/` | public API (`b86.h`), CPU state, x87 and protected-mode helpers |
| `src/interp.c`, `src/i386_*.inc` | interpreter; 386 real mode (`rm*`), protected mode (`pm*`), paging (`pg`) |
| `src/jit.c`, `src/backend.h`, `src/be_t2.c`, `src/be_a64.c` | translator, backend contract, Thumb-2 and AArch64 backends |
| `src/i386_pj.inc`, `src/b86_v30f_*` | protected-mode native tier |
| `tests/`, `tools/` | test programs; vector fetch/convert scripts, host-instruction counter, Windows CMake |
| `b86.bat`, `b86.config.psd1`, `scripts/` | Windows commands |
| `pico/`, `integration/` | standalone probe sources, copied into microDOS by `b86 sidecar` |
| `blitzOverview/` | project overview images |

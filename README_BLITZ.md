# blitz86

Fast 8086 / real-mode DOS CPU core for ARM microcontrollers and SBCs:

* **Thumb-2 backend** — RP2350 (Pimoroni Pico 2 Plus, Cortex-M33 @ 300 MHz)
* **AArch64 backend** — Raspberry Pi Zero 2 W (Cortex-A53 @ 1 GHz)
* **Reference interpreter** — passes all 614,000 SingleStepTests vectors
  captured from a physical Intel 8086

It is a dynamic binary translator that translates *everything*: an
instruction without a native lowering becomes an inline call to the
interpreter for that single instruction, never a rejected region. Guest
registers stay in host registers across chained blocks, x86 flags are fused
into host NZCV or recorded lazily based on liveness, and self-modifying code
is tracked at 64-byte granularity. See **DESIGN.md**.

## Status

| check | AArch64 | Thumb-2 |
|---|---|---|
| interpreter vs 8086 silicon | 614,000 / 614,000 | (shared) |
| translator vs silicon, 1 insn/block | 585,933 / 585,933 | 585,933 / 585,933 |
| random structured programs vs interpreter | 4,000 / 4,000 | 4,000 / 4,000 |
| host insns per guest insn, native kernels | 3.6 – 8.2 | 4.2 – 9.6 |

All numbers come from qemu-user; nothing has run on hardware yet. DOS/BIOS
services, video and the Pico/Pi firmware are outside this repository
(the host plugs them in through the hooks below).

## Build and test (Linux or WSL)

    sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf qemu-user
    make sst          # once: downloads and converts the 8086 vectors (~170 MB)
    make quick        # a minute: subset of everything
    make test         # full silicon + fuzz on both backends
    make bench        # host-instruction table (uses qemu exec logs)

Debugging a fuzz mismatch: `FUZZ_TRACE=1 qemu-arm build/fuzz_t2 1 <seed>`
runs block-by-block against the interpreter and prints the first diverging
block, its bytes, entry state and the previous block. Build with
`-DB86_DEBUG_DUMP` to write every translated block to `/tmp/blk_CS_IP.bin`
(disassemble with `objdump -D -b binary -marm -M force-thumb` or
`-maarch64`).

## Embedding

```c
static uint8_t guest[B86_MEM_BYTES] __attribute__((aligned(64)));  /* PSRAM ok */
static uint8_t code[192 * 1024] __attribute__((aligned(8)));       /* SRAM, executable */

B86Cpu cpu;
b86_init(&cpu, guest);
cpu.int_hook = my_int_hook;   /* return 1 to handle INT n in C (DOS/BIOS HLE) */
cpu.in8 = my_in;  cpu.out8 = my_out;
b86_jit_create(&cpu, code, sizeof code);
b86_set_seg(&cpu, B86_CS, 0x1000); cpu.ip = 0x100; /* ... */
for (;;) {
    int r = b86_jit_run(&cpu, ~0ull);   /* returns on HLT or when cpu.irq != 0 */
    if (r == B86_EXIT) { cpu.irq = 0; service_timers_usb_video(); /* maybe b86_interrupt(&cpu, 8) */ }
    if (r == B86_HALT) wait_for_interrupt();
}
```

Set `cpu.irq` from a timer ISR; translated code polls it on every backward
branch and indirect jump. Code writes from C into guest memory that may
contain translated code must go through `b86_jit_flush` (or the SMC hook).

## Layout

    include/b86.h        public API, CPU state
    src/interp.c         reference interpreter
    src/jit.c            translator frontend + runtime
    src/backend.h        backend contract
    src/be_a64.c         AArch64 backend
    src/be_t2.c          Thumb-2 backend
    tests/sst_interp.c   interpreter vs silicon vectors
    tests/sst_jit.c      translator vs silicon vectors
    tests/fuzz.c         differential fuzzer (+ lockstep tracer)
    tests/bench.c        hot-loop kernels
    tools/               vector fetch/convert, host-instruction counter

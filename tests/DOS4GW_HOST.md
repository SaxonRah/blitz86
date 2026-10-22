# dos4gw_host: DOS/4GW programs on the host

Interpreter (x86-64 host):

    gcc -O2 -DB86_RAM_BYTES=0x400000u -Iinclude src/interp.c tests/dos4gw_host.c -o build/dos4gw_host

Thumb-2 JIT exactly as the Pico runs it (paged, under qemu-arm):

    arm-linux-gnueabihf-gcc -static -mthumb -march=armv7-a+fp -O2 -DB86_PAGED -DDOS4GW_JIT \
        -DB86_RAM_BYTES=0x400000u -Iinclude src/interp.c src/jit.c src/be_t2.c tests/dos4gw_host.c \
        -o build/dos4gw_host_t2p
    qemu-arm build/dos4gw_host_t2p DIR DESCENTR.EXE --jit

DIR plays C:\ . Options: --steps N, --trace FROM COUNT (per-instruction
registers, interpreter mode), --trace-int (every BIOS/DOS call), --trace-pe,
--env NAME=VALUE, --pico-ivt (leave BIOS vectors 10h-1Ah at 0000:0000 like
blitzBUS did before BB622_BIOS_STUBS_V1). On exit or stop it writes
dos4gw_mem.bin (guest RAM) for objdump -b binary -m i8086/i386.

Bind Descent first (DESCENT.UBN from the ARJ volumes):

    build/dos4gw_host DIR 4GWBIND.EXE 4GWPRO.EXE DESCENT.UBN DESCENTR.EXE

Status (r64): DOS/4GW Professional 1.97 + Descent run through startup in
both modes with identical DOS call sequences, then Descent exits:
"You need 4718592 more bytes of extended/expanded memory" (needs 7.5 MB;
3 MB extended). DOS4GVM=1 enables the VMM, which needs 386 paging (CR3).

r67/r68 additions
-----------------
    --irq                 PIT channel 0 + master PIC; IRQ0 (and IRQ1 for --key)
                          delivered through b86_irq() on a virtual clock
    --vmips N             virtual clock in guest MIPS for the PIT (default 25)
    --key INSN:SC         queue scancode SC (hex) at retired instruction INSN;
                          the break code follows 300k instructions later
    TRACE_DPMI=1          log INT 31h calls made from 32-bit code and results
    screen.ppm            written at exit when the guest is in mode 13h

DOS model fixes: handles go through the current PSP's job file table
(DOS/4GW copies JFT entries between PSPs for its swap file), a zero-length
write sets the file size, and the stub BIOS/DOS services return with IF=1.

Descent with virtual memory, timer and a typed pilot name:

    build/dos4gw_host DIR DESCENTR.EXE --irq \
        --env "DOS4GVM=VIRTUALSIZE#32768 SWAPNAME#C:\DESCENT.SWP" \
        --key 390000000:19 --key 393000000:17 --key 396000000:2E \
        --key 399000000:18 --key 402000000:1C --steps 650000000

PM40 verifier build (runs every PM40 instruction, undoes it, reruns it
through the stage chain and compares registers, EIP, defined flags,
retired count and written memory):

    gcc -O2 -DB86_PM40_VERIFY -DB86_PM40_REP=64u -DB86_RAM_BYTES=0x400000u \
        -Iinclude src/interp.c tests/dos4gw_host.c -o build/dos4gw_vfy

r68b: PM40 also covers 16-bit protected-mode code and 16-bit addressing
(DOS/4GW's own kernel), segment-register moves/push/pop, LES/LDS/LFS/LGS,
PUSHA/POPA, PUSHF, ENTER (level 0), LOOP/JCXZ, BT/BTS/BTR/BTC, CLI/STI.
-DB86_PM40_STATS prints which opcodes PM40 still declines.

r70: PJ, native Thumb-2 translation of flat 32-bit protected-mode code
---------------------------------------------------------------------
src/i386_pj.inc, built with -DB86_PJ=1 on 32-bit ARM hosts (Pico, qemu-arm).
The host attaches it with b86_pj_attach(cpu, state, b86_pj_state_bytes(),
code, code_bytes). Blocks of supported instructions run natively with the
guest registers in r4-r11, write the interpreter's lazy-flag records, call
one helper per memory access (page walk, fault -> exit before the
instruction), chain to each other (direct for static targets, a native
hash lookup for RET/indirect), and stop at pending interrupts, written code
pages or after B86_PJ_CHAIN instructions.

    # Descent under qemu-arm with PJ
    arm-linux-gnueabihf-gcc -static -mthumb -march=armv7-a+fp -O2 -DB86_PAGED \
        -DB86_PJ=1 -DB86_RAM_BYTES=0x400000u -Iinclude src/interp.c \
        tests/dos4gw_host.c -o build/pj_t2
    qemu-arm build/pj_t2 DIR DESCENTR.EXE --pj --irq --env "DOS4GVM=..."

    # verifier: every native run is undone and replayed in the interpreter
    ... -DB86_PJ_VERIFY ...   (prints [pj-verify] MISMATCH lines)

    # random differential fuzzer (single blocks vs the interpreter)
    arm-linux-gnueabihf-gcc -static -mthumb -march=armv7-a+fp -O1 -DB86_PJ=1 \
        -DB86_PJ_STR_MAX=64u -DB86_RAM_BYTES=0x400000u -Iinclude -Isrc \
        tests/pj_fuzz.c -o build/pj_fuzz
    qemu-arm build/pj_fuzz 6000 <seed>

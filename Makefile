# blitz86 build and test. Cross toolchains + qemu-user run both backends on
# any Linux/WSL host:  apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf qemu-user
#
#   make test-interp     interpreter vs physical-8086 vectors (host gcc)
#   make test            silicon vectors + fuzzer through both backends (qemu)
#   make bench           host-instructions-per-guest-instruction table
#   make sst             download + convert SingleStepTests (once, ~170 MB)

SST     ?= sst
FUZZ_N  ?= 1000
PJ_FUZZ_N ?= 2000
PJ_QUICK_N ?= 200
PJ_SEEDS ?= 1 7 85
PJ_MODES ?= 0 2 12
CFLAGS  ?= -O2 -Wall -Wextra -Winit-self
INC      = -Iinclude
CORE     = src/interp.c src/jit.c
A64CC   ?= aarch64-linux-gnu-gcc -static
T2CC    ?= arm-linux-gnueabihf-gcc -static -mthumb -march=armv7-a+fp
QA64    ?= qemu-aarch64
QT2     ?= qemu-arm
B        = build

all: $(B)/sst_jit_t2p $(B)/fuzz_t2p $(B)/sst_interp $(B)/sst_jit_a64 $(B)/sst_jit_t2 $(B)/fuzz_a64 $(B)/fuzz_t2 $(B)/bench_a64 $(B)/bench_t2 \
     $(B)/hwbench_a64 $(B)/hwbench_t2

$(B):
	mkdir -p $(B)

$(B)/sst_interp: src/interp.c tests/sst_interp.c tests/sst.h include/b86.h | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ src/interp.c tests/sst_interp.c

$(B)/%_a64: tests/%.c $(CORE) src/be_a64.c src/backend.h include/b86.h | $(B)
	$(A64CC) $(CFLAGS) $(INC) -o $@ $(CORE) src/be_a64.c $<

$(B)/%_t2: tests/%.c $(CORE) src/be_t2.c src/backend.h include/b86.h | $(B)
	$(T2CC) $(CFLAGS) $(INC) -o $@ $(CORE) src/be_t2.c $<

# Paged guest memory (B86_PAGED) on Thumb-2: the RP2350 build's addressing.
$(B)/%_t2p: tests/%.c $(CORE) src/be_t2.c src/backend.h include/b86.h | $(B)
	$(T2CC) $(CFLAGS) -DB86_PAGED $(INC) -o $@ $(CORE) src/be_t2.c $<

# PJ uses Thumb-2 emitter code built into interp.c, not the generic
# jit.c/backend test binary. Keep O1 and RAM size aligned with the documented
# DOS4GW_HOST.md differential fuzz invocation.
PJ_DEPS = tests/pj_fuzz.c src/interp.c src/i386_pj.inc $(wildcard src/i386*.inc src/b86_v30f_* include/*.h)
$(B)/pj_fuzz_t2p: $(PJ_DEPS) | $(B)
	$(T2CC) $(CFLAGS) -O1 -DB86_PAGED -DB86_PJ=1 -DB86_PJ_STR_MAX=64u -DB86_RAM_BYTES=0x400000u $(INC) -Isrc tests/pj_fuzz.c -o $@

$(B)/pj_fuzz_t2p_nosplit: $(PJ_DEPS) | $(B)
	$(T2CC) $(CFLAGS) -O1 -DB86_PAGED -DB86_PJ=1 -DB86_PJ_STR_MAX=64u -DB86_RAM_BYTES=0x400000u -DB86_PJ_V85_TIER_SPLIT=0 $(INC) -Isrc tests/pj_fuzz.c -o $@

# Reproducible seeds, both counter modes and PM32 / mixed / PM16 generators.
# A nonzero exit from any differential comparison makes the target fail.
pj-quick: $(B)/pj_fuzz_t2p $(B)/pj_fuzz_t2p_nosplit
	@set -e; for exe in $(B)/pj_fuzz_t2p $(B)/pj_fuzz_t2p_nosplit; do \
	  for mode in $(PJ_MODES); do for seed in $(PJ_SEEDS); do \
	    echo "PJ quick $$exe mode=$$mode seed=$$seed"; $(QT2) $$exe $(PJ_QUICK_N) $$seed $$mode; \
	  done; done; done

pj-test: $(B)/pj_fuzz_t2p $(B)/pj_fuzz_t2p_nosplit
	@set -e; for exe in $(B)/pj_fuzz_t2p $(B)/pj_fuzz_t2p_nosplit; do \
	  for mode in $(PJ_MODES); do for seed in $(PJ_SEEDS); do \
	    echo "PJ test $$exe mode=$$mode seed=$$seed"; $(QT2) $$exe $(PJ_FUZZ_N) $$seed $$mode; \
	  done; done; done

paged: $(B)/sst_jit_t2p $(B)/fuzz_t2p

sst:
	tools/fetch_sst.sh $(SST)

test-interp: $(B)/sst_interp
	$(B)/sst_interp $(SST)/all.bin

test: all pj-test
	$(B)/sst_interp $(SST)/all.bin
	$(QA64) $(B)/sst_jit_a64 $(SST)/all.bin
	$(QT2) $(B)/sst_jit_t2 $(SST)/all.bin
	$(QA64) $(B)/fuzz_a64 $(FUZZ_N) 1
	$(QT2) $(B)/fuzz_t2 $(FUZZ_N) 1

# the nine workloads of the microDOS Pico/Pi drivers, same logic, under qemu
hwbench: $(B)/hwbench_a64 $(B)/hwbench_t2
	$(QA64) $(B)/hwbench_a64
	$(QT2) $(B)/hwbench_t2

quick: all pj-quick
	$(B)/sst_interp $(SST)/quick.bin
	$(QA64) $(B)/sst_jit_a64 $(SST)/quick.bin
	$(QT2) $(B)/sst_jit_t2 $(SST)/quick.bin
	$(QA64) $(B)/fuzz_a64 200 1
	$(QT2) $(B)/fuzz_t2 200 1

bench: $(B)/bench_a64 $(B)/bench_t2
	@for k in 0 1 2 3 4 5 6 7 8 9; do tools/hostcount.sh $(QA64) $(B)/bench_a64 $$k 100 200; done
	@for k in 0 1 2 3 4 5 6 7 8 9; do tools/hostcount.sh $(QT2) $(B)/bench_t2 $$k 100 200; done

clean:
	rm -rf $(B)

.PHONY: all sst test-interp test quick bench hwbench clean pj-quick pj-test

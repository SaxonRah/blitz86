#!/bin/sh
# Steady-state host instructions per guest instruction, via qemu exec logs.
#   tools/hostcount.sh <qemu> <bench-binary> <kernel> [n1] [n2]
# "code" = instructions executed inside the translation buffer,
# "all"  = every guest-visible host instruction (C helpers, dispatcher too).
Q=$1; B=$2; K=$3; N1=${4:-200}; N2=${5:-400}
out() { $Q $B $K $1; }
G1=$(out $N1 | sed -n 's/.*guest=\([0-9]*\).*/\1/p'); G2=$(out $N2 | sed -n 's/.*guest=\([0-9]*\).*/\1/p')
LO=$(out $N1 | sed -n 's/.*buf=\(0x[0-9a-f]*\)-.*/\1/p')
h() { BENCH_NOREF=1 $Q -one-insn-per-tb -d exec,nochain $2 -D /tmp/hc.log $B $K $1 >/dev/null; grep -c '^Trace' /tmp/hc.log; }
C1=$(h $N1 "-dfilter $LO+0x100000"); C2=$(h $N2 "-dfilter $LO+0x100000")
A1=$(h $N1 ""); A2=$(h $N2 "")
NAME=$(out $N1 | awk '{print $1}')
python3 -c "
dg=$G2-$G1
print('%-8s code %6.2f   all %6.2f   host insns / guest insn' % ('$NAME', ($C2-$C1)/dg, ($A2-$A1)/dg))"

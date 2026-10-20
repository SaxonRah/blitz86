# Protected-mode differential fuzzer (blitz86 vs Unicorn)

    pip install unicorn
    gcc -O1 -Iinclude src/interp.c tests/pmfz/pm_run.c -o tests/pmfz/pm_run
    cd tests/pmfz && python3 pmfz.py <seed> <cases-per-template>
    CODE16=1 python3 pmfz.py ...     # 16-bit code segment (see note)

One instruction (or a short fixed sequence) per case, random registers,
flags and a 4 KiB data image; flat CPL0 descriptors. Registers, defined
flags (per-template undefined-flag masks) and the data image are compared.

Note: CODE16 mode still uses the 32-bit-oriented generators, so many
templates encode different instruction lengths in a 16-bit segment; its
numbers measure coverage only roughly until 16-bit generators exist.

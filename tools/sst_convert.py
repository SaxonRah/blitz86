#!/usr/bin/env python3
"""Convert SingleStepTests 8086 v1 JSON vectors to a compact binary file
for tests/sst_run.c.  Only 'normal' and 'alias' opcodes are kept.

Record layout (little endian):
  u8  opcode, u8 reg(0xFF=none)
  u16 init[14]  (ax bx cx dx cs ss ds es sp bp si di ip flags)
  u16 fin[14]
  u16 flagmask
  u16 n_init, n_fin, n_ignore
  n_init  x (u32 addr, u8 val)
  n_fin   x (u32 addr, u8 val)
  n_ignore x u32 addr     (bytes whose final value is architecturally undefined)
"""
import gzip, json, struct, sys, os
REG = ["ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags"]
ARCH = 0x0FD5
OF = 0x0800

def mask_of(meta):
    v = meta.get("flags-mask")
    return 0xFFFF if v is None else int(v) & 0xFFFF

def main(src, dst, per_file):
    md = json.load(open(os.path.join(src, "metadata.json")))["opcodes"]
    out = open(dst, "wb")
    total = skipped = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".json.gz"): continue
        parts = fn.split(".")
        op = int(parts[0], 16)
        reg = int(parts[1]) if len(parts) == 4 else None
        meta = dict(md.get(parts[0], {}))
        if reg is not None:
            meta.update(meta.get("reg", {}).get(str(reg), {}))
        if meta.get("status", "normal") not in ("normal", "alias"):
            continue
        tests = json.load(gzip.open(os.path.join(src, fn)))[:per_file]
        for t in tests:
            ini = t["initial"]; fin = t["final"]
            ir = ini["regs"]; fr = dict(ir); fr.update(fin["regs"])
            cs, ip = ir["cs"], ir["ip"]
            ram = {a: v for a, v in ini["ram"]}
            # prefetch conflict: executed bytes differ from RAM at CS:IP
            bad = False
            for i, b in enumerate(t["bytes"]):
                a = ((cs << 4) + ((ip + i) & 0xFFFF)) & 0xFFFFF
                if a in ram and ram[a] != b: bad = True
            if bad: skipped += 1; continue
            fmask = mask_of(meta) & ARCH
            if op in (0xD2, 0xD3) and (ir["cx"] & 0xFF) <= 1: fmask |= OF
            ignore = []
            # Type-0 exceptions: stacked FLAGS undefined for DIV/IDIV/AAM
            if op in (0xF6, 0xF7, 0xD4):
                v0 = ram.get(0, 0) | (ram.get(1, 0) << 8)
                c0 = ram.get(2, 0) | (ram.get(3, 0) << 8)
                if fr["ip"] == v0 and fr["cs"] == c0 and fr["sp"] == ((ir["sp"] - 6) & 0xFFFF):
                    base = fr["ss"] << 4
                    ignore = [(base + ((fr["sp"] + 4) & 0xFFFF)) & 0xFFFFF,
                              (base + ((fr["sp"] + 5) & 0xFFFF)) & 0xFFFFF]
                    fmask &= 0xFFFF
            out.write(struct.pack("<BB", op, 0xFF if reg is None else reg))
            out.write(struct.pack("<14H", *[ir[k] & 0xFFFF for k in REG]))
            out.write(struct.pack("<14H", *[fr[k] & 0xFFFF for k in REG]))
            out.write(struct.pack("<HHHH", fmask, len(ini["ram"]), len(fin["ram"]), len(ignore)))
            for a, v in ini["ram"]: out.write(struct.pack("<IB", a, v))
            for a, v in fin["ram"]: out.write(struct.pack("<IB", a, v))
            for a in ignore: out.write(struct.pack("<I", a))
            total += 1
    print(f"wrote {total} vectors ({skipped} prefetch-conflict skipped) -> {dst}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2000)

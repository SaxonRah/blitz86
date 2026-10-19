#!/usr/bin/env python3
"""SingleStepTests 80386 v1_ex_real_mode (moo2json output *.json) -> s386.bin
for tests/sst386.c.  Usage: json_dir 80386.csv out.bin [per_file] [regex]
Record: 12-byte name, u32 init[18], u32 fin[18], u32 fmask, u16 ni, u16 nf,
ni+nf * (u32 addr, u8 val).  Register order: eax ecx edx ebx esp ebp esi edi
(b86 index order), es cs ss ds fs gs, eip, eflags."""
import csv, json, os, re, struct, sys
R = ["eax","ecx","edx","ebx","esp","ebp","esi","edi","es","cs","ss","ds","fs","gs","eip","eflags"]
def masks(csvf):
    m = {}
    for row in csv.DictReader(open(csvf)):
        key = row["op"] + ("." + row["ex"] if row["ex"] not in ("", None) else "")
        um = row.get("f_umask") or ""
        m[key] = int(um, 16) if um.startswith("0x") else 0xFFFF
        m.setdefault(row["op"], m[key])
    return m
def main(src, csvf, dst, per, rx):
    mk = masks(csvf); pat = re.compile(rx) if rx else None
    out = open(dst, "wb"); n = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".json"): continue
        name = fn[:-5]
        if pat and not pat.search(name): continue
        fm = mk.get(name, mk.get(name.split(".")[0], 0xFFFF))
        tests = json.load(open(os.path.join(src, fn)))
        tests = tests["tests"] if isinstance(tests, dict) else tests
        for t in tests[:per]:
            ir = t["initial"]["regs"]; fr = dict(ir); fr.update(t["final"]["regs"])
            out.write(name.encode()[:12].ljust(12, b"\0"))
            out.write(struct.pack("<16I", *[ir[k] & 0xFFFFFFFF for k in R]))
            out.write(struct.pack("<16I", *[fr[k] & 0xFFFFFFFF for k in R]))
            ri, rf = t["initial"]["ram"], t["final"]["ram"]
            out.write(struct.pack("<IHH", fm, len(ri), len(rf)))
            for a, v in ri: out.write(struct.pack("<IB", a, v))
            for a, v in rf: out.write(struct.pack("<IB", a, v))
            n += 1
    print(f"wrote {n} vectors -> {dst}")
if __name__ == "__main__":
    a = sys.argv
    main(a[1], a[2], a[3], int(a[4]) if len(a) > 4 else 200, a[5] if len(a) > 5 else None)

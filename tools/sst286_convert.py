#!/usr/bin/env python3
"""SingleStepTests 80286 v1_real_mode (moo2json output, *.json) -> the
sst.bin record format of sst_convert.py. Exception cases keep their stacked
FLAGS bytes ignored (undefined bits). Usage: src_dir metadata.json out.bin [per_file]"""
import json, struct, sys, os
REG = ["ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags"]
ARCH = 0x0FD5
KEEP_EX = (0, 1, 3, 4, 5, 6)   # 186 has these; 286 segment-limit faults (#SS/#GP) etc. are 286-only
def main(src, metaf, dst, per_file):
    md = json.load(open(metaf))["opcodes"]
    out = open(dst, "wb"); total = skipped = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".json"): continue
        parts = fn[:-5].split(".")
        op = int(parts[0], 16); reg = int(parts[1]) if len(parts) == 2 else None
        meta = dict(md.get(parts[0], {}))
        if reg is not None: meta.update(meta.get("reg", {}).get(str(reg), {}))
        if meta.get("status", "normal") not in ("normal", "alias"): continue
        if op == 0xF4: continue                      # HLT: the test's own trailing HLT protocol
        fmask = int(meta.get("flags-mask", 0xFFFF)) & ARCH
        tests = json.load(open(os.path.join(src, fn)))
        tests = tests["tests"] if isinstance(tests, dict) else tests
        for t in tests[:per_file]:
            ini, fin = t["initial"], t["final"]
            ir = ini["regs"]; fr = dict(ir); fr.update(fin["regs"])
            fr["ip"] = (fr["ip"] - 1) & 0xFFFF          # the trailing HLT has run
            ignore = []
            ex = t.get("exception")
            if ex and ex["number"] not in KEEP_EX:
                skipped += 1; continue
            if ex: ignore = [ex["flag_address"], ex["flag_address"] + 1]
            out.write(struct.pack("<BB", op, 0xFF if reg is None else reg))
            out.write(struct.pack("<14H", *[ir[k] & 0xFFFF for k in REG]))
            out.write(struct.pack("<14H", *[fr[k] & 0xFFFF for k in REG]))
            out.write(struct.pack("<HHHH", fmask, len(ini["ram"]), len(fin["ram"]), len(ignore)))
            for a, v in ini["ram"]: out.write(struct.pack("<IB", a, v))
            for a, v in fin["ram"]: out.write(struct.pack("<IB", a, v))
            for a in ignore: out.write(struct.pack("<I", a))
            total += 1
    print(f"wrote {total} vectors ({skipped} with 286-only faults skipped) -> {dst}")
if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 2000)

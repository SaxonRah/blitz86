#!/usr/bin/env python3
"""Windows-native acquisition of physical 8086 SingleStepTests vectors."""
import concurrent.futures
import json
import subprocess
import sys
import urllib.request
import urllib.error
from pathlib import Path

repo = Path(sys.argv[1]).resolve()
dst = repo / "sst"
dst.mkdir(exist_ok=True)
base = "https://raw.githubusercontent.com/SingleStepTests/8086/main/v1/"
def download(name):
    target=dst/name
    if target.is_file() and target.stat().st_size:
        return "cached"
    try:
        with urllib.request.urlopen(base+name, timeout=90) as r:
            body=r.read()
        if not body: raise ValueError("empty download")
        target.write_bytes(body)
        return "downloaded"
    except urllib.error.HTTPError as ex:
        if ex.code == 404: return "not-published"
        return f"ERROR {name}: {ex}"
    except Exception as ex:
        return f"ERROR {name}: {ex}"

meta=download("metadata.json")
if meta.startswith("ERROR"): raise SystemExit(meta)
opcodes=json.loads((dst/"metadata.json").read_text(encoding="utf8"))["opcodes"]
names=[]
for key, value in opcodes.items():
    if "reg" in value:
        names += [f"{key}.{r}.json.gz" for r in value["reg"]]
    else:
        names.append(f"{key}.json.gz")
print(f"Downloading {len(names)} opcode vector sets...")
errors=[]
missing=0
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
    for i,(name,result) in enumerate(zip(names,pool.map(download,names)),1):
        if result.startswith("ERROR"): errors.append(result)
        elif result == "not-published": missing += 1
        if i % 25 == 0: print(f"{i}/{len(names)} complete",flush=True)
print(f"Not-published vector archives (HTTP 404): {missing}; skipped (upstream also skips these).",flush=True)
if errors:
    for e in errors[:20]: print(e,file=sys.stderr)
    raise SystemExit(f"{len(errors)} vector archives failed; rerun deps to retry")
converter=repo/"tools"/"sst_convert.py"
for file,amount in [("all.bin",2000),("quick.bin",100)]:
    command=[sys.executable,str(converter),str(dst),str(dst/file),str(amount)]
    print(" ".join(command),flush=True)
    subprocess.run(command,check=True)
print("Windows-native silicon vectors ready",flush=True)

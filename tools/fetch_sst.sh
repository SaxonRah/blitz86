#!/bin/sh
# Download the SingleStepTests 8086 v1 vectors (physical P80C86A-2) and
# convert them for tests/sst_*.c.   Usage: tools/fetch_sst.sh [dir]
set -e
D=${1:-sst}
mkdir -p "$D"
BASE=https://raw.githubusercontent.com/SingleStepTests/8086/main/v1
curl -sfo "$D/metadata.json" "$BASE/metadata.json"
python3 - "$D" <<'PY'
import json, sys
d = sys.argv[1]
m = json.load(open(d + "/metadata.json"))["opcodes"]
names = []
for k, v in m.items():
    if "reg" in v: names += [f"{k}.{r}.json.gz" for r in v["reg"]]
    else: names.append(f"{k}.json.gz")
open(d + "/names.txt", "w").write("\n".join(names) + "\n")
PY
while read n; do
  [ -s "$D/$n" ] || curl -sfo "$D/$n" "$BASE/$n" || true &
  [ $(jobs -p | wc -l) -ge 16 ] && wait
done < "$D/names.txt"
wait
python3 "$(dirname "$0")/sst_convert.py" "$D" "$D/all.bin" 2000
python3 "$(dirname "$0")/sst_convert.py" "$D" "$D/quick.bin" 100

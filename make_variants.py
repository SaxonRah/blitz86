#!/usr/bin/env python3
from pathlib import Path
import argparse, hashlib
p=argparse.ArgumentParser(description="Generate full jit.c diagnostic variants without git patches")
p.add_argument('--repo',default=r'C:\blitz86_v2')
p.add_argument('--install',choices=['baseline','no-chain','no-fast','neither'])
args=p.parse_args()
repo=Path(args.repo)
src=repo/'src'/'jit.c'
out=repo/'diagnostics'/'seed51187'
out.mkdir(parents=True,exist_ok=True)
base=out/'jit.baseline.c'
if not base.exists():
    if not src.exists(): raise SystemExit(f'ERROR missing {src}')
    base.write_bytes(src.read_bytes())
original=base.read_text(encoding='utf-8')
chain='        if (patch_site && patch_gen == j->flush_gen) {\n            be_patch_branch(patch_site, b->host);\n            j->st.chains++;\n        }'
fast='        if (!j->single_step) {\n            uint32_t f = fast_hash(key);\n            j->fast[f].key = key;\n            j->fast[f].host = be_code_ptr(b->host);\n        }'
for label, anchor in [('chain',chain),('fast',fast)]:
    count=original.count(anchor)
    if count != 1: raise SystemExit(f'ERROR expected exactly one {label} anchor, found {count}; nothing installed.')
variants={'baseline':(False,False),'no-chain':(True,False),'no-fast':(False,True),'neither':(True,True)}
for name,(nc,nf) in variants.items():
    content=original
    if nc: content=content.replace(chain,'        /* SEED51187_DIAG: branch patch disabled. */\n        (void)patch_site;\n        (void)patch_gen;')
    if nf: content=content.replace(fast,'        /* SEED51187_DIAG: fast table population disabled. */')
    (out/('jit.'+name+'.c')).write_text(content,encoding='utf-8',newline='\n')
print('Baseline SHA256:',hashlib.sha256(base.read_bytes()).hexdigest())
for name in variants: print('Generated:',out/('jit.'+name+'.c'))
if args.install:
    chosen=out/('jit.'+args.install+'.c')
    src.write_bytes(chosen.read_bytes())
    print('Installed complete source:',src,'variant:',args.install)
    print('Rebuild ARM fuzz binary before testing this variant.')

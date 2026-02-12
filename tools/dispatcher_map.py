#!/usr/bin/env python3
import os, re, sys

ROOT = sys.argv[1] if len(sys.argv)>1 else "."
REG = re.compile(r'\bregisterListener\s*\(')
UNR = re.compile(r'\bunregisterListener\s*\(')
ONTICK = re.compile(r'\bonTick\s*\(')

hits={"register":[],"unregister":[],"onTick":[]}
for dp,_,fns in os.walk(ROOT):
    for fn in fns:
        if not fn.endswith(('.hpp','.cpp','.cc','.cxx')): continue
        path=os.path.join(dp,fn)
        try:
            with open(path,'r',encoding='utf-8',errors='ignore') as f:
                for i,l in enumerate(f,1):
                    if REG.search(l): hits["register"].append((path,i,l.strip()))
                    if UNR.search(l): hits["unregister"].append((path,i,l.strip()))
                    if ONTICK.search(l): hits["onTick"].append((path,i,l.strip()))
        except Exception:
            pass

os.makedirs("artifacts", exist_ok=True)
with open("artifacts/T1_dispatcher_map.md","w") as f:
    f.write("# Dispatcher wiring\n\n")
    f.write("## registerListener call sites\n")
    for p,i,l in hits["register"]: f.write(f"- {p}:{i} :: {l}\n")
    f.write("\n## unregisterListener call sites\n")
    for p,i,l in hits["unregister"]: f.write(f"- {p}:{i} :: {l}\n")
    f.write("\n## onTick entry points\n")
    for p,i,l in hits["onTick"]: f.write(f"- {p}:{i} :: {l}\n")

print("Wrote artifacts/T1_dispatcher_map.md")
